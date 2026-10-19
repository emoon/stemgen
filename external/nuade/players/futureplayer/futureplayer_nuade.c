// SPDX-License-Identifier: MIT
// Future Player format as a nuade player.

#include <nuade/player.h>

#include "nuade_base.h"
#include <nuade/players/futureplayer.h>
#include <stdio.h>

#define FP_STRING_CAPACITY 256

// The eagleplayer's check (a one-hunk executable with the player's stub) plus a walk of the hunk, the song tables and
// the tracks.
static NuadeProbe futureplayer_probe(const uint8_t* data, size_t size, const char* filename) {
    (void)filename;
    return fp_probe(data, size) ? NuadeProbe_Supported : NuadeProbe_Unsupported;
}

static NuadePlayerOpenResult futureplayer_open(const NuadeSongSource* source) {
    NuadePlayerOpenResult result = { 0 };
    const FpCreateResult created = fp_create(source->data, source->size);
    if (created.player == nullptr) {
        if (created.status == FpStatus_OutOfMemory) {
            result.error.status = NuadeStatus_OutOfMemory;
            snprintf(result.error.message, sizeof(result.error.message), "out of memory");
        } else {
            result.error.status = NuadeStatus_InvalidData;
            snprintf(result.error.message, sizeof(result.error.message), "not a valid Future Player module");
        }
        return result;
    }
    result.user_data = created.player;
    result.info = (NuadeSongInfo) { .voice_count = FP_VOICES,
                                    .subsong_count = (uint32_t)fp_subsong_count(created.player),
                                    .default_subsong = 0,
                                    .timebase_hz = FP_TIMEBASE_HZ };
    return result;
}

static void futureplayer_close(void* user_data) {
    fp_destroy((FpPlayer*)user_data);
}

static bool futureplayer_start(void* user_data, uint32_t subsong, uint32_t sample_rate, NuadeVoices* voices) {
    (void)sample_rate;
    return fp_start((FpPlayer*)user_data, (int)subsong, voices);
}

static NuadeTickResult futureplayer_tick(void* user_data, NuadeVoices* voices) {
    const FpTickResult tick = fp_tick((FpPlayer*)user_data, voices);
    uint32_t flags = NuadeTick_None;
    if (tick.looped) {
        flags |= NuadeTick_Looped;
    }
    if (tick.ended) {
        flags |= NuadeTick_Ended;
    }
    return (NuadeTickResult) { .length = tick.length, .flags = flags };
}

static void futureplayer_metadata(void* user_data, const NuadeMetadataSink* sink) {
    const FpPlayer* player = (const FpPlayer*)user_data;
    char text[FP_STRING_CAPACITY];
    sink->set_tag(sink->ctx, NUADE_META_FORMAT, "Future Player");
    fp_module_string(player, player->module.title, text, sizeof(text));
    if (text[0] != 0) {
        sink->set_tag(sink->ctx, NUADE_META_TITLE, text);
    }
    fp_module_string(player, player->module.author, text, sizeof(text));
    if (text[0] != 0) {
        sink->set_tag(sink->ctx, NUADE_META_ARTIST, text);
    }
    fp_module_string(player, player->module.info, text, sizeof(text));
    if (text[0] != 0) {
        sink->set_tag(sink->ctx, NUADE_META_MESSAGE, text);
    }
}

static uint32_t futureplayer_voice_info(void* user_data, NuadeVoiceInfo* out, uint32_t cap) {
    const FpPlayer* player = (const FpPlayer*)user_data;
    const uint32_t count = cap < FP_VOICES ? cap : FP_VOICES;
    for (uint32_t i = 0; i < count; i++) {
        const FpVoiceState state = fp_voice_state(player, (int)i);
        out[i] = (NuadeVoiceInfo) { .note = state.note, .instrument = state.instrument };
    }
    return count;
}

static uint32_t futureplayer_grid_ticks(void* user_data, uint32_t subsong) {
    return fp_grid_ticks((const FpPlayer*)user_data, (int)subsong);
}

const NuadePlayer nuade_player_futureplayer = {
    .api_version = NUADE_PLAYER_API_VERSION,
    .id = "futureplayer",
    .name = "Future Player",
    .version = "1",
    .extensions = "fp",
    .probe = futureplayer_probe,
    .open = futureplayer_open,
    .close = futureplayer_close,
    .start = futureplayer_start,
    .tick = futureplayer_tick,
    .metadata = futureplayer_metadata,
    .voice_info = futureplayer_voice_info,
    .grid_ticks = futureplayer_grid_ticks,
};

FpPlayer* fp_from_nuade(NuadeSong* song) {
    if (nuade_player(song) != &nuade_player_futureplayer) {
        return nullptr;
    }
    return (FpPlayer*)nuade_player_data(song);
}
