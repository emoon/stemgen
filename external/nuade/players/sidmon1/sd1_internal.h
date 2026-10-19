// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Daniel Collin
// What the loader and the player of SidMon 1.0 share: the format of the module's tables and the module as the replayer
// sees it in memory.

#pragma once

#include <nuade/players/sidmon1.h>

#include "nuade_paula_channel.h"

#define MIN_MODULE_SIZE 1024

// The tables of the module
#define SEQUENCE_ENTRY_SIZE 6
#define SEQUENCE_TRANSPOSE 5
#define TRACK_ROW_SIZE 5
#define INSTRUMENT_SIZE 32
#define WAVE_LIST_ENTRY_SIZE 16
#define WAVEFORM_SIZE 32
#define SAMPLE_ENTRY_SIZE 32
#define MAX_INSTRUMENTS 63
#define ROWS_PER_TRACK 64

// Song parameter record: two longs per mixer parameter (first mixer, second mixer)
#define SONG_MIX_SOURCE_1 0x00
#define SONG_MIX_SOURCE_2 0x08
#define SONG_MIX_DESTINATION 0x10
#define SONG_ROWS 0x18
#define SONG_POSITIONS 0x1C
#define SONG_SPEED 0x20
#define SONG_MIX_SPEED 0x24

// Instrument record
#define INSTRUMENT_WAVEFORM 0x00
#define INSTRUMENT_ARPEGGIO 0x04
#define INSTRUMENT_ATTACK_SPEED 0x14
#define INSTRUMENT_ATTACK_MAX 0x15
#define INSTRUMENT_DECAY_SPEED 0x16
#define INSTRUMENT_DECAY_MIN 0x17
#define INSTRUMENT_SUSTAIN_TIME 0x18
#define INSTRUMENT_RELEASE_SPEED 0x1A
#define INSTRUMENT_RELEASE_MIN 0x1B
#define INSTRUMENT_PHASE_SHIFT 0x1C
#define INSTRUMENT_PHASE_SPEED 0x1D
#define INSTRUMENT_FINE_TUNE 0x1E
#define INSTRUMENT_PITCH_FALL 0x1F

// Sample record
#define SAMPLE_START 0x00
#define SAMPLE_LOOP 0x04
#define SAMPLE_END 0x08
#define SAMPLE_NAME 0x0C

// The module as the replayer sees it

// The module followed by NUADE_PAULA_MAX_SAMPLE_BYTES zero bytes: a table index that runs off the module reads zeros,
// and so does Paula.
typedef struct Image {
    uint8_t* bytes;
    uint32_t size; // the module; bytes holds size + NUADE_PAULA_MAX_SAMPLE_BYTES
} Image;

#define IMAGE_LIMIT(m) ((uint64_t)(m)->size + NUADE_PAULA_MAX_SAMPLE_BYTES)

static inline uint8_t image_u8(const Image* m, uint32_t offset) {
    return offset < IMAGE_LIMIT(m) ? m->bytes[offset] : 0;
}

static inline int8_t image_s8(const Image* m, uint32_t offset) {
    return (int8_t)image_u8(m, offset);
}

static inline uint16_t image_u16(const Image* m, uint32_t offset) {
    return (uint16_t)(image_u8(m, offset) << 8 | image_u8(m, offset + 1));
}

static inline uint32_t image_u32(const Image* m, uint32_t offset) {
    return (uint32_t)image_u16(m, offset) << 16 | image_u16(m, offset + 2);
}

// The replayer writes only into the module: what lies past it is zeros for Paula.
static inline void image_set_u8(Image* m, uint32_t offset, uint8_t value) {
    if (offset < m->size) {
        m->bytes[offset] = value;
    }
}

// The first row of the track that the sequence entry at `entry` names.
uint32_t sd1_track_start(const Image* m, const Sd1Layout* l, uint32_t entry);

// Find the tables of a module through the replayer code at its start. False when the module is not a SidMon 1.0
// module the player can play.
bool sd1_layout_from_module(const Image* m, Sd1Layout* l);
