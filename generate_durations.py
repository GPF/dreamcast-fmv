#!/usr/bin/env python3
"""
generate_durations.py
---------------------
Analyzes a sequence of TGA frames with improved duplicate detection.
"""

import os
import sys
import shutil
from PIL import Image, ImageChops
import numpy as np

def difference_percentage_v1(img1, img2):
    """Original method - compute average pixel difference as percentage."""
    diff = ImageChops.difference(img1, img2)
    np_diff = np.array(diff, dtype=np.float32)
    # Average difference per pixel channel, as percentage of max possible (255)
    avg_diff = np.mean(np_diff) / 255.0 * 100
    return avg_diff

def difference_percentage_v2(img1, img2):
    """RMS (Root Mean Square) difference method."""
    diff = ImageChops.difference(img1, img2)
    np_diff = np.array(diff, dtype=np.float32)
    # RMS difference as percentage
    rms_diff = np.sqrt(np.mean(np_diff ** 2)) / 255.0 * 100
    return rms_diff

def difference_percentage_v3(img1, img2):
    """Perceptual difference - weight changes more heavily."""
    diff = ImageChops.difference(img1, img2)
    np_diff = np.array(diff, dtype=np.float32)
    # Only count pixels with significant differences (>= 5 pixel values)
    significant_diff = np_diff[np_diff >= 5.0]
    if len(significant_diff) == 0:
        return 0.0
    percent_changed = (len(significant_diff) / np_diff.size) * 100
    avg_change = np.mean(significant_diff) / 255.0 * 100
    # Combine: how many pixels changed significantly * how much they changed
    return percent_changed * (avg_change / 100.0)

def difference_percentage_histogram(img1, img2):
    """Histogram-based comparison - good for compression artifacts."""
    # Convert to numpy arrays
    arr1 = np.array(img1, dtype=np.float32)
    arr2 = np.array(img2, dtype=np.float32)
    
    # Calculate histogram differences for each channel
    total_diff = 0
    for channel in range(3):  # RGB
        hist1, _ = np.histogram(arr1[:,:,channel], bins=256, range=(0, 255))
        hist2, _ = np.histogram(arr2[:,:,channel], bins=256, range=(0, 255))
        # Normalize histograms
        hist1 = hist1.astype(np.float32) / np.sum(hist1)
        hist2 = hist2.astype(np.float32) / np.sum(hist2)
        # Calculate chi-square distance
        total_diff += np.sum((hist1 - hist2) ** 2 / (hist1 + hist2 + 1e-10))
    
    return total_diff * 100 / 3  # Average across channels

def generate_durations(input_dir, output_dir, threshold=0.5):
    frames = sorted([f for f in os.listdir(input_dir) if f.lower().endswith(".tga")])
    if not frames:
        print(f"No TGA frames found in {input_dir}")
        sys.exit(1)

    # Use the improved v1 method by default (you can change this line to test others)
    diff_func = difference_percentage_v1
    print(f"Using improved difference calculation with threshold: {threshold}% (type: {type(threshold)})")
    print(f"Debug: sys.argv = {sys.argv}")

    os.makedirs(output_dir, exist_ok=True)
    durations = []
    unique_count = 0

    # Start with the first frame
    first_frame_path = os.path.join(input_dir, frames[0])
    prev_frame = Image.open(first_frame_path).convert("RGB")
    durations.append(1)
    unique_out_path = os.path.join(output_dir, f"frame{unique_count:05d}.tga")
    shutil.copy2(first_frame_path, unique_out_path)
    unique_count += 1

    # Keep track of statistics
    differences = []

    for i, fname in enumerate(frames[1:], start=1):
        current_path = os.path.join(input_dir, fname)
        current = Image.open(current_path).convert("RGB")
        
        diff = diff_func(prev_frame, current)
        differences.append(diff)
        
        # Debug output for specific frames
        if i in range(124, 128):
            print(f"Frame {i}: diff = {diff:.6f}% (threshold: {threshold}%, is_duplicate: {diff < threshold})")
        
        if diff < threshold:
            durations[-1] += 1
        else:
            durations.append(1)
            unique_out_path = os.path.join(output_dir, f"frame{unique_count:05d}.tga")
            shutil.copy2(current_path, unique_out_path)
            unique_count += 1

        prev_frame = current

        if (i + 1) % 500 == 0 or (i + 1) == len(frames):
            print(f"   Processed {i+1}/{len(frames)} frames ({(i+1)/len(frames)*100:.1f}%)", end='\r', flush=True)
    print()

    # Write frame_durations.txt
    durations_path = os.path.join(output_dir, "frame_durations.txt")
    with open(durations_path, "w") as f:
        f.write(",".join(map(str, durations)))

    # Show statistics
    differences = np.array(differences)
    print(f"\nDifference Statistics:")
    print(f" Min:    {np.min(differences):.6f}%")
    print(f" Max:    {np.max(differences):.6f}%")
    print(f" Mean:   {np.mean(differences):.6f}%")
    print(f" Median: {np.median(differences):.6f}%")
    print(f" 95th percentile: {np.percentile(differences, 95):.6f}%")

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