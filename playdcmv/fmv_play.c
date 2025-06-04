#include <kos.h>
#include <dc/sound/stream.h>
#include <dc/sound/sound.h>
#include <dc/pvr.h>
#include <dc/maple/controller.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd/zstd.h>
#include "profiler.h"

#define DCMV_MAGIC "DCMV"
#define VIDEO_FILE "/pc/movie.dcmv"

static FILE *fp = NULL, *audio_fp = NULL;
static uint8_t *frame_buffer = NULL, *compressed_buffer = NULL;
static size_t compressed_buffer_size = 0;
static uint32_t *frame_offsets = NULL;
uint32_t max_compressed_size = 0;
static int frame_index = 0;
static int video_width, video_height, fps, sample_rate, num_frames, video_frame_size, audio_channels;
static int audio_bytes_fed = 0;
snd_stream_hnd_t stream;
pvr_ptr_t pvr_txr;
pvr_poly_hdr_t hdr;
pvr_vertex_t vert[4];
char screenshotfilename[256];
static ZSTD_DCtx *dctx = NULL;

static size_t audio_cb(snd_stream_hnd_t hnd, uintptr_t l, uintptr_t r, size_t req) {
    if (audio_channels == 2) {
        size_t lbytes = fread((void *)l, 1, req / 2, audio_fp);
        size_t rbytes = fread((void *)r, 1, req / 2, audio_fp);
        audio_bytes_fed += lbytes + rbytes;
        return lbytes + rbytes;
    } else {
        size_t bytes = fread((void *)l, 1, req, audio_fp);
        audio_bytes_fed += bytes;
        return bytes;
    }
}

static int load_header(void) {
    char magic[4];
    fread(magic, 1, 4, fp);
    if (memcmp(magic, DCMV_MAGIC, 4)) return -1;

    uint32_t version;
    fread(&version, 4, 1, fp);
    fread(&video_width, 2, 1, fp);
    fread(&video_height, 2, 1, fp);
    fread(&fps, 2, 1, fp);
    fread(&sample_rate, 2, 1, fp);
    fread(&audio_channels, 2, 1, fp);
    fread(&num_frames, 4, 1, fp);
    fread(&video_frame_size, 4, 1, fp);
    fread(&max_compressed_size, 4, 1, fp); 

    printf("📦 Header: %dx%d @ %dfps, %dHz, %dch, %d frames, frame_size=%d, max_compressed_size=%ld\n",
           video_width, video_height, fps, sample_rate, audio_channels, num_frames, video_frame_size,max_compressed_size);

    return 0;
}

static int load_frame(int frame_num) {
    uint32_t offset = frame_offsets[frame_num];
    fseek(fp, offset, SEEK_SET);

    uint32_t compressed_size;
    if (fread(&compressed_size, 1, 4, fp) != 4) {
        printf("❌ Failed to read compressed size\n");
        return -1;
    }

    if (compressed_size > compressed_buffer_size) {
        free(compressed_buffer);
        compressed_buffer = malloc(compressed_size);
        if (!compressed_buffer) {
            printf("❌ Failed to allocate compressed buffer\n");
            return -1;
        }
        compressed_buffer_size = compressed_size;
    }

    if (fread(compressed_buffer, 1, compressed_size, fp) != compressed_size) {
        printf("❌ Failed to read compressed frame data (%lu bytes)\n", compressed_size);
        return -1;
    }

    size_t decompressed = ZSTD_decompressDCtx(dctx, frame_buffer, video_frame_size,
                                               compressed_buffer, compressed_size);
    if (ZSTD_isError(decompressed)) {
        printf("❌ ZSTD decompress failed on frame %d: %s\n", frame_num, ZSTD_getErrorName(decompressed));
        return -1;
    }
    if (decompressed != video_frame_size) {
        printf("❌ Unexpected decompressed size: got %u, expected=%d\n", decompressed, video_frame_size);
        return -1;
    }

    pvr_txr_load_dma(frame_buffer, pvr_txr, video_frame_size, true, NULL, NULL);
    return 0;
}


static int init_pvr(void) {
    pvr_init_defaults();
    pvr_txr = pvr_mem_malloc(video_frame_size);
    if (!pvr_txr) return -1;

    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY,
                     PVR_TXRFMT_RGB565 | PVR_TXRFMT_TWIDDLED | PVR_TXRFMT_VQ_ENABLE,
                     video_width, video_height, pvr_txr, PVR_FILTER_BILINEAR);
    pvr_poly_compile(&hdr, &cxt);

    // /* Set SQ to YUV converter. */
    // PVR_SET(PVR_YUV_ADDR, (((unsigned int)pvr_txr) & 0xffffff));
    // /* Divide PVR texture width and texture height by 16 and subtract 1. */
    // PVR_SET(PVR_YUV_CFG, (0x01 << 24) | /* Set bit to specify 422 data format */
    //                      (((video_height / 16) - 1) << 8) | 
    //                      ((video_width / 16) - 1));
    // /* Need to read once. */
    // PVR_GET(PVR_YUV_CFG);

    // pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, 
    //                 PVR_TXRFMT_YUV422 | PVR_TXRFMT_NONTWIDDLED, 
    //                 video_width, video_height, 
    //                 pvr_txr, 
    //                 PVR_FILTER_BILINEAR);
    // pvr_poly_compile(&hdr, &cxt);

    // hdr.mode3 |= PVR_TXRFMT_STRIDE;    

    vert[0] = (pvr_vertex_t){.flags = PVR_CMD_VERTEX, .x=0, .y=0, .z=1, .u=0, .v=0, .argb=0xffffffff};
    vert[1] = (pvr_vertex_t){.flags = PVR_CMD_VERTEX, .x=640, .y=0, .z=1, .u=1, .v=0, .argb=0xffffffff};
    vert[2] = (pvr_vertex_t){.flags = PVR_CMD_VERTEX, .x=0, .y=480, .z=1, .u=0, .v=1, .argb=0xffffffff};
    vert[3] = (pvr_vertex_t){.flags = PVR_CMD_VERTEX_EOL, .x=640, .y=480, .z=1, .u=1, .v=1, .argb=0xffffffff};
    return 0;
}

static void draw_frame(void) {
    pvr_wait_ready();
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_prim(&hdr, sizeof(hdr));
    for (int i = 0; i < 4; ++i)
        pvr_prim(&vert[i], sizeof(pvr_vertex_t));
    pvr_list_finish();
    pvr_scene_finish();
}

static void wait_exit(void) {
    maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    if (dev) {
        cont_state_t *state = (cont_state_t *)maple_dev_status(dev);
        if (state) {
            if (state->buttons & CONT_A) {
                sprintf(screenshotfilename, "/pc/screenshot%d.ppm", frame_index);
                vid_screen_shot(screenshotfilename);
            } else if (state->buttons){
                            profiler_stop();
                profiler_clean_up();
                arch_exit();}
        }
    }
}

int main(void) {
    profiler_init("/pc/gmon.out");
    profiler_start();
    dctx = ZSTD_createDCtx();
    ZSTD_DCtx_setParameter(dctx, ZSTD_d_format, ZSTD_f_zstd1_magicless);
    ZSTD_DCtx_setParameter(dctx, ZSTD_d_forceIgnoreChecksum, 0);    
    fp = fopen(VIDEO_FILE, "rb");
    if (!fp || load_header() < 0) return -1;

    fseek(fp, 0x1A + 4 + num_frames * sizeof(uint32_t), SEEK_SET);  // 0x1A = 26 (header end), 4 = max_compressed_size

    // Seek past the size table to the offset table
    fseek(fp, 0x1A + 4 + num_frames * sizeof(uint32_t), SEEK_SET);  // 0x1A is 26 bytes header, 4 bytes max_compressed_size

    frame_offsets = malloc(num_frames * sizeof(uint32_t));
    if (!frame_offsets) {
        printf("❌ Failed to allocate frame_offsets\n");
        return -1;
    }
    fread(frame_offsets, sizeof(uint32_t), num_frames, fp);

    uint32_t last_offset = 0;
    uint32_t last_size = 0;
    size_t audio_offset = 0;

    last_offset = frame_offsets[num_frames - 1];
    fseek(fp, last_offset, SEEK_SET);
    fread(&last_size, 4, 1, fp);
    audio_offset = last_offset + 4 + last_size;
    printf("🔊 Calculated audio offset: 0x%zX\n", audio_offset);

    compressed_buffer = malloc(max_compressed_size);
    if (!compressed_buffer) {
        printf("❌ Failed to allocate compressed buffer\n");
        return -1;
    }
    compressed_buffer_size = max_compressed_size;
        
    audio_fp = fopen(VIDEO_FILE, "rb");
    fseek(audio_fp, audio_offset, SEEK_SET);

    frame_buffer = memalign(32, video_frame_size);
    if (!frame_buffer) return -1;

    if (init_pvr() < 0) return -1;

    snd_stream_init();
    stream = snd_stream_alloc(NULL, 4096);
    snd_stream_set_callback_direct(stream, audio_cb);
    snd_stream_start_adpcm(stream, sample_rate, audio_channels == 2 ? 1 : 0);
    audio_bytes_fed = 0;

    // Precompute bytes_per_frame as float
    float samples_per_frame_f = (float)sample_rate / (float)fps;
    float bytes_per_frame_f = samples_per_frame_f * ((float)audio_channels / 2.0f);

    while (frame_index < num_frames) {
        int should_be_frame = (int)((float)audio_bytes_fed / bytes_per_frame_f);
        // printf("here1\n");
        while (frame_index < should_be_frame && frame_index < num_frames) {

            if (load_frame(frame_index) != 0) break;
            draw_frame();
            ++frame_index;
        }

        snd_stream_poll(stream);
        wait_exit();
    }

    profiler_stop();
    profiler_clean_up();
    ZSTD_freeDCtx(dctx);
    snd_stream_stop(stream);
    snd_stream_destroy(stream);
    pvr_mem_free(pvr_txr);
    free(frame_offsets);
    free(frame_buffer);
    free(compressed_buffer);
    fclose(fp);
    fclose(audio_fp);
    return 0;
}