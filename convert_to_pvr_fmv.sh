#!/bin/bash
#
# convert_to_pvr_fmv.sh - Dreamcast FMV Toolchain Driver Script
# -------------------------------------------------------------
# This script automates the conversion of an MP4 video into a DCMV container
# suitable for playback on the Sega Dreamcast using the `fmv_play.elf` player.
#
# Steps performed:
# 1. Extract RGB frames from input video using `ffmpeg`
# 2. Convert each frame to RGB565 and encode into VQ-compressed PVR textures using `pvrtex`
# 3. Extract and encode audio to Dreamcast ADPCM format using `dcaconv`
# 4. Package the VQ textures and audio into a `.dcmv` container using `pack_dcmv`
#
# Requirements:
# - ffmpeg (for video and audio extraction)
# - dcaconv (TapamN’s Dreamcast ADPCM encoder: https://github.com/TapamN/dcaconv)
# - pvrtex (KOS utility for RGB565 VQ texture generation)
# - pack_dcmv (custom LZ4-based video+audio packer)
#
# Customize the variables below (input path, resolution, audio settings, etc.)
# before running the script to match your desired output.
#
# Author: Troy E. Davis (GPF) — https://github.com/GPF
#
# Note: Ensure all tools are compiled and accessible in PATH or set explicitly.

# Settings
INPUT="input/Dreamcast Startup (60fps).mp4"
OUTPUT_DIR="output"
TEMP_DIR="temp_frames"
FPS=30
WIDTH=512
HEIGHT=512
AUDIO_RATE=32000
CHANNELS=1
DCACONV="./dcaconv" # https://github.com/TapamN/dcaconv
PVRTX="/opt/toolchains/dc/kos/utils/pvrtex/pvrtex"
PACKER="./pack_dcmv"
EXT="dt"  # can also be tex or pvr

# Prepare folders
mkdir -p "$OUTPUT_DIR"
mkdir -p "$TEMP_DIR"
echo "📂 Created output and temp directories"

echo "📁 Checking for existing $EXT frames..."
if ls "$OUTPUT_DIR"/frame*.${EXT} 1> /dev/null 2>&1; then
  echo "✅ Found preconverted .${EXT} frames, skipping frame extraction and conversion."
else
  echo "🖼️  Extracting PNG frames at ${FPS} fps..."
  ffmpeg -hide_banner -loglevel error -y -i "$INPUT" \
    -vf "fps=${FPS},scale=${WIDTH}:${HEIGHT}:flags=lanczos" \
    -start_number 0 "$TEMP_DIR/frame%04d.png" || exit 1

  echo "🎞️ Converting frames to VQ-compressed ${EXT}..."
  frame_idx=0
  for png in "$TEMP_DIR"/frame*.png; do
    base=$(printf "frame%04d" "$frame_idx")
    "$PVRTX" -i "$png" -o "$OUTPUT_DIR/${base}.${EXT}" -f RGB565 -c small --dither 1 || exit 1
    ((frame_idx++))
  done
fi

# Extract and convert audio
echo "🔊 Extracting and converting audio to ADPCM (channels=${CHANNELS}, rate=${AUDIO_RATE})..."
ffmpeg -hide_banner -loglevel error -i "$INPUT" -ac "$CHANNELS" -ar "$AUDIO_RATE" -c:a pcm_s16le -y "$TEMP_DIR/temp.wav"
"$DCACONV" --long --rate "$AUDIO_RATE" -c "$CHANNELS" -f ADPCM \
  -i "$TEMP_DIR/temp.wav" -o "$TEMP_DIR/audio.dca" || exit 1

# Pack video frames + audio into compressed .dcmv format
echo "📦 Packing into compressed .dcmv format..."
"$PACKER" "./playdcmv/movie.dcmv" "$WIDTH" "$HEIGHT" "$FPS" "$AUDIO_RATE" "$CHANNELS" \
  "$OUTPUT_DIR/frame%04d.${EXT}" "$TEMP_DIR/audio.dca" || exit 1

# Clean up intermediate files
# echo "🧹 Cleaning up temporary files..."
# rm -rf "$OUTPUT_DIR"
# rm -rf "$TEMP_DIR"

echo "✅ Final .dcmv + audio.dca created:"
ls -lh ./playdcmv/movie.dcmv
