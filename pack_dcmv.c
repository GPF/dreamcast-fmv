/*
 * pack_dcmv.c
 * ---------------------
 * Dreamcast movie packer utility for the custom .dcmv format.
 *
 * This tool builds a .dcmv container consisting of:
 *   - LZ4 HC-compressed RGB565/YUV422 VQ PVR texture frames (.dt)
 *   - Optional ADPCM-encoded audio track
 *   - Frame offset table for decompression and sync
 *   - Extended header (version 4) with metadata + scale dimensions
 *
 * Header format (41 bytes total):
 *   4 bytes  - Magic "DCMV"
 *   4 bytes  - Version (4)
 *   1 byte   - Frame type (0=RGB565, 1=YUV422)
 *   2 bytes  - Texture width (e.g., 320 for strided, 512 for POT)
 *   2 bytes  - Texture height (e.g., 240 for strided, 256 for POT)
 *   2 bytes  - Content width (always 320 for 4:3 content)
 *   2 bytes  - Content height (always 240 for 4:3 content)
 *   4 bytes  - Frame rate (fps)
 *   2 bytes  - Audio sample rate
 *   2 bytes  - Audio channel count
 *   4 bytes  - Number of video frames
 *   4 bytes  - Uncompressed frame size
 *   4 bytes  - Maximum compressed frame size (LZ4)
 *   4 bytes  - Audio stream offset (absolute file position)
 *   Offset Table:
 *   - Immediately follows the 41-byte header
 *   - Contains (num_frames + 1) uint32_t values
 *   - Each entry is a byte offset to the start of a frame
 *   - The final offset points to the start of the audio stream
 * 
 * The tool assumes input video frames follow a numeric pattern like:
 *   "output/frame%04d.dt"
 * All frames must be of the same size and format (e.g., RGB565 VQ).
 *
 * Audio input should be ADPCM (.dca) with optional 64-byte "DcAF" header.
 * The audio is appended at the end of the compressed video + offset table.
 *
 * Usage:
 *   pack_dcmv <output.dcmv> <frame_type> <width> <height> <scale_width> <scale_height> <fps> <sample_rate> <channels> <frame_pattern> <audio_file>
 *
 * Frame types:
 *   0 = RGB565
 *   1 = YUV422
 *
 * Example:
 *   ./pack_dcmv movie.dcmv 1 320 240 320 240 23.97 32000 1 output/frame%05d.dt audio.dca
 
 * Dependencies:
 *   - LZ4 (lz4.h, lz4hc.h)
 *   - Output files must be accessible and match expected binary layout
 *
 * Author: Troy Davis (gpf)
 * GitHub: https://github.com/GPF
 * License: Public Domain / MIT-style — use freely with attribution.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <lz4.h>
#include <lz4hc.h>

#define MAX_FRAMES 99999
#define FRAME_FILENAME_MAX 256
#define HEADER_SIZE 41
#define DT_HEADER_MAGIC "DcTx"

void write_header(FILE *out, uint8_t frame_type, uint16_t width, uint16_t height, 
                  uint16_t scale_width, uint16_t scale_height, float fps, uint16_t sample_rate,
                  uint16_t channels, uint32_t num_frames, uint32_t frame_size, 
                  uint32_t max_compressed_size, uint32_t audio_offset) {
    fwrite("DCMV", 1, 4, out);
    uint32_t version = 4;
    fwrite(&version, 4, 1, out);
    fwrite(&frame_type, 1, 1, out);
    fwrite(&width, 2, 1, out);
    fwrite(&height, 2, 1, out);
    fwrite(&scale_width, 2, 1, out);     // New: content dimensions
    fwrite(&scale_height, 2, 1, out);    // New: content dimensions
    fwrite(&fps, sizeof(float), 1, out);
    fwrite(&sample_rate, 2, 1, out);
    fwrite(&channels, 2, 1, out);
    fwrite(&num_frames, 4, 1, out);
    fwrite(&frame_size, 4, 1, out);
    fwrite(&max_compressed_size, 4, 1, out);
    fwrite(&audio_offset, 4, 1, out);
}

const char* get_frame_type_name(uint8_t frame_type) {
    switch(frame_type) {
        case 0: return "RGB565";
        case 1: return "YUV422";
        default: return "Unknown";
    }
}

// Function to load frame data, stripping DT header if present
size_t load_frame_data(const char* filename, uint8_t* buffer, size_t buffer_size) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        return 0;
    }
    
    // Read first 32 bytes to check for DT header
    uint8_t header_buf[32];
    size_t header_read = fread(header_buf, 1, sizeof(header_buf), fp);
    if (header_read < sizeof(header_buf)) {
        fclose(fp);
        return 0;
    }
    
    size_t skip = 0;
    if (memcmp(header_buf, DT_HEADER_MAGIC, 4) == 0) {
        uint8_t header_size = header_buf[9];
        skip = (header_size + 1) * 32;
        // printf("🔧 Stripping DT header (%zu bytes) from %s\n", skip, filename);
        
        if (fseek(fp, skip, SEEK_SET) != 0) {
            fclose(fp);
            return 0;
        }
    } else {
        // No DT header, rewind and read from beginning
        rewind(fp);
    }
    
    // Read the actual texture data
    size_t bytes_read = fread(buffer, 1, buffer_size, fp);
    fclose(fp);
    
    return bytes_read;
}

// Function to get actual texture data size (without DT header)
size_t get_texture_data_size(const char* filename) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        return 0;
    }
    
    // Check for DT header
    uint8_t header_buf[32];
    size_t header_read = fread(header_buf, 1, sizeof(header_buf), fp);
    if (header_read < sizeof(header_buf)) {
        fclose(fp);
        return 0;
    }
    
    size_t skip = 0;
    if (memcmp(header_buf, DT_HEADER_MAGIC, 4) == 0) {
        uint8_t header_size = header_buf[9];
        skip = (header_size + 1) * 32;
    }
    
    // Get total file size and subtract header
    fseek(fp, 0, SEEK_END);
    size_t total_size = ftell(fp);
    fclose(fp);
    
    return total_size - skip;
}

int main(int argc, char **argv) {
    if (argc != 12) {
        printf("Usage: %s <output.dcmv> <frame_type> <width> <height> <scale_width> <scale_height> <fps> <sample_rate> <channels> <frame_pattern> <audio_file>\n", argv[0]);
        printf("Frame types: 0=RGB565, 1=YUV422\n");
        printf("Example: %s movie.dcmv 1 320 240 320 240 23.97 32000 1 output/frame%%05d.dt audio.dca\n", argv[0]);
        return 1;
    }

    const char *output_path = argv[1];
    uint8_t frame_type = atoi(argv[2]);
    uint16_t width = atoi(argv[3]);
    uint16_t height = atoi(argv[4]);
    uint16_t scale_width = atoi(argv[5]);
    uint16_t scale_height = atoi(argv[6]);
    float fps = strtof(argv[7], NULL); 
    uint16_t sample_rate = atoi(argv[8]);
    uint16_t channels = atoi(argv[9]);
    const char *frame_pattern = argv[10];
    const char *audio_path = argv[11];

    // Validate frame type
    if (frame_type > 1) {
        fprintf(stderr, "Error: Invalid frame type %d (must be 0=RGB565 or 1=YUV422)\n", frame_type);
        return 1;
    }

    printf("📦 DCMV Packer v4 - FIXED (DT Header Stripping)\n");
    printf("   Format: %s (%d)\n", get_frame_type_name(frame_type), frame_type);
    printf("   Texture: %dx%d\n", width, height);
    printf("   Content: %dx%d\n", scale_width, scale_height);
    printf("   FPS: %.2f\n", fps);
    printf("   Audio: %dHz, %d channel(s)\n", sample_rate, channels);

    // Open audio file
    FILE *audio_fp = fopen(audio_path, "rb");
    if (!audio_fp) { 
        perror("Audio open failed"); 
        return 1; 
    }

    // Check for and skip DcAF header if present
    char head[4];
    size_t read_bytes = fread(head, 1, 4, audio_fp);
    if (memcmp(head, "DcAF", 4) == 0) {
        fseek(audio_fp, 0x40, SEEK_SET);
        printf("🔊 Skipping 64-byte DcAF header from %s\n", audio_path);
    } else {
        rewind(audio_fp);
    }

    // Count frames
    char filename[FRAME_FILENAME_MAX];
    int frame_count = 0;
    for (int i = 0; i < MAX_FRAMES; ++i) {
        snprintf(filename, sizeof(filename), frame_pattern, i);
        FILE *fp = fopen(filename, "rb");
        if (!fp) break;
        fclose(fp);
        frame_count++;
    }
    
    if (frame_count == 0) {
        fprintf(stderr, "❌ No frames found matching pattern: %s\n", frame_pattern);
        return 1;
    }
    
    printf("📽️  Found %d frames\n", frame_count);

    // Get actual texture data size from first frame (without DT header)
    snprintf(filename, sizeof(filename), frame_pattern, 0);
    size_t frame_size = get_texture_data_size(filename);
    if (frame_size == 0) {
        fprintf(stderr, "❌ Failed to get texture data size from first frame: %s\n", filename);
        return 1;
    }
    
    printf("📏 Actual texture data size: %zu bytes (DT header stripped)\n", frame_size);

    // Open output file
    FILE *out = fopen(output_path, "wb+");
    if (!out) { 
        perror("Output file creation failed"); 
        return 1; 
    }

    // Allocate offset table
    uint32_t *offsets = malloc((frame_count + 1) * sizeof(uint32_t));
    if (!offsets) {
        fprintf(stderr, "❌ Memory allocation failed\n");
        return 1;
    }

    // Reserve space for header and offset table
    fseek(out, HEADER_SIZE, SEEK_SET);
    long offset_table_pos = ftell(out);
    fseek(out, (frame_count + 1) * sizeof(uint32_t), SEEK_CUR);
    offsets[0] = ftell(out);

    // Compress and write frames
    uint8_t *frame_buf = malloc(frame_size);
    uint8_t *compressed_buf = malloc(LZ4_compressBound(frame_size));
    if (!frame_buf || !compressed_buf) {
        fprintf(stderr, "❌ Memory allocation failed for buffers\n");
        return 1;
    }

    uint32_t max_compressed_size = 0;
    printf("🗜️  Compressing frames (stripping DT headers)...\n");
    int dt_header_count = 0;

    for (int i = 0; i < frame_count; ++i) {
        snprintf(filename, sizeof(filename), frame_pattern, i);
        
        // Load frame data with DT header stripping
        size_t read_size = load_frame_data(filename, frame_buf, frame_size);
        if (read_size == 0) {
            fprintf(stderr, "❌ Failed to load frame %d: %s\n", i, filename);
            return 1;
        }
        
        if (read_size != frame_size) {
            fprintf(stderr, "❌ Frame %d size mismatch: expected %zu, got %zu\n", i, frame_size, read_size);
            return 1;
        }

        // Compress frame with LZ4 HC
        int compressed_size = LZ4_compress_HC((char*)frame_buf, (char*)compressed_buf, frame_size, LZ4_compressBound(frame_size), LZ4HC_CLEVEL_MAX);
        if (compressed_size <= 0) {
            fprintf(stderr, "❌ Compression failed for frame %d\n", i);
            return 1;
        }

        // Write compressed frame
        fwrite(compressed_buf, 1, compressed_size, out);
        
        if (compressed_size > max_compressed_size) {
            max_compressed_size = compressed_size;
        }

        // Set next frame offset
        if (i < frame_count - 1) {
            offsets[i + 1] = ftell(out);
        }

        // Progress indicator
        if ((i + 1) % 100 == 0 || i == frame_count - 1) {
            printf("\r   Processed %d/%d frames (%.1f%%) - Latest compressed: %d bytes", 
                i + 1, frame_count, (float)(i + 1) / frame_count * 100.0f, compressed_size);
            fflush(stdout);  // Ensure the output is displayed immediately
        }
    }

    // Audio offset (after all compressed frames)
    uint32_t audio_offset = ftell(out);
    offsets[frame_count] = audio_offset;

    // Copy audio data
    printf("🔊 Copying audio data...\n");
    uint8_t audio_buffer[4096];
    size_t audio_bytes_copied = 0;
    while (!feof(audio_fp)) {
        size_t bytes_read = fread(audio_buffer, 1, sizeof(audio_buffer), audio_fp);
        if (bytes_read > 0) {
            fwrite(audio_buffer, 1, bytes_read, out);
            audio_bytes_copied += bytes_read;
        }
    }
    fclose(audio_fp);
    printf("   Copied %zu bytes of audio\n", audio_bytes_copied);

    // Write header
    fseek(out, 0, SEEK_SET);
    write_header(out, frame_type, width, height, scale_width, scale_height, fps, 
                 sample_rate, channels, frame_count, frame_size, max_compressed_size, audio_offset);

    // Write offset table
    fseek(out, offset_table_pos, SEEK_SET);
    fwrite(offsets, sizeof(uint32_t), frame_count + 1, out);

    // Cleanup
    free(frame_buf);
    free(compressed_buf);
    free(offsets);
    fclose(out);

    // Final stats
    fseek(out, 0, SEEK_END);
    size_t total_size = ftell(out);
    
    printf("✅ DCMV file created successfully!\n");
    printf("   Output: %s\n", output_path);
    printf("   Total size: %.2f MB\n", total_size / (1024.0 * 1024.0));
    printf("   Compression ratio: %.1f%% (avg %d bytes/frame)\n", 
           (float)max_compressed_size / frame_size * 100.0f, max_compressed_size);

    return 0;
}