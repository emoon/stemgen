// Raw voice mixer: nearest sample, no filter, no ramp. Implements the nuade_voice_* calls of include/nuade/voice.h.

#pragma once

#include "nuade_base.h"
#include "nuade_test.h"
#include <nuade/viz.h>

// `track_count` is how many voices can be muted (the display voices; at least voice_count). `gates`: the mixer leaves
// muted voices and instruments out itself; false for a player that applies mutes itself.
NuadeVoices* nuade_mixer_create(u32 voice_count, u32 track_count, bool gates);
void nuade_mixer_destroy(NuadeVoices* mixer);

// Silence every voice and restart the frame counter at the given output rate. The observer is kept.
void nuade_mixer_reset(NuadeVoices* mixer, u32 sample_rate);

void nuade_mixer_set_observer(NuadeVoices* mixer, NuadeVoiceObserver observer, void* user);

// Scale every voice's pan by `separation` (0..2) when mixing; 1 (the default) mixes the pans as set, above 1 a pan
// goes past +-1 and the far side gets a negative gain. Kept across nuade_mixer_reset. Events and the voice state report
// the pan the player set.
void nuade_mixer_set_separation(NuadeVoices* mixer, f32 separation);

// Leave a voice out of the stereo mix. The voice still plays on (position, loops, events) and its voice buffer and
// meters still see it. Kept across nuade_mixer_reset. False if `track` is out of range.
bool nuade_mixer_set_muted(NuadeVoices* mixer, u32 track, bool muted);

// Leave out of the stereo mix every sound that `instrument` started (the number the player reports in
// NuadeVoiceInfo.instrument). A sound belongs to the instrument its voice reports right after the tick that started it
// (nuade_mixer_tag_starts), so a mute applies to sounds started after the call; a sound with no instrument is never
// muted. Kept across nuade_mixer_reset. False if `instrument` is out of range.
#define NUADE_MAX_INSTRUMENT 65536
bool nuade_mixer_set_instrument_muted(NuadeVoices* mixer, u32 instrument, bool muted);
bool nuade_mixer_has_instrument_mutes(const NuadeVoices* mixer);

// After a tick: give each voice that started a sound (or queued one behind a playing sound) since the last call the
// instrument `info[voice]` reports; `count` is how many voices `info` holds, the rest read as none.
void nuade_mixer_tag_starts(NuadeVoices* mixer, const NuadeVoiceInfo* info, u32 count);

// Unmute every voice and instrument.
void nuade_mixer_clear_mutes(NuadeVoices* mixer);

// Add `frames` frames of output to `stereo` (interleaved) starting at frame `offset`. `voice_out` is 0 or one buffer
// per voice; voice_out[v][offset + i] is set to voice v's left + right contribution (not added).
void nuade_mixer_mix(NuadeVoices* mixer, float* stereo, float* const* voice_out, u32 offset, u32 frames);

// Move every voice forward by `frames` frames exactly as nuade_mixer_mix does (loop wraps, queued switches, one-shot
// ends, observer events), without reading or writing any sample. A player that reads the voice state back sees the same
// state as after a real render.
void nuade_mixer_advance(NuadeVoices* mixer, u32 frames);
