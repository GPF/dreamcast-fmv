/**
 * fmv_play.c - Dreamcast FMV Player
 * ----------------------------------
 * Thin wrapper around the shared dcfmv module.
 */

#include "dcfmv.h"
#include <dc/maple/controller.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIN(a,b) ((a) < (b) ? (a) : (b))

static kthread_t *worker_thread_id = NULL;
static char screenshotfilename[256];
static char movie_path[256];
static int resume_after_seek = 0;

static int is_power_of_2(int n) {
    return n > 0 && (n & (n - 1)) == 0;
}

static const char *compression_name(uint8_t compression_type) {
    switch (compression_type) {
        case 0: return "LZ4";
        case 1: return "Zstandard";
        default: return "Unknown";
    }
}

static const char *frame_type_name(uint8_t frame_type) {
    switch (frame_type) {
        case 0: return "RGB565";
        case 1: return "YUV422";
        default: return "Unknown";
    }
}

static int resolve_movie_path(void) {
    file_t fd = fs_open("/pc/movie.dcmv", O_RDONLY);
    const char *base_try = "/pc/movie.dcmv";
    if (fd < 0) {
        fd = fs_open("/cd/movie.dcmv", O_RDONLY);
        base_try = "/cd/movie.dcmv";
    }

    strncpy(movie_path, base_try, sizeof(movie_path) - 1);
    movie_path[sizeof(movie_path) - 1] = '\0';

    if (fd < 0) {
        printf("movie.dcmv not found on either /pc or /cd.\n");
        movie_path[0] = '\0';
        return -1;
    }

    fs_close(fd);
    return 0;
}

static int init_pvr(dcfmv_t *fmv) {
    const dcfmv_media_info_t *info = dcfmv_media_info(fmv);
    int use_strided;
    int pot_width = 1;
    int pot_height = 1;
    uint32_t txr_format;
    pvr_poly_cxt_t cxt;

    if (!fmv || !info)
        return -1;

    pvr_init_defaults();
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);

    use_strided = !is_power_of_2(info->tex_width) || !is_power_of_2(info->tex_height);
    while (pot_width < info->tex_width) pot_width <<= 1;
    while (pot_height < info->tex_height) pot_height <<= 1;

    fmv->pvr_txr = pvr_mem_malloc(pot_width * pot_height * 2);
    if (!fmv->pvr_txr)
        return -1;

    txr_format = (info->frame_type == 1) ? PVR_TXRFMT_YUV422 : PVR_TXRFMT_RGB565;
    if (use_strided) {
        int display_width;
        int display_height;

        txr_format |= PVR_TXRFMT_VQ_ENABLE | (1 << 25) | PVR_TXRFMT_NONTWIDDLED;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, txr_format,
                         pot_width, pot_height, fmv->pvr_txr, PVR_FILTER_NEAREST);
        pvr_poly_compile(&fmv->hdr, &cxt);
        PVR_SET(PVR_TEXTURE_MODULO, (info->tex_width / 32));

        display_width = (info->tex_width == 320) ? 320 : 640;
        display_height = (info->tex_width == 320) ? 240 : 480;

        fmv->vert[0] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX,.x=0,.y=0,.z=1,.u=0,.v=0,.argb=0xffffffff};
        fmv->vert[1] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX,.x=display_width,.y=0,.z=1,.u=(float)info->content_width / pot_width,.v=0,.argb=0xffffffff};
        fmv->vert[2] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX,.x=0,.y=display_height,.z=1,.u=0,.v=(float)info->content_height / pot_height,.argb=0xffffffff};
        fmv->vert[3] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX_EOL,.x=display_width,.y=display_height,.z=1,.u=(float)info->content_width / pot_width,.v=(float)info->content_height / pot_height,.argb=0xffffffff};
    } else {
        float umin;
        float vmin;
        float umax;
        float vmax;

        txr_format |= PVR_TXRFMT_TWIDDLED | PVR_TXRFMT_VQ_ENABLE;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY,
                         txr_format,
                         pot_width, pot_height, fmv->pvr_txr, PVR_FILTER_NEAREST);
        pvr_poly_compile(&fmv->hdr, &cxt);

        umin = (float)(info->tex_width - info->content_width) / (2.0f * info->tex_width);
        vmin = (float)(info->tex_height - info->content_height) / (2.0f * info->tex_height);
        umax = 1.0f - umin;
        vmax = 1.0f - vmin;

        fmv->vert[0] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX,    .x=0,  .y=0,   .z=1, .u=umin, .v=vmin, .argb=0xffffffff};
        fmv->vert[1] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX,    .x=640,.y=0,   .z=1, .u=umax, .v=vmin, .argb=0xffffffff};
        fmv->vert[2] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX,    .x=0,  .y=480, .z=1, .u=umin, .v=vmax, .argb=0xffffffff};
        fmv->vert[3] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX_EOL,.x=640,.y=480, .z=1, .u=umax, .v=vmax, .argb=0xffffffff};
    }

    fmv->fallback_hdr = fmv->hdr;
    memcpy(fmv->fallback_vert, fmv->vert, sizeof(fmv->vert));
    dcfmv_set_render_resources(fmv, fmv->pvr_txr, &fmv->hdr, &fmv->fallback_hdr,
                               fmv->vert, fmv->fallback_vert);
    dcfmv_reset_render_tracking(fmv);
    return 0;
}

static int init_audio(dcfmv_t *fmv) {
    if (!fmv)
        return -1;

    dcfmv_set_audio_clock_mode(fmv, dcfmv_audio_channels(fmv) > 0);
    if (dcfmv_audio_channels(fmv) <= 0) {
        dcfmv_set_audio_muted(fmv, 1);
        return 0;
    }
    if (dcfmv_audio_init(fmv) < 0)
        return -1;

    dcfmv_set_audio_muted(fmv, 1);
    dcfmv_set_audio_volume(fmv, 255);
    return 0;
}

static void start_playback_from_current_frame(dcfmv_t *fmv) {
    if (!fmv)
        return;

    dcfmv_reanchor_clock_to_current_frame(fmv);
    if (dcfmv_audio_channels(fmv) > 0) {
        dcfmv_audio_start_stream(fmv);
        dcfmv_set_audio_muted(fmv, 0);
    }
}

static void pause_for_seek(dcfmv_t *fmv) {
    if (!fmv || dcfmv_is_paused(fmv))
        return;

    dcfmv_set_paused(fmv, 1);
    resume_after_seek = 1;
    dcfmv_set_audio_muted(fmv, 1);
    dcfmv_audio_stop_stream(fmv);
    printf("[FMV] paused for seek\n");
}

static void wait_exit(dcfmv_t *fmv) {
    static uint16_t prev_buttons = 0;
    maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    cont_state_t *state;
    int current;

    if (!fmv || !dev)
        return;

    state = (cont_state_t *)maple_dev_status(dev);
    if (!state || state->buttons == prev_buttons)
        return;
    prev_buttons = state->buttons;

    current = dcfmv_frame_index(fmv);
    if (state->buttons & CONT_START) {
        dcfmv_toggle_pause(fmv);
        if (dcfmv_is_paused(fmv)) {
            dcfmv_set_audio_muted(fmv, 1);
            dcfmv_audio_stop_stream(fmv);
            printf("[FMV] paused\n");
        } else {
            resume_after_seek = 0;
            start_playback_from_current_frame(fmv);
            printf("[FMV] resumed\n");
        }
    } else if (state->buttons & CONT_DPAD_RIGHT) {
        pause_for_seek(fmv);
        dcfmv_request_seek(fmv, current + 500);
    } else if (state->buttons & CONT_DPAD_LEFT) {
        pause_for_seek(fmv);
        dcfmv_request_seek(fmv, current - 500);
    } else if (state->buttons & CONT_A) {
        sprintf(screenshotfilename, "/pc/screenshot%d.ppm", current);
        vid_screen_shot(screenshotfilename);
    } else if (state->buttons) {
        arch_exit();
    }
}

static void *worker_thread(void *p) {
    dcfmv_t *fmv = (dcfmv_t *)p;

    while (1) {
        dcfmv_worker_step(fmv);
    }
    return NULL;
}

int main(int argc, char **argv) {
    dcfmv_t *fmv;
    const dcfmv_media_info_t *info;

    (void)argc;
    (void)argv;

    fmv = dcfmv_create(DCFMV_PRESENT_OWNED);
    if (!fmv) {
        printf("Failed to allocate FMV state\n");
        return -1;
    }

    dcfmv_current = fmv;
    dcfmv_control_reset();

    if (resolve_movie_path() < 0) {
        dcfmv_destroy(fmv);
        return -1;
    }

    if (dcfmv_open(fmv, movie_path) < 0) {
        dcfmv_destroy(fmv);
        return -1;
    }

    info = dcfmv_media_info(fmv);
    if (!info) {
        dcfmv_close(fmv);
        dcfmv_destroy(fmv);
        return -1;
    }

    printf("Header v%lu (%s): %s %ux%u (content: %ux%u) @ %.2ffps, %uHz, %uch, unique=%lu, total=%lu\n",
           (unsigned long)info->version,
           dcfmv_backend(fmv) == DCFMV_BACKEND_CHUNKS ? "chunks" : "frames",
           frame_type_name(info->frame_type),
           (unsigned)info->tex_width, (unsigned)info->tex_height,
           (unsigned)info->content_width, (unsigned)info->content_height,
           info->fps, (unsigned)info->sample_rate, (unsigned)dcfmv_audio_channels(fmv),
           (unsigned long)info->num_unique_frames,
           (unsigned long)info->num_total_frames);
    printf("Frame size: %lu, Max compressed: %lu, Audio offset: 0x%lX, Compression: %s\n",
           (unsigned long)info->uncompressed_frame_size,
           (unsigned long)info->max_compressed_frame_size,
           (unsigned long)dcfmv_audio_offset(fmv),
           compression_name(info->compression_type));

    vid_set_mode(info->tex_width == 320 ? DM_320x240 : DM_640x480, PM_RGB565);

    if (init_pvr(fmv) < 0 ||
        init_audio(fmv) < 0) {
        dcfmv_close(fmv);
        dcfmv_destroy(fmv);
        return -1;
    }

    worker_thread_id = thd_create(0, worker_thread, fmv);

    printf("Loading initial frames synchronously...\n");
    for (int frame = 0; frame < MIN(4, fmv->num_total_frames); ++frame) {
        int unique = dcfmv_total_to_unique(fmv, frame);
        int buf = unique % DCFMV_NUM_BUFFERS;
        atomic_store(&fmv->buf_state[buf], DCFMV_BUF_LOADING);
        if (dcfmv_load_frame(fmv, frame, buf) != 0)
            atomic_store(&fmv->buf_state[buf], DCFMV_BUF_EMPTY);
    }

    for (int t = dcfmv_frame_index(fmv);
         t < MIN(dcfmv_frame_index(fmv) + MIN(DCFMV_NUM_BUFFERS, (int)(fmv->fps * 2.5f)),
                 fmv->num_total_frames);
         ++t) {
        dcfmv_schedule_frame_preload(fmv, t);
    }

    dcfmv_reanchor_clock_to_current_frame(fmv);
    dcfmv_set_audio_muted(fmv, dcfmv_audio_channels(fmv) == 0 ? 1 : 0);
    if (dcfmv_audio_channels(fmv) > 0)
        dcfmv_set_audio_muted(fmv, 0);

    printf("Starting playback @ %.2fms/frame, total=%d, unique=%d\n",
           fmv->frame_duration, fmv->num_total_frames, fmv->num_unique_frames);

    while (dcfmv_frame_index(fmv) < fmv->num_total_frames) {
        wait_exit(fmv);
        dcfmv_tick(fmv);
        dcfmv_render_current_video(fmv);
        if (resume_after_seek &&
            dcfmv_is_paused(fmv) &&
            atomic_load(&fmv->seek_in_progress) == 0 &&
            atomic_load(&fmv->seek_settle_frames) <= 0) {
            resume_after_seek = 0;
            dcfmv_set_paused(fmv, 0);
            start_playback_from_current_frame(fmv);
            printf("[FMV] resumed after seek\n");
        }
    }

    printf("Playback finished\n");
    dcfmv_set_audio_muted(fmv, 1);
    dcfmv_close(fmv);
    dcfmv_destroy(fmv);
    return 0;
}
