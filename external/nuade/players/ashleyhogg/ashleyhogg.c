// SPDX-License-Identifier: MIT
// Ashley Hogg player: the entry points of the two driver families (ah_old.c, ah_new.c) and the helpers they share; the
// module checks and the table locating are in ah_module.c.

#include "ah_internal.h"

// AUDxLC ignores its lowest bit, so a sample at an odd offset is fetched from the byte before it.

void ah_set_sample(const AhMemory* m, NuadePaulaChannel* paula, uint32_t offset, uint16_t length_words) {
    offset &= ~1u;
    paula->lc = offset <= m->capacity ? (const int8_t*)(m->data + offset) : nullptr;
    paula->len_words = length_words;
}

uint8_t ah_paula_volume(unsigned volume) {
    return (uint8_t)(volume & AH_VOLUME_MASK);
}

static void sync_channels(NuadePaulaChannel* channels, NuadeVoices* voices) {
    for (int i = 0; i < AH_VOICES; i++) {
        nuade_paula_channel_sync(&channels[i], (uint32_t)i, voices);
    }
}

bool ah_start(AhPlayer* player, int subsong, NuadeVoices* voices) {
    if (player == nullptr || voices == nullptr || subsong < 0 || subsong >= ah_subsong_count(player)) {
        return false;
    }
    if (player->family == AhFamily_New) {
        if (subsong >= NEW_MAX_SUBSONG) {
            return false;
        }
        ah_new_start(&player->new_family, &player->module, subsong);
    } else {
        ah_old_start(&player->old_family, &player->module, subsong);
    }
    for (int i = 0; i < AH_VOICES; i++) {
        nuade_voice_stop(voices, (uint32_t)i);
    }
    return true;
}

AhTickResult ah_tick(AhPlayer* player, NuadeVoices* voices) {
    if (player->family == AhFamily_New) {
        AhNewSong* song = &player->new_family.song;
        ah_new_tick(&player->new_family, &player->module);
        sync_channels(song->paula, voices);
        return (AhTickResult) { .looped = song->looped, .ended = false };
    }
    AhOldSong* song = &player->old_family.song;
    ah_old_tick(&player->old_family, &player->module);
    sync_channels(song->paula, voices);
    return (AhTickResult) { .looped = song->looped, .ended = song->ended };
}

uint32_t ah_grid_ticks(const AhPlayer* player, int subsong) {
    if (subsong < 0 || subsong >= ah_subsong_count(player)) {
        return 0;
    }
    if (player->family == AhFamily_New) {
        return NEW_DEFAULT_PHASE_LIMIT;
    }
    return ah_u8(&player->module, player->old_family.tables.speeds + (uint32_t)subsong);
}

// The sound count of the new family: the records at the start of the smp file end where its first sample starts. A
// song picks a record by a byte.
static int new_sample_count(const AhMemory* samples) {
    const uint32_t first = ah_u32(samples, 0);
    if (first < NEW_RECORD_SIZE || first > samples->size) {
        return 0;
    }
    const uint32_t count = first / NEW_RECORD_SIZE;
    return count < UINT8_MAX + 1u ? (int)count : UINT8_MAX + 1;
}

int ah_sample_count(const AhPlayer* player) {
    if (player->family == AhFamily_New) {
        return new_sample_count(&player->new_family.samples);
    }
    return (int)ah_old_instrument_count(&player->old_family);
}

// A sound plays its attack once and then repeats `loop_words` from `loop`. It is one run of the data when the repeat
// starts inside the attack, or right behind it, and reaches its end: the data then goes from the attack's start to the
// repeat's end. Any other sound has no single run.
static bool run_pcm(const AhMemory* m, uint32_t attack, uint32_t attack_words, uint32_t loop, uint32_t loop_words,
                    const int8_t** data, uint32_t* length, uint32_t* loop_start, uint32_t* loop_length) {
    const uint64_t attack_end = (uint64_t)attack + 2u * attack_words;
    const uint64_t loop_end = (uint64_t)loop + 2u * loop_words;
    // a repeat of one word is the silent word the drivers write when a sound does not repeat
    const bool repeats = loop_words > 1;
    if (attack_words == 0 || attack_end > m->size || (repeats && loop_end > m->size)) {
        return false;
    }
    *data = (const int8_t*)m->data + attack;
    *length = (uint32_t)(attack_end - attack);
    *loop_start = 0;
    *loop_length = 0;
    if (!repeats) {
        return true;
    }
    if (loop < attack || loop > attack_end || loop_end < attack_end) {
        return false;
    }
    *length = (uint32_t)(loop_end - attack);
    *loop_start = loop - attack;
    *loop_length = (uint32_t)(loop_end - loop);
    return true;
}

bool ah_sample_pcm(const AhPlayer* player, int sound, const int8_t** data, uint32_t* length, uint32_t* loop_start,
                   uint32_t* loop_length) {
    if (sound < 0 || sound >= ah_sample_count(player)) {
        return false;
    }
    if (player->family == AhFamily_New) {
        const AhMemory* samples = &player->new_family.samples;
        const AhNewSample s = ah_new_read_sample(samples, (uint8_t)sound);
        return run_pcm(samples, s.start & ~1u, s.start_length, s.loop_start & ~1u, s.loop_length, data, length,
                       loop_start, loop_length);
    }
    const AhOld* old = &player->old_family;
    const AhOldInstrument in
        = ah_old_read_instrument(&player->module, old->tables.instruments + (uint32_t)sound * OLD_INSTRUMENT_SIZE);
    if (in.wave_table || in.sample == 0) {
        return false;
    }
    const uint32_t attack = (old->tables.samples + in.sample) & ~1u;
    // the repeat is read with the attack's length; offset 0 is the silent repeat
    return run_pcm(&player->module, attack, in.length_words, in.loop != 0 ? (old->tables.samples + in.loop) & ~1u : 0,
                   in.loop != 0 ? in.length_words : 0, data, length, loop_start, loop_length);
}

AhVoiceState ah_voice_state(const AhPlayer* player, int voice) {
    if (voice < 0 || voice >= AH_VOICES) {
        return (AhVoiceState) { .note = -1, .instrument = -1 };
    }
    if (player->family == AhFamily_New) {
        const AhNewVoice* v = &player->new_family.song.voices[voice];
        return (AhVoiceState) { .note = v->display_note, .instrument = v->display_instrument };
    }
    const AhOldVoice* v = &player->old_family.song.voices[voice];
    return (AhVoiceState) { .note = v->display_note, .instrument = v->display_instrument };
}
