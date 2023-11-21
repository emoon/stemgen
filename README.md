# stemgen

STEM generator using libopenmpt with glue in Rust

# Building

Download Rust by following the instructions here https://rustup.rs
Notice that a C++ toolchain needs to be installed as well. On Windows this means Visual Studo 2019 or later and on Linux or macOS clang or gcc.

`cargo build --release`

And to run

`cargo run --release -- <parameters>`

or just run directly from

`target/release/stemgen`

after building the release config

# Usage

```
Usage: stemgen [OPTIONS] --input <INPUT> --output <OUTPUT>

Options:
  -i, --input <INPUT>              Input file or directory of files supported by libopenmpt
  -o, --output <OUTPUT>            Output directory to place the generated wav files
  -r, --recursive                  If input is a directory recursive can be used to get the all files within that directory
  -p, --panning <PANNING>          Panning value of the active channel. Should be in [-1.0, 1.0] where 0.0 is center
      --full                       Render the whole song as is
      --progress                   Show progressbar when generating
  -s, --sample-rate <SAMPLE_RATE>  Output sample rate. Should be in [8000, 192000] [default: 48000]
      --stereo                     Render the instruments to stereo wav files. mono is default
  -c, --channels                   Render each instrument for each channel (if false only a _all file will be generated)
  -f, --format <FORMAT>            Sample depth for the rendering. Suppored are "float" and "int16" [default: int16]
  -w, --write <WRITE>              Write format for the rendering. Suppored are "flac" and "wav" [default: flac]
  -h, --help                       Print help
  -V, --version                    Print version
```
