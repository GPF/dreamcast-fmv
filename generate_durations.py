#!/usr/bin/env python3
"""
generate_durations.py
---------------------
Analyzes a sequence of PNG frames (e.g., frame00000.png to frameXXXXX.png),
detects visually duplicate frames using a threshold, and outputs:

1. A `frame_durations.txt` file containing repeat counts for each unique frame.
2. Copies all unique frames into a specified output folder for pvrtex conversion.

Usage:
    python3 generate_durations.py <input_frames_dir> <unique_frames_dir> [threshold]

Example:
    python3 generate_durations.py temp_frames unique_frames 0.5
"""

import os
import sys
import shutil
from PIL import Image, ImageChops
import numpy as np

def difference_percentage(img1, img2):
    """Compute the average pixel difference as a percentage."""
    diff = ImageChops.difference(img1, img2)
    np_diff = np.array(diff, dtype=np.int16)
    total_diff = np.sum(np_diff) / (np_diff.size * 255)
    return total_diff * 100

def generate_durations(input_dir, output_dir, threshold=0.5):
    frames = sorted([f for f in os.listdir(input_dir) if f.lower().endswith(".png")])
    if not frames:
        print(f"No PNG frames found in {input_dir}")
        sys.exit(1)

    os.makedirs(output_dir, exist_ok=True)
    durations = []
    unique_count = 0

    # Start with the first frame
    first_frame_path = os.path.join(input_dir, frames[0])
    last_unique = Image.open(first_frame_path).convert("RGB")
    durations.append(1)
    unique_out_path = os.path.join(output_dir, f"frame{unique_count:05d}.png")
    shutil.copy2(first_frame_path, unique_out_path)
    unique_count += 1

    for i, fname in enumerate(frames[1:], start=1):
        current_path = os.path.join(input_dir, fname)
        current = Image.open(current_path).convert("RGB")
        diff = difference_percentage(last_unique, current)

        if diff < threshold:
            durations[-1] += 1
        else:
            durations.append(1)
            last_unique = current
            unique_out_path = os.path.join(output_dir, f"frame{unique_count:05d}.png")
            shutil.copy2(current_path, unique_out_path)
            unique_count += 1

        if (i + 1) % 500 == 0 or (i + 1) == len(frames):
            print(f"   Processed {i+1}/{len(frames)} frames ({(i+1)/len(frames)*100:.1f}%)", end='\r', flush=True)
    print()  

    # Write frame_durations.txt
    durations_path = os.path.join(output_dir, "frame_durations.txt")
    with open(durations_path, "w") as f:
        f.write(",".join(map(str, durations)))

    print("\nDeduplication Complete:")
    print(f" Total frames:     {len(frames)}")
    print(f" Unique frames:    {unique_count}")
    print(f" Duplicate frames: {len(frames) - unique_count} "
          f"({(len(frames)-unique_count)/len(frames)*100:.2f}%)")
    print(f" Frame durations saved to: {durations_path}")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python3 generate_durations.py <input_frames_dir> <unique_frames_dir> [threshold]")
        sys.exit(1)

    input_dir = sys.argv[1]
    output_dir = sys.argv[2]
    threshold = float(sys.argv[3]) if len(sys.argv) > 3 else 0.5
    generate_durations(input_dir, output_dir, threshold)
