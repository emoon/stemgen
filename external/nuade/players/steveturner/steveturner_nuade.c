// SPDX-License-Identifier: MIT
// Steve Turner format as a nuade player.

#include <nuade/player.h>

#include "nuade_base.h"
#include <nuade/players/steveturner.h>
#include <stdio.h>

// The module is 68k code with fixed table positions; the check of the eagleplayer is specific enough to be sure,
// and the probe walks everything the songs reach.
static NuadeProbe steveturner_probe(const uint8_t* data, size_t size, const char* filename) {
    (void)filename;
    return st_probe(data, size) ? NuadeProbe_Supported : NuadeProbe_Unsupported;
}

static NuadePlayerOpenResult steveturner_open(const NuadeSongSource* source) {
    NuadePlayerOpenResult result = { 0 };
    const StCreateResult created = st_create(source->data, source->size);
    if (created.player == nullptr) {
        if (created.status == StStatus_OutOfMemory) {
            result.error.status = NuadeStatus_OutOfMemory;
            snprintf(result.error.message, sizeof(result.error.message), "out of memory");
        } else {
            result.error.status = NuadeStatus_InvalidData;
            snprintf(result.error.message, sizeof(result.error.message), "not a valid Steve Turner module");
        }
        return result;
    }
    result.user_data = created.player;
    result.info = (NuadeSongInfo) { .voice_count = ST_VOICES,
                                    .subsong_count = (uint32_t)st_subsong_count(created.player),
                                    .default_subsong = 0,
                                    .timebase_hz = ST_TIMEBASE_HZ };
    return result;
}

static void steveturner_close(void* user_data) {
    st_destroy((StPlayer*)user_data);
}

static bool steveturner_start(void* user_data, uint32_t subsong, uint32_t sample_rate, NuadeVoices* voices) {
    (void)sample_rate;
    return st_start((StPlayer*)user_data, (int)subsong, voices);
}

static NuadeTickResult steveturner_tick(void* user_data, NuadeVoices* voices) {
    const StTickResult tick = st_tick((StPlayer*)user_data, voices);
    uint32_t flags = NuadeTick_None;
    if (tick.looped) {
        flags |= NuadeTick_Looped;
    }
    if (tick.ended) {
        flags |= NuadeTick_Ended;
    }
    return (NuadeTickResult) { .length = ST_TICK_LENGTH, .flags = flags };
}

static void steveturner_metadata(void* user_data, const NuadeMetadataSink* sink) {
    (void)user_data;
    sink->set_tag(sink->ctx, NUADE_META_FORMAT, "Steve Turner");
}

static uint32_t steveturner_voice_info(void* user_data, NuadeVoiceInfo* out, uint32_t cap) {
    const StPlayer* player = (const StPlayer*)user_data;
    const uint32_t count = cap < ST_VOICES ? cap : ST_VOICES;
    for (uint32_t i = 0; i < count; i++) {
        const StVoiceState state = st_voice_state(player, (int)i);
        out[i] = (NuadeVoiceInfo) { .note = state.note, .instrument = state.instrument };
    }
    return count;
}

static uint32_t steveturner_grid_ticks(void* user_data, uint32_t subsong) {
    return st_grid_ticks((const StPlayer*)user_data, (int)subsong);
}

static uint32_t steveturner_sample_count(void* user_data) {
    return st_instrument_count((const StPlayer*)user_data);
}

static bool steveturner_sample_get(void* user_data, uint32_t n, NuadeExportSample* out) {
    const StPlayer* player = (const StPlayer*)user_data;
    const int8_t* data;
    uint32_t frames, loop_start, loop_frames;
    if (!st_instrument_pcm(player, n, &data, &frames, &loop_start, &loop_frames)) {
        return false;
    }
    *out = (NuadeExportSample) {
        .index = n, .data = data, .frames = frames, .bits = 8, .loop_start = loop_start, .loop_frames = loop_frames
    };
    return true;
}

const NuadePlayer nuade_player_steveturner = {
    .api_version = NUADE_PLAYER_API_VERSION,
    .id = "steveturner",
    .name = "Steve Turner",
    .version = "1",
    .extensions = "jpo,jpold",
    .probe = steveturner_probe,
    .open = steveturner_open,
    .close = steveturner_close,
    .start = steveturner_start,
    .tick = steveturner_tick,
    .metadata = steveturner_metadata,
    .voice_info = steveturner_voice_info,
    .grid_ticks = steveturner_grid_ticks,
    .sample_count = steveturner_sample_count,
    .sample_get = steveturner_sample_get,
};

StPlayer* st_from_nuade(NuadeSong* song) {
    if (nuade_player(song) != &nuade_player_steveturner) {
        return nullptr;
    }
    return (StPlayer*)nuade_player_data(song);
}
