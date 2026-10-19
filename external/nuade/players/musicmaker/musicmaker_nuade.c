// SPDX-License-Identifier: MIT
// MusicMaker V8 as a nuade player.

#include <nuade/player.h>

#include "nuade_base.h"
#include <nuade/players/musicmaker.h>
#include <stdio.h>

static NuadeProbe musicmaker_probe(const uint8_t* data, size_t size, const char* filename) {
    return mm_probe(data, size, filename) ? NuadeProbe_Supported : NuadeProbe_Unsupported;
}

static NuadePlayerOpenResult musicmaker_open(const NuadeSongSource* source) {
    NuadePlayerOpenResult result = { 0 };
    MmPlayer* player = mm_create(source);
    if (player == nullptr) {
        result.error.status = NuadeStatus_InvalidData;
        snprintf(result.error.message, sizeof(result.error.message), "not a valid MusicMaker V8 song");
        return result;
    }
    result.user_data = player;
    result.info = (NuadeSongInfo) { .voice_count = mm_voice_count(player),
                                    .subsong_count = 1,
                                    .default_subsong = 0,
                                    .timebase_hz = mm_timebase_hz(player),
                                    // the 8 voice replayer mixes its tracks into the Paula voices: show the tracks, and
                                    // leave a muted one out of the mix
                                    .display_voices
                                    = mm_track_count(player) != mm_voice_count(player) ? mm_track_count(player) : 0 };
    result.info.player_mutes = result.info.display_voices != 0;
    return result;
}

static void musicmaker_close(void* user_data) {
    mm_destroy((MmPlayer*)user_data);
}

static bool musicmaker_start(void* user_data, uint32_t subsong, uint32_t sample_rate, NuadeVoices* voices) {
    (void)sample_rate;
    return subsong == 0 && mm_start((MmPlayer*)user_data, voices);
}

static NuadeTickResult musicmaker_tick(void* user_data, NuadeVoices* voices) {
    const MmTickResult tick = mm_tick((MmPlayer*)user_data, voices);
    return (NuadeTickResult) { .length = tick.length, .flags = tick.looped ? NuadeTick_Looped : NuadeTick_None };
}

static void musicmaker_metadata(void* user_data, const NuadeMetadataSink* sink) {
    sink->set_tag(sink->ctx, NUADE_META_FORMAT, "MusicMaker V8");
    sink->set_tag(sink->ctx, NUADE_META_TITLE, mm_title((const MmPlayer*)user_data));
}

// One display voice per track (NuadeSongInfo.display_voices).
static uint32_t musicmaker_voice_info(void* user_data, NuadeVoiceInfo* out, uint32_t cap) {
    const MmPlayer* player = (const MmPlayer*)user_data;
    const uint32_t n = cap < mm_track_count(player) ? cap : mm_track_count(player);
    for (uint32_t i = 0; i < n; i++) {
        const MmVoiceState v = mm_voice_state(player, i);
        out[i] = (NuadeVoiceInfo) { .note = v.note, .instrument = v.instrument, .note_starts = v.note_starts };
    }
    return n;
}

static uint32_t musicmaker_grid_ticks(void* user_data, uint32_t subsong) {
    (void)subsong;
    return mm_grid_ticks((const MmPlayer*)user_data);
}

static uint32_t musicmaker_sample_count(void* user_data) {
    (void)user_data;
    return MM_INSTRUMENTS;
}

static bool musicmaker_sample_get(void* user_data, uint32_t n, NuadeExportSample* out) {
    const int8_t* data;
    uint32_t length, loop_start, loop_length;
    if (!mm_sample_pcm((const MmPlayer*)user_data, n, &data, &length, &loop_start, &loop_length)) {
        return false;
    }
    *out = (NuadeExportSample) {
        .index = n, .data = data, .frames = length, .bits = 8, .loop_start = loop_start, .loop_frames = loop_length
    };
    return true;
}

const NuadePlayer nuade_player_musicmaker = {
    .api_version = NUADE_PLAYER_API_VERSION,
    .id = "musicmaker",
    .name = "MusicMaker V8",
    .version = "1",
    .extensions = "mm4,mm8,sdata",
    .probe = musicmaker_probe,
    .open = musicmaker_open,
    .close = musicmaker_close,
    .start = musicmaker_start,
    .tick = musicmaker_tick,
    .metadata = musicmaker_metadata,
    .voice_info = musicmaker_voice_info,
    .grid_ticks = musicmaker_grid_ticks,
    .sample_count = musicmaker_sample_count,
    .sample_get = musicmaker_sample_get,
};

MmPlayer* mm_from_nuade(NuadeSong* song) {
    if (nuade_player(song) != &nuade_player_musicmaker) {
        return nullptr;
    }
    return (MmPlayer*)nuade_player_data(song);
}
