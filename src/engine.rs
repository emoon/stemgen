//! The libraries that play a song: libopenmpt, and nuade for the Amiga formats libopenmpt doesn't know.
//! Everything FFI lives here; the rest of the program only sees `Song`.

use clap::ValueEnum;
use std::{
    ffi::{c_char, c_void, CStr, CString},
    path::Path,
};

#[repr(C)]
#[derive(ValueEnum, Debug, Copy, Clone)]
pub enum SampleOutputFormat {
    Flac,
    Wav,
}

#[repr(C)]
#[derive(Debug)]
pub struct SongInfo {
    pub channel_count: u32,
    pub instrument_count: u32,
    pub duration_seconds: f32,
}

// Has to match the struct in the C code
#[repr(C)]
pub struct RenderParams {
    pub sample_rate: u32,
    pub bytes_per_sample: u32,
    pub channel_to_play: i32, // if -1 use all channels, otherwise pick one channel
    pub instrument_to_play: i32, // if -1 use all instruments, otherwise pick one
    pub stereo_separation: u32,
    pub stereo_separation_enabled: bool,
    pub stereo_output: bool,
}

// Has to match SampleDesc in nuade_interface.c
#[repr(C)]
struct SampleDesc {
    index: u32,
    name: *const c_char,
    data: *const c_void,
    frames: u32,
    bits: u32,
    rate: u32,
    loop_start: u32,
    loop_frames: u32,
}

// Has to match SampleExportInfo in nuade_interface.c
#[repr(C)]
struct SampleExportInfo {
    supported: bool,
    total: u32,
    skipped: u32,
    player_name: [c_char; 64],
}

type SampleCallback = extern "C" fn(*mut c_void, *const SampleDesc);

extern "C" {
    fn get_song_info_c(
        data: *const u8,
        len: u32,
        sample_output_path: *const c_char,
        sample_format: u32,
    ) -> SongInfo;
    fn song_render_c(
        output: *mut u8,
        output_len: u32,
        input_data: *const u8,
        input_len: u32,
        params: *const RenderParams,
    ) -> u32;
    fn get_song_info_nuade(data: *const u8, len: u32, path: *const c_char) -> SongInfo;
    fn get_instrument_ids_nuade(
        data: *const u8,
        len: u32,
        path: *const c_char,
        out: *mut u32,
        cap: u32,
    ) -> u32;
    fn song_render_nuade(
        output: *mut u8,
        output_len: u32,
        input_data: *const u8,
        input_len: u32,
        path: *const c_char,
        params: *const RenderParams,
    ) -> u32;
    fn export_samples_nuade(
        data: *const u8,
        len: u32,
        path: *const c_char,
        callback: SampleCallback,
        ctx: *mut c_void,
    ) -> SampleExportInfo;
}

#[derive(Debug, Copy, Clone, PartialEq)]
enum Engine {
    Openmpt,
    Nuade,
}

/// A sound from a song as 16 bit mono PCM
pub struct PcmSample {
    /// The number the format gives the sound, the same as `Song::instrument_ids`
    pub index: u32,
    pub name: String,
    pub data: Vec<i16>,
    /// Sample rate at the base note, 0 if not known
    pub rate: u32,
}

/// What `Song::export_samples` did
#[derive(Debug, Default)]
pub struct SampleReport {
    pub exported: u32,
    /// Sounds without PCM to export (synth instruments and such)
    pub skipped: u32,
    /// Set when the song's player generates all its sounds at run time. Nothing can be exported then.
    pub generated_by: Option<String>,
}

pub struct Song<'a> {
    engine: Engine,
    pub info: SongInfo,
    /// The number the format gives each instrument. Rendering a single instrument takes one of these.
    pub instrument_ids: Vec<u32>,
    data: &'a [u8],
    // Path to the file, nuade uses it to find files that belong to the song (instruments, samples)
    path: CString,
}

impl<'a> Song<'a> {
    /// Finds an engine that can play the song, libopenmpt first. None if nobody can.
    pub fn open(data: &'a [u8], filename: &str) -> Option<Song<'a>> {
        let path = CString::new(filename).ok()?;

        let info =
            unsafe { get_song_info_c(data.as_ptr(), data.len() as u32, std::ptr::null(), 0) };
        if info.channel_count != 0 {
            let instrument_ids = (0..info.instrument_count).collect();
            return Some(Song {
                engine: Engine::Openmpt,
                info,
                instrument_ids,
                data,
                path,
            });
        }

        let info = unsafe { get_song_info_nuade(data.as_ptr(), data.len() as u32, path.as_ptr()) };
        if info.channel_count != 0 {
            let mut instrument_ids = vec![0u32; info.instrument_count as usize];
            unsafe {
                get_instrument_ids_nuade(
                    data.as_ptr(),
                    data.len() as u32,
                    path.as_ptr(),
                    instrument_ids.as_mut_ptr(),
                    instrument_ids.len() as u32,
                );
            }
            return Some(Song {
                engine: Engine::Nuade,
                info,
                instrument_ids,
                data,
                path,
            });
        }

        None
    }

    pub fn engine_name(&self) -> &'static str {
        match self.engine {
            Engine::Openmpt => "libopenmpt",
            Engine::Nuade => "nuade",
        }
    }

    pub fn render(&self, output: &mut [u8], render_params: &RenderParams) -> u32 {
        unsafe {
            match self.engine {
                Engine::Openmpt => song_render_c(
                    output.as_mut_ptr(),
                    output.len() as u32,
                    self.data.as_ptr(),
                    self.data.len() as u32,
                    render_params,
                ),
                Engine::Nuade => song_render_nuade(
                    output.as_mut_ptr(),
                    output.len() as u32,
                    self.data.as_ptr(),
                    self.data.len() as u32,
                    self.path.as_ptr(),
                    render_params,
                ),
            }
        }
    }

    /// Writes out the samples of the song. libopenmpt writes the files itself (named
    /// `<out_stem>_sample_NNNN.<ext>`); nuade hands each sound to `write` to encode, as
    /// the PCM data is all it has.
    pub fn export_samples(
        &self,
        out_stem: &Path,
        format: SampleOutputFormat,
        mut write: impl FnMut(PcmSample),
    ) -> SampleReport {
        match self.engine {
            Engine::Openmpt => {
                let stem = CString::new(out_stem.to_string_lossy().into_owned()).unwrap();
                unsafe {
                    get_song_info_c(
                        self.data.as_ptr(),
                        self.data.len() as u32,
                        stem.as_ptr(),
                        format as u32,
                    );
                }
                SampleReport::default()
            }
            Engine::Nuade => self.export_samples_nuade(&mut write),
        }
    }

    fn export_samples_nuade(&self, write: &mut dyn FnMut(PcmSample)) -> SampleReport {
        let mut exported = 0;
        let mut ctx: (&mut dyn FnMut(PcmSample), &mut u32) = (write, &mut exported);

        extern "C" fn on_sample(ctx: *mut c_void, desc: *const SampleDesc) {
            let (write, exported) =
                unsafe { &mut *(ctx as *mut (&mut dyn FnMut(PcmSample), &mut u32)) };
            let desc = unsafe { &*desc };
            let frames = desc.frames as usize;

            // 8 bit widens losslessly, so every sound is written the same way
            let data = unsafe {
                if desc.bits == 8 {
                    std::slice::from_raw_parts(desc.data as *const i8, frames)
                        .iter()
                        .map(|&s| (s as i16) << 8)
                        .collect()
                } else {
                    std::slice::from_raw_parts(desc.data as *const i16, frames).to_vec()
                }
            };
            let name = if desc.name.is_null() {
                String::new()
            } else {
                unsafe { CStr::from_ptr(desc.name) }
                    .to_string_lossy()
                    .into_owned()
            };

            **exported += 1;
            write(PcmSample {
                index: desc.index,
                name,
                data,
                rate: desc.rate,
            });
        }

        let info = unsafe {
            export_samples_nuade(
                self.data.as_ptr(),
                self.data.len() as u32,
                self.path.as_ptr(),
                on_sample,
                &mut ctx as *mut _ as *mut c_void,
            )
        };

        let player_name = unsafe { CStr::from_ptr(info.player_name.as_ptr()) }
            .to_string_lossy()
            .into_owned();

        SampleReport {
            exported,
            skipped: info.skipped,
            generated_by: (!info.supported).then_some(player_name),
        }
    }
}
