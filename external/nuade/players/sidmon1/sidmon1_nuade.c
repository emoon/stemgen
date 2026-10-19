// SPDX-License-Identifier: MIT
// SidMon 1.0 as a nuade player.

#include <nuade/player.h>

#include "nuade_base.h"
#include "nuade_tracker.h"
#include <nuade/players/sidmon1.h>
#include <stdio.h>

static NuadeProbe sidmon1_probe(const uint8_t* data, size_t size, const char* filename) {
    (void)filename;
    return sd1_probe(data, size) ? NuadeProbe_Supported : NuadeProbe_Unsupported;
}

static NuadePlayerOpenResult sidmon1_open(const NuadeSongSource* source) {
    NuadePlayerOpenResult result = { 0 };
    const Sd1CreateResult created = sd1_create(source->data, source->size);
    switch (created.status) {
        case Sd1Status_Ok:
            result.user_data = created.player;
            result.info = (NuadeSongInfo) {
                .voice_count = SD1_VOICES, .subsong_count = 1, .default_subsong = 0, .timebase_hz = SD1_TIMEBASE_HZ
            };
            break;
        case Sd1Status_OutOfMemory:
            result.error.status = NuadeStatus_OutOfMemory;
            snprintf(result.error.message, sizeof(result.error.message), "out of memory");
            break;
        case Sd1Status_InvalidData:
            result.error.status = NuadeStatus_InvalidData;
            snprintf(result.error.message, sizeof(result.error.message), "not a valid SidMon 1.0 module");
            break;
    }
    return result;
}

static void sidmon1_close(void* user_data) {
    sd1_destroy((Sd1Player*)user_data);
}

static bool sidmon1_start(void* user_data, uint32_t subsong, uint32_t sample_rate, NuadeVoices* voices) {
    (void)sample_rate;
    return subsong == 0 && sd1_start((Sd1Player*)user_data, voices);
}

static NuadeTickResult sidmon1_tick(void* user_data, NuadeVoices* voices) {
    const Sd1TickResult tick = sd1_tick((Sd1Player*)user_data, voices);
    return (NuadeTickResult) { .length = SD1_TICK_LENGTH, .flags = tick.looped ? NuadeTick_Looped : NuadeTick_None };
}

static void sidmon1_metadata(void* user_data, const NuadeMetadataSink* sink) {
    const Sd1Player* player = (const Sd1Player*)user_data;
    sink->set_tag(sink->ctx, NUADE_META_FORMAT, "SidMon 1.0");
    char name[SD1_SAMPLE_NAME_SIZE + 1];
    for (int i = 1; i <= sd1_instrument_count(player); i++) {
        if (!sd1_sample_name(player, i, name)) {
            snprintf(name, sizeof(name), "Synth %d", i);
        }
        sink->add_instrument(sink->ctx, (uint32_t)(i - 1), name);
    }
}

static NuadePositionResult sidmon1_position(void* user_data) {
    const Sd1Position p = sd1_position((const Sd1Player*)user_data);
    return (NuadePositionResult) { .valid = true,
                                   .position = { .mode = NuadeScroll_Rows,
                                                 .order = (uint32_t)p.order,
                                                 .row = (uint32_t)p.row,
                                                 .rows = (uint32_t)p.rows,
                                                 .speed = (uint32_t)p.speed } };
}

static uint32_t sidmon1_voice_info(void* user_data, NuadeVoiceInfo* out, uint32_t cap) {
    const Sd1Player* player = (const Sd1Player*)user_data;
    const Sd1Position p = sd1_position(player);
    const uint32_t n = cap < SD1_VOICES ? cap : SD1_VOICES;
    for (uint32_t i = 0; i < n; i++) {
        const Sd1VoiceState v = sd1_voice_state(player, (int)i);
        out[i] = (NuadeVoiceInfo) { .note = v.note,
                                    .instrument = v.instrument > 0 ? v.instrument - 1 : -1,
                                    .order = (uint32_t)p.order,
                                    .row = (uint32_t)p.row };
    }
    return n;
}

static int32_t sidmon1_note(const Sd1Player* player, int note) {
    if (note == 0) {
        return NUADE_CELL_EMPTY;
    }
    if (note == SD1_NOTE_RELEASE) {
        return NUADE_NOTE_OFF;
    }
    return sd1_note_semitone(player, note);
}

static NuadeTrackerInfoResult sidmon1_tracker_info(void* user_data, const NuadePosition* at) {
    const Sd1Player* player = (const Sd1Player*)user_data;
    if ((int)at->order >= sd1_order_count(player)) {
        return (NuadeTrackerInfoResult) { 0 };
    }
    return (NuadeTrackerInfoResult) { .valid = true,
                                      .info = { .channels = SD1_VOICES,
                                                .rows = (uint32_t)sd1_position(player).rows,
                                                .column_count = 4,
                                                .columns = { { "note", NuadeColumn_Note, 3, SD1_NOTE_OCTAVE_SHIFT },
                                                             { "inst", NuadeColumn_Hex, 2, 0 },
                                                             { "fx", NuadeColumn_Hex, 2, 0 },
                                                             { "arg", NuadeColumn_Hex, 2, 0 } } } };
}

static NuadePatternCell sidmon1_cell(const void* ctx, uint32_t order, uint32_t channel, uint32_t row) {
    NuadePatternCell cell = nuade_cell_empty();
    const Sd1Player* player = (const Sd1Player*)ctx;
    const Sd1Cell c = sd1_pattern_cell(player, (int)order, (int)channel, (int)row);
    cell.value[0] = sidmon1_note(player, c.note);
    if (c.instrument != 0) {
        cell.value[1] = c.instrument;
    }
    if (c.effect != 0 || c.parameter != 0) {
        cell.value[2] = c.effect;
        cell.value[3] = c.parameter;
    }
    return cell;
}

static uint32_t sidmon1_tracker_cells(void* user_data, const NuadePosition* at, uint32_t channel, int32_t row_lo,
                                      int32_t row_hi, NuadePatternCell* out, uint32_t cap) {
    const Sd1Player* player = (const Sd1Player*)user_data;
    if ((int)at->order >= sd1_order_count(player)) {
        return 0;
    }
    return nuade_tracker_fill(at, SD1_VOICES, (uint32_t)sd1_position(player).rows, channel, row_lo, row_hi, out, cap,
                              sidmon1_cell, user_data);
}

static uint32_t sidmon1_sample_count(void* user_data) {
    return (uint32_t)sd1_exportable_count((const Sd1Player*)user_data);
}

static bool sidmon1_sample_get(void* user_data, uint32_t n, NuadeExportSample* out) {
    const int8_t* data;
    uint32_t length, loop_start, loop_length;
    if (!sd1_instrument_pcm((const Sd1Player*)user_data, (int)n + 1, &data, &length, &loop_start, &loop_length)) {
        return false;
    }
    *out = (NuadeExportSample) {
        .index = n, .data = data, .frames = length, .bits = 8, .loop_start = loop_start, .loop_frames = loop_length
    };
    return true;
}

const NuadePlayer nuade_player_sidmon1 = {
    .api_version = NUADE_PLAYER_API_VERSION,
    .id = "sidmon1",
    .name = "SidMon 1.0",
    .version = "1",
    .extensions = "sid,sid1,sd1",
    .probe = sidmon1_probe,
    .open = sidmon1_open,
    .close = sidmon1_close,
    .start = sidmon1_start,
    .tick = sidmon1_tick,
    .metadata = sidmon1_metadata,
    .position = sidmon1_position,
    .voice_info = sidmon1_voice_info,
    .tracker_info = sidmon1_tracker_info,
    .tracker_cells = sidmon1_tracker_cells,
    .sample_count = sidmon1_sample_count,
    .sample_get = sidmon1_sample_get,
};

Sd1Player* sd1_from_nuade(NuadeSong* song) {
    if (nuade_player(song) != &nuade_player_sidmon1) {
        return nullptr;
    }
    return (Sd1Player*)nuade_player_data(song);
}
