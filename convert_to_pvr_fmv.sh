#!/bin/bash

# Settings
INPUT="input/Dreamcast Startup (60fps).mp4"
OUTPUT_DIR="output"
TEMP_DIR="temp_frames"
FPS=30
WIDTH=512
HEIGHT=256
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

# Extract and scale frames
echo "🖼️  Extracting ${WIDTH}x${HEIGHT} PNG frames at ${FPS} fps..."
ffmpeg -hide_banner -loglevel error -y -i "$INPUT" \
  -vf "fps=${FPS},scale=${WIDTH}:${HEIGHT}:flags=lanczos" \
  -start_number 0 "$TEMP_DIR/frame%04d.png" || exit 1

# Convert frames to Dreamcast texture format
echo "🎞️ Converting PNG frames to .${EXT} with VQ compression..."
frame_idx=0
for png in "$TEMP_DIR"/frame*.png; do
  base=$(printf "frame%04d" "$frame_idx")
  "$PVRTX" -i "$png" -o "$OUTPUT_DIR/${base}.${EXT}" -f RGB565 -c small --dither 1 || exit 1
  ((frame_idx++))
done

# Extract and convert audio
echo "🔊 Extracting and converting audio to ADPCM (channels=${CHANNELS}, rate=${AUDIO_RATE})..."
ffmpeg -hide_banner -loglevel error -i "$INPUT" -ac "$CHANNELS" -ar "$AUDIO_RATE" -c:a pcm_s16le -y "$TEMP_DIR/temp.wav"
"$DCACONV" --long --rate "$AUDIO_RATE" -c "$CHANNELS" -f ADPCM \
  -i "$TEMP_DIR/temp.wav" -o "./playdcmv/audio.dca" || exit 1

# Pack video frames + audio into compressed .dcmv format
echo "📦 Packing into compressed .dcmv format..."
"$PACKER" "./playdcmv/movie.dcmv" "$WIDTH" "$HEIGHT" "$FPS" "$AUDIO_RATE" "$CHANNELS" \
  "$OUTPUT_DIR/frame%04d.${EXT}" "./playdcmv/audio.dca" || exit 1

# Clean up intermediate files
echo "🧹 Cleaning up temporary files..."
rm -rf "$OUTPUT_DIR"
rm -rf "$TEMP_DIR"

echo "✅ Final .dcmv + audio.dca created:"
ls -lh ./playdcmv/movie.dcmv ./playdcmv/audio.dca
