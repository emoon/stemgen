#include <libopenmpt/libopenmpt.hpp>
#include <libopenmpt/libopenmpt_ext.hpp>
#include <stdint.h>

struct SongInfo {
    int num_channels;
    int num_instruments;
    float length_seconds;
};

extern "C"
{

SongInfo get_song_info_c(const uint8_t* buffer, uint32_t len) {
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
    }
    catch (const std::exception&)
    {
    }

    return info;
}

uint32_t song_render_c(
    uint8_t* output, uint32_t output_len, 
    const uint8_t* input, uint32_t len, 
    uint32_t sample_rate, 
    uint32_t bytes_per_sample, // 2 for 16 bit, 4 for floats
    int32_t channel_to_play, // if -1 use all channels, otherwise pick one channel
    int32_t instrument_to_play, // if -1 use all instruments, otherwise pick one
    bool stereo_output) 
{
    try
    {
        openmpt::detail::initial_ctls_map ctls;
        openmpt::module_ext song(input, (size_t)len, std::clog, ctls);
        int16_t* output_16bit = (int16_t*)output;
        float* output_float = (float*)output;
        uint32_t samples_generated = 0;

        int num_channels = song.get_num_channels();
        int instrument_count = song.get_num_instruments();

        // Some formats doesn't have instruments (mod) so we assume samples is the same as amount of instruments in that case
        if (instrument_count == 0) {
            instrument_count = song.get_num_samples();
        }

        openmpt::ext::interactive* interactive = static_cast<openmpt::ext::interactive*>(song.get_interface(openmpt::ext::interactive_id));

        if (channel_to_play >= 0 && interactive != nullptr) {
            // Deactivate all channels execpt the one we care about
            for (int i = 0; i < num_channels; ++i) {
                if (i == channel_to_play)
                    interactive->set_channel_mute_status(i, false);
                else
                    interactive->set_channel_mute_status(i, true);
            }
        }

        if (instrument_to_play >= 0 && interactive) {
            // Deactivate all channels execpt the one we care about
            for (int i = 0; i < instrument_count; ++i) {
                if (i == instrument_to_play) {
                    interactive->set_instrument_mute_status(i, false);
                } else {
                    interactive->set_instrument_mute_status(i, true);
                }
            }
        }

        if (bytes_per_sample == 2) {
            for (uint32_t i = 0; i < output_len; i += sample_rate) {
                uint32_t gen_count = 0;

                if (stereo_output) {
                    gen_count = (uint32_t)song.read_interleaved_stereo(sample_rate, sample_rate, output_16bit);
                    output_16bit += sample_rate * 2;
                }
                else {
                    gen_count = (uint32_t)song.read(sample_rate, sample_rate, output_16bit);
                    output_16bit += sample_rate;
                }

                samples_generated += gen_count;

                // if we don't get the number of samples we requested we are at the end
                if (gen_count != sample_rate)
                    break;
            }
        } else {
            for (uint32_t i = 0; i < output_len; i += sample_rate) {
                uint32_t gen_count = 0;

                if (stereo_output) {
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

        if (stereo_output)
            return samples_generated * 2 * bytes_per_sample;
        else
            return samples_generated * bytes_per_sample;
    }
    catch (const std::exception& e)
    {
    }

    return 0;
}

}

