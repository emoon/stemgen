// SPDX-License-Identifier: MIT
// Steve Turner player, ported from the Steve Turner player in Wanted Team's "Steve Turner_v4.asm" (engine (c) 1989-90
// Steve Turner, adapted by Mr.Larmer/Wanted Team; from the game Off Road Racer). Plays the first Graftgold music format
// (Rainbow Islands, Paradroid 90, Simulcra, Off Road Racer, ...).

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

#define ST_VOICES 4

// The CIA clock: the timebase of ST_TICK_LENGTH.
#define ST_TIMEBASE_HZ 709379
// One player tick: the eagleplayer sets the CIA timer to 0x1BC0 (about 100 Hz), in 1 / ST_TIMEBASE_HZ seconds.
#define ST_TICK_LENGTH 7104

// Pitch records a sound can step through (an instrument holds 10 bytes of them each, before its parameters at 0x1E).
#define ST_PITCH_RECORDS 3

typedef struct StPlayer StPlayer;

typedef enum StStatus {
    StStatus_Ok = 0,
    StStatus_InvalidData,
    StStatus_OutOfMemory,
} StStatus;

typedef struct StCreateResult {
    StPlayer* player; // null on failure
    StStatus status;
} StCreateResult;

typedef struct StTickResult {
    bool looped; // a track list reached its end word
    bool ended;  // the data sent a voice somewhere the replayer cannot follow; the song stops
} StTickResult;

// What a voice (0-based) last started: its note (index into the period table, -1 if none yet) and the instrument
// (-1 if none).
typedef struct StVoiceState {
    int note;
    int instrument;
} StVoiceState;

// The module: an image of the original's memory, 0x2E bytes of start code and then the four tables. Offsets are from
// the start of the image; a read outside it gives 0.
typedef struct StModule {
    uint8_t* data; // size bytes followed by the zero padding the Paula channel needs
    uint32_t size;
    uint32_t instruments; // 0x30 bytes each
    uint32_t songs;       // 12 bytes each: priority, speed, 2 unused bytes, then the track list offset of each voice
    uint32_t patterns;    // word offsets of the patterns from this table
    uint32_t samples;     // long offsets of the samples from this table; a sample is its length in bytes, then data
    uint32_t calc_size;   // where the last pattern ends; 0 if the pattern scan found no end
    uint32_t subsongs;
} StModule;

// What a voice's sequencer does on its next call.
typedef enum StSequenceState {
    StSequence_Stopped,
    StSequence_ReadTrack, // the next track list byte
    StSequence_Wait,      // the current note runs
} StSequenceState;

// The volume envelope of the sound that plays.
typedef enum StEnvelopeState {
    StEnvelope_Stopped,
    StEnvelope_DmaOff, // a sound was started: DMA stops, the next tick restarts it
    StEnvelope_DmaOn,
    StEnvelope_Loop,   // the sample is one-shot: its repeat is the silent buffer
    StEnvelope_Attack, // the phases of the envelope: each steps the level by a rate for a number of ticks
    StEnvelope_Sustain,
    StEnvelope_Hold,
    StEnvelope_Release,
} StEnvelopeState;

typedef enum StPitchState {
    StPitch_Stopped,
    StPitch_Start,
    StPitch_Wait, // the pitch record holds for a number of ticks
    StPitch_Step, // the record's slide and flags run
} StPitchState;

// A pitch record of an instrument: the period slides by delta (which changes by delta_step at every step) for `steps`
// steps of `hold_ticks` ticks each, `repeats` times, then the flags apply.
typedef struct StPitchSpec {
    uint16_t period;
    int16_t delta;
    uint8_t repeats;
    uint8_t steps;
    uint8_t hold_ticks;
    int8_t delta_step;
    uint8_t flags;
} StPitchSpec;

// The instrument record of the module (0x30 bytes).
typedef struct StInstrument {
    StPitchSpec pitch[ST_PITCH_RECORDS];
    uint8_t priority; // a sound needs at least the priority of the one that plays to replace it
    uint8_t sample;   // bit 7: no sample
    uint8_t repeats;  // passes of the envelope; 0 plays the sample once
    uint8_t attack_ticks;
    int8_t attack_rate;
    uint8_t sustain_ticks;
    int8_t sustain_rate;
    uint8_t period_shift; // the period register gets the pitch shifted right by this
    uint16_t hold_phase_ticks;
    int8_t hold_rate;
    uint8_t hold_turn_ticks; // the hold phase changes the sign of its rate this often
    int8_t release_rate;
    uint8_t record_count;
    uint8_t vibrato_delay;
    uint8_t vibrato_speed;
    int8_t vibrato_depth;
    uint8_t next; // the instrument that starts when this one ends, 1-based; 0 for none
} StInstrument;

// The working copy of a pitch record.
typedef struct StPitchRecord {
    uint16_t period;
    uint16_t reload; // the period the record's reload flag returns to
    int16_t delta;
    uint8_t steps;
} StPitchRecord;

typedef struct StVoice {
    NuadePaulaChannel paula;

    // Sequencer
    StSequenceState sequence;
    uint32_t track_pos;   // next track list byte
    uint32_t pattern_pos; // next pattern byte
    uint8_t duration;     // ticks of one note, in steps of the song's speed
    uint8_t note_timer;
    int8_t portamento_shift; // 0: a note sets the pitch, else the pitch slides to it by delta >> shift
    uint8_t instrument_priority;
    uint16_t instrument_offset; // the instrument the next notes play (an offset in the instrument table)
    uint16_t target_period;     // the portamento's goal

    // The sound that plays
    uint8_t playing_priority; // a note needs at least this priority to replace it
    StInstrument sound;       // the instrument that plays

    // The start slots of the original's voice record. A voice that no track list drives keeps them zeroed: the first
    // tick starts instrument 1 from the request slot, and the countdown starts it once more when it wraps.
    bool request_armed;   // the request slot holds instrument 1 (a track list start clears it)
    bool queued_armed;    // the queued start holds instrument 1 at priority 0
    uint8_t queued_delay; // ticks until the queued start, counted down from 0 (wrapping)

    // Volume envelope
    StEnvelopeState envelope;
    uint8_t repeats;      // passes of the envelope left
    bool one_shot;        // the sound runs once: its sample is cut off after the first play
    uint16_t phase_ticks; // ticks left of the phase
    int8_t rate;          // added to the level at every tick
    uint8_t turn_ticks;   // ticks until the rate changes sign, in the hold phase
    uint16_t level;       // the volume is level / 4

    // Pitch
    StPitchState pitch_state;
    bool portamento; // the pitch slides to target_period
    StPitchRecord records[ST_PITCH_RECORDS];
    uint8_t record;      // the record that plays
    uint8_t outer_count; // the record's repeats left
    uint8_t inner_count; // steps left before the record's flags apply
    uint8_t hold_ticks;
    uint8_t vibrato_delay; // ticks until the vibrato starts
    uint8_t vibrato_timer;
    int8_t vibrato_step; // added to the period at every tick

    // Display
    int note;
    int instrument_shown;
} StVoice;

struct StPlayer {
    StModule module;
    int8_t silence[4]; // the buffer a one-shot sample's repeat points at

    // The song
    uint8_t song;          // the subsong that plays (1-based), 0 before the first start
    uint8_t song_priority; // priority of the song that plays; another song needs at least that
    uint8_t speed;         // ticks per step of a note's duration
    uint8_t speed_count;
    uint8_t restart; // song to start in this tick, 0 for none
    StVoice voices[ST_VOICES];
    bool looped;
    bool failed;
};

// True when the data passes the eagleplayer's module check, its tables fit, and so do the track lists, patterns,
// instruments and samples that its songs reach.
bool st_probe(const uint8_t* data, size_t size);
StCreateResult st_create(const uint8_t* data, size_t size);
void st_destroy(StPlayer* player);

int st_subsong_count(const StPlayer* player);
// (Re)start a subsong (0-based) from its beginning; the player drives `voices` (Paula channel n = voice n).
bool st_start(StPlayer* player, int subsong, NuadeVoices* voices);

// Run one player tick and hand the Paula registers it wrote to the voices.
StTickResult st_tick(StPlayer* player, NuadeVoices* voices);

// Ticks of one step of a note's duration in a subsong (0-based); 0 for a bad subsong.
uint32_t st_grid_ticks(const StPlayer* player, int subsong);
StVoiceState st_voice_state(const StPlayer* player, int voice);

// The player's state behind a song it opened; 0 when the song is another player's.
StPlayer* st_from_nuade(NuadeSong* song);

#ifdef __cplusplus
}
#endif
