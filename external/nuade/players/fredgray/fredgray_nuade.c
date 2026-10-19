// SPDX-License-Identifier: MIT
// Fred Gray player as a nuade player.

#include <nuade/player.h>

#include "nuade_base.h"
#include <nuade/players/fredgray.h>
#include <stdio.h>

// Ticks one row of the display grid spans: the sections run their own steps, so the grid is a display choice only.
#define FG_GRID_TICKS 8

static NuadeProbe fredgray_probe(const uint8_t* data, size_t size, const char* filename) {
    (void)filename;
    return fg_probe(data, size) ? NuadeProbe_Supported : NuadeProbe_Unsupported;
}

static NuadePlayerOpenResult fredgray_open(const NuadeSongSource* source) {
    NuadePlayerOpenResult result = { 0 };
    FgPlayer* player = fg_create(source->data, source->size);
    if (player == nullptr) {
        result.error.status = NuadeStatus_InvalidData;
        snprintf(result.error.message, sizeof(result.error.message), "not a known Fred Gray module");
        return result;
    }
    result.user_data = player;
    result.info = (NuadeSongInfo) { .voice_count = FG_VOICES,
                                    .subsong_count = (uint32_t)fg_subsong_count(player),
                                    .default_subsong = 0,
                                    .timebase_hz = FG_TIMEBASE_HZ };
    return result;
}

static void fredgray_close(void* user_data) {
    fg_destroy((FgPlayer*)user_data);
}

static bool fredgray_start(void* user_data, uint32_t subsong, uint32_t sample_rate, NuadeVoices* voices) {
    (void)sample_rate;
    return fg_start((FgPlayer*)user_data, (int)subsong, voices);
}

static NuadeTickResult fredgray_tick(void* user_data, NuadeVoices* voices) {
    const FgTickResult tick = fg_tick((FgPlayer*)user_data, voices);
    return (NuadeTickResult) { .length = FG_TICK_LENGTH, .flags = tick.looped ? NuadeTick_Looped : NuadeTick_None };
}

static void fredgray_metadata(void* user_data, const NuadeMetadataSink* sink) {
    const FgPlayer* player = (const FgPlayer*)user_data;
    sink->set_tag(sink->ctx, NUADE_META_FORMAT, "Fred Gray");
    sink->set_tag(sink->ctx, NUADE_META_TITLE, fg_title(player));
    sink->set_tag(sink->ctx, NUADE_META_ARTIST, fg_author(player));
    sink->set_tag(sink->ctx, NUADE_META_TOOL, fg_version_name(player));
}

// The wave a voice loops is its sound: its offset in the module image names it.
static int32_t voice_wave(const FgPlayer* player, int voice) {
    const int8_t* lc = player->voices[voice].paula.lc;
    return lc != nullptr ? (int32_t)(lc - (const int8_t*)player->work.image) : -1;
}

static uint32_t fredgray_voice_info(void* user_data, NuadeVoiceInfo* out, uint32_t cap) {
    const FgPlayer* player = (const FgPlayer*)user_data;
    const uint32_t count = cap < FG_VOICES ? cap : FG_VOICES;
    for (uint32_t i = 0; i < count; i++) {
        const FgVoiceState state = fg_voice_state(player, (int)i);
        out[i] = (NuadeVoiceInfo) { .note = state.note, .instrument = voice_wave(player, (int)i) };
    }
    return count;
}

static uint32_t fredgray_grid_ticks(void* user_data, uint32_t subsong) {
    (void)user_data;
    (void)subsong;
    return FG_GRID_TICKS;
}

static uint32_t fredgray_sample_count(void* user_data) {
    return fg_sound_count((const FgPlayer*)user_data);
}

static bool fredgray_sample_get(void* user_data, uint32_t n, NuadeExportSample* out) {
    uint32_t number, bytes;
    const int8_t* data;
    bool loops;
    if (!fg_sound_pcm((const FgPlayer*)user_data, n, &number, &data, &bytes, &loops)) {
        return false;
    }
    *out = (NuadeExportSample) {
        .index = number, .data = data, .frames = bytes, .bits = 8, .loop_frames = loops ? bytes : 0
    };
    return true;
}

const NuadePlayer nuade_player_fredgray = {
    .api_version = NUADE_PLAYER_API_VERSION,
    .id = "fredgray",
    .name = "Fred Gray",
    .version = "1",
    .extensions = "gray",
    .probe = fredgray_probe,
    .open = fredgray_open,
    .close = fredgray_close,
    .start = fredgray_start,
    .tick = fredgray_tick,
    .metadata = fredgray_metadata,
    .voice_info = fredgray_voice_info,
    .grid_ticks = fredgray_grid_ticks,
    .sample_count = fredgray_sample_count,
    .sample_get = fredgray_sample_get,
};

FgPlayer* fg_from_nuade(NuadeSong* song) {
    if (nuade_player(song) != &nuade_player_fredgray) {
        return nullptr;
    }
    return (FgPlayer*)nuade_player_data(song);
}
