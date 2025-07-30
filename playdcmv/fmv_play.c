/**
 * fmv_play.c - Dreamcast FMV (Full Motion Video) Player
 * -----------------------------------------------------
 * This is the runtime video player for .dcmv containers, designed for the Sega Dreamcast.
 * It loads and decompresses LZ4-compressed VQ PVR textures on the fly and synchronizes
 * them to ADPCM audio streamed via the KOS sound API.
 *
 * Features:
 * - Parses custom DCMV v4 container format (video+audio in one file)
 * - Supports both strided and power-of-two texture formats
 * - Uses LZ4 decompression for each video frame
 * - Leverages PVR DMA and VQ textures for efficient rendering
 * - Streams audio using snd_stream with optional stereo/mono handling
 * - Uses profiler integration for performance tuning
 *
 * Author: Troy Davis (GPF) — https://github.com/GPF
 * License: Public Domain / MIT-style — use freely with attribution.
 *
 * Controls:
 * - Press A: Take screenshot (/pc/screenshot#.ppm)
 * - DPad Left and Right seek to +/-500 frames
 * - Press any other button: Exit cleanly
 *
 * Dependencies:
 * - KallistiOS (KOS)
 * - LZ4
 * - dcprofiler (optional, used for analysis)
 *
 * Related tools:
 * - pack_dcmv (LZ4-based container builder)
 * - convert_to_pvr_fmv.sh (FFmpeg + pvrtex + dcaconv automation)
 */

#include <kos.h>
#include <dc/sound/stream.h>
#include <dc/sound/sound.h>
#include <dc/pvr.h>
#include <dc/maple/controller.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <fastmem/fastmem.h>
#define LZ4_memcpy(d,s,n) memcpy_fast((d),(s),(n))
#define LZ4_memmove(d,s,n) memmove_fast((d),(s),(n))
#define LZ4_memset(d,s,n) memset_fast((d),(s),(n))
#define LZ4_FREESTANDING 1
#include <lz4/lz4.h>

#define DCMV_MAGIC "DCMV"
#define VIDEO_FILE "/pc/movie.dcmv"

// static FILE *fp = NULL, *audio_fp_left = NULL, *audio_fp_right = NULL;
static file_t video_fd = -1;    // replaces FILE *fp
static file_t audio_fd_left = -1, audio_fd_right = -1;

static long left_channel_size = 0;
static uint8_t *compressed_buffer = NULL;
static uint32_t *frame_offsets = NULL;
static atomic_int frame_index = 0;
static float fps;
static int frame_type, video_width, video_height, content_width, content_height, sample_rate, num_frames, video_frame_size, audio_channels, max_compressed_size, audio_offset;
static atomic_int audio_bytes_fed = 0;
snd_stream_hnd_t stream;
static kthread_t *wthread;
static atomic_int audio_muted = 0;
float start_time;
static float frame_duration = 1.0f / 30.0f; // Default, overwritten after header read
static _Atomic double audio_start_time_ms = 0.0;
static atomic_int seek_request = -1;
static double frame_start_time = 0.0;
pvr_ptr_t pvr_txr;
pvr_poly_hdr_t hdr;
pvr_vertex_t vert[4];
char screenshotfilename[256];

#define VIDEO_START_FRAME 0
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))

#define NUM_BUFFERS 24
static uint8_t *frame_buffer[NUM_BUFFERS];
#define INVALID_FRAME -1
enum BufState {
    BUF_EMPTY = 0,
    BUF_LOADING = 1,
    BUF_READY = 2
};
#define RING_CAPACITY NUM_BUFFERS + 1

typedef struct {
    int frame;
    int generation;
} PreloadJob;

static PreloadJob preload_ring[RING_CAPACITY];

static int GSeekGeneration = 0;  // Incremented on seek to invalidate stale jobs
static atomic_int preload_ring_head = 0;
static atomic_int preload_ring_tail = 0;

#define PREFETCH_AHEAD 20  // How many frames ahead to keep loaded
#define INITIAL_PRELOAD 3  // Frames to preload after seek

_Atomic int buf_state[NUM_BUFFERS] = { BUF_EMPTY, BUF_EMPTY, BUF_EMPTY };

static volatile int audio_started = 0;
int soundbufferalloc = 4096;
static volatile float current_audio_frame = 0;

static inline double psTimer(void)
{
    // Clock off AICA
    //
    // according to purist, sh4 is 199.5MHz (KOS assumes 200 mhz)
    // and the sh4 has a different clock domain from AICA
    //
    // This solves the sound drift issue in the part 2 of the intro
    //
    // N.B. This depends on the jiffies per second from AICA
    //      and only works after AICA has been initialized
    #define AICA_MEM_CLOCK      0x021000    /* 4 bytes */
    uint32_t jiffies = g2_read_32(SPU_RAM_UNCACHED_BASE + AICA_MEM_CLOCK);
    return jiffies / 4.410f;
}

static int load_frame(int frame_num, int buf_index) {
    uint32_t offset = frame_offsets[frame_num];
    uint32_t next_offset = frame_offsets[frame_num + 1];
    uint32_t compressed_size = next_offset - offset;

    // fseek(fp, offset, SEEK_SET);
    // fread(compressed_buffer, 1, compressed_size, fp);
    fs_seek(video_fd, offset, SEEK_SET);
    fs_read(video_fd, compressed_buffer, compressed_size);
        
    int res = LZ4_decompress_fast(
        (const char *)compressed_buffer,
        (char *)frame_buffer[buf_index],
        video_frame_size);

    if (res < 0) {
        printf("❌ LZ4 decompression failed on frame %d\n", frame_num);
        return -1;
    }        

    return 0;
}

static size_t audio_cb(snd_stream_hnd_t hnd, uintptr_t l, uintptr_t r, size_t req) {
    // Correct atomic read
    if (atomic_load(&audio_muted) == 1) {
        printf("muted %d bytes\n", req);
        memset((void *)l, 0, req);
        if (audio_channels == 2)
            memset((void *)r, 0, req);
        return req;
    }
    // printf("audio_cb %d bytes\n", req);
    if (audio_channels == 2) {
        // size_t lbytes = fread((void *)l, 1, req/2, audio_fp_left);
        // size_t rbytes = fread((void *)r, 1, req/2, audio_fp_right);
        size_t lbytes = fs_read(audio_fd_left, (void *)l, req / 2);
        size_t rbytes = fs_read(audio_fd_right, (void *)r, req / 2);        
        atomic_fetch_add(&audio_bytes_fed, lbytes + rbytes);
        return lbytes+ rbytes;
    } else {
        // size_t bytes = fread((void *)l, 1, req, audio_fp_left);
        size_t bytes = fs_read(audio_fd_left, (void *)l, req);
        atomic_fetch_add(&audio_bytes_fed, bytes);
        if (bytes < req) {
            printf("Warning: Audio underflow, requested=%zu, provided=%zu\n", req, bytes);
        }
        return bytes;
    }
}

static int load_header(void) {
    char magic[4];
    // fread(magic, 1, 4, fp);
    fs_read(video_fd, magic, 4);
    if (memcmp(magic, DCMV_MAGIC, 4)) return -1;
    
    uint32_t version;
    // fread(&version, 4, 1, fp);
    fs_read(video_fd, &version, 4);
    
    if (version != 4) {
        printf("❌ Unsupported DCMV version: %d (expected 4)\n", version);
        return -1;
    }
    
    // fread(&frame_type, 1, 1, fp);
    // fread(&video_width, 2, 1, fp);
    // fread(&video_height, 2, 1, fp);
    // fread(&content_width, 2, 1, fp);    // New: content dimensions
    // fread(&content_height, 2, 1, fp);   // New: content dimensions
    // fread(&fps, sizeof(float), 1, fp); 
    // fread(&sample_rate, 2, 1, fp);
    // fread(&audio_channels, 2, 1, fp);
    // fread(&num_frames, 4, 1, fp);
    // fread(&video_frame_size, 4, 1, fp);
    // fread(&max_compressed_size, 4, 1, fp);
    // fread(&audio_offset, 4, 1, fp);
    fs_read(video_fd, &frame_type, 1);
    fs_read(video_fd, &video_width, 2);
    fs_read(video_fd, &video_height, 2);
    fs_read(video_fd, &content_width, 2);
    fs_read(video_fd, &content_height, 2);
    fs_read(video_fd, &fps, sizeof(float));
    fs_read(video_fd, &sample_rate, 2);
    fs_read(video_fd, &audio_channels, 2);
    fs_read(video_fd, &num_frames, 4);
    fs_read(video_fd, &video_frame_size, 4);
    fs_read(video_fd, &max_compressed_size, 4);
    fs_read(video_fd, &audio_offset, 4);    

    printf("📦 Header v%d: %s %dx%d (content: %dx%d) @ %.2ffps, %dHz, %dch %s, %d frames, frame_size=%d, max_compressed_size=%d, audio_offset=0x%X\n",
        version, frame_type == 1 ? "YUV422" : "RGB565", video_width, video_height, content_width, content_height, fps, sample_rate, audio_channels, (audio_channels == 2 ? "Stereo" : "Mono"), num_frames, video_frame_size, max_compressed_size, audio_offset);

    return 0;
}

// Helper function to check if a number is power of 2
static int is_power_of_2(int n) {
    return n > 0 && (n & (n - 1)) == 0;
}

static int init_pvr(int frame_type) {
    pvr_init_defaults();
    
    // Determine if we need strided mode (non-power-of-2 dimensions)
    int use_strided = !is_power_of_2(video_width) || !is_power_of_2(video_height);
    
    // Allocate PVR memory
    if (frame_type == 1) {
        // YUV422 - always uses 2 bytes per pixel
        pvr_txr = pvr_mem_malloc(video_width * video_height * 2);
    } else {
        // RGB565 VQ - use the exact frame size from header
        pvr_txr = pvr_mem_malloc(video_frame_size);
    }
    
    if (!pvr_txr) {
        printf("❌ Failed to allocate PVR memory!\n");
        return -1;
    }

    pvr_poly_cxt_t cxt;
    
    if (use_strided) {
        printf("📐 Using STRIDED texture mode for %s: %dx%d\n", 
               frame_type == 1 ? "YUV422" : "RGB565", video_width, video_height);
        
        int txr_format;
        if (frame_type == 1) {
            // YUV422 strided
            txr_format = PVR_TXRFMT_YUV422 | PVR_TXRFMT_VQ_ENABLE | PVR_TXRFMT_NONTWIDDLED | (1 << 25);
        } else {
            // RGB565 strided
            txr_format = PVR_TXRFMT_RGB565 | PVR_TXRFMT_VQ_ENABLE | PVR_TXRFMT_NONTWIDDLED | (1 << 25);
        }
        
        // Find next power of 2 dimensions for texture setup
        int pot_width = 1, pot_height = 1;
        while (pot_width < video_width) pot_width <<= 1;
        while (pot_height < video_height) pot_height <<= 1;
        
        pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY,
                         txr_format,
                         pot_width, pot_height, pvr_txr, PVR_FILTER_BILINEAR);
        pvr_poly_compile(&hdr, &cxt);
        
        // Set stride modulo for non-POT textures
        PVR_SET(PVR_TEXTURE_MODULO, (video_width / 32));
        
        // Set up vertices for strided rendering - map content area to full screen
        vert[0] = (pvr_vertex_t){.flags = PVR_CMD_VERTEX, .x=0, .y=0, .z=1, .u=0, .v=0, .argb=0xffffffff};
        vert[1] = (pvr_vertex_t){.flags = PVR_CMD_VERTEX, .x=640, .y=0, .z=1, .u=(float)content_width/pot_width, .v=0, .argb=0xffffffff};
        vert[2] = (pvr_vertex_t){.flags = PVR_CMD_VERTEX, .x=0, .y=480, .z=1, .u=0, .v=(float)content_height/pot_height, .argb=0xffffffff};
        vert[3] = (pvr_vertex_t){.flags = PVR_CMD_VERTEX_EOL, .x=640, .y=480, .z=1, .u=(float)content_width/pot_width, .v=(float)content_height/pot_height, .argb=0xffffffff};

        // Set up vertices for strided rendering - match non-strided scaling
        // float scaled_width = video_width * (640.0f / 512.0f);   // 320 * 1.25 = 400
        // float scaled_height = video_height * (480.0f / 256.0f); // 240 * 1.875 = 450
        
        // float center_x = (640.0f - scaled_width) / 2.0f;   // (640 - 400) / 2 = 120
        // float center_y = (480.0f - scaled_height) / 2.0f;  // (480 - 450) / 2 = 15
        
        // vert[0] = (pvr_vertex_t){.flags = PVR_CMD_VERTEX, .x=center_x, .y=center_y, .z=1, .u=0, .v=0, .argb=0xffffffff};
        // vert[1] = (pvr_vertex_t){.flags = PVR_CMD_VERTEX, .x=center_x+scaled_width, .y=center_y, .z=1, .u=(float)video_width/pot_width, .v=0, .argb=0xffffffff};
        // vert[2] = (pvr_vertex_t){.flags = PVR_CMD_VERTEX, .x=center_x, .y=center_y+scaled_height, .z=1, .u=0, .v=(float)video_height/pot_height, .argb=0xffffffff};
        // vert[3] = (pvr_vertex_t){.flags = PVR_CMD_VERTEX_EOL, .x=center_x+scaled_width, .y=center_y+scaled_height, .z=1, .u=(float)video_width/pot_width, .v=(float)video_height/pot_height, .argb=0xffffffff};    
        
    } else {
        printf("📐 Using POWER-OF-TWO texture mode: %dx%d\n", video_width, video_height);
        
        if (frame_type == 1) {
            // YUV422 power-of-2 (original method)
            pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY,
                PVR_TXRFMT_YUV422 | PVR_TXRFMT_TWIDDLED | PVR_TXRFMT_VQ_ENABLE,
                video_width, video_height, pvr_txr, PVR_FILTER_BILINEAR);
            pvr_poly_compile(&hdr, &cxt);
        } else {
            // RGB565 + VQ power-of-2 (original method)
            pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY,
                            PVR_TXRFMT_RGB565 | PVR_TXRFMT_TWIDDLED | PVR_TXRFMT_VQ_ENABLE,
                            video_width, video_height, pvr_txr, PVR_FILTER_BILINEAR);
            pvr_poly_compile(&hdr, &cxt);
        }
        
        // Standard full-screen vertices for power-of-2 textures
        vert[0] = (pvr_vertex_t){.flags = PVR_CMD_VERTEX, .x=0, .y=0, .z=1, .u=0, .v=0, .argb=0xffffffff};
        vert[1] = (pvr_vertex_t){.flags = PVR_CMD_VERTEX, .x=640, .y=0, .z=1, .u=1, .v=0, .argb=0xffffffff};
        vert[2] = (pvr_vertex_t){.flags = PVR_CMD_VERTEX, .x=0, .y=480, .z=1, .u=0, .v=1, .argb=0xffffffff};
        vert[3] = (pvr_vertex_t){.flags = PVR_CMD_VERTEX_EOL, .x=640, .y=480, .z=1, .u=1, .v=1, .argb=0xffffffff};
    }

    return 0;
}


void draw_frame(int buf_index, int frame_id) {
    // dcache_flush_range((uintptr_t)frame_buffer[buf_index],(uintptr_t)(frame_buffer[buf_index] + video_frame_size));
    pvr_txr_load(frame_buffer[buf_index], pvr_txr, video_frame_size);
    // ready[buf_index] = 0;
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_dr_state_t dr;
    pvr_dr_init(&dr);

    // PVR TA store queue destination address
    uintptr_t sq_dest_addr = (uintptr_t)SQ_MASK_DEST(PVR_TA_INPUT);
    
    // Submit polygon header
    sq_fast_cpy((void *)sq_dest_addr, &hdr, 1);
    // Submit 4 vertices
    sq_fast_cpy((void *)sq_dest_addr, vert, 4);

    pvr_dr_finish();
    pvr_list_finish();
    pvr_scene_finish();
}



bool schedule_frame_preload(int frame) {
    if (frame >= num_frames) return false;

    int buf = frame % NUM_BUFFERS;
    if (atomic_load(&buf_state[buf]) != BUF_EMPTY) return false;

    int head = atomic_load(&preload_ring_head);
    int tail = atomic_load(&preload_ring_tail);
    int next_head = (head + 1) % RING_CAPACITY;
    if (next_head == tail) return false;

    // Prevent duplicates
    for (int i = tail; i != head; i = (i + 1) % RING_CAPACITY) {
        if (preload_ring[i].frame == frame) return false;
    }

    preload_ring[head].frame = frame;
    preload_ring[head].generation = GSeekGeneration;
    atomic_store(&preload_ring_head, next_head);
    return true;
}

static void prefetch_frames(int current_frame) {
    for (int i = 1; i <= PREFETCH_AHEAD; i++) {
        int next = current_frame + i;
        if (next >= num_frames) break;
        schedule_frame_preload(next);
    }
}

void seek_to_frame(int new_frame) {
    if (new_frame < 0) new_frame = 0;
    if (new_frame >= num_frames) new_frame = num_frames - 1;

    // 1. Pause audio and clear buffers
    atomic_store(&audio_muted, 1);

    GSeekGeneration++;
    // Clear frame buffers to prevent stale data
    for (int i = 0; i < NUM_BUFFERS; i++) {
        atomic_store(&buf_state[i], BUF_EMPTY);
    }
    atomic_store(&preload_ring_head, 0);
    atomic_store(&preload_ring_tail, 0);

    // 2. Calculate audio positions (for split stereo layout)
    int samples_per_frame = (int)(sample_rate / fps);
    int bytes_per_frame_per_channel = samples_per_frame; // 4-bit ADPCM = 0.5 bytes/sample
    bytes_per_frame_per_channel = (bytes_per_frame_per_channel + 15) & ~0xF; // Align to 16 bytes

    // Left channel offset
    long left_offset = audio_offset + ((long)new_frame * bytes_per_frame_per_channel);
    if (left_offset > (audio_offset + left_channel_size))
        left_offset = audio_offset + left_channel_size; // Clamp to max left channel
    // fseek(audio_fp_left, left_offset, SEEK_SET);

    // if (audio_channels == 2){
    //     // Right channel offset
    //     long right_offset = audio_offset + left_channel_size + ((long)new_frame * bytes_per_frame_per_channel);
    //     if (right_offset > (audio_offset + left_channel_size * 2))
    //         right_offset = audio_offset + left_channel_size * 2; // Clamp to max right channel
    //     fseek(audio_fp_right, right_offset, SEEK_SET);
    //     printf("🔊 Seeked to frame %d | left=0x%lX right=0x%lX\n", new_frame, left_offset, right_offset);
    //  } else {
    //     printf("🔊 Seeked to frame %d | left=0x%lX (mono)\n", new_frame, left_offset);
    // }
    fs_seek(audio_fd_left, left_offset, SEEK_SET);

    if (audio_channels == 2) {
        // Right channel offset
        long right_offset = audio_offset + left_channel_size + ((long)new_frame * bytes_per_frame_per_channel);
        if (right_offset > (audio_offset + left_channel_size * 2))
            right_offset = audio_offset + left_channel_size * 2; // Clamp to max right channel
        fs_seek(audio_fd_right, right_offset, SEEK_SET);
        printf("🔊 Seeked to frame %d | left=0x%lX right=0x%lX\n", new_frame, left_offset, right_offset);
    } else {
        printf("🔊 Seeked to frame %d | left=0x%lX (mono)\n", new_frame, left_offset);
    }
    // 4. Update video state
    atomic_store(&frame_index, new_frame);
    double current_time = psTimer();
    frame_start_time = current_time;
    double new_audio_time = (double)(new_frame * samples_per_frame) * 1000.0 / sample_rate;
    atomic_store(&audio_start_time_ms, new_audio_time);
    current_audio_frame = new_frame;

    // 5. Preload video frames
    for (int i = 0; i < MIN(NUM_BUFFERS, 4) && (new_frame + i) < num_frames; i++) {
        schedule_frame_preload(new_frame + i);
    }

    // 6. Wait for initial buffers (optional)
    for (int i = 0; i < MIN(3, NUM_BUFFERS); i++) {
        int buf_id = (new_frame + i) % NUM_BUFFERS;
        for (int wait = 0; wait < 200 && !atomic_load(&buf_state[buf_id]); wait++) {
            thd_sleep(1);
        }
    }

    for (int i = 0; i < MIN(NUM_BUFFERS, PREFETCH_AHEAD) && (new_frame + i) < num_frames; i++) {
        schedule_frame_preload(new_frame + i);
    }    
    // 7. Resume playback
    atomic_store(&audio_muted, 0);
}



static void wait_exit(void) {
    static uint16_t prev_buttons = 0;
    maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    if (!dev) return;

    cont_state_t *state = (cont_state_t *)maple_dev_status(dev);
    if (!state || !dev->status_valid) return;

    if (state->buttons == prev_buttons) return;
    prev_buttons = state->buttons;

    int current_frame = atomic_load(&frame_index);
    
    if (state->buttons & CONT_DPAD_RIGHT) {
        atomic_store(&seek_request, current_frame + 500);
    } else if (state->buttons & CONT_DPAD_LEFT) {
        atomic_store(&seek_request, current_frame - 500);
    } else if (state->buttons & CONT_A) {
        sprintf(screenshotfilename, "/pc/screenshot%d.ppm", current_frame);
        vid_screen_shot(screenshotfilename);
    } else if (state->buttons) {
        arch_exit();
    }
}


void *worker_thread(void *p) {
    while (1) {
        snd_stream_poll(stream);  // Keep audio flowing

        int tail = atomic_load(&preload_ring_tail);
        int head = atomic_load(&preload_ring_head);

        if (tail != head) {  // There is work in the ring
            PreloadJob job = preload_ring[tail];

            // Skip stale jobs from previous seek
            if (job.generation != GSeekGeneration) {
                atomic_store(&preload_ring_tail, (tail + 1) % RING_CAPACITY);
                continue;
            }

            int frame = job.frame;
            int buf = frame % NUM_BUFFERS;

            // Avoid loading stale frames behind current playback
            int current_frame = atomic_load(&frame_index);
            if (frame < current_frame - NUM_BUFFERS) {
                printf("🗑️ Skipping stale frame %d (current=%d)\n", frame, current_frame);
                atomic_store(&preload_ring_tail, (tail + 1) % RING_CAPACITY);
                continue;
            }

            int expected = BUF_EMPTY;
            if (atomic_compare_exchange_strong(&buf_state[buf], &expected, BUF_LOADING)) {
                if (load_frame(frame, buf) == 0) {
                    atomic_store(&buf_state[buf], BUF_READY);
                } else {
                    printf("❌ Worker: Failed to load frame %d\n", frame);
                    atomic_store(&buf_state[buf], BUF_EMPTY);
                }
            } else {
                // Handle buffer conflicts
                int state = atomic_load(&buf_state[buf]);
                printf("🔧 Worker: buf %d in state %d (frame %d)\n", buf, state, frame);
            }

            atomic_store(&preload_ring_tail, (tail + 1) % RING_CAPACITY);
        }

        wait_exit();
        thd_pass();
    }
    return NULL;
}





    int main(int argc, char **argv) {
        // atomic_store(&frame_index, 31438); // outtakes for Dragon's Lair
        atomic_store(&frame_index,0);
        int current_frame = atomic_load(&frame_index);
        // profiler_init("/pc/gmon.out");
        // profiler_start();

        // dctx = ZSTD_createDCtx();
        // ZSTD_DCtx_setParameter(dctx, ZSTD_d_format, ZSTD_f_zstd1_magicless);
        // ZSTD_DCtx_setParameter(dctx, ZSTD_d_windowLogMax, 15);
        // ZSTD_DCtx_setParameter(dctx, ZSTD_d_forceIgnoreChecksum, 1);
        // ZSTD_DCtx_setParameter(dctx, ZSTD_d_refMultipleDDicts, ZSTD_rmd_refSingleDDict);
        // ZSTD_DCtx_setParameter(dctx, ZSTD_d_maxBlockSize, 65536);
        // ZSTD_DCtx_refDDict(dctx, NULL);
        // ZSTD_DCtx_reset(dctx, ZSTD_reset_session_only);    

        // fp = fopen(VIDEO_FILE, "rb");
        // if (!fp || load_header() < 0) return -1;
        video_fd = fs_open(VIDEO_FILE, O_RDONLY);
        if (video_fd < 0 || load_header() < 0) return -1;        
        frame_duration = 1.0f / (float)fps;

        // Read frame offsets
        frame_offsets = malloc((num_frames + 1) * sizeof(uint32_t));
        // fread(frame_offsets, sizeof(uint32_t), num_frames + 1, fp);
        fs_read(video_fd, frame_offsets, (num_frames + 1) * sizeof(uint32_t));

        // Allocate buffer for compressed frames
        compressed_buffer = memalign(32, max_compressed_size);
        if (!compressed_buffer) return -1;

        // long current_pos = ftell(fp);
        // fseek(fp, 0, SEEK_END);
        // long file_end = ftell(fp);

        // // Calculate total audio size
        // long total_audio_size = file_end - audio_offset;
        // if (audio_channels == 2) {
        //     left_channel_size = total_audio_size / 2;
        // } else {
        //     left_channel_size = total_audio_size;  // Mono: whole chunk
        // }
        // fseek(fp, current_pos, SEEK_SET);

        // Save current position
        off_t current_pos = fs_tell(video_fd);

        // Seek to end to determine file size
        fs_seek(video_fd, 0, SEEK_END);
        off_t file_end = fs_tell(video_fd);

        // Calculate total audio size
        off_t total_audio_size = file_end - audio_offset;
        if (audio_channels == 2) {
            left_channel_size = total_audio_size / 2;
        } else {
            left_channel_size = total_audio_size;  // Mono: whole chunk
        }

        // Restore file pointer
        fs_seek(video_fd, current_pos, SEEK_SET);        
        // int target_frame = (int)(current_time / frame_time) + frame_index;
        // Open the audio file and seek to the audio offset
        // audio_fp_left = fopen(VIDEO_FILE, "rb"); // Point to the same file as video
        // if (audio_channels == 2) {        
        //     audio_fp_right = fopen(VIDEO_FILE, "rb"); // Point to the same file as video
        // }
        audio_fd_left = fs_open(VIDEO_FILE, O_RDONLY);
        if (audio_channels == 2)
            audio_fd_right = fs_open(VIDEO_FILE, O_RDONLY);        
        // Allocate frame buffer
        for (int i = 0; i < NUM_BUFFERS; i++) {
            frame_buffer[i] = memalign(32, video_frame_size);
            if (!frame_buffer[i]) return -1;
        }

        // Initialize the PVR for rendering
        if (init_pvr(frame_type) < 0) return -1;

        // Initialize audio stream
        snd_stream_init_ex(audio_channels, soundbufferalloc);
        // double video_start_time = psTimer();
        // double audio_start_time = psTimer(); 

        // Timer accuracy test
        // double t1 = psTimer();
        // thd_sleep(1000);  // Sleep for 1 second (1000ms)
        // double t2 = psTimer();

        // printf("Timer test: Expected 1000ms, got %.0fms (drift: %+.0fms) | System uptime: %.0fms\n",
        //     t2 - t1,
        //     (t2 - t1) - 1000.0,  // Shows the timing drift
        //     video_start_time);
        stream = snd_stream_alloc(NULL, soundbufferalloc);
        snd_stream_set_callback_direct(stream, audio_cb);

        // Calculate initial audio position (same as seek function)
        // float frames_per_second = (float)fps;
        // float samples_per_second = (float)sample_rate;
        // int samples_per_frame = (int)(samples_per_second / frames_per_second);
        // int total_samples_to_skip = frame_index * samples_per_frame;

        // int bytes_to_skip = (total_samples_to_skip / 2);
        // bytes_to_skip = (bytes_to_skip + 15) & ~0xF;
        // bytes_to_skip += audio_offset;

        // fseek(audio_fp, bytes_to_skip, SEEK_SET);
        // int initial_audio_skip = bytes_to_skip - audio_offset;
        // printf("Starting at frame %d (audio byte offset: %d)\n", 
        //        frame_index, initial_audio_skip);
        
    // Calculate timing in milliseconds
        double frame_time_ms = 1000.0 / (double)fps; // ~43.48ms for 23fps
        printf("Frame timing: %ffps = %.3fms per frame\n", fps, frame_time_ms);
        atomic_store(&audio_muted, 1);
        snd_stream_start_adpcm(stream, sample_rate, audio_channels == 2 ? 1 : 0);
        // ✅ Start worker before pushing preload jobs
        wthread = thd_create(0, worker_thread, NULL);

        // 🔁 Let worker run at least a frame
        thd_sleep(30);  // Allow ~10ms for worker to dequeue from ring
        atomic_store(&seek_request,current_frame);
        // 🔁 Prime ring with preload jobs
        // for (int i = 0; i < NUM_BUFFERS; i++) {
        //     int preload_frame_id = current_frame + i;
        //     if (preload_frame_id >= num_frames) break;
        //     schedule_frame_preload(preload_frame_id);
        // }

        // // ✅ Wait until those buffers are ready
        // for (int i = 0; i < NUM_BUFFERS; i++) {
        //     int preload_frame_id = current_frame + i;
        //     if (preload_frame_id >= num_frames) break;

        //     int buf_id = preload_frame_id % NUM_BUFFERS;
        //     int wait = 0;
        //     while (atomic_load(&buf_state[buf_id]) != BUF_READY && wait++ < 100) {
        //         thd_sleep(1);
        //     }

        //     if (atomic_load(&buf_state[buf_id]) != BUF_READY) {
        //         printf("⚠️ Timeout waiting for preload of frame %d (buf %d)\n", preload_frame_id, buf_id);
        //     }
        // }

    printf("✅ All initial frames ready. Starting at frame %d\n", current_frame);
    printf("Starting at frame %d\n", current_frame);

    double accumulated_frame_debt = 0.0;
    int frames_dropped = 0;
    double max_frame_time = 0.0;
    double avg_frame_time = 0.0;
    double frame_time_samples = 0.0;
    int stall_count = 0; 

    // if (load_frame(atomic_load(&frame_index), 0) != 0) {
    //       printf("❌ Failed to load frame %d, skipping\n", atomic_load(&frame_index));
    // }

    while (atomic_load(&frame_index) < num_frames) {
        int requested_seek = atomic_exchange(&seek_request, -1);
        int current_frame = atomic_load(&frame_index);        
        double loop_timer_ms = psTimer();
        
        if (requested_seek != -1) {
            printf("Seeking to frame %d\n", requested_seek);
            seek_to_frame(requested_seek);
            
            // **CRITICAL FIX**: Reset state after seek
            current_frame = atomic_load(&frame_index);
            accumulated_frame_debt = 0.0;
            stall_count = 0;
            frame_start_time = psTimer();
            
            continue;
        }


        double current_audio_start_ms = atomic_load(&audio_start_time_ms);
        double current_audio_time_ms = current_audio_start_ms + (loop_timer_ms - frame_start_time);
        double expected_video_time = current_frame * frame_time_ms;

        // Calculate target time with debt compensation
        double target_time_ms = expected_video_time;    
        if (accumulated_frame_debt > 0.0) {
            target_time_ms += MIN(accumulated_frame_debt, frame_time_ms * 0.5);
        } else if (accumulated_frame_debt < 0.0) {
            target_time_ms += MAX(accumulated_frame_debt, -frame_time_ms * 0.5);
        }
        
        // Frame skipping logic (same as before)
        int frames_to_skip = 0;
        int temp_frame = current_frame;

        // while ((temp_frame < num_frames) &&
        //        ((temp_frame * frame_time_ms) < (current_audio_start_ms + psTimer() - frame_time_ms))) {
        while ((temp_frame < num_frames) &&
            ((temp_frame * frame_time_ms) < (current_audio_start_ms))) {    
            temp_frame++;
            frames_to_skip++;
            accumulated_frame_debt = 0.0;
        }
        
    if (frames_to_skip > 0) {
        printf("⚠️ Skipping %d frame(s): %d → %d (audio ahead by %.1fms)\n",
            frames_to_skip,
            current_frame,
            temp_frame,
            current_audio_time_ms - expected_video_time);

        for (int f = current_frame; f < temp_frame; f++) {
            printf("⏩ Would have drawn frame %d (buf %d)\n", f, f % NUM_BUFFERS);
        }

        atomic_fetch_add(&frame_index, frames_to_skip);
        frames_dropped += frames_to_skip;
        current_frame = temp_frame;
    }
        
    // static int last_drawn_frame = -1;
        
    double frame_render_start = psTimer();
    static int stall_count = 0;
    if (current_audio_time_ms >= target_time_ms) {
            int draw_frame_id = atomic_load(&frame_index);
            int buf_index = draw_frame_id % NUM_BUFFERS;
            
            if (atomic_load_explicit(&buf_state[buf_index], memory_order_acquire) == BUF_READY) {
                draw_frame(buf_index, draw_frame_id);
                atomic_store_explicit(&buf_state[buf_index], BUF_EMPTY, memory_order_release);

                stall_count = 0;
                atomic_fetch_add(&frame_index, 1);

                // New: Always prefetch ahead after a successful draw
                for (int i = 1; i < NUM_BUFFERS; i++) {
                    int next = draw_frame_id + i;
                    if (next >= num_frames) break;
                    schedule_frame_preload(next);
                }
            } else {
                stall_count++;
                if (stall_count > 5) {
                    printf("⚠️ Emergency advancing past stalled frame %d\n", draw_frame_id);
                    atomic_store(&buf_state[buf_index], BUF_EMPTY);
                    atomic_fetch_add(&frame_index, 1);
                    prefetch_frames(draw_frame_id);
                    stall_count = 0;
                }
            }
        }

        // Timing tracking...
        double frame_render_end = psTimer();
        double this_frame_time = frame_render_end - frame_render_start;

        if (this_frame_time > max_frame_time)
            max_frame_time = this_frame_time;

        avg_frame_time = (avg_frame_time * frame_time_samples + this_frame_time) / (frame_time_samples + 1);
        frame_time_samples++;

        // Step 6: Adjust sync debt
        double frame_overrun = this_frame_time - frame_time_ms;
        if (frame_overrun > 0.0) {
            accumulated_frame_debt -= frame_overrun;
        } else {
            accumulated_frame_debt += (-frame_overrun * 0.1);
        }
        accumulated_frame_debt *= 0.95;

        // Optional: debug log
        if (this_frame_time > frame_time_ms * 0.8) {
            printf("⚠️ Frame %d took %.1fms (%.1f%%), debt: %.2fms\n",
                current_frame, this_frame_time,
                (this_frame_time / frame_time_ms) * 100.0,
                accumulated_frame_debt);
        } else {
            // Waiting logic (same as before)
            double wait_ms = target_time_ms - current_audio_time_ms;
            
            if (accumulated_frame_debt < -10.0) {
                wait_ms = MAX(0.0, wait_ms + accumulated_frame_debt * 0.1);
            }
            
            if (wait_ms > 8.0) {
                int sleep_ms = (int)(wait_ms - 3.0);
                if (sleep_ms > 0) {
                    thd_sleep(sleep_ms);
                }
            } else if (wait_ms > 1.0) {
                thd_pass();
            }
        }
        // if (current_frame % 100 == 0) {
        // printf("Frame %d | Ready[0]=%d Ready[1]=%d | Audio=%.1fms Video=%.1fms\n", 
        //        current_frame, ready[0], ready[1], current_audio_time_ms, expected_video_time);
    // }
    }
    // printf("Final stats - Frames dropped: %d, Max frame time: %.1fms, Avg frame time: %.1fms\n",
    //        frames_dropped, max_frame_time, avg_frame_time);
    atomic_store(&audio_muted, 1);
    // profiler_stop();
    // profiler_clean_up();
    // Clean up
    thd_join(wthread, NULL);
    snd_stream_stop(stream);
    snd_stream_destroy(stream);
    // fclose(fp);
    // fclose(audio_fp_left);
    // if (audio_channels == 2) {
    //     fclose(audio_fp_right);
    // }
    fs_close(video_fd);
    fs_close(audio_fd_left);
    if (audio_channels == 2)
        fs_close(audio_fd_right);    
    // free(frame_buffer);
    free(compressed_buffer);
    free(frame_offsets);

    return 0;
}
