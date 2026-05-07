# Dreamcast FMV Player

This repository contains a Dreamcast FMV toolchain and standalone player for
custom `.dcmv` movies. The converter extracts video frames with `ffmpeg`,
turns them into VQ-compressed PVR textures with KOS `pvrtex`, converts audio
with `dcaconv`, and packages the result for playback on Dreamcast hardware.

The current player supports two DCMV container layouts:

* `frames` / v6: frame-table video followed by one ADPCM audio stream.
* `chunks` / v1: time-based chunks with interleaved compressed video and
  chunk-local ADPCM audio.

Both modes have been tested on real hardware with `dc-tool-ip`.

## Tools

* `convert_to_pvr_fmv.sh`: main conversion script.
* `pack_dcmv.c`: v6 frame-table packer source.
* `pack_dcmv`: v6 frame-table packer binary.
* `pack_dcmv_chunk.c`: v1 chunked packer source.
* `pack_dcmv_chunk`: v1 chunked packer binary.
* `generate_durations.py`: optional frame deduplication helper.
* `dcaconv`: Dreamcast ADPCM encoder.
* `playdcmv/fmv_play.elf`: standalone Dreamcast playback binary.

## Dependencies

Install or build these before converting movies:

* `ffmpeg`
* `python3` with Pillow and NumPy for frame deduplication
* `pvrtex` from KallistiOS, expected by default at
  `/opt/toolchains/dc/kos/utils/pvrtex/pvrtex`
* `dcaconv`
* `lz4` development library
* `zstd` development library
* host C compiler such as `gcc`
* KallistiOS toolchain for rebuilding `playdcmv/fmv_play.elf`

## Build Packers

```bash
gcc -O2 pack_dcmv.c -o pack_dcmv -llz4 -lzstd
gcc -O2 pack_dcmv_chunk.c -o pack_dcmv_chunk -llz4 -lzstd -lm
```

## Build Player

```bash
make -C playdcmv
```

The player reads `/pc/movie.dcmv` first, then falls back to `/cd/movie.dcmv`.
The conversion script writes `./playdcmv/movie.dcmv` by default, which matches
the `/pc` path when launched from `playdcmv` with `dc-tool-ip -m .`.

## Configure Conversion

Edit the user configuration block at the top of `convert_to_pvr_fmv.sh`.
The most important settings are:

```bash
INPUT="input/movie.mp4"
AUDIOINPUT=$INPUT
FINAL_OUTPUT="./playdcmv/movie.dcmv"

FPS=24
FORMAT="yuv422"          # rgb565 or yuv422
USE_STRIDED=true         # true = direct texture dimensions, false = POT padded
SCALE_WIDTH=320
SCALE_HEIGHT=240

AUDIO_RATE=44100
CHANNELS=1               # 0 = video-only, 1 = mono, 2 = stereo

USE_DEDUP=false
COMPRESSION_BACKEND="lz4" # lz4 or zstd
DCMV_CONTAINER="chunks"   # frames or chunks
CHUNK_DURATION="${CHUNK_DURATION:-0.5}"
FRAME_DIGITS="${FRAME_DIGITS:-5}"
```

For 640x480 output, set `SCALE_WIDTH=640` and `SCALE_HEIGHT=480`.
For long movies with more than 99,999 frames, set `FRAME_DIGITS=6`.

## Container Modes

Use v6 frame-table mode when you want the older contiguous audio layout:

```bash
DCMV_CONTAINER="frames"
```

This mode calls `./pack_dcmv`, writes header version `6`, supports video-only
files with `CHANNELS=0`, and stores `Audio offset` as an absolute file offset.

Use v1 chunked mode when you want interleaved video/audio chunks:

```bash
DCMV_CONTAINER="chunks"
CHUNK_DURATION=0.5
```

This mode calls `./pack_dcmv_chunk` and writes header version `1`. It supports
`CHANNELS=0` video-only output as well as mono and stereo audio. `Audio offset`
is expected to print as `0x0` in the standalone player because chunked audio is
found through the chunk index, not through the v6 audio-offset field.

## Convert A Movie

For a clean conversion:

```bash
rm -rf output temp_frames
./convert_to_pvr_fmv.sh
```

The script creates:

* `temp_frames/`: extracted intermediate frames.
* `output/unique_frames/`: deduplicated or copied source frames plus
  `frame_durations.txt`.
* `output/frame*.dt`: VQ-compressed Dreamcast textures.
* `output/audio.dca`: v6 ADPCM audio when `DCMV_CONTAINER=frames`.
* `temp_frames/temp.wav`: PCM audio used by the chunked packer.
* `playdcmv/movie.dcmv`: final movie.

## Run On Hardware

From the `playdcmv` directory:

```bash
dc-tool-ip -t 192.168.0.128 -f -x fmv_play.elf -m . > out.log
```

Replace the IP with your Dreamcast Broadband Adapter IP.

Expected v6 frame-table log:

```text
Header v6 (frames): YUV422 640x480 (content: 640x480) @ 24.00fps, 44100Hz, 2ch
Frame size: 78848, Max compressed: ..., Audio offset: 0x..., Compression: LZ4
Starting playback @ 41.67ms/frame
Playback finished
```

Expected v1 chunked log:

```text
Header v1 (chunks): YUV422 320x240 (content: 320x240) @ 24.00fps, 44100Hz, 1ch
Frame size: 21248, Max compressed: ..., Audio offset: 0x0, Compression: LZ4
Starting playback @ 41.67ms/frame
```

## Notes

* `lz4` is usually the faster decode choice.
* `zstd` can reduce file size but costs more decode CPU.
* `USE_DEDUP=true` can reduce file size when source material has repeated
  frames; it also writes repeat counts to `frame_durations.txt`.
* The standalone player owns PVR presentation and waits for PVR readiness before
  upload/submit. Client integrations such as DCSinge should do those waits in
  their own presentation layer.

## License

This project is for educational and demo use. See individual tools for their
respective licenses.

## Author

Troy E. Davis ([@GPF](https://github.com/GPF))
