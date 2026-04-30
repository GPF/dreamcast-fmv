#ifndef DCFMV_H
#define DCFMV_H

#ifndef DCFMV_DEBUG_LOGS
#define DCFMV_DEBUG_LOGS 0
#endif

#include <kos.h>
#include <dc/pvr.h>
#include <dc/sound/stream.h>
#include <stdbool.h>
#include <stdatomic.h>

#define DCFMV_MAGIC "DCMV"
#define DCFMV_NUM_BUFFERS 24
#define DCFMV_RING_CAPACITY (DCFMV_NUM_BUFFERS + 1)

enum dcfmv_buf_state {
    DCFMV_BUF_EMPTY = 0,
    DCFMV_BUF_LOADING = 1,
    DCFMV_BUF_READY = 2
};

enum dcfmv_present_mode {
    DCFMV_PRESENT_CLIENT = 0,
    DCFMV_PRESENT_OWNED = 1
};

typedef struct dcfmv_preload_job {
    int frame;
    int generation;
} dcfmv_preload_job_t;
typedef dcfmv_preload_job_t PreloadJob;

typedef struct dcfmv {
    dcfmv_preload_job_t preload_ring[DCFMV_RING_CAPACITY];
    atomic_int preload_ring_head;
    atomic_int preload_ring_tail;

    file_t video_fd;
    file_t audio_fd_left;
    file_t audio_fd_right;

    long left_channel_size;
    uint8_t *compressed_buffer;
    uint8_t *frame_buffer[DCFMV_NUM_BUFFERS];
    uint32_t *frame_offsets;
    uint16_t *frame_durations;

    int last_unique_frame_drawn;
    atomic_int buf_ref_count[DCFMV_NUM_BUFFERS];
    atomic_int displayed_total_frame;
    atomic_int frame_index;

    float fps;
    int frame_type;
    int video_width;
    int video_height;
    int content_width;
    int content_height;
    int sample_rate;
    int audio_channels;
    int g_disable_fmv_audio;
    int g_enable_mp3;
    int num_unique_frames;
    int num_total_frames;
    int video_frame_size;
    int max_compressed_size;
    int audio_offset;
    char path[256];

    pvr_ptr_t pvr_txr;
    pvr_poly_hdr_t hdr;
    pvr_poly_hdr_t fallback_hdr;
    pvr_vertex_t vert[4];
    pvr_vertex_t fallback_vert[4];
    snd_stream_hnd_t stream;

    _Atomic double audio_start_time_ms;
    _Atomic int audio_muted;
    int use_audio_clock;
    float frame_duration;
    double frame_timer_anchor;
    _Atomic int buf_state[DCFMV_NUM_BUFFERS];

    int soundbufferalloc;
    volatile int audio_started;
    int use_zstd;

    _Atomic int g_audio_left_on;
    _Atomic int g_audio_right_on;
    _Atomic int g_audio_movie_vol;

    uint32_t fps_num;
    uint32_t fps_den;
    double frame_duration_ms;
    int *GTotalToUnique;
    uint32_t vfd_last_end;
    long last_audio_left_pos;
    long last_audio_right_pos;
    int audio_unmute_pending;
    int audio_clock_resume_pending;
    _Atomic double audio_clock_resume_until_ms;

    int g_is_paused;
    _Atomic int preload_paused;
    _Atomic unsigned int GSeekGeneration;
    int GSeeking;
    int GSeekTargetFrame;
    atomic_int seek_request;
    atomic_int seek_in_progress;
    atomic_int seek_settle_frames;
    int worker_idle_ticks;
    int g_playback_started;
    enum dcfmv_present_mode present_mode;
} dcfmv_t;

extern dcfmv_t *dcfmv_current;

dcfmv_t *dcfmv_create(enum dcfmv_present_mode present_mode);
void dcfmv_destroy(dcfmv_t *fmv);
void dcfmv_control_reset(void);

int dcfmv_open(dcfmv_t *fmv, const char *path);
void dcfmv_close(dcfmv_t *fmv);
void dcfmv_seek(dcfmv_t *fmv, int frame);
void dcfmv_request_seek(dcfmv_t *fmv, int frame);
int dcfmv_take_seek_request(dcfmv_t *fmv);
void dcfmv_set_paused(dcfmv_t *fmv, int paused);
void dcfmv_toggle_pause(dcfmv_t *fmv);
void dcfmv_set_audio_muted(dcfmv_t *fmv, int muted);
void dcfmv_set_audio_volume(dcfmv_t *fmv, int volume);
void dcfmv_set_audio_clock_mode(dcfmv_t *fmv, int use_audio_clock);
void dcfmv_reanchor_clock_to_current_frame(dcfmv_t *fmv);
void dcfmv_set_preload_paused(dcfmv_t *fmv, int paused);
void dcfmv_set_seek_settle_frames(dcfmv_t *fmv, int frames);
int dcfmv_handle_seek_settle(dcfmv_t *fmv, int paused);
void dcfmv_log_state(const char *tag, dcfmv_t *fmv);
int dcfmv_load_frame(dcfmv_t *fmv, int unique_frame, int buf_index);
bool dcfmv_schedule_frame_preload(dcfmv_t *fmv, int frame);
bool dcfmv_schedule_frame_preload_with_generation(dcfmv_t *fmv, int frame, int generation);
void dcfmv_worker_step(dcfmv_t *fmv);
void dcfmv_render_current_video(dcfmv_t *fmv);
void dcfmv_seek_to_frame(dcfmv_t *fmv, int new_frame);
void dcfmv_submit(dcfmv_t *fmv);
double dcfmv_tick(dcfmv_t *fmv);
double dcfmv_wait_until(dcfmv_t *fmv);
size_t dcfmv_audio_poll(dcfmv_t *fmv);

/*
 * dcfmv_audio_init() - allocate and start the KOS ADPCM stream.
 *
 * Call this after dcfmv_open() has populated the header fields
 * (sample_rate, audio_channels, soundbufferalloc).  The function
 * allocates the stream handle, registers the built-in audio_cb, and
 * calls snd_stream_start_adpcm().
 *
 * Returns 0 on success, -1 if audio_channels == 0 (no-op).
 */
int  dcfmv_audio_init(dcfmv_t *fmv);

/*
 * dcfmv_audio_stop_stream() - stop the active ADPCM DMA stream.
 * This keeps the file descriptors open so playback can resume later.
 * Safe to call even if dcfmv_audio_init() was never called.
 */
void dcfmv_audio_stop_stream(dcfmv_t *fmv);

/*
 * dcfmv_audio_start_stream() - start or resume the active ADPCM DMA stream.
 * Safe to call even if the stream was already running.
 */
int  dcfmv_audio_start_stream(dcfmv_t *fmv);

/*
 * dcfmv_audio_stop() - stop and release the KOS ADPCM stream.
 * Safe to call even if dcfmv_audio_init() was never called.
 */
void dcfmv_audio_stop(dcfmv_t *fmv);

const char *dcfmv_path(dcfmv_t *fmv);
const char *dcfmv_header(dcfmv_t *fmv);
int dcfmv_frame_index(dcfmv_t *fmv);
double dcfmv_frame_duration_ms(dcfmv_t *fmv);
int dcfmv_is_paused(dcfmv_t *fmv);
int dcfmv_playback_started(dcfmv_t *fmv);
double dcfmv_ps_ms(void);

#endif /* DCFMV_H */
