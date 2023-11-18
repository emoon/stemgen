#include <libopenmpt/libopenmpt.hpp>
#include <libopenmpt/libopenmpt_ext.hpp>

extern "C"
{
    struct SongInfo {
        int num_channels;
        int num_instruments;
        float length_seconds;
    };

    ModInfo get_song_info(unsigned char *buffer, int len, int dump_patterns)
    {
        ModInfo info = { 0, 0, 0.0f };

        try
        {
            openmpt::detail::initial_ctls_map ctls;
            ctls["load.skip_samples"] = "1";
            ctls["load.skip_plugins"] = "1";
            openmpt::module song(buffer, (size_t)len, std::clog, ctls);

            info.num_channels = song.get_num_channels();
            info.num_instruments = song.get_num_instruments();
            info.length_seconds = song.get_duration_seconds();
        }
        catch (const std::exception &e)
        {
            // std::cout << "Cannot open " << filename << ": " << (e.what() ? e.what() : "unknown error") << std::endl;
        }

        return info;
    }
}

