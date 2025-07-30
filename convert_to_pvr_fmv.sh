#!/bin/bash
# Debug/Cleanup Settings
CLEANUP_TEMP=false
SKIP_IF_EXISTS=true
#
# convert_to_pvr_fmv.sh - Dreamcast FMV Toolchain Driver Script
# -------------------------------------------------------------
# This script automates the conversion of an MP4 video into a DCMV container
# suitable for playback on the Sega Dreamcast using the `fmv_play.elf` player.
#
# Steps performed:
# 1. Extract RGB frames from input video using `ffmpeg`
# 2. Convert each frame to RGB565/YUV and encode into VQ-compressed PVR textures using `pvrtex`
# 3. Extract and encode audio to Dreamcast ADPCM format using `dcaconv`
# 4. Package the VQ textures and audio into a `.dcmv` container using `pack_dcmv`
#
# Author: Troy E. Davis (GPF) — https://github.com/GPF

# ==================== USER CONFIGURATION ====================

# Input/Output Settings
INPUT="input/dolby-atmos-trailer_amaze_1080.mp4"
# INPUT="input/lair.ogv" # Example for other video
OUTPUT_DIR="output"
TEMP_DIR="temp_frames"
FINAL_OUTPUT="./playdcmv/movie.dcmv"

# Video Settings
FPS=23.97
FORMAT="yuv422" # Options: rgb565, yuv422
USE_STRIDED=true # true = 320x240 strided, false = 512x256 POT with padding

# Texture Dimensions
SCALE_WIDTH=640 # Content dimensions (always 320x240 for 4:3)
SCALE_HEIGHT=480

# Frame Range Control
# Set to "all" (or "last") to process the entire video.
VIDEO_FRAMES="99999" # Default to process all frames
# VIDEO_FRAMES=31438 # Example: Stop at frame 31438 (1-indexed) skip the unused frames in Dragon's Lair

# Audio Settings
AUDIO_RATE=44100
CHANNELS=2

if [ "$USE_STRIDED" = true ]; then
    WIDTH=640 # Direct strided texture
    HEIGHT=480
else
    WIDTH=512 # POT texture with padding
    HEIGHT=256
fi

# Tool Paths
PVRTX="/opt/toolchains/dc/kos/utils/pvrtex/pvrtex"
DCACONV="./dcaconv" # https://github.com/TapamN/dcaconv
PACKER="./pack_dcmv"

# Performance Settings
THREADS=$(nproc) # Auto-detect CPU cores
FFMPEG_LOGLEVEL="warning" # Options: error, warning, info
PVRTX_QUIET=">/dev/null 2>&1" # Set to "" to see pvrtex output

# Dithering Settings
USE_FFMPEG_DITHER=false # Best to let pvrtex handle dithering for final conversion
PVRTX_DITHER=1 # 0 = no dithering, 1 = enable (recommended for RGB565 from pvrtex)

# Intermediate file format
INTERMEDIATE_FORMAT="png" # PNG is the most practical choice

# ==================== END CONFIGURATION ====================

# Calculated values
PAD_X=$(( (WIDTH - SCALE_WIDTH) / 2 ))
PAD_Y=$(( (HEIGHT - SCALE_HEIGHT) / 2 ))

# Setup directories
mkdir -p "$OUTPUT_DIR" "$TEMP_DIR"
echo "📂 Created directories: $OUTPUT_DIR, $TEMP_DIR"

if [ "$USE_STRIDED" = true ]; then
    echo "📐 Using STRIDED texture mode: ${WIDTH}x${HEIGHT} (no padding)"
else
    echo "📐 Using POT texture mode: ${WIDTH}x${HEIGHT} with ${SCALE_WIDTH}x${SCALE_HEIGHT} content (pad: ${PAD_X}x${PAD_Y})"
fi

# Determine FFmpeg pixel format for intermediate PNGs
FFMPEG_PIX_FMT=""
case "$FORMAT" in
    "rgb565")
        FFMPEG_PIX_FMT="rgba64be"
        ;;
    "yuv422")
        FFMPEG_PIX_FMT="rgba64be"
        ;;
    *)
        echo "Error: Invalid FORMAT specified. Use 'rgb565' or 'yuv422'."
        exit 1
        ;;
esac

# Build FFmpeg filter chain based on texture mode and frame limits
build_ffmpeg_opts() {
    local base_opts=(
        -hide_banner
        -loglevel "$FFMPEG_LOGLEVEL"
        -y
        -i "$INPUT"
    )

    local filter_chain=""
    if [ "$USE_STRIDED" = true ]; then
        filter_chain="scale=${SCALE_WIDTH}:${SCALE_HEIGHT}:flags=lanczos"
    else
        filter_chain="scale=${SCALE_WIDTH}:${SCALE_HEIGHT}:flags=lanczos,pad=${WIDTH}:${HEIGHT}:${PAD_X}:${PAD_Y}:black"
    fi

    # Keep hqdn3d as it's a good quality filter
    filter_chain="${filter_chain},hqdn3d=0.8:0.6:4.0:3.0"

    # Frame limiting: Use -frames:v for robustness
    local output_frame_limit_opts=()

    if [[ "$VIDEO_FRAMES" =~ ^[0-9]+$ ]]; then
        # If VIDEO_FRAMES is a number, use -frames:v directly
        echo "✂️ Limiting video to $VIDEO_FRAMES frames using -frames:v."
        output_frame_limit_opts+=("-frames:v" "$VIDEO_FRAMES")
    else
        echo "🎥 Processing all frames."
    fi

    # Construct the final FFMPEG_OPTS array
    FFMPEG_OPTS=(
        "${base_opts[@]}"
        -vf "$filter_chain"
        -pix_fmt "$FFMPEG_PIX_FMT"
        -sws_flags "+accurate_rnd+full_chroma_int+full_chroma_inp"
        -r "$FPS"
        -start_number 0
        "${output_frame_limit_opts[@]}"
    )
}

process_rgb565() {
    EXT="dt"
    FRAME_TYPE=0 # Corresponds to RGB565 in your packer
    echo "📁 Checking for existing $EXT frames..."

    if [ "$SKIP_IF_EXISTS" = true ] && compgen -G "$OUTPUT_DIR/frame*.${EXT}" >/dev/null; then
        echo "✅ Found preconverted .${EXT} frames, skipping frame extraction and conversion."
        return 0
    fi

    # Build FFmpeg options
    build_ffmpeg_opts

    # Extract frames as high-precision PNGs
    echo "🖼️ Extracting frames @ ${FPS}fps, ${WIDTH}x${HEIGHT} as ${FFMPEG_PIX_FMT} PNGs..."
    ffmpeg "${FFMPEG_OPTS[@]}" "$TEMP_DIR/frame%05d.$INTERMEDIATE_FORMAT" || exit 1

    # Convert frames to VQ-compressed format using pvrtex
    echo "🎞️ Converting frames to VQ-compressed ${EXT} (RGB565)..."

    local pvrtx_opts=(-f RGB565 -c) # Base pvrtex options for RGB565 VQ
    if [ "$USE_STRIDED" = true ]; then
        pvrtx_opts+=(-s) # Add strided flag
    fi
    if [ "$PVRTX_DITHER" -eq 1 ]; then
        pvrtx_opts+=(--dither) # Add dithering flag for pvrtex
    fi

    if command -v parallel >/dev/null; then
        find "$TEMP_DIR" -name "frame*.$INTERMEDIATE_FORMAT" -print0 | \
            parallel -0 -j "$THREADS" --bar \
            "$PVRTX -i {} -o $OUTPUT_DIR/{/.}.$EXT ${pvrtx_opts[*]} $PVRTX_QUIET"
    else
        local frame_idx=0
        for intermediate_file in "$TEMP_DIR"/frame*.$INTERMEDIATE_FORMAT; do
            local base=$(printf "frame%05d" "$frame_idx")
            $PVRTX -i "$intermediate_file" -o "$OUTPUT_DIR/${base}.${EXT}" "${pvrtx_opts[@]}" $PVRTX_QUIET || exit 1
            ((frame_idx++))
        done
    fi
}

process_yuv422() {
    EXT="dt"
    FRAME_TYPE=1 # Corresponds to YUV422 in your packer
    echo "📁 Checking for existing $EXT frames..."

    if [ "$SKIP_IF_EXISTS" = true ] && compgen -G "$OUTPUT_DIR/frame*.${EXT}" >/dev/null; then
        echo "✅ Found preconverted .${EXT} frames, skipping frame extraction and conversion."
        return 0
    fi

    # Build FFmpeg options
    build_ffmpeg_opts

    # Extract frames as high-precision PNGs
    echo "🖼️ Extracting frames @ ${FPS}fps, ${WIDTH}x${HEIGHT} as ${FFMPEG_PIX_FMT} PNGs..."
    ffmpeg "${FFMPEG_OPTS[@]}" "$TEMP_DIR/frame%05d.$INTERMEDIATE_FORMAT" || exit 1

    # Convert frames to VQ-compressed format with YUV using pvrtex
    echo "🎞️ Converting frames to VQ-compressed ${EXT} (YUV422)..."

    local pvrtx_opts=(-f YUV -c) # Base pvrtex options for YUV VQ
    if [ "$USE_STRIDED" = true ]; then
        pvrtx_opts+=(-s) # Add strided flag
    fi
    if [ "$PVRTX_DITHER" -eq 1 ]; then
        pvrtx_opts+=(--dither) # Add dithering flag for pvrtex (if applicable for YUV)
    fi

    if command -v parallel >/dev/null; then
        find "$TEMP_DIR" -name 'frame*.png' -print0 | \
            parallel -0 -j "$THREADS" --bar \
            "$PVRTX -i {} -o $OUTPUT_DIR/{/.}.$EXT ${pvrtx_opts[*]} $PVRTX_QUIET"
    else
        local frame_idx=0
        for png in "$TEMP_DIR"/frame*.png; do
            local base=$(printf "frame%05d" "$frame_idx")
            $PVRTX -i "$png" -o "$OUTPUT_DIR/${base}.${EXT}" "${pvrtx_opts[@]}" $PVRTX_QUIET || exit 1
            ((frame_idx++))
        done
    fi
}

# Main processing
case "$FORMAT" in
    rgb565)
        process_rgb565
        ;;
    yuv422)
        process_yuv422
        ;;
    *)
        echo "❌ Unknown format: $FORMAT (supported: rgb565, yuv422)"
        exit 1
        ;;
esac

echo "✅ Converted frames complete."

# Extract and convert audio
AUDIO_OUT="$OUTPUT_DIR/audio.dca"
if [ "$SKIP_IF_EXISTS" = true ] && [[ -f "$AUDIO_OUT" ]]; then
    echo "✅ Found existing audio.dca, skipping audio extraction."
else
    echo "🔊 Extracting and converting audio to ADPCM (channels=${CHANNELS}, rate=${AUDIO_RATE})..."
    ffmpeg -hide_banner -loglevel error -i "$INPUT" -ac "$CHANNELS" -ar "$AUDIO_RATE" -c:a pcm_s16le -y "$TEMP_DIR/temp.wav"
    "$DCACONV" --long --rate "$AUDIO_RATE" -c "$CHANNELS" -f ADPCM \
      -i "$TEMP_DIR/temp.wav" -o "$AUDIO_OUT" || exit 1
fi

# Pack video frames + audio into compressed .dcmv format
echo "📦 Packing into compressed .dcmv format..."
"$PACKER" "$FINAL_OUTPUT" "$FRAME_TYPE" "$WIDTH" "$HEIGHT" "$SCALE_WIDTH" "$SCALE_HEIGHT" "$FPS" "$AUDIO_RATE" "$CHANNELS" \
  "$OUTPUT_DIR/frame%05d.${EXT}" "$AUDIO_OUT" || exit 1

# Clean up intermediate files
if [ "$CLEANUP_TEMP" = true ]; then
    echo "🧹 Cleaning up temporary files..."
    rm -rf "$TEMP_DIR"
    if [ "$SKIP_IF_EXISTS" = false ]; then
        rm -rf "$OUTPUT_DIR"
    fi
fi

echo "✅ Final .dcmv created:"
ls -lh "$FINAL_OUTPUT"