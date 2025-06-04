// pack_dcmv_zstd_fixed.c - builds Dreamcast .dcmv container with Zstandard-compressed frames
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

#define MAX_FRAMES 10000
#define FRAME_FILENAME_MAX 256

void write_header(FILE *out, uint16_t width, uint16_t height, uint16_t fps, uint16_t sample_rate,
                  uint16_t channels, uint32_t num_frames, uint32_t frame_size, uint32_t max_compressed_size) {
    fwrite("DCMV", 1, 4, out);
    uint32_t version = 3;
    fwrite(&version, 4, 1, out);
    fwrite(&width, 2, 1, out);
    fwrite(&height, 2, 1, out);
    fwrite(&fps, 2, 1, out);
    fwrite(&sample_rate, 2, 1, out);
    fwrite(&channels, 2, 1, out);
    fwrite(&num_frames, 4, 1, out);
    fwrite(&frame_size, 4, 1, out);
    fwrite(&max_compressed_size, 4, 1, out); // will be overwritten later
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
    if (!audio_fp) { perror("Audio open failed"); return 1; }

    char head[4];
    fread(head, 1, 4, audio_fp);
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
    fseek(first_fp, 0, SEEK_END);
    size_t original_size = ftell(first_fp);
    rewind(first_fp);
    uint8_t *raw_buf = malloc(original_size);
    fread(raw_buf, 1, original_size, first_fp);
    fclose(first_fp);

    uint32_t skip = 0;
    if (memcmp(raw_buf, "DcTx", 4) == 0) {
        uint8_t header_size = raw_buf[9];
        skip = (header_size + 1) * 32;
    } else if (memcmp(raw_buf, "DTEX", 4) == 0 || memcmp(raw_buf, "PVRT", 4) == 0) {
        skip = 0x10;
    } else {
        fprintf(stderr, "Unknown texture format in frame 0\n");
        return 1;
    }

    size_t src_len = original_size - skip;

    FILE *out = fopen(output_path, "wb+");
    if (!out) { perror("Output open failed"); return 1; }

    write_header(out, width, height, fps, sample_rate, channels, frame_count, src_len, 0);

    long size_table_pos = ftell(out);
    fseek(out, frame_count * sizeof(uint32_t), SEEK_CUR);
    long offset_table_pos = ftell(out);
    fseek(out, frame_count * sizeof(uint32_t), SEEK_CUR);

    uint32_t *sizes = malloc(frame_count * sizeof(uint32_t));
    uint32_t *offsets = malloc(frame_count * sizeof(uint32_t));
    if (!sizes || !offsets) {
        fprintf(stderr, "OOM\n");
        return 1;
    }

    ZSTD_CCtx *cctx = ZSTD_createCCtx();
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_format, ZSTD_f_zstd1_magicless);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 9);

    uint32_t max_compressed_size = 0;
    for (int i = 0; i < frame_count; ++i) {
        snprintf(filename, sizeof(filename), frame_pattern, i);
        FILE *fp = fopen(filename, "rb");
        fread(raw_buf, 1, original_size, fp);
        fclose(fp);

        uint8_t *src = raw_buf + skip;
        size_t bound = ZSTD_compressBound(src_len);
        uint8_t *comp = malloc(bound);
        ZSTD_CCtx_reset(cctx, ZSTD_reset_session_only);
        ZSTD_inBuffer input = { src, src_len, 0 };
        ZSTD_outBuffer output = { comp, bound, 0 };

        size_t res = ZSTD_compressStream2(cctx, &output, &input, ZSTD_e_end);
        if (ZSTD_isError(res)) {
            fprintf(stderr, "ZSTD compress error on frame %d: %s\n", i, ZSTD_getErrorName(res));
            return 1;
        }

        sizes[i] = output.pos;
        offsets[i] = ftell(out);

        if (sizes[i] > max_compressed_size)
            max_compressed_size = sizes[i];

        // printf("🧵 frame %04d compressed size: %u\n", i, sizes[i]);

        fwrite(&sizes[i], 4, 1, out);
        fwrite(comp, 1, sizes[i], out);
        free(comp);
    }

    free(raw_buf);
    ZSTD_freeCCtx(cctx);

    // patch max_compressed_size back into header
    fseek(out, 0x1A, SEEK_SET);
    fwrite(&max_compressed_size, 4, 1, out);
    printf("📏 max_compressed_size written to header: %u\n", max_compressed_size);

    fseek(out, size_table_pos, SEEK_SET);
    fwrite(sizes, sizeof(uint32_t), frame_count, out);

    fseek(out, offset_table_pos, SEEK_SET);
    fwrite(offsets, sizeof(uint32_t), frame_count, out);

    fseek(out, 0, SEEK_END);
    uint8_t abuf[4096];
    size_t n;
    while ((n = fread(abuf, 1, sizeof(abuf), audio_fp)) > 0)
        fwrite(abuf, 1, n, out);

    fclose(audio_fp);
    fclose(out);
    free(sizes);
    free(offsets);

    printf("✅ Packed %d Zstd-compressed frames + audio into %s\n", frame_count, output_path);
    return 0;
}
