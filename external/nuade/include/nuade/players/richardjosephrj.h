// SPDX-License-Identifier: MIT
// Richard Joseph game driver (the music and sound driver of Cadaver, The Lure of the Temptress, Moonstone and other
// games programmed or composed by Richard Joseph), translated from the Wanted Team eagleplayer "Richard Joseph_v2.asm"
// (UADE 3.05, adapted by Wanted Team; the replayer is the game's own driver, which the converted modules carry) and
// from the driver code inside the converted modules.
//
// A module is the Wanted Team conversion of one game's driver: an AmigaDOS hunk executable (one chip code hunk) that
// starts `moveq #-1,d0; rts`, "R.JOSEPH", and the entry points of the eagleplayer, followed by the game's driver code,
// its tables and the sequences. The driver is identified by its code.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <nuade/nuade.h>
#include <nuade/paula.h>
#include <nuade/voice.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RJG_VOICES 4

// The CIA clock: the timebase of RJG_TICK_LENGTH.
#define RJG_TIMEBASE_HZ 709379
// DeliTracker's default 50 Hz tick, in 1 / RJG_TIMEBASE_HZ seconds.
#define RJG_TICK_LENGTH 14187

typedef struct RjgPlayer RjgPlayer;

typedef enum RjgStatus {
    RjgStatus_Ok = 0,
    RjgStatus_InvalidData,
    RjgStatus_OutOfMemory,
} RjgStatus;

typedef struct RjgCreateResult {
    RjgPlayer* player; // null on failure
    RjgStatus status;
} RjgCreateResult;

typedef struct RjgTickResult {
    bool looped; // every voice that plays a sequence has reached its loop or end since the last time this was set
    bool ended;  // every voice has stopped
} RjgTickResult;

// What a voice last started: the note byte of its sequence (-1 if none yet) and the instrument row it plays (-1 if
// none).
typedef struct RjgVoiceState {
    int note;
    int instrument;
} RjgVoiceState;

// What the subsong start of a module calls.
typedef enum RjgStart {
    RjgStart_Tune,     // a tune: four sequence numbers, one per voice; a 0 starts the null sequence and ends the row
    RjgStart_TuneFour, // a tune: four sequence numbers, all four voices start
    RjgStart_Sequence, // one sequence on voice 0
} RjgStart;

// A module in memory: the hunk body with the relocated longs as they are stored (offsets into the body), followed by
// the zeros a Paula channel may read behind it. Every read past `size` gives 0.
typedef struct RjgModule {
    uint8_t* data;
    uint32_t size;
    RjgStart start_kind;

    // The tables the driver's code points at (offsets in the body)
    uint32_t dummy;            // the empty word a sequence start and the audio interrupt point a channel at
    uint32_t sequences;        // longs: the offset of each sequence
    uint32_t modulation;       // 15-byte rows of volume and pitch modulation
    uint32_t envelopes;        // 8-byte volume envelope rows
    uint32_t instruments;      // 14-byte instrument rows
    uint32_t tunes;            // 4 sequence numbers per tune (RjgStart_Tune*), else 0
    uint16_t tempo_initial;    // the tempo word as the module has it
    uint32_t tempo_scale;      // the tempo command divides this by its operand
    uint32_t clear_longs;      // the loop count of the clear a sequence start does from record offset 0x10: longs - 1
    bool stop_dma_after_start; // a sequence start ends with the voice's DMA off

    // The subsong list of the conversion's InitSong
    uint32_t subsong_count;
    uint32_t subsong_map; // one byte per subsong: what the start gets as d0
} RjgModule;

// An instrument row of the module (14 bytes).
typedef struct RjgInstrument {
    int16_t loops;        // negative: the sample loops
    uint16_t loop_offset; // bytes
    uint16_t length_words;
    uint32_t sample;       // offset in the module
    uint32_t period_table; // offset in the module: the periods of the notes, four words per note
} RjgInstrument;

// Slots of volume modulation (a tremolo) and of pitch modulation (vibrato and slides) a voice runs
#define RJG_VOLUME_SLOTS 2
#define RJG_PITCH_SLOTS 3

typedef struct RjgVoice {
    NuadePaulaChannel paula;
    uint32_t index;

    // The channel's audio interrupt
    uint16_t interrupt_phase;
    bool interrupt_enabled;
    uint32_t wraps_seen;

    // Sequence
    uint32_t sequence;       // next byte; 0: the voice does not play
    uint32_t sequence_start; // where the sequence restarts at its loop command; 0 after an end command
    uint32_t stack;          // call stack pointer, an index into the player's stack memory
    uint16_t wait;           // ticks to the next sequence byte
    uint16_t duration;       // ticks of a note (the duration command's operand times the tempo)
    bool passed_end;         // reached the loop or end command since the last song end

    // Instrument
    RjgInstrument instrument;
    uint32_t envelope;    // offset of the 8-byte volume envelope row, 0 for none
    int transpose;        // in steps of a quarter note
    uint16_t base_period; // the period of the note
    uint16_t max_volume;
    uint16_t volume;
    uint16_t period;
    bool trigger; // a note started: the channel restarts in this tick's register write

    // Volume envelope
    uint8_t envelope_timer;
    bool envelope_active;
    uint16_t envelope_phase;

    // Modulation: slot counts and the values a slot reloads
    uint16_t volume_delay[RJG_VOLUME_SLOTS];
    uint16_t volume_count[RJG_VOLUME_SLOTS];
    uint16_t pitch_delay[RJG_PITCH_SLOTS];
    uint16_t pitch_count[RJG_PITCH_SLOTS];
    uint16_t volume_count_reload[RJG_VOLUME_SLOTS];
    uint16_t pitch_count_reload[RJG_PITCH_SLOTS];
    uint16_t volume_delta[RJG_VOLUME_SLOTS];
    uint16_t pitch_delta[RJG_PITCH_SLOTS];
    uint16_t volume_delay_reload[RJG_VOLUME_SLOTS];
    uint16_t pitch_delay_reload[RJG_PITCH_SLOTS];
    uint16_t volume_offset;
    uint16_t pitch_offset;
    bool volume_slots_reload; // the volume slots reload when idle
    bool pitch_slots_reload;  // the pitch slots reload when idle

    // Display
    int shown_note;
    int shown_instrument;
} RjgVoice;

// The sequence call stacks: four blocks next to each other, voice n's grows down from the top of block n
#define RJG_STACK_SPACING 0x80 // bytes of a block (the tops are this far apart)
#define RJG_STACK_BYTES (RJG_VOICES * RJG_STACK_SPACING)

struct RjgPlayer {
    RjgModule module;
    uint16_t tempo;
    uint8_t stack_memory[RJG_STACK_BYTES];
    RjgVoice voices[RJG_VOICES];
};

// True when the data is a Richard Joseph module of a known driver build, with a subsong list the player can run.
bool rjg_probe(const uint8_t* data, size_t size);
RjgCreateResult rjg_create(const uint8_t* data, size_t size);
void rjg_destroy(RjgPlayer* player);

int rjg_subsong_count(const RjgPlayer* player);
// (Re)start a subsong (0-based) from its beginning; the player drives `voices` (Paula channel n = voice n).
bool rjg_start(RjgPlayer* player, int subsong, NuadeVoices* voices);

// Run one player tick and hand the Paula registers it wrote to the voices.
RjgTickResult rjg_tick(RjgPlayer* player, NuadeVoices* voices);

RjgVoiceState rjg_voice_state(const RjgPlayer* player, int voice);

// Ticks of one duration unit in a subsong (0-based): the first tempo the sequences set, 1 if they set none; 0 for a
// bad subsong.
uint32_t rjg_grid_ticks(const RjgPlayer* player, int subsong);

// The player's state behind a song it opened; 0 when the song is another player's.
RjgPlayer* rjg_from_nuade(NuadeSong* song);

#ifdef __cplusplus
}
#endif
