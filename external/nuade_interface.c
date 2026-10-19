#include <nuade/nuade.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Longest song we measure; a song that neither loops nor ends before this is cut here.
#define MAX_SONG_SECONDS (10 * 60)
#define DURATION_RATE 48000
#define RENDER_BLOCK_FRAMES 1024

typedef struct SongInfo {
    int32_t num_channels;
    int32_t num_instruments;
    float length_seconds;
} SongInfo;

// Has to match the struct on the Rust side (same layout as the libopenmpt interface)
typedef struct RenderParams {
    uint32_t sample_rate;
    uint32_t bytes_per_sample;
    int32_t channel_to_play;
    int32_t instrument_to_play;
    int32_t stereo_separation;
    bool stereo_separation_enabled;
    bool stereo_output;
} RenderParams;

// The voices a stem is made of: the player's own display voices when it has them (the framework sets display_voices to
// voice_count otherwise). nuade_mute_voice numbers voices this way.
static uint32_t stem_voice_count(const NuadeSongInfo* song_info) {
    return song_info->display_voices ? song_info->display_voices : song_info->voice_count;
}

static NuadeSong* open_song(const uint8_t* buffer, uint32_t len, const char* path) {
    NuadeOpenParams params;
    memset(&params, 0, sizeof(params));
    params.path = path;
    params.data = buffer;
    params.size = len;

    NuadeSongResult result = nuade_open_ex(&params);
    return result.song;
}

// Some formats start on a silent or empty subsong (a "stop" or placeholder), so the default would give an empty
// render. Use the default unless it is silent for the first seconds, then the first subsong that isn't.
#define PROBE_SECONDS 10

static bool subsong_audible(NuadeSong* song, uint32_t subsong) {
    if (!nuade_start(song, subsong, DURATION_RATE)) {
        return false;
    }

    float block[RENDER_BLOCK_FRAMES * 2];
    uint64_t frames = 0;
    while (frames < (uint64_t)PROBE_SECONDS * DURATION_RATE) {
        NuadeRenderResult r = nuade_render(song, block, NULL, RENDER_BLOCK_FRAMES);
        for (uint32_t i = 0; i < r.frames * 2; ++i) {
            if (block[i] != 0.0f) {
                return true;
            }
        }
        frames += r.frames;
        if ((r.flags & NuadeRenderFlags_Ended) || r.frames == 0) {
            break;
        }
    }
    return false;
}

static uint32_t pick_subsong(NuadeSong* song) {
    const NuadeSongInfo* info = nuade_info(song);
    if (subsong_audible(song, info->default_subsong)) {
        return info->default_subsong;
    }
    for (uint32_t n = 0; n < info->subsong_count; ++n) {
        if (n != info->default_subsong && subsong_audible(song, n)) {
            return n;
        }
    }
    return info->default_subsong;
}

SongInfo get_song_info_nuade(const uint8_t* buffer, uint32_t len, const char* path) {
    SongInfo info = {0, 0, 0.0f};

    NuadeSong* song = open_song(buffer, len, path);
    if (!song) {
        return info;
    }

    const NuadeSongInfo* song_info = nuade_info(song);
    info.num_channels = (int32_t)stem_voice_count(song_info);
    info.num_instruments = (int32_t)nuade_instrument_count(song);

    NuadeDuration duration;
    if (nuade_duration(song, pick_subsong(song), DURATION_RATE, (uint64_t)MAX_SONG_SECONDS * DURATION_RATE,
                       &duration)) {
        info.length_seconds = (float)((double)duration.frames / DURATION_RATE);
    }

    nuade_close(song);
    return info;
}

// Fills `out` with the format's number for each instrument the player reports (up to `cap`), returns how many exist.
uint32_t get_instrument_ids_nuade(const uint8_t* buffer, uint32_t len, const char* path, uint32_t* out, uint32_t cap) {
    NuadeSong* song = open_song(buffer, len, path);
    if (!song) {
        return 0;
    }

    uint32_t count = nuade_instrument_count(song);
    for (uint32_t n = 0; n < count && n < cap; ++n) {
        nuade_instrument_name(song, n, &out[n]);
    }

    nuade_close(song);
    return count;
}

// Leaves only the requested voice/instrument audible. The instrument is the number the format gives it (see
// get_instrument_ids_nuade). False if the player can't be muted that way.
static bool apply_solo(NuadeSong* song, const RenderParams* params) {
    if (params->channel_to_play >= 0) {
        uint32_t voice_count = stem_voice_count(nuade_info(song));
        for (uint32_t i = 0; i < voice_count; ++i) {
            if (!nuade_mute_voice(song, i, i != (uint32_t)params->channel_to_play)) {
                return false;
            }
        }
    }

    if (params->instrument_to_play >= 0) {
        uint32_t count = nuade_instrument_count(song);
        for (uint32_t n = 0; n < count; ++n) {
            uint32_t index = 0;
            nuade_instrument_name(song, n, &index);
            if (!nuade_mute_instrument(song, index, index != (uint32_t)params->instrument_to_play)) {
                return false;
            }
        }
    }

    return true;
}

static int16_t to_i16(float v) {
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return (int16_t)lrintf(v * 32767.0f);
}

uint32_t song_render_nuade(uint8_t* output, uint32_t output_len, const uint8_t* input, uint32_t len, const char* path,
                           const RenderParams* params) {
    NuadeSong* song = open_song(input, len, path);
    if (!song) {
        return 0;
    }

    uint32_t out_channels = params->stereo_output ? 2 : 1;
    uint32_t frame_bytes = out_channels * params->bytes_per_sample;
    uint64_t capacity = output_len / frame_bytes;
    uint64_t written = 0;

    uint32_t subsong = pick_subsong(song); // before any mute, a muted probe would look silent

    // A solo that can't be applied would render the whole song as a stem, so render nothing instead.
    bool ok = apply_solo(song, params);
    if (ok && params->stereo_separation_enabled) {
        ok = nuade_set_stereo_separation(song, (float)params->stereo_separation / 100.0f);
    }
    ok = ok && nuade_start(song, subsong, params->sample_rate);

    if (ok) {
        nuade_set_repeat_count(song, 1);

        float block[RENDER_BLOCK_FRAMES * 2];
        bool ended = false;

        while (!ended && written < capacity) {
            uint32_t want = RENDER_BLOCK_FRAMES;
            if (capacity - written < want) {
                want = (uint32_t)(capacity - written);
            }

            NuadeRenderResult r = nuade_render(song, block, NULL, want);
            ended = (r.flags & NuadeRenderFlags_Ended) != 0;

            for (uint32_t i = 0; i < r.frames; ++i) {
                float l = block[i * 2];
                float rr = block[i * 2 + 1];
                float samples[2] = {l, rr};
                if (!params->stereo_output) {
                    samples[0] = (l + rr) * 0.5f;
                }

                uint8_t* dst = output + (written + i) * frame_bytes;
                for (uint32_t c = 0; c < out_channels; ++c) {
                    if (params->bytes_per_sample == 2) {
                        int16_t v = to_i16(samples[c]);
                        memcpy(dst + c * 2, &v, 2);
                    } else {
                        memcpy(dst + c * 4, &samples[c], 4);
                    }
                }
            }
            written += r.frames;
        }
    }

    nuade_close(song);
    return (uint32_t)(written * frame_bytes);
}

// A sound handed to the SampleCallback; the data is only valid during the call.
typedef struct SampleDesc {
    uint32_t index;
    const char* name;
    const void* data;
    uint32_t frames;
    uint32_t bits;
    uint32_t rate; // 0 = not known
    uint32_t loop_start;
    uint32_t loop_frames;
} SampleDesc;

typedef void (*SampleCallback)(void* ctx, const SampleDesc* sample);

typedef struct SampleExportInfo {
    bool supported;           // false: the player generates its sounds at run time, there is nothing to export
    uint32_t total;           // sounds the format holds
    uint32_t skipped;         // of those, how many have no PCM (synth instruments and such)
    char player_name[64];
} SampleExportInfo;

// True if every byte of the sound is zero: an empty slot of the format (the 1-word dummy, a blank placeholder). It is
// silence in 8 and 16 bit alike, so it is checked as bytes.
static bool is_silent(const NuadeExportSample* sample) {
    const uint8_t* bytes = (const uint8_t*)sample->data;
    size_t size = (size_t)sample->frames * (sample->bits / 8);
    for (size_t i = 0; i < size; ++i) {
        if (bytes[i] != 0) {
            return false;
        }
    }
    return true;
}

// Hands every exportable sound of the song to `callback`. A silent one is left out like one without PCM.
SampleExportInfo export_samples_nuade(const uint8_t* buffer, uint32_t len, const char* path, SampleCallback callback,
                                      void* ctx) {
    SampleExportInfo info;
    memset(&info, 0, sizeof(info));

    NuadeSong* song = open_song(buffer, len, path);
    if (!song) {
        return info;
    }

    strncpy(info.player_name, nuade_player(song)->name, sizeof(info.player_name) - 1);
    info.supported = nuade_samples_supported(song);
    info.total = nuade_sample_count(song);

    for (uint32_t n = 0; n < info.total; ++n) {
        NuadeExportSample sample;
        if (!nuade_sample_get(song, n, &sample) || is_silent(&sample)) {
            info.skipped++;
            continue;
        }

        SampleDesc desc = {sample.index,  sample.name,       sample.data,       sample.frames,
                           sample.bits,   sample.rate,       sample.loop_start, sample.loop_frames};
        callback(ctx, &desc);
    }

    nuade_close(song);
    return info;
}
