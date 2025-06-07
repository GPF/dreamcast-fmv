#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>

#define BLOCK_SIZE_16x16 384  // 64 V + 64 U + 256 Y
static bool quiet_mode = false;

typedef struct {
    uint8_t* data;
    size_t size;
} PlaneData;

int read_plane(FILE* file, PlaneData* plane, const char* plane_name) {
    size_t read = fread(plane->data, 1, plane->size, file);
    if (read != plane->size) {
        fprintf(stderr, "Error reading %s plane: %s\n", plane_name,
                feof(file) ? "Unexpected EOF" : strerror(errno));
        return 0;
    }
    return 1;
}

void write_macroblock_sequence(FILE* out, const uint8_t* block) {
    // Write V top 64, U top 64, Y top 128
    fwrite(block, 1, 64, out);         // V block
    fwrite(block + 64, 1, 64, out);    // U block
    fwrite(block + 128, 1, 128, out);  // Y block top
    // Write Y bottom 128
    fwrite(block + 256, 1, 128, out);
}

void process_block(const uint8_t* y_plane, const uint8_t* u_plane, const uint8_t* v_plane,
                   int width, int height, int x_blk, int y_blk, uint8_t* block) {
    // U and V are 8x8 per 16x16 macroblock in column-major order
    for (int col = 0; col < 8; col++) {
        for (int row = 0; row < 8; row++) {
            int ux = x_blk / 2 + col;
            int uy = y_blk / 2 + row;
            int vx = ux;
            int vy = uy;

            if (ux >= width / 2) ux = (width / 2) - 1;
            if (uy >= height / 2) uy = (height / 2) - 1;

            int u_idx = uy * (width / 2) + ux;
            int v_idx = vy * (width / 2) + vx;

            block[row * 8 + col] = u_plane[u_idx];       // U block
            block[64 + row * 8 + col] = v_plane[v_idx];  // V block
        }
    }

    // Y block (4 tiles of 8x8)
    for (int tile = 0; tile < 4; tile++) {
        int tile_x = x_blk + (tile % 2) * 8;
        int tile_y = y_blk + (tile / 2) * 8;
        for (int row = 0; row < 8; row++) {
            int y = tile_y + row;
            if (y >= height) y = height - 1;
            int src_index = y * width + tile_x;
            memcpy(&block[128 + tile * 64 + row * 8], &y_plane[src_index], 8);
        }
    }
}

int preprocess_yuv420(const char* input_yuv, const char* output_bin, int width, int height) {
    if (width % 16 != 0 || height % 16 != 0) {
        fprintf(stderr, "Error: Image dimensions must be multiples of 16 (got %dx%d)\n", width, height);
        return 0;
    }

    FILE *in = fopen(input_yuv, "rb");
    if (!in) {
        fprintf(stderr, "Error opening input file: %s\n", strerror(errno));
        return 0;
    }

    FILE *out = fopen(output_bin, "wb");
    if (!out) {
        fprintf(stderr, "Error opening output file: %s\n", strerror(errno));
        fclose(in);
        return 0;
    }

    PlaneData planes[3] = {
        {.size = width * height},
        {.size = (width/2) * (height/2)},
        {.size = (width/2) * (height/2)}
    };

    for (int i = 0; i < 3; i++) {
        planes[i].data = aligned_alloc(32, planes[i].size);
        if (!planes[i].data) {
            fprintf(stderr, "Memory allocation failed for plane %d\n", i);
            for (int j = 0; j < i; j++) free(planes[j].data);
            fclose(in);
            fclose(out);
            return 0;
        }
    }

    if (!read_plane(in, &planes[0], "Y") ||
        !read_plane(in, &planes[1], "U") ||
        !read_plane(in, &planes[2], "V")) {
        for (int i = 0; i < 3; i++) free(planes[i].data);
        fclose(in);
        fclose(out);
        return 0;
    }

    uint8_t block[BLOCK_SIZE_16x16] __attribute__((aligned(32)));
    size_t blocks_written = 0;
    int padded_width = (width + 15) & ~15;
    int padded_height = (height + 15) & ~15;

    // if (padded_width < 512) padded_width = 512;
    // if (padded_height < 256) padded_height = 256;

    for (int y_blk = 0; y_blk < padded_height; y_blk += 16) {
        for (int x_blk = 0; x_blk < padded_width; x_blk += 16) {
            if (x_blk < width && y_blk < height) {
                process_block(planes[0].data, planes[1].data, planes[2].data,
                              width, height, x_blk, y_blk, block);
            } else {
                memset(block, 128, BLOCK_SIZE_16x16);
            }

            write_macroblock_sequence(out, block);
            blocks_written++;
        }
    }

    for (int i = 0; i < 3; i++) free(planes[i].data);
    fclose(in);
    fclose(out);

    if (!quiet_mode) {
        printf("Successfully converted %dx%d (padded to %dx%d)\n",
            width, height, padded_width, padded_height);
        printf("Wrote %zu blocks (%zu bytes total)\n",
            blocks_written, blocks_written * BLOCK_SIZE_16x16);
    }
    return 1;
}

int main(int argc, char** argv) {
    if (argc != 5 && argc != 6) {
        printf("Usage: %s <input.yuv> <output.bin> <width> <height> [-q]\n", argv[0]);
        printf("Example: %s frame420.yuv romdisk/frame420.bin 512 256 -q\n", argv[0]);
        return 1;
    }

    int width = atoi(argv[3]);
    int height = atoi(argv[4]);

    if (argc == 6 && strcmp(argv[5], "-q") == 0) {
        quiet_mode = true;
    }

    if (!preprocess_yuv420(argv[1], argv[2], width, height)) {
        return 1;
    }

    return 0;
}