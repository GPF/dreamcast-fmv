// dcfmv.c - Dreamcast FMV core (DCMV v1.0 chunked)
// Public Domain / MIT — Troy Davis (GPF)

#include <assert.h>
#include "dcfmv.h"

#include <kos.h>
#include <kos/dbgio.h>
#include <kos/thread.h>

#include <dc/sound/stream.h>
#include <dc/sound/sound.h>
#include <dc/pvr.h>
#include <dc/sq.h>          // sq_fast_cpy

#include <arch/timer.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <stdint.h>
#include <malloc.h>
#include <fcntl.h>

#include <lz4/lz4.h>

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd/zstd.h>

#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))

#define DCMV_MAGIC "DCMV"
#define ADPCM_BYTES_PER_SAMPLE 0.5   // 4-bit ADPCM: 0.5 bytes/sample/channel

// You can raise this if you ever want more decode workers.
#define DCFMV_MAX_DECODE_THREADS 8

// ------------------------------
// Clock
// ------------------------------
double dcfmv_ps_ms(void) {
    #define AICA_MEM_CLOCK    0x021000
    #define AICA_TICKS_PER_MS 4.410
    uint32_t j = g2_read_32(SPU_RAM_UNCACHED_BASE + AICA_MEM_CLOCK);
    return (double)j / AICA_TICKS_PER_MS;
}

// ------------------------------
// Chunk index (stride=20)
// ------------------------------
typedef struct {
    uint32_t chunk_offset;
    uint32_t video_section_size;
    uint32_t audio_size;        // bytes per channel
    uint32_t start_frame;
    uint32_t num_frames;
} ChunkEntry;

// ------------------------------
// Chunk cache
// ------------------------------
typedef struct {
    _Atomic int  valid;  // CHUNK_EMPTY=0, CHUNK_LOADING=-1, CHUNK_READY=1
    _Atomic int  refs;
    int          chunk_id;

    uint8_t     *data;
    uint32_t     size;

    uint8_t     *video_section;
    uint32_t     video_bytes_size;
    uint8_t     *audio_L;
    uint8_t     *audio_R;  // NULL for mono

    uint32_t     map_cap;
    uint32_t    *frame_off_local;
    uint32_t    *frame_sz_local;

    uint32_t     seen_cap;
    uint32_t    *seen_u;
    uint32_t    *seen_off;

    uint32_t     last_used;
} ChunkCache;

enum { CHUNK_EMPTY=0, CHUNK_LOADING=-1, CHUNK_READY=1 };

// ------------------------------
// Async IO
// ------------------------------
typedef enum { IO_IDLE=0, IO_LOADING=1 } IOState;

typedef struct {
    IOState state;
    int     chunk_id;
    int     pin0, pin1, pin2;
    ChunkCache *slot;
    uint32_t file_off, total_bytes, progress;
    uint32_t video_real_bytes, video_disk_bytes, audio_disk_bytes;
    uint32_t cur_off;
    int      did_seek;
} ChunkIOJob;

// ------------------------------
// Audio ring
// ------------------------------
typedef struct {
    // MUST be aligned for dcache flush / spu_memload blocks
    uint8_t    *left;
    uint8_t    *right;
    size_t      valid_bytes; // per-channel
    _Atomic int valid;
} AudioSlot;

// ------------------------------
// Decode job queue
// ------------------------------
typedef struct {
    int total_frame, unique_id, buf, generation, decode_seq;
} DecodeJob;

typedef struct {
    struct dcfmv *p;
    int tid;
} DecodeThreadArg;

// ------------------------------
// Context
// ------------------------------
struct dcfmv {
    dcfmv_config_t cfg;
    char *path;

    // file + tables
    file_t     fd;
    DCMVHeader header;
    ChunkEntry *chunk_index;

    uint32_t  *frame_sizes;
    uint16_t  *frame_durations;
    uint16_t  *t2u_lut;

    // compression contexts
    ZSTD_DCtx **zstd_dctx; // [decode_threads]

    // chunk cache
    ChunkCache *chunk_cache;
    int chunk_cache_size;        // allocated slots (same as active slots for now)
    uint32_t global_cache_tick;

    mutex_t file_mutex;
    mutex_t chunk_cache_mutex;

    // async IO
    mutex_t io_mutex;
    ChunkIOJob io_job;
    _Atomic int io_enabled;

    // wake seq (genwait)
    _Atomic uint32_t decode_wake_seq;
    _Atomic uint32_t worker_wake_seq;
    _Atomic uint32_t io_wake_seq;

    // pvr state (texture + compiled poly + verts)
    pvr_ptr_t      pvr_txr;
    pvr_poly_hdr_t poly_hdr;
    pvr_vertex_t   vert[4];
    int            pvr_inited;
    int            pvr_strided;

    // decoded frame buffers
    uint8_t      **frame_buffer;     // [num_buffers]
    _Atomic int   *buf_state;        // [num_buffers]
    _Atomic int   *buf_total_frame;  // [num_buffers]
    _Atomic int   *buf_unique_id;    // [num_buffers]
    int            last_unique_frame_drawn;
    int            pending_free_buf;

    enum BufState { BUF_EMPTY=0, BUF_QUEUED=1, BUF_LOADING=2, BUF_READY=3 } buf_state_var;

    // playback state
    atomic_int  frame_index;
    atomic_int  paused;
    atomic_int  audio_muted;
    _Atomic int playback_started;
    _Atomic double playback_t0_ms;
    double frame_duration_ms;
    _Atomic int seek_request;

    // audio state
    snd_stream_hnd_t stream;
    int snd_inited;

    _Atomic int stream_started;
    AudioSlot   *audio_ring;       // [audio_ring_slots]
    atomic_int   audio_write_idx;
    atomic_int   audio_read_idx;
    _Atomic int  audio_refill_needed;
    size_t       audio_ring_read_pos;

    int          current_audio_chunk;
    size_t       audio_chunk_read_pos;

    // decode queue (SPSC)
    DecodeJob   *decode_q;
    int          decode_q_cap;
    _Atomic int  decode_q_head;
    _Atomic int  decode_q_tail;
    _Atomic int  seek_generation;
    _Atomic int  decode_generation;

    // threads
    int decode_threads;
    kthread_t *th_worker;
    kthread_t *th_io;
    kthread_t **th_decode;        // dynamic array [decode_threads]
    DecodeThreadArg *decode_arg;  // per-thread argument

    _Atomic int seek_paused;
};

// ------------------------------
// Helpers
// ------------------------------
static inline uint32_t align32(uint32_t x)       { return (x + 31u) & ~31u; }
static inline uint32_t pad32_after(uint32_t end) { return (32u - (end & 31u)) & 31u; }
static inline uint32_t frame_comp_size(const dcfmv_t *p, uint32_t u) { return p->frame_sizes[u] & 0x1FFFFFFFu; }

static inline void decode_signal(dcfmv_t *p) {
    atomic_fetch_add_explicit(&p->decode_wake_seq, 1, memory_order_release);
    genwait_wake_one((void*)&p->decode_wake_seq);
}
static inline void worker_signal(dcfmv_t *p) {
    atomic_fetch_add_explicit(&p->worker_wake_seq, 1, memory_order_release);
    genwait_wake_one((void*)&p->worker_wake_seq);
}
static inline void io_signal(dcfmv_t *p) {
    if (atomic_load_explicit(&p->io_enabled, memory_order_acquire)) {
        atomic_fetch_add_explicit(&p->io_wake_seq, 1, memory_order_release);
        genwait_wake_one((void*)&p->io_wake_seq);
    }
}
static inline void wait_on_seq(_Atomic uint32_t *seq_atomic, uint32_t *last_seen, int timeout_ms) {
    while (atomic_load_explicit(seq_atomic, memory_order_acquire) == *last_seen) {
        genwait_wait((void*)seq_atomic, NULL, timeout_ms);
        if (timeout_ms > 0) break;
    }
    *last_seen = atomic_load_explicit(seq_atomic, memory_order_acquire);
}

static long get_file_size(file_t fd) {
    long cur = fs_tell(fd); fs_seek(fd, 0, SEEK_END);
    long sz  = fs_tell(fd); fs_seek(fd, cur, SEEK_SET);
    return sz;
}

const DCMVHeader *dcfmv_header(const dcfmv_t *p) { return p ? &p->header : NULL; }
const char *dcfmv_path(const dcfmv_t *p) { return p ? p->path : NULL; }
int dcfmv_frame_index(const dcfmv_t *p) { return p ? atomic_load(&p->frame_index) : 0; }
int dcfmv_playback_started(const dcfmv_t *p) { return p ? atomic_load_explicit(&p->playback_started, memory_order_acquire) : 0; }
double dcfmv_frame_duration_ms(const dcfmv_t *p) { return p ? p->frame_duration_ms : 0.0; }

// ------------------------------
// Total->unique + chunk lookup
// ------------------------------
static inline int total_to_unique(const dcfmv_t *p, int tf) {
    if (tf < 0) return 0;
    if (tf >= (int)p->header.num_total_frames)
        return (int)(p->header.num_unique_frames ? p->header.num_unique_frames - 1 : 0);
    return (int)p->t2u_lut[tf];
}

static int find_chunk_for_frame(const dcfmv_t *p, int tf) {
    if (!p->chunk_index || !p->header.num_chunks) return 0;
    tf = MAX(0, MIN(tf, (int)p->header.num_total_frames - 1));
    int lo = 0, hi = (int)p->header.num_chunks - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t s = p->chunk_index[mid].start_frame;
        uint32_t n = p->chunk_index[mid].num_frames;
        if ((uint32_t)tf < s)         hi = mid - 1;
        else if ((uint32_t)tf >= s+n) lo = mid + 1;
        else                          return mid;
    }
    return (int)p->header.num_chunks - 1;
}

// ------------------------------
// Chunk cache: acquire/release/evict
// ------------------------------
static ChunkCache *cache_acquire(dcfmv_t *p, int chunk_id) {
    ChunkCache *ret = NULL;
    mutex_lock(&p->chunk_cache_mutex);
    for (int i = 0; i < p->chunk_cache_size; i++) {
        ChunkCache *cc = &p->chunk_cache[i];
        if (!cc->data) continue;
        if (atomic_load_explicit(&cc->valid, memory_order_acquire) == CHUNK_READY &&
            cc->chunk_id == chunk_id) {
            atomic_fetch_add_explicit(&cc->refs, 1, memory_order_acq_rel);
            cc->last_used = ++p->global_cache_tick;
            ret = cc;
            break;
        }
    }
    mutex_unlock(&p->chunk_cache_mutex);
    return ret;
}
static void cache_release(ChunkCache *c) {
    if (c) atomic_fetch_sub_explicit(&c->refs, 1, memory_order_acq_rel);
}

static ChunkCache *evict_slot(dcfmv_t *p, int pin0, int pin1, int pin2) {
    for (int i = 0; i < p->chunk_cache_size; i++) {
        ChunkCache *cc = &p->chunk_cache[i];
        if (cc->data &&
            atomic_load_explicit(&cc->valid, memory_order_relaxed) == CHUNK_EMPTY)
            return cc;
    }
    int lru = -1; uint32_t best = 0xFFFFFFFFu;
    for (int i = 0; i < p->chunk_cache_size; i++) {
        ChunkCache *cc = &p->chunk_cache[i];
        if (!cc->data) continue;
        if (atomic_load_explicit(&cc->valid, memory_order_relaxed) != CHUNK_READY) continue;
        if (atomic_load_explicit(&cc->refs,  memory_order_acquire) > 0) continue;
        int cid = cc->chunk_id;
        if (cid == pin0 || cid == pin1 || cid == pin2) continue;
        if (cc->last_used < best) { best = cc->last_used; lru = i; }
    }
    if (lru < 0) return NULL;
    ChunkCache *cc = &p->chunk_cache[lru];

    atomic_store_explicit(&cc->valid, CHUNK_EMPTY, memory_order_release);
    atomic_store_explicit(&cc->refs,  0,           memory_order_release);
    cc->chunk_id         = -1;
    cc->video_section    = cc->audio_L = cc->audio_R = NULL;
    cc->video_bytes_size = 0;
    cc->last_used        = 0;
    return cc;
}

// ------------------------------
// Per-chunk frame map build
// (decides raw vs aligned packing by comparing computed totals)
// ------------------------------
static void build_chunk_frame_map(dcfmv_t *p, ChunkCache *slot, int chunk_id) {
    ChunkEntry *e = &p->chunk_index[chunk_id];
    uint32_t n    = MIN(e->num_frames, slot->map_cap);
    uint32_t seen_cap = MIN(slot->seen_cap, n);

    uint32_t cur_raw = 0, cur_aln = 0, seen_cnt = 0;
    for (uint32_t j = 0; j < n; j++) {
        int uf = total_to_unique(p, (int)e->start_frame + (int)j);
        uf = MAX(0, MIN(uf, (int)p->header.num_unique_frames - 1));
        uint32_t sz = MAX(1u, frame_comp_size(p, (uint32_t)uf));
        int found = 0;
        if (slot->seen_u)
            for (uint32_t k = 0; k < seen_cnt; k++)
                if (slot->seen_u[k] == (uint32_t)uf) { found = 1; break; }
        if (!found) {
            if (slot->seen_u && seen_cnt < seen_cap) slot->seen_u[seen_cnt++] = (uint32_t)uf;
            cur_raw += sz;
            cur_aln += align32(sz);
        }
    }

    uint32_t vid = slot->video_bytes_size;
    int raw_ok = (cur_raw <= vid + 32), aln_ok = (cur_aln <= vid + 32);
    int use_aligned;
    if      ( raw_ok && !aln_ok) use_aligned = 0;
    else if (!raw_ok &&  aln_ok) use_aligned = 1;
    else {
        uint32_t rd = cur_raw > vid ? cur_raw - vid : vid - cur_raw;
        uint32_t ad = cur_aln > vid ? cur_aln - vid : vid - cur_aln;
        use_aligned = (ad < rd) ? 1 : 0;
    }

    seen_cnt = 0;
    uint32_t cur_off = 0;
    for (uint32_t j = 0; j < n; j++) {
        int uf = total_to_unique(p, (int)e->start_frame + (int)j);
        uf = MAX(0, MIN(uf, (int)p->header.num_unique_frames - 1));
        uint32_t sz  = MAX(1u, frame_comp_size(p, (uint32_t)uf));
        uint32_t off = cur_off;
        int found = 0;
        if (slot->seen_u && slot->seen_off)
            for (uint32_t k = 0; k < seen_cnt; k++)
                if (slot->seen_u[k] == (uint32_t)uf)
                    { off = slot->seen_off[k]; found = 1; break; }
        if (!found) {
            if (slot->seen_u && slot->seen_off && seen_cnt < seen_cap) {
                slot->seen_u[seen_cnt]   = (uint32_t)uf;
                slot->seen_off[seen_cnt] = cur_off;
                seen_cnt++;
            }
            cur_off += use_aligned ? align32(sz) : sz;
        }
        slot->frame_off_local[j] = off;
        slot->frame_sz_local[j]  = sz;
    }
}

// ------------------------------
// Chunk cache allocation sizing
// ------------------------------
static uint32_t max_chunk_disk_bytes(dcfmv_t *p) {
    uint32_t mx = 0;
    for (uint32_t i = 0; i < p->header.num_chunks; i++) {
        ChunkEntry *e = &p->chunk_index[i];
        uint32_t v = e->video_section_size + pad32_after(e->chunk_offset + e->video_section_size);
        uint32_t a = align32(e->audio_size) * (uint32_t)p->header.channels;
        mx = MAX(mx, v + a);
    }
    return align32(mx + 64);
}

static uint32_t max_frames_per_chunk(dcfmv_t *p) {
    uint32_t n = (uint32_t)((double)p->header.fps * (double)p->header.chunk_duration + 8.0);
    return MAX(n, 8u);
}

static int init_chunk_cache(dcfmv_t *p) {
    uint32_t max_chunk = max_chunk_disk_bytes(p);
    uint32_t map_cap   = max_frames_per_chunk(p);

    if (p->cfg.verbose) {
        printf("[cache] max chunk bytes (disk)=%u slots=%d map_cap=%u\n",
               (unsigned)max_chunk, p->chunk_cache_size, (unsigned)map_cap);
    }

    for (int i = 0; i < p->chunk_cache_size; i++) {
        ChunkCache *cc = &p->chunk_cache[i];

        cc->data = (uint8_t *)memalign(32, max_chunk);
        if (!cc->data) { printf("❌ [cache] memalign slot %d (%u)\n", i, (unsigned)max_chunk); return -1; }

        cc->frame_off_local = (uint32_t *)malloc(map_cap * 4);
        cc->frame_sz_local  = (uint32_t *)malloc(map_cap * 4);
        if (!cc->frame_off_local || !cc->frame_sz_local) { printf("❌ [cache] map alloc slot %d\n", i); return -1; }

        uint32_t want_seen = MAX(map_cap, 8u);
        cc->seen_u   = (uint32_t *)malloc(want_seen * 4);
        cc->seen_off = (uint32_t *)malloc(want_seen * 4);
        if (!cc->seen_u || !cc->seen_off) { printf("❌ [cache] seen alloc slot %d\n", i); return -1; }

        cc->seen_cap  = want_seen;
        cc->size      = max_chunk;
        cc->map_cap   = map_cap;
        cc->chunk_id  = -1;
        cc->last_used = 0;
        cc->video_section = cc->audio_L = cc->audio_R = NULL;
        cc->video_bytes_size = 0;

        atomic_store(&cc->valid, CHUNK_EMPTY);
        atomic_store(&cc->refs,  0);
    }

    return 0;
}

// ------------------------------
// Chunk finalise (after IO load)
// ------------------------------
static void chunk_finalise(dcfmv_t *p, ChunkCache *slot, int chunk_id,
                           uint32_t video_real, uint32_t video_disk) {
    ChunkEntry *e          = &p->chunk_index[chunk_id];
    slot->chunk_id         = chunk_id;
    slot->video_section    = slot->data;
    slot->video_bytes_size = video_real;

    uint32_t aud_disk_per_ch = align32(e->audio_size);

    slot->audio_L          = slot->data + video_disk;
    slot->audio_R          = (p->header.channels == 2) ? (slot->audio_L + aud_disk_per_ch) : NULL;

    slot->last_used        = ++p->global_cache_tick;
    build_chunk_frame_map(p, slot, chunk_id);
    atomic_store_explicit(&slot->valid, CHUNK_READY, memory_order_release);

    worker_signal(p);
}

// ------------------------------
// Async IO: request/pump/thread
// ------------------------------
static int is_cached_or_loading(dcfmv_t *p, int chunk_id) {
    mutex_lock(&p->chunk_cache_mutex);
    for (int i = 0; i < p->chunk_cache_size; i++) {
        ChunkCache *cc = &p->chunk_cache[i];
        if (!cc->data) continue;
        if (cc->chunk_id != chunk_id) continue;
        int v = atomic_load_explicit(&cc->valid, memory_order_acquire);
        if (v == CHUNK_READY || v == CHUNK_LOADING) { mutex_unlock(&p->chunk_cache_mutex); return 1; }
    }
    mutex_unlock(&p->chunk_cache_mutex);

    mutex_lock(&p->io_mutex);
    int loading = (p->io_job.state == IO_LOADING && p->io_job.chunk_id == chunk_id);
    mutex_unlock(&p->io_mutex);
    return loading;
}

static int request_chunk_async(dcfmv_t *p, int chunk_id, int pin0, int pin1, int pin2) {
    if (chunk_id < 0 || (uint32_t)chunk_id >= p->header.num_chunks) return -1;

    // Already cached?
    mutex_lock(&p->chunk_cache_mutex);
    for (int i = 0; i < p->chunk_cache_size; i++) {
        ChunkCache *cc = &p->chunk_cache[i];
        if (!cc->data) continue;
        if (atomic_load_explicit(&cc->valid, memory_order_acquire) == CHUNK_READY &&
            cc->chunk_id == chunk_id) {
            cc->last_used = ++p->global_cache_tick;
            mutex_unlock(&p->chunk_cache_mutex);
            return 0;
        }
    }
    mutex_unlock(&p->chunk_cache_mutex);

    mutex_lock(&p->io_mutex);
    if (p->io_job.state == IO_LOADING && p->io_job.chunk_id == chunk_id) { mutex_unlock(&p->io_mutex); return 0; }
    if (p->io_job.state != IO_IDLE) { mutex_unlock(&p->io_mutex); return 0; }

    mutex_lock(&p->chunk_cache_mutex);
    ChunkCache *slot = evict_slot(p, pin0, pin1, pin2);
    if (!slot) { mutex_unlock(&p->chunk_cache_mutex); mutex_unlock(&p->io_mutex); return -1; }

    slot->chunk_id = chunk_id;
    atomic_store_explicit(&slot->valid, CHUNK_LOADING, memory_order_release);
    atomic_store_explicit(&slot->refs,  0,             memory_order_release);
    slot->video_section = slot->audio_L = slot->audio_R = NULL;
    slot->video_bytes_size = 0;
    mutex_unlock(&p->chunk_cache_mutex);

    ChunkEntry *e     = &p->chunk_index[chunk_id];
    uint32_t vid_real = e->video_section_size;
    uint32_t vid_disk = vid_real + pad32_after(e->chunk_offset + vid_real);
    uint32_t aud_disk = align32(e->audio_size) * p->header.channels;
    uint32_t total    = vid_disk + aud_disk;

    if (total > slot->size) {
        printf("❌ [io] chunk %d needs %lu, slot has %lu\n", chunk_id, total, slot->size);
        mutex_lock(&p->chunk_cache_mutex);
        atomic_store_explicit(&slot->valid, CHUNK_EMPTY, memory_order_release);
        slot->chunk_id = -1;
        mutex_unlock(&p->chunk_cache_mutex);
        mutex_unlock(&p->io_mutex);
        return -1;
    }

    p->io_job = (ChunkIOJob){
        .state = IO_LOADING, .chunk_id = chunk_id,
        .pin0 = pin0, .pin1 = pin1, .pin2 = pin2, .slot = slot,
        .file_off = e->chunk_offset, .total_bytes = total,
        .progress = 0,
        .video_real_bytes = vid_real, .video_disk_bytes = vid_disk, .audio_disk_bytes = aud_disk,
        .cur_off = e->chunk_offset,
        .did_seek = 0,
    };
    mutex_unlock(&p->io_mutex);

    io_signal(p);
    return 0;
}

static void prefetch_around(dcfmv_t *p, int play_chunk) {
    int aud  = p->current_audio_chunk;
    int pin0 = aud, pin1 = play_chunk, pin2 = play_chunk + 1;

    int want[10];
    int n = 0;

    if ((uint32_t)aud < p->header.num_chunks) want[n++] = aud;
    if ((uint32_t)(aud + 1) < p->header.num_chunks) want[n++] = aud + 1;

    int runway = MAX(1, p->chunk_cache_size - 2);
    for (int k = 0; k <= runway && n < (int)(sizeof(want)/sizeof(want[0])); k++) {
        int c = play_chunk + k;
        if ((uint32_t)c < p->header.num_chunks) want[n++] = c;
    }

    for (int i = 0; i < n; i++) {
        if (!is_cached_or_loading(p, want[i]))
            request_chunk_async(p, want[i], pin0, pin1, pin2);
    }
}

static void io_pump_slice(dcfmv_t *p) {
    mutex_lock(&p->io_mutex);
    if (p->io_job.state != IO_LOADING) { mutex_unlock(&p->io_mutex); return; }
    ChunkIOJob job = p->io_job;
    mutex_unlock(&p->io_mutex);

    uint32_t remaining = job.total_bytes - job.progress;
    if (!remaining) return;

    uint32_t slice = MIN(remaining, (uint32_t)p->cfg.chunk_io_slice_bytes);
    if (slice >= 32) slice &= ~31u;

    mutex_lock(&p->file_mutex);
    if (!job.did_seek) {
        fs_seek(p->fd, job.cur_off, SEEK_SET);
        job.did_seek = 1;
    }
    ssize_t got = fs_read(p->fd, job.slot->data + job.progress, slice);
    mutex_unlock(&p->file_mutex);

    if (got != (ssize_t)slice) {
        printf("❌ [io] read chunk=%d got=%d exp=%lu\n", job.chunk_id, (int)got, slice);
        mutex_lock(&p->io_mutex);  p->io_job.state = IO_IDLE;  mutex_unlock(&p->io_mutex);
        mutex_lock(&p->chunk_cache_mutex);
        atomic_store_explicit(&job.slot->valid, CHUNK_EMPTY, memory_order_release);
        job.slot->chunk_id = -1;
        mutex_unlock(&p->chunk_cache_mutex);
        return;
    }

    job.progress += slice;
    job.cur_off  += slice;

    mutex_lock(&p->io_mutex);
    if (p->io_job.state == IO_LOADING && p->io_job.chunk_id == job.chunk_id) {
        p->io_job.progress = job.progress;
        p->io_job.cur_off  = job.cur_off;
        p->io_job.did_seek = job.did_seek;

        if (p->io_job.progress >= p->io_job.total_bytes) {
            mutex_lock(&p->chunk_cache_mutex);
            chunk_finalise(p, p->io_job.slot, p->io_job.chunk_id,
                           p->io_job.video_real_bytes, p->io_job.video_disk_bytes);
            mutex_unlock(&p->chunk_cache_mutex);
            p->io_job.state = IO_IDLE;
        }
    }
    mutex_unlock(&p->io_mutex);
}

static void *io_thread_fn(void *arg) {
    dcfmv_t *p = (dcfmv_t *)arg;

    uint32_t last_io_seq = atomic_load_explicit(&p->io_wake_seq, memory_order_acquire);

    while (1) {
        if (atomic_load_explicit(&p->seek_paused, memory_order_acquire)) {
            thd_sleep(1);
            continue;
        }
        if (!atomic_load_explicit(&p->io_enabled, memory_order_acquire)) {
            thd_pass();
            continue;
        }

        mutex_lock(&p->io_mutex);
        int busy = (p->io_job.state != IO_IDLE);
        mutex_unlock(&p->io_mutex);

        if (!busy) {
            wait_on_seq(&p->io_wake_seq, &last_io_seq, 0);
            continue;
        }

        io_pump_slice(p);
        thd_pass();
    }
    return NULL;
}

static void io_cancel_job(dcfmv_t *p) {
    mutex_lock(&p->io_mutex);
    if (p->io_job.state == IO_LOADING) {
        ChunkCache *slot = p->io_job.slot;

        mutex_lock(&p->chunk_cache_mutex);
        if (slot) {
            atomic_store_explicit(&slot->valid, CHUNK_EMPTY, memory_order_release);
            slot->chunk_id = -1;
            slot->video_section = slot->audio_L = slot->audio_R = NULL;
            slot->video_bytes_size = 0;
        }
        mutex_unlock(&p->chunk_cache_mutex);

        p->io_job.state = IO_IDLE;
    }
    mutex_unlock(&p->io_mutex);
}

// ------------------------------
// Sync chunk load (boot)
// ------------------------------
static int load_chunk_sync(dcfmv_t *p, int chunk_id, int pin0, int pin1, int pin2) {
    if (chunk_id < 0 || (uint32_t)chunk_id >= p->header.num_chunks) return -1;

    mutex_lock(&p->chunk_cache_mutex);
    for (int i = 0; i < p->chunk_cache_size; i++) {
        ChunkCache *cc = &p->chunk_cache[i];
        if (!cc->data) continue;
        if (atomic_load_explicit(&cc->valid, memory_order_acquire) == CHUNK_READY &&
            cc->chunk_id == chunk_id) {
            cc->last_used = ++p->global_cache_tick;
            mutex_unlock(&p->chunk_cache_mutex);
            return 0;
        }
    }

    ChunkCache *slot = evict_slot(p, pin0, pin1, pin2);
    if (!slot) { mutex_unlock(&p->chunk_cache_mutex); return -1; }

    slot->chunk_id = chunk_id;
    slot->video_section = slot->audio_L = slot->audio_R = NULL;
    slot->video_bytes_size = 0;

    atomic_store_explicit(&slot->refs,  0,             memory_order_release);
    atomic_store_explicit(&slot->valid, CHUNK_LOADING, memory_order_release);

    mutex_unlock(&p->chunk_cache_mutex);

    ChunkEntry *e     = &p->chunk_index[chunk_id];
    uint32_t vid_real = e->video_section_size;
    uint32_t vid_disk = vid_real + pad32_after(e->chunk_offset + vid_real);
    uint32_t aud_disk = align32(e->audio_size) * p->header.channels;
    uint32_t total    = vid_disk + aud_disk;

    if (total > slot->size) {
        printf("❌ [boot] chunk %d needs %lu, slot has %lu\n", chunk_id, total, slot->size);
        mutex_lock(&p->chunk_cache_mutex);
        atomic_store_explicit(&slot->valid, CHUNK_EMPTY, memory_order_release);
        slot->chunk_id = -1;
        mutex_unlock(&p->chunk_cache_mutex);
        return -1;
    }

    mutex_lock(&p->file_mutex);
    fs_seek(p->fd, e->chunk_offset, SEEK_SET);
    uint8_t *dst = slot->data; uint32_t left = total;
    while (left) {
        uint32_t n = MIN(left, 32768u);
        ssize_t got = fs_read(p->fd, dst, n);
        if (got <= 0) {
            mutex_unlock(&p->file_mutex);
            mutex_lock(&p->chunk_cache_mutex);
            atomic_store_explicit(&slot->valid, CHUNK_EMPTY, memory_order_release);
            slot->chunk_id = -1;
            mutex_unlock(&p->chunk_cache_mutex);
            return -1;
        }
        dst += got; left -= (uint32_t)got;
        thd_pass();
    }
    mutex_unlock(&p->file_mutex);

    mutex_lock(&p->chunk_cache_mutex);
    chunk_finalise(p, slot, chunk_id, vid_real, vid_disk);
    mutex_unlock(&p->chunk_cache_mutex);

    return 0;
}

// static int cache_is_ready(dcfmv_t *p, int chunk_id) {
//     int ok = 0;
//     mutex_lock(&p->chunk_cache_mutex);
//     for (int i = 0; i < p->chunk_cache_size; i++) {
//         ChunkCache *cc = &p->chunk_cache[i];
//         if (!cc->data) continue;
//         if (cc->chunk_id != chunk_id) continue;
//         if (atomic_load_explicit(&cc->valid, memory_order_acquire) == CHUNK_READY) {
//             ok = 1;
//             break;
//         }
//     }
//     mutex_unlock(&p->chunk_cache_mutex);
//     return ok;
// }

// // Pump IO until a particular chunk becomes READY (or timeout).
// static int ensure_chunk_ready(dcfmv_t *p, int chunk_id, int pin0, int pin1, int pin2, int timeout_ms) {
//     if (chunk_id < 0 || (uint32_t)chunk_id >= p->header.num_chunks) return -1;

//     if (cache_is_ready(p, chunk_id)) return 0;

//     if (!is_cached_or_loading(p, chunk_id)) {
//         request_chunk_async(p, chunk_id, pin0, pin1, pin2);
//         io_signal(p);
//     }

//     if (!atomic_load_explicit(&p->io_enabled, memory_order_acquire)) {
//         return load_chunk_sync(p, chunk_id, pin0, pin1, pin2);
//     }

//     double start = dcfmv_ps_ms();
//     while (!cache_is_ready(p, chunk_id)) {
//         io_pump_slice(p);
//         thd_pass();

//         if (timeout_ms > 0) {
//             double now = dcfmv_ps_ms();
//             if ((now - start) >= (double)timeout_ms) break;
//         }
//     }

//     return cache_is_ready(p, chunk_id) ? 0 : -1;
// }

// ------------------------------
// Audio
// ------------------------------
static inline double audio_entry_ms(dcfmv_t *p) {
    return (1000.0 * (double)p->cfg.audio_buffer_bytes) /
           ((double)p->header.sample_rate * ADPCM_BYTES_PER_SAMPLE);
}
static inline int target_audio_buffers(dcfmv_t *p) {
    double want_ms = p->cfg.target_audio_ms;
    int n = (int)((want_ms + audio_entry_ms(p) - 1.0) / audio_entry_ms(p));
    return MAX(3, MIN(n, p->cfg.audio_ring_slots - 2));
}

static void write_silence(uintptr_t dst, size_t bytes) {
    static uint8_t silence[4096] __attribute__((aligned(32)));
    static _Atomic int flushed_once = 0;

    bytes &= ~31u;
    if (!bytes) return;

    if (!atomic_load_explicit(&flushed_once, memory_order_acquire)) {
        dcache_flush_range((uint32)silence, (uint32)sizeof(silence));
        atomic_store_explicit(&flushed_once, 1, memory_order_release);
    }

    while (bytes) {
        size_t n = MIN(bytes, sizeof(silence)) & ~31u;
        if (!n) break;
        spu_memload(dst, silence, n);
        dst += n;
        bytes -= n;
    }
}

static void audio_seek_to_frame(dcfmv_t *p, int total_frame) {
    int cid = find_chunk_for_frame(p, total_frame);
    ChunkEntry *e = &p->chunk_index[cid];

    int in_chunk_frames = total_frame - (int)e->start_frame;
    if (in_chunk_frames < 0) in_chunk_frames = 0;
    if ((uint32_t)in_chunk_frames > e->num_frames) in_chunk_frames = (int)e->num_frames;

    uint32_t pos = 0;
    if (e->num_frames) {
        pos = (uint32_t)(((uint64_t)in_chunk_frames * (uint64_t)e->audio_size) /
                         (uint64_t)e->num_frames);
    }
    pos &= ~31u;
    if (pos > e->audio_size) pos = e->audio_size;

    p->current_audio_chunk  = cid;
    p->audio_chunk_read_pos = pos;

    if (p->audio_chunk_read_pos >= e->audio_size && (uint32_t)(cid + 1) < p->header.num_chunks) {
        p->current_audio_chunk++;
        p->audio_chunk_read_pos = 0;
    }
}

static void audio_ring_clear(dcfmv_t *p) {
    p->audio_ring_read_pos = 0;
    atomic_store_explicit(&p->audio_read_idx,      0, memory_order_release);
    atomic_store_explicit(&p->audio_write_idx,     0, memory_order_release);
    atomic_store_explicit(&p->audio_refill_needed, 0, memory_order_release);
    for (int i = 0; i < p->cfg.audio_ring_slots; i++) {
        atomic_store_explicit(&p->audio_ring[i].valid, 0, memory_order_release);
        p->audio_ring[i].valid_bytes = 0;
    }
}

static void refill_audio_ring(dcfmv_t *p) {
    const size_t WANT = ((size_t)p->cfg.audio_buffer_bytes) & ~31u;

    int wi  = atomic_load_explicit(&p->audio_write_idx, memory_order_acquire);
    int ri  = atomic_load_explicit(&p->audio_read_idx,  memory_order_acquire);
    int fill = (wi - ri + p->cfg.audio_ring_slots) % p->cfg.audio_ring_slots;
    int tgt  = target_audio_buffers(p);

    atomic_store_explicit(&p->audio_refill_needed, 0, memory_order_release);

    int tf = atomic_load_explicit(&p->frame_index, memory_order_acquire);
    int pc = find_chunk_for_frame(p, tf);

    while (fill < tgt) {
        int next = (wi + 1) % p->cfg.audio_ring_slots;
        if (next == ri) break;
        if ((uint32_t)p->current_audio_chunk >= p->header.num_chunks) break;

        memset(p->audio_ring[wi].left, 0, p->cfg.audio_buffer_bytes);
        if (p->header.channels == 2) memset(p->audio_ring[wi].right, 0, p->cfg.audio_buffer_bytes);

        size_t done = 0;

        while (done < WANT) {
            if ((uint32_t)p->current_audio_chunk >= p->header.num_chunks) break;

            if (!is_cached_or_loading(p, p->current_audio_chunk))
                request_chunk_async(p, p->current_audio_chunk, p->current_audio_chunk, pc, pc + 1);
            if ((uint32_t)(p->current_audio_chunk + 1) < p->header.num_chunks &&
                !is_cached_or_loading(p, p->current_audio_chunk + 1))
                request_chunk_async(p, p->current_audio_chunk + 1, p->current_audio_chunk, pc, pc + 1);

            ChunkCache *c = cache_acquire(p, p->current_audio_chunk);
            if (!c) break;

            ChunkEntry *e = &p->chunk_index[p->current_audio_chunk];

            if (p->audio_chunk_read_pos >= e->audio_size) {
                cache_release(c);
                p->current_audio_chunk++;
                p->audio_chunk_read_pos = 0;
                continue;
            }

            size_t rem = (size_t)e->audio_size - p->audio_chunk_read_pos;

            if (rem < 32) {
                cache_release(c);
                p->current_audio_chunk++;
                p->audio_chunk_read_pos = 0;
                continue;
            }

            size_t need = WANT - done;
            size_t take = (rem < need) ? rem : need;
            take &= ~31u;

            if (!take) {
                cache_release(c);
                break;
            }

            memcpy(p->audio_ring[wi].left + done, c->audio_L + p->audio_chunk_read_pos, take);
            if (p->header.channels == 2)
                memcpy(p->audio_ring[wi].right + done, c->audio_R + p->audio_chunk_read_pos, take);

            p->audio_chunk_read_pos += take;
            done += take;

            if (p->audio_chunk_read_pos >= e->audio_size) {
                cache_release(c);
                p->current_audio_chunk++;
                p->audio_chunk_read_pos = 0;
                continue;
            }

            if ((uint32_t)(p->current_audio_chunk + 1) < p->header.num_chunks) {
                size_t left_in_chunk = (size_t)e->audio_size - p->audio_chunk_read_pos;
                if (left_in_chunk < (WANT * 3)) {
                    request_chunk_async(p, p->current_audio_chunk + 1,
                                        p->current_audio_chunk, pc, pc + 1);
                }
            }

            cache_release(c);
        }

        done &= ~31u;
        if (done < 32) break;

        p->audio_ring[wi].valid_bytes = done;

        dcache_flush_range((uint32)p->audio_ring[wi].left,  (uint32)done);
        if (p->header.channels == 2)
            dcache_flush_range((uint32)p->audio_ring[wi].right, (uint32)done);

        atomic_store_explicit(&p->audio_ring[wi].valid, 1, memory_order_release);

        wi = next;
        atomic_store_explicit(&p->audio_write_idx, wi, memory_order_release);

        ri   = atomic_load_explicit(&p->audio_read_idx, memory_order_acquire);
        fill = (wi - ri + p->cfg.audio_ring_slots) % p->cfg.audio_ring_slots;
    }
}

// ---- Audio callback binding (avoid snd_stream userdata dependency) ----
static dcfmv_t *g_audio_owner = NULL;

static size_t audio_cb(snd_stream_hnd_t hnd, uintptr_t l, uintptr_t r, size_t req) {
    (void)hnd;

    dcfmv_t *p = g_audio_owner;
    if (!p) return 0;

    int ch = (int)p->header.channels;
    if (ch != 1 && ch != 2) return 0;

    // KOS direct ADPCM callback: req is typically TOTAL bytes (stereo => includes both channels).
    // Enforce 32-byte alignment for spu_memload.
    size_t req_total = req & ~31u;
    if (!req_total) return 0;

    size_t per_chan = (ch == 2) ? ((req_total / 2) & ~31u) : req_total;
    if (!per_chan) return 0;

    if (atomic_load_explicit(&p->audio_muted, memory_order_acquire)) {
        write_silence(l, per_chan);
        if (ch == 2) write_silence(r, per_chan);
        return (ch == 2) ? (per_chan * 2) : per_chan;
    }

    size_t pos    = p->audio_ring_read_pos;
    size_t remain = per_chan;
    size_t copied = 0;

    while (remain) {
        int ri = atomic_load_explicit(&p->audio_read_idx, memory_order_acquire);

        if (!atomic_load_explicit(&p->audio_ring[ri].valid, memory_order_acquire)) {
            atomic_store_explicit(&p->audio_refill_needed, 1, memory_order_release);
            worker_signal(p);

            write_silence(l + copied, remain);
            if (ch == 2) write_silence(r + copied, remain);

            copied += remain;
            remain = 0;
            break;
        }

        size_t valid = p->audio_ring[ri].valid_bytes; // bytes PER CHANNEL

        if (pos >= valid) {
            atomic_store_explicit(&p->audio_ring[ri].valid, 0, memory_order_release);
            atomic_store_explicit(&p->audio_read_idx,
                                  (ri + 1) % p->cfg.audio_ring_slots,
                                  memory_order_release);
            pos = 0;
            continue;
        }

        size_t to_copy = MIN(valid - pos, remain) & ~31u;

        if (to_copy < 32) {
            write_silence(l + copied, remain);
            if (ch == 2) write_silence(r + copied, remain);
            copied += remain;
            remain = 0;
            break;
        }

        // IMPORTANT:
        // Do NOT dcache_flush_range() here. refill_audio_ring() already flushed the slot ranges.
        spu_memload(l + copied, p->audio_ring[ri].left + pos, to_copy);
        if (ch == 2) {
            spu_memload(r + copied, p->audio_ring[ri].right + pos, to_copy);
        }

        pos    += to_copy;
        copied += to_copy;
        remain -= to_copy;

        if (pos >= valid) {
            atomic_store_explicit(&p->audio_ring[ri].valid, 0, memory_order_release);
            atomic_store_explicit(&p->audio_read_idx,
                                  (ri + 1) % p->cfg.audio_ring_slots,
                                  memory_order_release);
            pos = 0;
        }
    }

    p->audio_ring_read_pos = pos;

    return (ch == 2) ? (copied * 2) : copied;
}

static inline void audio_poll_safe(dcfmv_t *p) {
    if (!p || !p->snd_inited) return;
    if (!atomic_load_explicit(&p->stream_started, memory_order_acquire)) return;
    snd_stream_poll(p->stream);
}

static inline void audio_start_if_needed(dcfmv_t *p) {
    if (!p || !p->snd_inited) return;

    int started = atomic_load_explicit(&p->stream_started, memory_order_acquire);
    if (started) return;

    snd_stream_start_adpcm(p->stream, p->header.sample_rate,
                           p->header.channels == 2 ? 1 : 0);

    atomic_store_explicit(&p->stream_started, 1, memory_order_release);
    thd_pass();
}

void dcfmv_audio_poll(dcfmv_t *p) {
    audio_poll_safe(p);
}

// ------------------------------
// Decode queue
// ------------------------------
static inline int q_inc(dcfmv_t *p, int x) { return (x + 1) % p->decode_q_cap; }

static int decode_q_push(dcfmv_t *p, const DecodeJob *j) {
    int head = atomic_load_explicit(&p->decode_q_head, memory_order_relaxed);
    int next = q_inc(p, head);
    if (next == atomic_load_explicit(&p->decode_q_tail, memory_order_acquire)) return 0;
    p->decode_q[head] = *j;
    atomic_store_explicit(&p->decode_q_head, next, memory_order_release);
    decode_signal(p);
    return 1;
}

static int decode_q_pop(dcfmv_t *p, DecodeJob *out) {
    for (;;) {
        int tail = atomic_load_explicit(&p->decode_q_tail, memory_order_relaxed);
        int head = atomic_load_explicit(&p->decode_q_head, memory_order_acquire);
        if (tail == head) return 0;

        DecodeJob j = p->decode_q[tail];
        int next = q_inc(p, tail);

        if (atomic_compare_exchange_weak_explicit(
                &p->decode_q_tail,
                &tail,
                next,
                memory_order_acq_rel,
                memory_order_relaxed)) {
            *out = j;
            return 1;
        }
    }
}

static void decode_q_flush(dcfmv_t *p) {
    atomic_store_explicit(&p->decode_q_tail,
        atomic_load_explicit(&p->decode_q_head, memory_order_acquire),
        memory_order_release);
}

// ------------------------------
// Frame decompress
// ------------------------------
static int load_frame(dcfmv_t *p, int total_frame, int buf_index, int tid) {
    int chunk_id = find_chunk_for_frame(p, total_frame);
    ChunkCache *c = cache_acquire(p, chunk_id);
    if (!c) {
        int pc = find_chunk_for_frame(p, atomic_load(&p->frame_index));
        request_chunk_async(p, chunk_id, pc, pc+1, pc+2);
        return -1;
    }

    ChunkEntry *e = &p->chunk_index[chunk_id];
    int local = total_frame - (int)e->start_frame;
    if (local < 0 || (uint32_t)local >= e->num_frames || (uint32_t)local >= c->map_cap) {
        printf("❌ frame %d not in chunk %d\n", total_frame, chunk_id);
        cache_release(c); return -1;
    }

    uint32_t off = c->frame_off_local[local];
    uint32_t sz  = c->frame_sz_local[local];
    if (off + sz > c->video_bytes_size) {
        printf("❌ frame %d overflows chunk %d (off=%lu sz=%lu vid=%lu)\n",
               total_frame, chunk_id, off, sz, c->video_bytes_size);
        cache_release(c); return -1;
    }

    const uint8_t *src = c->video_section + off;
    uint8_t       *dst = p->frame_buffer[buf_index];
    int ok = 0;

    if (p->header.compression_type == 1) {
        ZSTD_DCtx *dctx = (tid >= 0 && tid < p->decode_threads) ? p->zstd_dctx[tid] : NULL;
        if (!dctx) {
            printf("❌ ZSTD no dctx tf=%d tid=%d\n", total_frame, tid);
            ok = -1;
        } else {
            ZSTD_DCtx_reset(dctx, ZSTD_reset_session_only);
            ZSTD_DCtx_setParameter(dctx, ZSTD_d_format, ZSTD_f_zstd1_magicless);

            ZSTD_inBuffer  in  = { src, (size_t)sz, 0 };
            ZSTD_outBuffer out = { dst, (size_t)p->header.uncompressed_frame_size, 0 };

            size_t ret = 1;
            while (ret != 0 && out.pos < out.size) {
                ret = ZSTD_decompressStream(dctx, &out, &in);
                if (ZSTD_isError(ret)) {
                    printf("❌ ZSTD decompress tf=%d: %s\n", total_frame, ZSTD_getErrorName(ret));
                    ok = -1;
                    break;
                }
            }

            if (ok == 0 && out.pos != (size_t)p->header.uncompressed_frame_size) {
                printf("❌ ZSTD tf=%d decoded %zu/%u\n",
                    total_frame, out.pos, (unsigned)p->header.uncompressed_frame_size);
                ok = -1;
            }
        }
    } else {
        int out = LZ4_decompress_safe((const char *)src, (char *)dst,
                                      (int)sz, (int)p->header.uncompressed_frame_size);
        if (out != (int)p->header.uncompressed_frame_size) {
            printf("❌ LZ4 frame %d out=%d exp=%lu\n",
                   total_frame, out, p->header.uncompressed_frame_size);
            ok = -1;
        }
    }

    cache_release(c);
    return ok;
}

// ------------------------------
// Decode scheduling
// ------------------------------
static int schedule_decode(dcfmv_t *p, int total_frame) {
    if (total_frame < 0 || total_frame >= (int)p->header.num_total_frames) return 0;

    int uid = total_to_unique(p, total_frame);
    int buf = uid % p->cfg.num_buffers;
    int gen = atomic_load_explicit(&p->seek_generation, memory_order_acquire);
    int dec = atomic_fetch_add_explicit(&p->decode_generation, 1, memory_order_acq_rel);
    int st  = atomic_load_explicit(&p->buf_state[buf],     memory_order_acquire);
    int bu  = atomic_load_explicit(&p->buf_unique_id[buf], memory_order_acquire);

    if ((st == BUF_READY || st == BUF_LOADING || st == BUF_QUEUED) && bu == uid) return 0;

    if (st == BUF_READY && bu != uid) {
        int old_tf = atomic_load_explicit(&p->buf_total_frame[buf], memory_order_acquire);
        if (old_tf >= 0 && old_tf < total_frame) {
            atomic_store_explicit(&p->buf_state[buf], BUF_EMPTY, memory_order_release);
            st = BUF_EMPTY;
        }
    }

    int expected = BUF_EMPTY;
    if (!atomic_compare_exchange_strong(&p->buf_state[buf], &expected, BUF_QUEUED)) return 0;

    atomic_store_explicit(&p->buf_total_frame[buf], total_frame, memory_order_release);
    atomic_store_explicit(&p->buf_unique_id[buf],   uid,         memory_order_release);

    DecodeJob j = { total_frame, uid, buf, gen, dec };
    if (!decode_q_push(p, &j)) {
        atomic_store_explicit(&p->buf_state[buf], BUF_EMPTY, memory_order_release);
        return 0;
    }
    return 1;
}

static void decode_reset(dcfmv_t *p) {
    atomic_fetch_add_explicit(&p->seek_generation, 1, memory_order_acq_rel);
    atomic_fetch_add_explicit(&p->decode_generation, 1, memory_order_acq_rel);
    decode_q_flush(p);
    for (int i = 0; i < p->cfg.num_buffers; i++) {
        atomic_store_explicit(&p->buf_state[i],       BUF_EMPTY, memory_order_release);
        atomic_store_explicit(&p->buf_total_frame[i], -1,        memory_order_release);
        atomic_store_explicit(&p->buf_unique_id[i],   -1,        memory_order_release);
    }
    p->pending_free_buf = p->last_unique_frame_drawn = -1;
}

static void *decode_thread_fn(void *arg) {
    DecodeThreadArg *a = (DecodeThreadArg *)arg;
    dcfmv_t *p = a ? a->p : NULL;
    int tid = a ? a->tid : 0;
    if (!p) return NULL;

    uint32_t last_seq = atomic_load_explicit(&p->decode_wake_seq, memory_order_acquire);

    while (1) {
        if (atomic_load_explicit(&p->seek_paused, memory_order_acquire)) {
            thd_sleep(1);
            continue;
        }

        int did_work = 0;

        for (int n = 0; n < 8; n++) {
            DecodeJob job;
            if (!decode_q_pop(p, &job)) break;
            did_work = 1;

            int cur_gen = atomic_load_explicit(&p->seek_generation, memory_order_acquire);
            if (job.generation != cur_gen) {
                if (atomic_load_explicit(&p->buf_total_frame[job.buf], memory_order_acquire) == job.total_frame)
                    atomic_store_explicit(&p->buf_state[job.buf], BUF_EMPTY, memory_order_release);
                continue;
            }
            if (atomic_load_explicit(&p->buf_total_frame[job.buf], memory_order_acquire) != job.total_frame ||
                atomic_load_explicit(&p->buf_unique_id[job.buf],   memory_order_acquire) != job.unique_id) {
                atomic_store_explicit(&p->buf_state[job.buf], BUF_EMPTY, memory_order_release);
                continue;
            }
            int expected = BUF_QUEUED;
            if (!atomic_compare_exchange_strong(&p->buf_state[job.buf], &expected, BUF_LOADING)) continue;

            if (p->cfg.verbose) {
                int cur = atomic_load_explicit(&p->frame_index, memory_order_acquire);
                const char *rel =
                    (job.total_frame < cur) ? "behind" :
                    (job.total_frame > cur) ? "ahead " :
                                             "equal ";

                printf("[dec q=%d sg=%d tid=%d] tf=%d (%s cur=%d) uid=%d buf=%d\n",
                       job.decode_seq,
                       job.generation,
                       (int)thd_get_id(NULL),
                       job.total_frame, rel, cur,
                       job.unique_id, job.buf);
            }

            int ok = load_frame(p, job.total_frame, job.buf, tid);
            cur_gen = atomic_load_explicit(&p->seek_generation, memory_order_acquire);

            if (ok == 0 && job.generation == cur_gen &&
                atomic_load_explicit(&p->buf_total_frame[job.buf], memory_order_acquire) == job.total_frame &&
                atomic_load_explicit(&p->buf_unique_id[job.buf],   memory_order_acquire) == job.unique_id) {
                atomic_store_explicit(&p->buf_state[job.buf], BUF_READY, memory_order_release);
            } else {
                atomic_store_explicit(&p->buf_state[job.buf], BUF_EMPTY, memory_order_release);
            }
        }

        if (did_work) {
            thd_pass();
        } else {
            wait_on_seq(&p->decode_wake_seq, &last_seq, 0);
        }
    }
    return NULL;
}

// ------------------------------
// Worker thread: audio refill + prefetch
// ------------------------------
static void *worker_thread_fn(void *arg) {
    dcfmv_t *p = (dcfmv_t *)arg;

    int last_prefetch_chunk = -2;
    uint32_t last_worker_seq = atomic_load_explicit(&p->worker_wake_seq, memory_order_acquire);

    while (1) {
        if (atomic_load_explicit(&p->seek_paused, memory_order_acquire)) {
            thd_sleep(1);
            continue;
        }

        audio_poll_safe(p);

        // audio refill
        {
            int wi = atomic_load_explicit(&p->audio_write_idx, memory_order_acquire);
            int ri = atomic_load_explicit(&p->audio_read_idx,  memory_order_acquire);
            int buf = (wi - ri + p->cfg.audio_ring_slots) % p->cfg.audio_ring_slots;

            atomic_store_explicit(&p->audio_refill_needed, 0, memory_order_release);

            if (buf < target_audio_buffers(p)) {
                refill_audio_ring(p);
            }
        }

        // chunk prefetch (early boundary trigger)
        {
            int tf = atomic_load_explicit(&p->frame_index, memory_order_acquire);
            int play_chunk = find_chunk_for_frame(p, tf);
            int prefetch_chunk = play_chunk;

            if ((uint32_t)play_chunk < p->header.num_chunks) {
                ChunkEntry *e = &p->chunk_index[play_chunk];
                int local_frame = tf - (int)e->start_frame;
                if (local_frame < 0) local_frame = 0;

                if ((uint32_t)local_frame < (int)e->num_frames) {
                    if (e->num_frames > (uint32_t)p->cfg.prefetch_boundary_frames &&
                        (uint32_t)local_frame >= (e->num_frames - (uint32_t)p->cfg.prefetch_boundary_frames)) {
                        if ((uint32_t)(play_chunk + 1) < p->header.num_chunks)
                            prefetch_chunk = play_chunk + 1;
                    }
                }
            }

            if (prefetch_chunk != last_prefetch_chunk) {
                prefetch_around(p, prefetch_chunk);
                last_prefetch_chunk = prefetch_chunk;
            }
        }

        wait_on_seq(&p->worker_wake_seq, &last_worker_seq, p->cfg.worker_poll_ms);
    }

    return NULL;
}

// ------------------------------
// PVR init (owned by dcfmv)
// ------------------------------
static int init_pvr_resources(dcfmv_t *p) {
    if (p->pvr_inited) return 0;

    p->pvr_txr = pvr_mem_malloc(p->header.frame_type == 1
        ? p->header.tex_width * p->header.tex_height * 2
        : p->header.uncompressed_frame_size);
    if (!p->pvr_txr) return -1;

    int pot_w = 1, pot_h = 1;
    while (pot_w < p->header.tex_width)  pot_w <<= 1;
    while (pot_h < p->header.tex_height) pot_h <<= 1;
    int strided = (pot_w != p->header.tex_width || pot_h != p->header.tex_height);
    p->pvr_strided = strided;

    pvr_poly_cxt_t cxt;
    int fmt = (p->header.frame_type == 1) ? PVR_TXRFMT_YUV422 : PVR_TXRFMT_RGB565;

    if (strided) {
        fmt |= PVR_TXRFMT_VQ_ENABLE | (1 << 25) | PVR_TXRFMT_NONTWIDDLED;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, fmt, pot_w, pot_h, p->pvr_txr, PVR_FILTER_NONE);
        pvr_poly_compile(&p->poly_hdr, &cxt);
        PVR_SET(PVR_TEXTURE_MODULO, p->header.tex_width / 32);

        float uw = (float)p->header.content_width  / pot_w;
        float vh = (float)p->header.content_height / pot_h;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-braces"
        p->vert[0]=(pvr_vertex_t){PVR_CMD_VERTEX,    0,   0,   1, 0,  0,  0xffffffff};
        p->vert[1]=(pvr_vertex_t){PVR_CMD_VERTEX,    640, 0,   1, uw, 0,  0xffffffff};
        p->vert[2]=(pvr_vertex_t){PVR_CMD_VERTEX,    0,   480, 1, 0,  vh, 0xffffffff};
        p->vert[3]=(pvr_vertex_t){PVR_CMD_VERTEX_EOL,640, 480, 1, uw, vh, 0xffffffff};
#pragma GCC diagnostic pop         
    } else {
        fmt |= PVR_TXRFMT_TWIDDLED | PVR_TXRFMT_VQ_ENABLE;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, fmt,
                         p->header.tex_width, p->header.tex_height, p->pvr_txr, PVR_FILTER_NONE);
        pvr_poly_compile(&p->poly_hdr, &cxt);

        float umin = (float)(p->header.tex_width  - p->header.content_width)  / (2.0f * p->header.tex_width);
        float vmin = (float)(p->header.tex_height - p->header.content_height) / (2.0f * p->header.tex_height);
        float umax = 1.f - umin, vmax = 1.f - vmin;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-braces"
        p->vert[0]=(pvr_vertex_t){PVR_CMD_VERTEX,    0,   0,   1, umin,vmin,0xffffffff};
        p->vert[1]=(pvr_vertex_t){PVR_CMD_VERTEX,    640, 0,   1, umax,vmin,0xffffffff};
        p->vert[2]=(pvr_vertex_t){PVR_CMD_VERTEX,    0,   480, 1, umin,vmax,0xffffffff};
        p->vert[3]=(pvr_vertex_t){PVR_CMD_VERTEX_EOL,640, 480, 1, umax,vmax,0xffffffff};
#pragma GCC diagnostic pop        
    }

    p->pvr_inited = 1;
    return 0;
}

// ------------------------------
// Submit draw (inside list) - internal impl
// ------------------------------
static int dcfmv_submit_impl(dcfmv_t *p) {
    if (!p) return 0;
    pvr_wait_ready();

    int cur = atomic_load(&p->frame_index);
    if (cur >= (int)p->header.num_total_frames) return 0;

    int uid = total_to_unique(p, cur);
    int buf = uid % p->cfg.num_buffers;
    int st  = atomic_load_explicit(&p->buf_state[buf], memory_order_acquire);

    if (st != BUF_READY || atomic_load_explicit(&p->buf_unique_id[buf], memory_order_acquire) != uid) {
        schedule_decode(p, cur);

        if (p->last_unique_frame_drawn >= 0) {
            sq_fast_cpy((void *)SQ_MASK_DEST(PVR_TA_INPUT), &p->poly_hdr, sizeof(p->poly_hdr)/32);
            sq_fast_cpy((void *)SQ_MASK_DEST(PVR_TA_INPUT), p->vert,      sizeof(p->vert)/32);
            return 1;
        }
        return 0;
    }

    if (uid != p->last_unique_frame_drawn) {
        dcache_flush_range((uint32)p->frame_buffer[buf], p->header.uncompressed_frame_size);
        pvr_txr_load_dma(p->frame_buffer[buf], p->pvr_txr,
                         p->header.uncompressed_frame_size, 1, NULL, 0);
        p->last_unique_frame_drawn = uid;
    }

    sq_fast_cpy((void *)SQ_MASK_DEST(PVR_TA_INPUT), &p->poly_hdr, sizeof(p->poly_hdr)/32);
    sq_fast_cpy((void *)SQ_MASK_DEST(PVR_TA_INPUT), p->vert,      sizeof(p->vert)/32);

    return 1;
}

int dcfmv_submit(dcfmv_t *p) {
    if (!p) return 0;
    if (!p->pvr_inited) {
        if (init_pvr_resources(p) < 0) return 0;
    }
    return dcfmv_submit_impl(p);
}

// ------------------------------
// Wait helper
// ------------------------------
void dcfmv_wait_until(dcfmv_t *p, double deadline_wall_ms) {
    const double SLEEP_SAFE_MS  = 3.0;
    const double SPIN_MARGIN_MS = 1.5;

    if (p) dcfmv_audio_poll(p);

    while (1) {
        double remain = deadline_wall_ms - dcfmv_ps_ms();
        if (remain <= 0.0) break;

        if (p) dcfmv_audio_poll(p);

        if      (remain > SLEEP_SAFE_MS)  thd_sleep(1);
        else if (remain > SPIN_MARGIN_MS) thd_pass();
        else                              { thd_pass(); break; }
    }
}

// ------------------------------
// Core tick
// ------------------------------
static inline double playback_now_ms(dcfmv_t *p) {
    return dcfmv_ps_ms() - atomic_load_explicit(&p->playback_t0_ms, memory_order_acquire);
}

void dcfmv_request_seek(dcfmv_t *p, int total_frame) {
    if (!p) return;
    atomic_store_explicit(&p->seek_request, total_frame, memory_order_release);
}

double dcfmv_tick(dcfmv_t *p) {
    if (!p) return dcfmv_ps_ms();

    int req = atomic_exchange_explicit(&p->seek_request, -1, memory_order_acq_rel);
    if (req >= 0) {
        if (req < 0) req = 0;
        if (req >= (int)p->header.num_total_frames) req = (int)p->header.num_total_frames - 1;
        dcfmv_seek(p, req);

        double now = dcfmv_ps_ms();
        return now + p->frame_duration_ms;
    }

    int cur = atomic_load_explicit(&p->frame_index, memory_order_acquire);
    if (cur >= (int)p->header.num_total_frames) return dcfmv_ps_ms();

    if (atomic_load_explicit(&p->paused, memory_order_acquire)) {
        schedule_decode(p, cur);
        atomic_store_explicit(&p->audio_muted, 1, memory_order_release);
        double now = dcfmv_ps_ms();
        return now + p->frame_duration_ms;
    }

    int active = atomic_load_explicit(&p->playback_started, memory_order_acquire);

    if (active) {
        double aud_ms = playback_now_ms(p);
        int ideal = (int)(aud_ms / p->frame_duration_ms);

        if (ideal < 0) ideal = 0;
        if (ideal >= (int)p->header.num_total_frames)
            ideal = (int)p->header.num_total_frames - 1;

        if (ideal > cur) {
            atomic_store_explicit(&p->frame_index, ideal, memory_order_release);
            cur = ideal;
        }

        if (p->cfg.verbose && (cur % 30) == 0) {
            int wi = atomic_load_explicit(&p->audio_write_idx, memory_order_acquire);
            int ri = atomic_load_explicit(&p->audio_read_idx,  memory_order_acquire);
            int rb = (wi - ri + p->cfg.audio_ring_slots) % p->cfg.audio_ring_slots;

            double exp = (double)cur * p->frame_duration_ms;
            double drift = aud_ms - exp;

            printf("[sync] tf=%d aud=%.2f exp=%.2f drift=%.2f rb=%d\n",
                   cur, aud_ms, exp, drift, rb);
        }
    }

    for (int i = 0; i <= p->cfg.prefetch_ahead; i++) {
        int tf = cur + i;
        if (tf >= (int)p->header.num_total_frames) break;
        schedule_decode(p, tf);
    }

    if (!active) {
        int uid = total_to_unique(p, cur);
        int buf = uid % p->cfg.num_buffers;
        int st  = atomic_load_explicit(&p->buf_state[buf], memory_order_acquire);

        if (st == BUF_READY &&
            atomic_load_explicit(&p->buf_unique_id[buf], memory_order_acquire) == uid) {

            audio_start_if_needed(p);

            atomic_store_explicit(&p->audio_muted, 1, memory_order_release);
            audio_ring_clear(p);
            audio_seek_to_frame(p, cur);
            refill_audio_ring(p);

            double now = dcfmv_ps_ms();
            double anchor = now - ((double)cur * p->frame_duration_ms);
            atomic_store_explicit(&p->playback_t0_ms,   anchor, memory_order_release);
            atomic_store_explicit(&p->playback_started, 1,      memory_order_release);

            atomic_store_explicit(&p->audio_muted, 0, memory_order_release);

            if (p->cfg.verbose) {
                int wi = atomic_load_explicit(&p->audio_write_idx, memory_order_acquire);
                int ri = atomic_load_explicit(&p->audio_read_idx,  memory_order_acquire);
                printf("[sync] started tf=%d chunk=%d pos=%lu rb=%d\n",
                       cur, p->current_audio_chunk, (unsigned long)p->audio_chunk_read_pos,
                       (wi - ri + p->cfg.audio_ring_slots) % p->cfg.audio_ring_slots);
            }

            active = 1;
        } else {
            if (p->cfg.verbose && (cur % 30) == 0) {
                printf("[stall] waiting start tf=%d uid=%d buf=%d st=%d\n", cur, uid, buf, st);
            }
            double now = dcfmv_ps_ms();
            return now + p->frame_duration_ms;
        }
    }

    double now = dcfmv_ps_ms();
    double aud_now = active ? playback_now_ms(p) : 0.0;

    int next_tf = cur + 1;
    double next_tgt = (double)next_tf * p->frame_duration_ms;
    double wait = next_tgt - aud_now;

    if (active && wait > 0.0) return now + wait;
    return now + p->frame_duration_ms;
}

int dcfmv_is_paused(const dcfmv_t *p) {
    if (!p) return 0;
    return atomic_load_explicit((atomic_int *)&p->paused, memory_order_acquire) ? 1 : 0;
}

void dcfmv_set_paused(dcfmv_t *p, int paused) {
    if (!p) return;

    if (paused) {
        atomic_store_explicit(&p->paused, 1, memory_order_release);
        atomic_store_explicit(&p->seek_paused, 1, memory_order_release);

        atomic_store_explicit(&p->audio_muted, 1, memory_order_release);

        if (p->snd_inited) snd_stream_stop(p->stream);
        atomic_store_explicit(&p->stream_started, 0, memory_order_release);

        atomic_store_explicit(&p->playback_started, 0, memory_order_release);
    } else {
        atomic_store_explicit(&p->paused, 0, memory_order_release);

        atomic_store_explicit(&p->audio_muted, 1, memory_order_release);
        atomic_store_explicit(&p->playback_started, 0, memory_order_release);

        if (p->snd_inited) snd_stream_stop(p->stream);
        atomic_store_explicit(&p->stream_started, 0, memory_order_release);

        atomic_store_explicit(&p->seek_paused, 0, memory_order_release);

        worker_signal(p);
        decode_signal(p);
        io_signal(p);
    }
}

void dcfmv_toggle_pause(dcfmv_t *p) {
    if (!p) return;
    int cur = atomic_load_explicit(&p->paused, memory_order_acquire);
    dcfmv_set_paused(p, !cur);
    if (p->cfg.verbose) printf("[pause] %s\n", cur ? "off" : "on");
}

// ------------------------------
// Seek API
// ------------------------------
void dcfmv_seek(dcfmv_t *p, int total_frame) {
    if (!p) return;
    total_frame = MAX(0, MIN(total_frame, (int)p->header.num_total_frames - 1));

    atomic_store_explicit(&p->seek_paused, 1, memory_order_release);
    atomic_store_explicit(&p->audio_muted, 1, memory_order_release);

    if (p->snd_inited) snd_stream_stop(p->stream);
    atomic_store_explicit(&p->stream_started, 0, memory_order_release);

    io_cancel_job(p);

    atomic_store_explicit(&p->playback_started, 0, memory_order_release);
    atomic_store_explicit(&p->playback_t0_ms, 0.0, memory_order_release);

    atomic_store_explicit(&p->frame_index, total_frame, memory_order_release);

    decode_reset(p);
    p->last_unique_frame_drawn = -1;
    p->pending_free_buf = -1;

    audio_ring_clear(p);
    p->current_audio_chunk = 0;
    p->audio_chunk_read_pos = 0;
    audio_seek_to_frame(p, total_frame);

    int play_chunk = find_chunk_for_frame(p, total_frame);
    int aud_chunk  = p->current_audio_chunk;

    int pin0 = aud_chunk;
    int pin1 = play_chunk;
    int pin2 = play_chunk + 1;

    (void)load_chunk_sync(p, play_chunk, pin0, pin1, pin2);
    if ((uint32_t)aud_chunk < p->header.num_chunks)
        (void)load_chunk_sync(p, aud_chunk, pin0, pin1, pin2);

    if ((uint32_t)(play_chunk + 1) < p->header.num_chunks)
        (void)load_chunk_sync(p, play_chunk + 1, pin0, pin1, pin2);
    if ((uint32_t)(aud_chunk + 1) < p->header.num_chunks)
        (void)load_chunk_sync(p, aud_chunk + 1, pin0, pin1, pin2);

    schedule_decode(p, total_frame);

    atomic_store_explicit(&p->seek_paused, 0, memory_order_release);

    worker_signal(p);
    decode_signal(p);
    io_signal(p);

    if (p->cfg.verbose) {
        printf("[seek] -> tf=%d play_chunk=%d aud_chunk=%d pos=%lu\n",
               total_frame, play_chunk, aud_chunk, (unsigned long)p->audio_chunk_read_pos);
    }
}

// ------------------------------
// File loading: header, chunk index, LUT
// ------------------------------
static int load_header(dcfmv_t *p) {
    fs_seek(p->fd, 0, SEEK_SET);
    if (fs_read(p->fd, &p->header, sizeof(p->header)) != (ssize_t)sizeof(p->header))
        { printf("❌ header read\n"); return -1; }
    if (memcmp(p->header.magic, DCMV_MAGIC, 4) != 0)
        { printf("❌ bad magic\n"); return -1; }
    if (p->header.version != 1)
        { printf("❌ bad version %lu\n", (unsigned long)p->header.version); return -1; }

    // if (p->cfg.verbose) {
        printf("📦 DCMV v1.0 %dx%d @ %.2f fps  Audio: %dHz %dch  Frames:%lu  Chunks:%lu (%.2fs)  Codec:%s\n",
            p->header.tex_width, p->header.tex_height, p->header.fps,
            p->header.sample_rate, p->header.channels,
            (unsigned long)p->header.num_total_frames,
            (unsigned long)p->header.num_chunks, p->header.chunk_duration,
            p->header.compression_type ? "Zstd" : "LZ4");
    // }
    return 0;
}

static int load_chunk_index(dcfmv_t *p) {
    long fsz      = get_file_size(p->fd);
    uint32_t need = p->header.num_chunks * 20u;
    uint8_t *raw  = (uint8_t *)malloc(need);
    if (!raw) { printf("❌ [idx] malloc\n"); return -1; }

    fs_seek(p->fd, p->header.chunk_index_offset, SEEK_SET);
    if (fs_read(p->fd, raw, need) != (ssize_t)need)
        { printf("❌ [idx] read\n"); free(raw); return -1; }

    ChunkEntry *ci = (ChunkEntry *)calloc(p->header.num_chunks, sizeof(*ci));
    if (!ci) { free(raw); return -1; }

    for (uint32_t i = 0; i < p->header.num_chunks; i++) {
        memcpy(&ci[i].chunk_offset,       raw + i*20 +  0, 4);
        memcpy(&ci[i].video_section_size, raw + i*20 +  4, 4);
        memcpy(&ci[i].audio_size,         raw + i*20 +  8, 4);
        memcpy(&ci[i].start_frame,        raw + i*20 + 12, 4);
        memcpy(&ci[i].num_frames,         raw + i*20 + 16, 4);
    }
    free(raw);

    uint32_t min_off = (uint32_t)sizeof(DCMVHeader) +
                       (p->header.num_unique_frames + 1) * 4u +
                        p->header.num_unique_frames * 2u;
    if (min_off < 0x80u) min_off = 0x80u;

    uint32_t prev = 0;
    for (uint32_t i = 0; i < p->header.num_chunks; i++) {
        ChunkEntry *e = &ci[i];
        uint64_t end = (uint64_t)e->chunk_offset + e->video_section_size +
                       pad32_after(e->chunk_offset + e->video_section_size) +
                       (uint64_t)align32(e->audio_size) * p->header.channels;

        if (e->chunk_offset < min_off || (long)e->chunk_offset >= fsz ||
            (i > 0 && e->chunk_offset < prev) ||
            !e->video_section_size || !e->audio_size || !e->num_frames ||
            e->start_frame >= p->header.num_total_frames || end > (uint64_t)fsz) {
            printf("❌ [idx] chunk %lu invalid\n", i);
            free(ci);
            return -1;
        }
        prev = e->chunk_offset;
    }

    free(p->chunk_index);
    p->chunk_index = ci;

    if (p->cfg.verbose) {
        printf("[idx] %lu chunks\n", (unsigned long)p->header.num_chunks);
        for (int i = 0; i < 4 && (uint32_t)i < p->header.num_chunks; i++) {
            ChunkEntry *e = &ci[i];
            printf("[pad] chunk %u off=%lu vid=%lu pad=%lu aud=%lu start=%lu n=%lu\n",
                i, e->chunk_offset, e->video_section_size,
                pad32_after(e->chunk_offset + e->video_section_size),
                e->audio_size, e->start_frame, e->num_frames);
        }
    }
    return 0;
}

static int build_t2u_lut(dcfmv_t *p) {
    p->t2u_lut = (uint16_t *)malloc(p->header.num_total_frames * 2);
    if (!p->t2u_lut) return -1;

    int t = 0;
    for (uint32_t u = 0; u < p->header.num_unique_frames; u++) {
        uint16_t dur = p->frame_durations[u]; if (!dur) dur = 1;
        for (uint16_t rr = 0; rr < dur && t < (int)p->header.num_total_frames; rr++)
            p->t2u_lut[t++] = (uint16_t)u;
    }
    uint16_t last = p->header.num_unique_frames ? (uint16_t)(p->header.num_unique_frames - 1) : 0;
    while (t < (int)p->header.num_total_frames) p->t2u_lut[t++] = last;
    return 0;
}

// ------------------------------
// Public lifecycle
// ------------------------------
dcfmv_t *dcfmv_create(const dcfmv_config_t *cfg) {
    dcfmv_t *p = (dcfmv_t *)calloc(1, sizeof(*p));
    if (!p) return NULL;

    dcfmv_config_t d = {
        .decode_threads = 3,
        .num_buffers = 16,
        .chunk_cache_slots = 4,
        .audio_ring_slots = 24,
        .audio_buffer_bytes = 4096,
        .target_audio_ms = 700.0,
        .prefetch_ahead = 0,
        .prefetch_boundary_frames = 4,
        .initial_preload = 0,
        .chunk_io_slice_bytes = (128 * 1024),
        .worker_poll_ms = 5,
        .verbose = 1,
    };
    p->cfg = cfg ? *cfg : d;

    if (p->cfg.prefetch_ahead <= 0) p->cfg.prefetch_ahead = p->cfg.num_buffers - 3;
    if (p->cfg.initial_preload <= 0) p->cfg.initial_preload = p->cfg.num_buffers;

    p->decode_threads = MAX(1, p->cfg.decode_threads);
    if (p->decode_threads > DCFMV_MAX_DECODE_THREADS) p->decode_threads = DCFMV_MAX_DECODE_THREADS;

    p->chunk_cache_size = MAX(2, p->cfg.chunk_cache_slots);
    p->decode_q_cap = 64;

    p->fd = -1;

    mutex_init(&p->file_mutex, MUTEX_TYPE_NORMAL);
    mutex_init(&p->chunk_cache_mutex, MUTEX_TYPE_NORMAL);
    mutex_init(&p->io_mutex, MUTEX_TYPE_NORMAL);

    atomic_store(&p->frame_index, 0);
    atomic_store(&p->audio_muted, 1);
    atomic_store(&p->playback_started, 0);
    atomic_store_explicit(&p->paused, 0, memory_order_release);
    atomic_store_explicit(&p->playback_t0_ms, 0.0, memory_order_release);
    atomic_store_explicit(&p->seek_request, -1, memory_order_release);

    atomic_store_explicit(&p->io_enabled, 0, memory_order_release);
    atomic_store_explicit(&p->seek_paused, 0, memory_order_release);
    atomic_store_explicit(&p->decode_generation, 0, memory_order_release);

    p->th_decode   = NULL;
    p->decode_arg  = NULL;
    p->th_worker   = NULL;
    p->th_io       = NULL;

    p->last_unique_frame_drawn = -1;
    p->pending_free_buf = -1;

    return p;
}

static void free_everything(dcfmv_t *p) {
    if (!p) return;

    if (p->snd_inited) {
        atomic_store(&p->audio_muted, 1);
        snd_stream_stop(p->stream);
        snd_stream_destroy(p->stream);
        p->snd_inited = 0;
    }

    if (p->fd >= 0) { fs_close(p->fd); p->fd = -1; }

    if (p->chunk_cache) {
        for (int i = 0; i < p->chunk_cache_size; i++) {
            free(p->chunk_cache[i].data);
            free(p->chunk_cache[i].frame_off_local);
            free(p->chunk_cache[i].frame_sz_local);
            free(p->chunk_cache[i].seen_u);
            free(p->chunk_cache[i].seen_off);
        }
        free(p->chunk_cache);
        p->chunk_cache = NULL;
    }

    if (p->frame_buffer) {
        for (int i = 0; i < p->cfg.num_buffers; i++) free(p->frame_buffer[i]);
        free(p->frame_buffer);
        p->frame_buffer = NULL;
    }
    free(p->buf_state);       p->buf_state = NULL;
    free(p->buf_total_frame); p->buf_total_frame = NULL;
    free(p->buf_unique_id);   p->buf_unique_id = NULL;

    if (p->audio_ring) {
        for (int i = 0; i < p->cfg.audio_ring_slots; i++) {
            free(p->audio_ring[i].left);
            free(p->audio_ring[i].right);
        }
        free(p->audio_ring);
        p->audio_ring = NULL;
    }

    free(p->decode_q); p->decode_q = NULL;
    free(p->th_decode); p->th_decode = NULL;
    free(p->decode_arg);    p->decode_arg = NULL;

    free(p->frame_sizes); p->frame_sizes = NULL;
    free(p->frame_durations); p->frame_durations = NULL;
    free(p->chunk_index); p->chunk_index = NULL;
    free(p->t2u_lut); p->t2u_lut = NULL;

    if (p->zstd_dctx) {
        for (int i = 0; i < p->decode_threads; i++) {
            if (p->zstd_dctx[i]) ZSTD_freeDCtx(p->zstd_dctx[i]);
        }
        free(p->zstd_dctx);
        p->zstd_dctx = NULL;
    }

    p->pvr_txr = NULL;
    p->pvr_inited = 0;
}

int dcfmv_open(dcfmv_t *p, const char *path) {
    if (!p || !path) return -1;

    dcfmv_close(p);

    p->path = strdup(path);
    p->fd = fs_open(path, O_RDONLY);
    if (p->fd < 0) {
        printf("❌ : cannot open %s\n", path);
        free(p->path); p->path = NULL;
        return -1;
    }

    if (load_header(p) < 0) return -1;

    if (p->header.compression_type == 1) {
        p->zstd_dctx = (ZSTD_DCtx **)calloc(p->decode_threads, sizeof(ZSTD_DCtx *));
        if (!p->zstd_dctx) return -1;
        for (int i = 0; i < p->decode_threads; i++) {
            p->zstd_dctx[i] = ZSTD_createDCtx();
            if (!p->zstd_dctx[i]) { printf("❌ ZSTD_createDCtx[%d]\n", i); return -1; }
        }
    }

    fs_seek(p->fd, sizeof(DCMVHeader), SEEK_SET);
    p->frame_sizes     = (uint32_t *)malloc((p->header.num_unique_frames + 1) * 4);
    p->frame_durations = (uint16_t *)malloc(p->header.num_unique_frames * 2);
    if (!p->frame_sizes || !p->frame_durations) { printf("❌ frame table alloc\n"); return -1; }

    fs_read(p->fd, p->frame_sizes,     (p->header.num_unique_frames + 1) * 4);
    fs_read(p->fd, p->frame_durations,  p->header.num_unique_frames * 2);

    if (load_chunk_index(p) < 0) return -1;
    if (build_t2u_lut(p) < 0) return -1;

    p->chunk_cache = (ChunkCache *)calloc(p->chunk_cache_size, sizeof(ChunkCache));
    if (!p->chunk_cache) return -1;
    if (init_chunk_cache(p) < 0) return -1;

    p->frame_buffer = (uint8_t **)calloc(p->cfg.num_buffers, sizeof(uint8_t *));
    p->buf_state = (_Atomic int *)calloc(p->cfg.num_buffers, sizeof(_Atomic int));
    p->buf_total_frame = (_Atomic int *)calloc(p->cfg.num_buffers, sizeof(_Atomic int));
    p->buf_unique_id = (_Atomic int *)calloc(p->cfg.num_buffers, sizeof(_Atomic int));
    if (!p->frame_buffer || !p->buf_state || !p->buf_total_frame || !p->buf_unique_id) return -1;

    for (int i = 0; i < p->cfg.num_buffers; i++) {
        p->frame_buffer[i] = (uint8_t *)memalign(32, p->header.uncompressed_frame_size);
        if (!p->frame_buffer[i]) { printf("❌ frame_buffer[%d]\n", i); return -1; }
        atomic_store(&p->buf_state[i], BUF_EMPTY);
        atomic_store(&p->buf_total_frame[i], -1);
        atomic_store(&p->buf_unique_id[i], -1);
    }

    p->audio_ring = (AudioSlot *)calloc(p->cfg.audio_ring_slots, sizeof(AudioSlot));
    if (!p->audio_ring) return -1;

    for (int i = 0; i < p->cfg.audio_ring_slots; i++) {
        p->audio_ring[i].left  = (uint8_t *)memalign(32, p->cfg.audio_buffer_bytes);
        p->audio_ring[i].right = (uint8_t *)memalign(32, p->cfg.audio_buffer_bytes);
        if (!p->audio_ring[i].left || !p->audio_ring[i].right) return -1;
        atomic_store(&p->audio_ring[i].valid, 0);
        p->audio_ring[i].valid_bytes = 0;
    }

    p->decode_q = (DecodeJob *)calloc(p->decode_q_cap, sizeof(DecodeJob));
    p->th_decode = (kthread_t **)calloc(p->decode_threads, sizeof(kthread_t *));
    p->decode_arg = (DecodeThreadArg *)calloc(p->decode_threads, sizeof(DecodeThreadArg));
    if (!p->decode_q || !p->th_decode || !p->decode_arg ) return -1;

    snd_stream_init_ex(p->header.channels, p->cfg.audio_buffer_bytes);

    g_audio_owner = p;

    p->stream = snd_stream_alloc(NULL, p->cfg.audio_buffer_bytes);
    snd_stream_set_callback_direct(p->stream, audio_cb);

    snd_stream_volume(p->stream, 255);
    p->snd_inited = 1;

    atomic_store(&p->audio_muted, 1);
    atomic_store(&p->playback_started, 0);
    atomic_store_explicit(&p->playback_t0_ms, 0.0, memory_order_release);
    atomic_store_explicit(&p->seek_request, -1, memory_order_release);

    audio_ring_clear(p);
    p->current_audio_chunk = 0;
    p->audio_chunk_read_pos = 0;

    if (p->cfg.verbose) printf("🔄 Preloading chunks...\n");

    int boot_preload = MIN(p->chunk_cache_size, (int)p->header.num_chunks);

    for (int i = 0; i < boot_preload; i++) {
        if (load_chunk_sync(p, i, 0, 1, 2) < 0) {
            printf("❌ sync load chunk %d\n", i);
            return -1;
        }
    }

    decode_reset(p);
    int warm = MIN(p->cfg.initial_preload, (int)p->header.num_total_frames);

    for (int tf = 0; tf < warm; tf++) {
        int uf = total_to_unique(p, tf), b = uf % p->cfg.num_buffers;
        atomic_store(&p->buf_state[b], BUF_LOADING);
        atomic_store(&p->buf_total_frame[b], tf);
        atomic_store(&p->buf_unique_id[b], uf);
        if (load_frame(p, tf, b, 0) == 0) atomic_store(&p->buf_state[b], BUF_READY);
        else                               atomic_store(&p->buf_state[b], BUF_EMPTY);
    }

    p->frame_duration_ms = 1000.0 / (double)p->header.fps;

    thd_set_hz(1000);

    for (int i = 0; i < p->decode_threads; i++) {
        p->decode_arg[i].p = p;
        p->decode_arg[i].tid = i;
        p->th_decode[i] = thd_create(2 + i, decode_thread_fn, &p->decode_arg[i]);
    }
    p->th_worker = thd_create(2 + p->decode_threads, worker_thread_fn, p);
    p->th_io = thd_create(3 + p->decode_threads, io_thread_fn, p);

    atomic_store_explicit(&p->io_enabled, 1, memory_order_release);
    for (int i = 3; i < MIN(3 + p->chunk_cache_size - 3, (int)p->header.num_chunks); i++)
        request_chunk_async(p, i, 0, 1, 2);

    return 0;
}

void dcfmv_close(dcfmv_t *p) {
    if (!p) return;

    if (p->snd_inited) {
        atomic_store(&p->audio_muted, 1);
        snd_stream_stop(p->stream);
    }

    atomic_store_explicit(&p->io_enabled, 0, memory_order_release);

    free_everything(p);

    free(p->path);
    p->path = NULL;
    if (g_audio_owner == p) g_audio_owner = NULL;
}

void dcfmv_destroy(dcfmv_t *p) {
    if (!p) return;
    dcfmv_close(p);
    free(p);
}