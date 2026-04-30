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

static int load_header(dcfmv_t *fmv) {
    char magic[4];
    uint32_t version;
    uint8_t compression_type = 0;

    if (!fmv || fmv->video_fd < 0)
        return -1;

    fs_read(fmv->video_fd, magic, 4);
    if (memcmp(magic, DCFMV_MAGIC, 4))
        return -1;

    fs_read(fmv->video_fd, &version, 4);
    if (version != 6) {
        printf("Unsupported DCMV version: %lu (expected 6)\n",
               (unsigned long)version);
        return -1;
    }

    fs_read(fmv->video_fd, &fmv->frame_type, 1);
    fs_read(fmv->video_fd, &fmv->video_width, 2);
    fs_read(fmv->video_fd, &fmv->video_height, 2);
    fs_read(fmv->video_fd, &fmv->content_width, 2);
    fs_read(fmv->video_fd, &fmv->content_height, 2);
    fs_read(fmv->video_fd, &fmv->fps, sizeof(float));
    fs_read(fmv->video_fd, &fmv->sample_rate, 2);
    fs_read(fmv->video_fd, &fmv->audio_channels, 2);
    fs_read(fmv->video_fd, &fmv->num_unique_frames, 4);
    fs_read(fmv->video_fd, &fmv->num_total_frames, 4);
    fs_read(fmv->video_fd, &fmv->video_frame_size, 4);
    fs_read(fmv->video_fd, &fmv->max_compressed_size, 4);
    fs_read(fmv->video_fd, &fmv->audio_offset, 4);
    fs_read(fmv->video_fd, &compression_type, 1);

    fmv->use_zstd = (compression_type == 1);
    fmv->frame_duration = 1000.0f / fmv->fps;
    fmv->frame_duration_ms = fmv->frame_duration;
    dcfmv_set_audio_clock_mode(fmv, fmv->audio_channels > 0);

    printf("Header v%lu: %s %dx%d (content: %dx%d) @ %.2ffps, %dHz, %dch, unique=%d, total=%d\n",
           (unsigned long)version,
           fmv->frame_type == 1 ? "YUV422" : "RGB565",
           fmv->video_width, fmv->video_height,
           fmv->content_width, fmv->content_height,
           fmv->fps, fmv->sample_rate, fmv->audio_channels,
           fmv->num_unique_frames, fmv->num_total_frames);
    printf("Frame size: %d, Max compressed: %d, Audio offset: 0x%X, Compression: %s\n",
           fmv->video_frame_size, fmv->max_compressed_size, fmv->audio_offset,
           fmv->use_zstd ? "Zstandard" : "LZ4");

    return 0;
}

static int init_pvr(dcfmv_t *fmv) {
    int use_strided;
    pvr_poly_cxt_t cxt;

    if (!fmv)
        return -1;

    pvr_init_defaults();
    use_strided = !is_power_of_2(fmv->video_width) || !is_power_of_2(fmv->video_height);

    if (fmv->frame_type == 1)
        fmv->pvr_txr = pvr_mem_malloc(fmv->video_width * fmv->video_height * 2);
    else
        fmv->pvr_txr = pvr_mem_malloc(fmv->video_frame_size);
    if (!fmv->pvr_txr)
        return -1;

    if (use_strided) {
        int txr_format = (fmv->frame_type == 1)
            ? PVR_TXRFMT_YUV422 | PVR_TXRFMT_VQ_ENABLE | (1 << 25) | PVR_TXRFMT_NONTWIDDLED
            : PVR_TXRFMT_RGB565 | PVR_TXRFMT_VQ_ENABLE | (1 << 25) | PVR_TXRFMT_NONTWIDDLED;
        int pot_width = 1;
        int pot_height = 1;
        int display_width;
        int display_height;

        while (pot_width < fmv->video_width) pot_width <<= 1;
        while (pot_height < fmv->video_height) pot_height <<= 1;

        pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, txr_format,
                         pot_width, pot_height, fmv->pvr_txr, PVR_FILTER_NEAREST);
        pvr_poly_compile(&fmv->hdr, &cxt);
        PVR_SET(PVR_TEXTURE_MODULO, (fmv->video_width / 32));

        display_width = (fmv->video_width == 320) ? 320 : 640;
        display_height = (fmv->video_width == 320) ? 240 : 480;

        fmv->vert[0] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX,.x=0,.y=0,.z=1,.u=0,.v=0,.argb=0xffffffff};
        fmv->vert[1] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX,.x=display_width,.y=0,.z=1,.u=(float)fmv->content_width / pot_width,.v=0,.argb=0xffffffff};
        fmv->vert[2] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX,.x=0,.y=display_height,.z=1,.u=0,.v=(float)fmv->content_height / pot_height,.argb=0xffffffff};
        fmv->vert[3] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX_EOL,.x=display_width,.y=display_height,.z=1,.u=(float)fmv->content_width / pot_width,.v=(float)fmv->content_height / pot_height,.argb=0xffffffff};
    } else {
        float umin;
        float vmin;
        float umax;
        float vmax;

        pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY,
                         (fmv->frame_type == 1 ? PVR_TXRFMT_YUV422 : PVR_TXRFMT_RGB565) |
                         PVR_TXRFMT_TWIDDLED | PVR_TXRFMT_VQ_ENABLE,
                         fmv->video_width, fmv->video_height, fmv->pvr_txr, PVR_FILTER_NEAREST);
        pvr_poly_compile(&fmv->hdr, &cxt);

        umin = (float)(fmv->video_width - fmv->content_width) / (2.0f * fmv->video_width);
        vmin = (float)(fmv->video_height - fmv->content_height) / (2.0f * fmv->video_height);
        umax = 1.0f - umin;
        vmax = 1.0f - vmin;

        fmv->vert[0] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX,    .x=0,  .y=0,   .z=1, .u=umin, .v=vmin, .argb=0xffffffff};
        fmv->vert[1] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX,    .x=640,.y=0,   .z=1, .u=umax, .v=vmin, .argb=0xffffffff};
        fmv->vert[2] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX,    .x=0,  .y=480, .z=1, .u=umin, .v=vmax, .argb=0xffffffff};
        fmv->vert[3] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX_EOL,.x=640,.y=480, .z=1, .u=umax, .v=vmax, .argb=0xffffffff};
    }

    fmv->fallback_hdr = fmv->hdr;
    memcpy(fmv->fallback_vert, fmv->vert, sizeof(fmv->vert));
    return 0;
}

static int init_frame_tables(dcfmv_t *fmv) {
    int t = 0;

    if (!fmv)
        return -1;

    fs_seek(fmv->video_fd, 50, SEEK_SET);
    fmv->frame_offsets = malloc((fmv->num_unique_frames + 1) * sizeof(uint32_t));
    fmv->frame_durations = malloc(fmv->num_unique_frames * sizeof(uint16_t));
    fmv->GTotalToUnique = malloc(fmv->num_total_frames * sizeof(int));
    if (!fmv->frame_offsets || !fmv->frame_durations || !fmv->GTotalToUnique)
        return -1;

    fs_read(fmv->video_fd, fmv->frame_offsets,
            (fmv->num_unique_frames + 1) * sizeof(uint32_t));
    fs_read(fmv->video_fd, fmv->frame_durations,
            fmv->num_unique_frames * sizeof(uint16_t));

    for (int u = 0; u < fmv->num_unique_frames; ++u) {
        uint16_t reps = fmv->frame_durations[u];
        for (uint16_t r = 0; r < reps && t < fmv->num_total_frames; ++r)
            fmv->GTotalToUnique[t++] = u;
    }
    for (; t < fmv->num_total_frames; ++t)
        fmv->GTotalToUnique[t] = fmv->num_unique_frames - 1;

    return 0;
}

static int init_buffers(dcfmv_t *fmv) {
    if (!fmv)
        return -1;

    fmv->compressed_buffer = memalign(32, fmv->max_compressed_size);
    if (!fmv->compressed_buffer)
        return -1;

    for (int i = 0; i < DCFMV_NUM_BUFFERS; i++) {
        fmv->frame_buffer[i] = memalign(32, fmv->video_frame_size);
        if (!fmv->frame_buffer[i])
            return -1;
        atomic_store(&fmv->buf_state[i], DCFMV_BUF_EMPTY);
    }

    return 0;
}

static int init_audio(dcfmv_t *fmv) {
    off_t save_pos;
    off_t file_end;
    off_t total_audio_size;

    if (!fmv)
        return -1;

    save_pos = fs_tell(fmv->video_fd);
    fs_seek(fmv->video_fd, 0, SEEK_END);
    file_end = fs_tell(fmv->video_fd);
    total_audio_size = file_end - fmv->audio_offset;
    fmv->left_channel_size = (fmv->audio_channels == 2) ? (total_audio_size / 2) : total_audio_size;
    fs_seek(fmv->video_fd, save_pos, SEEK_SET);

    if (dcfmv_audio_init(fmv) < 0)
        return -1;

    dcfmv_set_audio_muted(fmv, fmv->audio_channels == 0 ? 1 : 0);
    dcfmv_set_audio_volume(fmv, 255);
    return 0;
}

static void start_playback_from_current_frame(dcfmv_t *fmv) {
    if (!fmv)
        return;

    dcfmv_reanchor_clock_to_current_frame(fmv);
    if (fmv->audio_channels > 0) {
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

    fmv->video_fd = fs_open(movie_path, O_RDONLY);
    if (fmv->video_fd < 0 || load_header(fmv) < 0) {
        dcfmv_close(fmv);
        dcfmv_destroy(fmv);
        return -1;
    }

    vid_set_mode(fmv->video_width == 320 ? DM_320x240 : DM_640x480, PM_RGB565);

    if (init_frame_tables(fmv) < 0 ||
        init_buffers(fmv) < 0 ||
        init_pvr(fmv) < 0 ||
        init_audio(fmv) < 0) {
        dcfmv_close(fmv);
        dcfmv_destroy(fmv);
        return -1;
    }

    worker_thread_id = thd_create(0, worker_thread, fmv);

    printf("Loading initial frames synchronously...\n");
    for (int uf = 0; uf < MIN(4, fmv->num_unique_frames); ++uf) {
        int buf = uf % DCFMV_NUM_BUFFERS;
        atomic_store(&fmv->buf_state[buf], DCFMV_BUF_LOADING);
        if (dcfmv_load_frame(fmv, uf, buf) != 0)
            atomic_store(&fmv->buf_state[buf], DCFMV_BUF_EMPTY);
    }

    for (int t = dcfmv_frame_index(fmv);
         t < MIN(dcfmv_frame_index(fmv) + MIN(DCFMV_NUM_BUFFERS, (int)(fmv->fps * 2.5f)),
                 fmv->num_total_frames);
         ++t) {
        dcfmv_schedule_frame_preload(fmv, t);
    }

    dcfmv_reanchor_clock_to_current_frame(fmv);
    dcfmv_set_audio_muted(fmv, fmv->audio_channels == 0 ? 1 : 0);
    if (fmv->audio_channels > 0)
        dcfmv_set_audio_muted(fmv, 0);

    printf("Starting playback @ %.2fms/frame, total=%d, unique=%d\n",
           fmv->frame_duration, fmv->num_total_frames, fmv->num_unique_frames);

    while (dcfmv_frame_index(fmv) < fmv->num_total_frames) {
        wait_exit(fmv);
        dcfmv_tick(fmv);
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
