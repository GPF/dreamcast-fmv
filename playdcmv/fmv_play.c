#include <kos.h>
#include <dc/sound/stream.h>
#include <dc/sound/sound.h>
#include <dc/pvr.h>
#include <dc/maple/controller.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lz4/lz4.h>

#define DCMV_MAGIC "DCMV"
#define VIDEO_FILE "/cd/movie.dcmv"

static FILE *fp = NULL, *audio_fp = NULL;
static uint8_t *compressed_buffer = NULL;
static uint32_t *frame_offsets = NULL;
static int frame_index = 0;
static int frame_type, video_width, video_height, fps, sample_rate, num_frames, video_frame_size, audio_channels, max_compressed_size, audio_offset;
static int audio_bytes_fed = 0;
snd_stream_hnd_t stream;
pvr_ptr_t pvr_txr;
pvr_poly_hdr_t hdr;
pvr_vertex_t vert[4];
char screenshotfilename[256];

static uint8_t *frame_buffer;
static volatile int ready_buffer = -1;
static int preload_frame = 0;
static volatile int preload_done = 0;
static volatile int audio_started = 0;
static uint16_t spos = 0;
static uint32_t audio_played_bytes = 0;
int soundbufferalloc = 4096;
static volatile float current_audio_frame = 0;

static int load_frame(int frame_num) {
    uint32_t offset = frame_offsets[frame_num];
    uint32_t next_offset = frame_offsets[frame_num + 1];
    uint32_t compressed_size = next_offset - offset;

    fseek(fp, offset, SEEK_SET);
    fread(compressed_buffer, 1, compressed_size, fp);
    
    int decompressed = LZ4_decompress_fast(
        (const char *)compressed_buffer,
        (char *)frame_buffer,
        video_frame_size);

    pvr_txr_load_dma(frame_buffer, pvr_txr, video_frame_size, true, NULL, NULL);
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
        pvr_dma_transfer(frame_buffer, PVR_TA_YUV_CONV, video_frame_size, PVR_DMA_YUV, true, NULL, NULL);
    } else {
        pvr_txr_load(frame_buffer, pvr_txr, video_frame_size);
    }

    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_dr_state_t dr;
    pvr_dr_init(&dr);
    pvr_poly_hdr_t *hdr_ptr = (pvr_poly_hdr_t *)pvr_dr_target(dr);
    sq_cpy(hdr_ptr, &hdr, sizeof(pvr_poly_hdr_t));
    pvr_dr_commit(hdr_ptr);
    for (int i = 0; i < 4; ++i) {
        pvr_vertex_t *v = (pvr_vertex_t *)pvr_dr_target(dr);
        *v = vert[i];
        pvr_dr_commit(v);
    }
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
        snd_stream_poll(stream);
        thd_sleep(10);  
        wait_exit();

    }
    return NULL;
}

int main(int argc, char **argv) {
    fp = fopen(VIDEO_FILE, "rb");
    if (!fp || load_header() < 0) return -1;

    // Read frame offsets
    frame_offsets = malloc((num_frames + 1) * sizeof(uint32_t));
    fread(frame_offsets, sizeof(uint32_t), num_frames + 1, fp);

    // Allocate buffer for compressed frames
    compressed_buffer = malloc(max_compressed_size);
    if (!compressed_buffer) return -1;

    // Open the audio file and seek to the audio offset
    audio_fp = fopen(VIDEO_FILE, "rb"); // Point to the same file as video
    fseek(audio_fp, audio_offset, SEEK_SET);  // Seek to the correct audio offset

    // Allocate frame buffer
    frame_buffer = memalign(32, video_frame_size);
    if (!frame_buffer) return -1;

    // Initialize the PVR for rendering
    if (init_pvr(frame_type) < 0) return -1;

    // Initialize audio stream
    snd_stream_init();
    stream = snd_stream_alloc(NULL, soundbufferalloc);
    snd_stream_set_callback_direct(stream, audio_cb);
    snd_stream_start_adpcm(stream, sample_rate, audio_channels == 2 ? 1 : 0);

    // Create audio polling thread
    thd_create(1, audio_poll_thread, NULL);
    // Precompute bytes_per_frame as float
    float bytes_per_sample = (float)audio_channels / 2.0f;
    float inv_bytes_per_frame = (fps / (float)sample_rate) / bytes_per_sample; 
    // Frame rendering loop
    while (frame_index < num_frames) {
        int should_be_frame = (int)((float)audio_bytes_fed * inv_bytes_per_frame);
        int target_frame = should_be_frame < num_frames ? should_be_frame : num_frames;
        while (frame_index < target_frame) {
            if (load_frame(frame_index) != 0) break;
            draw_frame();
            ++frame_index;
        }
    }

    // Clean up
    snd_stream_stop(stream);
    snd_stream_destroy(stream);
    fclose(fp);
    fclose(audio_fp);
    free(frame_buffer);
    free(compressed_buffer);
    free(frame_offsets);

    return 0;
}
