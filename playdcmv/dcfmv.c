#include "dcfmv.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd/zstd.h>
#include <lz4/lz4.h>

static void DCMV_Log(const char *fmt, ...) {
#if !DCFMV_DEBUG_LOGS
    (void)fmt;
    return;
#else
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
#endif
}

static void DCMV_Error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

static ZSTD_DCtx *dcfmv_zstd_dctx = NULL;
static mutex_t dcfmv_io_lock = MUTEX_INITIALIZER;
static mutex_t dcfmv_audio_lock = MUTEX_INITIALIZER;

static inline double dcfmv_decode_timer_ms(void) {
    #define AICA_MEM_CLOCK 0x021000
    uint32_t jiffies = g2_read_32(SPU_RAM_UNCACHED_BASE + AICA_MEM_CLOCK);
    const double aica_ticks_per_ms = 4.410;
    return (double)jiffies / aica_ticks_per_ms;
}

/*
 * ROLLBACK_EXPERIMENT(audio-warmup):
 * Hold on the first post-seek frame for a couple of ticks before restarting
 * ADPCM. This does not truly prefill a muted stream, but it lets the seek
 * handoff settle before audio becomes the master clock again.
 */
#define DCFMV_AUDIO_WARMUP_TICKS 2
dcfmv_t *dcfmv_current = NULL;

static int dcfmv_clamp_stream_volume(int vol) {
    if (vol < 0) return 0;
    if (vol > 255) return 255;
    return vol;
}

static double dcfmv_audio_resume_bias_ms(const dcfmv_t *fmv) {
    double queued_samples_per_channel;

    if (!fmv || fmv->sample_rate <= 0 || fmv->soundbufferalloc <= 0)
        return 0.0;

    /* KOS start_adpcm() prefills exactly soundbufferalloc bytes per channel. */
    queued_samples_per_channel = (double)fmv->soundbufferalloc * 2.0;
    return (queued_samples_per_channel * 1000.0) / (double)fmv->sample_rate;
}

static void dcfmv_audio_apply_volume(dcfmv_t *fmv) {
    int vol;

    if (!fmv || fmv->audio_channels <= 0 || fmv->stream == SND_STREAM_INVALID)
        return;

    vol = atomic_load(&fmv->audio_muted)
        ? 0
        : dcfmv_clamp_stream_volume(atomic_load(&fmv->g_audio_movie_vol));

    mutex_lock(&dcfmv_audio_lock);
    snd_stream_volume(fmv->stream, vol);
    mutex_unlock(&dcfmv_audio_lock);
}

void dcfmv_log_state(const char *tag, dcfmv_t *fmv) {
    if (!fmv) return;
    DCMV_Log("[FMV] %s frame=%d paused=%d muted=%d settle=%d hold=%d clock=%d anchor=%.2f base=%.2f stream=%ld L=%d R=%d",
             tag,
             atomic_load(&fmv->frame_index),
             fmv->g_is_paused,
             atomic_load(&fmv->audio_muted),
             atomic_load(&fmv->seek_settle_frames),
             atomic_load(&fmv->preload_paused),
             fmv->use_audio_clock,
             fmv->frame_timer_anchor,
             atomic_load(&fmv->audio_start_time_ms),
             (long)fmv->stream,
             fmv->audio_fd_left,
             fmv->audio_fd_right);
}

static void dcfmv_free_buffers(dcfmv_t *fmv) {
    if (!fmv) return;

    if (fmv->compressed_buffer) {
        free(fmv->compressed_buffer);
        fmv->compressed_buffer = NULL;
    }

    for (int i = 0; i < DCFMV_NUM_BUFFERS; i++) {
        if (fmv->frame_buffer[i]) {
            free(fmv->frame_buffer[i]);
            fmv->frame_buffer[i] = NULL;
        }
    }

    if (fmv->frame_offsets) {
        free(fmv->frame_offsets);
        fmv->frame_offsets = NULL;
    }

    if (fmv->frame_durations) {
        free(fmv->frame_durations);
        fmv->frame_durations = NULL;
    }

    if (fmv->GTotalToUnique) {
        free(fmv->GTotalToUnique);
        fmv->GTotalToUnique = NULL;
    }
}

static inline int dcfmv_total_to_unique_frame(dcfmv_t *fmv, int total_frame) {
    if (!fmv || (unsigned)total_frame >= (unsigned)fmv->num_total_frames)
        return fmv ? (fmv->num_unique_frames - 1) : 0;
    return fmv->GTotalToUnique[total_frame];
}

dcfmv_t *dcfmv_create(enum dcfmv_present_mode present_mode) {
    dcfmv_t *fmv = calloc(1, sizeof(*fmv));
    if (!fmv) return NULL;

    fmv->video_fd = -1;
    fmv->audio_fd_left = -1;
    fmv->audio_fd_right = -1;
    fmv->stream = SND_STREAM_INVALID;
    fmv->seek_request = -1;
    fmv->seek_in_progress = 0;
    fmv->seek_settle_frames = 0;
    fmv->g_enable_mp3 = 1;
    fmv->soundbufferalloc = 4096;
    fmv->audio_muted = 1;
    fmv->preload_paused = 0;

    for (int i = 0; i < DCFMV_NUM_BUFFERS; i++) {
        atomic_store(&fmv->buf_state[i], DCFMV_BUF_EMPTY);
        atomic_store(&fmv->buf_ref_count[i], 0);
    }

    atomic_store(&fmv->preload_ring_head, 0);
    atomic_store(&fmv->preload_ring_tail, 0);
    atomic_store(&fmv->displayed_total_frame, 0);
    atomic_store(&fmv->frame_index, 0);
    atomic_store(&fmv->audio_start_time_ms, 0.0);
    atomic_store(&fmv->g_audio_left_on, 1);
    atomic_store(&fmv->g_audio_right_on, 1);
    atomic_store(&fmv->g_audio_movie_vol, 255);
    atomic_store(&fmv->GSeekGeneration, 0);
    fmv->use_audio_clock = 1;

    fmv->frame_duration = 1.0f / 30.0f;
    fmv->frame_duration_ms = 0.0;
    fmv->fps = 30.0f;
    fmv->g_disable_fmv_audio = 0;
    fmv->audio_started = 0;
    fmv->use_zstd = 0;
    fmv->audio_unmute_pending = 0;
    fmv->audio_clock_resume_pending = 0;
    atomic_store(&fmv->audio_clock_resume_until_ms, 0.0);
    fmv->present_mode = present_mode;
    if (!dcfmv_zstd_dctx) {
        dcfmv_zstd_dctx = ZSTD_createDCtx();
    }
    return fmv;
}

void dcfmv_destroy(dcfmv_t *fmv) {
    if (!fmv) return;
    dcfmv_free_buffers(fmv);
    if (dcfmv_current == fmv) {
        dcfmv_current = NULL;
    }
    free(fmv);
}

void dcfmv_control_reset(void) {
    if (!dcfmv_current) return;

    dcfmv_current->g_is_paused = 0;
    atomic_store(&dcfmv_current->preload_paused, 0);
    atomic_store(&dcfmv_current->GSeekGeneration, 0);
    dcfmv_current->GSeeking = 0;
    dcfmv_current->GSeekTargetFrame = -1;
    atomic_store(&dcfmv_current->seek_request, -1);
    atomic_store(&dcfmv_current->seek_in_progress, 0);
    atomic_store(&dcfmv_current->seek_settle_frames, 0);
    dcfmv_current->audio_unmute_pending = 0;
    dcfmv_current->audio_clock_resume_pending = 0;
    atomic_store(&dcfmv_current->audio_clock_resume_until_ms, 0.0);
    dcfmv_current->worker_idle_ticks = 0;
    dcfmv_current->g_playback_started = 0;
}

int dcfmv_open(dcfmv_t *fmv, const char *path) {
    if (!fmv) return -1;
    dcfmv_current = fmv;
    if (path) {
        strncpy(fmv->path, path, sizeof(fmv->path) - 1);
        fmv->path[sizeof(fmv->path) - 1] = '\0';
    } else {
        fmv->path[0] = '\0';
    }
    return 0;
}

void dcfmv_close(dcfmv_t *fmv) {
    if (!fmv) return;
    dcfmv_audio_stop(fmv);
    if (fmv->video_fd >= 0) {
        fs_close(fmv->video_fd);
        fmv->video_fd = -1;
    }
}

void dcfmv_request_seek(dcfmv_t *fmv, int frame) {
    if (!fmv) return;
    atomic_store(&fmv->seek_request, frame);
}

void dcfmv_seek(dcfmv_t *fmv, int frame) {
    dcfmv_request_seek(fmv, frame);
}

int dcfmv_take_seek_request(dcfmv_t *fmv) {
    if (!fmv) return -1;
    return atomic_exchange(&fmv->seek_request, -1);
}

void dcfmv_set_paused(dcfmv_t *fmv, int paused) {
    if (!fmv) return;
    if (fmv->g_is_paused != (paused ? 1 : 0)) {
        DCMV_Log("[FMV] paused -> %d", paused ? 1 : 0);
    }
    fmv->g_is_paused = paused ? 1 : 0;
}

void dcfmv_toggle_pause(dcfmv_t *fmv) {
    if (!fmv) return;
    fmv->g_is_paused = !fmv->g_is_paused;
}

void dcfmv_set_audio_muted(dcfmv_t *fmv, int muted) {
    if (!fmv) return;
    atomic_store(&fmv->audio_muted, muted ? 1 : 0);
    dcfmv_audio_apply_volume(fmv);
}

void dcfmv_set_audio_volume(dcfmv_t *fmv, int volume) {
    if (!fmv) return;
    atomic_store(&fmv->g_audio_movie_vol, dcfmv_clamp_stream_volume(volume));
    dcfmv_audio_apply_volume(fmv);
}

void dcfmv_set_audio_clock_mode(dcfmv_t *fmv, int use_audio_clock) {
    if (!fmv) return;
    if (fmv->use_audio_clock != (use_audio_clock ? 1 : 0)) {
        DCMV_Log("[FMV] audio clock mode -> %s", use_audio_clock ? "audio" : "fps");
    }
    fmv->use_audio_clock = use_audio_clock ? 1 : 0;
}

void dcfmv_reanchor_clock_to_current_frame(dcfmv_t *fmv) {
    if (!fmv) return;

    int current_frame = atomic_load(&fmv->frame_index);
    double current_time_ms = (double)current_frame * fmv->frame_duration;
    double now = dcfmv_ps_ms();

    if (fmv->use_audio_clock) {
        fmv->frame_timer_anchor = now;
        atomic_store(&fmv->audio_start_time_ms, current_time_ms);
    } else {
        fmv->frame_timer_anchor = now - current_time_ms;
        atomic_store(&fmv->audio_start_time_ms, 0.0);
    }
}

void dcfmv_set_preload_paused(dcfmv_t *fmv, int paused) {
    if (!fmv) return;
    atomic_store(&fmv->preload_paused, paused ? 1 : 0);
}

void dcfmv_set_seek_settle_frames(dcfmv_t *fmv, int frames) {
    if (!fmv) return;
    atomic_store(&fmv->seek_settle_frames, frames < 0 ? 0 : frames);
}

int dcfmv_handle_seek_settle(dcfmv_t *fmv, int paused) {
    if (!fmv) return 0;

    int settle_frames = atomic_load(&fmv->seek_settle_frames);
    if (settle_frames <= 0)
        return 0;

    int remaining = settle_frames - 1;
    atomic_store(&fmv->seek_settle_frames, remaining);

    if (!fmv->use_audio_clock) {
        DCMV_Log("[FMV] settle fps-clock: frame=%d remaining=%d paused=%d",
                  atomic_load(&fmv->frame_index), remaining, paused);
    }

    dcfmv_reanchor_clock_to_current_frame(fmv);

    if (remaining <= 0 && !paused && fmv->use_audio_clock) {
        dcfmv_audio_start_stream(fmv);
        fmv->audio_unmute_pending = 1;
    }

    return 1;
}

int dcfmv_load_frame(dcfmv_t *fmv, int unique_frame, int buf_index) {
    if (!fmv || unique_frame < 0 || buf_index < 0 || buf_index >= DCFMV_NUM_BUFFERS)
        return -1;

    uint32_t offset = fmv->frame_offsets[unique_frame];
    uint32_t next_offset = fmv->frame_offsets[unique_frame + 1];
    uint32_t compressed_size = next_offset - offset;

    if (fmv->vfd_last_end != (long)offset) {
        mutex_lock(&dcfmv_io_lock);
        fs_seek(fmv->video_fd, offset, SEEK_SET);
        mutex_unlock(&dcfmv_io_lock);
        fmv->vfd_last_end = offset;
    }

    mutex_lock(&dcfmv_io_lock);
    fs_read(fmv->video_fd, fmv->compressed_buffer, compressed_size);
    mutex_unlock(&dcfmv_io_lock);
    fmv->vfd_last_end = offset + compressed_size;

    if (fmv->use_zstd == 1) {
        if (!dcfmv_zstd_dctx) return -1;
        ZSTD_DCtx_reset(dcfmv_zstd_dctx, ZSTD_reset_session_only);
        ZSTD_inBuffer in = { fmv->compressed_buffer, compressed_size, 0 };
        ZSTD_outBuffer out = { fmv->frame_buffer[buf_index], (size_t)fmv->video_frame_size, 0 };

        size_t ret = 1;
        while (ret != 0 && out.pos < out.size) {
            ret = ZSTD_decompressStream(dcfmv_zstd_dctx, &out, &in);
            if (ZSTD_isError(ret)) return -1;
        }
        if (out.pos != (size_t)fmv->video_frame_size) return -1;
    } else {
        double decode_start_ms = dcfmv_decode_timer_ms();
        int res = LZ4_decompress_fast(
            (const char *)fmv->compressed_buffer,
            (char *)fmv->frame_buffer[buf_index],
            fmv->video_frame_size);
        double decode_elapsed_ms = dcfmv_decode_timer_ms() - decode_start_ms;
        if (res < 0) {
            DCMV_Error("LZ4_decompress_fast failed for frame %d (buf %d)", unique_frame, buf_index);
            return -1;
        }
        DCMV_Log("[LZ4] frame=%d buf=%d compressed=%lu decoded=%d time=%.3fms",
                 unique_frame,
                 buf_index,
                 (unsigned long)compressed_size,
                 fmv->video_frame_size,
                 decode_elapsed_ms);
    }

    atomic_store(&fmv->buf_state[buf_index], DCFMV_BUF_READY);
    return 0;
}

bool dcfmv_schedule_frame_preload(dcfmv_t *fmv, int frame) {
    if (!fmv || frame >= fmv->num_total_frames) return false;
    int unique_frame = dcfmv_total_to_unique_frame(fmv, frame);
    int buf = unique_frame % DCFMV_NUM_BUFFERS;

    if (atomic_load(&fmv->buf_state[buf]) != DCFMV_BUF_EMPTY) return false;

    int head = atomic_load(&fmv->preload_ring_head);
    int tail = atomic_load(&fmv->preload_ring_tail);
    int next_head = (head + 1) % DCFMV_RING_CAPACITY;
    if (next_head == tail) return false;

    for (int i = tail; i != head; i = (i + 1) % DCFMV_RING_CAPACITY) {
        int queued_unique = dcfmv_total_to_unique_frame(fmv, fmv->preload_ring[i].frame);
        if (queued_unique == unique_frame) return false;
    }

    fmv->preload_ring[head].frame = frame;
    fmv->preload_ring[head].generation = atomic_load(&fmv->GSeekGeneration);
    atomic_store(&fmv->preload_ring_head, next_head);
    return true;
}

bool dcfmv_schedule_frame_preload_with_generation(dcfmv_t *fmv, int frame, int generation) {
    if (!fmv || frame >= fmv->num_total_frames) return false;
    int unique_frame = dcfmv_total_to_unique_frame(fmv, frame);
    int buf = unique_frame % DCFMV_NUM_BUFFERS;

    if (atomic_load(&fmv->buf_state[buf]) != DCFMV_BUF_EMPTY) return false;

    int head = atomic_load(&fmv->preload_ring_head);
    int tail = atomic_load(&fmv->preload_ring_tail);
    int next_head = (head + 1) % DCFMV_RING_CAPACITY;
    if (next_head == tail) return false;

    for (int i = tail; i != head; i = (i + 1) % DCFMV_RING_CAPACITY) {
        int queued_unique = dcfmv_total_to_unique_frame(fmv, fmv->preload_ring[i].frame);
        if (queued_unique == unique_frame) return false;
    }

    fmv->preload_ring[head].frame = frame;
    fmv->preload_ring[head].generation = generation;
    atomic_store(&fmv->preload_ring_head, next_head);
    return true;
}

void dcfmv_submit(dcfmv_t *fmv) {
    (void)fmv;
}

double dcfmv_wait_until(dcfmv_t *fmv) {
    (void)fmv;
    return 0.0;
}

size_t dcfmv_audio_poll(dcfmv_t *fmv) {
    if (!fmv) return 0;
    if (fmv->audio_channels > 0 &&
        fmv->audio_started &&
        !atomic_load(&fmv->audio_muted)) {
        mutex_lock(&dcfmv_audio_lock);
        snd_stream_poll(fmv->stream);
        mutex_unlock(&dcfmv_audio_lock);
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * audio_cb - KOS snd_stream callback
 *
 * Called from the KOS audio thread.  Reads raw ADPCM bytes directly from
 * the open file descriptors.  The dcfmv_io_lock protects the fs_read calls
 * so they do not race with the seek / preload paths.
 * --------------------------------------------------------------------------- */
static size_t dcfmv_audio_cb(snd_stream_hnd_t hnd, uintptr_t l, uintptr_t r,
                              size_t req) {
    dcfmv_t *fmv = dcfmv_current;   /* stream is always on the active instance */
    size_t total_bytes = 0;
    (void)hnd;

    if (!fmv || fmv->audio_channels <= 0) {
        memset((void *)l, 0, req);
        return req;
    }

    if (fmv->audio_channels == 1) {
        /* Mono — only left channel file descriptor is used. */
        size_t lbytes = 0;

        if (atomic_load(&fmv->g_audio_left_on)) {
            mutex_lock(&dcfmv_io_lock);
            lbytes = fs_read(fmv->audio_fd_left, (void *)l, req);
            mutex_unlock(&dcfmv_io_lock);
            fmv->last_audio_left_pos += lbytes;
        } else {
            memset((void *)l, 0, req);
            lbytes = req;
            fmv->last_audio_left_pos += lbytes;
        }

        total_bytes = lbytes;
    } else {
        /* Stereo — split the request evenly between L and R descriptors. */
        size_t half   = req / 2;
        size_t lbytes = 0, rbytes = 0;

        if (atomic_load(&fmv->g_audio_left_on)) {
            mutex_lock(&dcfmv_io_lock);
            lbytes = fs_read(fmv->audio_fd_left, (void *)l, half);
            mutex_unlock(&dcfmv_io_lock);
            fmv->last_audio_left_pos += lbytes;
        } else {
            memset((void *)l, 0, half);
            lbytes = half;
            fmv->last_audio_left_pos += lbytes;
        }

        if (atomic_load(&fmv->g_audio_right_on)) {
            mutex_lock(&dcfmv_io_lock);
            rbytes = fs_read(fmv->audio_fd_right, (void *)r, half);
            mutex_unlock(&dcfmv_io_lock);
            fmv->last_audio_right_pos += rbytes;
        } else {
            memset((void *)r, 0, half);
            rbytes = half;
            fmv->last_audio_right_pos += rbytes;
        }

        total_bytes = lbytes + rbytes;
    }

    return total_bytes;
}

/* ---------------------------------------------------------------------------
 * dcfmv_audio_init
 *
 * Allocates the KOS ADPCM stream handle and starts playback.
 * Must be called after the header fields (sample_rate, audio_channels,
 * soundbufferalloc, audio_offset, left_channel_size) have been populated
 * by the caller via dcfmv_open / header parsing.
 *
 * Audio file descriptors (audio_fd_left / audio_fd_right) are also opened
 * here, so the caller must have already set fmv->path and fmv->audio_offset.
 *
 * Returns  0 on success (or when audio_channels == 0, treated as no-op).
 * Returns -1 on failure.
 * --------------------------------------------------------------------------- */
int dcfmv_audio_init(dcfmv_t *fmv) {
    if (!fmv) return -1;

    if (fmv->audio_channels <= 0) {
        fmv->stream = SND_STREAM_INVALID;
        DCMV_Log("[Audio] No audio channels — stream not started.");
        return 0;
    }

    /* Open the per-channel file descriptors (separate read cursors). */
    fmv->audio_fd_left = fs_open(fmv->path, O_RDONLY);
    if (fmv->audio_fd_left < 0) {
        DCMV_Error("[Audio] PANIC: Failed to open audio_fd_left from %s", fmv->path);
        return -1;
    }
    fs_seek(fmv->audio_fd_left, fmv->audio_offset, SEEK_SET);
    fmv->last_audio_left_pos = fmv->audio_offset;

    if (fmv->audio_channels == 2) {
        fmv->audio_fd_right = fs_open(fmv->path, O_RDONLY);
        if (fmv->audio_fd_right < 0) {
            DCMV_Error("[Audio] PANIC: Failed to open audio_fd_right from %s", fmv->path);
            fs_close(fmv->audio_fd_left);
            fmv->audio_fd_left = -1;
            return -1;
        }
        fs_seek(fmv->audio_fd_right, fmv->audio_offset + fmv->left_channel_size, SEEK_SET);
        fmv->last_audio_right_pos = fmv->audio_offset + fmv->left_channel_size;
    }

    /* Initialise the KOS stream subsystem and allocate our stream slot. */
    snd_stream_init_ex(fmv->audio_channels, fmv->soundbufferalloc);

    fmv->stream = snd_stream_alloc(NULL, fmv->soundbufferalloc);
    if (fmv->stream == SND_STREAM_INVALID) {
        DCMV_Error("[Audio] PANIC: snd_stream_alloc failed");
        if (fmv->audio_fd_right >= 0) {
            fs_close(fmv->audio_fd_right);
            fmv->audio_fd_right = -1;
        }
        if (fmv->audio_fd_left >= 0) {
            fs_close(fmv->audio_fd_left);
            fmv->audio_fd_left = -1;
        }
        return -1;
    }

    snd_stream_set_callback_direct(fmv->stream, dcfmv_audio_cb);
    mutex_lock(&dcfmv_audio_lock);
    snd_stream_start_adpcm(fmv->stream, fmv->sample_rate,
                           fmv->audio_channels == 2 ? 1 : 0);
    mutex_unlock(&dcfmv_audio_lock);
    fmv->audio_started = 1;

    /* Start muted; caller unmutes when playback is ready. */
    dcfmv_set_audio_muted(fmv, 1);

    DCMV_Log("[Audio] Stream started: rate=%d ch=%d buf=%d",
             fmv->sample_rate, fmv->audio_channels, fmv->soundbufferalloc);
    return 0;
}

void dcfmv_audio_stop_stream(dcfmv_t *fmv) {
    if (!fmv || fmv->audio_channels <= 0 || fmv->stream == SND_STREAM_INVALID) return;
    if (!fmv->audio_started) {
        dcfmv_log_state("stop_stream(already-stopped)", fmv);
        return;
    }

    mutex_lock(&dcfmv_audio_lock);
    snd_stream_stop(fmv->stream);
    mutex_unlock(&dcfmv_audio_lock);
    fmv->audio_started = 0;
    dcfmv_log_state("stop_stream", fmv);
}

int dcfmv_audio_start_stream(dcfmv_t *fmv) {
    if (!fmv) return -1;
    if (fmv->audio_channels <= 0) return 0;
    if (fmv->stream == SND_STREAM_INVALID) return -1;
    if (fmv->audio_started) {
        dcfmv_log_state("start_stream(already-started)", fmv);
        return 0;
    }

    mutex_lock(&dcfmv_audio_lock);
    snd_stream_start_adpcm(fmv->stream, fmv->sample_rate,
                           fmv->audio_channels == 2 ? 1 : 0);
    mutex_unlock(&dcfmv_audio_lock);
    fmv->audio_started = 1;
    dcfmv_audio_apply_volume(fmv);
    dcfmv_log_state("start_stream", fmv);
    return 0;
}

/* ---------------------------------------------------------------------------
 * dcfmv_audio_stop
 *
 * Stops and destroys the ADPCM stream, then closes the audio file
 * descriptors.  Safe to call even when no stream was started.
 * --------------------------------------------------------------------------- */
void dcfmv_audio_stop(dcfmv_t *fmv) {
    if (!fmv) return;

    if (fmv->stream != SND_STREAM_INVALID) {
        mutex_lock(&dcfmv_audio_lock);
        snd_stream_destroy(fmv->stream);
        mutex_unlock(&dcfmv_audio_lock);
        fmv->stream = SND_STREAM_INVALID;
        fmv->audio_started = 0;
    }

    if (fmv->audio_fd_left >= 0) {
        fs_close(fmv->audio_fd_left);
        fmv->audio_fd_left = -1;
    }

    if (fmv->audio_fd_right >= 0) {
        fs_close(fmv->audio_fd_right);
        fmv->audio_fd_right = -1;
    }
}

const char *dcfmv_path(dcfmv_t *fmv) {
    return fmv ? fmv->path : NULL;
}

const char *dcfmv_header(dcfmv_t *fmv) {
    static char header[256];
    if (!fmv) return NULL;
    snprintf(header, sizeof(header),
             "DCMV %d %dx%d content=%dx%d fps=%.2f sample_rate=%d channels=%d unique=%d total=%d frame=%d audio_offset=%d compression=%s",
             6,
             fmv->video_width, fmv->video_height,
             fmv->content_width, fmv->content_height,
             fmv->fps, fmv->sample_rate, fmv->audio_channels,
             fmv->num_unique_frames, fmv->num_total_frames,
             fmv->video_frame_size, fmv->audio_offset,
             fmv->use_zstd ? "zstd" : "lz4");
    return header;
}

int dcfmv_frame_index(dcfmv_t *fmv) {
    if (!fmv) return 0;
    return atomic_load(&fmv->frame_index);
}

double dcfmv_frame_duration_ms(dcfmv_t *fmv) {
    if (!fmv) return 0.0;
    return fmv->frame_duration_ms;
}

int dcfmv_is_paused(dcfmv_t *fmv) {
    if (!fmv) return 0;
    return fmv->g_is_paused;
}

int dcfmv_playback_started(dcfmv_t *fmv) {
    if (!fmv) return 0;
    return fmv->g_playback_started;
}

double dcfmv_ps_ms(void) {
    return (double)timer_ms_gettime64();
}

double dcfmv_tick(dcfmv_t *fmv) {
    if (!fmv) return 0.0;

    static double accumulated_frame_debt = 0.0;
    static int frames_dropped = 0;
    static int stall_count = 0;
    static double max_frame_time = 0.0;
    static double avg_frame_time = 0.0;
    static double frame_time_samples = 0.0;
    static int unique_display_count = 0;
    static int expected_display_count = 0;

    int req = dcfmv_take_seek_request(fmv);
    if (req >= 0) {
        atomic_store(&fmv->seek_in_progress, 1);
        if (!fmv->use_audio_clock) {
            DCMV_Log("[FMV] seek request fps-clock: frame=%d paused=%d settle=%d",
                      req, fmv->g_is_paused, atomic_load(&fmv->seek_settle_frames));
        }
        dcfmv_seek_to_frame(fmv, req);
        atomic_store(&fmv->frame_index, req);
        atomic_store(&fmv->seek_in_progress, 0);

        if (atomic_load(&fmv->seek_settle_frames) <= 0) {
            if (fmv->use_audio_clock) {
                atomic_store(&fmv->seek_settle_frames, DCFMV_AUDIO_WARMUP_TICKS);
                DCMV_Log("[Audio] ROLLBACK_EXPERIMENT warmup hold: %d ticks after seek to frame %d",
                         DCFMV_AUDIO_WARMUP_TICKS, req);
            }
        }

        if (atomic_load(&fmv->seek_settle_frames) <= 0) {
            double seek_time_ms = (double)req * (1000.0 / fmv->fps);
            double now = dcfmv_ps_ms();
            if (fmv->use_audio_clock) {
                fmv->frame_timer_anchor = now;
                atomic_store(&fmv->audio_start_time_ms, seek_time_ms);
                if (!fmv->g_is_paused) {
                    dcfmv_audio_start_stream(fmv);
                    dcfmv_set_audio_muted(fmv, 0);
                }
            } else {
                fmv->frame_timer_anchor = now - seek_time_ms;
                atomic_store(&fmv->audio_start_time_ms, 0.0);
            }
        }
    }

    if (dcfmv_handle_seek_settle(fmv, fmv->g_is_paused)) {
        dcfmv_render_current_video(fmv);
        if (atomic_load(&fmv->seek_settle_frames) <= 0 && !fmv->g_is_paused) {
            int current_frame = atomic_load(&fmv->frame_index);
            double current_time_ms = (double)current_frame * fmv->frame_duration;
            double now = dcfmv_ps_ms();
            if (fmv->use_audio_clock) {
                fmv->frame_timer_anchor = now;
                atomic_store(&fmv->audio_start_time_ms, current_time_ms);
            } else {
                fmv->frame_timer_anchor = now - current_time_ms;
                atomic_store(&fmv->audio_start_time_ms, 0.0);
            }
        }
        return 0.0;
    }

    if (fmv->audio_unmute_pending &&
        fmv->use_audio_clock &&
        fmv->audio_started &&
        !fmv->g_is_paused) {
        double now_unmute;
        double bias_ms;
        double current_time_ms;

        fmv->audio_unmute_pending = 0;
        dcfmv_set_audio_muted(fmv, 0);
        bias_ms = dcfmv_audio_resume_bias_ms(fmv);
        now_unmute = dcfmv_ps_ms();
        current_time_ms = (double)atomic_load(&fmv->frame_index) * fmv->frame_duration;
        fmv->frame_timer_anchor = now_unmute - current_time_ms;
        fmv->audio_clock_resume_pending = 1;
        atomic_store(&fmv->audio_clock_resume_until_ms, now_unmute + bias_ms);
    }

    int current_frame = atomic_load(&fmv->frame_index);
    double now = dcfmv_ps_ms();
    double elapsed_ms = now - fmv->frame_timer_anchor;
    double current_playback_time_ms;
    if (fmv->audio_clock_resume_pending &&
        now >= atomic_load(&fmv->audio_clock_resume_until_ms)) {
        fmv->audio_clock_resume_pending = 0;
        dcfmv_reanchor_clock_to_current_frame(fmv);
        now = dcfmv_ps_ms();
        elapsed_ms = now - fmv->frame_timer_anchor;
    }

    if (fmv->use_audio_clock) {
        if (fmv->audio_clock_resume_pending) {
            current_playback_time_ms = elapsed_ms;
        } else {
            double audio_base_ms = atomic_load(&fmv->audio_start_time_ms);
            current_playback_time_ms = audio_base_ms + elapsed_ms;
        }
    } else {
        current_playback_time_ms = elapsed_ms;
        static int last_noaudio_tick_frame = -1;
        if (current_frame != last_noaudio_tick_frame) {
            DCMV_Log("[FMV] fps-clock tick: frame=%d elapsed=%.2fms settle=%d paused=%d",
                      current_frame, current_playback_time_ms,
                      atomic_load(&fmv->seek_settle_frames), fmv->g_is_paused);
            last_noaudio_tick_frame = current_frame;
        }
    }
    double expected_video_time = current_frame * fmv->frame_duration;

    double playback_delta = current_playback_time_ms - expected_video_time;
    if (playback_delta < 2.0 && playback_delta > -2.0)
        current_playback_time_ms = expected_video_time;

    double target_time_ms = expected_video_time;

    if (fmv->g_is_paused) {
        /*
         * Keep presenting the last decoded frame while paused. Some scripts
         * intentionally pause between clip transitions after the current frame
         * has already aged out of the preload ring, and gating redraw on the
         * current buffer state would drop to the fallback clear color.
         */
        static int last_noaudio_pause_frame = -1;
        if (!fmv->use_audio_clock) {
            int paused_frame = atomic_load(&fmv->frame_index);
            if (paused_frame != last_noaudio_pause_frame) {
                DCMV_Log("[FMV] fps-clock paused redraw: frame=%d settle=%d",
                          paused_frame, atomic_load(&fmv->seek_settle_frames));
                last_noaudio_pause_frame = paused_frame;
            }
        }
        dcfmv_render_current_video(fmv);
        fmv->frame_timer_anchor = dcfmv_ps_ms();
        return 0.0;
    }

    int frames_to_skip = 0;
    (void)frames_to_skip;

    int expected_frame = (int)(current_playback_time_ms / fmv->frame_duration);
    if (expected_frame < 0)
        expected_frame = 0;

    if (current_playback_time_ms >= target_time_ms) {
        dcfmv_render_current_video(fmv);

        int draw_total = current_frame;
        int unique_id = dcfmv_total_to_unique_frame(fmv, draw_total);
        int buf = unique_id % DCFMV_NUM_BUFFERS;
        int state = atomic_load(&fmv->buf_state[buf]);

        if (state == DCFMV_BUF_READY) {
            if (unique_id != fmv->last_unique_frame_drawn) {
                fmv->last_unique_frame_drawn = unique_id;
                unique_display_count = 1;
                expected_display_count = fmv->frame_durations[unique_id];
            } else {
                unique_display_count++;
            }

            if (unique_display_count >= expected_display_count)
                atomic_store(&fmv->buf_state[buf], DCFMV_BUF_EMPTY);

            atomic_store(&fmv->frame_index, current_frame + 1);
            atomic_fetch_add(&fmv->displayed_total_frame, 1);
        }
    }

    int cur_frame = atomic_load(&fmv->frame_index);
    int preloads = 0;
    const int window = (DCFMV_NUM_BUFFERS / 2 < 8) ? (DCFMV_NUM_BUFFERS / 2) : 8;
    for (int i = 0; i < window; i++) {
        int target = cur_frame + i;
        if (target >= fmv->num_total_frames) break;
        int unique = dcfmv_total_to_unique_frame(fmv, target);
        int buf = unique % DCFMV_NUM_BUFFERS;
        if (atomic_load(&fmv->buf_state[buf]) == DCFMV_BUF_EMPTY) {
            if (dcfmv_schedule_frame_preload(fmv, target)) preloads++;
        }
    }

    double t1 = dcfmv_ps_ms();
    double render_ms = (t1 - now);

    if (render_ms > max_frame_time) max_frame_time = render_ms;
    avg_frame_time = (avg_frame_time * frame_time_samples + render_ms) / (frame_time_samples + 1.0);
    frame_time_samples += 1.0;

    double overrun = render_ms - fmv->frame_duration;
    if (overrun > 0.0)
        accumulated_frame_debt -= overrun;
    else
        accumulated_frame_debt += (-overrun * 0.1);
    accumulated_frame_debt *= 0.95;

    double wait_ms = target_time_ms - current_playback_time_ms;
    if (wait_ms > 1.0) {
        if (wait_ms > fmv->frame_duration) wait_ms = fmv->frame_duration;
        thd_sleep((int)wait_ms);
    } else if (wait_ms > 0.0) {
        thd_pass();
    }

    (void)frames_dropped;
    (void)stall_count;
    return render_ms;
}

void dcfmv_seek_to_frame(dcfmv_t *fmv, int new_frame) {
    if (!fmv) return;

    if (new_frame < 0) new_frame = 0;
    if (new_frame >= fmv->num_total_frames)
        new_frame = fmv->num_total_frames - 1;

    dcfmv_set_audio_muted(fmv, 1);
    atomic_store(&fmv->preload_paused, 1);
    dcfmv_audio_stop_stream(fmv);
    fmv->GSeekTargetFrame = new_frame;

    DCMV_Log("[Seek] >>> Begin seek_to_frame(%d)", new_frame);
    dcfmv_log_state("seek(begin)", fmv);

    for (int i = 0; i < DCFMV_NUM_BUFFERS; i++) {
        atomic_store(&fmv->buf_state[i], DCFMV_BUF_EMPTY);
    }

    atomic_store(&fmv->preload_ring_head, 0);
    atomic_store(&fmv->preload_ring_tail, 0);
    memset(fmv->preload_ring, 0, sizeof(fmv->preload_ring));

    fmv->last_unique_frame_drawn = -1;
    atomic_store(&fmv->seek_request, -1);

    mutex_lock(&dcfmv_io_lock);
    fs_close(fmv->video_fd);
    thd_sleep(10);
    fmv->video_fd = fs_open(fmv->path, O_RDONLY);
    mutex_unlock(&dcfmv_io_lock);

    int uf = dcfmv_total_to_unique_frame(fmv, new_frame);
    uint32_t off = fmv->frame_offsets[uf];

    mutex_lock(&dcfmv_io_lock);
    fs_seek(fmv->video_fd, off, SEEK_SET);
    mutex_unlock(&dcfmv_io_lock);

    fmv->vfd_last_end = off;

    if (fmv->audio_channels > 0) {
        double samples_exact = ((double)new_frame * (double)fmv->sample_rate) / (double)fmv->fps;
        uint32_t samples_i   = (uint32_t)(samples_exact + 0.5);
        uint32_t bytes_per_channel = (samples_i / 2);
        bytes_per_channel = (bytes_per_channel + 15) & ~0xF;

        long left_offset  = fmv->audio_offset + (long)bytes_per_channel;
        if (left_offset > (fmv->audio_offset + fmv->left_channel_size))
            left_offset = fmv->audio_offset + fmv->left_channel_size;

        long right_offset = fmv->audio_offset + fmv->left_channel_size + (long)bytes_per_channel;
        long right_limit  = fmv->audio_offset + (long)fmv->left_channel_size * 2;
        if (right_offset > right_limit) right_offset = right_limit;

        mutex_lock(&dcfmv_io_lock);

        fs_close(fmv->audio_fd_left);
        fmv->audio_fd_left = fs_open(fmv->path, O_RDONLY);
        fs_seek(fmv->audio_fd_left, left_offset, SEEK_SET);

        if (fmv->audio_channels == 2) {
            fs_close(fmv->audio_fd_right);
            fmv->audio_fd_right = fs_open(fmv->path, O_RDONLY);
            fs_seek(fmv->audio_fd_right, right_offset, SEEK_SET);
        }

        mutex_unlock(&dcfmv_io_lock);

        fmv->last_audio_left_pos  = left_offset;
        fmv->last_audio_right_pos = right_offset;

        /*
         * The stream remains active; seek handling only repositions the
         * underlying file descriptors and re-anchors playback timing.
         */
    }

    atomic_store(&fmv->frame_index, new_frame);
    atomic_store(&fmv->displayed_total_frame, 0);

    fmv->frame_timer_anchor = dcfmv_ps_ms();

    double frame_ms = (double)new_frame * (1000.0 / (double)fmv->fps);

    if (fmv->use_audio_clock) {
        atomic_store(&fmv->audio_start_time_ms, frame_ms);
        fmv->frame_timer_anchor = dcfmv_ps_ms();
    } else {
        atomic_store(&fmv->audio_start_time_ms, 0.0);
        fmv->frame_timer_anchor = dcfmv_ps_ms() - frame_ms;
        DCMV_Log("[FMV] seek fps-clock anchor: frame=%d anchor=%.2f",
                 new_frame, fmv->frame_timer_anchor);
    }

    DCMV_Log("[Seek] anchor=%.2f base=%.2f (frame=%d, fps=%.2f, frame_dur=%.2fms)",
             fmv->frame_timer_anchor,
             atomic_load(&fmv->audio_start_time_ms),
             new_frame,
             fmv->fps,
             1000.0 / fmv->fps);
    dcfmv_log_state("seek(anchored)", fmv);

    /*
     * Bump generation before priming/scheduling so every post-seek preload
     * belongs to the fresh seek generation.
     */
    atomic_fetch_add(&fmv->GSeekGeneration, 1);
    int cur_gen = atomic_load(&fmv->GSeekGeneration);

    for (int i = 0; i < DCFMV_RING_CAPACITY; i++) {
        fmv->preload_ring[i].frame = -1;
        fmv->preload_ring[i].generation = cur_gen;
    }

    DCMV_Log("[Seek] Incremented GSeekGeneration -> %d (flushed ring)", cur_gen);

    /*
     * Prime the exact target frame synchronously.
     */
    int first_unique = dcfmv_total_to_unique_frame(fmv, new_frame);
    int first_buf = first_unique % DCFMV_NUM_BUFFERS;

    if (atomic_load(&fmv->buf_state[first_buf]) == DCFMV_BUF_EMPTY) {
        atomic_store(&fmv->buf_state[first_buf], DCFMV_BUF_LOADING);

        if (dcfmv_load_frame(fmv, first_unique, first_buf) == 0) {
            atomic_store(&fmv->buf_state[first_buf], DCFMV_BUF_READY);
            DCMV_Log("[Seek] Primed initial frame %d (unique=%d buf=%d)",
                     new_frame, first_unique, first_buf);
        } else {
            atomic_store(&fmv->buf_state[first_buf], DCFMV_BUF_EMPTY);
            DCMV_Log("[Seek] Failed to prime initial frame %d (unique=%d buf=%d)",
                     new_frame, first_unique, first_buf);
        }
    }

    /*
     * Let the worker run before queueing the runway.
     */
    atomic_store(&fmv->preload_paused, 0);

    /*
     * Queue a small post-seek runway so frame+1/frame+2 are not still loading
     * when playback resumes after heavy Lua/resource work.
     */
    int max_preloads = (DCFMV_NUM_BUFFERS / 2) < 16 ? (DCFMV_NUM_BUFFERS / 2) : 16;

    for (int i = 1; i < max_preloads; i++) {
        int target = new_frame + i;
        if (target >= fmv->num_total_frames) break;

        dcfmv_schedule_frame_preload_with_generation(fmv, target, cur_gen);
    }

    /*
     * Short warmup. Unlike the old version, preload is already unpaused here,
     * so this gives the worker time to actually decode the runway.
     */
    thd_sleep(20);

    DCMV_Log("[Seek] <<< Completed seek_to_frame(%d)", new_frame);
    dcfmv_log_state("seek(end)", fmv);
}

void dcfmv_worker_step(dcfmv_t *fmv) {
    if (!fmv) return;

    if (atomic_load(&fmv->preload_paused)) {
        thd_sleep(2);
        return;
    }

    int cur_gen = atomic_load(&fmv->GSeekGeneration);
    dcfmv_audio_poll(fmv);

    int tail = atomic_load(&fmv->preload_ring_tail);
    int head = atomic_load(&fmv->preload_ring_head);

    if (tail != head) {
        PreloadJob job = fmv->preload_ring[tail];
        atomic_store(&fmv->preload_ring_tail, (tail + 1) % DCFMV_RING_CAPACITY);

        if (job.generation == cur_gen) {
            int total_frame = job.frame;
            int unique_frame = dcfmv_total_to_unique_frame(fmv, total_frame);
            int buf = unique_frame % DCFMV_NUM_BUFFERS;

            int expected = DCFMV_BUF_EMPTY;
            if (atomic_compare_exchange_strong(&fmv->buf_state[buf], &expected, DCFMV_BUF_LOADING)) {
                int res = dcfmv_load_frame(fmv, unique_frame, buf);
                if (res != 0) {
                    atomic_store(&fmv->buf_state[buf], DCFMV_BUF_EMPTY);
                    DCMV_Log("[Worker] load_frame failed for %d (unique=%d buf=%d)",
                           total_frame, unique_frame, buf);
                }
            }

            fmv->worker_idle_ticks = 0;
        }
    }

    int current = atomic_load(&fmv->frame_index);
    int max_preloads = (DCFMV_NUM_BUFFERS < 16) ? DCFMV_NUM_BUFFERS : 16;
    int scheduled = 0;
    for (int i = 1; i <= max_preloads; i++) {
        int target = current + i;
        if (target >= fmv->num_total_frames)
            break;

        int unique = dcfmv_total_to_unique_frame(fmv, target);
        int buf = unique % DCFMV_NUM_BUFFERS;
        if (atomic_load(&fmv->buf_state[buf]) == DCFMV_BUF_EMPTY) {
            if (dcfmv_schedule_frame_preload_with_generation(fmv, target, cur_gen)) {
                scheduled++;
            }
        }
    }

    if (scheduled == 0 && tail == head) {
        int cur = atomic_load(&fmv->frame_index);
        int unique = dcfmv_total_to_unique_frame(fmv, cur);
        int cur_buf = unique % DCFMV_NUM_BUFFERS;
        int cur_state = atomic_load(&fmv->buf_state[cur_buf]);

        if (cur_state != DCFMV_BUF_READY &&
            ++fmv->worker_idle_ticks > 120 &&
            !fmv->g_is_paused &&
            atomic_load(&fmv->seek_request) < 0 &&
            !atomic_load(&fmv->seek_in_progress)) {
            DCMV_Log("[Worker] Idle/stalled (cur=%d gen=%d). Re-seeding preload window.", cur, cur_gen);
            fmv->worker_idle_ticks = 0;

            for (int j = 0; j < DCFMV_NUM_BUFFERS; j++) {
                atomic_store(&fmv->buf_state[j], DCFMV_BUF_EMPTY);
            }

            atomic_store(&fmv->preload_ring_head, 0);
            atomic_store(&fmv->preload_ring_tail, 0);

            int reseed = (DCFMV_NUM_BUFFERS < 8) ? DCFMV_NUM_BUFFERS : 8;
            for (int k = 0; k < reseed; k++) {
                int target = cur + k;
                if (target >= fmv->num_total_frames) break;
                dcfmv_schedule_frame_preload_with_generation(fmv, target, cur_gen);
            }
        } else {
            fmv->worker_idle_ticks = 0;
        }
    } else {
        fmv->worker_idle_ticks = 0;
    }

    thd_sleep(1);
}

void dcfmv_render_current_video(dcfmv_t *fmv) {
    if (!fmv) return;

    int cur_total = atomic_load(&fmv->frame_index);
    int unique = dcfmv_total_to_unique_frame(fmv, cur_total);
    int buf = unique % DCFMV_NUM_BUFFERS;
    int state = atomic_load(&fmv->buf_state[buf]);
    static int last_render_logged = -1;

    atomic_store(&fmv->displayed_total_frame, cur_total);

    if (unique != last_render_logged) {
        DCMV_Log("[Render] frame=%d unique=%d buf=%d state=%d last=%d",
                  cur_total, unique, buf, state, fmv->last_unique_frame_drawn);
        last_render_logged = unique;
    }

    if (unique != fmv->last_unique_frame_drawn && state == DCFMV_BUF_READY) {
        pvr_txr_load_dma(fmv->frame_buffer[buf], fmv->pvr_txr, fmv->video_frame_size, -1, NULL, 0);
        fmv->last_unique_frame_drawn = unique;
    }

    if (fmv->present_mode == DCFMV_PRESENT_OWNED) {
        pvr_scene_begin();
        pvr_list_begin(PVR_LIST_OP_POLY);
    }

    uintptr_t sq_dest_addr = (uintptr_t)SQ_MASK_DEST(PVR_TA_INPUT);

    if (state == DCFMV_BUF_READY || fmv->last_unique_frame_drawn >= 0) {
        sq_fast_cpy((void *)sq_dest_addr, &fmv->hdr, 1);
        sq_fast_cpy((void *)sq_dest_addr, fmv->vert, 4);
    } else {
        sq_fast_cpy((void *)sq_dest_addr, &fmv->fallback_hdr, 1);
        sq_fast_cpy((void *)sq_dest_addr, fmv->fallback_vert, 4);
    }

    if (fmv->present_mode == DCFMV_PRESENT_OWNED) {
        pvr_list_finish();
        pvr_scene_finish();
    }
}
