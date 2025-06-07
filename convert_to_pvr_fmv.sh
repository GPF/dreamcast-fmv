#!/bin/bash

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
PACKER="./pack_dcmv"
FORMAT="rgb565"  #yuv420p or rgb565

# Prepare folders
mkdir -p "$OUTPUT_DIR"
mkdir -p "$TEMP_DIR"
echo "📂 Created output and temp directories"


if [[ "$FORMAT" == "rgb565" ]]; then
  EXT="dt"
  PVRTX="/opt/toolchains/dc/kos/utils/pvrtex/pvrtex"
  FRAME_TYPE=0

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
      "$PVRTX" -i "$png" -o "$OUTPUT_DIR/${base}.${EXT}" -f RGB565 -c small --dither 1 > /dev/null 2>&1 || exit 1
      ((frame_idx++))
    done
fi
elif [[ "$FORMAT" == "yuv420p" ]]; then
  EXT="bin"
  YUVCONVERTER="./yuv420converter"
  FRAME_TYPE=1

  FRAME_SIZE=$((WIDTH * HEIGHT * 3 / 2))
  # New: Extract raw YUV and split
echo "🎥 Extracting raw YUV420p frames from MP4..."
ffmpeg -hide_banner -loglevel warning -y -i "$INPUT" \
  -vf "fps=${FPS},scale=${WIDTH}:${HEIGHT}" \
  -pix_fmt yuv420p \
  -an \
  "$TEMP_DIR/full.yuv" || exit 1

echo "✂️ Splitting raw YUV420p into individual frames..."
split -b "$FRAME_SIZE" -d -a 4 "$TEMP_DIR/full.yuv" "$TEMP_DIR/frame" --additional-suffix=".yuv"

echo "🔀 Converting YUV frames to PVR macroblock format..."
frame_idx=0
for yuv in "$TEMP_DIR"/frame*.yuv; do
  frame_num=$(printf "%04d" "$frame_idx")
  out_bin="$OUTPUT_DIR/frame${frame_num}.bin"

  "$YUVCONVERTER" "$yuv" "$out_bin" "$WIDTH" "$HEIGHT" -q || exit 1

  ((frame_idx++))
done
else
  echo "❌ Unknown format: $FORMAT"
  exit 1
fi

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
  "$OUTPUT_DIR/frame%04d.${EXT}" "$TEMP_DIR/audio.dca" || exit 1

# Clean up intermediate files
# echo "🧹 Cleaning up temporary files..."
# rm -rf "$OUTPUT_DIR"
# rm -rf "$TEMP_DIR"

echo "✅ Final .dcmv + audio.dca created:"
ls -lh ./playdcmv/movie.dcmv
