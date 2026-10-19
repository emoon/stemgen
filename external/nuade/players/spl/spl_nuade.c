// SPDX-License-Identifier: MIT
// Sound Programming Language as a nuade player.

#include <nuade/player.h>

#include "nuade_base.h"
#include <nuade/players/spl.h>
#include <stdio.h>

static NuadeProbe spl_nuade_probe(const uint8_t* data, size_t size, const char* filename) {
    (void)filename;
    return spl_probe(data, size) ? NuadeProbe_Supported : NuadeProbe_Unsupported;
}

static NuadePlayerOpenResult spl_nuade_open(const NuadeSongSource* source) {
    NuadePlayerOpenResult result = { 0 };
    SplPlayer* player = spl_create(source->data, source->size);
    if (player == nullptr) {
        result.error.status = NuadeStatus_InvalidData;
        snprintf(result.error.message, sizeof(result.error.message), "not a known Sound Programming Language module");
        return result;
    }
    result.user_data = player;
    result.info = (NuadeSongInfo) { .voice_count = SPL_VOICES,
                                    .subsong_count = (uint32_t)spl_subsong_count(player),
                                    .default_subsong = 0,
                                    .timebase_hz = SPL_TIMEBASE_HZ };
    return result;
}

static void spl_nuade_close(void* user_data) {
    spl_destroy((SplPlayer*)user_data);
}

static bool spl_nuade_start(void* user_data, uint32_t subsong, uint32_t sample_rate, NuadeVoices* voices) {
    (void)sample_rate;
    return spl_start((SplPlayer*)user_data, (int)subsong, voices);
}

static NuadeTickResult spl_nuade_tick(void* user_data, NuadeVoices* voices) {
    SplPlayer* player = (SplPlayer*)user_data;
    const SplTickResult tick = spl_tick(player, voices);
    return (NuadeTickResult) { .length = spl_tick_length(player),
                               .flags = tick.looped ? NuadeTick_Looped : NuadeTick_None };
}

static void spl_nuade_metadata(void* user_data, const NuadeMetadataSink* sink) {
    (void)user_data;
    sink->set_tag(sink->ctx, NUADE_META_FORMAT, "Sound Programming Language");
}

static uint32_t spl_nuade_voice_info(void* user_data, NuadeVoiceInfo* out, uint32_t cap) {
    const SplPlayer* player = (const SplPlayer*)user_data;
    const uint32_t count = cap < SPL_VOICES ? cap : SPL_VOICES;
    for (uint32_t i = 0; i < count; i++) {
        const int32_t sound = spl_record_sound_id(player->module->build, &player->voices[i].record);
        out[i] = (NuadeVoiceInfo) { .note = -1, .instrument = sound };
    }
    return count;
}

static uint32_t spl_nuade_sample_count(void* user_data) {
    return spl_sound_count((const SplPlayer*)user_data);
}

static bool spl_nuade_sample_get(void* user_data, uint32_t n, NuadeExportSample* out) {
    int32_t id;
    const int8_t* data;
    uint32_t frames, loop_start, loop_frames;
    if (!spl_sound_pcm((const SplPlayer*)user_data, n, &id, &data, &frames, &loop_start, &loop_frames)) {
        return false;
    }
    *out = (NuadeExportSample) { .index = (uint32_t)id,
                                 .data = data,
                                 .frames = frames,
                                 .bits = 8,
                                 .loop_start = loop_start,
                                 .loop_frames = loop_frames };
    return true;
}

const NuadePlayer nuade_player_spl = {
    .api_version = NUADE_PLAYER_API_VERSION,
    .id = "spl",
    .name = "Sound Programming Language",
    .version = "1",
    .extensions = "spl",
    .probe = spl_nuade_probe,
    .open = spl_nuade_open,
    .close = spl_nuade_close,
    .start = spl_nuade_start,
    .tick = spl_nuade_tick,
    .metadata = spl_nuade_metadata,
    .voice_info = spl_nuade_voice_info,
    .sample_count = spl_nuade_sample_count,
    .sample_get = spl_nuade_sample_get,
};

SplPlayer* spl_from_nuade(NuadeSong* song) {
    if (nuade_player(song) != &nuade_player_spl) {
        return nullptr;
    }
    return (SplPlayer*)nuade_player_data(song);
}
