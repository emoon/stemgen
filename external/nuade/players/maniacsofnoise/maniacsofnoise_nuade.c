// SPDX-License-Identifier: MIT
// Maniacs of Noise player as a nuade player.

#include <nuade/player.h>

#include "nuade_base.h"
#include <nuade/players/maniacsofnoise.h>
#include <stdio.h>

static NuadeProbe maniacsofnoise_probe(const uint8_t* data, size_t size, const char* filename) {
    (void)filename;
    return mon_probe(data, size) ? NuadeProbe_Supported : NuadeProbe_Unsupported;
}

static NuadePlayerOpenResult maniacsofnoise_open(const NuadeSongSource* source) {
    NuadePlayerOpenResult result = { 0 };
    MonPlayer* player = mon_create(source->data, source->size);
    if (player == nullptr) {
        result.error.status = NuadeStatus_InvalidData;
        snprintf(result.error.message, sizeof(result.error.message), "not a Maniacs of Noise module");
        return result;
    }
    result.user_data = player;
    result.info = (NuadeSongInfo) { .voice_count = MON_VOICES,
                                    .subsong_count = (uint32_t)mon_subsong_count(player),
                                    .default_subsong = 0,
                                    .timebase_hz = MON_TIMEBASE_HZ };
    return result;
}

static void maniacsofnoise_close(void* user_data) {
    mon_destroy((MonPlayer*)user_data);
}

static bool maniacsofnoise_start(void* user_data, uint32_t subsong, uint32_t sample_rate, NuadeVoices* voices) {
    (void)sample_rate;
    return mon_start((MonPlayer*)user_data, (int)subsong, voices);
}

static NuadeTickResult maniacsofnoise_tick(void* user_data, NuadeVoices* voices) {
    const MonTickResult tick = mon_tick((MonPlayer*)user_data, voices);
    return (NuadeTickResult) { .length = MON_TICK_LENGTH, .flags = tick.ended ? NuadeTick_Ended : NuadeTick_None };
}

static void maniacsofnoise_metadata(void* user_data, const NuadeMetadataSink* sink) {
    (void)user_data;
    sink->set_tag(sink->ctx, NUADE_META_FORMAT, "Maniacs of Noise");
}

static uint32_t maniacsofnoise_voice_info(void* user_data, NuadeVoiceInfo* out, uint32_t cap) {
    const MonPlayer* player = (const MonPlayer*)user_data;
    const uint32_t count = cap < MON_VOICES ? cap : MON_VOICES;
    for (uint32_t i = 0; i < count; i++) {
        const MonVoiceState v = mon_voice_state(player, (int)i);
        out[i] = (NuadeVoiceInfo) { .note = v.note, .instrument = v.instrument };
    }
    return count;
}

static uint32_t maniacsofnoise_sample_count(void* user_data) {
    return ((const MonPlayer*)user_data)->module.instrument_count;
}

static bool maniacsofnoise_sample_get(void* user_data, uint32_t n, NuadeExportSample* out) {
    const int8_t* data;
    uint32_t frames, loop_start, loop_frames;
    if (!mon_instrument_pcm((const MonPlayer*)user_data, n, &data, &frames, &loop_start, &loop_frames)) {
        return false;
    }
    *out = (NuadeExportSample) {
        .index = n, .data = data, .frames = frames, .bits = 8, .loop_start = loop_start, .loop_frames = loop_frames
    };
    return true;
}

const NuadePlayer nuade_player_maniacsofnoise = {
    .api_version = NUADE_PLAYER_API_VERSION,
    .id = "maniacsofnoise",
    .name = "Maniacs of Noise",
    .version = "1",
    .extensions = "mon",
    .probe = maniacsofnoise_probe,
    .open = maniacsofnoise_open,
    .close = maniacsofnoise_close,
    .start = maniacsofnoise_start,
    .tick = maniacsofnoise_tick,
    .metadata = maniacsofnoise_metadata,
    .voice_info = maniacsofnoise_voice_info,
    .sample_count = maniacsofnoise_sample_count,
    .sample_get = maniacsofnoise_sample_get,
};

MonPlayer* mon_from_nuade(NuadeSong* song) {
    if (nuade_player(song) != &nuade_player_maniacsofnoise) {
        return nullptr;
    }
    return (MonPlayer*)nuade_player_data(song);
}
