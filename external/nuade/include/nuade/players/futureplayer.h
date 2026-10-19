// SPDX-License-Identifier: MIT
// Future Player, ported from the Future Player replayer in Wanted Team's "Future Player_v1.asm" ((c) 1988-89 Paul van
// der Valk, adapted by Wanted Team; Hybris replayer (c) 1989 Discovery Software). Plays the .fp files of Wanted Team's
// converted format: an Amiga executable whose single hunk holds the song data.

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

#define FP_VOICES 4

// The CIA clock: the timebase of the tick length.
#define FP_TIMEBASE_HZ 709379

// The addresses the module's pointers hold: the hunk loaded at this address, as if relocated.
#define FP_BASE_ADDRESS 0x10000u

// Levels of the call stack of sub-sequences and loops that a voice's track keeps.
#define FP_STACK_LEVELS 8

// Instruments a player numbers for the display; a module has far fewer.
#define FP_MAX_INSTRUMENTS 256

typedef struct FpPlayer FpPlayer;

typedef enum FpStatus {
    FpStatus_Ok = 0,
    FpStatus_InvalidData,
    FpStatus_OutOfMemory,
} FpStatus;

typedef struct FpCreateResult {
    FpPlayer* player; // null on failure
    FpStatus status;
} FpCreateResult;

typedef struct FpTickResult {
    bool looped;     // the last running voice reached its end: the replayer calls SongEnd
    bool ended;      // the data sent a voice somewhere the replayer cannot follow; the song stops
    uint32_t length; // until the next tick, in 1 / FP_TIMEBASE_HZ seconds
} FpTickResult;

// What a voice (0-based) last started: its note (semitones, -1 if none yet) and the instrument (-1 if none).
typedef struct FpVoiceState {
    int note;
    int instrument;
} FpVoiceState;

// The module: the hunk of the file as it lies in memory, then the replayer's own tables that the module's pointers
// can reach. An address is FP_BASE_ADDRESS + an offset in `data`; an address outside it reads 0.
typedef struct FpModule {
    uint8_t* data;       // image_size bytes followed by the zero padding the Paula channel needs
    uint32_t hunk_size;  // bytes of the hunk
    uint32_t image_size; // the hunk, then the replayer's tables
    uint32_t song_table; // address of the song table: 8 bytes per song, ended by a zero address
    uint32_t subsongs;
    uint32_t title; // addresses of the strings, 0 if none
    uint32_t author;
    uint32_t info;
    // The replayer's tables, as addresses in the image.
    uint32_t arpeggio_default; // a table of one step, 0
    uint32_t silence;          // the two words every one-shot sample repeats
    uint32_t instrument_default;
    uint32_t sample_default;
} FpModule;

typedef enum FpModulationKind {
    FpModulation_Pitch1,
    FpModulation_Pitch2,
    FpModulation_Address1,
    FpModulation_Address2,
    FpModulation_Count,
} FpModulationKind;

// A table that a voice steps through while a note plays, one entry per tick: it bends the period or moves the sample
// address.
typedef struct FpModulation {
    uint32_t table; // address of the table record (0: none); its data pointer is at +8, its length at +12
    uint8_t shift;  // entries are shifted left by this
    uint8_t delay;  // ticks before the table starts
    uint8_t flags;  // 1: keep the position at a new note; bit 7: stop at the end instead of wrapping
    bool negate;
} FpModulation;

// The sound parameters of an instrument (the record at +12 of the instrument).
typedef struct FpParams {
    uint8_t volume;
    int8_t detune[FP_VOICES]; // added to the period of voice 0..3 when an instrument is selected
    uint8_t attack_rate;      // the level a note starts at; added to the level each tick of the attack
    uint8_t attack_level;
    uint8_t decay_rate;
    uint8_t decay_level;
    int8_t sustain_rate; // bit 7: the level falls by the rate without that bit
    uint8_t sustain_level;
    uint8_t release_rate;
    bool retrigger; // a note always starts at attack_rate; else it keeps a higher level of the sound before it
    FpModulation modulation[FpModulation_Count];
} FpParams;

// The sample record an instrument plays.
typedef struct FpSample {
    uint32_t address;
    uint16_t bytes;
    bool dynamic;  // bit 0: the sample address is written every tick and moved by two modulation tables
    bool one_shot; // bit 1: after the first pass the sample repeats a silent buffer
    uint8_t note_offset;
} FpSample;

typedef enum FpEnvelopePhase {
    FpEnvelope_Attack,
    FpEnvelope_Decay,
    FpEnvelope_Sustain,
    FpEnvelope_Hold,
} FpEnvelopePhase;

typedef struct FpVoice {
    NuadePaulaChannel paula;

    // Track
    bool stopped;  // the voice ended and its sound is cut off
    bool finished; // the track ended: the sequencer does not run
    uint32_t track_pos;
    uint8_t duration_left; // ticks (in steps of the tempo) until the next track entry
    uint8_t note;          // the note the track set last
    uint8_t note_on;       // the track holds a note (0: released)
    bool new_note;         // the track started a note: the sound restarts in this tick
    uint8_t transpose1;
    uint8_t transpose2;
    uint32_t instrument; // address of the instrument record
    uint32_t arpeggio;   // address of the arpeggio table (steps of 1 byte at +8, ended by 0x80)
    uint16_t arpeggio_pos;
    uint16_t portamento_speed; // 0: notes set the period; else the period slides to the note by this much a tick
    // The call stack of sub-sequences and loops
    uint8_t stack_level; // the level the track runs at, below FP_STACK_LEVELS
    uint32_t return_pos[FP_STACK_LEVELS];
    uint8_t loop_count[FP_STACK_LEVELS];
    uint32_t loop_start[FP_STACK_LEVELS];

    // The sound
    int16_t detune;
    uint8_t level;
    uint8_t master; // the fade level, scales the volume
    FpEnvelopePhase phase;
    uint16_t base_period; // the period of the note before the modulation
    uint16_t modulation_pos[FpModulation_Count];
    uint8_t modulation_delay[FpModulation_Count];
    // The sequence of samples some instruments play one after the other
    bool sequence_started;
    uint32_t sequence_sample; // address of the sample record that plays
    uint8_t sequence_ticks;
    uint32_t sequence_loop; // where the sequence goes back to
    uint32_t sequence_next;

    // Display
    int shown_note;
    int shown_instrument;
} FpVoice;

struct FpPlayer {
    FpModule module;
    uint32_t instrument_address[FP_MAX_INSTRUMENTS]; // the instruments numbered so far, for the display
    uint32_t instrument_count;
    FpVoice voices[FP_VOICES];

    // The song
    uint8_t tempo; // ticks per step of the track
    uint8_t tempo_count;
    uint16_t timer; // CIA timer value of one tick
    bool prerun;    // the song start runs the sequencers ahead
    bool fading_in;
    uint8_t fade_level;
    uint8_t fade_speed;
    bool voice_present[FP_VOICES]; // the song has a track for the voice
    bool voice_running[FP_VOICES]; // the voice has not reached its end
    uint8_t command_guard;         // commands left in one sequencer call
    bool looped;
    bool failed;
};

// True when the data is a Future Player module: the eagleplayer's check, the hunk and the song tables fit, and so do
// the track streams of every song and the instruments they select.
bool fp_probe(const uint8_t* data, size_t size);
FpCreateResult fp_create(const uint8_t* data, size_t size);
void fp_destroy(FpPlayer* player);

int fp_subsong_count(const FpPlayer* player);
// (Re)start a subsong (0-based) from its beginning; the player drives `voices` (Paula channel n = voice n).
bool fp_start(FpPlayer* player, int subsong, NuadeVoices* voices);

// Run one player tick and hand the Paula registers it wrote to the voices.
FpTickResult fp_tick(FpPlayer* player, NuadeVoices* voices);

// Ticks of one step of the track in a subsong (0-based); 0 for a bad subsong.
uint32_t fp_grid_ticks(const FpPlayer* player, int subsong);
FpVoiceState fp_voice_state(const FpPlayer* player, int voice);
// The module's strings (title, author, info), copied into `out`; empty if there is none.
void fp_module_string(const FpPlayer* player, uint32_t address, char* out, size_t cap);

// The player's state behind a song it opened; 0 when the song is another player's.
FpPlayer* fp_from_nuade(NuadeSong* song);

#ifdef __cplusplus
}
#endif
