/*
 * pack_dcmv.c
 * ---------------------
 * Dreamcast movie packer utility for the custom .dcmv v1.0 format.
 *
 * NEW in v1.0 (Chunked Format):
 *   - Container organized into time-based chunks (configurable duration)
 *   - Each chunk contains: compressed frames + raw ADPCM audio for that time period
 *   - Optimized for CDR sequential reading (minimal seeking)
 *   - Chunk index table for random access
 *   - Still supports frame deduplication and flexible compression
 *
 * Header format (64 bytes total):
 *   4 bytes  - Magic "DCMV"
 *   4 bytes  - Version (1)
 *   1 byte   - Frame type (0 = RGB565, 1 = YUV422)
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
 *   4 bytes  - Maximum compressed frame size (for LZ4 or Zstd)
 *   1 byte   - Compression type (0 = LZ4, 1 = Zstandard)
 *   4 bytes  - Chunk duration (float, e.g., 2.0 seconds)
 *   4 bytes  - Number of chunks
 *   4 bytes  - Chunk index offset (absolute file position)
 *   10 bytes - Padding (reserved for future use)
 *
 * Chunk Index Entry (20 bytes each)  [UPDATED v1.0 FIX]:
 *   4 bytes  - Chunk offset (absolute file position)
 *   4 bytes  - Video section size (total compressed video bytes)
 *   4 bytes  - Audio size (bytes per channel, raw ADPCM)
 *   4 bytes  - Start frame (first TOTAL frame in chunk)
 *   4 bytes  - Number of TOTAL frames in chunk
 *
 * Usage:
 *   pack_dcmv_v1 <output.dcmv> <frame_type> <width> <height>
 *                <scale_width> <scale_height> <fps>
 *                <sample_rate> <channels>
 *                <frame_pattern> <audio_file> <frame_durations.txt>
 *                <compression> <chunk_duration>
 *
 * Author: Troy Davis (gpf)
 * License: Public Domain / MIT-style — use freely with attribution.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <math.h>

#include <lz4.h>
#include <lz4hc.h>

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

#define MAX_FRAMES 999999
#define FRAME_FILENAME_MAX 256
#define HEADER_SIZE 64
#define DT_HEADER_MAGIC "DcTx"
#define CHUNK_INDEX_ENTRY_SIZE 20  // UPDATED (was 16)

#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif

static size_t align_up_size(size_t v, size_t a) {
    return (v + (a - 1)) & ~(a - 1);
}

// Structures
typedef struct __attribute__((packed)) {
    char magic[4];                  // "DCMV"
    uint32_t version;               // 1
    uint8_t frame_type;             // 0=RGB565, 1=YUV422
    uint16_t tex_width;
    uint16_t tex_height;
    uint16_t content_width;
    uint16_t content_height;
    float fps;
    uint16_t sample_rate;
    uint16_t channels;
    uint32_t num_unique_frames;
    uint32_t num_total_frames;
    uint32_t uncompressed_frame_size;
    uint32_t max_compressed_frame_size;
    uint8_t compression_type;       // 0=LZ4, 1=Zstd
    float chunk_duration;           // Seconds per chunk
    uint32_t num_chunks;
    uint32_t chunk_index_offset;
    uint8_t padding[10];
} DCMVHeader;

typedef struct __attribute__((packed)) {
    uint32_t chunk_offset;          // Absolute file offset
    uint32_t video_section_size;    // Total compressed video bytes
    uint32_t audio_size;            // Bytes per channel (raw ADPCM)
    uint32_t start_frame;           // First TOTAL frame in chunk
    uint32_t num_frames;            // TOTAL frames in chunk
} ChunkIndexEntry;

// Global state
static uint16_t *durations = NULL;
static uint32_t num_unique_frames = 0;
static uint32_t num_total_frames = 0;
static uint32_t *frame_offsets = NULL;

// Helper function prototypes
static const char* get_frame_type_name(uint8_t frame_type);
static size_t load_frame_data(const char* filename, uint8_t* buffer, size_t buffer_size);
static size_t get_texture_data_size(const char* filename);
static int load_durations(const char *path);
static void pad_to_alignment(FILE *fp, size_t alignment);
static int total_to_unique_frame(int total_frame);

// ============================================================================
// Helper Functions
// ============================================================================

static const char* get_frame_type_name(uint8_t frame_type) {
    switch(frame_type) {
        case 0: return "RGB565";
        case 1: return "YUV422";
        default: return "Unknown";
    }
}

// Convert total frame index to unique frame index
static int total_to_unique_frame(int total_frame) {
    if (!durations) return total_frame;

    int current_total = 0;
    for (uint32_t i = 0; i < num_unique_frames; i++) {
        current_total += durations[i];
        if (total_frame < current_total) {
            return (int)i;
        }
    }
    return (int)(num_unique_frames - 1);
}

// Function to load frame data, stripping DT header if present
static size_t load_frame_data(const char* filename, uint8_t* buffer, size_t buffer_size) {
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
        skip = (size_t)(header_size + 1) * 32;
        if (fseek(fp, (long)skip, SEEK_SET) != 0) {
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
static size_t get_texture_data_size(const char* filename) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        printf("❌ ERROR: Could not open file: %s\n", filename);
        return 0;
    }

    uint8_t header_buf[32];
    size_t header_read = fread(header_buf, 1, sizeof(header_buf), fp);
    if (header_read < sizeof(header_buf)) {
        printf("❌ ERROR: Could not read header from: %s (read %zu bytes)\n", filename, header_read);
        fclose(fp);
        return 0;
    }

    size_t skip = 0;
    if (memcmp(header_buf, DT_HEADER_MAGIC, 4) == 0) {
        uint8_t header_size = header_buf[9];
        skip = (size_t)(header_size + 1) * 32;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return 0;
    }

    long endpos = ftell(fp);
    fclose(fp);

    if (endpos < 0) return 0;
    size_t total_size = (size_t)endpos;
    if (total_size < skip) return 0;

    return total_size - skip;
}

// Load frame_durations.txt into durations array
// Accepts CSV with optional trailing comma and whitespace/newlines.
static int load_durations(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        perror("Failed to open frame_durations.txt");
        return 1;
    }

    // First pass: count entries + total frames
    num_unique_frames = 0;
    num_total_frames  = 0;

    int value;
    while (fscanf(fp, " %d%*[, \t\r\n]", &value) == 1) {
        if (value < 0) value = 0;
        num_unique_frames++;
        num_total_frames += (uint32_t)value;
    }

    if (num_unique_frames == 0) {
        fprintf(stderr, "❌ durations file appears empty or unparsable: %s\n", path);
        fclose(fp);
        return 1;
    }

    rewind(fp);

    durations = (uint16_t *)malloc(num_unique_frames * sizeof(uint16_t));
    if (!durations) {
        fprintf(stderr, "Memory allocation failed for durations (%u entries)\n",
                (unsigned)num_unique_frames);
        fclose(fp);
        return 1;
    }

    uint32_t i = 0;
    while (i < num_unique_frames && fscanf(fp, " %d%*[, \t\r\n]", &value) == 1) {
        if (value < 0) value = 0;
        if (value > 65535) value = 65535;
        durations[i++] = (uint16_t)value;
    }

    if (i != num_unique_frames) {
        fprintf(stderr,
                "❌ durations parse mismatch: expected %u entries, got %u\n",
                (unsigned)num_unique_frames, (unsigned)i);
        free(durations);
        durations = NULL;
        fclose(fp);
        return 1;
    }

    fclose(fp);
    return 0;
}

// Function to pad file to next alignment boundary
static void pad_to_alignment(FILE *fp, size_t alignment) {
    long current_pos = ftell(fp);
    if (current_pos < 0) return;

    long remainder = current_pos % (long)alignment;
    if (remainder != 0) {
        long padding_needed = (long)alignment - remainder;
        uint8_t zero = 0;
        for (long i = 0; i < padding_needed; i++) {
            fwrite(&zero, 1, 1, fp);
        }
    }
}

// ============================================================================
// Main Program
// ============================================================================

int main(int argc, char **argv) {
    if (argc != 15) {
        printf("Usage: %s <output.dcmv> <frame_type> <width> <height> "
               "<scale_width> <scale_height> <fps> "
               "<sample_rate> <channels> "
               "<frame_pattern> <audio_file> <frame_durations.txt> "
               "<compression> <chunk_duration>\n", argv[0]);
        printf("\nExample:\n");
        printf("  %s movie.dcmv 1 320 240 320 240 23.97 44100 1 \\\n", argv[0]);
        printf("     frames/frame%%05d.dt audio.dca durations.txt lz4 2.0\n");
        printf("\nCompression: lz4 or zstd\n");
        printf("Chunk duration: seconds per chunk (e.g., 1.0, 2.0, 5.0)\n");
        return 1;
    }

    // Parse arguments
    const char *output_path = argv[1];
    uint8_t frame_type = (uint8_t)atoi(argv[2]);
    uint16_t width = (uint16_t)atoi(argv[3]);
    uint16_t height = (uint16_t)atoi(argv[4]);
    uint16_t scale_width = (uint16_t)atoi(argv[5]);
    uint16_t scale_height = (uint16_t)atoi(argv[6]);
    float fps = (float)atof(argv[7]);
    uint16_t sample_rate = (uint16_t)atoi(argv[8]);
    uint16_t channels = (uint16_t)atoi(argv[9]);
    const char *frame_pattern = argv[10];
    const char *audio_path = argv[11];
    const char *durations_path = argv[12];
    const char *compression_arg = argv[13];
    float chunk_duration = (float)atof(argv[14]);

    // Validate chunk duration
    if (chunk_duration <= 0.0f || chunk_duration > 60.0f) {
        fprintf(stderr, "❌ Invalid chunk duration: %.2f (must be 0.1-60.0 seconds)\n", chunk_duration);
        return 1;
    }

    if (fps <= 0.0f) {
        fprintf(stderr, "❌ Invalid fps: %.4f\n", fps);
        return 1;
    }

    if (channels != 1 && channels != 2) {
        fprintf(stderr, "❌ Invalid channels: %u (must be 1 or 2)\n", (unsigned)channels);
        return 1;
    }

    // Determine compression
    int use_zstd = 0;
    if (strcmp(compression_arg, "zstd") == 0) {
        use_zstd = 1;
    } else if (strcmp(compression_arg, "lz4") != 0) {
        fprintf(stderr, "❌ Invalid compression: %s (use 'lz4' or 'zstd')\n", compression_arg);
        return 1;
    }

    printf("📦 DCMV v1.0 Chunked Format Packer\n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

    // Load frame durations
    if (load_durations(durations_path) != 0) {
        return 1;
    }

    printf("📊 Video Configuration:\n");
    printf("   Format: %s, %dx%d (content: %dx%d)\n",
           get_frame_type_name(frame_type), width, height, scale_width, scale_height);
    printf("   FPS: %.2f, Audio: %dHz, %d channel(s)\n", fps, sample_rate, channels);
    printf("   Unique frames: %u, Total frames: %u\n", num_unique_frames, num_total_frames);
    printf("   Compression: %s\n", use_zstd ? "Zstandard" : "LZ4");
    printf("   Chunk duration: %.2f seconds\n", chunk_duration);

    // Calculate chunk parameters
    // Use rounding instead of floor to better match requested duration.
    int frames_per_chunk = (int)lroundf(fps * chunk_duration);
    if (frames_per_chunk < 1) frames_per_chunk = 1;

    int num_chunks = (int)((num_total_frames + (uint32_t)frames_per_chunk - 1) / (uint32_t)frames_per_chunk);

    printf("   Frames per chunk: %d\n", frames_per_chunk);
    printf("   Total chunks: %d\n", num_chunks);

    // Open audio file
    FILE *audio_fp = fopen(audio_path, "rb");
    if (!audio_fp) {
        perror("Audio open failed");
        return 1;
    }

    // Skip DcAF header if present (64 bytes)
    long audio_data_start = 0;
    {
        char head[4];
        size_t got = fread(head, 1, 4, audio_fp);
        if (got == 4 && memcmp(head, "DcAF", 4) == 0) {
            fseek(audio_fp, 0x40, SEEK_SET);
            printf("🔊 Skipping 64-byte DcAF header from %s\n", audio_path);
        } else {
            rewind(audio_fp);
        }
        audio_data_start = ftell(audio_fp);
        if (audio_data_start < 0) audio_data_start = 0;
    }

    // Get total audio size
    fseek(audio_fp, 0, SEEK_END);
    long audio_end = ftell(audio_fp);
    if (audio_end < 0) audio_end = 0;
    long total_audio_size = (audio_end - audio_data_start);
    if (total_audio_size < 0) total_audio_size = 0;
    fseek(audio_fp, audio_data_start, SEEK_SET);
    printf("🔊 Audio file size: %ld bytes\n", total_audio_size);

    // Determine frame size
    char filename[FRAME_FILENAME_MAX];
    snprintf(filename, sizeof(filename), frame_pattern, 0);
    size_t frame_size = get_texture_data_size(filename);
    if (frame_size == 0) {
        fprintf(stderr, "Failed to get texture data size from first frame\n");
        fclose(audio_fp);
        return 1;
    }
    printf("📐 Frame size: %zu bytes\n", frame_size);

    // Open output file
    FILE *out = fopen(output_path, "wb+");
    if (!out) {
        perror("Output file creation failed");
        fclose(audio_fp);
        return 1;
    }

    // Allocate frame offset table (kept as-is: stores per-unique metadata; you currently store comp_size)
    frame_offsets = (uint32_t *)calloc((num_unique_frames + 1), sizeof(uint32_t));
    if (!frame_offsets) {
        fprintf(stderr, "Memory allocation failed for frame_offsets\n");
        fclose(audio_fp);
        fclose(out);
        return 1;
    }

    // Allocate chunk index
    ChunkIndexEntry *chunk_index = (ChunkIndexEntry *)calloc((size_t)num_chunks, sizeof(ChunkIndexEntry));
    if (!chunk_index) {
        fprintf(stderr, "Memory allocation failed for chunk_index\n");
        fclose(audio_fp);
        fclose(out);
        free(frame_offsets);
        return 1;
    }

    // Reserve space for header + frame offset table + duration table + chunk index
    fseek(out, HEADER_SIZE, SEEK_SET);

    long frame_offset_table_pos = ftell(out);
    fseek(out, (long)((num_unique_frames + 1) * sizeof(uint32_t)), SEEK_CUR);

    long duration_table_pos = ftell(out);
    fseek(out, (long)(num_unique_frames * sizeof(uint16_t)), SEEK_CUR);

    long chunk_index_table_pos = ftell(out);
    fseek(out, (long)(num_chunks * (int)sizeof(ChunkIndexEntry)), SEEK_CUR);

    // NEW: Align start of first chunk to 2048 (CD sector)
    pad_to_alignment(out, 2048);

    // Allocate compression buffers
    uint8_t *frame_buf = (uint8_t *)malloc(frame_size);
    if (!frame_buf) {
        fprintf(stderr, "Memory allocation failed for frame_buf\n");
        fclose(audio_fp);
        fclose(out);
        free(frame_offsets);
        free(chunk_index);
        return 1;
    }

    uint8_t *compressed_buf = NULL;
    ZSTD_CCtx *cctx = NULL;

    size_t comp_bound = 0;
    if (use_zstd) {
        cctx = ZSTD_createCCtx();
        if (!cctx) {
            fprintf(stderr, "❌ ZSTD_createCCtx failed\n");
            fclose(audio_fp);
            fclose(out);
            free(frame_offsets);
            free(chunk_index);
            free(frame_buf);
            return 1;
        }

        ZSTD_CCtx_setParameter(cctx, ZSTD_c_format, ZSTD_f_zstd1_magicless);
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_windowLog, 16);
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 13);
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_enableLongDistanceMatching, 0);
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 0);
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_contentSizeFlag, 0);

        comp_bound = ZSTD_compressBound(frame_size);
    } else {
        comp_bound = (size_t)LZ4_compressBound((int)frame_size);
    }

    compressed_buf = (uint8_t *)malloc(comp_bound);
    if (!compressed_buf) {
        fprintf(stderr, "Memory allocation failed for compressed_buf\n");
        fclose(audio_fp);
        fclose(out);
        free(frame_offsets);
        free(chunk_index);
        free(frame_buf);
        if (cctx) ZSTD_freeCCtx(cctx);
        return 1;
    }

    // Audio buffer (allocate to worst-case per chunk, per channel)
    // ADPCM 4-bit: bytes/sec per channel = sample_rate/2
    const size_t bytes_per_sec_ch = (size_t)(sample_rate / 2);
    size_t max_audio_bytes_per_ch = (size_t)ceil((double)chunk_duration * (double)bytes_per_sec_ch);
    if (max_audio_bytes_per_ch < 1) max_audio_bytes_per_ch = 1;

    // keep even + 32-aligned friendly
    max_audio_bytes_per_ch = (max_audio_bytes_per_ch + 1) & ~((size_t)1);
    max_audio_bytes_per_ch = align_up_size(max_audio_bytes_per_ch, 32);

    uint8_t *audio_buffer = (uint8_t *)malloc(max_audio_bytes_per_ch);
    if (!audio_buffer) {
        fprintf(stderr, "Memory allocation failed for audio_buffer\n");
        fclose(audio_fp);
        fclose(out);
        free(frame_offsets);
        free(chunk_index);
        free(frame_buf);
        free(compressed_buf);
        if (cctx) ZSTD_freeCCtx(cctx);
        return 1;
    }

    uint32_t max_compressed_size = 0;
    uint64_t total_compressed_bytes = 0;

    printf("\n🗜️  Encoding chunks...\n");

    // ========================================================================
    // MAIN ENCODING LOOP - Process each chunk
    // ========================================================================

    for (int chunk_id = 0; chunk_id < num_chunks; chunk_id++) {
        uint32_t start_frame = (uint32_t)chunk_id * (uint32_t)frames_per_chunk;
        uint32_t end_frame = start_frame + (uint32_t)frames_per_chunk;
        if (end_frame > num_total_frames) {
            end_frame = num_total_frames;
        }
        uint32_t frames_in_chunk = end_frame - start_frame;

        // Record chunk start
        long chunk_pos = ftell(out);
        if (chunk_pos < 0) chunk_pos = 0;

        chunk_index[chunk_id].chunk_offset = (uint32_t)chunk_pos;
        chunk_index[chunk_id].start_frame  = start_frame;
        chunk_index[chunk_id].num_frames   = frames_in_chunk;

        printf("\r📦 Chunk %d/%d: frames %u-%u (%u frames)...",
               chunk_id + 1, num_chunks,
               start_frame,
               end_frame ? (end_frame - 1) : 0,
               frames_in_chunk);
        fflush(stdout);

        // Track unique frames we've already compressed in this chunk
        uint8_t *already_compressed = (uint8_t *)calloc(num_unique_frames, 1);
        if (!already_compressed) {
            fprintf(stderr, "\n❌ calloc failed for already_compressed (%u)\n",
                    (unsigned)num_unique_frames);
            fclose(audio_fp);
            fclose(out);
            free(frame_offsets);
            free(chunk_index);
            free(frame_buf);
            free(compressed_buf);
            free(audio_buffer);
            if (cctx) ZSTD_freeCCtx(cctx);
            return 1;
        }

        uint32_t video_section_size = 0;

        for (uint32_t total_frame = start_frame; total_frame < end_frame; total_frame++) {
            int unique_frame = total_to_unique_frame((int)total_frame);

            if (unique_frame < 0 || (uint32_t)unique_frame >= num_unique_frames) {
                fprintf(stderr,
                        "\n❌ total_to_unique_frame(%u) -> %d out of range (num_unique_frames=%u)\n",
                        total_frame, unique_frame, (unsigned)num_unique_frames);
                free(already_compressed);
                return 1;
            }

            if (already_compressed[unique_frame]) {
                continue;
            }
            already_compressed[unique_frame] = 1;

            snprintf(filename, sizeof(filename), frame_pattern, unique_frame);
            size_t read_size = load_frame_data(filename, frame_buf, frame_size);
            if (read_size != frame_size) {
                fprintf(stderr,
                        "\n❌ Frame %d load error (read=%zu expected=%zu) file=%s\n",
                        unique_frame, read_size, frame_size, filename);
                free(already_compressed);
                return 1;
            }

            size_t comp_size;
            if (use_zstd) {
                ZSTD_CCtx_reset(cctx, ZSTD_reset_session_only);

                ZSTD_inBuffer input   = { frame_buf, frame_size, 0 };
                ZSTD_outBuffer output = { compressed_buf, comp_bound, 0 };

                size_t res = ZSTD_compressStream2(cctx, &output, &input, ZSTD_e_end);
                if (ZSTD_isError(res)) {
                    fprintf(stderr, "\n❌ ZSTD compress error: %s\n", ZSTD_getErrorName(res));
                    free(already_compressed);
                    return 1;
                }
                comp_size = output.pos;
            } else {
                int result = LZ4_compress_HC(
                    (const char *)frame_buf,
                    (char *)compressed_buf,
                    (int)frame_size,
                    (int)comp_bound,
                    12
                );
                if (result <= 0) {
                    fprintf(stderr, "\n❌ LZ4 compression failed (frame=%d)\n", unique_frame);
                    free(already_compressed);
                    return 1;
                }
                comp_size = (size_t)result;
            }

            if (fwrite(compressed_buf, 1, comp_size, out) != comp_size) {
                fprintf(stderr, "\n❌ fwrite failed for frame %d\n", unique_frame);
                free(already_compressed);
                return 1;
            }

            // Preserve your existing semantics: store comp_size in the table.
            {
                uint32_t flags = 0;
                frame_offsets[unique_frame] =
                    (flags & 0xE0000000) | ((uint32_t)comp_size & 0x1FFFFFFF);
            }

            video_section_size += (uint32_t)comp_size;
            total_compressed_bytes += (uint64_t)comp_size;

            if (comp_size > max_compressed_size) {
                max_compressed_size = (uint32_t)comp_size;
            }
        }

        free(already_compressed);
        chunk_index[chunk_id].video_section_size = video_section_size;

// Align BEFORE audio for DMA friendliness
pad_to_alignment(out, 32);

{
    const long per_ch_total = (channels == 2) ? (total_audio_size / 2) : total_audio_size;

    // Frame->byte mapping based on REAL file size (opaque ADPCM stream)
    auto uint64_t byte_pos_for_frame(uint32_t tf) {
        return ((uint64_t)tf * (uint64_t)per_ch_total) / (uint64_t)num_total_frames;
    }

    uint64_t start_b = byte_pos_for_frame(start_frame);
    uint64_t end_b   = byte_pos_for_frame(end_frame);

    if (end_b < start_b) end_b = start_b;
    size_t payload_bytes = (size_t)(end_b - start_b);

    // ADPCM safety: align boundaries (start with 32; if still blasts, try 256)
    const size_t ADPCM_ALIGN = 32;

    // round start down, end down, so we don't ever jump into the middle of a block
    start_b &= ~((uint64_t)ADPCM_ALIGN - 1);
    end_b   &= ~((uint64_t)ADPCM_ALIGN - 1);
    if (end_b < start_b) end_b = start_b;

    payload_bytes = (size_t)(end_b - start_b);
    if (payload_bytes < 1) payload_bytes = ADPCM_ALIGN;

    // what we write (pad up to 32B so player can copy in 32B units)
    size_t written_bytes = align_up_size(payload_bytes, 32);

    // clamp for buffer safety
    if (written_bytes > max_audio_bytes_per_ch) written_bytes = max_audio_bytes_per_ch;
    if (payload_bytes > written_bytes) payload_bytes = written_bytes;

    // Index should describe REAL payload bytes per channel
    chunk_index[chunk_id].audio_size = (uint32_t)payload_bytes;

    // Helper to write one channel payload
    auto void write_channel(long base_off) {
        // base_off is audio_data_start (+0 for L, +per_ch_total for R)
        memset(audio_buffer, 0, written_bytes);

        if ((long)start_b < per_ch_total) {
            long avail = per_ch_total - (long)start_b;
            size_t to_read = payload_bytes;
            if ((long)to_read > avail) to_read = (size_t)avail;

            fseek(audio_fp, base_off + (long)start_b, SEEK_SET);
            fread(audio_buffer, 1, to_read, audio_fp);
        }

        // zero padding already in audio_buffer to written_bytes
        fwrite(audio_buffer, 1, written_bytes, out);
    };

    if (channels == 1) {
        write_channel(audio_data_start);
    } else {
        write_channel(audio_data_start);                 // LEFT
        write_channel(audio_data_start + per_ch_total);  // RIGHT (planar)
    }
}

// Chunk end aligned to sector boundary
pad_to_alignment(out, 2048);
    }

    printf("\n✅ All chunks encoded\n");

    fclose(audio_fp);

    double avg_compressed = (double)total_compressed_bytes / (double)num_unique_frames;
    double ratio = (double)total_compressed_bytes / ((double)frame_size * (double)num_unique_frames);

    printf("\n📊 Compression Statistics:\n");
    printf("   Max compressed frame: %u bytes\n", max_compressed_size);
    printf("   Avg compressed frame: %.1f bytes\n", avg_compressed);
    printf("   Compression ratio: %.2f%%\n", ratio * 100.0);

    // Write header
    DCMVHeader header;
    memset(&header, 0, sizeof(header));
    memcpy(header.magic, "DCMV", 4);
    header.version = 1;
    header.frame_type = frame_type;
    header.tex_width = width;
    header.tex_height = height;
    header.content_width = scale_width;
    header.content_height = scale_height;
    header.fps = fps;
    header.sample_rate = sample_rate;
    header.channels = channels;
    header.num_unique_frames = num_unique_frames;
    header.num_total_frames = num_total_frames;
    header.uncompressed_frame_size = (uint32_t)frame_size;
    header.max_compressed_frame_size = max_compressed_size;
    header.compression_type = (uint8_t)(use_zstd ? 1 : 0);
    header.chunk_duration = chunk_duration;
    header.num_chunks = (uint32_t)num_chunks;
    header.chunk_index_offset = (uint32_t)chunk_index_table_pos;

    fseek(out, 0, SEEK_SET);
    fwrite(&header, sizeof(DCMVHeader), 1, out);

    // Write frame offset table
    fseek(out, frame_offset_table_pos, SEEK_SET);
    fwrite(frame_offsets, sizeof(uint32_t), num_unique_frames + 1, out);

    // Write duration table
    fseek(out, duration_table_pos, SEEK_SET);
    fwrite(durations, sizeof(uint16_t), num_unique_frames, out);

    // Write chunk index table
    fseek(out, chunk_index_table_pos, SEEK_SET);
    fwrite(chunk_index, sizeof(ChunkIndexEntry), (size_t)num_chunks, out);

    // Cleanup
    free(frame_offsets);
    free(durations);
    free(frame_buf);
    free(compressed_buf);
    free(audio_buffer);
    free(chunk_index);
    if (cctx) ZSTD_freeCCtx(cctx);
    fclose(out);

    printf("\n✅ DCMV v1.0 chunked file created successfully: %s\n", output_path);
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

    return 0;
}