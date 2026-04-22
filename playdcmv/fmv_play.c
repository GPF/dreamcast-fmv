/**
 * fmv_play.c - Dreamcast FMV Player
 * ----------------------------------
 * Thin wrapper around the shared dcfmv module.
 */

#define DCFMV_USE_STATE_MACROS
#include "dcfmv.h"
#include <dc/maple/controller.h>
#include <dc/sound/sound.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIN(a,b) ((a) < (b) ? (a) : (b))

static kthread_t *worker_thread_id = NULL;
static char screenshotfilename[256];
static char movie_path[256];
static int *t2u_lut = NULL;
static int resume_after_seek = 0;

// Timer function
static inline float psTimer(void) {
    #define AICA_MEM_CLOCK 0x021000
    uint32_t jiffies = g2_read_32(SPU_RAM_UNCACHED_BASE + AICA_MEM_CLOCK);
    const float AICA_TICKS_PER_MS = 4.410f; 
    return jiffies / AICA_TICKS_PER_MS;
}

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
        printf("⚠️ movie.dcmv not found on either /pc or /cd.\n");
        movie_path[0] = '\0';
        return -1;
    }

    fs_close(fd);
    return 0;
}

static int load_header(void) {
    char magic[4];
    fs_read(video_fd, magic, 4);
    if (memcmp(magic, DCFMV_MAGIC, 4)) return -1;

    uint32_t version;
    fs_read(video_fd, &version, 4);
    if (version != 6) {
        printf("Unsupported DCMV version: %lu (expected 6)\n",
               (unsigned long)version);
        return -1;
    }

    fs_read(video_fd, &frame_type, 1);
    fs_read(video_fd, &video_width, 2);
    fs_read(video_fd, &video_height, 2);
    fs_read(video_fd, &content_width, 2);
    fs_read(video_fd, &content_height, 2);
    fs_read(video_fd, &fps, sizeof(float));
    fs_read(video_fd, &sample_rate, 2);
    fs_read(video_fd, &audio_channels, 2);

    fs_read(video_fd, &num_unique_frames, 4);
    fs_read(video_fd, &num_total_frames, 4);

    fs_read(video_fd, &video_frame_size, 4);
    fs_read(video_fd, &max_compressed_size, 4);
    fs_read(video_fd, &audio_offset, 4);

    uint8_t compression_type = 0;
    fs_read(video_fd, &compression_type, 1);
    use_zstd = (compression_type == 1);

    frame_duration = 1000.0f / fps;
    frame_duration_ms = frame_duration;
    use_audio_clock = (audio_channels > 0);

    printf("Header v%lu: %s %dx%d (content: %dx%d) @ %.2ffps, %dHz, %dch, unique=%d, total=%d\n",
           (unsigned long)version,
           frame_type == 1 ? "YUV422" : "RGB565",
           video_width, video_height, content_width, content_height,
           fps, sample_rate, audio_channels,
           num_unique_frames, num_total_frames);
    printf("Frame size: %d, Max compressed: %d, Audio offset: 0x%X, Compression: %s\n",
           video_frame_size, max_compressed_size, audio_offset,
           use_zstd ? "Zstandard" : "LZ4");

    return 0;
}

static int init_pvr(int frame_type_value) {
    pvr_init_defaults();
    int use_strided = !is_power_of_2(video_width) || !is_power_of_2(video_height);

    if (frame_type_value == 1)
        pvr_txr = pvr_mem_malloc(video_width * video_height * 2);
    else
        pvr_txr = pvr_mem_malloc(video_frame_size);
    if (!pvr_txr) return -1;

    pvr_poly_cxt_t cxt;
    if (use_strided) {
        int txr_format = (frame_type_value == 1)
            ? PVR_TXRFMT_YUV422 | PVR_TXRFMT_VQ_ENABLE | (1 << 25) | PVR_TXRFMT_NONTWIDDLED
            : PVR_TXRFMT_RGB565 | PVR_TXRFMT_VQ_ENABLE | (1 << 25) | PVR_TXRFMT_NONTWIDDLED;

        int pot_width = 1, pot_height = 1;
        while (pot_width < video_width) pot_width <<= 1;
        while (pot_height < video_height) pot_height <<= 1;

        pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, txr_format,
                         pot_width, pot_height, pvr_txr, PVR_FILTER_NEAREST);
        pvr_poly_compile(&hdr, &cxt);
        PVR_SET(PVR_TEXTURE_MODULO, (video_width / 32));

        int display_width = (video_width == 320) ? 320 : 640;
        int display_height = (video_width == 320) ? 240 : 480;

        vert[0] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX,.x=0,.y=0,.z=1,.u=0,.v=0,.argb=0xffffffff};
        vert[1] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX,.x=display_width,.y=0,.z=1,.u=(float)content_width/pot_width,.v=0,.argb=0xffffffff};
        vert[2] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX,.x=0,.y=display_height,.z=1,.u=0,.v=(float)content_height/pot_height,.argb=0xffffffff};
        vert[3] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX_EOL,.x=display_width,.y=display_height,.z=1,.u=(float)content_width/pot_width,.v=(float)content_height/pot_height,.argb=0xffffffff};
    } else {
        pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY,
                         (frame_type_value == 1 ? PVR_TXRFMT_YUV422 : PVR_TXRFMT_RGB565) |
                         PVR_TXRFMT_TWIDDLED | PVR_TXRFMT_VQ_ENABLE,
                         video_width, video_height, pvr_txr, PVR_FILTER_NEAREST);
        pvr_poly_compile(&hdr, &cxt);

        float umin = (float)(video_width - content_width) / (2.0f * video_width);
        float vmin = (float)(video_height - content_height) / (2.0f * video_height);
        float umax = 1.0f - umin;
        float vmax = 1.0f - vmin;

        vert[0] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX,    .x=0,  .y=0,   .z=1, .u=umin, .v=vmin, .argb=0xffffffff};
        vert[1] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX,    .x=640,.y=0,   .z=1, .u=umax, .v=vmin, .argb=0xffffffff};
        vert[2] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX,    .x=0,  .y=480, .z=1, .u=umin, .v=vmax, .argb=0xffffffff};
        vert[3] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX_EOL,.x=640,.y=480, .z=1, .u=umax, .v=vmax, .argb=0xffffffff};
    }

    fallback_hdr = hdr;
    memcpy(fallback_vert, vert, sizeof(vert));
    return 0;
}

static size_t audio_cb(snd_stream_hnd_t hnd, uintptr_t l, uintptr_t r, size_t req) {
    (void)hnd;
    if (atomic_load(&audio_muted)) {
        memset((void *)l, 0, req);
        if (audio_channels == 2) memset((void *)r, 0, req);
        return req;
    }

    if (audio_channels == 2) {
        size_t lbytes = fs_read(audio_fd_left, (void *)l, req / 2);
        size_t rbytes = fs_read(audio_fd_right, (void *)r, req / 2);
        return lbytes + rbytes;
    }

    return fs_read(audio_fd_left, (void *)l, req);
}

static void seek_audio_to_current_frame(void) {
    if (audio_channels <= 0) return;

    int current_frame = dcfmv_frame_index(dcfmv_current);
    double samples_exact = ((double)current_frame * (double)sample_rate) / (double)fps;
    uint32_t samples_i = (uint32_t)(samples_exact + 0.5);
    uint32_t bytes_per_channel = (samples_i / 2);
    bytes_per_channel = (bytes_per_channel + 15) & ~0xF;

    long left_offset = audio_offset + (long)bytes_per_channel;
    if (left_offset > (audio_offset + left_channel_size))
        left_offset = audio_offset + left_channel_size;

    long right_offset = audio_offset + left_channel_size + (long)bytes_per_channel;
    long right_limit = audio_offset + (long)left_channel_size * 2;
    if (right_offset > right_limit)
        right_offset = right_limit;

    if (audio_fd_left >= 0) {
        fs_close(audio_fd_left);
        audio_fd_left = fs_open(movie_path, O_RDONLY);
        if (audio_fd_left >= 0)
            fs_seek(audio_fd_left, left_offset, SEEK_SET);
    }

    if (audio_channels == 2 && audio_fd_right >= 0) {
        fs_close(audio_fd_right);
        audio_fd_right = fs_open(movie_path, O_RDONLY);
        if (audio_fd_right >= 0)
            fs_seek(audio_fd_right, right_offset, SEEK_SET);
    }
}

static void pause_for_seek(void) {
    if (dcfmv_is_paused(dcfmv_current))
        return;

    dcfmv_set_paused(dcfmv_current, 1);
    resume_after_seek = 1;
    atomic_store(&audio_muted, 1);
    if (audio_channels > 0 && audio_started) {
        snd_stream_stop(stream);
        audio_started = 0;
    }
    printf("[FMV] paused for seek\n");
}

static void resume_playback_from_current_frame(void) {
    double now = dcfmv_ps_ms();
    int current_frame = dcfmv_frame_index(dcfmv_current);
    double frame_ms = (double)current_frame * frame_duration;
    frame_timer_anchor = now;
    atomic_store(&audio_start_time_ms, audio_channels > 0 ? frame_ms : 0.0);
    if (audio_channels > 0) {
        seek_audio_to_current_frame();
        thd_sleep(2);
        if (!audio_started) {
            snd_stream_start_adpcm(stream, sample_rate, audio_channels == 2 ? 1 : 0);
            snd_stream_volume(stream, 255);
            audio_started = 1;
        }
        atomic_store(&audio_muted, 0);
    }
}

static void wait_exit(void) {
    static uint16_t prev_buttons = 0;
    maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    if (!dev) return;

    cont_state_t *state = (cont_state_t *)maple_dev_status(dev);
    if (!state) return;
    if (state->buttons == prev_buttons) return;
    prev_buttons = state->buttons;

    int current = atomic_load(&frame_index);
    if (state->buttons & CONT_START) {
        dcfmv_toggle_pause(dcfmv_current);
        if (dcfmv_is_paused(dcfmv_current)) {
            atomic_store(&audio_muted, 1);
            if (audio_channels > 0 && audio_started) {
                snd_stream_stop(stream);
                audio_started = 0;
            }
            printf("[FMV] paused\n");
        } else {
            resume_after_seek = 0;
            resume_playback_from_current_frame();
            printf("[FMV] resumed\n");
        }
    } else if (state->buttons & CONT_DPAD_RIGHT) {
        pause_for_seek();
        dcfmv_request_seek(dcfmv_current, current + 500);
    } else if (state->buttons & CONT_DPAD_LEFT) {
        pause_for_seek();
        dcfmv_request_seek(dcfmv_current, current - 500);
    } else if (state->buttons & CONT_A) {
        sprintf(screenshotfilename, "/pc/screenshot%d.ppm", current);
        vid_screen_shot(screenshotfilename);
    } else if (state->buttons) {
        arch_exit();
    }
}

static void *worker_thread(void *p) {
    (void)p;
    while (1) {
        dcfmv_worker_step(dcfmv_current);
    }
    return NULL;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    dcfmv_current = dcfmv_create(DCFMV_PRESENT_OWNED);
    if (!dcfmv_current) {
        printf("Failed to allocate FMV state\n");
        return -1;
    }
    if (resolve_movie_path() < 0) {
        dcfmv_destroy(dcfmv_current);
        return -1;
    }

    dcfmv_open(dcfmv_current, movie_path);

    video_fd = fs_open(movie_path, O_RDONLY);
    if (video_fd < 0 || load_header() < 0) return -1;

    vid_set_mode(video_width == 320 ? DM_320x240 : DM_640x480, PM_RGB565);

    fs_seek(video_fd, 50, SEEK_SET);
    frame_offsets = malloc((num_unique_frames + 1) * sizeof(uint32_t));
    frame_durations = malloc(num_unique_frames * sizeof(uint16_t));
    if (!frame_offsets || !frame_durations) return -1;
    fs_read(video_fd, frame_offsets, (num_unique_frames + 1) * sizeof(uint32_t));
    fs_read(video_fd, frame_durations, num_unique_frames * sizeof(uint16_t));

    frame_duration = 1000.0f / (double)fps;
    frame_duration_ms = frame_duration;

    off_t save_pos = fs_tell(video_fd);
    fs_seek(video_fd, 0, SEEK_END);
    off_t file_end = fs_tell(video_fd);
    off_t total_audio_size = file_end - audio_offset;
    left_channel_size = (audio_channels == 2) ? (total_audio_size / 2) : total_audio_size;
    fs_seek(video_fd, save_pos, SEEK_SET);

    if (audio_channels > 0) {
        audio_fd_left = fs_open(movie_path, O_RDONLY);
        if (audio_fd_left < 0) {
            printf("Failed to open audio left fd\n");
            return -1;
        }
        fs_seek(audio_fd_left, audio_offset, SEEK_SET);

        if (audio_channels == 2) {
            audio_fd_right = fs_open(movie_path, O_RDONLY);
            if (audio_fd_right < 0) {
                printf("Failed to open audio right fd\n");
                return -1;
            }
            fs_seek(audio_fd_right, audio_offset + left_channel_size, SEEK_SET);
        }
    }

    compressed_buffer = memalign(32, max_compressed_size);
    if (!compressed_buffer) return -1;

    for (int i = 0; i < NUM_BUFFERS; i++) {
        frame_buffer[i] = memalign(32, video_frame_size);
        if (!frame_buffer[i]) return -1;
        atomic_store(&buf_state[i], BUF_EMPTY);
    }

    if (init_pvr(frame_type) < 0) return -1;

    dcfmv_set_audio_clock_mode(dcfmv_current, audio_channels > 0);
    atomic_store(&audio_muted, audio_channels == 0 ? 1 : 0);

    if (audio_channels > 0) {
        snd_stream_init_ex(audio_channels, 4096);
        stream = snd_stream_alloc(NULL, 4096);
        snd_stream_set_callback_direct(stream, audio_cb);
        snd_stream_start_adpcm(stream, sample_rate, audio_channels == 2 ? 1 : 0);
        snd_stream_volume(stream, 255);
        audio_started = 1;
    }

    t2u_lut = malloc(num_total_frames * sizeof(int));
    if (!t2u_lut) return -1;
    int t = 0;
    for (int u = 0; u < num_unique_frames; ++u) {
        uint16_t reps = frame_durations[u];
        for (uint16_t r = 0; r < reps && t < num_total_frames; ++r)
            t2u_lut[t++] = u;
    }
    for (; t < num_total_frames; ++t) t2u_lut[t] = num_unique_frames - 1;
    GTotalToUnique = t2u_lut;

    worker_thread_id = thd_create(0, worker_thread, NULL);

    printf("Loading initial frames synchronously...\n");
    for (int uf = 0; uf < MIN(4, num_unique_frames); ++uf) {
        int buf = uf % NUM_BUFFERS;
        atomic_store(&buf_state[buf], BUF_LOADING);
        if (dcfmv_load_frame(dcfmv_current, uf, buf) != 0) {
            atomic_store(&buf_state[buf], BUF_EMPTY);
        }
    }

    int start_total = atomic_load(&frame_index);
    int end_total = MIN(start_total + MIN(NUM_BUFFERS, (int)(fps * 2.5f)), num_total_frames);
    for (int t = start_total; t < end_total; ++t) {
        dcfmv_schedule_frame_preload(dcfmv_current, t);
    }

    frame_timer_anchor = psTimer();
    atomic_store(&audio_start_time_ms, 0.0);
    atomic_store(&audio_muted, audio_channels == 0 ? 1 : 0);

    printf("Starting playback @ %.2fms/frame, total=%d, unique=%d\n",
           frame_duration, num_total_frames, num_unique_frames);

    while (atomic_load(&frame_index) < num_total_frames) {
        wait_exit();
        dcfmv_tick(dcfmv_current);
        if (resume_after_seek &&
            dcfmv_is_paused(dcfmv_current) &&
            atomic_load(&seek_in_progress) == 0 &&
            atomic_load(&seek_settle_frames) <= 0) {
            resume_after_seek = 0;
            dcfmv_set_paused(dcfmv_current, 0);
            resume_playback_from_current_frame();
            printf("[FMV] resumed after seek\n");
        }
    }

    printf("Playback finished\n");
    arch_exit();

    atomic_store(&audio_muted, 1);
    if (worker_thread_id) thd_join(worker_thread_id, NULL);
    if (audio_channels > 0) {
        snd_stream_stop(stream);
        snd_stream_destroy(stream);
    }
    dcfmv_close(dcfmv_current);
    dcfmv_destroy(dcfmv_current);

    return 0;
}
