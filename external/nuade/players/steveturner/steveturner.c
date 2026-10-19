// SPDX-License-Identifier: MIT
// Steve Turner player, ported from the Steve Turner player in Wanted Team's "Steve Turner_v4.asm" (engine (c) 1989-90
// Steve Turner, adapted by Mr.Larmer/Wanted Team; from the game Off Road Racer).
//
// A song is four track lists (one per voice) of pattern numbers. A pattern holds notes, durations, instrument
// numbers and a portamento setting. An instrument is a record of 0x30 bytes: the sample, a volume envelope of four
// phases, and up to three pitch records (a slide, a vibrato and flags that loop them).
//
// One tick runs every voice (sequencer, envelope, pitch, portamento) and then looks at the request to start a song.

#include <nuade/players/steveturner.h>

#include "nuade_base.h"
#include "nuade_paula_channel.h"
#include "nuade_utils.h"
#include <stdlib.h>
#include <string.h>

#define ST_CHECK_BYTES 0x2E // the module starts with this much start code
#define ST_SONG_BYTES 12
#define ST_INSTRUMENT_BYTES 0x30
#define ST_MASTER_LEVEL 0xFF // the envelope level of full volume; the eagleplayer fades up to it in its first tick
#define ST_ONE_SHOT_BYTES 2  // a one-shot sample repeats this many bytes of silence
#define ST_PITCH_RECORD_BYTES 10
#define ST_INSTRUMENT_INDEX_BIAS 129 // an instrument index runs from -129 (a chain's next number) to 126
#define ST_INSTRUMENT_INDEXES 256
#define ST_SEQUENCE_STEP_LIMIT 0x10000 // control bytes one sequencer call may walk before it gives up

// The pattern, track list and sound parameters of the original.
enum {
    StTrack_Stop = 0xFE,
    StTrack_End = 0xFF,
    StPattern_Duration = 0x80,   // 0x80..0xAF: the note duration is the byte - 0x7F
    StPattern_Instrument = 0xB0, // 0xB0..0xCF: start the instrument without a note
    StPattern_Select = 0xD0,     // 0xD0..0xEF: the instrument the notes play
    StPattern_Portamento = 0xF0, // 0xF0..0xF8: portamento shift
    StPattern_Rest = 0xFE,       // a rest
};

enum {
    StFlag_Reload = 1,  // the period returns to the record's reload value
    StFlag_Negate = 2,  // the delta changes sign
    StFlag_Shorten = 4, // the record's step count shrinks by one, down to 8
};

static const uint16_t st_periods[] = {
    0xEEE4, 0xE17B, 0xD4D4, 0xC8E1, 0xBD9C, 0xB2F6, 0xA8EC, 0x9F71, 0x967D, 0x8E0B, 0x8612, 0x7E8C, 0x7772, 0x70BE,
    0x6A6A, 0x6471, 0x5ECE, 0x597B, 0x5476, 0x4FB9, 0x4B3F, 0x4706, 0x4309, 0x3F46, 0x3BB9, 0x385F, 0x3535, 0x3239,
    0x2F67, 0x2CBE, 0x2A3B, 0x27DD, 0x25A0, 0x2383, 0x2185, 0x1FA3, 0x1DDD, 0x1C30, 0x1A9B, 0x191D, 0x17B4, 0x165F,
    0x151E, 0x13EF, 0x12D0, 0x11C2, 0x10C3, 0x0FD2, 0x0EEF, 0x0E18, 0x0D4E, 0x0C8F, 0x0BDA, 0x0B30, 0x0A8F, 0x09F8,
    0x0968, 0x08E1, 0x0862, 0x07E9, 0x0778, 0x070C, 0x06A7, 0x0648, 0x05ED, 0x0598, 0x0548, 0x04FC, 0x04B4, 0x0471,
    0x0431, 0x03F5, 0x03BC, 0x0386, 0x0354, 0x0324, 0x02F7, 0x02CC, 0x02A4, 0x027E, 0x025A, 0x0239, 0x0219, 0x01FB,
};

// Module access: an address outside the image reads 0, a read of a table entry sign-extends where the original does.

static uint8_t st_u8(const StModule* m, uint32_t address) {
    return address < m->size ? m->data[address] : 0;
}

static uint16_t st_u16(const StModule* m, uint32_t address) {
    return (uint16_t)((st_u8(m, address) << 8) | st_u8(m, address + 1));
}

static int16_t st_s16(const StModule* m, uint32_t address) {
    return (int16_t)st_u16(m, address);
}

static uint32_t st_u32(const StModule* m, uint32_t address) {
    return ((uint32_t)st_u16(m, address) << 16) | st_u16(m, address + 2);
}

static uint32_t st_add_s16(uint32_t base, int16_t offset) {
    return base + (uint32_t)(int32_t)offset;
}

// The module

static bool st_check_code(const uint8_t* data, size_t size) {
    if (size < ST_CHECK_BYTES) {
        return false;
    }
    static const uint8_t move_long_to_a5[] = { 0x2B, 0x7C };
    for (size_t at = 0; at < 0x20; at += 8) {
        if (memcmp(data + at, move_long_to_a5, sizeof(move_long_to_a5)) != 0) {
            return false;
        }
    }
    return nuade_read_be32(data + 0x20) == 0x303C00FF && nuade_read_be32(data + 0x24) == 0x32004EB9
           && nuade_read_be16(data + 0x2C) == 0x4E75;
}

// Where the last pattern ends, found the way the eagleplayer does: the highest pattern offset below the first
// 0xF0FF word of the pattern table, then the next 0xFF byte. 0 if there is none.
static uint32_t st_calc_size(const StModule* m) {
    uint16_t highest = 10;
    for (uint32_t at = m->patterns; at < m->size; at += 2) {
        const uint16_t word = st_u16(m, at);
        if (word == 0xF0FF) {
            break;
        }
        if (word > highest) {
            highest = word;
        }
    }
    for (uint32_t at = st_add_s16(m->patterns, (int16_t)highest); at < m->size;) {
        if (m->data[at++] == 0xFF) {
            return at;
        }
    }
    return 0;
}

static uint32_t st_count_subsongs(const StModule* m) {
    // The eagleplayer counts the songs up to the first with a priority or a speed over 15; the 17213 byte Paradroid 90
    // module has a sixth entry that is not a song.
    if (m->calc_size == 17213) {
        return 5;
    }
    uint32_t count = 0;
    while (m->songs + (count + 1) * ST_SONG_BYTES <= m->size
           && (st_u16(m, m->songs + count * ST_SONG_BYTES) & 0xFFF0) == 0) {
        count++;
    }
    return count;
}

// The offset of a table that the start code stores as an address; false if it does not lie in the module.
static bool st_table_offset(const uint8_t* data, size_t size, size_t pointer_at, uint32_t minimum_bytes,
                            uint32_t* out) {
    const uint32_t base = nuade_read_be32(data + 2);
    const uint32_t address = nuade_read_be32(data + pointer_at);
    if (address < base || address - base > size) {
        return false;
    }
    const uint32_t offset = address - base + ST_CHECK_BYTES;
    if (offset > size || size - offset < minimum_bytes) {
        return false;
    }
    *out = offset;
    return true;
}

// The instrument and song records

static StPitchSpec st_read_pitch_spec(const StModule* m, uint32_t at) {
    return (StPitchSpec) { .period = st_u16(m, at),
                           .delta = st_s16(m, at + 2),
                           .repeats = st_u8(m, at + 4),
                           .steps = st_u8(m, at + 5),
                           .hold_ticks = st_u8(m, at + 6),
                           .delta_step = (int8_t)st_u8(m, at + 7),
                           .flags = st_u8(m, at + 8) };
}

// The instrument at a table offset (a signed word, as the original adds it to the table address).
static StInstrument st_read_instrument(const StModule* m, uint16_t offset) {
    const uint32_t at = st_add_s16(m->instruments, (int16_t)offset);
    StInstrument instrument = {
        .priority = st_u8(m, at + 0x1E),
        .sample = st_u8(m, at + 0x1F),
        .repeats = st_u8(m, at + 0x20),
        .attack_ticks = st_u8(m, at + 0x21),
        .attack_rate = (int8_t)st_u8(m, at + 0x22),
        .sustain_ticks = st_u8(m, at + 0x23),
        .sustain_rate = (int8_t)st_u8(m, at + 0x24),
        .period_shift = st_u8(m, at + 0x25),
        .hold_phase_ticks = st_u16(m, at + 0x26),
        .hold_rate = (int8_t)st_u8(m, at + 0x28),
        .hold_turn_ticks = st_u8(m, at + 0x29),
        .release_rate = (int8_t)st_u8(m, at + 0x2A),
        .record_count = st_u8(m, at + 0x2B),
        .vibrato_delay = st_u8(m, at + 0x2C),
        .vibrato_speed = st_u8(m, at + 0x2D),
        .vibrato_depth = (int8_t)st_u8(m, at + 0x2E),
        .next = st_u8(m, at + 0x2F),
    };
    for (int i = 0; i < ST_PITCH_RECORDS; i++) {
        instrument.pitch[i] = st_read_pitch_spec(m, at + (uint32_t)i * ST_PITCH_RECORD_BYTES);
    }
    return instrument;
}

typedef struct StSongEntry {
    uint8_t priority;
    uint8_t speed;
    uint16_t track[ST_VOICES]; // offsets of the voices' track lists from the song table; 0 for none
} StSongEntry;

static StSongEntry st_read_song(const StModule* m, uint8_t number) {
    const uint32_t at = m->songs + (uint32_t)(uint16_t)((number & 0x7F) - 1) * ST_SONG_BYTES;
    StSongEntry song = { .priority = st_u8(m, at), .speed = st_u8(m, at + 1) };
    for (int i = 0; i < ST_VOICES; i++) {
        song.track[i] = st_u16(m, at + 4 + (uint32_t)i * 2);
    }
    return song;
}

// Where the sample a sample number selects starts: the table holds offsets from its own start; a sample is its length
// in bytes and then its data. False if the table entry or the length word lies outside the module.
static bool st_sample_at(const StModule* m, uint8_t number, uint32_t* at) {
    const uint32_t entry = m->samples + (uint32_t)number * 4;
    if (entry > m->size || m->size - entry < 4) {
        return false;
    }
    const uint64_t start = (uint64_t)m->samples + st_u32(m, entry);
    if (start > m->size || m->size - start < 2) {
        return false;
    }
    *at = (uint32_t)start;
    return true;
}

static void st_load_sample(const StModule* m, StVoice* v, uint8_t number) {
    uint32_t at;
    if (!st_sample_at(m, number, &at)) {
        v->paula.len_words = 0;
        v->paula.lc = (const int8_t*)m->data + m->size;
        return;
    }
    v->paula.len_words = (uint16_t)(st_u16(m, at) >> 1);
    v->paula.lc = (const int8_t*)m->data + at + 2;
}

// The module, continued

// What the replayer can reach from the song table: the track lists, the patterns they name, the instruments the
// patterns start and the chains and samples of those. The probe walks all of it, so that playing never reads past the
// module for a song that opened.
typedef struct StWalk {
    const StModule* module;
    uint8_t* track_seen;   // one bit per module byte: a track list byte that was read
    uint8_t* pattern_seen; // the same for pattern bytes
    bool pattern_done[256];
    bool instrument_done[ST_INSTRUMENT_INDEXES];
} StWalk;

static bool st_bit_test_set(uint8_t* bits, uint32_t index) {
    const bool set = (bits[index >> 3] >> (index & 7) & 1) != 0;
    bits[index >> 3] |= (uint8_t)(1u << (index & 7));
    return set;
}

// The instrument `index` of the table (a signed number: a chain's next instrument counts back from the table),
// its chain, and its sample.
static bool st_walk_instrument(StWalk* w, int index) {
    const StModule* m = w->module;
    while (!w->instrument_done[index + ST_INSTRUMENT_INDEX_BIAS]) {
        w->instrument_done[index + ST_INSTRUMENT_INDEX_BIAS] = true;
        const uint16_t offset = (uint16_t)(index * ST_INSTRUMENT_BYTES);
        const int64_t at = (int64_t)m->instruments + (int16_t)offset;
        if (at < 0 || at > (int64_t)m->size - ST_INSTRUMENT_BYTES) {
            return false;
        }
        const StInstrument instrument = st_read_instrument(m, offset);
        uint32_t sample_at;
        if (instrument.record_count == 0 || instrument.record_count > ST_PITCH_RECORDS
            || (instrument.sample < 0x80 && !st_sample_at(m, instrument.sample, &sample_at))) {
            return false;
        }
        if (instrument.next == 0) {
            break;
        }
        index = (int8_t)instrument.next - 1;
    }
    return true;
}

// The pattern table entry `number`: the pattern up to its end byte.
static bool st_walk_pattern(StWalk* w, uint8_t number) {
    const StModule* m = w->module;
    if (w->pattern_done[number]) {
        return true;
    }
    w->pattern_done[number] = true;
    const uint64_t entry = (uint64_t)m->patterns + (uint32_t)number * 2;
    if (entry + 2 > m->size) {
        return false;
    }
    const int64_t start = (int64_t)m->patterns + st_s16(m, (uint32_t)entry);
    if (start < 0 || start >= m->size) {
        return false;
    }
    for (uint64_t at = (uint64_t)start; at < m->size; at++) {
        if (st_bit_test_set(w->pattern_seen, (uint32_t)at)) {
            return true;
        }
        const uint8_t byte = m->data[at];
        if (byte >= StPattern_Instrument && byte < StPattern_Portamento) {
            const int index = byte >= StPattern_Select ? byte - StPattern_Select : byte - StPattern_Instrument;
            if (!st_walk_instrument(w, index)) {
                return false;
            }
        } else if (byte > StPattern_Portamento + 8 && byte != StPattern_Rest) {
            return true;
        }
    }
    return true; // the pattern runs to the end of the module (the 17213 byte Paradroid 90 does): the rest reads 0
}

// A track list up to its stop or end entry.
static bool st_walk_track(StWalk* w, uint32_t start) {
    const StModule* m = w->module;
    for (uint32_t at = start; at < m->size; at++) {
        if (st_bit_test_set(w->track_seen, at)) {
            return true;
        }
        const uint8_t entry = m->data[at];
        if (entry >= StTrack_Stop) {
            return true;
        }
        if (!st_walk_pattern(w, entry)) {
            return false;
        }
    }
    return start < m->size; // a list that runs to the end of the module reads 0 (pattern 0) from there
}

static bool st_reachable_valid(const StModule* m) {
    StWalk* w = calloc(1, sizeof(StWalk));
    const size_t bitmap_bytes = (size_t)m->size / 8 + 1;
    uint8_t* bitmaps = calloc(2, bitmap_bytes);
    bool valid = w != nullptr && bitmaps != nullptr;
    if (valid) {
        w->module = m;
        w->track_seen = bitmaps;
        w->pattern_seen = bitmaps + bitmap_bytes;
        // Voices that no track list drives start instrument 1 in the first tick, and notes before the first
        // instrument select play it.
        valid = st_walk_instrument(w, 0);
        for (uint32_t number = 1; valid && number <= m->subsongs && number <= UINT8_MAX; number++) {
            const StSongEntry song = st_read_song(m, (uint8_t)number);
            for (int i = 0; valid && i < ST_VOICES; i++) {
                valid = song.track[i] == 0 || st_walk_track(w, st_add_s16(m->songs, (int16_t)song.track[i]));
            }
        }
    }
    free(bitmaps);
    free(w);
    return valid;
}

// Fills `m` from the image `data`, which the module then points at as it is; the caller replaces that with its own
// copy before anything writes to it.
static bool st_module_parse(StModule* m, const uint8_t* data, size_t size) {
    if (size > 0x1000000 || !st_check_code(data, size)) {
        return false;
    }
    *m = (StModule) { .data = (uint8_t*)data, .size = (uint32_t)size, .instruments = ST_CHECK_BYTES };
    if (!st_table_offset(data, size, 10, ST_SONG_BYTES, &m->songs) || !st_table_offset(data, size, 18, 2, &m->patterns)
        || !st_table_offset(data, size, 26, 4, &m->samples)) {
        return false;
    }
    m->calc_size = st_calc_size(m);
    m->subsongs = st_count_subsongs(m);
    return m->subsongs != 0 && st_reachable_valid(m);
}

bool st_probe(const uint8_t* data, size_t size) {
    StModule module;
    return st_module_parse(&module, data, size);
}

// The eagleplayer corrects one period in the 6802 byte Rainbow Islands module.
static void st_module_patch(StModule* m) {
    if (m->calc_size == 6802 && st_u32(m, 3650) == 0x019F019F) {
        for (uint32_t at = 3650; at <= 3654; at += 4) {
            m->data[at] = 0x01;
            m->data[at + 1] = 0xA0;
            m->data[at + 2] = 0x01;
            m->data[at + 3] = 0xA0;
        }
    }
}

// The voice

// The sound stops sounding, its volume and DMA off. The sequencer goes on.
static void st_voice_stop_sound(StVoice* v) {
    v->paula.volume = 0;
    nuade_paula_channel_dma_off(&v->paula);
    v->envelope = StEnvelope_Stopped;
    v->pitch_state = StPitch_Stopped;
    v->portamento = false;
    v->playing_priority = 0;
}

// The voice starts a track list: nothing plays, nothing is playing, and the start slots are cleared.
static void st_voice_reset(StVoice* v) {
    v->request_armed = false;
    v->queued_armed = false;
    v->queued_delay = 0;
    v->envelope = StEnvelope_Stopped;
    v->pitch_state = StPitch_Stopped;
    v->portamento = false;
    v->sequence = StSequence_Stopped;
    v->playing_priority = 0;
    v->portamento_shift = 0;
    nuade_paula_channel_dma_off(&v->paula);
}

// The vibrato and the period register.
static void st_pitch_output(StVoice* v) {
    StPitchRecord* record = &v->records[v->record];
    const StInstrument* sound = &v->sound;
    if (sound->vibrato_speed != 0) {
        if (v->vibrato_delay != 0) {
            v->vibrato_delay--;
        } else {
            record->period = (uint16_t)(record->period + v->vibrato_step);
            if (--v->vibrato_timer == 0) {
                v->vibrato_timer = sound->vibrato_speed;
                int8_t step = (int8_t)-v->vibrato_step;
                if (step >= 0 && step < sound->vibrato_depth) {
                    step++;
                }
                v->vibrato_step = step;
            }
        }
    }
    const unsigned shift = sound->period_shift & 63u;
    v->paula.period = shift < 16 ? (uint16_t)(record->period >> shift) : 0;
}

static void st_pitch_wait(StVoice* v) {
    st_pitch_output(v);
    if (--v->hold_ticks == 0) {
        v->pitch_state = StPitch_Step;
    }
}

static void st_pitch_hold(StVoice* v) {
    v->hold_ticks = v->sound.pitch[v->record].hold_ticks;
    v->pitch_state = StPitch_Wait;
    st_pitch_wait(v);
}

static void st_pitch_steps(StVoice* v) {
    v->inner_count = v->records[v->record].steps;
    st_pitch_hold(v);
}

static void st_pitch_record_start(StVoice* v) {
    v->outer_count = v->sound.pitch[v->record].repeats;
    st_pitch_steps(v);
}

static void st_pitch_step(StVoice* v) {
    StPitchRecord* record = &v->records[v->record];
    const StPitchSpec* spec = &v->sound.pitch[v->record];
    record->period = (uint16_t)(record->period + record->delta);
    record->delta = (int16_t)(record->delta + spec->delta_step);
    if (--v->inner_count != 0) {
        st_pitch_hold(v);
        return;
    }
    if (spec->flags & StFlag_Reload) {
        record->period = record->reload;
    }
    if (spec->flags & StFlag_Negate) {
        record->delta = (int16_t)-record->delta;
    }
    if ((spec->flags & StFlag_Shorten) && record->steps > 8) {
        record->steps--;
    }
    if (--v->outer_count != 0) {
        st_pitch_steps(v);
        return;
    }
    const bool last = v->record + 1u == v->sound.record_count;
    v->record = last ? 0 : (uint8_t)(v->record + 1);
    st_pitch_record_start(v);
}

static void st_pitch_run(StVoice* v) {
    switch (v->pitch_state) {
        case StPitch_Stopped:
            break;
        case StPitch_Start:
            st_pitch_record_start(v);
            break;
        case StPitch_Wait:
            st_pitch_wait(v);
            break;
        case StPitch_Step:
            st_pitch_step(v);
            break;
    }
}

// Copy the instrument's pitch records and start its pitch program.
static void st_pitch_start(StPlayer* p, StVoice* v) {
    const StInstrument* sound = &v->sound;
    v->vibrato_delay = sound->vibrato_delay;
    v->vibrato_step = 0;
    v->record = 0;
    if (sound->record_count == 0 || sound->record_count > ST_PITCH_RECORDS) {
        p->failed = true; // the original runs its pitch program through the rest of its memory
        return;
    }
    for (int i = 0; i < sound->record_count; i++) {
        const StPitchSpec* spec = &sound->pitch[i];
        v->records[i] = (StPitchRecord) {
            .period = spec->period, .reload = spec->period, .delta = spec->delta, .steps = spec->steps
        };
    }
    v->pitch_state = StPitch_Start;
}

// Portamento: the period of record 0 moves towards the target by a fraction of the distance.
static void st_portamento_run(StVoice* v) {
    if (!v->portamento) {
        return;
    }
    StPitchRecord* record = &v->records[0];
    const int16_t distance = (int16_t)(v->target_period - record->period);
    const unsigned shift = (unsigned)v->portamento_shift & 63u;
    const int16_t step = (int16_t)(shift < 16 ? distance >> shift : (distance < 0 ? -1 : 0));
    if (step != 0) {
        record->period = (uint16_t)(record->period + step);
    } else {
        record->period = v->target_period;
        v->portamento = false;
    }
}

// A note's period replaces the one of the first pitch record, or the portamento slides to it.
static void st_set_pitch(StVoice* v, uint16_t period) {
    StPitchRecord* record = &v->records[0];
    if (v->portamento_shift == 0) {
        v->target_period = period;
        record->period = period;
        record->reload = period;
        v->portamento = false;
    } else {
        record->period = v->target_period;
        v->target_period = period;
        record->reload = period;
        v->portamento = true;
    }
}

static uint16_t st_note_period(uint8_t note) {
    return note < nuade_sizeof_array(st_periods) ? st_periods[note] : 0;
}

// Start an instrument: the sample registers are written, the sound restarts through the envelope.
static void st_start_instrument(StPlayer* p, StVoice* v, uint16_t offset, uint8_t priority) {
    v->sound = st_read_instrument(&p->module, offset);
    v->playing_priority = priority;
    v->envelope = StEnvelope_DmaOff;
    v->pitch_state = StPitch_Stopped;
    v->portamento = false;
    if (v->sound.sample < 0x80) {
        st_load_sample(&p->module, v, v->sound.sample);
    }
    st_pitch_start(p, v);
}

// The envelope. Its phases step the level by a rate for a number of ticks; the sound then repeats or chains on.

static void st_envelope_level(StVoice* v) {
    int level = (int)v->level + v->rate;
    if (level < 0) {
        level = 0;
    } else if (level > ST_MASTER_LEVEL) {
        level = ST_MASTER_LEVEL;
    }
    v->level = (uint16_t)level;
    v->paula.volume = (uint8_t)(level >> 2);
}

static void st_envelope_attack_start(StVoice* v) {
    v->phase_ticks = v->sound.attack_ticks;
    v->rate = v->sound.attack_rate;
    v->envelope = StEnvelope_Loop;
}

static void st_envelope_dma_on(StVoice* v) {
    v->paula.volume = 0;
    nuade_paula_channel_dma_on(&v->paula);
    v->repeats = v->sound.repeats;
    v->one_shot = v->repeats == 0;
    if (v->one_shot) {
        v->level = ST_MASTER_LEVEL;
        v->paula.volume = ST_MASTER_LEVEL >> 2;
        v->repeats++;
    } else {
        v->level = 0;
    }
    st_envelope_attack_start(v);
}

// The sound is over: it repeats, the next instrument of the chain starts, or the voice falls silent.
static void st_envelope_end(StPlayer* p, StVoice* v) {
    if (--v->repeats != 0) {
        v->level = 0;
        st_envelope_attack_start(v);
    } else if (v->sound.next != 0) {
        // The number is a signed byte, extended to a word before the table offset is formed.
        const uint16_t index = (uint16_t)((int16_t)(int8_t)v->sound.next - 1);
        const uint16_t offset = (uint16_t)(index * ST_INSTRUMENT_BYTES);
        st_start_instrument(p, v, offset, st_read_instrument(&p->module, offset).priority);
    } else {
        st_voice_stop_sound(v);
    }
}

static void st_envelope_run(StPlayer* p, StVoice* v) {
    const StInstrument* sound = &v->sound;
    switch (v->envelope) {
        case StEnvelope_Stopped:
            break;
        case StEnvelope_DmaOff:
            nuade_paula_channel_dma_off(&v->paula);
            v->envelope = StEnvelope_DmaOn;
            break;
        case StEnvelope_DmaOn:
            st_envelope_dma_on(v);
            break;
        case StEnvelope_Loop:
            if (v->one_shot) {
                v->paula.len_words = ST_ONE_SHOT_BYTES / 2;
                v->paula.lc = p->silence;
            }
            v->envelope = StEnvelope_Attack;
            st_envelope_run(p, v);
            break;
        case StEnvelope_Attack:
            st_envelope_level(v);
            if (--v->phase_ticks == 0) {
                v->phase_ticks = sound->sustain_ticks;
                v->rate = sound->sustain_rate;
                v->envelope = StEnvelope_Sustain;
            }
            break;
        case StEnvelope_Sustain:
            st_envelope_level(v);
            if (--v->phase_ticks == 0) {
                v->phase_ticks = sound->hold_phase_ticks;
                v->turn_ticks = sound->hold_turn_ticks;
                v->rate = (int8_t)-sound->hold_rate;
                v->envelope = StEnvelope_Hold;
            }
            break;
        case StEnvelope_Hold:
            st_envelope_level(v);
            if (--v->phase_ticks == 0) {
                v->rate = sound->release_rate;
                v->envelope = StEnvelope_Release;
            } else if (--v->turn_ticks == 0) {
                v->turn_ticks = sound->hold_turn_ticks;
                v->rate = (int8_t)-v->rate;
            }
            break;
        case StEnvelope_Release:
            st_envelope_level(v);
            if (v->level == 0) {
                st_envelope_end(p, v);
            }
            break;
    }
}

// The start slots

// Every tick counts the queued start down; when it reaches 0 the slot starts instrument 1 if no sound of a higher
// priority plays, and is spent whether it did or not.
static void st_queued_start_run(StPlayer* p, StVoice* v) {
    if (--v->queued_delay != 0 || !v->queued_armed) {
        return;
    }
    if (v->playing_priority == 0) {
        st_start_instrument(p, v, 0, 0);
        v->instrument_shown = 0;
    }
    v->queued_armed = false;
}

// The request slot starts instrument 1 once, with the priority of the instrument.
static void st_request_run(StPlayer* p, StVoice* v) {
    if (!v->request_armed) {
        return;
    }
    const uint8_t priority = st_read_instrument(&p->module, 0).priority;
    if (priority >= v->playing_priority) {
        st_start_instrument(p, v, 0, priority);
        v->instrument_shown = 0;
    }
    v->request_armed = false;
}

// The sequencer

// The song is over: every voice stops sequencing and the song starts again from the top.
static void st_song_end(StPlayer* p) {
    for (int i = 0; i < ST_VOICES; i++) {
        p->voices[i].sequence = StSequence_Stopped;
    }
    p->song_priority = 0;
    p->restart = p->song;
    p->song = 0;
    p->looped = true;
}

static void st_note_start(StVoice* v) {
    v->sequence = StSequence_Wait;
    v->note_timer = v->duration;
}

// Run the pattern from `pattern_pos` to its next note, rest or instrument start. False if it ran into the next
// track list entry first.
static bool st_pattern_run(StPlayer* p, StVoice* v) {
    for (;;) {
        const uint8_t byte = st_u8(&p->module, v->pattern_pos++);
        if (byte < StPattern_Duration) {
            if (v->instrument_priority >= v->playing_priority) {
                st_start_instrument(p, v, v->instrument_offset, v->instrument_priority);
                st_set_pitch(v, st_note_period(byte));
            }
            v->note = byte;
            v->instrument_shown = v->instrument_offset / ST_INSTRUMENT_BYTES;
            return true;
        }
        if (byte < StPattern_Instrument) {
            v->duration = (uint8_t)(byte - (StPattern_Duration - 1));
        } else if (byte < StPattern_Select) {
            const uint16_t offset = (uint16_t)((byte - StPattern_Instrument) * ST_INSTRUMENT_BYTES);
            const uint8_t priority = st_read_instrument(&p->module, offset).priority;
            if (priority >= v->playing_priority) {
                st_start_instrument(p, v, offset, priority);
                v->instrument_shown = offset / ST_INSTRUMENT_BYTES;
            }
            return true;
        } else if (byte < StPattern_Portamento) {
            v->instrument_offset = (uint16_t)((byte - StPattern_Select) * ST_INSTRUMENT_BYTES);
            v->instrument_priority = st_read_instrument(&p->module, v->instrument_offset).priority;
        } else if (byte < StPattern_Portamento + 9) {
            v->portamento_shift = (int8_t)(byte - StPattern_Portamento);
        } else if (byte == StPattern_Rest) {
            return true;
        } else {
            return false;
        }
    }
}

// The next track list entry: a pattern number runs the pattern, 0xFE stops the voice, 0xFF ends the song.
static void st_sequence_read_track(StPlayer* p, StVoice* v) {
    for (int steps = 0; steps < ST_SEQUENCE_STEP_LIMIT; steps++) {
        const uint8_t entry = st_u8(&p->module, v->track_pos++);
        if (entry == StTrack_End) {
            st_song_end(p);
            return;
        }
        if (entry == StTrack_Stop) {
            v->sequence = StSequence_Stopped;
            return;
        }
        const StModule* m = &p->module;
        v->pattern_pos = st_add_s16(m->patterns, st_s16(m, m->patterns + (uint32_t)entry * 2));
        if (st_pattern_run(p, v)) {
            st_note_start(v);
            return;
        }
    }
    p->failed = true;
}

static void st_sequence_continue(StPlayer* p, StVoice* v) {
    if (st_pattern_run(p, v)) {
        st_note_start(v);
    } else {
        st_sequence_read_track(p, v);
    }
}

static void st_sequence_run(StPlayer* p, StVoice* v) {
    switch (v->sequence) {
        case StSequence_Stopped:
            break;
        case StSequence_ReadTrack:
            st_sequence_read_track(p, v);
            break;
        case StSequence_Wait:
            // The speed counter runs through the speed byte; a note lasts duration steps of it.
            if (p->speed_count == 0) {
                if (--v->note_timer == 0) {
                    st_sequence_continue(p, v);
                }
            } else if (v->note_timer == 0) {
                st_sequence_continue(p, v);
            }
            break;
    }
}

// The song

// Start song `number` (1-based) unless a song of higher priority plays.
static void st_song_start(StPlayer* p, uint8_t number) {
    const StSongEntry song = st_read_song(&p->module, number);
    if (song.priority < p->song_priority) {
        return;
    }
    p->song = number;
    p->song_priority = song.priority;
    p->speed = song.speed;
    p->speed_count = song.speed;
    for (int i = 0; i < ST_VOICES; i++) {
        if (song.track[i] != 0) {
            StVoice* v = &p->voices[i];
            v->track_pos = st_add_s16(p->module.songs, (int16_t)song.track[i]);
            st_voice_reset(v);
            v->sequence = StSequence_ReadTrack;
        }
    }
}

static void st_sync(StPlayer* p, NuadeVoices* voices) {
    for (int i = 0; i < ST_VOICES; i++) {
        nuade_paula_channel_sync(&p->voices[i].paula, (uint32_t)i, voices);
    }
}

StCreateResult st_create(const uint8_t* data, size_t size) {
    StCreateResult result = { .status = StStatus_InvalidData };
    StModule probe;
    if (!st_module_parse(&probe, data, size)) {
        return result;
    }
    StPlayer* player = calloc(1, sizeof(StPlayer));
    uint8_t* image = nuade_paula_padded_copy(data, size);
    if (player == nullptr || image == nullptr) {
        free(player);
        free(image);
        result.status = StStatus_OutOfMemory;
        return result;
    }
    player->module = probe;
    player->module.data = image;
    st_module_patch(&player->module);
    result.player = player;
    result.status = StStatus_Ok;
    return result;
}

void st_destroy(StPlayer* player) {
    if (player == nullptr) {
        return;
    }
    free(player->module.data);
    free(player);
}

int st_subsong_count(const StPlayer* player) {
    return (int)player->module.subsongs;
}

bool st_start(StPlayer* player, int subsong, NuadeVoices* voices) {
    if (subsong < 0 || (uint32_t)subsong >= player->module.subsongs) {
        return false;
    }
    const StModule module = player->module;
    memset(player, 0, sizeof(*player));
    player->module = module;
    for (int i = 0; i < ST_VOICES; i++) {
        player->voices[i].request_armed = true;
        player->voices[i].queued_armed = true;
        player->voices[i].note = -1;
        player->voices[i].instrument_shown = -1;
    }
    st_song_start(player, (uint8_t)(subsong + 1));
    st_sync(player, voices);
    return true;
}

StTickResult st_tick(StPlayer* player, NuadeVoices* voices) {
    player->looped = false;
    player->speed_count--;
    for (int i = 0; i < ST_VOICES; i++) {
        StVoice* v = &player->voices[i];
        st_queued_start_run(player, v);
        st_sequence_run(player, v);
        st_envelope_run(player, v);
        st_pitch_run(v);
        st_portamento_run(v);
    }
    if (player->speed_count == 0) {
        player->speed_count = player->speed;
    }
    if (player->restart != 0) {
        const uint8_t song = player->restart;
        player->restart = 0;
        st_song_start(player, song);
    } else {
        for (int i = 0; i < ST_VOICES; i++) {
            st_request_run(player, &player->voices[i]);
        }
    }
    st_sync(player, voices);
    return (StTickResult) { .looped = player->looped, .ended = player->failed };
}

uint32_t st_grid_ticks(const StPlayer* player, int subsong) {
    if (subsong < 0 || (uint32_t)subsong >= player->module.subsongs) {
        return 0;
    }
    return st_read_song(&player->module, (uint8_t)(subsong + 1)).speed;
}

StVoiceState st_voice_state(const StPlayer* player, int voice) {
    const StVoice* v = &player->voices[voice];
    return (StVoiceState) { .note = v->note, .instrument = v->instrument_shown };
}
