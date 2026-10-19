// SPDX-License-Identifier: MIT
// Dynamic Synthesizer player: the entry points of the player (create, start, tick, display) around the replayer in
// dns_replayer.c, and the loading of the sample file.

#include "dns_internal.h"
#include "nuade_file_index.h"
#include "nuade_path.h"
#include "nuade_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ExtLoad: the sample file is the song's file name with its "dns." prefix changed to "smp.", in the song's directory.

static bool load_samples(DnsSamples* samples, const char* path, const NuadeIo* io) {
    char dir[1024] = { 0 };
    char name[512];
    const char* base = path != nullptr ? nuade_path_basename(path) : nullptr;
    if (!dns_has_module_prefix(base) || io == nullptr || strlen(base) + 1 > sizeof(name)) {
        return false;
    }
    if (!nuade_path_parent(path, dir, sizeof(dir))) {
        snprintf(dir, sizeof(dir), "."); // a bare file name: the current directory
    }
    snprintf(name, sizeof(name), DNS_SAMPLE_PREFIX "%s", base + sizeof(DNS_MODULE_PREFIX) - 1);
    const char* dirs[] = { dir };
    NuadeFileIndex* index = nuade_file_index_create(io, dirs, nuade_sizeof_array(dirs));
    const char* url = index != nullptr ? nuade_file_index_find(index, name) : nullptr;
    bool ok = false;
    if (url != nullptr) {
        const NuadeIoReadResult file = NuadeIo_read_url_to_memory(io, url);
        if (file.data != nullptr) {
            ok = dns_samples_init(samples, file.data, file.size);
            NuadeIo_free_url_to_memory(io, file.data);
        }
    }
    nuade_file_index_destroy(index);
    return ok;
}

DnsCreateResult dns_create(const uint8_t* data, size_t size, const char* path, const NuadeIo* io) {
    DnsPlayer* player = calloc(1, sizeof(*player));
    if (player == nullptr) {
        return (DnsCreateResult) { .player = nullptr, .status = DnsStatus_OutOfMemory };
    }
    DnsStatus status = dns_module_load(&player->module, data, size);
    if (status == DnsStatus_Ok && !load_samples(&player->samples, path, io)) {
        status = DnsStatus_MissingSamples;
    }
    if (status != DnsStatus_Ok) {
        dns_destroy(player);
        return (DnsCreateResult) { .player = nullptr, .status = status };
    }
    return (DnsCreateResult) { .player = player, .status = DnsStatus_Ok };
}

void dns_destroy(DnsPlayer* player) {
    if (player == nullptr) {
        return;
    }
    dns_samples_free(&player->samples);
    dns_module_free(&player->module);
    free(player);
}

int dns_subsong_count(const DnsPlayer* player) {
    return (int)player->module.subsongs;
}

bool dns_start(DnsPlayer* player, int subsong, NuadeVoices* voices) {
    if (player == nullptr || voices == nullptr || subsong < 0 || subsong >= dns_subsong_count(player)) {
        return false;
    }
    dns_replayer_start(player, (unsigned)subsong);
    for (int i = 0; i < DNS_VOICES; i++) {
        nuade_voice_stop(voices, (uint32_t)i);
    }
    return true;
}

DnsTickResult dns_tick(DnsPlayer* player, NuadeVoices* voices) {
    dns_replayer_tick(player);
    for (int i = 0; i < DNS_VOICES; i++) {
        nuade_paula_channel_sync(&player->voices[i].paula, (uint32_t)i, voices);
    }
    // The song start can already have reached the end of the song (a one position song): the first tick reports it.
    const bool looped = player->looped;
    player->looped = false;
    return (DnsTickResult) { .length = player->module.version.tick_length, .looped = looped };
}

uint32_t dns_grid_ticks(const DnsPlayer* player, int subsong) {
    if (player == nullptr || subsong < 0 || subsong >= dns_subsong_count(player)) {
        return 0;
    }
    return dns_read_song(&player->module, (unsigned)subsong).speed;
}

DnsVoiceState dns_voice_state(const DnsPlayer* player, int voice) {
    if (voice < 0 || voice >= DNS_VOICES) {
        return (DnsVoiceState) { .note = -1, .instrument = -1 };
    }
    const DnsVoice* v = &player->voices[voice];
    return (DnsVoiceState) { .note = v->display_note, .instrument = v->display_instrument };
}

uint32_t dns_sample_count(const DnsPlayer* player) {
    return dns_instrument_count(&player->module, player->samples.size);
}

bool dns_sample_pcm(const DnsPlayer* player, uint32_t n, const int8_t** data, uint32_t* length, uint32_t* loop_start,
                    uint32_t* loop_length) {
    if (n >= dns_sample_count(player)) {
        return false;
    }
    const DnsInstrument in = dns_read_instrument(&player->module, n);
    const uint32_t start = in.start & ~1u;
    const uint32_t loop = in.loop & ~1u;
    uint32_t first = (uint32_t)in.start_words * 2;
    if (first == 0) {
        return false;
    }
    *loop_start = 0;
    *loop_length = 0;
    // The sound plays its first run once, then repeats the loop run. It is one sound with a loop when the loop lies in
    // or directly after the first run; a loop elsewhere (the silent run at the start of the samples) is not part of it.
    if (in.loop_words > 1 && loop >= start && loop <= start + first) {
        const uint32_t loop_end = loop + (uint32_t)in.loop_words * 2;
        *loop_start = loop - start;
        *loop_length = (uint32_t)in.loop_words * 2;
        if (loop_end > start + first) {
            first = loop_end - start;
        }
    }
    *data = (const int8_t*)(player->samples.data + start);
    *length = first;
    return true;
}
