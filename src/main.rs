use anyhow::Result;
use clap::Parser;
use rayon::prelude::*;
use simple_logger::SimpleLogger;
use std::{fs::File, io::Read, path::Path};
use walkdir::WalkDir;
use wav;

#[derive(Parser, Debug)]
#[clap(author, version, about, long_about = None)]
struct Args {
    /// Input file or directory of files supported by libopenmpt
    #[clap(short, long)]
    input: String,

    /// Output directory to place the generated wav files
    #[clap(short, long)]
    output: String,

    /// If input is a directory recursive can be used to get the all files within that directory
    #[clap(short, long)]
    recursive: bool,

    /// Show progressbar when generating
    #[clap(short, long, default_value = "false")]
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

    /// Sample depth for the rendering. Support is "float" and "int16"
    #[clap(short, long, default_value = "int16")]
    format: String,
}

#[repr(C)]
#[derive(Debug)]
struct SongInfo {
    channel_count: u32,
    instrument_count: u32,
    duration_seconds: f32,
}

extern "C" {
    fn get_song_info_c(data: *const u8, len: u32) -> SongInfo;
    fn song_render_c(
        output: *mut u8,
        output_len: u32,
        input_data: *const u8,
        input_len: u32,
        sample_rate: u32,
        bits_per_sample: u32,
        channel_to_play: i32, // if -1 use all channels, otherwise pick one channel
        instrument_to_play: i32, // if -1 use all instruments, otherwise pick one
        stereo_output: bool,
    ) -> u32;
}

fn get_song_info(file_data: &[u8]) -> SongInfo {
    unsafe { get_song_info_c(file_data.as_ptr(), file_data.len() as u32) }
}

fn song_render_instrument(
    output: &mut [u8],
    input: &[u8],
    sample_rate: u32,
    bits_per_sample: u32,
    channel_to_use: i32,
    instrument_to_play: i32,
) -> u32 {
    unsafe {
        song_render_c(
            output.as_mut_ptr(),
            output.len() as u32,
            input.as_ptr(),
            input.len() as u32,
            sample_rate,
            bits_per_sample,
            channel_to_use,
            instrument_to_play,
            false,
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

fn write_wav_file(
    filename: &Path,
    buffer: Vec<u8>,
    sample_rate: u32,
    channel_count: usize,
    bytes_per_sample: usize,
) {
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

fn gen_song(
    filestem: &str,
    song_info: &SongInfo,
    song: &[u8],
    args: &Args,
    channel: i32,
    instrument: i32,
) {
    let sample_rate = args.sample_rate as usize;
    // Number of bytes needed given a sample depth
    let bytes_per_sample = if args.format == "float" { 4 } else { 2 };
    // Number of bytes needed given a sample depth
    let channel_count = if args.stereo { 2 } else { 1 };
    // We add 1 sec extra to the duration to make sure the buffer is large enough
    let song_len = (song_info.duration_seconds + 1.0) as usize;

    let filename = if channel == -1 {
        Path::new(&args.output).join(format!("{}_{:04}_chan_full.wav", filestem, instrument))
    } else {
        Path::new(&args.output).join(format!(
            "{}_{:04}_chan_{:04}.wav",
            filestem, instrument, channel
        ))
    };

    let output_size_bytes = song_len * sample_rate * bytes_per_sample * channel_count;
    let mut output_buffer = vec![0u8; output_size_bytes];

    song_render_instrument(
        &mut output_buffer,
        song,
        args.sample_rate,
        bytes_per_sample as _,
        channel,
        instrument,
    );

    // TODO: Optimize
    if output_buffer.iter().any(|x| *x != 0) {
        write_wav_file(
            &filename,
            output_buffer,
            args.sample_rate,
            channel_count,
            bytes_per_sample,
        );
    }
}

fn main() -> Result<()> {
    let args = Args::parse();
    SimpleLogger::new()
        .with_level(log::LevelFilter::Info)
        .init()?;

    let files = get_files(&args.input, args.recursive);

    for filename in files {
        let file_path = Path::new(&filename);
        let mut file = File::open(&filename)?;
        let mut song_buffer = Vec::new();
        file.read_to_end(&mut song_buffer)?;
        let song_info = get_song_info(&song_buffer);
        let stemname = file_path.file_stem().unwrap().to_str().unwrap();

        log::info!("Processing file {}", filename);

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

        if args.channels {
            let channel_count = song_info.channel_count;
            let instrument_count = song_info.instrument_count;
            let total_count = channel_count * instrument_count; 

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
                    )
                });
        } else {
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
                    )
                });
        }
    }

    Ok(())
}
