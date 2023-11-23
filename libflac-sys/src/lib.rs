//! Raw FFI bindings to the `libFLAC` library.
//!
//! Original C API documentation: <https://xiph.org/flac/api/>
//!

#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(non_upper_case_globals)]
#![allow(clippy::useless_transmute)]

use std::path::Path;
use std::ffi::{CString, CStr};

#[allow(clippy::upper_case_acronyms)]
pub type FILE = libc::FILE;

include!("bindings.rs");

pub fn encode_flac(filename: &Path, buffer: &[u8], channels: u32, bytes_per_sample: u32, sample_rate: u32) -> bool {
    let os_path = filename.to_string_lossy().into_owned();
    let c_filename = CString::new(os_path).unwrap();

    let bits_per_sample = if bytes_per_sample == 4 { 24 } else { 16 };

    let samples = if bytes_per_sample == 4 {
        let data: &[f32] = bytemuck::cast_slice(&buffer);
        data.iter().map(|x| (*x * (1 << (bits_per_sample - 1)) as f32) as i32).collect::<Vec<i32>>()
    } else {
        let data: &[i16] = bytemuck::cast_slice(&buffer);
        data.iter().map(|x| (*x as i32)).collect::<Vec<i32>>()
    };

    unsafe {
        let  encoder = FLAC__stream_encoder_new();

        FLAC__stream_encoder_set_verify(encoder, 1);
        FLAC__stream_encoder_set_compression_level(encoder, 8); // Max compression 

        FLAC__stream_encoder_set_channels(encoder, channels);
        FLAC__stream_encoder_set_bits_per_sample(encoder, bits_per_sample);
        FLAC__stream_encoder_set_sample_rate(encoder, sample_rate);

        FLAC__stream_encoder_set_total_samples_estimate(encoder, 0); // Unknown number of samples

        FLAC__stream_encoder_set_ogg_serial_number(encoder, 0); // Not using Ogg encapsulation

        FLAC__stream_encoder_init_file(encoder, c_filename.as_ptr(), None, std::ptr::null_mut());

        let success = FLAC__stream_encoder_process_interleaved(encoder, samples.as_ptr(), samples.len() as u32 / channels);

        if success == 0 {
            let cstr = CStr::from_ptr(FLAC__stream_encoder_get_resolved_state_string(encoder));
            let error = String::from_utf8_lossy(cstr.to_bytes()).to_string();
            println!("FLAC__stream_encoder_process_interleaved failed for file {:?} {}", filename, error);
            
            false
        } else {
            FLAC__stream_encoder_finish(encoder);
            FLAC__stream_encoder_delete(encoder);
            true
        }
    }
}
