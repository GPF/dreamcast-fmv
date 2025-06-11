/**
 * fmv_play.c - Dreamcast FMV (Full Motion Video) Player
 * -----------------------------------------------------
 * This is the runtime video player for .dcmv containers, designed for the Sega Dreamcast.
 * It loads and decompresses LZ4-compressed VQ PVR textures on the fly and synchronizes
 * them to ADPCM audio streamed via the KOS sound API.
 *
 * Features:
 * - Parses custom DCMV v3 container format (video+audio in one file)
 * - Uses LZ4 decompression for each video frame (compressed with LZ4-HC)
 * - Leverages PVR DMA and VQ textures for efficient rendering
 * - Streams audio using snd_stream with optional stereo/mono handling
 * - Uses profiler integration for performance tuning
 *
 * Author: Troy Davis (GPF) — https://github.com/GPF
 * License: Public Domain / MIT-style — use freely with attribution.
 *
 * Controls:
 * - Press A: Take screenshot (/pc/screenshot#.ppm)
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
// #include <lz4/lz4.h>
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd/zstd.h>
static ZSTD_DCtx *dctx = NULL;
// #include "kosinski_lz4.h"
// #include "zstd_dreamcast.h"
// ZSTD_DC_Context ctx;


#define DCMV_MAGIC "DCMV"
#define VIDEO_FILE "/pc/movie.dcmv"

static FILE *fp = NULL, *audio_fp = NULL;
static uint8_t *compressed_buffer = NULL;
static uint32_t *frame_offsets = NULL;
static atomic_int frame_index =170 ;
static float fps;
static int frame_type, video_width, video_height, sample_rate, num_frames, video_frame_size, audio_channels, max_compressed_size, audio_offset;
static atomic_int audio_bytes_fed = 0;
snd_stream_hnd_t stream;
static kthread_t *wthread;
static atomic_int audio_muted = 0;
float start_time;
static float frame_duration = 1.0f / 30.0f; // Default, overwritten after header read
// static double audio_start_time = 0.0;
static _Atomic double audio_start_time_ms = 0.0;
// static float audio_time_offset = 0.0f;
static atomic_int seek_request = -1;
static double frame_start_time = 0.0;
// static int current_frame = 0;
pvr_ptr_t pvr_txr;
pvr_poly_hdr_t hdr;
pvr_vertex_t vert[4];
char screenshotfilename[256];

#define NUM_BUFFERS 2
static uint8_t *frame_buffer[NUM_BUFFERS];
static volatile int ready[NUM_BUFFERS] = {0, 0};
static volatile int audio_started = 0;
int soundbufferalloc = 8192;
static volatile float current_audio_frame = 0;

// static LZ4_DC_Stream lz4_ctx; 

double
psTimer(void)
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

// double psTimer_seconds(void) {
//     uint32_t jiffies = g2_read_32(SPU_RAM_UNCACHED_BASE + AICA_MEM_CLOCK);
//     return jiffies / 4410.0; // Convert to seconds (not milliseconds)
// }

static int load_frame(int frame_num, int buf_index) {
    uint32_t offset = frame_offsets[frame_num];
    uint32_t next_offset = frame_offsets[frame_num + 1];
    uint32_t compressed_size = next_offset - offset;

    fseek(fp, offset, SEEK_SET);
    fread(compressed_buffer, 1, compressed_size, fp);
        
    // double start = psTimer();

    // fread(frame_buffer, 1, compressed_size, fp);


    // int result = LZ4_DC_decompressHC_safest_fast(
    //     &lz4_ctx,
    //     compressed_buffer,
    //     frame_buffer,
    //     compressed_size,
    //     video_frame_size
    // );

    // LZ4_decompress_fast(
    //     (const char *)compressed_buffer,
    //     (char *)frame_buffer,
    //     video_frame_size);

    // ZSTD_inBuffer input = { compressed_buffer, compressed_size, 0 };
    // ZSTD_outBuffer output = { frame_buffer, video_frame_size, 0 };

    // ZSTD_DCtx_reset(dctx, ZSTD_reset_session_only);

    // while (input.pos < input.size && output.pos < output.size) {
    //     size_t res = ZSTD_decompressStream(dctx, &output, &input);
    //     if (ZSTD_isError(res)) {
    //         printf("❌ ZSTD stream failed: %s\n", ZSTD_getErrorName(res));
    //         return -1;
    //     }
    // }

    // ZSTD_DCtx_reset(dctx, ZSTD_reset_session_only);
    size_t decompressed = ZSTD_decompressDCtx(dctx, frame_buffer[buf_index], video_frame_size,
                                            compressed_buffer, compressed_size);
    // if (ZSTD_isError(decompressed)) {
    //     printf("❌ ZSTD decompress failed on frame %d: %s\n", frame_num, ZSTD_getErrorName(decompressed));
    //     return -1;

    // int result = ZSTD_DC_DecompressFrame(&ctx, 
    //                                     frame_buffer, video_frame_size,
    //                                     compressed_buffer, compressed_size);         
    // if (result <0) {
    //     printf("❌ ZSTDdc decompress failed on frame %d\n", frame_num);
    //     return -1;     
    // }                                                              
    
    // double end = psTimer();
    // double ms = (end - start);  // Already in milliseconds
    // printf("🧩 Frame %d decompressed from %ld to %d in %.2f ms:\n", frame_num, compressed_size, video_frame_size, ms);

    ready[buf_index] = 1;
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
    
    if (audio_channels == 2) {
        size_t lbytes = fread((void *)l, 1, req / 2, audio_fp);
        size_t rbytes = fread((void *)r, 1, req / 2, audio_fp);
        atomic_fetch_add(&audio_bytes_fed, lbytes + rbytes);
        return lbytes + rbytes;
    } else {
        size_t bytes = fread((void *)l, 1, req, audio_fp);
        atomic_fetch_add(&audio_bytes_fed, bytes);
        if (bytes < req) {
            printf("Warning: Audio underflow, requested=%zu, provided=%zu\n", req, bytes);
        }
        return bytes;
    }
}

static int load_header(void) {
    char magic[4];
    fread(magic, 1, 4, fp);
    if (memcmp(magic, DCMV_MAGIC, 4)) return -1;
    uint32_t version;
    fread(&version, 4, 1, fp);
    fread(&frame_type, 1, 1, fp);
    fread(&video_width, 2, 1, fp);
    fread(&video_height, 2, 1, fp);
    fread(&fps, sizeof(float), 1, fp); 
    fread(&sample_rate, 2, 1, fp);
    fread(&audio_channels, 2, 1, fp);
    fread(&num_frames, 4, 1, fp);
    fread(&video_frame_size, 4, 1, fp);
    fread(&max_compressed_size, 4, 1, fp);
    fread(&audio_offset, 4, 1, fp);

    printf("📦 Header: %s %dx%d @ %ffps, %dHz, %dch, %d frames, frame_size=%d, max_compressed_size=%d, audio_offset=0x%X\n",
           frame_type == 1 ? "YUV420P" : "RGB565", video_width, video_height, fps, sample_rate, audio_channels, num_frames, video_frame_size, max_compressed_size, audio_offset);

    return 0;
}

static int init_pvr(int frame_type) {
    // LZ4_DC_init(&lz4_ctx);
        pvr_init_defaults();
    if (frame_type == 1) {
        pvr_txr = pvr_mem_malloc(video_width * video_height * 2);
    } else {
        pvr_txr = pvr_mem_malloc(video_frame_size);
    }
    if (!pvr_txr) return -1;

    pvr_poly_cxt_t cxt;
    if (frame_type == 1) {
        // YUV422 texture setup
        PVR_SET(PVR_YUV_ADDR, ((unsigned int)pvr_txr) & 0xffffff);
        PVR_SET(PVR_YUV_CFG, (0x00 << 24) |
                             (((video_height / 16) - 1) << 8) |
                             ((video_width / 16) - 1));
        PVR_GET(PVR_YUV_CFG);

        pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY,
                         PVR_TXRFMT_YUV422 | PVR_TXRFMT_NONTWIDDLED,
                         video_width, video_height, pvr_txr, PVR_FILTER_BILINEAR);
        pvr_poly_compile(&hdr, &cxt);
        hdr.mode3 |= PVR_TXRFMT_STRIDE;
    } else {
        // RGB565 + VQ
        pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY,
                         PVR_TXRFMT_RGB565 | PVR_TXRFMT_TWIDDLED | PVR_TXRFMT_VQ_ENABLE,
                         video_width, video_height, pvr_txr, PVR_FILTER_BILINEAR);
        pvr_poly_compile(&hdr, &cxt);
    }

    // vert[0] = (pvr_vertex_t){.flags = PVR_CMD_VERTEX, .x=0, .y=0, .z=1, .u=0, .v=0, .argb=0xffffffff};
    // vert[1] = (pvr_vertex_t){.flags = PVR_CMD_VERTEX, .x=640, .y=0, .z=1, .u=1, .v=0, .argb=0xffffffff};
    // vert[2] = (pvr_vertex_t){.flags = PVR_CMD_VERTEX, .x=0, .y=480, .z=1, .u=0, .v=1, .argb=0xffffffff};
    // vert[3] = (pvr_vertex_t){.flags = PVR_CMD_VERTEX_EOL, .x=640, .y=480, .z=1, .u=1, .v=1, .argb=0xffffffff};

    vert[0] = (pvr_vertex_t){.flags = PVR_CMD_VERTEX, .x=80, .y=0, .z=1, .u=0.1875f, .v=0.03125f, .argb=0xffffffff};     // Top-left
    vert[1] = (pvr_vertex_t){.flags = PVR_CMD_VERTEX, .x=560, .y=0, .z=1, .u=0.8125f, .v=0.03125f, .argb=0xffffffff};    // Top-right
    vert[2] = (pvr_vertex_t){.flags = PVR_CMD_VERTEX, .x=80, .y=480, .z=1, .u=0.1875f, .v=0.96875f, .argb=0xffffffff};   // Bottom-left
    vert[3] = (pvr_vertex_t){.flags = PVR_CMD_VERTEX_EOL, .x=560, .y=480, .z=1, .u=0.8125f, .v=0.96875f, .argb=0xffffffff}; // Bottom-right    

    return 0;
}

void draw_frame(int buf_index) {
    if (!ready[buf_index]) return; 
    if (frame_type == 1) {
        // dcache_flush_range((uintptr_t)frame_buffer, (uintptr_t)(frame_buffer + video_frame_size));
        // pvr_dma_transfer(frame_buffer, PVR_TA_YUV_CONV, video_frame_size, PVR_DMA_YUV, true, NULL, NULL);
        // sq_cpy((void *)0x10800000, (void *)frame_buffer, video_frame_size);
        pvr_sq_load(NULL, frame_buffer[buf_index], video_frame_size, PVR_DMA_YUV);
        
    } else {
        // dcache_flush_range((uintptr_t)pvr_txr, (uintptr_t)(frame_buffer + video_frame_size));
        pvr_txr_load(frame_buffer[buf_index], pvr_txr, video_frame_size);
    }
    ready[buf_index] = 0;
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

void seek_to_frame(int new_frame) {
    if (new_frame < 0) new_frame = 0;
    if (new_frame >= num_frames) new_frame = num_frames - 1;

    int old_frame = atomic_load(&frame_index);
    double old_audio_time = atomic_load(&audio_start_time_ms);
    
    // Stop audio and close file
    fclose(audio_fp);
    atomic_store(&audio_muted, 1);

    // Clear frame buffers to prevent stale data
    // for (int i = 0; i < NUM_BUFFERS; i++) {
    //     ready[i] = 0;
    // }

    // Calculate new audio position
    int samples_per_frame = (int)(sample_rate / fps);
    int bytes_to_skip = ((new_frame * samples_per_frame) / 2 + 15) & ~0xF;
    bytes_to_skip += audio_offset;
    
    // Reopen audio file at new position
    audio_fp = fopen(VIDEO_FILE, "rb");
    fseek(audio_fp, bytes_to_skip, SEEK_SET);

    // Update timing variables
    double new_audio_time = (double)(new_frame * samples_per_frame) * 1000.0 / sample_rate;
    atomic_store(&audio_start_time_ms, new_audio_time);
    atomic_store(&frame_index, new_frame);
    current_audio_frame = new_frame;
    
    // CRITICAL: Reset frame timing
    // frame_start_time = psTimer();

    // FIX #1: Preload the current frame immediately after seek
    // int buf_index = new_frame % NUM_BUFFERS;
    // if (load_frame(new_frame, buf_index) == 0) {
    //     printf("✅ Preloaded frame %d after seek\n", new_frame);
    // } else {
    //     printf("❌ Failed to preload frame %d after seek\n", new_frame);
    // }

    printf("🔄 Seek: frame %d → %d | audio %.2fms → %.2fms | byte offset: %d\n",
           old_frame, new_frame, old_audio_time, new_audio_time,
           bytes_to_skip - audio_offset);

    // Restart audio
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
        // Correct atomic read
        if (atomic_load(&audio_muted) == 0) {
            snd_stream_poll(stream);
        }
        thd_sleep(20);
        wait_exit();
    }
    return NULL;
}

#define VIDEO_START_FRAME 0
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))



int main(int argc, char **argv) {
    atomic_store(&frame_index, 170);
    // ZSTD_DC_Init(&ctx);
    // profiler_init("/pc/gmon.out");
    // profiler_start();
    // dctx = ZSTD_createDCtx();
    // // Critical optimization parameters:
    // ZSTD_DCtx_setParameter(dctx, ZSTD_d_format, ZSTD_f_zstd1_magicless);
    // ZSTD_DCtx_setParameter(dctx, ZSTD_d_windowLogMax, 17);  // Must match compression
    // ZSTD_DCtx_setParameter(dctx, ZSTD_d_forceIgnoreChecksum, 1);  // Skip verification
    // ZSTD_DCtx_setParameter(dctx, ZSTD_d_refMultipleDDicts, ZSTD_rmd_refSingleDDict);
    // ZSTD_DCtx_refDDict(dctx, NULL);
    
    // // SH-4 specific tuning:
    // ZSTD_DCtx_setParameter(dctx, ZSTD_d_maxBlockSize, 65536);  // 64KB blocks

    dctx = ZSTD_createDCtx();
    ZSTD_DCtx_setParameter(dctx, ZSTD_d_format, ZSTD_f_zstd1_magicless);
    ZSTD_DCtx_setParameter(dctx, ZSTD_d_windowLogMax, 16);
    ZSTD_DCtx_setParameter(dctx, ZSTD_d_forceIgnoreChecksum, 1);
    ZSTD_DCtx_setParameter(dctx, ZSTD_d_refMultipleDDicts, ZSTD_rmd_refSingleDDict);
    ZSTD_DCtx_setParameter(dctx, ZSTD_d_maxBlockSize, 65536);
    ZSTD_DCtx_refDDict(dctx, NULL);
    ZSTD_DCtx_reset(dctx, ZSTD_reset_session_only);    

    fp = fopen(VIDEO_FILE, "rb");
    if (!fp || load_header() < 0) return -1;
    frame_duration = 1.0f / (float)fps;

    // Read frame offsets
    frame_offsets = malloc((num_frames + 1) * sizeof(uint32_t));
    fread(frame_offsets, sizeof(uint32_t), num_frames + 1, fp);

    // Allocate buffer for compressed frames
    compressed_buffer = memalign(32, max_compressed_size);
    if (!compressed_buffer) return -1;
    
    // int target_frame = (int)(current_time / frame_time) + frame_index;
    // Open the audio file and seek to the audio offset
    audio_fp = fopen(VIDEO_FILE, "rb"); // Point to the same file as video
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


    wthread = thd_create(0, worker_thread, NULL);
    seek_to_frame(frame_index);
    snd_stream_start_adpcm(stream, sample_rate, audio_channels == 2 ? 1 : 0);
    printf("Starting at frame %d\n", frame_index);


// Add these variables at the top
double accumulated_frame_debt = 0.0;  // Track cumulative timing debt
// double last_frame_end_time = 0.0;
int frames_dropped = 0;
// double frame_end_time = 0.0;
double max_frame_time = 0.0;
double avg_frame_time = 0.0;
double frame_time_samples = 0.0;

while (atomic_load(&frame_index) < num_frames) {
    int current_frame = atomic_load(&frame_index);
    int requested_seek = atomic_exchange(&seek_request, -1);
    
    if (requested_seek != -1) {
        seek_to_frame(requested_seek);
        current_frame = atomic_load(&frame_index);
        accumulated_frame_debt = 0.0;
        frame_start_time = psTimer();
        continue; // FIX #2a: Continue immediately after seek to start fresh
    }

    double current_audio_start_ms = atomic_load(&audio_start_time_ms);
    double current_audio_time_ms = current_audio_start_ms + (psTimer() - frame_start_time);
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
        atomic_fetch_add(&frame_index, frames_to_skip);
        frames_dropped += frames_to_skip;
        current_frame = temp_frame;
    }
    
    // FIX #3: Always ensure we have a frame loaded before trying to display
    if (current_audio_time_ms >= target_time_ms) {
        double frame_render_start = psTimer();
        
        int buf_index = current_frame % NUM_BUFFERS;

        // FIX #3a: Force load if not ready
        if (!ready[buf_index]) {
            if (load_frame(current_frame, buf_index) != 0) {
                printf("❌ Failed to load frame %d, skipping\n", current_frame);
                atomic_fetch_add(&frame_index, 1);
                continue;
            }
        }

        // FIX #3b: Always draw if we have a ready frame
        draw_frame(buf_index);
        
        // FIX #4: Preload next frame(s) for smoother playback
        int next_frame = current_frame + 1;
        if (next_frame < num_frames) {
            int next_buf_index = next_frame % NUM_BUFFERS;
            if (!ready[next_buf_index]) {
                // Load next frame in background (non-blocking)
                load_frame(next_frame, next_buf_index);
            }
        }
        
        atomic_fetch_add(&frame_index, 1);
        
        // Frame timing calculations (same as before)
        double frame_render_end = psTimer();
        double this_frame_time = frame_render_end - frame_render_start;
        
        if (this_frame_time > max_frame_time) {
            max_frame_time = this_frame_time;
        }
        avg_frame_time = (avg_frame_time * frame_time_samples + this_frame_time) / (frame_time_samples + 1);
        frame_time_samples++;
        
        double frame_overrun = this_frame_time - frame_time_ms;
        if (frame_overrun > 0.0) {
            accumulated_frame_debt -= frame_overrun;
        } else {
            accumulated_frame_debt += (-frame_overrun * 0.1);
        }
        
        accumulated_frame_debt *= 0.95;
        
        if (this_frame_time > frame_time_ms * 0.8) {
            printf("WARNING: Frame %d took %.1fms (%.1f%% of budget), debt now: %.1fms\n", 
                   current_frame, this_frame_time, (this_frame_time / frame_time_ms) * 100.0, accumulated_frame_debt);
        }
        
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

    // profiler_stop();
    // profiler_clean_up();
    // Clean up
    thd_join(wthread, NULL);
    snd_stream_stop(stream);
    snd_stream_destroy(stream);
    fclose(fp);
    fclose(audio_fp);
    // free(frame_buffer);
    free(compressed_buffer);
    free(frame_offsets);

    return 0;
}
