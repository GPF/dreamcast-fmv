/*
 * pack_dcmv_v5.c
 * ---------------------
 * Dreamcast movie packer utility for the custom .dcmv v5 format.
 *
 * Changes from v4:
 *   - Supports deduplicated frames
 *   - Reads frame_durations.txt to determine frame repeat counts
 *   - Stores both total and unique frame counts in the header
 *   - Writes an additional frame_durations[] table after frame_offsets[]
 *
 * Header format (49 bytes total):
 *   4 bytes  - Magic "DCMV"
 *   4 bytes  - Version (5)
 *   1 byte   - Frame type (0=RGB565, 1=YUV422)
 *   2 bytes  - Texture width
 *   2 bytes  - Texture height
 *   2 bytes  - Content width
 *   2 bytes  - Content height
 *   4 bytes  - Frame rate (fps as float)
 *   2 bytes  - Audio sample rate
 *   2 bytes  - Audio channel count
 *   4 bytes  - Number of unique frames
 *   4 bytes  - Number of total frames (including duplicates)
 *   4 bytes  - Uncompressed frame size
 *   4 bytes  - Maximum compressed frame size (LZ4)
 *   4 bytes  - Audio stream offset
 *   Offset Table:
 *     (num_unique_frames + 1) uint32_t values
 *   Duration Table:
 *     num_unique_frames uint16_t values (frame durations)
 *
 * Usage:
 *   pack_dcmv_v5 <output.dcmv> <frame_type> <width> <height>
 *                <scale_width> <scale_height> <fps>
 *                <sample_rate> <channels>
 *                <frame_pattern> <audio_file> <frame_durations.txt>
 *
 * Example:
 *   ./pack_dcmv_v5 movie.dcmv 1 320 240 320 240 23.97
 *                  32000 2 output/frame%05d.dt audio.dca output/unique_frames/frame_durations.txt
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
#define HEADER_SIZE 49
#define DT_HEADER_MAGIC "DcTx"

static uint16_t *durations = NULL;
static uint32_t num_unique_frames = 0;
static uint32_t num_total_frames = 0;

void write_header(FILE *out, uint8_t frame_type, uint16_t width, uint16_t height,
                  uint16_t scale_width, uint16_t scale_height, float fps,
                  uint16_t sample_rate, uint16_t channels,
                  uint32_t num_unique_frames, uint32_t num_total_frames,
                  uint32_t frame_size, uint32_t max_compressed_size, uint32_t audio_offset) {
    fwrite("DCMV", 1, 4, out);
    uint32_t version = 5;
    fwrite(&version, 4, 1, out);
    fwrite(&frame_type, 1, 1, out);
    fwrite(&width, 2, 1, out);
    fwrite(&height, 2, 1, out);
    fwrite(&scale_width, 2, 1, out);
    fwrite(&scale_height, 2, 1, out);
    fwrite(&fps, sizeof(float), 1, out);
    fwrite(&sample_rate, 2, 1, out);
    fwrite(&channels, 2, 1, out);
    fwrite(&num_unique_frames, 4, 1, out);
    fwrite(&num_total_frames, 4, 1, out);
    fwrite(&frame_size, 4, 1, out);
    fwrite(&max_compressed_size, 4, 1, out);
    fwrite(&audio_offset, 4, 1, out);
}

static const char* get_frame_type_name(uint8_t frame_type) {
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
        if (fseek(fp, skip, SEEK_SET) != 0) {
            fclose(fp);
            return 0;
        }
    } else {
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
    if (!fp) return 0;

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

    fseek(fp, 0, SEEK_END);
    size_t total_size = ftell(fp);
    fclose(fp);

    return total_size - skip;
}

// Load frame_durations.txt into durations array
int load_durations(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        perror("Failed to open frame_durations.txt");
        return 1;
    }

    // Count entries first
    num_unique_frames = 0;
    num_total_frames = 0;
    int value;
    while (fscanf(fp, "%d,", &value) == 1) {
        num_unique_frames++;
        num_total_frames += value;
    }

    rewind(fp);
    durations = (uint16_t *)malloc(num_unique_frames * sizeof(uint16_t));
    if (!durations) {
        fprintf(stderr, "Memory allocation failed for durations\n");
        fclose(fp);
        return 1;
    }

    for (uint32_t i = 0; i < num_unique_frames; i++) {
        if (fscanf(fp, "%d,", &value) != 1) break;
        durations[i] = (uint16_t)value;
    }
    fclose(fp);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 13) {
        printf("Usage: %s <output.dcmv> <frame_type> <width> <height> "
               "<scale_width> <scale_height> <fps> "
               "<sample_rate> <channels> "
               "<frame_pattern> <audio_file> <frame_durations.txt>\n", argv[0]);
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
    const char *durations_path = argv[12];

    if (load_durations(durations_path) != 0) {
        return 1;
    }

    printf("📦 DCMV Packer v5 (Deduplicated Frames)\n");
    printf("   Format: %s (%d)\n", get_frame_type_name(frame_type), frame_type);
    printf("   Texture: %dx%d, Content: %dx%d\n", width, height, scale_width, scale_height);
    printf("   FPS: %.2f, Audio: %dHz, %d channel(s)\n", fps, sample_rate, channels);
    printf("   Unique frames: %u, Total frames: %u\n", num_unique_frames, num_total_frames);

    // Open audio
    FILE *audio_fp = fopen(audio_path, "rb");
    if (!audio_fp) {
        perror("Audio open failed");
        return 1;
    }
    char head[4];
    size_t read_bytes = fread(head, 1, 4, audio_fp);
    if (memcmp(head, "DcAF", 4) == 0) {
        fseek(audio_fp, 0x40, SEEK_SET);
        printf("🔊 Skipping 64-byte DcAF header from %s\n", audio_path);
    } else {
        rewind(audio_fp);
    }

    // Determine frame size
    char filename[FRAME_FILENAME_MAX];
    snprintf(filename, sizeof(filename), frame_pattern, 0);
    size_t frame_size = get_texture_data_size(filename);
    if (frame_size == 0) {
        fprintf(stderr, "Failed to get texture data size from first frame\n");
        return 1;
    }

    // Open output
    FILE *out = fopen(output_path, "wb+");
    if (!out) {
        perror("Output file creation failed");
        return 1;
    }

    // Allocate offset table
    uint32_t *offsets = malloc((num_unique_frames + 1) * sizeof(uint32_t));
    if (!offsets) {
        fprintf(stderr, "Memory allocation failed for offsets\n");
        return 1;
    }

    // Reserve header + offset + duration tables
    fseek(out, HEADER_SIZE, SEEK_SET);
    long offset_table_pos = ftell(out);
    fseek(out, (num_unique_frames + 1) * sizeof(uint32_t), SEEK_CUR);
    long duration_table_pos = ftell(out);
    fseek(out, num_unique_frames * sizeof(uint16_t), SEEK_CUR);
    offsets[0] = ftell(out);

    // Compress frames
    uint8_t *frame_buf = malloc(frame_size);
    uint8_t *compressed_buf = malloc(LZ4_compressBound(frame_size));
    uint32_t max_compressed_size = 0;
    printf("🗜️  Compressing unique frames...\n");
    for (uint32_t i = 0; i < num_unique_frames; i++) {
        snprintf(filename, sizeof(filename), frame_pattern, i);
        size_t read_size = load_frame_data(filename, frame_buf, frame_size);
        if (read_size != frame_size) {
            fprintf(stderr, "Frame %u load error\n", i);
            return 1;
        }
        int compressed_size = LZ4_compress_HC((char*)frame_buf, (char*)compressed_buf, frame_size, LZ4_compressBound(frame_size), LZ4HC_CLEVEL_MAX);
        fwrite(compressed_buf, 1, compressed_size, out);
        if (compressed_size > max_compressed_size) max_compressed_size = compressed_size;
        if (i < num_unique_frames - 1) offsets[i + 1] = ftell(out);
        if ((i + 1) % 100 == 0 || i == num_unique_frames - 1) {
            printf("Frame %u compressed: %d bytes\n", i, compressed_size);
            printf("\r   Processed %u/%u frames (%.1f%%)", i + 1, num_unique_frames, (float)(i + 1) / num_unique_frames * 100.0f);
            fflush(stdout);
        }
    }
    printf("\n");

    // Audio offset
    uint32_t audio_offset = ftell(out);
    offsets[num_unique_frames] = audio_offset;

    // Copy audio
    uint8_t audio_buffer[4096];
    size_t audio_bytes_copied = 0;
    while (!feof(audio_fp)) {
        size_t bytes_read = fread(audio_buffer, 1, sizeof(audio_buffer), audio_fp);
        fwrite(audio_buffer, 1, bytes_read, out);
        audio_bytes_copied += bytes_read;
    }
    fclose(audio_fp);
    printf("🔊 Copied %zu bytes of audio\n", audio_bytes_copied);

    // Write header
    fseek(out, 0, SEEK_SET);
    write_header(out, frame_type, width, height, scale_width, scale_height, fps,
                 sample_rate, channels, num_unique_frames, num_total_frames,
                 frame_size, max_compressed_size, audio_offset);

    // Write offset table
    fseek(out, offset_table_pos, SEEK_SET);
    fwrite(offsets, sizeof(uint32_t), num_unique_frames + 1, out);

    // Write duration table
    fseek(out, duration_table_pos, SEEK_SET);
    fwrite(durations, sizeof(uint16_t), num_unique_frames, out);

    free(offsets);
    free(durations);
    free(frame_buf);
    free(compressed_buf);
    fclose(out);

    printf("✅ DCMV v5 file created successfully: %s\n", output_path);
    return 0;
}
