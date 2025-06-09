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
#include <lz4/lz4.h>
// #include "kosinski_lz4.h"
// #include "profiler.h"


#define DCMV_MAGIC "DCMV"
#define VIDEO_FILE "/pc/movie.dcmv"

static FILE *fp = NULL, *audio_fp = NULL;
static uint8_t *compressed_buffer = NULL;
static uint32_t *frame_offsets = NULL;
static int frame_index =18282 ;
static int frame_type, video_width, video_height, fps, sample_rate, num_frames, video_frame_size, audio_channels, max_compressed_size, audio_offset;
static int audio_bytes_fed = 0;
snd_stream_hnd_t stream;
static kthread_t *audio_thread;
pvr_ptr_t pvr_txr;
pvr_poly_hdr_t hdr;
pvr_vertex_t vert[4];
char screenshotfilename[256];

static uint8_t *frame_buffer;
static volatile int ready_buffer = -1;
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

// int load_frame(int frame_num) {
//     uint32_t offset = frame_offsets[frame_num];
//     uint32_t next_offset = frame_offsets[frame_num + 1];
//     uint32_t compressed_size = next_offset - offset;
// // printf("FRAME %d: comp_size=%u, decomp_size=%u\n",
// //        frame_num, compressed_size, video_frame_size);
//     // Validate sizes
//     if (compressed_size > max_compressed_size || compressed_size < 4) {
//         printf("Invalid compressed size: %lu\n", compressed_size);
//         return -1;
//     }

//     fseek(fp, offset, SEEK_SET);
//     size_t read = fread(compressed_buffer, 1, compressed_size, fp);
//     if (read != compressed_size) {
//         printf("Read failed: %zu/%lu\n", read, compressed_size);
//         return -1;
//     }
// // printf("BUFFER RANGES: src=%p-%p, dst=%p-%p\n",
// //        compressed_buffer, compressed_buffer + compressed_size,
// //        frame_buffer, frame_buffer + video_frame_size);
//     // 3. Decompress
//     int result = LZ4_DC_decompressHC_safest_fast(
//         &lz4_ctx,
//         compressed_buffer,
//         frame_buffer,
//         compressed_size,
//         video_frame_size
//     );

//     if (result != video_frame_size) {
//         printf("Decompression failed @ frame %d\n", frame_num);
//         return -1;
//     }

//     if (result != video_frame_size) {
//         printf("Decompression failed: %d/%d\n", result, video_frame_size);
        
//         // Debug: Print first 8 bytes
//         printf("Input header:");
//         for (int i = 0; i < 8 && i < compressed_size; i++) {
//             printf(" %02X", compressed_buffer[i]);
//         }
//         printf("\n");
        
//         return -1;
//     }

//     return 0;
// }

static int load_frame(int frame_num) {
    uint32_t offset = frame_offsets[frame_num];
    uint32_t next_offset = frame_offsets[frame_num + 1];
    uint32_t compressed_size = next_offset - offset;

    fseek(fp, offset, SEEK_SET);
    fread(compressed_buffer, 1, compressed_size, fp);
    printf("Frame %d , compressed = %ld\n", frame_num,compressed_size );
    // fread(frame_buffer, 1, compressed_size, fp);
    LZ4_decompress_fast(
        (const char *)compressed_buffer,
        (char *)frame_buffer,
        video_frame_size);

    return 0;
}


static size_t audio_cb(snd_stream_hnd_t hnd, uintptr_t l, uintptr_t r, size_t req) {
    if (audio_channels == 2) {
        size_t lbytes = fread((void *)l, 1, req / 2, audio_fp);
        size_t rbytes = fread((void *)r, 1, req / 2, audio_fp);
        audio_bytes_fed += lbytes + rbytes;
        return lbytes + rbytes;
    } else {
        size_t bytes = fread((void *)l, 1, req, audio_fp);
        audio_bytes_fed += bytes;
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
    fread(&fps, 2, 1, fp);
    fread(&sample_rate, 2, 1, fp);
    fread(&audio_channels, 2, 1, fp);
    fread(&num_frames, 4, 1, fp);
    fread(&video_frame_size, 4, 1, fp);
    fread(&max_compressed_size, 4, 1, fp);
    fread(&audio_offset, 4, 1, fp);

    printf("📦 Header: %s %dx%d @ %dfps, %dHz, %dch, %d frames, frame_size=%d, max_compressed_size=%d, audio_offset=0x%X\n",
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

    vert[0] = (pvr_vertex_t){.flags = PVR_CMD_VERTEX, .x=0, .y=0, .z=1, .u=0, .v=0, .argb=0xffffffff};
    vert[1] = (pvr_vertex_t){.flags = PVR_CMD_VERTEX, .x=640, .y=0, .z=1, .u=1, .v=0, .argb=0xffffffff};
    vert[2] = (pvr_vertex_t){.flags = PVR_CMD_VERTEX, .x=0, .y=480, .z=1, .u=0, .v=1, .argb=0xffffffff};
    vert[3] = (pvr_vertex_t){.flags = PVR_CMD_VERTEX_EOL, .x=640, .y=480, .z=1, .u=1, .v=1, .argb=0xffffffff};
    return 0;
}

void draw_frame() {
    if (frame_type == 1) {
        // dcache_flush_range((uintptr_t)frame_buffer, (uintptr_t)(frame_buffer + video_frame_size));
        // pvr_dma_transfer(frame_buffer, PVR_TA_YUV_CONV, video_frame_size, PVR_DMA_YUV, true, NULL, NULL);
        // sq_cpy((void *)0x10800000, (void *)frame_buffer, video_frame_size);
        pvr_sq_load(NULL, frame_buffer, video_frame_size, PVR_DMA_YUV);
        
    } else {
        // dcache_flush_range((uintptr_t)pvr_txr, (uintptr_t)(frame_buffer + video_frame_size));
        pvr_txr_load(frame_buffer, pvr_txr, video_frame_size);
    }

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

static void wait_exit(void) {
    static uint16_t prev_buttons = 0;

    maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    if (!dev) return;

    cont_state_t *state = (cont_state_t *)maple_dev_status(dev);
    if (!state || !dev->status_valid) return;

    // Avoid repeated work if button state hasn't changed
    if (state->buttons == prev_buttons) return;
    prev_buttons = state->buttons;

    if (state->buttons & CONT_A) {
        sprintf(screenshotfilename, "/pc/screenshot%d.ppm", frame_index);
        vid_screen_shot(screenshotfilename);
    } else if (state->buttons) {
        arch_exit();  // Graceful exit
    }
}


void *audio_poll_thread(void *p) {
    while (1) {
        // printf("snd_stream_poll\n");
        snd_stream_poll(stream);
        thd_sleep(20);  
        wait_exit();

    }
    return NULL;
}
float start_time;
int main(int argc, char **argv) {
    // profiler_init("/pc/gmon.out");
    // profiler_start();
    fp = fopen(VIDEO_FILE, "rb");
    if (!fp || load_header() < 0) return -1;

    // Read frame offsets
    frame_offsets = malloc((num_frames + 1) * sizeof(uint32_t));
    fread(frame_offsets, sizeof(uint32_t), num_frames + 1, fp);

    // Allocate buffer for compressed frames
    compressed_buffer = memalign(32, max_compressed_size);
    if (!compressed_buffer) return -1;
    
    // int target_frame = (int)(current_time / frame_time) + frame_index;
    // Open the audio file and seek to the audio offset
    audio_fp = fopen(VIDEO_FILE, "rb"); // Point to the same file as video
    // int samples_per_frame = sample_rate / fps;
    // int total_samples_to_skip = frame_index * samples_per_frame;

    // // ADPCM is 4 bits per sample, so bytes = samples / 2
    // // Plus we need block alignment (16 bytes per ADPCM block)
    // int bytes_to_skip = (total_samples_to_skip / 2) & ~0xF;  // Align to 16-byte blocks
    // bytes_to_skip += audio_offset;

    // // Seek to the calculated position
    // fseek(audio_fp, bytes_to_skip, SEEK_SET);
    // Allocate frame buffer
    frame_buffer = memalign(32, video_frame_size);
    if (!frame_buffer) return -1;

    // Initialize the PVR for rendering
    if (init_pvr(frame_type) < 0) return -1;

    // Initialize audio stream
    // Initialize audio stream
    // snd_stream_init();
    snd_stream_init_ex(audio_channels, soundbufferalloc);
    stream = snd_stream_alloc(NULL, soundbufferalloc);
    snd_stream_set_callback_direct(stream, audio_cb);
    
    // Calculate exact audio position for frame 140
    float frames_per_second = (float)fps;
    float samples_per_second = (float)sample_rate;
    int samples_per_frame = (int)(samples_per_second / frames_per_second);
    int total_samples_to_skip = frame_index * samples_per_frame;
    
    // ADPCM is 4 bits per sample, so bytes = samples / 2
    // Plus we need block alignment (16 bytes per ADPCM block)
    int bytes_to_skip = (total_samples_to_skip / 2);
    bytes_to_skip = (bytes_to_skip + 15) & ~0xF;  // Round up to nearest 16-byte boundary
    bytes_to_skip += audio_offset;
    
    printf("Seeking to audio position: 0x%X bytes (frame %d)\n", bytes_to_skip, frame_index);
    
    // Seek audio and reset stream
    fseek(audio_fp, bytes_to_skip, SEEK_SET);
    int initial_audio_skip = bytes_to_skip - audio_offset;
// audio_bytes_fed = 0;
        snd_stream_start_adpcm(stream, sample_rate, audio_channels == 2 ? 1 : 0);
    // Create audio polling thread
    audio_thread = thd_create(0, audio_poll_thread, NULL);

    int initial_frame_index=frame_index;
    float audio_time_offset = (float)(initial_audio_skip * 2) / (float)sample_rate;
    #define FRAME_DURATION (1.0f / frames_per_second)
    #define VIDEO_START_FRAME 0
    // // Load and display FIRST FRAME immediately
    // if (load_frame(frame_index)) {
    //     printf("Failed to load initial frame %d\n", frame_index);
    //     return -1;
    // }
    // draw_frame();
    // Set up timing - account for the fact we're starting at frame 140
    start_time = psTimer() - ((float)(frame_index) * FRAME_DURATION);
    float video_time_offset = (float)frame_index * FRAME_DURATION;
    frame_index++; // Next frame to process

#define MIN(a,b) ((a) < (b) ? (a) : (b))
// Main rendering loop
#define FRAME_DURATION (1.0f / frames_per_second)

while (frame_index < num_frames) {
    float now = psTimer() - start_time;

    float audio_time = audio_time_offset + ((float)audio_bytes_fed * 2 / (float)(sample_rate * audio_channels));


    float effective_time = MIN(now, audio_time);
    float expected_time = (float)(frame_index - VIDEO_START_FRAME) * FRAME_DURATION;
    float drift = effective_time - expected_time;

    if (frame_index %100 == 0) {
        printf("Frame %d (logical frame %d) | ⏱ now=%.3f 🎧 audio=%.3f 🎞 drift=%.3f\n",
       frame_index, frame_index - VIDEO_START_FRAME, now, audio_time, drift);
    }

if (effective_time >= expected_time) {
    if (load_frame(frame_index)) break;
    draw_frame();
    frame_index++;

    float drift = effective_time - expected_time;
    int sleep_ms = (int)((FRAME_DURATION - drift) * 1000);
    if (sleep_ms > 0 && sleep_ms < 100) {
        thd_sleep(sleep_ms);  // smooth adjustment
        // printf("video sleep\n");
    } else {
        thd_pass();  // avoid busy wait
        // printf("video pass\n");
    }
} else {
    int sleep_ms = (int)((expected_time - effective_time) * 1000);
    if (sleep_ms > 0) thd_sleep(sleep_ms);
    else thd_sleep(1);
}

        wait_exit();
    }

    // profiler_stop();
    // profiler_clean_up();
    // Clean up
    thd_join(audio_thread, NULL);
    snd_stream_stop(stream);
    snd_stream_destroy(stream);
    fclose(fp);
    fclose(audio_fp);
    free(frame_buffer);
    free(compressed_buffer);
    free(frame_offsets);

    return 0;
}
