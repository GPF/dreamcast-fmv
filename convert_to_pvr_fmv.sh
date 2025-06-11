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
# Configuration - User Adjustable
INPUT="/home/gpf/code/dreamcast/DirkSimple/lair.ogv"
OUTPUT_DIR="output"
TEMP_DIR="temp_frames"
FPS=23.97
WIDTH=512
HEIGHT=256
SCALE_WIDTH=320
SCALE_HEIGHT=240
PAD_X=$(( (WIDTH - SCALE_WIDTH) / 2 ))  # (512 - 320) / 2 = 96
PAD_Y=$(( (HEIGHT - SCALE_HEIGHT) / 2 )) # (256 - 240) / 2 = 8
AUDIO_RATE=32000
CHANNELS=1
FORMAT="rgb565"  # yuv420p or rgb565

# Tool Paths (adjust as needed)
PVRTX="/opt/toolchains/dc/kos/utils/pvrtex/pvrtex"
DCACONV="./dcaconv" # https://github.com/TapamN/dcaconv
PACKER="./pack_dcmv"
YUVCONVERTER="./yuv420converter"

# Performance Optimization
THREADS=$(nproc)                # Auto-detect CPU cores
FFMPEG_LOGLEVEL="warning"       # error/warning/info
PVRTX_QUIET=">/dev/null 2>&1"   # Suppress pvrtex output

# Dreamcast-specific optimizations
  # FFMPEG_OPTS=(
  #     -hide_banner
  #     -loglevel "$FFMPEG_LOGLEVEL"
  #     -y
  #     -i "$INPUT"
  #     -vf "fps=$FPS,scale=$WIDTH:$HEIGHT:flags=lanczos,hqdn3d=1.0:1.0:6.0:6.0,smartblur=1.0:0.0"
  #     -sws_flags "+accurate_rnd+full_chroma_int+full_chroma_inp"
  # )
FFMPEG_OPTS=(
    -hide_banner
    -loglevel "$FFMPEG_LOGLEVEL"
    -y
    -i "$INPUT"
    -vf "scale=$SCALE_WIDTH:$SCALE_HEIGHT:flags=lanczos,pad=$WIDTH:$HEIGHT:$PAD_X:$PAD_Y:black,hqdn3d=1.0:1.0:6.0:6.0,smartblur=1.0:0.0"
    -sws_flags "+accurate_rnd+full_chroma_int+full_chroma_inp"
)

PVRTX_OPTS=(
  -f RGB565
  -c 256
  --dither 0
)
# Setup directories
mkdir -p "$OUTPUT_DIR" "$TEMP_DIR"
echo "📂 Created directories: $OUTPUT_DIR, $TEMP_DIR"

process_rgb565() {
    EXT="dt"
    echo "📁 Checking for existing $EXT frames..."
    
    if compgen -G "$OUTPUT_DIR/frame*.${EXT}" >/dev/null; then
        echo "✅ Found preconverted .${EXT} frames, skipping frame extraction and conversion."
        return 0
    fi

    # Extract frames with optimized settings
    echo "🖼️ Extracting frames @ ${FPS}fps, ${WIDTH}x${HEIGHT} RGB24..."
    ffmpeg "${FFMPEG_OPTS[@]}" -pix_fmt rgb24 -start_number 0 "$TEMP_DIR/frame%05d.png" || exit 1

    # Convert frames to VQ-compressed format
    echo "🎞️ Converting frames to VQ-compressed ${EXT}..."
    
    if command -v parallel >/dev/null; then
        # Parallel processing if available
        find "$TEMP_DIR" -name 'frame*.png' -print0 | \
            parallel -0 -j "$THREADS" --bar \
            "$PVRTX -i {} -o $OUTPUT_DIR/{/.}.$EXT ${PVRTX_OPTS[*]} $PVRTX_QUIET"
    else
        # Sequential fallback
        local frame_idx=0
        for png in "$TEMP_DIR"/frame*.png; do
            local base=$(printf "frame%05d" "$frame_idx")
            $PVRTX -i "$png" -o "$OUTPUT_DIR/${base}.${EXT}" "${PVRTX_OPTS[@]}" $PVRTX_QUIET || exit 1
            ((frame_idx++))
        done
    fi
}

process_yuv420p() {
    EXT="bin"
    local FRAME_SIZE=$((WIDTH * HEIGHT * 3 / 2))
    
    echo "🎥 Extracting raw YUV420p frames..."
    ffmpeg "${FFMPEG_OPTS[@]}" -pix_fmt yuv420p -an "$TEMP_DIR/full.yuv" || exit 1

    echo "✂️ Splitting raw YUV420p into individual frames..."
    split -b "$FRAME_SIZE" -d -a 5 "$TEMP_DIR/full.yuv" "$TEMP_DIR/frame" --additional-suffix=".yuv"

    echo "🔀 Converting YUV frames to PVR macroblock format..."
    local frame_idx=0
    for yuv in "$TEMP_DIR"/frame*.yuv; do
        local frame_num=$(printf "%05d" "$frame_idx")
        local out_bin="$OUTPUT_DIR/frame${frame_num}.bin"
        
        $YUVCONVERTER "$yuv" "$out_bin" "$WIDTH" "$HEIGHT" -q || exit 1
        ((frame_idx++))
    done
}

# Main processing
case "$FORMAT" in
    rgb565)
        process_rgb565
        ;;
    yuv420p)
        process_yuv420p
        ;;
    *)
        echo "❌ Unknown format: $FORMAT"
        exit 1
        ;;
esac

TOTAL_FRAMES=$frame_idx
echo "✅ Converted $TOTAL_FRAMES frames."

# Extract and convert audio
echo "🔊 Extracting and converting audio to ADPCM (channels=${CHANNELS}, rate=${AUDIO_RATE})..."
ffmpeg -hide_banner -loglevel error -i "$INPUT" -ac "$CHANNELS" -ar "$AUDIO_RATE" -c:a pcm_s16le -y "$TEMP_DIR/temp.wav"
"$DCACONV" --long --rate "$AUDIO_RATE" -c "$CHANNELS" -f ADPCM \
  -i "$TEMP_DIR/temp.wav" -o "$TEMP_DIR/audio.dca" || exit 1

# Pack video frames + audio into compressed .dcmv format
echo "📦 Packing into compressed .dcmv format..."
"$PACKER" "./playdcmv/movie.dcmv" "$FRAME_TYPE" "$WIDTH" "$HEIGHT" "$FPS" "$AUDIO_RATE" "$CHANNELS" \
  "$OUTPUT_DIR/frame%05d.${EXT}" "$TEMP_DIR/audio.dca" || exit 1

# Clean up intermediate files
# echo "🧹 Cleaning up temporary files..."
# rm -rf "$OUTPUT_DIR"
# rm -rf "$TEMP_DIR"

echo "✅ Final .dcmv + audio.dca created:"
ls -lh ./playdcmv/movie.dcmv

