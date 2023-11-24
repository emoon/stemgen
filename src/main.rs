use anyhow::Result;
use clap::{Parser, ValueEnum};
use indicatif::{ProgressBar, ProgressStyle};
use rayon::prelude::*;
use simple_logger::SimpleLogger;
use std::{fs::File, io::{Read, Write}, path::Path, path::PathBuf};
use vorbis_rs::{VorbisEncoderBuilder, VorbisBitrateManagementStrategy};
use mp3lame_encoder::{InterleavedPcm, MonoPcm, Builder, FlushNoGap};
use walkdir::WalkDir;
use wav;

#[repr(C)]
#[derive(ValueEnum, Debug, Copy, Clone)]
enum SampleOutputFormat {
    Flac,
    Wav,
}

#[repr(C)]
#[derive(ValueEnum, Debug, Copy, Clone, PartialEq)]
enum WriteFormat {
    Flac,
    Wav,
    Vorbis,
    Mp3,
}

#[repr(C)]
#[derive(ValueEnum, Debug, Copy, Clone, PartialEq)]
enum SampleDepth {
    Int16,
    Float,
}

#[repr(C)]
#[derive(ValueEnum, Debug, Copy, Clone, PartialEq)]
enum OggMode {
    Vbr,
    QualityVbr,
    Abr,
    ConstrainedAbr,
}

#[derive(Parser, Debug)]
#[clap(author, version, about, long_about = None)]
struct Args {
    /// Input song or directory of files supported by libopenmpt
    #[clap(short, long)]
    input: String,

    /// Output directory to place the generated files
    #[clap(short, long)]
    output: String,

    /// If input is a directory recursive can be used to get the all files within that directory
    #[clap(short, long)]
    recursive: bool,

    /// Represents the stereo separation generated by the mixer in percent. Range is [0, 200] and default value is 100.
    #[clap(long, default_value = None)]
    stereo_separation: Option<u32>,

    /// Render the whole song as is 
    #[clap(long, default_value = "false")]
    full: bool,

    /// Show progressbar when generating
    #[clap(long, default_value = "false")]
    progress: bool,

    /// Output sample rate. Should be in [8000, 192000]
    #[clap(short, long, default_value = "48000")]
    sample_rate: u32,

    /// Render the instruments to stereo wav files. mono is default
    #[clap(long, default_value = "false")]
    stereo: bool,

    /// Render each instrument for each channel (if false only a _all file will be generated)
    #[clap(short, long, default_value = "false")]
    channels: bool,

    /// Render each instrument to a separate file
    #[clap(long, default_value = "false")]
    instruments: bool,

    /// Write samples in the song to disk
    #[clap(long)]
    song_samples: Option<SampleOutputFormat>,

    /// Sample depth for the rendering.
    #[clap(short, long, default_value = "int16")]
    format: SampleDepth,

    /// Write format for the rendering.
    #[clap(short, long, default_value = "flac")]
    write: WriteFormat,

    /// Mode for the ogg vorbis encoding. 
    #[clap(long, default_value = "vbr")]
    vorbis_mode: OggMode,

    /// Bitrate option for vbr, abr, quality-vbr and constrained-abr 
    #[clap(long, default_value = "160")]
    vorbis_bitrate: u32,

    /// Quality option for quality-vbr range is [-0.2, 1]
    #[clap(long, default_value = "0.5")]
    vorbis_quality: f32,
}

#[repr(C)]
#[derive(Debug)]
struct SongInfo {
    channel_count: u32,
    instrument_count: u32,
    duration_seconds: f32,
}

// Has to match the struct in the C code
#[repr(C)]
struct RenderParams {
    sample_rate: u32,
    bytes_per_sample: u32,
    channel_to_play: i32, // if -1 use all channels, otherwise pick one channel
    instrument_to_play: i32, // if -1 use all instruments, otherwise pick one
    stereo_separation: u32,
    stereo_separation_enabled: bool,
    stereo_output: bool,
}

extern "C" {
    fn get_song_info_c(data: *const u8, len: u32, sample_output_path: *const u8, sample_format: u32) -> SongInfo;
    fn song_render_c(
        output: *mut u8,
        output_len: u32,
        input_data: *const u8,
        input_len: u32,
        params: *const RenderParams,
    ) -> u32;
}

fn get_song_info(file_data: &[u8], samples_output_path: Option<&Path>, sample_format: u32) -> SongInfo {
    if let Some(path) = samples_output_path {
        let os_path = path.to_string_lossy().into_owned();
        let c_filename = std::ffi::CString::new(os_path).unwrap();
        unsafe { get_song_info_c(file_data.as_ptr(), file_data.len() as u32, c_filename.as_ptr() as *const _, sample_format ) }
    } else {
        unsafe { get_song_info_c(file_data.as_ptr(), file_data.len() as u32, std::ptr::null(), 0) }
    }
}
fn song_render(
    output: &mut [u8],
    input: &[u8],
    render_params: &RenderParams,
) -> u32 {
    unsafe {
        song_render_c(
            output.as_mut_ptr(),
            output.len() as u32,
            input.as_ptr(),
            input.len() as u32,
            render_params,
        )
    }
}

// Get files for a given directory or single filename
fn get_files(path: &str, recurse: bool) -> Vec<String> {
    if !Path::new(path).exists() {
        log::info!(
            "Path/File \"{}\" doesn't exist. No file(s) will be processed.",
            path
        );
        return Vec::new();
    }

    // Check if "path" is a single file
    let md = std::fs::metadata(path).unwrap();

    if md.is_file() {
        return vec![path.to_owned()];
    }

    let max_depth = if !recurse { 1 } else { usize::MAX };

    let files: Vec<String> = WalkDir::new(path)
        .max_depth(max_depth)
        .into_iter()
        .filter_map(|e| {
            let file = e.unwrap();
            let metadata = file.metadata().unwrap();

            if let Some(filename) = file.path().to_str() {
                if metadata.is_file() {
                    return Some(filename.to_owned());
                }
            }
            None
        })
        .collect();
    files
}

fn write_flac_file(
    filename: &Path,
    buffer: Vec<u8>,
    sample_rate: u32,
    channel_count: usize,
    bytes_per_sample: usize,
) {
    let filename = PathBuf::from(filename).with_extension("flac"); 

    libflac_sys::encode_flac(
        &filename, 
        &buffer, 
        channel_count as _, 
        bytes_per_sample as _, 
        sample_rate as _);  
}

fn write_wav_file(
    filename: &Path,
    buffer: Vec<u8>,
    sample_rate: u32,
    channel_count: usize,
    bytes_per_sample: usize,
) {
    let filename = PathBuf::from(filename).with_extension("wav"); 

    let (format, bits) = if bytes_per_sample == 4 {
        (wav::header::WAV_FORMAT_IEEE_FLOAT, 32)
    } else {
        (wav::header::WAV_FORMAT_PCM, 16)
    };

    let mut out_file = match File::create(&filename) {
        Ok(f) => f,
        Err(e) => {
            log::error!("Unable to write to {:?} error: {:?}", filename, e);
            return;
        }
    };

    // Write out wav file
    let wav_header = wav::Header::new(format, channel_count as _, sample_rate, bits);
    wav::write(wav_header, &buffer.into(), &mut out_file).unwrap();
}

fn write_ogg_vorbis(
    filename: &Path,
    buffer: Vec<u8>,
    args: &Args,
    channel_count: usize,
) {
    let filename = PathBuf::from(filename).with_extension("ogg"); 
    let mut out_file = match File::create(&filename) {
        Ok(f) => f,
        Err(e) => {
            log::error!("Unable to write to {:?} error: {:?}", filename, e);
            return;
        }
    };

    let br = core::num::NonZeroU32::new((args.vorbis_bitrate * 1000) as _).unwrap();
    let target_quality = f32::clamp(args.vorbis_quality, -0.2, 1.0);

    let bitrate_mode = match args.vorbis_mode {
        OggMode::Vbr => VorbisBitrateManagementStrategy::Vbr { target_bitrate: br },
        OggMode::Abr => VorbisBitrateManagementStrategy::Abr { average_bitrate: br },
        OggMode::ConstrainedAbr => VorbisBitrateManagementStrategy::ConstrainedAbr { maximum_bitrate: br },
        OggMode::QualityVbr => VorbisBitrateManagementStrategy::QualityVbr { target_quality },
    };

    let mut encoder = VorbisEncoderBuilder::new(
        core::num::NonZeroU32::new(args.sample_rate as _).unwrap(),
        core::num::NonZeroU8::new(channel_count as _).unwrap(),
        &mut out_file,
    ).unwrap().bitrate_management_strategy(bitrate_mode).build().unwrap();

    if channel_count == 1 {
        let data: &[f32] = bytemuck::cast_slice(&buffer);

        let sample_step = 48000;
        let len = data.len();
        let mut offset = 0;

        loop {
            let step_value = std::cmp::min(sample_step, len - offset);

            let t = [&data[offset..offset + step_value]];

            match encoder.encode_audio_block(&t) {
                Ok(_) => (),
                Err(e) => {
                    log::error!("Unable to encode vorbis file: {:?}", e);
                    return;
                }
            }

            if step_value != sample_step {
                break;
            }

            offset += step_value;
        }
    } else {
        let data: &[f32] = bytemuck::cast_slice(&buffer);
        let channel0: Vec<f32> = data.iter().skip(0).step_by(2).copied().collect();
        let channel1: Vec<f32> = data.iter().skip(1).step_by(2).copied().collect();

        let sample_step = 48000;
        let len = channel0.len();
        let mut offset = 0;

        loop {
            let step_value = std::cmp::min(sample_step, len - offset);

            let t = [&channel0[offset..offset + step_value], &channel1[offset.. offset + step_value]];

            match encoder.encode_audio_block(&t) {
                Ok(_) => (),
                Err(e) => {
                    log::error!("Unable to encode vorbis file: {:?}", e);
                    return;
                }
            }

            if step_value != sample_step {
                break;
            }

            offset += step_value;
        }
    }

    match encoder.finish() {
        Ok(_) => (),
        Err(e) => {
            log::error!("Unable to finish vorbis file: {:?}", e);
            return;
        }
    }
}

fn write_mp3(
    filename: &Path,
    buffer: Vec<u8>,
    args: &Args,
    channel_count: usize,
    bytes_per_sample: usize,
) {
    let filename = PathBuf::from(filename).with_extension("mp3"); 

    let mut out_file = match File::create(&filename) {
        Ok(f) => f,
        Err(e) => {
            log::error!("Unable to write to {:?} error: {:?}", filename, e);
            return;
        }
    };

    let mut mp3_encoder = Builder::new().expect("Create LAME builder");
    mp3_encoder.set_num_channels(channel_count as _).expect("set channels");
    mp3_encoder.set_sample_rate(args.sample_rate as _).expect("set sample rate");
    mp3_encoder.set_brate(mp3lame_encoder::Bitrate::Kbps192).expect("set brate");
    mp3_encoder.set_quality(mp3lame_encoder::Quality::Best).expect("set quality");
    let mut mp3_encoder = mp3_encoder.build().expect("To initialize LAME encoder");

    let mut mp3_out_buffer = Vec::new();
    let encoded_size;

    if channel_count == 2 {
        if bytes_per_sample == 2 {
            let data: &[i16] = bytemuck::cast_slice(&buffer);
            let input = InterleavedPcm(data);

            mp3_out_buffer.reserve(mp3lame_encoder::max_required_buffer_size(data.len() / 2));
            encoded_size = mp3_encoder.encode(input, mp3_out_buffer.spare_capacity_mut()).expect("To encode");
        } else {
            let data: &[f32] = bytemuck::cast_slice(&buffer);
            let input = InterleavedPcm(data);

            mp3_out_buffer.reserve(mp3lame_encoder::max_required_buffer_size(data.len() / 2));
            encoded_size = mp3_encoder.encode(input, mp3_out_buffer.spare_capacity_mut()).expect("To encode");
        }
    } else {
        if bytes_per_sample == 2 {
            let data: &[i16] = bytemuck::cast_slice(&buffer);
            let input = MonoPcm(data);

            mp3_out_buffer.reserve(mp3lame_encoder::max_required_buffer_size(data.len()));
            encoded_size = mp3_encoder.encode(input, mp3_out_buffer.spare_capacity_mut()).expect("To encode");
        } else {
            let data: &[f32] = bytemuck::cast_slice(&buffer);
            let input = MonoPcm(data);

            mp3_out_buffer.reserve(mp3lame_encoder::max_required_buffer_size(data.len()));
            encoded_size = mp3_encoder.encode(input, mp3_out_buffer.spare_capacity_mut()).expect("To encode");
        }
    }

    unsafe {
        mp3_out_buffer.set_len(mp3_out_buffer.len().wrapping_add(encoded_size));
    }

    let encoded_size = mp3_encoder.flush::<FlushNoGap>(mp3_out_buffer.spare_capacity_mut()).expect("to flush");
    unsafe {
        mp3_out_buffer.set_len(mp3_out_buffer.len().wrapping_add(encoded_size));
    }

    out_file.write_all(&mp3_out_buffer).unwrap();
}


fn gen_song(
    filestem: &str,
    song_info: &SongInfo,
    song: &[u8],
    args: &Args,
    channel: i32,
    instrument: i32,
    stereo: bool,

) {
    // Number of bytes needed given a sample depth
    let bytes_per_sample = if args.format == SampleDepth::Float { 4 } else { 2 };
    // Number of bytes needed given a sample depth
    let mut channel_count = if args.stereo { 2 } else { 1 };

    let (stereo_separation, stereo_separation_enabled) = if let Some(stereo_sep) = args.stereo_separation {
        (stereo_sep, true)
    } else {
        (100, false)
    };

    let mut stereo = stereo;    

    // two channels for full track
    if channel == -1 && instrument == -1 {
        channel_count = 2; 
        stereo = true;
    }

    let render_params = RenderParams {
        sample_rate: args.sample_rate as _,
        bytes_per_sample,
        channel_to_play: channel,
        instrument_to_play: instrument,
        stereo_separation,
        stereo_separation_enabled,
        stereo_output: stereo,
    };

    let sample_rate = args.sample_rate as usize;
    // We add 5 sec extra to the duration to make sure the buffer is large enough
    let song_len = song_info.duration_seconds as usize;

    let filename = if channel == -1 && instrument == -1 {
        Path::new(&args.output).join(format!("{}", filestem))
    } else if channel == -1 {
        Path::new(&args.output).join(format!("{}_{:04}_chan_full", filestem, instrument + 1))
    } else {
        Path::new(&args.output).join(format!(
            "{}_{:04}_chan_{:04}",
            filestem, instrument + 1, channel
        ))
    };

    // two channels for full track
    if channel == -1 && instrument == -1 {
        channel_count = 2; 
    }

    let output_size_bytes = song_len * sample_rate * bytes_per_sample as usize * channel_count * 2;
    let mut output_buffer = vec![0u8; output_size_bytes];

    let render_len = song_render(&mut output_buffer, song, &render_params);

    output_buffer.truncate(render_len as _);

    // TODO: Optimize
    if output_buffer.iter().any(|x| *x != 0) {
        match args.write {
            WriteFormat::Flac => {
                write_flac_file(
                    &filename,
                    output_buffer,
                    args.sample_rate,
                    channel_count,
                    bytes_per_sample as _,
                );
            }
            WriteFormat::Wav => {
                write_wav_file(
                    &filename,
                    output_buffer,
                    args.sample_rate,
                    channel_count,
                    bytes_per_sample as _,
                );
            }
            WriteFormat::Vorbis => {
                write_ogg_vorbis(
                    &filename,
                    output_buffer,
                    &args,
                    channel_count,
                );
            }
            WriteFormat::Mp3 => {
                write_mp3(
                    &filename,
                    output_buffer,
                    &args,
                    channel_count,
                    bytes_per_sample as _,
                );
            }
        }
    }
}

fn main() -> Result<()> {
    let mut args = Args::parse();
    SimpleLogger::new()
        .with_level(log::LevelFilter::Error)
        .init()?;

    let files = get_files(&args.input, args.recursive);

    // Force float if writing vorbis
    if args.write == WriteFormat::Vorbis {
        args.format = SampleDepth::Float;
    }

    for filename in files {
        let file_path = Path::new(&filename);
        let mut file = File::open(&filename)?;
        let mut song_buffer = Vec::new();
        file.read_to_end(&mut song_buffer)?;
        
        let stemname = file_path.file_stem().unwrap().to_str().unwrap();

        println!("Processing file {}", filename);

        let song_info = if let Some(sample_format) = args.song_samples {
            let sample_path = Path::new(&args.output).join(format!("{}", stemname));
            get_song_info(&song_buffer, Some(&sample_path), sample_format as _)
        } else {
            get_song_info(&song_buffer, None, 0)
        };

        if song_info.channel_count == 0 || song_info.instrument_count == 0 {
            log::error!(
                "Song {} doesn'n contain any channels or instruments so is being skipped!",
                &filename
            );
            continue;
        }

        if song_info.duration_seconds == 0.0 {
            log::error!("Song {} doesn'n have a duration. Skipping", &filename);
            continue;
        }

        if args.full {
            gen_song(
                &stemname,
                &song_info,
                &song_buffer,
                &args,
                -1,
                -1,
                true,
            );
        }

        let mut pb = None;

        let spinner_style =
            ProgressStyle::with_template("{prefix:.bold.dim} {wide_bar} {pos}/{len}").unwrap();

        if args.channels {
            let channel_count = song_info.channel_count;
            let instrument_count = song_info.instrument_count;
            let total_count = channel_count * instrument_count; 

            if args.progress {
                let p = ProgressBar::new(total_count as u64);
                p.set_style(spinner_style);
                pb = Some(p);
            }

            (0..total_count)
                .into_par_iter()
                .for_each(|index| {
                    let instrument = index / channel_count;
                    let channel = index % channel_count;
                    gen_song(
                        &stemname,
                        &song_info,
                        &song_buffer,
                        &args,
                        channel as _,
                        instrument as _,
                        args.stereo
                    );

                    if let Some(p) = &pb {
                        p.inc(1);
                    }
                });
        } else if args.instruments {
            if args.progress {
                let p = ProgressBar::new(song_info.instrument_count as u64);
                p.set_style(spinner_style);
                pb = Some(p);
            }
            (0..song_info.instrument_count)
                .into_par_iter()
                .for_each(|instrument| {
                    gen_song(
                        &stemname,
                        &song_info,
                        &song_buffer,
                        &args,
                        -1,
                        instrument as _,
                        args.stereo
                    );

                    if let Some(p) = &pb {
                        p.inc(1);
                    }
                });
        }
    }

    Ok(())
}
