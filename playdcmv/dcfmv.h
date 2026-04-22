#ifndef DCFMV_H
#define DCFMV_H

#include <kos.h>
#include <dc/pvr.h>
#include <dc/sound/stream.h>
#include <stdbool.h>
#include <stdatomic.h>

#define DCFMV_MAGIC "DCMV"
#define DCFMV_NUM_BUFFERS 24
#define DCFMV_RING_CAPACITY (DCFMV_NUM_BUFFERS + 1)
#define NUM_BUFFERS DCFMV_NUM_BUFFERS
#define RING_CAPACITY DCFMV_RING_CAPACITY
#define BUF_EMPTY DCFMV_BUF_EMPTY
#define BUF_LOADING DCFMV_BUF_LOADING
#define BUF_READY DCFMV_BUF_READY

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
void dcfmv_set_audio_clock_mode(dcfmv_t *fmv, int use_audio_clock);
void dcfmv_set_preload_paused(dcfmv_t *fmv, int paused);
void dcfmv_set_seek_settle_frames(dcfmv_t *fmv, int frames);
int dcfmv_handle_seek_settle(dcfmv_t *fmv, int paused);
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

const char *dcfmv_path(dcfmv_t *fmv);
const char *dcfmv_header(dcfmv_t *fmv);
int dcfmv_frame_index(dcfmv_t *fmv);
double dcfmv_frame_duration_ms(dcfmv_t *fmv);
int dcfmv_is_paused(dcfmv_t *fmv);
int dcfmv_playback_started(dcfmv_t *fmv);
double dcfmv_ps_ms(void);

#ifdef DCFMV_USE_STATE_MACROS
#define video_fd              dcfmv_current->video_fd
#define audio_fd_left         dcfmv_current->audio_fd_left
#define audio_fd_right        dcfmv_current->audio_fd_right
#define left_channel_size     dcfmv_current->left_channel_size
#define compressed_buffer     dcfmv_current->compressed_buffer
#define frame_buffer          dcfmv_current->frame_buffer
#define frame_offsets         dcfmv_current->frame_offsets
#define frame_durations       dcfmv_current->frame_durations
#define last_unique_frame_drawn dcfmv_current->last_unique_frame_drawn
#define buf_ref_count         dcfmv_current->buf_ref_count
#define displayed_total_frame dcfmv_current->displayed_total_frame
#define frame_index           dcfmv_current->frame_index
#define fps                   dcfmv_current->fps
#define frame_type            dcfmv_current->frame_type
#define video_width           dcfmv_current->video_width
#define video_height          dcfmv_current->video_height
#define content_width         dcfmv_current->content_width
#define content_height        dcfmv_current->content_height
#define sample_rate           dcfmv_current->sample_rate
#define audio_channels        dcfmv_current->audio_channels
#define g_disable_fmv_audio   dcfmv_current->g_disable_fmv_audio
#define g_enable_mp3          dcfmv_current->g_enable_mp3
#define num_unique_frames     dcfmv_current->num_unique_frames
#define num_total_frames      dcfmv_current->num_total_frames
#define video_frame_size      dcfmv_current->video_frame_size
#define max_compressed_size   dcfmv_current->max_compressed_size
#define audio_offset          dcfmv_current->audio_offset
#define stream                dcfmv_current->stream
#define audio_start_time_ms   dcfmv_current->audio_start_time_ms
#define audio_muted           dcfmv_current->audio_muted
#define use_audio_clock       dcfmv_current->use_audio_clock
#define frame_duration        dcfmv_current->frame_duration
#define frame_timer_anchor    dcfmv_current->frame_timer_anchor
#define buf_state             dcfmv_current->buf_state
#define pvr_txr               dcfmv_current->pvr_txr
#define hdr                   dcfmv_current->hdr
#define fallback_hdr          dcfmv_current->fallback_hdr
#define vert                  dcfmv_current->vert
#define fallback_vert         dcfmv_current->fallback_vert
#define soundbufferalloc      dcfmv_current->soundbufferalloc
#define audio_started         dcfmv_current->audio_started
#define use_zstd              dcfmv_current->use_zstd
#define g_audio_left_on       dcfmv_current->g_audio_left_on
#define g_audio_right_on      dcfmv_current->g_audio_right_on
#define g_audio_movie_vol     dcfmv_current->g_audio_movie_vol
#define fps_num               dcfmv_current->fps_num
#define fps_den               dcfmv_current->fps_den
#define frame_duration_ms     dcfmv_current->frame_duration_ms
#define GTotalToUnique        dcfmv_current->GTotalToUnique
#define vfd_last_end          dcfmv_current->vfd_last_end
#define last_audio_left_pos   dcfmv_current->last_audio_left_pos
#define last_audio_right_pos  dcfmv_current->last_audio_right_pos
#define preload_ring          dcfmv_current->preload_ring
#define preload_ring_head     dcfmv_current->preload_ring_head
#define preload_ring_tail     dcfmv_current->preload_ring_tail
#define g_is_paused           dcfmv_current->g_is_paused
#define preload_paused        dcfmv_current->preload_paused
#define GSeekGeneration       dcfmv_current->GSeekGeneration
#define GSeeking              dcfmv_current->GSeeking
#define GSeekTargetFrame      dcfmv_current->GSeekTargetFrame
#define seek_request          dcfmv_current->seek_request
#define seek_in_progress      dcfmv_current->seek_in_progress
#define seek_settle_frames    dcfmv_current->seek_settle_frames
#define g_playback_started    dcfmv_current->g_playback_started
#endif

#endif /* DCFMV_H */
