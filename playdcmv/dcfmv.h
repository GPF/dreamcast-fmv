// dcfmv.h - Dreamcast FMV core (DCMV v1.0 chunked)
// Public Domain / MIT — Troy Davis (GPF)

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dcfmv dcfmv_t;

typedef struct {
    // If 0, dcfmv picks sensible defaults
    int decode_threads;          // default 3
    int num_buffers;             // default 16
    int chunk_cache_slots;       // default 4 (active slots)
    int audio_ring_slots;        // default 24
    int audio_buffer_bytes;      // default 4096 (per-channel per slot)
    double target_audio_ms;      // default 700.0
    int prefetch_ahead;          // default (num_buffers - 3)
    int prefetch_boundary_frames;// default 4
    int initial_preload;         // default num_buffers
    int chunk_io_slice_bytes;    // default 128k
    int worker_poll_ms;          // default 5
    int verbose;                 // default 1
} dcfmv_config_t;

// Header you already use (kept identical to packer).
typedef struct __attribute__((packed)) {
    char     magic[4];
    uint32_t version;
    uint8_t  frame_type;            // 0=RGB565, 1=YUV422
    uint16_t tex_width;
    uint16_t tex_height;
    uint16_t content_width;
    uint16_t content_height;
    float    fps;
    uint16_t sample_rate;
    uint16_t channels;
    uint32_t num_unique_frames;
    uint32_t num_total_frames;
    uint32_t uncompressed_frame_size;
    uint32_t max_compressed_frame_size;
    uint8_t  compression_type;      // 0=LZ4, 1=Zstd
    float    chunk_duration;
    uint32_t num_chunks;
    uint32_t chunk_index_offset;
    uint8_t  padding[10];
} DCMVHeader;

// Lifecycle
dcfmv_t *dcfmv_create(const dcfmv_config_t *cfg);  // cfg may be NULL
int      dcfmv_open(dcfmv_t *p, const char *path); // opens + loads tables/index + allocs caches/buffers
void     dcfmv_close(dcfmv_t *p);                  // safe to call even if not opened
void     dcfmv_destroy(dcfmv_t *p);

// Info
const DCMVHeader *dcfmv_header(const dcfmv_t *p);
const char       *dcfmv_path(const dcfmv_t *p);

// Playback controls
void dcfmv_seek(dcfmv_t *p, int total_frame);      // request seek; next ticks will converge quickly
int  dcfmv_frame_index(const dcfmv_t *p);          // current total frame
int  dcfmv_playback_started(const dcfmv_t *p);     // 0/1

// Timing
double dcfmv_ps_ms(void);                          // AICA-based wall clock in ms
double dcfmv_frame_duration_ms(const dcfmv_t *p);  // 1000/fps

// Core tick:
// - advances scheduling/decoding
// - updates audio start (first frame) when possible
// - performs frame skip decisions
// Returns: recommended absolute wall-clock deadline (psTimer ms) to wait until.
//   If <= now, you can just thd_pass() / poll.
// You still own the main loop and PVR scene.
double dcfmv_tick(dcfmv_t *p);

// Pause control
int  dcfmv_is_paused(const dcfmv_t *p);
void dcfmv_set_paused(dcfmv_t *p, int paused);
void dcfmv_toggle_pause(dcfmv_t *p);

// Rendering submission:
// Call inside an active scene/list (you control pvr_scene_begin/list_begin/etc).
// This submits the textured quad for the *current* frame if available.
// Returns 1 if it submitted a frame this call, else 0 (stall).
int dcfmv_submit(dcfmv_t *p);

// Audio hook helper (optional): call once per loop if you want.
// (dcfmv_tick already polls in its wait helper logic, but DCSinge might want explicit polling)
void dcfmv_audio_poll(dcfmv_t *p);

// Optional helper wait (uses snd_stream_poll internally).
void dcfmv_wait_until(dcfmv_t *p, double deadline_wall_ms);

#ifdef __cplusplus
}
#endif
