// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Daniel Collin
// Ported from NostalgicPlayer C# implementation by Thomas Neumann (MIT)
//
// SidMon 1.0 module loader. A module is the replayer followed by its data, and nothing says where the data is: the
// tables are found by the code that points at them. The replayer exists in a few builds (the oldest has no samples
// and no wave list); every instruction pattern below is one that the builds of the corpus share.

#include "sd1_internal.h"

#include "nuade_base.h"
#include "nuade_utils.h"
#include <stdlib.h>

#define CODE_SEARCH_SIZE 4096

#define FIRST_SPECIAL_INSTRUMENT 60

// Instruction words the locators look for
#define LEA_PC_A0 0x41FA
#define LEA_PC_A2 0x45FA
#define LEA_PC_A3 0x47FA
#define LEA_PC_A6 0x4DFA
#define MOVEA_L_A0_IND_A4 0x2850
#define ADDA_L_A0 0xD1E8
#define ADDA_L_A1 0xD3E9
#define ADDA_L_A4 0xD9EC
#define ADDA_L_A6 0xDDEE
#define ADDA_L_IMM_A6 0xDDFC
#define MOVEQ_3_D0 0x7003
#define MOVE_L_IMM_D0 0x203C
#define MOVEM_SAVE 0x48E7FFFEu
#define MOVEM_RESTORE 0x4CDF7FFFu
#define BSR_W 0x6100
#define BCC_W 0x6400
#define NOP 0x4E71
#define RTS 0x4E75
#define MOVE_W_IMM_ABS 0x33FC
#define DMACON_ALL_ON 0x800F
#define TST_L_A6_IND 0x4A96
#define NEG_B_A0_IND 0x4410
#define MOVE_L_A0_IND_D0 0x2010
#define CMPI_L_D16_A3 0x0CAB
#define CMPI_B_D0 0x0C00
#define CMPI_L_IMM_A6_IND 0x0C96
#define MOVE_B_D16_A4_D3 0x162C
#define MULU_W_IMM_D3 0xC6FC
#define LSL_W_3_D3 0xE74B
#define CLR_B_13_A0_HI 0x4228
#define CLR_B_13_A0_LO 0x0013
#define ASL_L_5_D1 0xEB81
#define LSL_W_5_D1 0xEB49

// ----------------------------------------------------------------------------------------------------------------
// Finding the tables

typedef struct Code {
    const uint8_t* data;
    int64_t size;
} Code;

static uint8_t code_u8(const Code* c, int64_t i) {
    return i >= 0 && i < c->size ? c->data[i] : 0;
}

static uint16_t code_u16(const Code* c, int64_t i) {
    return (uint16_t)(code_u8(c, i) << 8 | code_u8(c, i + 1));
}

static uint32_t code_u32(const Code* c, int64_t i) {
    return (uint32_t)code_u16(c, i) << 16 | code_u16(c, i + 2);
}

// The address a 16-bit displacement at `i` leads to, from `from`.
static int64_t code_target(const Code* c, int64_t i, int64_t from) {
    return from + (int16_t)code_u16(c, i);
}

// The first even position at or after `from` holding `word`, or -1. Stops 4 bytes short of the end.
static int64_t find_word(const Code* c, int64_t from, uint16_t word) {
    for (int64_t i = from < 0 ? 0 : from; i + 4 < c->size; i += 2) {
        if (code_u16(c, i) == word) {
            return i;
        }
    }
    return -1;
}

typedef struct Found {
    int64_t base; // the address the table pointers are relative to
    int64_t sequences[SD1_VOICES];
    int64_t track_data;
    int64_t track_offsets;
    int64_t instruments;
    int64_t play_note;
    int64_t song;
    int64_t wave_list;
    int64_t waveforms;
    int64_t samples;
    int64_t periods;
    uint32_t finetune_stride;
    uint32_t no_loop;
    int64_t mixer_variables[4]; // counter 1, position 1, counter 2, position 2
    int64_t filter_variable;
    int64_t counter_variable;
    int64_t flag_variables[2]; // new position, loop
    bool init_starts_playing;
    bool mixing;
    bool filter;
    bool clears_row_count;
    bool finetune_is_word;
    bool old_replayer;
    int64_t special_flag_variable;
    uint32_t special_offsets[SD1_SPECIAL_SAMPLES];
    uint16_t special_lengths[SD1_SPECIAL_SAMPLES];
} Found;

// A pointer the replayer takes from a table: the long at `at` is an offset from the base.
static bool find_displaced(const Code* c, int64_t* from, uint16_t word, int64_t base, int64_t* out) {
    const int64_t i = find_word(c, *from, word);
    if (i < 0) {
        return false;
    }
    *out = code_target(c, i + 2, base);
    *from = i + 4;
    return true;
}

// The init routine: the base of the module's pointer table, and the pointers to the sequences, tracks and instruments.
static bool locate_init(const Code* c, Found* f) {
    int64_t i = find_word(c, 0, LEA_PC_A0);
    if (i < 0) {
        return false;
    }
    f->base = code_target(c, i + 2, i + 2);
    int voice = 0;
    for (i += 4; i + 2 < c->size && voice < SD1_VOICES; i += 2) {
        if (code_u16(c, i) == ADDA_L_A0) {
            f->sequences[voice++] = code_target(c, i + 2, f->base);
        }
    }
    if (voice < SD1_VOICES) {
        return false;
    }
    for (; i + 2 < c->size; i += 2) {
        const uint16_t w = code_u16(c, i);
        if (w == MOVEQ_3_D0 || (w == MOVE_L_IMM_D0 && code_u8(c, i + 5) == 3)) {
            break;
        }
    }
    if (i + 4 >= c->size) {
        return false;
    }
    if (code_u16(c, i) == MOVE_L_IMM_D0) {
        i += 4;
    }
    if (code_u16(c, i + 2) != LEA_PC_A0 || code_u16(c, i + 6) != ADDA_L_A0) {
        return false;
    }
    f->track_data = code_target(c, i + 8, f->base);
    i += 10;
    if (!find_displaced(c, &i, ADDA_L_A4, f->base, &f->track_offsets)
        || !find_displaced(c, &i, ADDA_L_A4, f->base, &f->instruments)) {
        return false;
    }
    // The init routine ends by switching all four DMA channels on. The oldest replayers go on to clear the position
    // flags and load the speed counter there; later ones do that in a separate start routine.
    for (int64_t q = i; q + 12 < c->size; q += 2) {
        if (code_u16(c, q) == MOVE_W_IMM_ABS && code_u16(c, q + 2) == DMACON_ALL_ON) {
            f->init_starts_playing = code_u16(c, q + 8) != RTS;
            return true;
        }
    }
    return false;
}

// The oldest replayers play instruments 61 to 63 on voice 3 as fixed samples that are part of the replayer. The code
// that starts them compares the instrument number with 60, 61 and 62; each case loads a lea of the data and the length.
static bool locate_special_samples(const Code* c, Found* f, int64_t branch) {
    f->old_replayer = true;
    int64_t i = code_target(c, branch + 2, branch + 2);
    int found = 0;
    for (; i + 4 < c->size && code_u16(c, i) != RTS && found < SD1_SPECIAL_SAMPLES; i += 2) {
        if (code_u16(c, i) != CMPI_B_D0) {
            continue;
        }
        i += 8;
        if (code_u16(c, i) != LEA_PC_A6) {
            return false;
        }
        int64_t start = code_target(c, i + 2, i + 2);
        if (code_u16(c, i + 4) == ADDA_L_IMM_A6) {
            start += code_u32(c, i + 6);
            i += 6;
        }
        i += 10;
        if (code_u16(c, i) != MOVE_W_IMM_ABS) {
            return false;
        }
        f->special_offsets[found] = (uint32_t)start;
        f->special_lengths[found] = code_u16(c, i + 2);
        found++;
    }
    // The flag the code tests before it: a lea of a variable, then compare with 1
    for (int64_t q = branch - 0x20; q < branch; q += 2) {
        if (code_u16(c, q) == LEA_PC_A6 && code_u16(c, q + 4) == CMPI_L_IMM_A6_IND && code_u32(c, q + 6) == 1) {
            f->special_flag_variable = code_target(c, q + 2, q + 2);
        }
    }
    return found == SD1_SPECIAL_SAMPLES && f->special_flag_variable != 0;
}

// The code that plays a sample: a branch from the note routine to it, then the sample table and the value a sample's
// loop start has when it does not loop.
static bool locate_samples(const Code* c, Found* f, int64_t from) {
    int64_t i = from;
    while (i >= 0 && code_u16(c, i) != BCC_W) {
        i -= 2;
    }
    if (i < 4) {
        return false;
    }
    if (code_u16(c, i - 2) == 0x003C) {
        return locate_special_samples(c, f, i);
    }
    i = code_target(c, i + 2, i + 2);
    int64_t j = find_word(c, i, ADDA_L_A4);
    if (j < 0) {
        return false;
    }
    f->samples = code_target(c, j + 2, f->base);
    j = find_word(c, j, CMPI_L_D16_A3);
    if (j < 0) {
        return false;
    }
    f->no_loop = code_u32(c, j + 2);
    return true;
}

// The routines the interrupt ends with: the waveform mixer, the filter and the loop-register update of the samples.
static bool locate_tail(const Code* c, Found* f, int64_t from) {
    int64_t restore = from;
    while (restore + 4 < c->size && code_u32(c, restore) != MOVEM_RESTORE) {
        restore += 2;
    }
    if (restore + 4 >= c->size) {
        return false;
    }
    int64_t i = restore - 4;
    if (code_u16(c, i) == 0x00DF || code_u16(c, i) == NOP) {
        i -= 6;
    }
    for (int k = 0; k < 3 && code_u16(c, i) == BSR_W; k++, i -= 4) {
        const int64_t routine = code_target(c, i + 2, i + 2);
        if (code_u16(c, routine + 14) == NEG_B_A0_IND) {
            f->filter = true;
            for (int64_t q = routine; q < routine + 0x20; q += 2) {
                if (code_u16(c, q) == LEA_PC_A6) {
                    f->filter_variable = code_target(c, q + 2, q + 2);
                    break;
                }
            }
        } else if (code_u16(c, routine + 12) == MOVE_L_A0_IND_D0 || code_u16(c, routine + 14) == MOVE_L_A0_IND_D0) {
            f->mixing = true;
            int variables = 0;
            for (int64_t q = routine; q < routine + 0x1C0 && variables < 4; q += 2) {
                const uint16_t w = code_u16(c, q);
                if (w != LEA_PC_A6 && w != LEA_PC_A2) {
                    continue;
                }
                const int64_t target = code_target(c, q + 2, q + 2);
                bool seen = target == f->base;
                for (int v = 0; v < variables; v++) {
                    seen = seen || f->mixer_variables[v] == target;
                }
                if (!seen) {
                    f->mixer_variables[variables++] = target;
                }
            }
            if (variables < 4) {
                return false;
            }
        }
    }
    return true;
}

// The interrupt routine and what it calls: the note routine, the song parameters, the tail and the tables the note
// routine uses.
static bool locate_play(const Code* c, Found* f) {
    int64_t i = 0;
    while (i + 4 < c->size && code_u32(c, i) != MOVEM_SAVE) {
        i += 2;
    }
    i = find_word(c, i, BSR_W);
    if (i < 0) {
        return false;
    }
    f->play_note = code_target(c, i + 2, i + 2);
    if (!find_displaced(c, &i, ADDA_L_A1, f->base, &f->song) || !locate_tail(c, f, 0)) {
        return false;
    }
    i = find_word(c, f->play_note, ADDA_L_A4);
    if (i >= 0) {
        f->wave_list = code_target(c, i + 2, f->base);
    } else {
        i = f->play_note;
    }
    for (; i + 4 < c->size; i += 2) {
        const uint16_t w = code_u16(c, i);
        if (w == ASL_L_5_D1 || w == LSL_W_5_D1) {
            break;
        }
    }
    if (i + 4 >= c->size || (i = find_word(c, i, ADDA_L_A6)) < 0) {
        return false;
    }
    f->waveforms = code_target(c, i + 2, f->base);

    // The note routine finds a period through a lea; the fine tune scales an index by a multiply or a shift.
    for (int64_t q = f->play_note; q + 8 < c->size && q < f->play_note + 0x400; q += 2) {
        if (code_u16(c, q) != LEA_PC_A3 || code_u16(c, q + 4) != MOVEA_L_A0_IND_A4) {
            continue;
        }
        f->periods = code_target(c, q + 2, q + 2);
        for (int64_t j = q; j < q + 0x80; j += 2) {
            if (code_u16(c, j) != MOVE_B_D16_A4_D3) {
                continue;
            }
            if (code_u16(c, j + 4) == MULU_W_IMM_D3) {
                f->finetune_stride = code_u16(c, j + 6) / 2u;
            } else if (code_u16(c, j + 4) == LSL_W_3_D3) {
                f->finetune_stride = 68;
                f->finetune_is_word = true;
            }
            break;
        }
        break;
    }
    // The note routine reads the speed counter (compared with 0) and then the two position flags, each through a lea.
    int variables = 0;
    int64_t seen_variables[3];
    bool counter_found = false;
    for (int64_t q = f->play_note; q < f->play_note + 0x90 && variables < 3; q += 2) {
        if (code_u16(c, q) != LEA_PC_A6) {
            continue;
        }
        const int64_t target = code_target(c, q + 2, q + 2);
        const bool tests_zero = (code_u16(c, q + 4) == CMPI_L_IMM_A6_IND && code_u32(c, q + 6) == 0)
                                || code_u16(c, q + 4) == TST_L_A6_IND;
        bool known = target == f->base;
        for (int v = 0; v < variables; v++) {
            known = known || seen_variables[v] == target;
        }
        if (!counter_found && !tests_zero) {
            continue;
        }
        counter_found = true;
        if (!known) {
            seen_variables[variables++] = target;
        }
    }
    if (variables < 3) {
        return false;
    }
    f->counter_variable = seen_variables[0];
    f->flag_variables[0] = seen_variables[1];
    f->flag_variables[1] = seen_variables[2];
    for (int64_t q = f->play_note; q < f->play_note + 0x80; q += 2) {
        if (code_u16(c, q) == CLR_B_13_A0_HI && code_u16(c, q + 2) == CLR_B_13_A0_LO) {
            f->clears_row_count = true;
        }
    }
    return f->periods != 0 && locate_samples(c, f, i) && (f->old_replayer || f->finetune_stride != 0);
}

static bool locate(const Code* c, Found* f) {
    *f = (Found) { 0 };
    return locate_init(c, f) && locate_play(c, f);
}

// ----------------------------------------------------------------------------------------------------------------
// Loading

static uint32_t table_pointer(const Image* m, int64_t at, int64_t base) {
    return (uint32_t)(image_u32(m, (uint32_t)at) + (uint32_t)base);
}

static int compare_u32(const void* a, const void* b) {
    const uint32_t x = *(const uint32_t*)a;
    const uint32_t y = *(const uint32_t*)b;
    return x < y ? -1 : x > y;
}

// The offset of the data block that follows `offset` in the module (the module size for the last one).
static uint32_t next_block(const uint32_t* sorted, size_t count, uint32_t size, uint32_t offset) {
    for (size_t i = 0; i < count; i++) {
        if (sorted[i] > offset) {
            return sorted[i];
        }
    }
    return size;
}

// The tables have no sizes; each ends where the next table starts.
static bool count_blocks(const Image* m, Sd1Layout* l) {
    // (a table the variant has not stays 0, below every other)
    uint32_t sorted[]
        = { l->sequences[0], l->sequences[1], l->sequences[2], l->sequences[3], l->track_data, l->track_offsets,
            l->instruments,  l->wave_list,    l->waveforms,    l->samples,      l->song };
    qsort(sorted, nuade_sizeof_array(sorted), sizeof(sorted[0]), compare_u32);
    const size_t n = nuade_sizeof_array(sorted);
    const uint32_t end = m->size;

    if (next_block(sorted, n, end, l->track_offsets) == end) {
        // The offsets are the last block: count them up to the first that is 0 or out of the module.
        l->track_count = 1; // the first is always 0
        for (uint32_t at = l->track_offsets + 4; at + 4 <= m->size; at += 4) {
            const uint32_t offset = image_u32(m, at);
            if (offset == 0 || offset > m->size) {
                break;
            }
            l->track_count++;
        }
    } else {
        l->track_count = (int)((next_block(sorted, n, end, l->track_offsets) - l->track_offsets) / 4);
    }
    l->instrument_count = (int)((next_block(sorted, n, end, l->instruments) - l->instruments) / INSTRUMENT_SIZE);
    if (l->instrument_count > MAX_INSTRUMENTS) {
        l->instrument_count = MAX_INSTRUMENTS;
    }
    if (l->wave_list != 0) {
        l->wave_list_count = (int)((next_block(sorted, n, end, l->wave_list) - l->wave_list) / WAVE_LIST_ENTRY_SIZE);
    }
    l->waveform_count = (int)((next_block(sorted, n, end, l->waveforms) - l->waveforms) / WAVEFORM_SIZE);
    return l->track_count > 0 && l->instrument_count > 0 && l->waveform_count > 0
           && (l->old_replayer || l->wave_list_count > 0);
}

uint32_t sd1_track_start(const Image* m, const Sd1Layout* l, uint32_t entry) {
    return l->track_data + image_u32(m, l->track_offsets + image_u32(m, entry) * 4);
}

// The highest instrument number a track row of the song names.
static int highest_instrument(const Image* m, const Sd1Layout* l) {
    int highest = 0;
    for (int voice = 0; voice < SD1_VOICES; voice++) {
        for (int position = 0; position < l->position_count; position++) {
            uint32_t row = sd1_track_start(m, l, l->sequences[voice] + (uint32_t)position * SEQUENCE_ENTRY_SIZE);
            for (int line = 0; line < ROWS_PER_TRACK; row += TRACK_ROW_SIZE) {
                const int instrument = image_u8(m, row + 1);
                highest = instrument > highest ? instrument : highest;
                line += image_u8(m, row + 4) + 1;
            }
        }
    }
    return highest;
}

// The tables must lie in the module; a sequence entry must name a track.
static bool check_layout(const Image* m, const Sd1Layout* l) {
    const uint32_t blocks[] = { l->sequences[0],  l->sequences[1], l->sequences[2], l->sequences[3], l->track_data,
                                l->track_offsets, l->instruments,  l->waveforms,    l->song,         l->periods };
    for (size_t i = 0; i < nuade_sizeof_array(blocks); i++) {
        if (blocks[i] >= m->size) {
            return false;
        }
    }
    if (l->old_replayer) {
        for (int i = 0; i < SD1_SPECIAL_SAMPLES; i++) {
            if (l->special_samples[i].start >= m->size) {
                return false;
            }
        }
    } else if (l->wave_list >= m->size || l->samples >= m->size) {
        return false;
    }
    if (l->position_count < 1) {
        return false;
    }
    for (int v = 0; v < SD1_VOICES; v++) {
        if ((uint64_t)l->sequences[v] + (uint64_t)l->position_count * SEQUENCE_ENTRY_SIZE > m->size) {
            return false;
        }
        for (int p = 0; p < l->position_count; p++) {
            const uint32_t track = image_u32(m, l->sequences[v] + (uint32_t)p * SEQUENCE_ENTRY_SIZE);
            if (track >= (uint32_t)l->track_count) {
                return false;
            }
            const uint32_t offset = image_u32(m, l->track_offsets + track * 4);
            if ((uint64_t)l->track_data + offset >= m->size) {
                return false;
            }
        }
    }
    return true;
}

bool sd1_layout_from_module(const Image* m, Sd1Layout* l) {
    const Code code = { m->bytes, m->size < CODE_SEARCH_SIZE ? m->size : CODE_SEARCH_SIZE };
    Found f;
    if (m->size < MIN_MODULE_SIZE || !locate(&code, &f)) {
        return false;
    }
    *l = (Sd1Layout) { 0 };
    for (int v = 0; v < SD1_VOICES; v++) {
        l->sequences[v] = table_pointer(m, f.sequences[v], f.base);
    }
    l->track_data = table_pointer(m, f.track_data, f.base);
    l->track_offsets = table_pointer(m, f.track_offsets, f.base);
    l->instruments = table_pointer(m, f.instruments, f.base);
    l->waveforms = table_pointer(m, f.waveforms, f.base);
    l->song = table_pointer(m, f.song, f.base);
    l->old_replayer = f.old_replayer;
    if (f.old_replayer) {
        for (int i = 0; i < SD1_SPECIAL_SAMPLES; i++) {
            l->special_samples[i]
                = (Sd1SpecialSample) { .start = f.special_offsets[i], .length_words = f.special_lengths[i] };
        }
        l->special_flag = (uint32_t)f.special_flag_variable;
    } else {
        l->wave_list = table_pointer(m, f.wave_list, f.base);
        l->samples = table_pointer(m, f.samples, f.base);
        // The sample records sit between the table's first long and the data it points to
        l->sample_count = (int)(image_u32(m, l->samples) / SAMPLE_ENTRY_SIZE);
    }
    l->periods = (uint32_t)f.periods;
    l->finetune_stride = f.finetune_stride;
    l->finetune_is_word = f.finetune_is_word;
    l->no_loop = f.no_loop;
    l->mixing = f.mixing;
    l->filter = f.filter;
    l->clears_row_count = f.clears_row_count;
    if (f.mixing) {
        l->mix_counter[0] = (uint32_t)f.mixer_variables[0];
        l->mix_position[0] = (uint32_t)f.mixer_variables[1];
        l->mix_counter[1] = (uint32_t)f.mixer_variables[2];
        l->mix_position[1] = (uint32_t)f.mixer_variables[3];
    }
    l->init_starts_playing = f.init_starts_playing;
    l->counter = (uint32_t)f.counter_variable;
    l->new_position_flag = (uint32_t)f.flag_variables[0];
    l->loop_flag = (uint32_t)f.flag_variables[1];
    if (f.filter) {
        l->filter_position = (uint32_t)f.filter_variable;
        l->filter_shares_position = f.mixing && f.filter_variable == f.mixer_variables[1];
    }
    const uint32_t positions = image_u32(m, l->song + SONG_POSITIONS);
    l->position_count = positions > 1 && positions <= m->size ? (int)positions - 1 : 0;
    if (!count_blocks(m, l) || !check_layout(m, l)) {
        return false;
    }
    // A row may name an instrument past the table, whose record then is whatever lies there
    const int highest = highest_instrument(m, l);
    l->instrument_count = highest > l->instrument_count ? highest : l->instrument_count;
    return true;
}
