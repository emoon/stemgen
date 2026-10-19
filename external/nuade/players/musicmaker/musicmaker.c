// SPDX-License-Identifier: MIT
// MusicMaker V8 player: opening a song and driving the replayer of its variant.

#include "mm4.h"
#include "mm8.h"
#include "mm_internal.h"
#include <stdlib.h>

bool mm_probe(const uint8_t* data, size_t size, const char* filename) {
    const NuadeSongSource source = { .data = data, .size = size, .path = filename, .io = nullptr };
    MmSong song;
    MmInstruments instruments;
    MmVariant variant;
    if (!mm_file_open(&source, false, &song, &instruments, &variant)) {
        return false;
    }
    mm_song_close(&song);
    mm_instruments_close(&instruments);
    return true;
}

MmPlayer* mm_create(const NuadeSongSource* source) {
    MmPlayer* p = calloc(1, sizeof(MmPlayer));
    if (p == nullptr) {
        return nullptr;
    }
    if (!mm_file_open(source, true, &p->song, &p->instruments, &p->variant)) {
        free(p);
        return nullptr;
    }
    return p;
}

void mm_destroy(MmPlayer* player) {
    if (player == nullptr) {
        return;
    }
    mm8_stop(&player->eight);
    mm_song_close(&player->song);
    mm_instruments_close(&player->instruments);
    free(player);
}

uint32_t mm_voice_count(const MmPlayer* player) {
    (void)player;
    return MM4_VOICES;
}

// The tracks run their own note lengths, so the grid is a display choice only (no two starts of one track share a row
// in the test songs).
uint32_t mm_grid_ticks(const MmPlayer* player) {
    return player->variant == MmVariant_4V ? 5 : 4;
}

uint32_t mm_timebase_hz(const MmPlayer* player) {
    return player->variant == MmVariant_4V ? MM_TIMEBASE_HZ_4V : MM_TIMEBASE_HZ_8V;
}

bool mm_start(MmPlayer* player, NuadeVoices* voices) {
    if (player->variant == MmVariant_8V) {
        return mm8_start(&player->eight, &player->song, &player->instruments, voices);
    }
    mm4_start(&player->four, &player->song, &player->instruments, voices);
    return true;
}

MmTickResult mm_tick(MmPlayer* player, NuadeVoices* voices) {
    return player->variant == MmVariant_8V ? mm8_tick(&player->eight, voices) : mm4_tick(&player->four, voices);
}

uint32_t mm_track_count(const MmPlayer* player) {
    return player->variant == MmVariant_8V ? MM8_VOICES : MM4_VOICES;
}

MmVoiceState mm_voice_state(const MmPlayer* player, uint32_t track) {
    if (track >= mm_track_count(player)) {
        return (MmVoiceState) { .note = -1, .instrument = -1 };
    }
    const MmVoice* t
        = player->variant == MmVariant_8V ? &player->eight.core.voice[track] : &player->four.core.voice[track];
    return (MmVoiceState) { .note = t->note, .instrument = t->instrument, .note_starts = t->note_starts };
}

bool mm_sample_pcm(const MmPlayer* player, uint32_t slot, const int8_t** data, uint32_t* length, uint32_t* loop_start,
                   uint32_t* loop_length) {
    const MmInstruments* in = &player->instruments;
    if (slot >= MM_INSTRUMENTS || in->record[slot].length == 0 || in->sample[slot] >= in->block.size) {
        return false;
    }
    const MmSampleRecord* record = &in->record[slot];
    const bool four = player->variant != MmVariant_8V;
    // the 4 voice replayer loops a note that has a first part, the 8 voice one a sample with a repeat
    const bool loops
        = record->loop_length > 1 && (!four || record->first_length != 0) && record->loop_offset < record->length;
    const uint32_t repeat = (uint32_t)record->loop_length * 2;
    const uint32_t tail = (uint32_t)record->length - record->loop_offset;
    *data = (const int8_t*)in->storage + in->sample[slot];
    *length = record->length;
    *loop_start = loops ? record->loop_offset : 0;
    *loop_length = loops ? (repeat < tail ? repeat : tail) : 0;
    return true;
}

const char* mm_title(const MmPlayer* player) {
    return player->song.title;
}
