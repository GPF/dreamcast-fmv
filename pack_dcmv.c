// pack_dcmv.c - builds Dreamcast .dcmv container with raw DEFLATE-compressed frames from .dt/.tex/.pvr textures
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <zlib.h>

#define MAX_FRAMES 10000
#define FRAME_FILENAME_MAX 256
#define ALIGN32(x) (((x) + 31) & ~31)

// Header format for version 3
void write_header(FILE *out, uint16_t width, uint16_t height, uint16_t fps, uint16_t sample_rate,
                  uint16_t channels, uint32_t num_frames, uint32_t frame_size) {
    fwrite("DCMV", 1, 4, out);
    uint32_t version = 3;
    fwrite(&version, 4, 1, out);
    fwrite(&width, 2, 1, out);
    fwrite(&height, 2, 1, out);
    fwrite(&fps, 2, 1, out);
    fwrite(&sample_rate, 2, 1, out);
    fwrite(&channels, 2, 1, out);
    fwrite(&num_frames, 4, 1, out);
    fwrite(&frame_size, 4, 1, out); // decompressed frame size
}

int main(int argc, char **argv) {
    if (argc != 9) {
        printf("Usage: %s <output.dcmv> <width> <height> <fps> <sample_rate> <channels> <frame_pattern> <audio_file>\n", argv[0]);
        return 1;
    }

    const char *output_path = argv[1];
    uint16_t width = atoi(argv[2]);
    uint16_t height = atoi(argv[3]);
    uint16_t fps = atoi(argv[4]);
    uint16_t sample_rate = atoi(argv[5]);
    uint16_t channels = atoi(argv[6]);
    const char *frame_pattern = argv[7];
    const char *audio_path = argv[8];

    FILE *audio_fp = fopen(audio_path, "rb");
    if (!audio_fp) {
        perror("Audio open failed");
        return 1;
    }
    fseek(audio_fp, 0, SEEK_END);
    long audio_size = ftell(audio_fp);
    fseek(audio_fp, 0, SEEK_SET);

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

    // Probe first frame
    snprintf(filename, sizeof(filename), frame_pattern, 0);
    FILE *first_fp = fopen(filename, "rb");
    if (!first_fp) {
        fprintf(stderr, "Failed to open first frame\n");
        return 1;
    }
    fseek(first_fp, 0, SEEK_END);
    size_t original_size = ftell(first_fp);
    rewind(first_fp);
    uint8_t *raw_buf = malloc(original_size);
    fread(raw_buf, 1, original_size, first_fp);
    fclose(first_fp);

    uint32_t skip = 0;
    if (memcmp(raw_buf, "DcTx", 4) == 0) skip = 0x80;
    else if (memcmp(raw_buf, "DTEX", 4) == 0) skip = 0x10;
    else if (memcmp(raw_buf, "PVRT", 4) == 0) skip = 0x10;
    else {
        fprintf(stderr, "Unknown texture format in frame 0\n");
        return 1;
    }

    size_t src_len = original_size - skip;

    FILE *out = fopen(output_path, "wb+");
    if (!out) {
        perror("Output open failed");
        return 1;
    }

    write_header(out, width, height, fps, sample_rate, channels, frame_count, src_len);

    uint32_t *offsets = malloc(frame_count * sizeof(uint32_t));
    if (!offsets) {
        fprintf(stderr, "OOM\n");
        return 1;
    }

    long offset_index_pos = ftell(out);
    fseek(out, frame_count * sizeof(uint32_t), SEEK_CUR);

    for (int i = 0; i < frame_count; ++i) {
        snprintf(filename, sizeof(filename), frame_pattern, i);
        FILE *fp = fopen(filename, "rb");
        if (!fp) {
            fprintf(stderr, "Failed to open frame %d\n", i);
            return 1;
        }

        fread(raw_buf, 1, original_size, fp);
        fclose(fp);

        uint8_t *src = raw_buf + skip;

        z_stream zs = {0};
        uLongf bound = compressBound(src_len);
        uint8_t *comp = malloc(bound);
        if (!comp) {
            fprintf(stderr, "OOM compress\n");
            return 1;
        }

        zs.next_in = src;
        zs.avail_in = src_len;
        zs.next_out = comp;
        zs.avail_out = bound;

        if (deflateInit2(&zs, Z_BEST_SPEED, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
            fprintf(stderr, "❌ deflateInit2 failed on frame %d\n", i);
            return 1;
        }

        int ok = deflate(&zs, Z_FINISH);
        deflateEnd(&zs);

        if (ok != Z_STREAM_END) {
            fprintf(stderr, "❌ deflate failed on frame %d (ret=%d)\n", i, ok);
            return 1;
        }

        offsets[i] = ftell(out);
        fwrite(&zs.total_out, 4, 1, out);
        fwrite(comp, 1, zs.total_out, out);
        free(comp);
    }
    free(raw_buf);

    // Append audio
    uint8_t abuf[4096];
    size_t n;
    while ((n = fread(abuf, 1, sizeof(abuf), audio_fp)) > 0) {
        fwrite(abuf, 1, n, out);
    }
    fclose(audio_fp);

    fseek(out, offset_index_pos, SEEK_SET);
    fwrite(offsets, sizeof(uint32_t), frame_count, out);
    fclose(out);
    free(offsets);

    printf("✅ Packed %d raw-deflate frames + audio into %s\n", frame_count, output_path);
    return 0;
}
