// SPDX-License-Identifier: MIT
// Future Player, ported from the Future Player replayer in Wanted Team's "Future Player_v1.asm" ((c) 1988-89 Paul van
// der Valk, adapted by Wanted Team; Hybris replayer (c) 1989 Discovery Software).
//
// A module is an Amiga executable of one hunk: the replayer's header, a table of songs, and the song data, all
// referring to each other by absolute address (the file relocates them). A song has a track for each of the four
// voices; a track is a byte stream of notes and commands. An instrument is a record of an envelope, up to four
// modulation tables and a sample or a sequence of samples.
//
// One tick steps the fade, runs the sequencer of every voice when the tempo counter ends, then runs the sound of
// every voice (period, sample address, volume, envelope).

#include <nuade/players/futureplayer.h>

#include "nuade_base.h"
#include "nuade_paula_channel.h"
#include "nuade_tracker.h"
#include "nuade_utils.h"
#include <stdlib.h>
#include <string.h>

#define FP_HUNK_HEADER 0x3F3
#define FP_HUNK_CODE 0x3E9
#define FP_HUNK_RELOC32 0x3EC
#define FP_HUNK_END 0x3F2
#define FP_HEADER_BYTES 32 // the hunk header up to the data
#define FP_CODE_BYTES 12   // the stub 0x70FF4E75 and "F.PLAYER" that the eagleplayer checks
#define FP_SONG_TABLE 32   // offset of the song table in the hunk
#define FP_SONG_BYTES 8
#define FP_SONG_HEADER_BYTES 0x1A
#define FP_SEQUENCE_BYTES 12
#define FP_PARAMS_BYTES 0x3A // the parameters of an instrument: the fields and four modulation records
#define FP_SAMPLE_BYTES 0x12
#define FP_MAX_HUNK_BYTES 0x1000000
#define FP_END_OF_ARPEGGIO 0x80
#define FP_FADE_FULL 0xFF

// Where the fields of the module's records lie. A sequence or modulation record is 12 bytes or more; an instrument
// points at a sample set and at its parameters; a sample set holds a flag and the sample record or the sequence of
// them; an arpeggio is a table of steps.
enum {
    FpOffset_RecordData = 8,    // sequence and modulation records: address of the data
    FpOffset_RecordLength = 12, // modulation records: entries in the data
    FpOffset_InstrumentSet = 8,
    FpOffset_InstrumentParams = 12,
    FpOffset_SetIsSequence = 8,
    FpOffset_SetTarget = 10,
    FpOffset_SampleAddress = 8,
    FpOffset_SampleBytes = 12,
    FpOffset_SampleFlags = 0x10,
    FpOffset_SampleNoteOffset = 0x11,
    FpOffset_ArpeggioSteps = 8,
};

// The tables of the replayer that the module's pointers can reach, as they lie after the hunk.
enum {
    FpStatic_Arpeggio = 0,    // the arpeggio of one step, 0, then its end
    FpStatic_Silence = 12,    // 4 bytes
    FpStatic_Instrument = 16, // the instrument that voices start with
    FpStatic_Params = 32,     // its parameters: 58 bytes of zeros
    FpStatic_SampleSet = 96,  // its sample set
    FpStatic_Sample = 112,    // its sample: the silent buffer
    FpStatic_Size = 132,
};

// The commands of a track: the byte (0x80..0xBF) and 0x3F select one.
enum {
    FpCommand_End = 0,
    FpCommand_Instrument = 1,
    FpCommand_Arpeggio = 2,
    FpCommand_ArpeggioDefault = 3,
    FpCommand_PortamentoSpeed = 4,
    FpCommand_Nop = 5,
    FpCommand_Call = 6,
    FpCommand_Goto = 7,
    FpCommand_LoopStart = 8,
    FpCommand_LoopEnd = 9,
    FpCommand_LoopForever = 10,
    FpCommand_Transpose1 = 11,
    FpCommand_Transpose2 = 12,
    FpCommand_SongEnd = 13,
    FpCommand_Nop2 = 14,
    FpCommand_Count = 15,
};

static void fp_put16(uint8_t* at, uint16_t value) {
    at[0] = (uint8_t)(value >> 8);
    at[1] = (uint8_t)value;
}

static void fp_put32(uint8_t* at, uint32_t value) {
    fp_put16(at, (uint16_t)(value >> 16));
    fp_put16(at + 2, (uint16_t)value);
}

static const uint16_t fp_periods[] = {
    0x1C40, 0x1AC0, 0x1940, 0x17D0, 0x1680, 0x1530, 0x1400, 0x12E0, 0x11D0, 0x10D0, 0x0FE0, 0x0F00, 0x0E20, 0x0D60,
    0x0CA0, 0x0BE8, 0x0B40, 0x0A98, 0x0A00, 0x0970, 0x08E8, 0x0868, 0x07F0, 0x0780, 0x0710, 0x06B0, 0x0650, 0x05F4,
    0x05A0, 0x054C, 0x0500, 0x04B8, 0x0474, 0x0434, 0x03F8, 0x03C0, 0x0388, 0x0358, 0x0328, 0x02FA, 0x02D0, 0x02A6,
    0x0280, 0x025C, 0x023A, 0x021A, 0x01FC, 0x01E0, 0x01C4, 0x01AC, 0x0194, 0x017D, 0x0168, 0x0153, 0x0140, 0x012E,
    0x011D, 0x010D, 0x00FE, 0x00F0, 0x00E2, 0x00D6, 0x00CA, 0x00BE, 0x00B4, 0x00AA, 0x00A0, 0x0097, 0x008F, 0x0087,
    0x007F, 0x0078, 0x0071, 0x006B, 0x0065, 0x005F, 0x005A, 0x0055, 0x0050, 0x004C, 0x0048, 0x0044, 0x0040, 0x003C,
    0x0039, 0x0035, 0x0032, 0x002F, 0x002D, 0x002A, 0x0028, 0x0026, 0x0024, 0x0022, 0x0020, 0x001E, 0x001C,
};

// Module access: an address is FP_BASE_ADDRESS plus an offset in the image; an address outside it reads 0.

static uint8_t fp_u8(const FpModule* m, uint32_t address) {
    const uint32_t offset = address - FP_BASE_ADDRESS;
    return offset < m->image_size ? m->data[offset] : 0;
}

static uint16_t fp_u16(const FpModule* m, uint32_t address) {
    return (uint16_t)((fp_u8(m, address) << 8) | fp_u8(m, address + 1));
}

static uint32_t fp_u32(const FpModule* m, uint32_t address) {
    return ((uint32_t)fp_u16(m, address) << 16) | fp_u16(m, address + 2);
}

// The sample memory at an address; null when it lies outside the image.
static const int8_t* fp_chip(const FpModule* m, uint32_t address) {
    const uint32_t offset = address - FP_BASE_ADDRESS;
    return offset < m->image_size ? (const int8_t*)m->data + offset : nullptr;
}

// The records of the module

typedef struct FpSong {
    uint32_t header;
    uint16_t prerun_ticks; // sequencer calls to run ahead at the start
    uint8_t fade_speed;    // 0: full volume at once
} FpSong;

typedef struct FpSongHeader {
    uint32_t track[FP_VOICES]; // sequence records, 0 for a voice without a track
    uint8_t tempo;             // ticks per step of a track
    uint8_t timer_high;        // high byte of the CIA timer value of one tick
} FpSongHeader;

static FpSong fp_read_song(const FpModule* m, uint32_t number) {
    const uint32_t at = m->song_table + number * FP_SONG_BYTES;
    const uint32_t second = fp_u32(m, at + 4);
    return (
        FpSong) { .header = fp_u32(m, at), .prerun_ticks = (uint16_t)second, .fade_speed = (uint8_t)(second >> 16) };
}

static FpSongHeader fp_read_song_header(const FpModule* m, uint32_t at) {
    FpSongHeader header = { 0 };
    for (int i = 0; i < FP_VOICES; i++) {
        header.track[i] = fp_u32(m, at + 8 + 4u * (uint32_t)i);
    }
    header.tempo = fp_u8(m, at + 0x18) & 7;
    if (header.tempo == 0) {
        header.tempo = 8;
    }
    header.timer_high = fp_u8(m, at + 0x19);
    return header;
}

// A sequence or modulation record: its data is at the address in +8.
static uint32_t fp_record_data(const FpModule* m, uint32_t sequence) {
    return fp_u32(m, sequence + FpOffset_RecordData);
}

static FpParams fp_read_params(const FpModule* m, uint32_t at) {
    FpParams p = {
        .volume = fp_u8(m, at + 0x08),
        .attack_rate = fp_u8(m, at + 0x12),
        .attack_level = fp_u8(m, at + 0x13),
        .decay_rate = fp_u8(m, at + 0x14),
        .decay_level = fp_u8(m, at + 0x15),
        .sustain_rate = (int8_t)fp_u8(m, at + 0x16),
        .sustain_level = fp_u8(m, at + 0x17),
        .release_rate = fp_u8(m, at + 0x18),
        .retrigger = fp_u8(m, at + 0x19) != 0,
    };
    for (uint32_t i = 0; i < FP_VOICES; i++) {
        p.detune[i] = (int8_t)fp_u8(m, at + 0x0E + i);
    }
    for (uint32_t i = 0; i < FpModulation_Count; i++) {
        const uint32_t record = at + 0x1A + 8 * i;
        p.modulation[i] = (FpModulation) { .table = fp_u32(m, record),
                                           .shift = fp_u8(m, record + 4),
                                           .delay = fp_u8(m, record + 5),
                                           .flags = fp_u8(m, record + 6),
                                           .negate = fp_u8(m, record + 7) != 0 };
    }
    return p;
}

static FpSample fp_read_sample(const FpModule* m, uint32_t at) {
    const uint8_t flags = fp_u8(m, at + FpOffset_SampleFlags);
    return (FpSample) { .address = fp_u32(m, at + FpOffset_SampleAddress),
                        .bytes = fp_u16(m, at + FpOffset_SampleBytes),
                        .dynamic = (flags & 1) != 0,
                        .one_shot = (flags & 2) != 0,
                        .note_offset = fp_u8(m, at + FpOffset_SampleNoteOffset) };
}

// The instrument record and its sample set: the set's target is the sample or, when its flag is set, a sequence of
// samples.
static uint32_t fp_instrument_set(const FpModule* m, uint32_t instrument) {
    return fp_u32(m, instrument + FpOffset_InstrumentSet);
}

static uint32_t fp_instrument_params(const FpModule* m, uint32_t instrument) {
    return fp_u32(m, instrument + FpOffset_InstrumentParams);
}

static bool fp_set_is_sequence(const FpModule* m, uint32_t set) {
    return fp_u8(m, set + FpOffset_SetIsSequence) != 0;
}

static uint32_t fp_set_target(const FpModule* m, uint32_t set) {
    return fp_u32(m, set + FpOffset_SetTarget);
}

// The period of table entry `index` (only the first entries are the table; the rest of the 7 bits read other data).
static uint16_t fp_period(uint8_t index) {
    index &= 0x7F;
    return index < nuade_sizeof_array(fp_periods) ? fp_periods[index] : 0;
}

// A 16 bit left shift by a register count: counts of 16 and more leave nothing.
static uint16_t fp_shift_left(uint16_t value, uint8_t count) {
    count &= 63;
    return count >= 16 ? 0 : (uint16_t)(value << count);
}

// The module

static bool fp_in_hunk(const FpModule* m, uint32_t address, uint32_t bytes) {
    const uint32_t offset = address - FP_BASE_ADDRESS;
    return address >= FP_BASE_ADDRESS && offset <= m->hunk_size && m->hunk_size - offset >= bytes;
}

// The relocation blocks that follow the loaded data of the file, then the end of the hunk: each long they name gets the
// load address added.
static bool fp_relocate(const uint8_t* file, size_t size, size_t at, uint32_t memory, uint8_t* image) {
    while (at + 4 <= size) {
        const uint32_t type = nuade_read_be32(file + at) & 0x3FFFFFFF;
        at += 4;
        if (type == FP_HUNK_END) {
            return true;
        }
        if (type != FP_HUNK_RELOC32) {
            return false;
        }
        for (;;) {
            if (at + 4 > size) {
                return false;
            }
            const uint32_t count = nuade_read_be32(file + at);
            at += 4;
            if (count == 0) {
                break;
            }
            if (at + 4 > size || nuade_read_be32(file + at) != 0 || count > (size - at - 4) / 4) {
                return false;
            }
            at += 4;
            for (uint32_t i = 0; i < count; i++, at += 4) {
                const uint32_t offset = nuade_read_be32(file + at);
                if (offset > memory - 4) {
                    return false;
                }
                fp_put32(image + offset, nuade_read_be32(image + offset) + FP_BASE_ADDRESS);
            }
        }
    }
    return false;
}

// The module's hunk, relocated. `file` stays as it is; the image is a new buffer.
static bool fp_hunk_parse(const uint8_t* file, size_t size, uint8_t** image, FpModule* m) {
    if (size < FP_HEADER_BYTES + FP_CODE_BYTES || size > FP_MAX_HUNK_BYTES || nuade_read_be32(file) != FP_HUNK_HEADER
        || nuade_read_be32(file + 4) != 0 || nuade_read_be32(file + 8) != 1 || nuade_read_be32(file + 12) != 0
        || nuade_read_be32(file + 16) != 0) {
        return false;
    }
    // The size word's top bits ask for chip memory, which the eagleplayer's check wants.
    if (file[20] == 0 || nuade_read_be32(file + 24) != FP_HUNK_CODE) {
        return false;
    }
    const uint32_t memory = (nuade_read_be32(file + 20) & 0x3FFFFFFF) * 4;
    const uint32_t loaded = nuade_read_be32(file + 28) * 4;
    if (loaded > size - FP_HEADER_BYTES || loaded < FP_CODE_BYTES || loaded > memory || memory > FP_MAX_HUNK_BYTES
        || memory < FP_SONG_TABLE + FP_SONG_BYTES) {
        return false;
    }
    const uint8_t* code = file + FP_HEADER_BYTES;
    if (nuade_read_be32(code) != 0x70FF4E75 || memcmp(code + 4, "F.PLAYER", 8) != 0) {
        return false;
    }
    const uint32_t statics = (memory + 3) & ~3u;
    uint8_t* buffer = calloc(1, statics + FpStatic_Size);
    if (buffer == nullptr) {
        return false;
    }
    memcpy(buffer, code, loaded);
    if (!fp_relocate(file, size, FP_HEADER_BYTES + loaded, memory, buffer)) {
        free(buffer);
        return false;
    }
    *m = (FpModule) { .hunk_size = memory,
                      .image_size = statics + FpStatic_Size,
                      .song_table = FP_BASE_ADDRESS + FP_SONG_TABLE,
                      .title = nuade_read_be32(buffer + 12),
                      .author = nuade_read_be32(buffer + 16),
                      .info = nuade_read_be32(buffer + 20),
                      .arpeggio_default = FP_BASE_ADDRESS + statics + FpStatic_Arpeggio,
                      .silence = FP_BASE_ADDRESS + statics + FpStatic_Silence,
                      .instrument_default = FP_BASE_ADDRESS + statics + FpStatic_Instrument,
                      .sample_default = FP_BASE_ADDRESS + statics + FpStatic_Sample };
    *image = buffer;
    return true;
}

// The tables the replayer keeps in its own memory: the default arpeggio, the silent buffer and the instrument that
// voices start with.
static void fp_statics_init(FpModule* m, uint8_t* image) {
    const uint32_t base = m->image_size - FpStatic_Size;
    uint8_t* statics = image + base;
    const uint32_t address = FP_BASE_ADDRESS + base;
    uint8_t* arpeggio = statics + FpStatic_Arpeggio + FpOffset_ArpeggioSteps;
    arpeggio[0] = 0;
    arpeggio[1] = FP_END_OF_ARPEGGIO;
    arpeggio[2] = FP_END_OF_ARPEGGIO;
    fp_put32(statics + FpStatic_Instrument + FpOffset_InstrumentSet, address + FpStatic_SampleSet);
    fp_put32(statics + FpStatic_Instrument + FpOffset_InstrumentParams, address + FpStatic_Params);
    fp_put32(statics + FpStatic_SampleSet + FpOffset_SetTarget, address + FpStatic_Sample);
    fp_put32(statics + FpStatic_Sample + FpOffset_SampleAddress, address + FpStatic_Silence);
    fp_put16(statics + FpStatic_Sample + FpOffset_SampleBytes, 2);
}

// What the probe walks: the byte streams of the tracks (and the sub-sequences they call or jump to) and the
// instruments they select. A stream position is decoded once.
typedef struct FpWalk {
    const FpModule* module;
    uint8_t* seen;     // a bit per byte of the hunk: a position where a stream entry was decoded
    uint32_t* pending; // streams to walk
    size_t pending_count;
    size_t pending_capacity;
} FpWalk;

static bool fp_walk_push(FpWalk* w, uint32_t address) {
    if (w->pending_count == w->pending_capacity) {
        const size_t capacity = w->pending_capacity != 0 ? w->pending_capacity * 2 : 64;
        uint32_t* grown = realloc(w->pending, capacity * sizeof(*grown));
        if (grown == nullptr) {
            return false;
        }
        w->pending = grown;
        w->pending_capacity = capacity;
    }
    w->pending[w->pending_count++] = address;
    return true;
}

// An instrument record, its sample set, its parameters and (for a single sample) its sample record lie in the hunk.
static bool fp_instrument_valid(const FpModule* m, uint32_t instrument) {
    if (!fp_in_hunk(m, instrument, FpOffset_InstrumentParams + 4)) {
        return false;
    }
    const uint32_t set = fp_instrument_set(m, instrument);
    if (!fp_in_hunk(m, set, FpOffset_SetTarget + 4)
        || !fp_in_hunk(m, fp_instrument_params(m, instrument), FP_PARAMS_BYTES)) {
        return false;
    }
    return fp_set_is_sequence(m, set) || fp_in_hunk(m, fp_set_target(m, set), FP_SAMPLE_BYTES);
}

// A sequence record and the start of its data lie in the hunk; the data is walked later.
static bool fp_walk_sequence(FpWalk* w, uint32_t record) {
    const FpModule* m = w->module;
    return fp_in_hunk(m, record, FP_SEQUENCE_BYTES) && fp_in_hunk(m, fp_record_data(m, record), 2)
           && fp_walk_push(w, fp_record_data(m, record));
}

// The entries of one stream, as fp_sequence_run decodes them, up to where the track ends or leaves for another
// stream. Every entry (two bytes, then the operand of a command) must lie in the hunk.
static bool fp_walk_stream(FpWalk* w, uint32_t pos) {
    const FpModule* m = w->module;
    for (;;) {
        if (!fp_in_hunk(m, pos, 2)) {
            return false;
        }
        const uint32_t offset = pos - FP_BASE_ADDRESS;
        if ((w->seen[offset >> 3] >> (offset & 7) & 1) != 0) {
            return true;
        }
        w->seen[offset >> 3] |= (uint8_t)(1u << (offset & 7));
        const uint8_t first = m->data[offset];
        if (first < 0x80) {
            pos += 2; // a note and its duration
            continue;
        }
        const uint32_t operand = pos + 2;
        switch (first & 0x3F) {
            case FpCommand_End:
            case FpCommand_SongEnd:
            case FpCommand_LoopForever:
                return true;
            case FpCommand_Instrument:
                if (!fp_in_hunk(m, operand, 4) || !fp_instrument_valid(m, fp_u32(m, operand))) {
                    return false;
                }
                pos += 6;
                break;
            case FpCommand_Arpeggio:
                if (!fp_in_hunk(m, operand, 4) || !fp_in_hunk(m, fp_u32(m, operand) + FpOffset_ArpeggioSteps, 1)) {
                    return false;
                }
                pos += 6;
                break;
            case FpCommand_PortamentoSpeed:
                if (!fp_in_hunk(m, operand, 2)) {
                    return false;
                }
                pos += 4;
                break;
            case FpCommand_Call:
                if (!fp_in_hunk(m, operand, 4) || !fp_walk_sequence(w, fp_u32(m, operand))) {
                    return false;
                }
                pos += 6;
                break;
            case FpCommand_Goto:
                return fp_in_hunk(m, operand, 4) && fp_walk_sequence(w, fp_u32(m, operand));
            case FpCommand_ArpeggioDefault:
            case FpCommand_Nop:
            case FpCommand_LoopStart:
            case FpCommand_LoopEnd:
            case FpCommand_Transpose1:
            case FpCommand_Transpose2:
            case FpCommand_Nop2:
                pos += 2;
                break;
            default:
                return true; // a command the replayer cannot run: the song ends there
        }
    }
}

// A song fits when its header lies in the hunk and everything its tracks reach is valid.
static bool fp_song_valid(FpWalk* w, FpSong song) {
    const FpModule* m = w->module;
    if (!fp_in_hunk(m, song.header, FP_SONG_HEADER_BYTES + 2)) {
        return false;
    }
    const FpSongHeader header = fp_read_song_header(m, song.header);
    for (int i = 0; i < FP_VOICES; i++) {
        if (header.track[i] != 0 && !fp_walk_sequence(w, header.track[i])) {
            return false;
        }
    }
    while (w->pending_count != 0) {
        if (!fp_walk_stream(w, w->pending[--w->pending_count])) {
            return false;
        }
    }
    return true;
}

static bool fp_module_parse(const uint8_t* file, size_t size, uint8_t** image, FpModule* m) {
    if (!fp_hunk_parse(file, size, image, m)) {
        return false;
    }
    m->data = *image;
    FpWalk walk = { .module = m, .seen = calloc(m->hunk_size / 8 + 1, 1) };
    // The songs: the table ends at the first zero header address.
    uint32_t count = 0;
    bool valid = walk.seen != nullptr;
    while (valid && fp_u32(m, m->song_table + count * FP_SONG_BYTES) != 0 && count < 256) {
        valid = fp_song_valid(&walk, fp_read_song(m, count));
        count++;
    }
    free(walk.seen);
    free(walk.pending);
    m->subsongs = count;
    if (!valid || count == 0) {
        free(*image);
        return false;
    }
    fp_statics_init(m, *image);
    return true;
}

bool fp_probe(const uint8_t* data, size_t size) {
    FpModule module;
    uint8_t* image = nullptr;
    if (!fp_module_parse(data, size, &image, &module)) {
        return false;
    }
    free(image);
    return true;
}

// Hardware and fade

// The fade level scales every voice's volume.
static void fp_set_master(FpPlayer* p, uint8_t level) {
    p->fade_level = level;
    for (int i = 0; i < FP_VOICES; i++) {
        p->voices[i].master = level;
    }
}

// All DMA off, every channel on the silent buffer.
static void fp_hardware_reset(FpPlayer* p) {
    for (int i = 0; i < FP_VOICES; i++) {
        NuadePaulaChannel* c = &p->voices[i].paula;
        nuade_paula_channel_dma_off(c);
        c->lc = fp_chip(&p->module, p->module.silence);
        c->len_words = 2;
        c->period = 1;
        c->volume = 0;
    }
}

// The display number of an instrument: the order they were first played in.
static int fp_instrument_number(FpPlayer* p, uint32_t instrument) {
    for (uint32_t i = 0; i < p->instrument_count; i++) {
        if (p->instrument_address[i] == instrument) {
            return (int)i;
        }
    }
    if (p->instrument_count < FP_MAX_INSTRUMENTS) {
        p->instrument_address[p->instrument_count] = instrument;
        return (int)p->instrument_count++;
    }
    return FP_MAX_INSTRUMENTS - 1;
}

// The sequencer

// A voice reached the end of its track; when it was the last one, the replayer calls SongEnd.
static void fp_voice_end(FpPlayer* p, int index) {
    FpVoice* v = &p->voices[index];
    v->finished = true;
    if (v->stopped) {
        return;
    }
    v->stopped = true;
    nuade_paula_channel_dma_off(&v->paula);
    p->voice_running[index] = false;
    for (int i = 0; i < FP_VOICES; i++) {
        if (p->voice_running[i]) {
            return;
        }
    }
    p->looped = true;
    memcpy(p->voice_running, p->voice_present, sizeof(p->voice_running));
}

// Select an instrument: the detune of every voice comes from its parameters, and this voice's modulations restart.
static void fp_select_instrument(FpPlayer* p, FpVoice* v, uint32_t instrument) {
    v->instrument = instrument;
    const FpParams params = fp_read_params(&p->module, fp_instrument_params(&p->module, instrument));
    for (int i = 0; i < FP_VOICES; i++) {
        p->voices[i].detune = params.detune[i];
    }
    memset(v->modulation_pos, 0, sizeof(v->modulation_pos));
    memset(v->modulation_delay, 0, sizeof(v->modulation_delay));
    v->sequence_started = false;
}

typedef enum FpCommandResult {
    FpCommandResult_Continue,
    FpCommandResult_Return, // the sequencer call is over
} FpCommandResult;

// A track command; `pos` is the track position after its operand byte.
static FpCommandResult fp_command_run(FpPlayer* p, int index, uint8_t command, uint8_t operand, uint32_t* pos) {
    const FpModule* m = &p->module;
    FpVoice* v = &p->voices[index];
    switch (command) {
        case FpCommand_End:
            if (v->stack_level != 0) {
                v->stack_level--;
                *pos = v->return_pos[v->stack_level];
                return FpCommandResult_Continue;
            }
            fp_voice_end(p, index);
            return FpCommandResult_Return;
        case FpCommand_SongEnd:
            if (p->prerun) {
                return FpCommandResult_Continue;
            }
            fp_voice_end(p, index);
            return FpCommandResult_Return;
        case FpCommand_Instrument:
            fp_select_instrument(p, v, fp_u32(m, *pos));
            *pos += 4;
            break;
        case FpCommand_Arpeggio:
            v->arpeggio = fp_u32(m, *pos);
            *pos += 4;
            break;
        case FpCommand_ArpeggioDefault:
            v->arpeggio = m->arpeggio_default;
            v->arpeggio_pos = 0;
            break;
        case FpCommand_PortamentoSpeed:
            v->portamento_speed = fp_u16(m, *pos);
            *pos += 2;
            break;
        case FpCommand_Call:
            v->return_pos[v->stack_level] = *pos + 4;
            if (v->stack_level != FP_STACK_LEVELS - 1) {
                v->stack_level++;
            }
            v->loop_count[v->stack_level] = 1;
            *pos = fp_record_data(m, fp_u32(m, *pos));
            break;
        case FpCommand_Goto:
            *pos = fp_record_data(m, fp_u32(m, *pos));
            break;
        case FpCommand_LoopStart:
            v->loop_start[v->stack_level] = *pos;
            v->loop_count[v->stack_level] = operand;
            break;
        case FpCommand_LoopEnd:
            if (--v->loop_count[v->stack_level] != 0) {
                *pos = v->loop_start[v->stack_level];
            }
            break;
        case FpCommand_LoopForever:
            *pos = v->loop_start[v->stack_level];
            break;
        case FpCommand_Transpose1:
            v->transpose1 = operand;
            break;
        case FpCommand_Transpose2:
            v->transpose2 = operand;
            break;
        case FpCommand_Nop:
        case FpCommand_Nop2:
            break;
        default:
            // The jump table ends at 15 entries; the replayer would run whatever lies after it.
            p->failed = true;
            return FpCommandResult_Return;
    }
    return FpCommandResult_Continue;
}

// Read the track up to the next note or rest.
static void fp_sequence_run(FpPlayer* p, int index) {
    const FpModule* m = &p->module;
    FpVoice* v = &p->voices[index];
    if (v->finished) {
        return;
    }
    v->new_note = false;
    p->command_guard = 0;
    if (--v->duration_left != 0) {
        return;
    }
    uint32_t pos = v->track_pos;
    for (;;) {
        const int8_t first = (int8_t)fp_u8(m, pos++);
        if (first < 0) {
            if (--p->command_guard == 0) {
                // 256 commands in one call: the replayer gives up for this tick (and waits 255 for the next)
                return;
            }
            const uint8_t operand = fp_u8(m, pos++);
            if (fp_command_run(p, index, (uint8_t)first & 0x3F, operand, &pos) == FpCommandResult_Return) {
                return;
            }
            continue;
        }
        if (first != 0) {
            v->note = (uint8_t)first;
        }
        const int8_t duration = (int8_t)fp_u8(m, pos++);
        v->track_pos = pos;
        v->note_on = (uint8_t)first;
        if (duration >= 0) {
            v->duration_left = (uint8_t)duration;
            v->new_note = first != 0;
        } else {
            // The note continues the sound: no restart
            v->duration_left = (uint8_t)duration & 0x7F;
        }
        return;
    }
}

// The sound of a voice

// The sample record that plays: the instrument's own, or the current one of its sequence of samples.
static uint32_t fp_sample_select(FpVoice* v, const FpModule* m, uint32_t set) {
    if (!fp_set_is_sequence(m, set)) {
        return fp_set_target(m, set);
    }
    if (!v->sequence_started) {
        const uint32_t first = fp_set_target(m, set) + 8;
        if (fp_u32(m, first) == 0) {
            return m->sample_default;
        }
        v->sequence_loop = first;
        v->sequence_sample = fp_u32(m, first);
        v->sequence_ticks = fp_u8(m, first + 4);
        v->sequence_next = first + 6;
        v->sequence_started = true;
    }
    const uint32_t sample = v->sequence_sample;
    if (--v->sequence_ticks == 0) {
        uint32_t entry = v->sequence_next;
        if (fp_u32(m, entry) == 0) {
            entry = v->sequence_loop;
        }
        v->sequence_sample = fp_u32(m, entry);
        v->sequence_ticks = fp_u8(m, entry + 4);
        if (fp_u8(m, entry + 5) != 0) {
            v->sequence_loop = entry;
        }
        v->sequence_next = entry + 6;
    }
    return sample;
}

// The period of the note: the arpeggio step added to the note, or the slide towards it.
static uint16_t fp_pitch(FpVoice* v, const FpModule* m, uint8_t note) {
    if (v->portamento_speed == 0) {
        const uint32_t table = v->arpeggio + FpOffset_ArpeggioSteps;
        uint16_t pos = v->arpeggio_pos;
        uint8_t step = fp_u8(m, table + (uint32_t)(int32_t)(int16_t)pos);
        if (step == FP_END_OF_ARPEGGIO) {
            step = fp_u8(m, table);
            pos = 0xFFFF;
        }
        v->arpeggio_pos = (uint16_t)(pos + 1);
        return fp_period((uint8_t)(note + step));
    }
    const uint16_t target = fp_period(note);
    const uint16_t current = v->base_period;
    const uint16_t speed = v->portamento_speed;
    if (target >= current) {
        const uint32_t sum = (uint32_t)current + speed;
        return (sum > 0xFFFF || sum >= target) ? target : (uint16_t)sum;
    }
    return (current < speed || current - speed <= target) ? target : (uint16_t)(current - speed);
}

// Step to the next entry of the table; at its end the table starts over, or stops (flag bit 7).
static void fp_modulation_step(FpVoice* v, const FpModule* m, FpModulationKind kind, const FpModulation* mod,
                               uint16_t pos) {
    pos++;
    if (pos >= fp_u16(m, mod->table + FpOffset_RecordLength)) {
        pos = (mod->flags & 0x80) != 0 ? 0xFFFF : 0;
    }
    v->modulation_pos[kind] = pos;
}

// The modulation of the period: a table whose entries (shifted, maybe negated) are added to it.
static uint16_t fp_period_modulate(FpVoice* v, const FpModule* m, const FpParams* params, FpModulationKind kind,
                                   uint16_t period) {
    const FpModulation* mod = &params->modulation[kind];
    if (mod->table == 0) {
        return period;
    }
    if (v->modulation_delay[kind] != 0) {
        v->modulation_delay[kind]--;
        return period;
    }
    uint16_t pos = v->modulation_pos[kind];
    if ((int16_t)pos < 0) {
        return period;
    }
    int16_t entry = (int8_t)fp_u8(m, fp_record_data(m, mod->table) + pos);
    if (mod->negate) {
        entry = (int16_t)-entry;
    }
    period = (uint16_t)(period + fp_shift_left((uint16_t)entry, mod->shift));
    fp_modulation_step(v, m, kind, mod, pos);
    return period;
}

// The modulation of the sample address of a dynamic sample.
static uint32_t fp_address_modulate(FpVoice* v, const FpModule* m, const FpParams* params, FpModulationKind kind,
                                    uint32_t address) {
    const FpModulation* mod = &params->modulation[kind];
    if (mod->table == 0) {
        return address;
    }
    if (v->modulation_delay[kind] != 0) {
        if (kind == FpModulation_Address1) {
            v->modulation_delay[kind]--;
            return address;
        }
        // The second table loops back to its start while the delay counts down: the delay passes at once.
        v->modulation_delay[kind] = 0;
    }
    uint16_t pos = v->modulation_pos[kind];
    if ((int16_t)pos < 0) {
        return address;
    }
    const uint16_t entry = fp_u8(m, fp_record_data(m, mod->table) + pos);
    address += fp_shift_left(entry, (uint8_t)(mod->shift + 1));
    fp_modulation_step(v, m, kind, mod, pos);
    return address;
}

// The sound restarts: DMA stops, the envelope starts over and the modulations that do not carry on restart.
static void fp_note_restart(FpVoice* v, const FpParams* params) {
    nuade_paula_channel_dma_off(&v->paula);
    v->arpeggio_pos = 0;
    v->sequence_started = false;
    v->phase = FpEnvelope_Attack;
    if (params->retrigger || v->level <= params->attack_rate) {
        v->level = params->attack_rate;
    }
    for (int i = 0; i < FpModulation_Count; i++) {
        if (params->modulation[i].flags != 1) {
            v->modulation_pos[i] = 0;
            v->modulation_delay[i] = params->modulation[i].delay;
        }
    }
}

static uint8_t fp_volume(const FpVoice* v, const FpParams* params) {
    const uint32_t scaled = (uint32_t)v->level * params->volume;
    return (uint8_t)((((scaled * v->master) >> 16) & 0xFF) >> 2);
}

static void fp_envelope_decay(FpVoice* v, const FpParams* params) {
    if (v->level < params->decay_rate || (uint8_t)(v->level - params->decay_rate) <= params->decay_level) {
        v->level = params->decay_level;
        v->phase = FpEnvelope_Sustain;
    } else {
        v->level = (uint8_t)(v->level - params->decay_rate);
    }
}

// Move the level one step through the envelope: attack to its peak, decay, sustain, then hold; a released note falls.
static void fp_envelope_step(FpVoice* v, const FpParams* params) {
    if (v->note_on == 0) {
        if (v->level < params->release_rate) {
            v->level = 0;
        } else {
            v->level = (uint8_t)(v->level - params->release_rate);
        }
        return;
    }
    switch (v->phase) {
        case FpEnvelope_Attack:
            if (params->attack_rate == params->attack_level) {
                // The attack is already at its peak: the decay runs without leaving the phase
                fp_envelope_decay(v, params);
                return;
            }
            const uint32_t sum = (uint32_t)v->level + params->attack_rate;
            if (sum <= 0xFF && (uint8_t)sum < params->attack_level) {
                v->level = (uint8_t)sum;
            } else {
                v->level = params->attack_level;
                v->phase = FpEnvelope_Decay;
            }
            return;
        case FpEnvelope_Decay:
            fp_envelope_decay(v, params);
            return;
        case FpEnvelope_Sustain:
            if (params->sustain_rate >= 0) {
                const uint32_t sum = (uint32_t)v->level + (uint8_t)params->sustain_rate;
                if (sum <= 0xFF && (uint8_t)sum < params->sustain_level) {
                    v->level = (uint8_t)sum;
                    return;
                }
            } else {
                const uint8_t rate = (uint8_t)params->sustain_rate & 0x7F;
                if (v->level >= rate && (uint8_t)(v->level - rate) > params->sustain_level) {
                    v->level = (uint8_t)(v->level - rate);
                    return;
                }
            }
            v->level = params->sustain_level;
            v->phase = FpEnvelope_Hold;
            return;
        case FpEnvelope_Hold:
            v->level = params->sustain_level;
            return;
    }
}

// Run the sound of one voice for a tick.
static void fp_sound_run(FpPlayer* p, int index) {
    const FpModule* m = &p->module;
    FpVoice* v = &p->voices[index];
    if (v->stopped) {
        return;
    }
    const FpParams params = fp_read_params(m, fp_instrument_params(m, v->instrument));
    if (v->new_note) {
        fp_note_restart(v, &params);
    }
    const uint32_t set = fp_instrument_set(m, v->instrument);
    const uint32_t sample_at = fp_sample_select(v, m, set);
    const FpSample sample = fp_read_sample(m, sample_at);

    // Period
    const uint8_t note = (uint8_t)(sample.note_offset + v->note + v->transpose1 + v->transpose2);
    uint16_t period = fp_pitch(v, m, note);
    v->base_period = period;
    period = fp_period_modulate(v, m, &params, FpModulation_Pitch1, period);
    period = fp_period_modulate(v, m, &params, FpModulation_Pitch2, period);
    v->paula.period = (uint16_t)(period + v->detune);
    if (v->new_note) {
        v->shown_note = nuade_period_to_note(v->base_period);
        v->shown_instrument = fp_instrument_number(p, v->instrument);
    }

    // Sample: written at a new note and, for a dynamic sample or a sequence of samples, every tick
    if (v->new_note || sample.dynamic || fp_set_is_sequence(m, set)) {
        uint32_t address = sample.address;
        uint16_t words = sample.bytes >> 1;
        if (sample.dynamic) {
            words >>= 1;
            address = fp_address_modulate(v, m, &params, FpModulation_Address1, address);
            address = fp_address_modulate(v, m, &params, FpModulation_Address2, address);
        }
        v->paula.len_words = words;
        v->paula.lc = fp_chip(m, address);
    }

    v->paula.volume = fp_volume(v, &params);
    fp_envelope_step(v, &params);

    if (v->new_note) {
        nuade_paula_channel_dma_on(&v->paula);
        v->new_note = false;
        return;
    }
    if (!fp_set_is_sequence(m, set) && sample.one_shot) {
        v->paula.len_words = 2;
        v->paula.lc = fp_chip(m, m->silence);
    }
}

// The song

// Set up the voices and the song's header data (the replayer's InitSound).
static void fp_song_init(FpPlayer* p, FpSong song) {
    const FpModule* m = &p->module;
    const FpSongHeader header = fp_read_song_header(m, song.header);
    memset(p->voice_running, 0, sizeof(p->voice_running));
    for (int i = 0; i < FP_VOICES; i++) {
        p->voice_present[i] = header.track[i] != 0;
    }
    memcpy(p->voice_running, p->voice_present, sizeof(p->voice_running));

    fp_hardware_reset(p);
    for (int i = 0; i < FP_VOICES; i++) {
        FpVoice* v = &p->voices[i];
        v->stopped = true;
        v->finished = true;
        v->instrument = m->instrument_default;
        v->arpeggio = m->arpeggio_default;
        v->duration_left = 1;
        v->note_on = 0;
        v->master = 0;
        v->shown_note = -1;
        v->shown_instrument = -1;
    }
    p->tempo = header.tempo;
    p->timer = (uint16_t)(header.timer_high << 8);
    for (int i = 0; i < FP_VOICES; i++) {
        if (header.track[i] != 0) {
            FpVoice* v = &p->voices[i];
            v->track_pos = fp_record_data(m, header.track[i]);
            v->return_pos[0] = v->track_pos;
            v->stack_level = 0;
            v->stopped = false;
            v->finished = false;
        }
    }
}

static void fp_sequence_all(FpPlayer* p) {
    for (int i = 0; i < FP_VOICES; i++) {
        fp_sequence_run(p, i);
    }
}

static void fp_sync(FpPlayer* p, NuadeVoices* voices) {
    for (int i = 0; i < FP_VOICES; i++) {
        nuade_paula_channel_sync(&p->voices[i].paula, (uint32_t)i, voices);
    }
}

// Mutable player state: everything but the module and the instrument numbering.
static void fp_state_clear(FpPlayer* p) {
    const FpModule module = p->module;
    const uint32_t instrument_count = p->instrument_count;
    uint32_t instrument_address[FP_MAX_INSTRUMENTS];
    memcpy(instrument_address, p->instrument_address, sizeof(instrument_address));
    memset(p, 0, sizeof(*p));
    p->module = module;
    p->instrument_count = instrument_count;
    memcpy(p->instrument_address, instrument_address, sizeof(instrument_address));
}

FpCreateResult fp_create(const uint8_t* data, size_t size) {
    FpCreateResult result = { .status = FpStatus_InvalidData };
    FpModule module;
    uint8_t* image = nullptr;
    if (!fp_module_parse(data, size, &image, &module)) {
        return result;
    }
    FpPlayer* player = calloc(1, sizeof(FpPlayer));
    uint8_t* padded = nuade_paula_padded_copy(image, module.image_size);
    free(image);
    if (player == nullptr || padded == nullptr) {
        free(player);
        free(padded);
        result.status = FpStatus_OutOfMemory;
        return result;
    }
    player->module = module;
    player->module.data = padded;
    result.player = player;
    result.status = FpStatus_Ok;
    return result;
}

void fp_destroy(FpPlayer* player) {
    if (player == nullptr) {
        return;
    }
    free(player->module.data);
    free(player);
}

int fp_subsong_count(const FpPlayer* player) {
    return (int)player->module.subsongs;
}

bool fp_start(FpPlayer* player, int subsong, NuadeVoices* voices) {
    if (subsong < 0 || (uint32_t)subsong >= player->module.subsongs) {
        return false;
    }
    fp_state_clear(player);
    const FpSong song = fp_read_song(&player->module, (uint32_t)subsong);
    player->tempo_count = 1;
    fp_set_master(player, 0);
    // The song is set up with the fade level at 0, then at full volume unless the song fades in.
    player->fade_speed = song.fade_speed;
    if (song.fade_speed == 0) {
        fp_set_master(player, FP_FADE_FULL);
    }
    player->fading_in = song.fade_speed != 0;
    fp_song_init(player, song);
    player->prerun = true;
    for (uint32_t i = 0; i < song.prerun_ticks; i++) {
        fp_sequence_all(player);
    }
    player->prerun = false;
    if (!player->fading_in) {
        fp_set_master(player, FP_FADE_FULL);
    }
    player->looped = false;
    fp_sync(player, voices);
    return true;
}

FpTickResult fp_tick(FpPlayer* player, NuadeVoices* voices) {
    player->looped = false;
    if (player->fading_in) {
        // Fading in: the level rises by the speed each tick up to full
        const uint32_t level = (uint32_t)player->fade_level + player->fade_speed;
        if (player->fade_speed == 0 || level > 0xFF) {
            fp_set_master(player, FP_FADE_FULL);
            player->fading_in = false;
        } else {
            fp_set_master(player, (uint8_t)level);
        }
    }
    if (--player->tempo_count == 0) {
        player->tempo_count = player->tempo;
        fp_sequence_all(player);
    }
    for (int i = 0; i < FP_VOICES; i++) {
        fp_sound_run(player, i);
    }
    fp_sync(player, voices);
    // A CIA timer loaded with N counts N + 1 cycles; a timer of 0 counts the full 65536.
    const uint32_t length = (player->timer != 0 ? player->timer : 0x10000u) + 1;
    return (FpTickResult) { .looped = player->looped, .ended = player->failed, .length = length };
}

uint32_t fp_grid_ticks(const FpPlayer* player, int subsong) {
    if (subsong < 0 || (uint32_t)subsong >= player->module.subsongs) {
        return 0;
    }
    return fp_read_song_header(&player->module, fp_read_song(&player->module, (uint32_t)subsong).header).tempo;
}

FpVoiceState fp_voice_state(const FpPlayer* player, int voice) {
    const FpVoice* v = &player->voices[voice];
    return (FpVoiceState) { .note = v->shown_note, .instrument = v->shown_instrument };
}

void fp_module_string(const FpPlayer* player, uint32_t address, char* out, size_t cap) {
    size_t length = 0;
    while (address != 0 && length + 1 < cap) {
        const uint8_t c = fp_u8(&player->module, address++);
        if (c == 0) {
            break;
        }
        out[length++] = (char)c;
    }
    out[length] = 0;
}
