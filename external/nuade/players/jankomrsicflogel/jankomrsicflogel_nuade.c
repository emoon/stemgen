// SPDX-License-Identifier: MIT
// Janko Mrsic-Flogel as a nuade player.

#include <nuade/player.h>

#include "nuade_base.h"
#include "nuade_tracker.h"
#include <nuade/players/jankomrsicflogel.h>
#include <stdio.h>

#define JMF_TEXT_BYTES 256

static NuadeProbe jankomrsicflogel_probe(const uint8_t* data, size_t size, const char* filename) {
    (void)filename;
    return jmf_probe(data, size) ? NuadeProbe_Supported : NuadeProbe_Unsupported;
}

static NuadePlayerOpenResult jankomrsicflogel_open(const NuadeSongSource* source) {
    NuadePlayerOpenResult result = { 0 };
    JmfPlayer* player = jmf_create(source->data, source->size);
    if (player == nullptr) {
        result.error.status = NuadeStatus_InvalidData;
        snprintf(result.error.message, sizeof(result.error.message), "not a Janko Mrsic-Flogel song");
        return result;
    }
    result.user_data = player;
    result.info = (NuadeSongInfo) { .voice_count = JMF_VOICES,
                                    .subsong_count = jmf_subsong_count(player),
                                    .default_subsong = 0,
                                    .timebase_hz = JMF_TIMEBASE_HZ };
    return result;
}

static void jankomrsicflogel_close(void* user_data) {
    jmf_destroy((JmfPlayer*)user_data);
}

static bool jankomrsicflogel_start(void* user_data, uint32_t subsong, uint32_t sample_rate, NuadeVoices* voices) {
    (void)sample_rate;
    return jmf_start((JmfPlayer*)user_data, subsong, voices);
}

static NuadeTickResult jankomrsicflogel_tick(void* user_data, NuadeVoices* voices) {
    const JmfTickResult tick = jmf_tick((JmfPlayer*)user_data, voices);
    return (NuadeTickResult) { .length = JMF_TICK_LENGTH, .flags = tick.looped ? NuadeTick_Looped : NuadeTick_None };
}

static void jankomrsicflogel_metadata(void* user_data, const NuadeMetadataSink* sink) {
    const JmfPlayer* player = (const JmfPlayer*)user_data;
    char text[JMF_TEXT_BYTES];
    sink->set_tag(sink->ctx, NUADE_META_FORMAT, "Janko Mrsic-Flogel");
    jmf_text(player, JmfText_Title, text, sizeof(text));
    if (text[0] != '\0') {
        sink->set_tag(sink->ctx, NUADE_META_TITLE, text);
    }
    jmf_text(player, JmfText_Artist, text, sizeof(text));
    if (text[0] != '\0') {
        sink->set_tag(sink->ctx, NUADE_META_ARTIST, text);
    }
    jmf_text(player, JmfText_Message, text, sizeof(text));
    if (text[0] != '\0') {
        sink->set_tag(sink->ctx, NUADE_META_MESSAGE, text);
    }
}

// The waveform a voice plays is its instrument: 1 the first, 2 the second, 3 the fourth voice's own.
static int32_t waveform_number(const JmfPlayer* player, uint32_t voice) {
    const int8_t* lc = player->voices[voice].paula.lc;
    const JmfModule* module = &player->module;
    if (lc == (const int8_t*)module->bytes + module->wave_a) {
        return 1;
    }
    if (lc == (const int8_t*)module->bytes + module->wave_b) {
        return 2;
    }
    return lc == (const int8_t*)module->bytes + module->idle_wave ? 3 : -1;
}

static uint32_t jankomrsicflogel_voice_info(void* user_data, NuadeVoiceInfo* out, uint32_t cap) {
    const JmfPlayer* player = (const JmfPlayer*)user_data;
    const uint32_t n = cap < JMF_VOICES ? cap : JMF_VOICES;
    for (uint32_t i = 0; i < n; i++) {
        out[i] = (NuadeVoiceInfo) { .note = nuade_period_to_note(jmf_voice_period(player, i)),
                                    .instrument = waveform_number(player, i) };
    }
    return n;
}

static uint32_t jankomrsicflogel_grid_ticks(void* user_data, uint32_t subsong) {
    (void)subsong;
    return jmf_grid_ticks((const JmfPlayer*)user_data);
}

// The two waveforms, numbered 1 and 2 as the voices report their instrument (the fourth voice's own never plays).
#define JANKOMRSICFLOGEL_WAVEFORMS 2

static uint32_t jankomrsicflogel_sample_count(void* user_data) {
    (void)user_data;
    return JANKOMRSICFLOGEL_WAVEFORMS;
}

static bool jankomrsicflogel_sample_get(void* user_data, uint32_t n, NuadeExportSample* out) {
    const int8_t* data;
    uint32_t frames;
    if (!jmf_waveform_pcm((const JmfPlayer*)user_data, n + 1, &data, &frames)) {
        return false;
    }
    *out = (NuadeExportSample) { .index = n + 1, .data = data, .frames = frames, .bits = 8, .loop_frames = frames };
    return true;
}

const NuadePlayer nuade_player_jankomrsicflogel = {
    .api_version = NUADE_PLAYER_API_VERSION,
    .id = "jankomrsicflogel",
    .name = "Janko Mrsic-Flogel",
    .version = "1",
    .extensions = "jmf",
    .probe = jankomrsicflogel_probe,
    .open = jankomrsicflogel_open,
    .close = jankomrsicflogel_close,
    .start = jankomrsicflogel_start,
    .tick = jankomrsicflogel_tick,
    .metadata = jankomrsicflogel_metadata,
    .voice_info = jankomrsicflogel_voice_info,
    .grid_ticks = jankomrsicflogel_grid_ticks,
    .sample_count = jankomrsicflogel_sample_count,
    .sample_get = jankomrsicflogel_sample_get,
};

JmfPlayer* jmf_from_nuade(NuadeSong* song) {
    if (nuade_player(song) != &nuade_player_jankomrsicflogel) {
        return nullptr;
    }
    return (JmfPlayer*)nuade_player_data(song);
}
