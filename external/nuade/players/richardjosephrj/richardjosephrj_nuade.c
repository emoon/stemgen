// SPDX-License-Identifier: MIT
// Richard Joseph game driver as a nuade player.

#include <nuade/player.h>

#include "nuade_base.h"
#include <nuade/players/richardjosephrj.h>
#include <stdio.h>

// The check of the eagleplayer is the hunk header and the "R.JOSEPH" signature; the player also knows the driver
// code, so what it accepts it can play.
static NuadeProbe richardjosephrj_probe(const uint8_t* data, size_t size, const char* filename) {
    (void)filename;
    return rjg_probe(data, size) ? NuadeProbe_Supported : NuadeProbe_Unsupported;
}

static NuadePlayerOpenResult richardjosephrj_open(const NuadeSongSource* source) {
    NuadePlayerOpenResult result = { 0 };
    const RjgCreateResult created = rjg_create(source->data, source->size);
    if (created.player == nullptr) {
        if (created.status == RjgStatus_OutOfMemory) {
            result.error.status = NuadeStatus_OutOfMemory;
            snprintf(result.error.message, sizeof(result.error.message), "out of memory");
        } else {
            result.error.status = NuadeStatus_InvalidData;
            snprintf(result.error.message, sizeof(result.error.message), "not a known Richard Joseph driver module");
        }
        return result;
    }
    result.user_data = created.player;
    result.info = (NuadeSongInfo) { .voice_count = RJG_VOICES,
                                    .subsong_count = (uint32_t)rjg_subsong_count(created.player),
                                    .default_subsong = 0,
                                    .timebase_hz = RJG_TIMEBASE_HZ };
    return result;
}

static void richardjosephrj_close(void* user_data) {
    rjg_destroy((RjgPlayer*)user_data);
}

static bool richardjosephrj_start(void* user_data, uint32_t subsong, uint32_t sample_rate, NuadeVoices* voices) {
    (void)sample_rate;
    return rjg_start((RjgPlayer*)user_data, (int)subsong, voices);
}

static NuadeTickResult richardjosephrj_tick(void* user_data, NuadeVoices* voices) {
    const RjgTickResult tick = rjg_tick((RjgPlayer*)user_data, voices);
    uint32_t flags = NuadeTick_None;
    if (tick.looped) {
        flags |= NuadeTick_Looped;
    }
    if (tick.ended) {
        flags |= NuadeTick_Ended;
    }
    return (NuadeTickResult) { .length = RJG_TICK_LENGTH, .flags = flags };
}

static void richardjosephrj_metadata(void* user_data, const NuadeMetadataSink* sink) {
    (void)user_data;
    sink->set_tag(sink->ctx, NUADE_META_FORMAT, "Richard Joseph game driver");
}

static uint32_t richardjosephrj_voice_info(void* user_data, NuadeVoiceInfo* out, uint32_t cap) {
    const RjgPlayer* player = (const RjgPlayer*)user_data;
    const uint32_t count = cap < RJG_VOICES ? cap : RJG_VOICES;
    for (uint32_t i = 0; i < count; i++) {
        const RjgVoiceState state = rjg_voice_state(player, (int)i);
        out[i] = (NuadeVoiceInfo) { .note = state.note, .instrument = state.instrument };
    }
    return count;
}

static uint32_t richardjosephrj_grid_ticks(void* user_data, uint32_t subsong) {
    return rjg_grid_ticks((const RjgPlayer*)user_data, (int)subsong);
}

const NuadePlayer nuade_player_richardjosephrj = {
    .api_version = NUADE_PLAYER_API_VERSION,
    .id = "richardjosephrj",
    .name = "Richard Joseph game driver",
    .version = "1",
    .extensions = "rj",
    .probe = richardjosephrj_probe,
    .open = richardjosephrj_open,
    .close = richardjosephrj_close,
    .start = richardjosephrj_start,
    .tick = richardjosephrj_tick,
    .metadata = richardjosephrj_metadata,
    .voice_info = richardjosephrj_voice_info,
    .grid_ticks = richardjosephrj_grid_ticks,
};

RjgPlayer* rjg_from_nuade(NuadeSong* song) {
    if (nuade_player(song) != &nuade_player_richardjosephrj) {
        return nullptr;
    }
    return (RjgPlayer*)nuade_player_data(song);
}
