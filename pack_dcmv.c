/*
 * pack_dcmv.c
 * ---------------------
 * Dreamcast movie packer utility for the custom .dcmv format.
 *
 * This tool builds a .dcmv container consisting of:
 *   - LZ4 HC-compressed RGB565 VQ PVR texture frames (.dt)
 *   - Optional ADPCM-encoded audio track
 *   - Frame offset table for decompression and sync
 *   - Extended header (version 3) with metadata + audio offset
 *
 * Header format (42 bytes total):
 *   4 bytes  - Magic "DCMV"
 *   4 bytes  - Version (3)
 *   2 bytes  - Video width
 *   2 bytes  - Video height
 *   2 bytes  - Frame rate (fps)
 *   2 bytes  - Audio sample rate
 *   2 bytes  - Audio channel count
 *   4 bytes  - Number of video frames
 *   4 bytes  - Uncompressed frame size
 *   4 bytes  - Maximum compressed frame size (LZ4)
 *   4 bytes  - Audio stream offset (absolute file position)
 *
 * The tool assumes input video frames follow a numeric pattern like:
 *   "output/frame%04d.dt"
 * All frames must be of the same size and format (e.g., RGB565 VQ).
 *
 * Audio input should be ADPCM (.dca) with optional 64-byte "DcAF" header.
 * The audio is appended at the end of the compressed video + offset table.
 *
 * Usage:
 *   pack_dcmv_lz4_fixed <output.dcmv> <width> <height> <fps> <sample_rate> <channels> <frame_pattern> <audio_file>
 *
 * Example:
 *   ./pack_dcmv_lz4_fixed movie.dcmv 512 512 24 32000 1 output/frame%04d.dt audio.dca
 *
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

#define MAX_FRAMES 10000
#define FRAME_FILENAME_MAX 256

void write_header(FILE *out, uint8_t frame_type, uint16_t width, uint16_t height, uint16_t fps, uint16_t sample_rate,
                  uint16_t channels, uint32_t num_frames, uint32_t frame_size, uint32_t max_compressed_size, uint32_t audio_offset) {
    fwrite("DCMV", 1, 4, out);
    uint32_t version = 3;
    fwrite(&version, 4, 1, out);
    fwrite(&frame_type, 1, 1, out);
    fwrite(&width, 2, 1, out);
    fwrite(&height, 2, 1, out);
    fwrite(&fps, 2, 1, out);
    fwrite(&sample_rate, 2, 1, out);
    fwrite(&channels, 2, 1, out);
    fwrite(&num_frames, 4, 1, out);
    fwrite(&frame_size, 4, 1, out);
    fwrite(&max_compressed_size, 4, 1, out); // to be overwritten
    fwrite(&audio_offset, 4, 1, out);       // 34 (new!)
}

int main(int argc, char **argv) {
    if (argc != 10) {
        printf("Usage: %s <output.dcmv> <frame_type 0=RGB565, 1=YUV420P> <width> <height> <fps> <sample_rate> <channels> <frame_pattern> <audio_file>\n", argv[0]);
        return 1;
    }

    const char *output_path = argv[1];
    uint16_t frame_type = atoi(argv[2]);
    uint16_t width = atoi(argv[3]);
    uint16_t height = atoi(argv[4]);
    uint16_t fps = atoi(argv[5]);
    uint16_t sample_rate = atoi(argv[6]);
    uint16_t channels = atoi(argv[7]);
    const char *frame_pattern = argv[8];
    const char *audio_path = argv[9];
    printf("audio path = %s\n", audio_path);
    FILE *audio_fp = fopen(audio_path, "rb");
    if (!audio_fp) { perror("Audio open failed"); return 1; }

    // Check for and skip DcAF header if present
    char head[4];
    size_t read_bytes = fread(head, 1, 4, audio_fp);
    if (memcmp(head, "DcAF", 4) == 0) {
        fseek(audio_fp, 0x40, SEEK_SET);
        printf("🔊 Skipping 64-byte DcAF header from %s\n", audio_path);
    } else {
        rewind(audio_fp);
    }

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
        fprintf(stderr, "No frames found matching pattern\n");
        return 1;
    }

    snprintf(filename, sizeof(filename), frame_pattern, 0);

    FILE *first_fp = fopen(filename, "rb");
    if (!first_fp) {
        fprintf(stderr, "Failed to open first frame\n");
        return 1;
    }
    uint32_t skip = 0;
    size_t frame_size = 0;
    uint8_t *raw_buf = NULL;

    FILE *out = fopen(output_path, "wb+");
    if (!out) { perror("Output open failed"); return 1; }

    fseek(out, 43, SEEK_SET);   // size of DCMV header
    long offset_table_pos = ftell(out);  // where offset table starts
    fseek(out, (frame_count + 1) * sizeof(uint32_t), SEEK_CUR);

    uint32_t *offsets = malloc((frame_count + 1) * sizeof(uint32_t));
    if (!offsets) {
        fprintf(stderr, "OOM\n");
        return 1;
    }

    uint32_t max_compressed_size = 0;

    for (int i = 0; i < frame_count; ++i) {
        snprintf(filename, sizeof(filename), frame_pattern, i);
        FILE *fp = fopen(filename, "rb");
        if (!fp) {
            fprintf(stderr, "Failed to open frame %d\n", i);
            return 1;
        }

        fseek(fp, 0, SEEK_END);
        size_t original_size = ftell(fp);
        rewind(fp);

        if (!raw_buf || original_size > 0x100000) {
            raw_buf = realloc(raw_buf, original_size);
            if (!raw_buf) {
                perror("Failed to realloc raw_buf");
                return 1;
            }
        }

        fread(raw_buf, 1, original_size, fp);
        fclose(fp);

        // Handle skip logic on frame 0 only (only relevant for RGB565)
        if (i == 0) {
            if (frame_type == 0) {
                if (memcmp(raw_buf, "DcTx", 4) == 0) {
                    uint8_t header_size = raw_buf[9];
                    skip = (header_size + 1) * 32;
                } else if (memcmp(raw_buf, "DTEX", 4) == 0 || memcmp(raw_buf, "PVRT", 4) == 0) {
                    skip = 0x10;
                } else {
                    fprintf(stderr, "Unknown texture format in frame 0 (expected RGB565+header)\n");
                    return 1;
                }
            } else {
                skip = 0;
            }

            frame_size = original_size - skip;  // store first frame's usable size
        }

        size_t src_len = original_size - skip;
        uint8_t *src = raw_buf + skip;

        int bound = LZ4_compressBound(src_len);
        uint8_t *comp = malloc(bound);
        if (!comp) {
            perror("Failed to malloc comp");
            return 1;
        }

        int comp_size = LZ4_compress_HC((const char *)src, (char *)comp, src_len, bound, 12);
        if (comp_size <= 0) {
            fprintf(stderr, "LZ4 compression failed on frame %d\n", i);
            return 1;
        }
        // printf("comp size=%d, max_compress_size so far=%d\n",comp_size, max_compressed_size);
        offsets[i] = ftell(out);
        if (comp_size > max_compressed_size)
            max_compressed_size = comp_size;

        fwrite(comp, 1, comp_size, out);
        free(comp);
    }

    free(raw_buf);
         
    // Patch max_compressed_size back into header
    // fseek(out, 0x1A, SEEK_SET);
    
    // fwrite(&max_compressed_size, 4, 1, out);
    printf("📏 max_compressed_size written to header: %u\n", max_compressed_size);

    uint32_t audio_offset = ftell(out); // <- this is the real offset
    printf("📏 audio_offset written to header: 0x%X\n", audio_offset);    
    // Write tables
    fseek(out, offset_table_pos, SEEK_SET);
    offsets[frame_count] = audio_offset;  // Extra offset for size calculation
    fwrite(offsets, sizeof(uint32_t), frame_count + 1, out);
    
    fseek(out, 0, SEEK_END);
    uint8_t abuf[4096];
    size_t n;
    while ((n = fread(abuf, 1, sizeof(abuf), audio_fp)) > 0)
        fwrite(abuf, 1, n, out);

    // Finally patch header
    fseek(out, 0, SEEK_SET);
    write_header(out, frame_type, width, height, fps, sample_rate, channels, frame_count,
                frame_size, max_compressed_size, audio_offset);        
    fclose(audio_fp);
    fclose(out);
    free(offsets);

    printf("✅ Packed %d LZ4-compressed frames + audio into %s\n", frame_count, output_path);
    return 0;
}
