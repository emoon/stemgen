#include <libopenmpt/libopenmpt.hpp>
#include <libopenmpt/libopenmpt_ext.hpp>
#include <stdafx.h>
#include <soundlib/Sndfile.h>
#include <stdint.h>
#include <iostream>
#include <fstream>

enum SampleType {
    SampleType_Wav,
    SampleType_Flac,
};

struct SongInfo {
    int num_channels;
    int num_instruments;
    float length_seconds;
};

// Has to match the struct on the Rust size 
struct RenderParams {
    uint32_t sample_rate;
    uint32_t bytes_per_sample;
    int32_t channel_to_play;
    int32_t instrument_to_play;
    int stereo_separation;
    bool stereo_separation_enabled;
    bool stereo_output;
};

enum SampleFormat {
    SampleFormat_Flac,
    SampleFormat_Wav,
};

extern "C"
{

SongInfo get_song_info_c(const uint8_t* buffer, uint32_t len, const char* output_with_stem, int sample_format) {
    SongInfo info = { 0, 0, 0.0f };

    try
    {
        openmpt::detail::initial_ctls_map ctls;
        ctls["load.skip_plugins"] = "1";
        openmpt::module song(buffer, (size_t)len, std::clog, ctls);

        info.num_channels = song.get_num_channels();
        info.num_instruments = song.get_num_instruments();

        // Some formats doesn't have instruments (such as mod)
        // so we assume num samples is the same as amount of instruments
        // in that case
        if (info.num_instruments == 0) {
            info.num_instruments = song.get_num_samples();
        }

        info.length_seconds = (float)song.get_duration_seconds();

        if (!output_with_stem) 
            return info;

        OpenMPT::CSoundFile* sf = song.get_snd_file();

        int num_samples = sf->GetNumSamples();

        for (int i = 1; i < num_samples + 1; ++i) {
            char name[4096];
            if (sample_format == SampleFormat_Flac) {
                sprintf(name, "%s_sample_%04d.flac", output_with_stem, i);
                std::ofstream f(name, std::ios::binary);
                if (!sf->SaveFLACSample(i, f)) {
                    printf("Failed to write sample: %s\n", name);
                }
            } else {
                sprintf(name, "%s_sample_%04d.wav", output_with_stem, i);
                std::ofstream f(name, std::ios::binary);
                if (!sf->SaveWAVSample(i, f)) {
                    printf("Failed to write sample: %s\n", name);
                }
            }
        }

        /*
        for (int i = 1; i < info.num_instruments + 1; ++i) {
            char name[4096];
            sprintf(name, "%s_song_inst_%04d.sfz", output_with_stem, i);
            std::ofstream f(name);
            OpenMPT::mpt::PathString mpt_string = OpenMPT::mpt::PathString::FromUTF8(name); 
            sf->SaveSFZInstrument(i, f, mpt_string, false);
        }
        */
    }
    catch (const std::exception&)
    {
    }

    return info;
}

uint32_t song_render_c(
    uint8_t* output, uint32_t output_len, 
    const uint8_t* input, uint32_t len, 
    RenderParams& params)
{
    try
    {
        openmpt::detail::initial_ctls_map ctls;
        ctls["play.at_end"] = "stop";
        openmpt::module_ext song(input, (size_t)len, std::clog, ctls);
        int16_t* output_16bit = (int16_t*)output;
        float* output_float = (float*)output;
        uint32_t samples_generated = 0;
        uint32_t sample_rate = params.sample_rate;

        int num_channels = song.get_num_channels();
        int instrument_count = song.get_num_instruments();

        // Some formats doesn't have instruments (mod) so we assume samples is the same as amount of instruments in that case
        if (instrument_count == 0) {
            instrument_count = song.get_num_samples();
        }

        if (params.stereo_separation_enabled) {
            song.set_render_param(openmpt::module::RENDER_STEREOSEPARATION_PERCENT, params.stereo_separation);
        }

        openmpt::ext::interactive* interactive = static_cast<openmpt::ext::interactive*>(song.get_interface(openmpt::ext::interactive_id));
        openmpt::ext::interactive2* interactive2 = static_cast<openmpt::ext::interactive2*>(song.get_interface(openmpt::ext::interactive2_id));

        if (params.channel_to_play >= 0 && interactive != nullptr) {
            // Deactivate all channels execpt the one we care about
            for (int i = 0; i < num_channels; ++i) {
                if (i == params.channel_to_play) {
                    interactive->set_channel_mute_status(i, false);
                } else { 
                    interactive->set_channel_mute_status(i, true);
                }
            }
        }

        if (params.instrument_to_play >= 0 && interactive && interactive2) {
            // Deactivate all channels execpt the one we care about
            for (int i = 0; i < instrument_count; ++i) {
                if (i == params.instrument_to_play) {
                    interactive->set_instrument_mute_status(i, false);
                } else {
                    interactive->set_instrument_mute_status(i, true);
                }
            }
        }

        if (params.bytes_per_sample == 2) {
            for (uint32_t i = 0; i < output_len; i += sample_rate) {
                uint32_t gen_count = 0;

                if (params.stereo_output) {
                    gen_count = (uint32_t)song.read_interleaved_stereo(sample_rate, sample_rate, output_16bit);
                    output_16bit += sample_rate * 2;
                }
                else {
                    gen_count = (uint32_t)song.read(sample_rate, sample_rate, output_16bit);
                    output_16bit += params.sample_rate;
                }

                samples_generated += gen_count;

                // if we don't get the number of samples we requested we are at the end
                if (gen_count != sample_rate)
                    break;
            }
        } else {
            for (uint32_t i = 0; i < output_len; i += sample_rate) {
                uint32_t gen_count = 0;

                if (params.stereo_output) {
                    gen_count = (uint32_t)song.read_interleaved_stereo(sample_rate, sample_rate, output_float);
                    output_float += sample_rate * 2;
                }
                else {
                    gen_count = (uint32_t)song.read(sample_rate, sample_rate, output_float);
                    output_float += sample_rate;
                }

                samples_generated += gen_count;

                // if we don't get the number of samples we requested we are at the end
                if (gen_count != sample_rate)
                    break;
            }
        }

	    //bool SaveSFZInstrument(INSTRUMENTINDEX nInstr, std::ostream &f, const mpt::PathString &filename, bool useFLACsamples) const;


        if (params.stereo_output)
            return samples_generated * 2 * params.bytes_per_sample;
        else
            return samples_generated * params.bytes_per_sample;
    }
    catch (const std::exception& e)
    {
    }

    return 0;
}

}

