#!/bin/bash
set -euo pipefail

INPUT_DIR="${1:-input/dvd}"
FINAL_DIR="${2:-playdcmv}"
WORK_ROOT="${WORK_ROOT:-output/dvd_batch}"
TEMP_ROOT="${TEMP_ROOT:-temp_frames/dvd_batch}"

mkdir -p "$FINAL_DIR" "$WORK_ROOT" "$TEMP_ROOT"

shopt -s nullglob
videos=("$INPUT_DIR"/*.m2v)

if [ "${#videos[@]}" -eq 0 ]; then
    echo "No .m2v files found in $INPUT_DIR"
    exit 1
fi

for video in "${videos[@]}"; do
    base=$(basename "$video" .m2v)
    audio="$INPUT_DIR/$base.ogg"
    final="$FINAL_DIR/$base.dcmv"
    output_dir="$WORK_ROOT/$base"
    temp_dir="$TEMP_ROOT/$base"

    if [ ! -f "$audio" ]; then
        echo "Skipping $video: missing matching audio file $audio"
        continue
    fi

    echo "=== Converting $base -> $final ==="
    rm -rf "$output_dir" "$temp_dir"

    INPUT="$video" \
    AUDIOINPUT="$audio" \
    OUTPUT_DIR="$output_dir" \
    UNIQUE_FRAMES="$output_dir/unique_frames" \
    TEMP_DIR="$temp_dir" \
    FINAL_OUTPUT="$final" \
    SKIP_IF_EXISTS=false \
    ./convert_to_pvr_fmv.sh
done

echo "Batch conversion complete."
