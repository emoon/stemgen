// SPDX-License-Identifier: MIT
// Wally Beben: the player's lifecycle around the replayer, as the eagleplayer's glue drives it.
//
// The eagleplayer's InitSound requests the song and runs the module's reset routine; its interrupt calls the module's
// tick routine 50 times a second. UADE plays on past SongEnd, so a song that reaches its end simply goes on.

#include "wb_replay.h"

#include "nuade_base.h"
#include <stdlib.h>
#include <string.h>

// Loads the module and proves that its replay never leaves what the port models; both draw on one work budget.
static bool open_module(WbModule* module, const uint8_t* data, size_t size) {
    WbBudget budget = { .left = WB_WORK_BUDGET };
    if (!wb_module_load(module, data, size, &budget)) {
        return false;
    }
    if (!wb_replay_prove(module, &budget)) {
        wb_module_free(module);
        return false;
    }
    return true;
}

bool wb_probe(const uint8_t* data, size_t size) {
    WbModule module;
    if (!open_module(&module, data, size)) {
        return false;
    }
    wb_module_free(&module);
    return true;
}

WbPlayer* wb_create(const uint8_t* data, size_t size) {
    WbPlayer* player = calloc(1, sizeof(*player));
    if (player == nullptr) {
        return nullptr;
    }
    if (!open_module(&player->module, data, size)) {
        free(player);
        return nullptr;
    }
    player->sample_ptr = calloc(player->module.sample_count, sizeof(uint32_t));
    if (player->sample_ptr == nullptr) {
        wb_module_free(&player->module);
        free(player);
        return nullptr;
    }
    return player;
}

void wb_destroy(WbPlayer* player) {
    if (player != nullptr) {
        free(player->sample_ptr);
        wb_module_free(&player->module);
        free(player);
    }
}

uint32_t wb_subsong_count(const WbPlayer* player) {
    return player->module.subsong_count;
}

const char* wb_version_name(const WbPlayer* player) {
    return player->module.version->name;
}

bool wb_start(WbPlayer* player, uint32_t subsong, NuadeVoices* voices) {
    if (player == nullptr || subsong >= player->module.subsong_count) {
        return false;
    }
    memset(player->paula, 0, sizeof(player->paula));
    wb_replay_init(&player->replay, &player->module, subsong + 1, player->paula, player->sample_ptr);
    for (uint32_t v = 0; v < WB_VOICES; v++) {
        if (voices != nullptr) {
            nuade_voice_stop(voices, v);
        }
    }
    return true;
}

WbTickResult wb_tick(WbPlayer* player, NuadeVoices* voices) {
    wb_replay_tick(&player->replay);
    for (uint32_t v = 0; v < WB_VOICES; v++) {
        nuade_paula_channel_sync(&player->paula[v], v, voices);
    }
    return (WbTickResult) { .looped = player->replay.song_end,
                            .ended = player->replay.stopped || player->replay.fault };
}

WbVoiceState wb_voice_state(const WbPlayer* player, uint32_t voice) {
    return (WbVoiceState) { .period = player->replay.last_period[voice],
                            .instrument = player->replay.last_instrument[voice] };
}

uint32_t wb_instrument_count(const WbPlayer* player) {
    // an instrument is named by a byte offset into the table
    const uint32_t reachable = (UINT8_MAX + 1) / WB_INSTRUMENT_BYTES;
    return player->module.instrument_count < reachable ? player->module.instrument_count : reachable;
}

bool wb_instrument_pcm(const WbPlayer* player, uint32_t index, const int8_t** data, uint32_t* length,
                       uint32_t* loop_start, uint32_t* loop_length) {
    const WbModule* m = &player->module;
    WbInstrument ins;
    if (index >= wb_instrument_count(player) || !wb_module_instrument(m, (uint8_t)(index * WB_INSTRUMENT_BYTES), &ins)
        || ins.sample >= m->sample_count) {
        return false;
    }
    const uint32_t start = m->sample_ptr[ins.sample];
    // the first block the note plays, then the repeat; the builds with a silent repeat have none for flags 0
    const bool first_uses_loop = ins.flags != 0 && !m->version->quirks.first_block_ignores_flags;
    uint32_t bytes = first_uses_loop ? (uint32_t)ins.loop_offset + ins.loop_length : ins.length;
    const bool silent_repeat = m->version->quirks.loop_mode == WbLoop_FlagSilence && ins.flags == 0;
    // a repeat of a word or less is the "no loop" of the Amiga
    const bool loops = !silent_repeat && ins.loop_length > 2;
    const uint32_t loop_end = (uint32_t)ins.loop_offset + ins.loop_length;
    if (loops && loop_end > bytes) {
        bytes = loop_end;
    }
    if (start == WB_BAD_POINTER || start >= m->size || bytes == 0) {
        return false;
    }
    if (bytes > m->size - start) {
        bytes = m->size - start;
    }
    *data = (const int8_t*)(m->image + start);
    *length = bytes;
    *loop_start = 0;
    *loop_length = 0;
    if (loops && loop_end <= bytes) {
        *loop_start = ins.loop_offset;
        *loop_length = ins.loop_length;
    }
    return true;
}
