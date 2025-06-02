#include <kos.h>
#include <dc/sound/stream.h>
#include <dc/sound/sound.h>
#include <dc/pvr.h>
#include <dc/maple/controller.h>
#include <zlib/zlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DCMV_MAGIC "DCMV"
#define VIDEO_FILE "/cd/movie.dcmv"

static FILE *fp = NULL, *audio_fp = NULL;
static uint8_t *frame_buffer = NULL, *compressed_buffer = NULL;
static size_t compressed_buffer_size = 0;
static uint32_t *frame_offsets = NULL;

static int frame_index = 0;
static int video_width, video_height, fps, sample_rate, num_frames, video_frame_size, audio_channels;
static int audio_bytes_fed = 0;
snd_stream_hnd_t stream;
pvr_ptr_t pvr_txr;
pvr_poly_hdr_t hdr;
pvr_vertex_t vert[4];
char screenshotfilename[256];

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

    printf("📦 Header: %dx%d @ %dfps, %dHz, %dch, %d frames, frame_size=%d\n",
           video_width, video_height, fps, sample_rate, audio_channels, num_frames, video_frame_size);

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
        compressed_buffer_size = compressed_size;
    }

    if (fread(compressed_buffer, 1, compressed_size, fp) != compressed_size) {
        printf("❌ Failed to read compressed frame data (%lu bytes)\n", compressed_size);
        return -1;
    }

    // printf("📦 Frame %d: compressed_size=%lu, decompressing...\n", frame_num, compressed_size);

    z_stream zs = {0};
    zs.next_in = compressed_buffer;
    zs.avail_in = compressed_size;
    zs.next_out = frame_buffer;
    zs.avail_out = video_frame_size;

    if (inflateInit2(&zs, -15) != Z_OK) {
        printf("❌ inflateInit2 failed\n");
        return -1;
    }

    int ret = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);

    if (ret != Z_STREAM_END || zs.total_out != video_frame_size) {
        printf("❌ zlib error: ret=%d, total_out=%lu, expected=%d\n", ret, zs.total_out, video_frame_size);
        return -1;
    }

    // printf("✅ Frame %d decompressed to %lu bytes\n", frame_num, zs.total_out);

    // printf("🖼️ DMA transfer to PVR\n");
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
            } else if (state->buttons)
                arch_exit();
        }
    }
}

int main(void) {
    fp = fopen(VIDEO_FILE, "rb");
    if (!fp || load_header() < 0) return -1;

    frame_offsets = malloc(num_frames * sizeof(uint32_t));
    fread(frame_offsets, sizeof(uint32_t), num_frames, fp);

    uint32_t last_offset = frame_offsets[num_frames - 1];
    fseek(fp, last_offset, SEEK_SET);
    uint32_t last_size;
    fread(&last_size, 4, 1, fp);
    size_t audio_offset = last_offset + 4 + last_size;
    printf("🔊 Calculated audio offset: 0x%zX\n", audio_offset);

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

    int samples_per_frame = (sample_rate + fps / 2) / fps;
    int bytes_per_frame = (samples_per_frame / 2) * audio_channels;

    while (frame_index < num_frames) {
        int should_be_frame = audio_bytes_fed / bytes_per_frame;

        while (frame_index < should_be_frame && frame_index < num_frames) {
            if (load_frame(frame_index) != 0) break;
            draw_frame();
            ++frame_index;
        }

        snd_stream_poll(stream);
        wait_exit();
    }

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
