// SPDX-License-Identifier: MIT
// Richard Joseph game driver: reading a converted module (Wanted Team's "Richard Joseph_v2.asm" describes the format
// and the game driver inside it).
//
// The file is an AmigaDOS hunk executable with one chip code hunk. Its first bytes are `moveq #-1,d0; rts`,
// "R.JOSEPH", and the eagleplayer's entry points (Play, Audio, InitPlayer, InitSong, sample info), then names, sizes
// and the number of subsongs. The driver itself is the game's 68k code. The eagleplayer calls into it; this player
// replaces the code by C, so it has to know which driver it is: the driver code from the audio interrupt handler to the
// last routine is hashed, with the relocated longs left out (they hold module addresses), and compared with the builds
// it was translated from. The operands that hold the driver's tables are then read at fixed places in that code.

#include "nuade_base.h"
#include "nuade_paula_channel.h"
#include "nuade_utils.h"
#include "rjg_internal.h"
#include <stdlib.h>
#include <string.h>

#define HUNK_HEADER 0x3F3
#define HUNK_CODE 0x3E9
#define HUNK_RELOC32 0x3EC
#define HUNK_END 0x3F2
#define HUNK_SIZE_MASK 0x3FFFFFFFu
// The largest body the port reads: a table address plus a row offset stays far from the wrap of 32 bits. The game
// modules are well below 1 MB.
#define MAX_BODY_SIZE 0x10000000u

#define HUNK_HEADER_SIZE 32 // the header up to the first byte of the code hunk with the standard layout
#define ID_MOVEQ_RTS 0x70FF4E75u
#define ID_R_JO 0x522E4A4Fu // "R.JO"
#define ID_SEPH 0x53455048u // "SEPH"
#define HEADER_PLAY 0x0C
#define HEADER_INITPLAYER 0x14
#define HEADER_INITSONG 0x18
#define HEADER_SUBSONGS 0x40
#define HEADER_SIZE 0x44

// InitSong of a conversion: movem.l d0-d7/a0-a6,-(a7); subq.w #1,d0; moveq #0,d1; lea map(pc),a0;
// move.b (0,a0,d0.w),d0; jsr start.l; movem.l (a7)+,d0-d7/a0-a6; lea ..(pc),a0; rts
#define INITSONG_SIZE 32
#define INITSONG_MAP_DISPLACEMENT 10
#define INITSONG_START 18

#define MAX_ALSO_AT 4
#define MAX_SUBSONGS 256

#define FNV_OFFSET 0xCBF29CE484222325ull
#define FNV_PRIME 0x100000001B3ull

// A driver build: where its code sits relative to the Play entry, the hash of its code and the places in it that hold
// the operands the port needs (offsets from the start of the code). The builds differ in where the tune start sits, in
// whether a sequence start ends with the voice's DMA off and in how much of the voice record it clears; the sequencer
// is the same.
typedef struct BuildSpec {
    RjgStart start_kind;
    uint32_t play_to_start;
    uint32_t window_size;
    uint64_t hash;
    uint32_t stack_top_at[RJG_VOICES]; // long operands: the four call stack tops
    uint32_t dummy_at;                 // long: the silent word
    uint32_t sequences_at;             // long: the sequence table
    uint32_t modulation_at;
    uint32_t envelopes_at;
    uint32_t instruments_at;
    // Other places of the code that hold the same addresses (0 ends a list): a table whose sites disagree is not the
    // driver's, and the port could not say which site the original follows.
    uint32_t dummy_also_at[MAX_ALSO_AT];
    uint32_t sequences_also_at[MAX_ALSO_AT];
    uint32_t instruments_also_at[MAX_ALSO_AT];
    uint32_t tunes_at; // long: the tune table; 0 for none
    uint32_t tempo_at; // the tempo word: a pc-relative word or an absolute long
    bool tempo_absolute;
    uint32_t tempo_scale_at; // long: the number the tempo command divides
    uint32_t clear_longs_at; // word: the count of the loop that clears a voice record at a sequence start
    bool stop_dma_after_start;
    uint32_t tune_start_at;  // where the tune start routine begins; 0 for none
    uint32_t start_voice_at; // where the sequence start routine begins
} BuildSpec;

static const BuildSpec k_builds[] = {
    // Cadaver, main image
    { .start_kind = RjgStart_Tune,
      .play_to_start = 0x74,
      .window_size = 0x8b4,
      .hash = 0xa236aa58d55fb992ull,
      .stack_top_at = { 0x2f2, 0x2fa, 0x302, 0x30a },
      .dummy_at = 0x23a,
      .sequences_at = 0x7c8,
      .modulation_at = 0x74e,
      .envelopes_at = 0x836,
      .instruments_at = 0x852,
      .dummy_also_at = { 0x41c },
      .sequences_also_at = { 0x43a, 0x878 },
      .instruments_also_at = { 0x452, 0x4fe, 0x5ac },
      .tunes_at = 0x3a6,
      .tempo_at = 0x4c0,
      .tempo_absolute = true,
      .tempo_scale_at = 0x712,
      .clear_longs_at = 0x3f8,
      .stop_dma_after_start = true,
      .tune_start_at = 0x39c,
      .start_voice_at = 0x3d6 },
    // Cadaver, sound sets 2-4
    { .start_kind = RjgStart_Tune,
      .play_to_start = 0x60,
      .window_size = 0x8c8,
      .hash = 0xcf3871e6c1f19e41ull,
      .stack_top_at = { 0x312, 0x31a, 0x322, 0x32a },
      .dummy_at = 0x25a,
      .sequences_at = 0x7dc,
      .modulation_at = 0x74c,
      .envelopes_at = 0x84a,
      .instruments_at = 0x866,
      .dummy_also_at = { 0x4a, 0x434 },
      .sequences_also_at = { 0x44e, 0x88c },
      .instruments_also_at = { 0x466, 0x508, 0x5b2 },
      .tunes_at = 0x3be,
      .tempo_at = 0x4cc,
      .tempo_absolute = false,
      .tempo_scale_at = 0x714,
      .clear_longs_at = 0x410,
      .stop_dma_after_start = false,
      .tune_start_at = 0x3b4,
      .start_voice_at = 0x3ee },
    // The Lure of the Temptress
    { .start_kind = RjgStart_TuneFour,
      .play_to_start = 0xee,
      .window_size = 0x924,
      .hash = 0x459d90170be14c46ull,
      .stack_top_at = { 0x380, 0x388, 0x390, 0x398 },
      .dummy_at = 0x2c8,
      .sequences_at = 0x838,
      .modulation_at = 0x7be,
      .envelopes_at = 0x8a6,
      .instruments_at = 0x8c2,
      .dummy_also_at = { 0xd8, 0x4a8 },
      .sequences_also_at = { 0x8e8 },
      .instruments_also_at = { 0x4d8, 0x57a, 0x624 },
      .tunes_at = 0x42c,
      .tempo_at = 0x53e,
      .tempo_absolute = false,
      .tempo_scale_at = 0x786,
      .clear_longs_at = 0x484,
      .stop_dma_after_start = true,
      .tune_start_at = 0x422,
      .start_voice_at = 0x462 },
    // Moonstone
    { .start_kind = RjgStart_Sequence,
      .play_to_start = 0xf2,
      .window_size = 0x922,
      .hash = 0x63381630926040f8ull,
      .stack_top_at = { 0x38c, 0x394, 0x39c, 0x3a4 },
      .dummy_at = 0x2d4,
      .sequences_at = 0x836,
      .modulation_at = 0x7bc,
      .envelopes_at = 0x8a4,
      .instruments_at = 0x8c0,
      .dummy_also_at = { 0xda, 0x48e },
      .sequences_also_at = { 0x8e6 },
      .instruments_also_at = { 0x4c2, 0x56c, 0x61a },
      .tunes_at = 0,
      .tempo_at = 0x530,
      .tempo_absolute = false,
      .tempo_scale_at = 0x780,
      .clear_longs_at = 0x46a,
      .stop_dma_after_start = true,
      .tune_start_at = 0,
      .start_voice_at = 0x448 },
};

typedef struct Hunk {
    const uint8_t* body;
    uint32_t body_size;
    uint32_t* relocs; // sorted offsets of the relocated longs
    uint32_t reloc_count;
} Hunk;

uint8_t rjg_u8(const RjgModule* module, uint64_t offset) {
    return offset < module->size ? module->data[offset] : 0;
}

uint16_t rjg_u16(const RjgModule* module, uint64_t offset) {
    return (uint16_t)(rjg_u8(module, offset) << 8 | rjg_u8(module, offset + 1));
}

uint32_t rjg_u32(const RjgModule* module, uint64_t offset) {
    return (uint32_t)rjg_u16(module, offset) << 16 | rjg_u16(module, offset + 2);
}

RjgInstrument rjg_instrument(const RjgModule* module, uint32_t row) {
    const uint64_t at = (uint64_t)module->instruments + (uint64_t)row * RJG_INSTRUMENT_SIZE;
    return (RjgInstrument) { .loops = (int16_t)rjg_u16(module, at),
                             .loop_offset = rjg_u16(module, at + 2),
                             .length_words = rjg_u16(module, at + 4),
                             .sample = rjg_u32(module, at + 6),
                             .period_table = rjg_u32(module, at + 10) };
}

uint32_t rjg_sequence_offset(const RjgModule* module, uint8_t sequence) {
    return rjg_u32(module, (uint64_t)module->sequences + (uint64_t)sequence * RJG_SEQUENCE_ENTRY);
}

uint8_t rjg_subsong_argument(const RjgModule* module, uint32_t subsong) {
    return rjg_u8(module, (uint64_t)module->subsong_map + subsong);
}

static uint32_t be32_at(const uint8_t* data, size_t size, size_t offset) {
    return offset <= size && size - offset >= 4 ? nuade_read_be32(data + offset) : 0;
}

static int compare_offsets(const void* a, const void* b) {
    const uint32_t x = *(const uint32_t*)a;
    const uint32_t y = *(const uint32_t*)b;
    return x < y ? -1 : x > y;
}

// The relocation blocks behind the code hunk, from `at` to the end of the hunk. The caller frees hunk->relocs.
static bool read_relocs(const uint8_t* data, size_t size, size_t at, Hunk* hunk) {
    uint32_t capacity = 0;
    for (;;) {
        const uint32_t type = be32_at(data, size, at) & HUNK_SIZE_MASK;
        at += 4;
        if (type == HUNK_END) {
            return true;
        }
        if (type != HUNK_RELOC32) {
            return false;
        }
        for (;;) {
            if (at > size || size - at < 4) {
                return false;
            }
            const uint32_t count = nuade_read_be32(data + at);
            at += 4;
            if (count == 0) {
                break;
            }
            if (size - at < 4 || nuade_read_be32(data + at) != 0 || count > (size - at - 4) / 4) {
                return false;
            }
            at += 4;
            if (hunk->reloc_count + count > capacity) {
                capacity = hunk->reloc_count + count;
                uint32_t* grown = realloc(hunk->relocs, (size_t)capacity * sizeof(uint32_t));
                if (grown == nullptr) {
                    return false;
                }
                hunk->relocs = grown;
            }
            for (uint32_t i = 0; i < count; i++, at += 4) {
                const uint32_t offset = nuade_read_be32(data + at);
                if (offset > hunk->body_size - 4 || (offset & 1) != 0) {
                    return false;
                }
                hunk->relocs[hunk->reloc_count++] = offset;
            }
        }
    }
}

// The file's one code hunk and its relocations. The eagleplayer's check wants the standard header: no resident
// library names, one hunk, and a size entry that asks for chip memory.
static bool parse_hunk(const uint8_t* data, size_t size, Hunk* hunk) {
    if (size < HUNK_HEADER_SIZE + 8 || nuade_read_be32(data) != HUNK_HEADER || nuade_read_be32(data + 4) != 0
        || nuade_read_be32(data + 8) != 1 || nuade_read_be32(data + 12) != 0 || nuade_read_be32(data + 16) != 0
        || data[20] == 0 || nuade_read_be32(data + 24) != HUNK_CODE) {
        return false;
    }
    const uint32_t table_words = nuade_read_be32(data + 20) & HUNK_SIZE_MASK;
    const uint32_t words = nuade_read_be32(data + 28);
    if (words > table_words || words > (size - HUNK_HEADER_SIZE) / 4 || words * 4 < HEADER_SIZE
        || words * 4 > MAX_BODY_SIZE) {
        return false;
    }
    hunk->body = data + HUNK_HEADER_SIZE;
    hunk->body_size = words * 4;
    hunk->relocs = nullptr;
    hunk->reloc_count = 0;
    if (!read_relocs(data, size, HUNK_HEADER_SIZE + (size_t)hunk->body_size, hunk)) {
        free(hunk->relocs);
        return false;
    }
    if (hunk->reloc_count != 0) {
        qsort(hunk->relocs, hunk->reloc_count, sizeof(uint32_t), compare_offsets);
    }
    return true;
}

// The eagleplayer's module check, on the body: the signature and the entry points it needs.
static bool check_header(const Hunk* hunk) {
    const uint8_t* b = hunk->body;
    if (nuade_read_be32(b) != ID_MOVEQ_RTS || nuade_read_be32(b + 4) != ID_R_JO || nuade_read_be32(b + 8) != ID_SEPH) {
        return false;
    }
    for (uint32_t at = HEADER_PLAY; at <= HEADER_INITSONG; at += 4) {
        const uint32_t entry = nuade_read_be32(b + at);
        if (entry == 0 || entry >= hunk->body_size) {
            return false;
        }
    }
    return nuade_read_be32(b + HEADER_SUBSONGS) != 0;
}

// FNV-1a of the code window with the relocated longs taken out.
static uint64_t window_hash(const Hunk* hunk, uint32_t start, uint32_t size) {
    uint64_t hash = FNV_OFFSET;
    uint32_t next = 0;
    while (next < hunk->reloc_count && hunk->relocs[next] + 4 <= start) {
        next++;
    }
    for (uint32_t i = 0; i < size; i++) {
        const uint32_t at = start + i;
        while (next < hunk->reloc_count && hunk->relocs[next] + 4 <= at) {
            next++;
        }
        const bool relocated = next < hunk->reloc_count && hunk->relocs[next] <= at;
        hash = (hash ^ (relocated ? 0 : hunk->body[at])) * FNV_PRIME;
    }
    return hash;
}

static const BuildSpec* find_build(const Hunk* hunk) {
    const uint32_t play = nuade_read_be32(hunk->body + HEADER_PLAY);
    for (size_t i = 0; i < nuade_sizeof_array(k_builds); i++) {
        const BuildSpec* spec = &k_builds[i];
        if (play < spec->play_to_start) {
            continue;
        }
        const uint32_t start = play - spec->play_to_start;
        if (start > hunk->body_size || hunk->body_size - start < spec->window_size) {
            continue;
        }
        if (window_hash(hunk, start, spec->window_size) == spec->hash) {
            return spec;
        }
    }
    return nullptr;
}

// InitSong must be the conversion's stub: the subsong number minus one indexes a byte map; the byte goes to the
// driver's start routine.
static bool read_subsong_list(RjgModule* module, const Hunk* hunk, const BuildSpec* spec, uint32_t window) {
    const uint8_t* b = hunk->body;
    const uint32_t at = nuade_read_be32(b + HEADER_INITSONG);
    static const uint8_t expected[INITSONG_SIZE] = {
        0x48, 0xE7, 0xFF, 0xFE, 0x53, 0x40, 0x72, 0x00, 0x41, 0xFA, 0,    0,    0x10, 0x30, 0x00, 0x00,
        0x4E, 0xB9, 0,    0,    0,    0,    0x4C, 0xDF, 0x7F, 0xFF, 0x41, 0xFA, 0,    0,    0x4E, 0x75,
    };
    if (at > hunk->body_size || hunk->body_size - at < INITSONG_SIZE) {
        return false;
    }
    for (uint32_t i = 0; i < INITSONG_SIZE; i++) {
        const bool operand = (i >= 10 && i < 12) || (i >= 18 && i < 22) || (i >= 28 && i < 30);
        if (!operand && b[at + i] != expected[i]) {
            return false;
        }
    }
    const uint32_t start = nuade_read_be32(b + at + INITSONG_START);
    const uint32_t routine = spec->tune_start_at != 0 ? spec->tune_start_at : spec->start_voice_at;
    if (start != window + routine) {
        return false;
    }
    const int32_t displacement = (int16_t)nuade_read_be16(b + at + INITSONG_MAP_DISPLACEMENT);
    const int64_t map = (int64_t)at + INITSONG_MAP_DISPLACEMENT + displacement;
    module->subsong_count = nuade_read_be32(b + HEADER_SUBSONGS);
    // The stub's move.b into d0 keeps the high byte of the subsong number, which the start routine then uses: with
    // more than 256 subsongs the byte map is not the whole argument. The game modules have 10 or 15.
    if (module->subsong_count > MAX_SUBSONGS) {
        return false;
    }
    if (map < 0 || (uint64_t)map > hunk->body_size || module->subsong_count > hunk->body_size - (uint64_t)map) {
        return false;
    }
    module->subsong_map = (uint32_t)map;
    return true;
}

static uint32_t operand_long(const Hunk* hunk, uint32_t window, uint32_t at) {
    return be32_at(hunk->body, hunk->body_size, (size_t)window + at);
}

// The operand at each of the sites must hold the same table address.
static bool sites_agree(const Hunk* hunk, uint32_t window, uint32_t value, const uint32_t* also_at) {
    for (size_t i = 0; i < MAX_ALSO_AT && also_at[i] != 0; i++) {
        if (operand_long(hunk, window, also_at[i]) != value) {
            return false;
        }
    }
    return true;
}

static bool read_tables(RjgModule* module, const Hunk* hunk, const BuildSpec* spec, uint32_t window) {
    uint32_t stack_top[RJG_VOICES];
    for (uint32_t i = 0; i < RJG_VOICES; i++) {
        stack_top[i] = operand_long(hunk, window, spec->stack_top_at[i]);
    }
    for (uint32_t i = 1; i < RJG_VOICES; i++) {
        if (stack_top[i] - stack_top[i - 1] != RJG_STACK_SPACING) {
            return false;
        }
    }
    module->dummy = operand_long(hunk, window, spec->dummy_at);
    module->sequences = operand_long(hunk, window, spec->sequences_at);
    module->modulation = operand_long(hunk, window, spec->modulation_at);
    module->envelopes = operand_long(hunk, window, spec->envelopes_at);
    module->instruments = operand_long(hunk, window, spec->instruments_at);
    module->tunes = spec->tunes_at != 0 ? operand_long(hunk, window, spec->tunes_at) : 0;
    module->tempo_scale = operand_long(hunk, window, spec->tempo_scale_at);
    module->clear_longs = nuade_read_be16(hunk->body + window + spec->clear_longs_at);
    if (!sites_agree(hunk, window, module->dummy, spec->dummy_also_at)
        || !sites_agree(hunk, window, module->sequences, spec->sequences_also_at)
        || !sites_agree(hunk, window, module->instruments, spec->instruments_also_at)) {
        return false;
    }
    uint32_t tempo_address;
    if (spec->tempo_absolute) {
        tempo_address = operand_long(hunk, window, spec->tempo_at);
    } else {
        const int32_t displacement = (int16_t)nuade_read_be16(hunk->body + window + spec->tempo_at);
        tempo_address = (uint32_t)((int64_t)window + spec->tempo_at + displacement);
    }
    const uint32_t size = hunk->body_size;
    // The tables lie inside the module; a read past its end gives 0 either way. A tune table cannot be where the
    // header is.
    const uint32_t bases[] = { module->dummy,       module->sequences, module->modulation, module->envelopes,
                               module->instruments, module->tunes,     tempo_address };
    for (size_t i = 0; i < nuade_sizeof_array(bases); i++) {
        if (bases[i] > size) {
            return false;
        }
    }
    if (spec->tunes_at != 0 && module->tunes < HEADER_SIZE) {
        return false;
    }
    if (tempo_address > size - 2 || stack_top[0] < RJG_STACK_SPACING) {
        return false;
    }
    module->tempo_initial = nuade_read_be16(hunk->body + tempo_address);
    return true;
}

bool rjg_module_load(RjgModule* module, const uint8_t* data, size_t size) {
    Hunk hunk;
    if (data == nullptr || !parse_hunk(data, size, &hunk)) {
        return false;
    }
    bool ok = false;
    const BuildSpec* spec = check_header(&hunk) ? find_build(&hunk) : nullptr;
    if (spec != nullptr) {
        const uint32_t window = nuade_read_be32(hunk.body + HEADER_PLAY) - spec->play_to_start;
        *module = (RjgModule) { .size = hunk.body_size, .start_kind = spec->start_kind };
        module->stop_dma_after_start = spec->stop_dma_after_start;
        ok = read_tables(module, &hunk, spec, window) && read_subsong_list(module, &hunk, spec, window);
        if (ok) {
            module->data = nuade_paula_padded_copy(hunk.body, hunk.body_size);
            ok = module->data != nullptr;
            if (ok) {
                // InitPlayer of the conversion clears the first long: it is the empty sample
                memset(module->data, 0, 4);
            }
        }
    }
    free(hunk.relocs);
    return ok;
}
