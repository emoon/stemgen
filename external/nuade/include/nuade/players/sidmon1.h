// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Daniel Collin
// Ported from NostalgicPlayer C# implementation by Thomas Neumann (MIT)
//
// SidMon 1.0 player (Reinier van Vliet, 1989). The replayer is part of every module.

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

#define SD1_VOICES 4

// The note column counts semitones from C-0 less this many octaves: the period table reaches below C-0.
#define SD1_NOTE_OCTAVE_SHIFT (-2)

// The CIA clock: the timebase of SD1_TICK_LENGTH.
#define SD1_TIMEBASE_HZ 709379
// One player tick (the module's interrupt, run at DeliTracker's default 50 Hz), in 1 / SD1_TIMEBASE_HZ seconds.
#define SD1_TICK_LENGTH 14187

typedef struct Sd1Player Sd1Player;

typedef enum Sd1Status {
    Sd1Status_Ok = 0,
    Sd1Status_InvalidData,
    Sd1Status_OutOfMemory,
} Sd1Status;

// The stages of a note's volume envelope; the replayer steps through them in twos.
typedef enum Sd1Envelope {
    Sd1Envelope_Attack = 0,
    Sd1Envelope_Decay = 2,
    Sd1Envelope_Sustain = 4,
    Sd1Envelope_Release = 6,
    Sd1Envelope_Done = 8,
} Sd1Envelope;

// The oldest replayers play three fixed samples on the fourth channel (instruments 61 to 63), which are part of the
// replayer.
#define SD1_SPECIAL_SAMPLES 3

typedef struct Sd1SpecialSample {
    uint32_t start; // module offset of the data
    uint16_t length_words;
} Sd1SpecialSample;

// Where the module's replayer keeps its tables and which of its variants it is. All offsets are module offsets;
// 0 where the variant does not have the part.
typedef struct Sd1Layout {
    uint32_t sequences[SD1_VOICES]; // per voice: 6-byte entries (track number, unused, transpose)
    uint32_t track_data;            // track rows: 5 bytes each (note, instrument, effect, parameter, duration)
    uint32_t track_offsets;         // per track: its offset in track_data
    uint32_t instruments;           // 32 bytes each
    uint32_t wave_list;             // 16 bytes each: (waveform number, ticks) pairs, 0xFF = jump
    uint32_t waveforms;             // 32 signed bytes each
    uint32_t samples;               // sample table: base offset, then 32 bytes each
    uint32_t song;                  // song parameters (mix sources, rows, positions, speed)
    uint32_t periods;               // period table
    uint32_t finetune_stride;       // words between fine tunes of the period table
    bool finetune_is_word;          // the replayer scales the fine tune in a signed 16-bit register
    uint32_t no_loop;               // a sample's loop start equal to this means no loop
    Sd1SpecialSample special_samples[SD1_SPECIAL_SAMPLES];
    uint32_t special_flag; // variable: 1 when the song uses them
    // The replayer's variables that start with their file contents.
    uint32_t mix_counter[2];
    uint32_t mix_position[2];
    uint32_t filter_position;
    uint32_t counter; // the speed counter and the two position flags
    uint32_t new_position_flag;
    uint32_t loop_flag;
    bool old_replayer;           // the oldest replayer: waveforms straight from instruments, special samples
    bool init_starts_playing;    // the init routine loads the speed counter itself
    bool mixing;                 // the waveform mixer runs every tick
    bool filter;                 // the waveform filter runs every tick
    bool filter_shares_position; // ... on the position the first mixer keeps
    bool clears_row_count;       // a new position resets every voice's row counter
    int instrument_count;
    int track_count;
    int sample_count;
    int waveform_count;
    int wave_list_count;
    int position_count; // sequence entries per voice
} Sd1Layout;

typedef struct Sd1Module {
    uint8_t* memory; // the module followed by zero bytes, as the replayer sees it before it runs
    uint32_t size;
    Sd1Layout layout;
} Sd1Module;

// A voice record of the replayer.
typedef struct Sd1Voice {
    int sequence;         // index into the voice's sequence entries
    uint32_t row_pointer; // module offset of the next track row
    uint32_t instrument;  // module offset of the current instrument record; 0 until one is chosen
    uint16_t period;      // the note's period after arpeggio, bend, phase and fall
    uint8_t bend_target;  // note index the bend slides to
    int8_t bend_speed;
    uint8_t note_offset; // twice the note index, as a byte
    uint8_t arpeggio_index;
    Sd1Envelope envelope;
    uint8_t row_count; // rows left of the current note
    uint8_t volume;
    uint8_t sustain_count;
    uint16_t pitch_control; // accumulated bend
    uint8_t phase_index;
    uint8_t phase_speed;
    uint16_t pitch_fall_control;
    uint8_t wave_index;
    uint8_t waveform; // the instrument's waveform number
    uint8_t wave_speed;
    uint8_t sample_state; // 0 waveform, 1 sample just started, 2 one interrupt later, 3 loop registers written
    uint32_t loop_start;  // module offset of the sample's loop, 0xFFFFFFFF for none
    uint32_t loop_length; // words
    int shown_note;       // semitones from C-0 + SD1_NOTE_OCTAVE_SHIFT octaves of the last note started, -1 if none
    int shown_instrument; // 1-based instrument of the last row that chose one, 0 if none

    NuadePaulaChannel paula;
} Sd1Voice;

typedef struct Sd1Song {
    uint32_t speed;
    uint32_t speed_counter;
    uint32_t rows;        // rows per position
    uint32_t row;         // row of the position, -1 before the first
    uint32_t position;    // 1-based
    bool new_position;    // this interrupt starts the next sequence entry
    bool loop_song;       // ... and it is the first again
    bool special_samples; // the song plays the special samples on voice 3
    bool special_pending; // ... one has been started, so its repeat length is cut to two words
    uint32_t mix_counter[2];
    uint32_t mix_position[2];
    uint32_t filter_position;
    bool looped;
    Sd1Voice voices[SD1_VOICES];
} Sd1Song;

struct Sd1Player {
    Sd1Module module;
    uint8_t* memory; // the working copy of the module memory: the replayer rewrites waveforms and instruments
    Sd1Song song;
};

typedef struct Sd1CreateResult {
    Sd1Player* player; // 0 on failure
    Sd1Status status;
} Sd1CreateResult;

typedef struct Sd1TickResult {
    bool looped; // the song wrapped to its first position during this tick
} Sd1TickResult;

typedef struct Sd1Position {
    int order; // 0-based position
    int row;
    int rows;
    int speed;
} Sd1Position;

typedef struct Sd1VoiceState {
    int note;       // semitones from C-0 + SD1_NOTE_OCTAVE_SHIFT octaves of the last note started, -1 if none
    int instrument; // 1-based instrument of the last row that chose one, 0 if none
} Sd1VoiceState;

#define SD1_NOTE_RELEASE 0xFF

// One track row as stored.
typedef struct Sd1Cell {
    uint8_t note; // 0 none, 0xFF release
    uint8_t instrument;
    uint8_t effect;
    uint8_t parameter;
    uint8_t duration; // rows after this one it lasts
} Sd1Cell;

// The replayer's tables are found by scanning for the code that uses them, as the eagleplayer does.
bool sd1_probe(const uint8_t* data, size_t size);

Sd1CreateResult sd1_create(const uint8_t* data, size_t size);
void sd1_destroy(Sd1Player* player);

// (Re)start the song from its beginning; the player drives `voices` (Paula channel n = voice n).
bool sd1_start(Sd1Player* player, NuadeVoices* voices);

Sd1TickResult sd1_tick(Sd1Player* player, NuadeVoices* voices);

Sd1Position sd1_position(const Sd1Player* player);
Sd1VoiceState sd1_voice_state(const Sd1Player* player, int voice);

// The cell of `voice` at `row` of the position `order`; the row where a note starts, or an empty cell.
Sd1Cell sd1_pattern_cell(const Sd1Player* player, int order, int voice, int row);
int sd1_order_count(const Sd1Player* player);
int sd1_instrument_count(const Sd1Player* player);

// The note column value of a note index of a track row (a note column counts semitones as the voice state does), -1
// for a note the table has no period for.
int sd1_note_semitone(const Sd1Player* player, int note);

// The name of a sample instrument (the 1-based `instrument` of a row) into `out`; false for a synthesised one or one
// out of range.
#define SD1_SAMPLE_NAME_SIZE 20
bool sd1_sample_name(const Sd1Player* player, int instrument, char out[SD1_SAMPLE_NAME_SIZE + 1]);

// The sounds that can be asked for with sd1_instrument_pcm (the instruments, and the fixed samples of the old replayer
// behind them when the song uses them) and the PCM of one (the 1-based `instrument` of a row): the data as the module
// holds it, the loop inside it. False for a synthesised instrument or one out of range.
int sd1_exportable_count(const Sd1Player* player);
bool sd1_instrument_pcm(const Sd1Player* player, int instrument, const int8_t** data, uint32_t* length,
                        uint32_t* loop_start, uint32_t* loop_length);

// The player's state behind a song it opened; 0 when the song is another player's.
Sd1Player* sd1_from_nuade(NuadeSong* song);

#ifdef __cplusplus
}
#endif
