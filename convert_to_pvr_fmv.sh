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
# AUDIOINPUT="input/dle.ogg" # Example for audio input
INPUT="input/60_JW4_Imagine_SB_UPRW255017EH-thedigitaltheater.mp4" # Path to input video file (MP4)
AUDIOINPUT=$INPUT
OUTPUT_DIR="output"
UNIQUE_FRAMES="$OUTPUT_DIR/unique_frames"
TEMP_DIR="temp_frames"
FINAL_OUTPUT="./playdcmv/movie.dcmv"

# Video Settings
FPS=23.98
FORMAT="yuv422" # Options: rgb565, yuv422
USE_STRIDED=true # true = 640x480 strided, false = 512x256 POT with padding

# Texture Dimensions
SCALE_WIDTH=640 # Content dimensions (always 320x240 for 4:3)
SCALE_HEIGHT=480

# Frame Range Control
# Set to "all" (or "last") to process the entire video.
# VIDEO_FRAMES="99999" # Default to process all frames
# VIDEO_FRAMES=31438 # Example: Stop at frame 31438 (1-indexed) skip the unused frames in Dragon's Lair

# Audio Settings
AUDIO_RATE=44100
CHANNELS=2

USE_DEDUP=false  # Set to false to disable frame deduplication
COMPRESSION_BACKEND="lz4" # Options: lz4, zstd
CHUNK_DURATION="${CHUNK_DURATION:-0.5}"  # Default 2.0, override with env variable if needed

if [ "$USE_STRIDED" = true ]; then
    WIDTH=$SCALE_WIDTH # Direct strided texture
    HEIGHT=$SCALE_HEIGHT
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
INTERMEDIATE_FORMAT="tga" # TGA is the most practical choice

# ==================== END CONFIGURATION ====================

# Calculated values
PAD_X=$(( (WIDTH - SCALE_WIDTH) / 2 ))
PAD_Y=$(( (HEIGHT - SCALE_HEIGHT) / 2 ))

# Setup directories
mkdir -p "$OUTPUT_DIR" "$TEMP_DIR" "$UNIQUE_FRAMES"
echo "📂 Created directories: $OUTPUT_DIR, $TEMP_DIR, $UNIQUE_FRAMES"

if [ "$USE_STRIDED" = true ]; then
    echo "📐 Using STRIDED texture mode: ${WIDTH}x${HEIGHT} (no padding)"
else
    echo "📐 Using POT texture mode: ${WIDTH}x${HEIGHT} with ${SCALE_WIDTH}x${SCALE_HEIGHT} content (pad: ${PAD_X}x${PAD_Y})"
fi

# Determine FFmpeg pixel format for intermediate TGAs
FFMPEG_PIX_FMT=""
case "$FORMAT" in
    "rgb565")
        FFMPEG_PIX_FMT="rgba48be"
        ;;
    "yuv422")
        FFMPEG_PIX_FMT="rgba48be"
        ;;
    *)
        echo "Error: Invalid FORMAT specified. Use 'rgb565' or 'yuv422'."
        exit 1
        ;;
esac

# Build FFmpeg filter chain based on texture mode and frame limits
# build_ffmpeg_opts() {
#     local base_opts=(
#         -hide_banner
#         -loglevel "$FFMPEG_LOGLEVEL"
#         -y
#         -i "$INPUT"
#     )

#     local filter_chain=""
#     if [ "$USE_STRIDED" = true ]; then
#         filter_chain="scale=${SCALE_WIDTH}:${SCALE_HEIGHT}:flags=lanczos"
#     else
#         filter_chain="scale=${SCALE_WIDTH}:${SCALE_HEIGHT}:flags=lanczos,pad=${WIDTH}:${HEIGHT}:${PAD_X}:${PAD_Y}:black"
#     fi

#     # Keep hqdn3d as it's a good quality filter
#     filter_chain="${filter_chain},hqdn3d=0.8:0.6:4.0:3.0"

#     # Frame limiting: Use -frames:v for robustness
#     local output_frame_limit_opts=()

#     if [[ "$VIDEO_FRAMES" =~ ^[0-9]+$ ]]; then
#         # If VIDEO_FRAMES is a number, use -frames:v directly
#         echo "✂️ Limiting video to $VIDEO_FRAMES frames using -frames:v."
#         output_frame_limit_opts+=("-frames:v" "$VIDEO_FRAMES")
#     else
#         echo "🎥 Processing all frames."
#     fi

#     # Construct the final FFMPEG_OPTS array
#     FFMPEG_OPTS=(
#         "${base_opts[@]}"
#         -vf "$filter_chain"
#         -pix_fmt "$FFMPEG_PIX_FMT"
#         -sws_flags "+accurate_rnd+full_chroma_int+full_chroma_inp"
#         -r "$FPS"
#         -start_number 0
#         "${output_frame_limit_opts[@]}"
#     )
# }

# Build FFmpeg filter chain - MINIMAL VERSION
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

    # Frame limiting
    local output_frame_limit_opts=()
    if [[ "$VIDEO_FRAMES" =~ ^[0-9]+$ ]]; then
        echo "✂️ Limiting video to $VIDEO_FRAMES frames using -frames:v."
        output_frame_limit_opts+=("-frames:v" "$VIDEO_FRAMES")
    else
        echo "🎥 Processing all frames."
    fi

    # MINIMAL FFMPEG_OPTS
    FFMPEG_OPTS=(
        "${base_opts[@]}"
        -vf "$filter_chain"
        -start_number 0
        "${output_frame_limit_opts[@]}"
    )
}
process_rgb565() {
    EXT="dt"
    FRAME_TYPE=0 # RGB565

    # Build FFmpeg options
    build_ffmpeg_opts

    # Extract frames if not already extracted
    if ! compgen -G "$UNIQUE_FRAMES/frame*.$INTERMEDIATE_FORMAT" >/dev/null; then
        echo "🖼️ Extracting frames @ ${FPS}fps, ${WIDTH}x${HEIGHT} as ${FFMPEG_PIX_FMT} TGAs..."
        ffmpeg "${FFMPEG_OPTS[@]}" "$TEMP_DIR/frame%06d.$INTERMEDIATE_FORMAT" || exit 1
    else
        echo "✅ Found extracted frames in $TEMP_DIR, skipping ffmpeg extraction."
    fi

    # Deduplicate frames regardless of .dt files
    if [ "$USE_DEDUP" = true ]; then
        if [ "$SKIP_IF_EXISTS" = true ] && compgen -G "$UNIQUE_FRAMES/frame*.${INTERMEDIATE_FORMAT}" >/dev/null; then
            echo "✅ Found unique frames, skipping deduplication."
        else
            echo "🔍 Running frame deduplication..."
            rm -rf "$UNIQUE_FRAMES"
            python3 ./generate_durations.py "$TEMP_DIR" "$UNIQUE_FRAMES" .5 || exit 1
        fi
    else
        echo "🧱 Deduplication disabled. Copying frames safely and generating frame_durations.txt..."

        rm -rf "$UNIQUE_FRAMES"
        mkdir -p "$UNIQUE_FRAMES"

        find "$TEMP_DIR" -name "frame*.${INTERMEDIATE_FORMAT}" -print0 \
        | xargs -0 cp -t "$UNIQUE_FRAMES"

        num_frames=$(find "$UNIQUE_FRAMES" -maxdepth 1 -name "frame*.${INTERMEDIATE_FORMAT}" | wc -l)
        yes 1 | head -n "$num_frames" > "$UNIQUE_FRAMES/frame_durations.txt"

    fi

    # Convert unique frames if no .dt files exist
    if [ "$SKIP_IF_EXISTS" = true ] && compgen -G "$OUTPUT_DIR/frame*.${EXT}" >/dev/null; then
        echo "✅ Found preconverted .${EXT} frames, skipping pvrtex conversion."
        return 0
    fi

    echo "🎞️ Converting unique frames to VQ-compressed ${EXT} (RGB565)..."
    local pvrtx_opts=(-f RGB565 -c)
    [ "$USE_STRIDED" = true ] && pvrtx_opts+=(-s)
    [ "$PVRTX_DITHER" -eq 1 ] && pvrtx_opts+=(--dither)

    if command -v parallel >/dev/null; then
        find "$UNIQUE_FRAMES" -name "frame*.$INTERMEDIATE_FORMAT" -print0 | \
            parallel -0 -j "$THREADS" --bar \
            "$PVRTX -i {} -o $OUTPUT_DIR/{/.}.$EXT ${pvrtx_opts[*]} $PVRTX_QUIET"
    else
        local frame_idx=0
        for intermediate_file in "$UNIQUE_FRAMES"/frame*.$INTERMEDIATE_FORMAT; do
            local base=$(printf "frame%06d" "$frame_idx")
            $PVRTX -i "$intermediate_file" -o "$OUTPUT_DIR/${base}.${EXT}" "${pvrtx_opts[@]}" $PVRTX_QUIET || exit 1
            ((frame_idx++))
        done
    fi
}

process_yuv422() {
    EXT="dt"
    FRAME_TYPE=1 # YUV422

    # Build FFmpeg options
    build_ffmpeg_opts

    # Extract frames if temp_frames is empty
    if ! compgen -G "$TEMP_DIR/frame*.$INTERMEDIATE_FORMAT" >/dev/null && \
       ! compgen -G "$UNIQUE_FRAMES/frame*.$INTERMEDIATE_FORMAT" >/dev/null; then
        echo "🖼️ Extracting frames @ ${FPS}fps, ${WIDTH}x${HEIGHT} as ${FFMPEG_PIX_FMT} TGAs..."
        ffmpeg "${FFMPEG_OPTS[@]}" "$TEMP_DIR/frame%06d.$INTERMEDIATE_FORMAT" || exit 1
    else
        echo "✅ Found extracted frames, skipping ffmpeg extraction."
    fi

    # Deduplicate frames
    if [ "$USE_DEDUP" = true ]; then
        if [ "$SKIP_IF_EXISTS" = true ] && compgen -G "$UNIQUE_FRAMES/frame*.${INTERMEDIATE_FORMAT}" >/dev/null; then
            echo "✅ Found unique frames, skipping deduplication."
        else
            echo "🔍 Running frame deduplication..."
            rm -rf "$UNIQUE_FRAMES"
            python3 ./generate_durations.py "$TEMP_DIR" "$UNIQUE_FRAMES" .08 || exit 1
        fi
    else
        if [ "$SKIP_IF_EXISTS" = true ] && compgen -G "$UNIQUE_FRAMES/frame*.${INTERMEDIATE_FORMAT}" >/dev/null; then
            echo "✅ Found frames in unique_frames, skipping move."
        else
            echo "🧱 Deduplication disabled. Moving frames and generating frame_durations.txt..."

            rm -rf "$UNIQUE_FRAMES"
            mkdir -p "$UNIQUE_FRAMES"

            find "$TEMP_DIR" -name "frame*.${INTERMEDIATE_FORMAT}" -print0 \
            | xargs -0 mv -t "$UNIQUE_FRAMES"

            num_frames=$(find "$UNIQUE_FRAMES" -maxdepth 1 -name "frame*.${INTERMEDIATE_FORMAT}" | wc -l)
            yes 1 | head -n "$num_frames" > "$UNIQUE_FRAMES/frame_durations.txt"
        fi
    fi

    # Convert unique frames to VQ-compressed format
    if [ "$SKIP_IF_EXISTS" = true ] && compgen -G "$OUTPUT_DIR/frame*.${EXT}" >/dev/null; then
        echo "✅ Found preconverted .${EXT} frames, skipping pvrtex conversion."
        return 0
    fi

    echo "🎞️ Converting unique frames to VQ-compressed ${EXT} (YUV422)..."
    local pvrtx_opts=(-f YUV -c)
    [ "$USE_STRIDED" = true ] && pvrtx_opts+=(-s)
    [ "$PVRTX_DITHER" -eq 1 ] && pvrtx_opts+=(--dither)

    if command -v parallel >/dev/null; then
        find "$UNIQUE_FRAMES" -name "frame*.$INTERMEDIATE_FORMAT" -print0 | \
            parallel -0 -j "$THREADS" --bar \
            "$PVRTX -i {} -o $OUTPUT_DIR/{/.}.$EXT ${pvrtx_opts[*]} $PVRTX_QUIET"
    else
        local frame_idx=0
        for intermediate_file in "$UNIQUE_FRAMES"/frame*.$INTERMEDIATE_FORMAT; do
            local base=$(printf "frame%06d" "$frame_idx")
            $PVRTX -i "$intermediate_file" -o "$OUTPUT_DIR/${base}.${EXT}" "${pvrtx_opts[@]}" $PVRTX_QUIET || exit 1
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
    
    # Test: Use FFmpeg's adpcm_yamaha with WAV format
    # ffmpeg -hide_banner -loglevel error -i "$AUDIOINPUT" \
    #   -ac "$CHANNELS" -ar "$AUDIO_RATE" \
    #   -c:a adpcm_yamaha -f wav -y "$AUDIO_OUT"
    
    # Original method (commented out for comparison)
    ffmpeg -hide_banner -loglevel error -i "$AUDIOINPUT" -ac "$CHANNELS" -ar "$AUDIO_RATE" -c:a pcm_s16le -y "$TEMP_DIR/temp.wav"
    "$DCACONV" --long --rate "$AUDIO_RATE" -c "$CHANNELS" -f ADPCM \
      -i "$TEMP_DIR/temp.wav" -o "$AUDIO_OUT" || exit 1
fi

echo "📦 Packing into compressed .dcmv format..."
"$PACKER" "$FINAL_OUTPUT" "$FRAME_TYPE" "$WIDTH" "$HEIGHT" "$SCALE_WIDTH" "$SCALE_HEIGHT" "$FPS" "$AUDIO_RATE" "$CHANNELS" \
  "$OUTPUT_DIR/frame%06d.${EXT}" "$AUDIO_OUT" "$UNIQUE_FRAMES/frame_durations.txt" "$COMPRESSION_BACKEND" "$CHUNK_DURATION" || exit 1


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
