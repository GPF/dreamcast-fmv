#!/usr/bin/env python3
"""
analyze_visual_duplicates.py
----------------------------
Scan PNG frames (e.g., frame00000.png ... frame31437.png),
detect visually duplicate frames (within a threshold),
and report potential compression savings.
"""

import os
import sys
from PIL import Image, ImageChops
import numpy as np

THRESHOLD = 1.0  # percentage difference below which frames are considered duplicates

def difference_percentage(img1, img2):
    """Calculate average pixel difference percentage between two images."""
    diff = ImageChops.difference(img1, img2)
    np_diff = np.array(diff, dtype=np.int16)
    total_diff = np.sum(np_diff) / (np_diff.size * 255)
    return total_diff * 100

def analyze_frames(directory):
    all_frames = sorted([os.path.join(directory, f)
                         for f in os.listdir(directory)
                         if f.lower().endswith(".png")])

    if not all_frames:
        print(f"No PNG frames found in {directory}")
        return

    last_unique_frame = Image.open(all_frames[0]).convert("RGB")
    total_frames = len(all_frames)
    unique_frames = 1
    duplicates = 0

    for i, frame_path in enumerate(all_frames[1:], start=1):
        current_frame = Image.open(frame_path).convert("RGB")
        diff = difference_percentage(last_unique_frame, current_frame)

        if diff < THRESHOLD:
            duplicates += 1
        else:
            unique_frames += 1
            last_unique_frame = current_frame

        if (i + 1) % 500 == 0:
            print(f"Processed {i+1}/{total_frames} frames...")

    duplicate_percentage = (duplicates / total_frames) * 100 if total_frames > 0 else 0

    print("\nAnalysis Complete:")
    print(f"Total frames:     {total_frames}")
    print(f"Unique frames:    {unique_frames}")
    print(f"Duplicate frames: {duplicates} ({duplicate_percentage:.2f}%)")
    print(f"Potential reduction: {(duplicates / total_frames) * 100:.2f}%")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 analyze_visual_duplicates.py /path/to/temp_frames")
        sys.exit(1)
    analyze_frames(sys.argv[1])
