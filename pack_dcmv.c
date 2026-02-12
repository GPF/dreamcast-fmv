/*
 * pack_dcmv_v1.c
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
 *   7 bytes  - Padding (reserved for future use)
 *
 * Chunk Index Entry (16 bytes each):
 *   4 bytes  - Chunk offset (absolute file position)
 *   4 bytes  - Video section size (total compressed video bytes)
 *   4 bytes  - Audio size (bytes per channel, raw ADPCM)
 *   2 bytes  - Start frame (first total frame in chunk)
 *   2 bytes  - Number of frames in chunk
 *
 * Usage:
 *   pack_dcmv_v1 <output.dcmv> <frame_type> <width> <height>
 *                <scale_width> <scale_height> <fps>
 *                <sample_rate> <channels>
 *                <frame_pattern> <audio_file> <frame_durations.txt> 
 *                <compression> <chunk_duration>
 *
 * Example:
 *   ./pack_dcmv_v1 movie.dcmv 1 320 240 320 240 23.97 \
 *                  44100 1 output/frame%05d.dt audio.dca \
 *                  output/unique_frames/frame_durations.txt lz4 2.0
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
#include <math.h>
#include <lz4.h>
#include <lz4hc.h>
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

#define MAX_FRAMES 999999
#define FRAME_FILENAME_MAX 256
#define HEADER_SIZE 64
#define DT_HEADER_MAGIC "DcTx"
#define CHUNK_INDEX_ENTRY_SIZE 16

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
    uint16_t start_frame;           // First total frame in chunk
    uint16_t num_frames;            // Number of frames in chunk
} ChunkIndexEntry;

// Global state
static uint16_t *durations = NULL;
static uint32_t num_unique_frames = 0;
static uint32_t num_total_frames = 0;
static uint32_t *frame_offsets = NULL;

// Helper function prototypes
static const char* get_frame_type_name(uint8_t frame_type);
size_t load_frame_data(const char* filename, uint8_t* buffer, size_t buffer_size);
size_t get_texture_data_size(const char* filename);
int load_durations(const char *path);
void pad_to_alignment(FILE *fp, size_t alignment);
int total_to_unique_frame(int total_frame);

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
int total_to_unique_frame(int total_frame) {
    if (!durations) return total_frame;
    
    int current_total = 0;
    for (uint32_t i = 0; i < num_unique_frames; i++) {
        current_total += durations[i];
        if (total_frame < current_total) {
            return i;
        }
    }
    return num_unique_frames - 1;
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
        skip = (header_size + 1) * 32;
    }

    fseek(fp, 0, SEEK_END);
    size_t total_size = ftell(fp);
    fclose(fp);
    
    return total_size - skip;
}

// Load frame_durations.txt into durations array
// Load frame_durations.txt into durations array
// Accepts CSV with optional trailing comma and whitespace/newlines.
int load_durations(const char *path) {
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
void pad_to_alignment(FILE *fp, size_t alignment) {
    long current_pos = ftell(fp);
    long remainder = current_pos % alignment;
    if (remainder != 0) {
        long padding_needed = alignment - remainder;
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
    uint8_t frame_type = atoi(argv[2]);
    uint16_t width = atoi(argv[3]);
    uint16_t height = atoi(argv[4]);
    uint16_t scale_width = atoi(argv[5]);
    uint16_t scale_height = atoi(argv[6]);
    float fps = atof(argv[7]);
    uint16_t sample_rate = atoi(argv[8]);
    uint16_t channels = atoi(argv[9]);
    const char *frame_pattern = argv[10];
    const char *audio_path = argv[11];
    const char *durations_path = argv[12];
    const char *compression_arg = argv[13];
    float chunk_duration = atof(argv[14]);

    // Validate chunk duration
    if (chunk_duration <= 0.0f || chunk_duration > 60.0f) {
        fprintf(stderr, "❌ Invalid chunk duration: %.2f (must be 0.1-60.0 seconds)\n", chunk_duration);
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
    int frames_per_chunk = (int)(fps * chunk_duration);
    if (frames_per_chunk < 1) frames_per_chunk = 1;
    
    int num_chunks = (num_total_frames + frames_per_chunk - 1) / frames_per_chunk;
    
    printf("   Frames per chunk: %d\n", frames_per_chunk);
    printf("   Total chunks: %d\n", num_chunks);

    // Open audio file
    FILE *audio_fp = fopen(audio_path, "rb");
    if (!audio_fp) {
        perror("Audio open failed");
        return 1;
    }
    
    // Skip DcAF header if present
    char head[4];
    fread(head, 1, 4, audio_fp);
    if (memcmp(head, "DcAF", 4) == 0) {
        fseek(audio_fp, 0x40, SEEK_SET);
        printf("🔊 Skipping 64-byte DcAF header from %s\n", audio_path);
    } else {
        rewind(audio_fp);
    }
    long audio_data_start = ftell(audio_fp);

    // Get total audio size
    fseek(audio_fp, 0, SEEK_END);
    long total_audio_size = ftell(audio_fp) - audio_data_start;
    fseek(audio_fp, audio_data_start, SEEK_SET);
    printf("🔊 Audio file size: %ld bytes\n", total_audio_size);

    // Determine frame size
    char filename[FRAME_FILENAME_MAX];
    snprintf(filename, sizeof(filename), frame_pattern, 0);
    size_t frame_size = get_texture_data_size(filename);
    if (frame_size == 0) {
        fprintf(stderr, "Failed to get texture data size from first frame\n");
        return 1;
    }
    printf("📐 Frame size: %zu bytes\n", frame_size);

    // Open output file
    FILE *out = fopen(output_path, "wb+");
    if (!out) {
        perror("Output file creation failed");
        return 1;
    }

    // Allocate frame offset table (still using absolute offsets)
    frame_offsets = (uint32_t *)calloc((num_unique_frames + 1), sizeof(uint32_t));
    if (!frame_offsets) {
        fprintf(stderr, "Memory allocation failed for frame_offsets\n");
        return 1;
    }

    // Allocate chunk index
    ChunkIndexEntry *chunk_index = calloc(num_chunks, sizeof(ChunkIndexEntry));
    if (!chunk_index) {
        fprintf(stderr, "Memory allocation failed for chunk_index\n");
        return 1;
    }

    // Reserve space for header + frame offset table + duration table + chunk index
    fseek(out, HEADER_SIZE, SEEK_SET);
    
    long frame_offset_table_pos = ftell(out);
    fseek(out, (num_unique_frames + 1) * sizeof(uint32_t), SEEK_CUR);
    
    long duration_table_pos = ftell(out);
    fseek(out, num_unique_frames * sizeof(uint16_t), SEEK_CUR);
    
    long chunk_index_table_pos = ftell(out);
    fseek(out, num_chunks * CHUNK_INDEX_ENTRY_SIZE, SEEK_CUR);

    // Align to 32 bytes before first chunk
    // pad_to_alignment(out, 32);

    // Allocate compression buffers
    uint8_t *frame_buf = malloc(frame_size);
    if (!frame_buf) {
        fprintf(stderr, "Memory allocation failed for frame_buf\n");
        return 1;
    }

    uint8_t *compressed_buf = NULL;
    ZSTD_CCtx *cctx = NULL;
    
    if (use_zstd) {
        cctx = ZSTD_createCCtx();
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_format, ZSTD_f_zstd1_magicless);
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_windowLog, 16);
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 13);
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_enableLongDistanceMatching, 0);
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 0);
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_contentSizeFlag, 0);
        size_t bound = ZSTD_compressBound(frame_size);
        compressed_buf = malloc(bound);
    } else {
        compressed_buf = malloc(LZ4_compressBound(frame_size));
    }
    
    if (!compressed_buf) {
        fprintf(stderr, "Memory allocation failed for compressed_buf\n");
        return 1;
    }

    // Audio buffer for reading
    size_t audio_samples_per_chunk = (size_t)(sample_rate * chunk_duration);
    size_t audio_bytes_per_channel = audio_samples_per_chunk / 2; // ADPCM 4-bit
    uint8_t *audio_buffer = malloc(audio_bytes_per_channel * channels);
    if (!audio_buffer) {
        fprintf(stderr, "Memory allocation failed for audio_buffer\n");
        return 1;
    }

    uint32_t max_compressed_size = 0;
    uint64_t total_compressed_bytes = 0;

    printf("\n🗜️  Encoding chunks...\n");
    
    // ========================================================================
    // MAIN ENCODING LOOP - Process each chunk
    // ========================================================================
    
    for (int chunk_id = 0; chunk_id < num_chunks; chunk_id++) {
        // Calculate frame range for this chunk
        int start_frame = chunk_id * frames_per_chunk;
        int end_frame = start_frame + frames_per_chunk;
        if (end_frame > (int)num_total_frames) {
            end_frame = num_total_frames;
        }
        int frames_in_chunk = end_frame - start_frame;

        // Record chunk start
        chunk_index[chunk_id].chunk_offset = ftell(out);
        chunk_index[chunk_id].start_frame = start_frame;
        chunk_index[chunk_id].num_frames = frames_in_chunk;

        printf("\r📦 Chunk %d/%d: frames %d-%d (%d frames)...", 
               chunk_id + 1, num_chunks, start_frame, end_frame - 1, frames_in_chunk);
        fflush(stdout);

        // Track unique frames we've already compressed in this chunk
        uint8_t *already_compressed = (uint8_t *)calloc(num_unique_frames, 1);
        if (!already_compressed) {
            fprintf(stderr, "\n❌ calloc failed for already_compressed (%u)\n",
                    (unsigned)num_unique_frames);
            return 1;
        }

        // Compress frames for this chunk
        uint32_t video_section_size = 0;

        for (int total_frame = start_frame; total_frame < end_frame; total_frame++) {
            int unique_frame = total_to_unique_frame(total_frame);

            // HARD GUARD: if mapping ever goes out of range, fail fast (no heap corruption)
            if (unique_frame < 0 || (uint32_t)unique_frame >= num_unique_frames) {
                fprintf(stderr,
                        "\n❌ total_to_unique_frame(%d) -> %d out of range (num_unique_frames=%u)\n",
                        total_frame, unique_frame, (unsigned)num_unique_frames);
                free(already_compressed);
                return 1;
            }
            // if (chunk_id >= num_chunks - 3) {
            //     printf("DBG chunk=%d total_frame=%d unique_frame=%d\n",
            //         chunk_id, total_frame, unique_frame);
            // }
            // Only compress each unique frame once per chunk
            if (already_compressed[unique_frame]) {
                continue;
            }
            already_compressed[unique_frame] = 1;

            // Load frame data
            snprintf(filename, sizeof(filename), frame_pattern, unique_frame);
            size_t read_size = load_frame_data(filename, frame_buf, frame_size);
            if (read_size != frame_size) {
                fprintf(stderr,
                        "\n❌ Frame %d load error (read=%zu expected=%zu) file=%s\n",
                        unique_frame, read_size, frame_size, filename);
                free(already_compressed);
                return 1;
            }

            // Compress frame
            size_t comp_size;
            if (use_zstd) {
                ZSTD_CCtx_reset(cctx, ZSTD_reset_session_only);

                ZSTD_inBuffer input  = { frame_buf, frame_size, 0 };
                ZSTD_outBuffer output = { compressed_buf, ZSTD_compressBound(frame_size), 0 };

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
                    (int)LZ4_compressBound((int)frame_size),
                    12
                );
                if (result <= 0) {
                    fprintf(stderr, "\n❌ LZ4 compression failed (frame=%d)\n", unique_frame);
                    free(already_compressed);
                    return 1;
                }
                comp_size = (size_t)result;
            }

            // Write compressed frame
            if (fwrite(compressed_buf, 1, comp_size, out) != comp_size) {
                fprintf(stderr, "\n❌ fwrite failed for frame %d\n", unique_frame);
                free(already_compressed);
                return 1;
            }
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

        // ✅ CRITICAL FIX: Pad to 32-byte alignment BEFORE audio section
        // This ensures audio starts at a 32-byte aligned address for AICA ADPCM
        pad_to_alignment(out, 32);

        // Write audio for this chunk (assumes planar source for stereo: [all L][all R])
        {
            const double chunk_start_time       = chunk_id * (double)chunk_duration;
            const double actual_chunk_duration  = (double)frames_in_chunk / (double)fps;

            // ADPCM 4-bit => bytes/sec/channel = sample_rate / 2
            const size_t bytes_per_sec_ch = (size_t)(sample_rate / 2);

            // How many bytes we WANT per channel for this chunk
            size_t want_bytes = (size_t)(actual_chunk_duration * (double)bytes_per_sec_ch);

            // How many bytes exist per channel in the file (planar)
            const long per_ch_total = (channels == 2) ? (total_audio_size / 2) : total_audio_size;

            // Where we want to start reading within a channel
            long rel = (long)(chunk_start_time * (double)bytes_per_sec_ch);

            // If we’re beyond EOF for this channel, write silence for the whole chunk
            if (rel < 0 || rel >= per_ch_total) {
                size_t silence_bytes = (want_bytes > 0) ? want_bytes : bytes_per_sec_ch;
                memset(audio_buffer, 0, silence_bytes);
                fwrite(audio_buffer, 1, silence_bytes, out);
                if (channels == 2) {
                    fwrite(audio_buffer, 1, silence_bytes, out);
                }
                chunk_index[chunk_id].audio_size = silence_bytes;
            } else {
                // Clamp to available bytes in channel
                long avail = per_ch_total - rel;
                size_t to_read = want_bytes;
                if ((long)to_read > avail) to_read = (size_t)avail;

                // Always write exactly want_bytes per channel (pad tail with zeros)
                if (to_read < want_bytes) {
                    // read what we can, then zero-fill remainder
                    memset(audio_buffer + to_read, 0, want_bytes - to_read);
                }

                if (channels == 1) {
                    fseek(audio_fp, audio_data_start + rel, SEEK_SET);
                    size_t got = fread(audio_buffer, 1, to_read, audio_fp);
                    if (got < to_read) memset(audio_buffer + got, 0, to_read - got);

                    fwrite(audio_buffer, 1, want_bytes, out);
                    chunk_index[chunk_id].audio_size = want_bytes;
                } else {
                    // LEFT
                    fseek(audio_fp, audio_data_start + rel, SEEK_SET);
                    size_t gotL = fread(audio_buffer, 1, to_read, audio_fp);
                    if (gotL < to_read) memset(audio_buffer + gotL, 0, to_read - gotL);
                    fwrite(audio_buffer, 1, want_bytes, out);

                    // RIGHT (second half of file)
                    fseek(audio_fp, audio_data_start + per_ch_total + rel, SEEK_SET);
                    size_t gotR = fread(audio_buffer, 1, to_read, audio_fp);
                    if (gotR < to_read) memset(audio_buffer + gotR, 0, to_read - gotR);
                    fwrite(audio_buffer, 1, want_bytes, out);

                    chunk_index[chunk_id].audio_size = want_bytes;
                }
            }
        }
    }
    
    printf("\n✅ All chunks encoded\n");

    fclose(audio_fp);

    // Compression statistics
    double avg_compressed = (double)total_compressed_bytes / num_unique_frames;
    double ratio = (double)total_compressed_bytes / ((double)frame_size * num_unique_frames);
    
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
    header.uncompressed_frame_size = frame_size;
    header.max_compressed_frame_size = max_compressed_size;
    header.compression_type = use_zstd ? 1 : 0;
    header.chunk_duration = chunk_duration;
    header.num_chunks = num_chunks;
    header.chunk_index_offset = chunk_index_table_pos;

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
    fwrite(chunk_index, CHUNK_INDEX_ENTRY_SIZE, num_chunks, out);

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
