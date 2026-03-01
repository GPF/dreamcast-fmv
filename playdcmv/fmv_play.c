/**
 * fmv_play.c - Dreamcast FMV Player (DCMV v1.0 Chunked Format)
 * ------------------------------------------------------------
 * Container : chunk-based (time-based segments), stride=20 index
 * Video     : LZ4 / Zstd compressed frames, per-chunk frame->offset map
 * Audio     : 4-bit ADPCM, ring-buffer fed from worker thread
 * Sync      : AICA wall-clock anchored to first displayed frame
 *
 * Author : Troy Davis (GPF) — https://github.com/GPF
 * License: Public Domain / MIT
 */

#include <kos.h>
#include <kos/dbgio.h>

#include <dc/sound/stream.h>
#include <dc/sound/sound.h>
#include <dc/pvr.h>
#include <dc/maple/controller.h>
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

// =============================================================================
// Tunables
// =============================================================================

#define NUM_BUFFERS              16
#define CHUNK_CACHE_SIZE         4
#define ACTIVE_CHUNK_CACHE_SLOTS 4
#define DECODE_THREADS           3
#define DECODE_Q_CAP             64
#define AUDIO_BUFFER_SIZE        4096   // bytes per channel per ring slot
#define AUDIO_RING_SIZE          24
#define TARGET_AUDIO_BUFFER_MS   700.0
#define PREFETCH_AHEAD           (NUM_BUFFERS - 3)
#define PREFETCH_BOUNDARY_FRAMES 4
#define INITIAL_PRELOAD          NUM_BUFFERS
#define CHUNK_IO_SLICE           (128 * 1024)
#define PROF_PRINT_EVERY_MS      1000.0
#define ADPCM_BYTES_PER_SAMPLE   0.5   // 4-bit: 0.5 bytes/sample/channel

// Worker wakes at least every WORKER_POLL_MS even without an explicit signal.
// This ensures the ring stays full during smooth playback where no underrun
// fires, and catches the case where refill filled fewer slots than needed
// because the next audio chunk wasn't cached yet.
#define WORKER_POLL_MS           5

#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))

#define DCMV_MAGIC "DCMV"
static const char *VIDEO_FILE = NULL;

// =============================================================================
// DCMV v1.0 Header  (packed — matches packer exactly)
// =============================================================================

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

static DCMVHeader header;

// =============================================================================
// Chunk index  (stride=20: off, vid_bytes, aud_bytes_per_ch, start_frame, n)
// =============================================================================

typedef struct {
    uint32_t chunk_offset;
    uint32_t video_section_size;
    uint32_t audio_size;        // bytes per channel
    uint32_t start_frame;
    uint32_t num_frames;
} ChunkEntry;

static ChunkEntry *chunk_index = NULL;

// =============================================================================
// Timing — AICA hardware clock (tied to audio subsystem, ideal for A/V sync)
// =============================================================================

static inline double psTimer(void) {
    #define AICA_MEM_CLOCK    0x021000
    #define AICA_TICKS_PER_MS 4.410
    uint32_t j = g2_read_32(SPU_RAM_UNCACHED_BASE + AICA_MEM_CLOCK);
    return (double)j / AICA_TICKS_PER_MS;
}

static double g_sleep_quantum_ms = 1.0;

// =============================================================================
// Thread profiling
// =============================================================================

typedef struct {
    const char *name;
    double   work_ms, idle_ms;
    uint32_t loops, c0, c1;
    double   last_print_ms;
} ThreadProf;

static void prof_init(ThreadProf *p, const char *name) {
    *p = (ThreadProf){ .name = name, .last_print_ms = psTimer() };
}

static void prof_tick(ThreadProf *p, double work_ms, double idle_ms) {
    p->work_ms += work_ms;
    p->idle_ms += idle_ms;
    p->loops++;
    double now = psTimer();
    if (now - p->last_print_ms < PROF_PRINT_EVERY_MS) return;
    double total = p->work_ms + p->idle_ms;
    double util  = total > 0.0 ? 100.0 * p->work_ms / total : 0.0;
    printf("[thr] %-7s util=%5.1f%% work=%.1fms idle=%.1fms loops=%lu c0=%lu c1=%lu\n",
           p->name, util, p->work_ms, p->idle_ms,
           (unsigned long)p->loops, (unsigned long)p->c0, (unsigned long)p->c1);
    p->work_ms = p->idle_ms = 0.0;
    p->loops = p->c0 = p->c1 = 0;
    p->last_print_ms = now;
}

// =============================================================================
// Chunk cache
// =============================================================================

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

static ChunkCache chunk_cache[CHUNK_CACHE_SIZE];
static uint32_t   global_cache_tick = 0;

static mutex_t file_mutex        = MUTEX_INITIALIZER;
static mutex_t chunk_cache_mutex = MUTEX_INITIALIZER;

// =============================================================================
// Async IO state
// =============================================================================

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

static mutex_t    io_mutex    = MUTEX_INITIALIZER;
static ChunkIOJob io_job      = {0};
static _Atomic int io_enabled = 0;

// =============================================================================
// genwait-based wake signals (avoid thd_sleep polling)
// =============================================================================

static _Atomic uint32_t decode_wake_seq = 0;
static _Atomic uint32_t worker_wake_seq = 0;
static _Atomic uint32_t io_wake_seq     = 0;

static inline void wait_on_seq(_Atomic uint32_t *seq_atomic, uint32_t *last_seen, int timeout_ms) {
    while (atomic_load_explicit(seq_atomic, memory_order_acquire) == *last_seen) {
        genwait_wait((void*)seq_atomic, NULL, timeout_ms);
        if (timeout_ms > 0) break;
    }
    *last_seen = atomic_load_explicit(seq_atomic, memory_order_acquire);
}

static inline void decode_signal(void) {
    atomic_fetch_add_explicit(&decode_wake_seq, 1, memory_order_release);
    genwait_wake_one((void*)&decode_wake_seq);
}

static inline void worker_signal(void) {
    atomic_fetch_add_explicit(&worker_wake_seq, 1, memory_order_release);
    genwait_wake_one((void*)&worker_wake_seq);
}

static void io_signal(void) {
    if (atomic_load_explicit(&io_enabled, memory_order_acquire)) {
        atomic_fetch_add_explicit(&io_wake_seq, 1, memory_order_release);
        genwait_wake_one((void*)&io_wake_seq);
    }
}

// =============================================================================
// PVR / video state
// =============================================================================

static pvr_ptr_t      pvr_txr;
static pvr_poly_hdr_t poly_hdr;
static pvr_vertex_t   vert[4];

enum BufState { BUF_EMPTY=0, BUF_QUEUED=1, BUF_LOADING=2, BUF_READY=3 };

static uint8_t    *frame_buffer[NUM_BUFFERS];
static _Atomic int buf_state[NUM_BUFFERS];
static _Atomic int buf_total_frame[NUM_BUFFERS];
static _Atomic int buf_unique_id[NUM_BUFFERS];

static atomic_int  frame_index       = 0;
static atomic_int  audio_muted       = 1;
static _Atomic int playback_started  = 0;

static _Atomic double playback_t0_ms  = 0.0;
static double         frame_duration  = 0.0;
static int            last_unique_frame_drawn = -1;
static int            pending_free_buf        = -1;

// =============================================================================
// File / codec globals
// =============================================================================

static file_t     video_fd        = -1;
static uint32_t  *frame_sizes     = NULL;
static uint16_t  *frame_durations = NULL;
static uint16_t  *t2u_lut         = NULL;

static snd_stream_hnd_t stream;

// =============================================================================
// Zstd: per-thread DCtx (NOT thread-safe)
// =============================================================================

static ZSTD_DCtx *g_zstd_dctx[DECODE_THREADS] = {0};

typedef struct {
    int tid;   // 0..DECODE_THREADS-1
} DecodeThreadArg;

static DecodeThreadArg g_decode_args[DECODE_THREADS];

// =============================================================================
// Audio ring buffer
// =============================================================================

typedef struct {
    uint8_t    left [AUDIO_BUFFER_SIZE] __attribute__((aligned(32)));
    uint8_t    right[AUDIO_BUFFER_SIZE] __attribute__((aligned(32)));
    size_t     valid_bytes;     // bytes PER CHANNEL
    _Atomic int valid;
} AudioSlot;

static AudioSlot   audio_ring[AUDIO_RING_SIZE] __attribute__((aligned(32)));
static atomic_int  audio_write_idx     = 0;
static atomic_int  audio_read_idx      = 0;
static _Atomic int audio_refill_needed = 0;
static size_t      g_audio_ring_read_pos = 0;

static int    current_audio_chunk  = 0;
static size_t audio_chunk_read_pos = 0;

static inline double audio_entry_ms(void) {
    return (1000.0 * (double)AUDIO_BUFFER_SIZE) /
           ((double)header.sample_rate * ADPCM_BYTES_PER_SAMPLE);
}

static inline int target_audio_buffers(void) {
    int n = (int)((TARGET_AUDIO_BUFFER_MS + audio_entry_ms() - 1.0) / audio_entry_ms());
    return MAX(4, MIN(n, AUDIO_RING_SIZE - 2));
}

// =============================================================================
// Decode job queue (SPSC lock-free)
// =============================================================================

typedef struct {
    int total_frame, unique_id, buf, generation;
} DecodeJob;

static DecodeJob   decode_q[DECODE_Q_CAP];
static _Atomic int decode_q_head   = 0;
static _Atomic int decode_q_tail   = 0;
static _Atomic int GSeekGeneration = 0;

static inline int q_inc(int x) { return (x + 1) % DECODE_Q_CAP; }

static int decode_q_push(const DecodeJob *j) {
    int head = atomic_load_explicit(&decode_q_head, memory_order_relaxed);
    int next = q_inc(head);
    if (next == atomic_load_explicit(&decode_q_tail, memory_order_acquire)) return 0;
    decode_q[head] = *j;
    atomic_store_explicit(&decode_q_head, next, memory_order_release);
    decode_signal();
    return 1;
}

static int decode_q_pop(DecodeJob *out) {
    int tail = atomic_load_explicit(&decode_q_tail, memory_order_relaxed);
    if (tail == atomic_load_explicit(&decode_q_head, memory_order_acquire)) return 0;
    *out = decode_q[tail];
    atomic_store_explicit(&decode_q_tail, q_inc(tail), memory_order_release);
    return 1;
}

static void decode_q_flush(void) {
    atomic_store_explicit(&decode_q_tail,
        atomic_load_explicit(&decode_q_head, memory_order_acquire),
        memory_order_release);
}

// =============================================================================
// Utility
// =============================================================================

static inline uint32_t align32(uint32_t x)        { return (x + 31u) & ~31u; }
static inline uint32_t pad32_after(uint32_t end)  { return (32u - (end & 31u)) & 31u; }
static inline uint32_t frame_comp_size(uint32_t u){ return frame_sizes[u] & 0x1FFFFFFFu; }

static inline int total_to_unique(int tf) {
    if (tf < 0) return 0;
    if (tf >= (int)header.num_total_frames)
        return (int)(header.num_unique_frames ? header.num_unique_frames - 1 : 0);
    return (int)t2u_lut[tf];
}

static int find_chunk_for_frame(int tf) {
    if (!chunk_index || !header.num_chunks) return 0;
    tf = MAX(0, MIN(tf, (int)header.num_total_frames - 1));
    int lo = 0, hi = (int)header.num_chunks - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t s = chunk_index[mid].start_frame;
        uint32_t n = chunk_index[mid].num_frames;
        if ((uint32_t)tf < s)         hi = mid - 1;
        else if ((uint32_t)tf >= s+n) lo = mid + 1;
        else                          return mid;
    }
    return (int)header.num_chunks - 1;
}

// =============================================================================
// Chunk cache helpers
// =============================================================================

static ChunkCache *cache_acquire(int chunk_id) {
    ChunkCache *ret = NULL;
    mutex_lock(&chunk_cache_mutex);
    for (int i = 0; i < CHUNK_CACHE_SIZE; i++) {
        if (!chunk_cache[i].data) continue;
        if (atomic_load_explicit(&chunk_cache[i].valid, memory_order_acquire) == CHUNK_READY &&
            chunk_cache[i].chunk_id == chunk_id) {
            atomic_fetch_add_explicit(&chunk_cache[i].refs, 1, memory_order_acq_rel);
            chunk_cache[i].last_used = ++global_cache_tick;
            ret = &chunk_cache[i];
            break;
        }
    }
    mutex_unlock(&chunk_cache_mutex);
    return ret;
}

static void cache_release(ChunkCache *c) {
    if (c) atomic_fetch_sub_explicit(&c->refs, 1, memory_order_acq_rel);
}

static ChunkCache *evict_slot(int pin0, int pin1, int pin2) {
    for (int i = 0; i < CHUNK_CACHE_SIZE; i++) {
        if (chunk_cache[i].data &&
            atomic_load_explicit(&chunk_cache[i].valid, memory_order_relaxed) == CHUNK_EMPTY)
            return &chunk_cache[i];
    }
    int lru = -1; uint32_t best = 0xFFFFFFFFu;
    for (int i = 0; i < CHUNK_CACHE_SIZE; i++) {
        if (!chunk_cache[i].data) continue;
        if (atomic_load_explicit(&chunk_cache[i].valid, memory_order_relaxed) != CHUNK_READY) continue;
        if (atomic_load_explicit(&chunk_cache[i].refs,  memory_order_acquire) > 0) continue;
        int cid = chunk_cache[i].chunk_id;
        if (cid == pin0 || cid == pin1 || cid == pin2) continue;
        if (chunk_cache[i].last_used < best) { best = chunk_cache[i].last_used; lru = i; }
    }
    if (lru < 0) return NULL;
    atomic_store_explicit(&chunk_cache[lru].valid, CHUNK_EMPTY, memory_order_release);
    atomic_store_explicit(&chunk_cache[lru].refs,  0,           memory_order_release);
    chunk_cache[lru].chunk_id        = -1;
    chunk_cache[lru].video_section   = chunk_cache[lru].audio_L = chunk_cache[lru].audio_R = NULL;
    chunk_cache[lru].video_bytes_size = chunk_cache[lru].last_used = 0;
    return &chunk_cache[lru];
}

// =============================================================================
// Per-chunk frame map
// =============================================================================

static void build_chunk_frame_map(ChunkCache *slot, int chunk_id) {
    ChunkEntry *e = &chunk_index[chunk_id];
    uint32_t n    = MIN(e->num_frames, slot->map_cap);
    uint32_t seen_cap = MIN(slot->seen_cap, n);

    uint32_t cur_raw = 0, cur_aln = 0, seen_cnt = 0;
    for (uint32_t j = 0; j < n; j++) {
        int uf = total_to_unique((int)e->start_frame + (int)j);
        uf = MAX(0, MIN(uf, (int)header.num_unique_frames - 1));
        uint32_t sz = MAX(1u, frame_comp_size((uint32_t)uf));
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
        int uf = total_to_unique((int)e->start_frame + (int)j);
        uf = MAX(0, MIN(uf, (int)header.num_unique_frames - 1));
        uint32_t sz  = MAX(1u, frame_comp_size((uint32_t)uf));
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

// =============================================================================
// Chunk cache buffer allocation
// =============================================================================

static uint32_t max_chunk_disk_bytes(void) {
    uint32_t mx = 0;
    for (uint32_t i = 0; i < header.num_chunks; i++) {
        ChunkEntry *e = &chunk_index[i];
        uint32_t v = e->video_section_size + pad32_after(e->chunk_offset + e->video_section_size);
        uint32_t a = align32(e->audio_size) * (uint32_t)header.channels;
        mx = MAX(mx, v + a);
    }
    return align32(mx + 64);
}

static uint32_t max_frames_per_chunk(void) {
    uint32_t n = (uint32_t)((double)header.fps * (double)header.chunk_duration + 8.0);
    return MAX(n, 8u);
}

static int init_chunk_cache(void) {
    uint32_t max_chunk = max_chunk_disk_bytes();
    uint32_t map_cap   = max_frames_per_chunk();

    printf("[cache] max chunk bytes (disk) = %u, slots=%d map_cap=%u\n",
           (unsigned)max_chunk, ACTIVE_CHUNK_CACHE_SLOTS, (unsigned)map_cap);

    for (int i = 0; i < CHUNK_CACHE_SIZE; i++) {
        if (i >= ACTIVE_CHUNK_CACHE_SLOTS) {
            free(chunk_cache[i].data);            chunk_cache[i].data            = NULL;
            free(chunk_cache[i].frame_off_local); chunk_cache[i].frame_off_local = NULL;
            free(chunk_cache[i].frame_sz_local);  chunk_cache[i].frame_sz_local  = NULL;
            free(chunk_cache[i].seen_u);          chunk_cache[i].seen_u          = NULL;
            free(chunk_cache[i].seen_off);        chunk_cache[i].seen_off        = NULL;
            chunk_cache[i].seen_cap = chunk_cache[i].map_cap = 0;
            chunk_cache[i].chunk_id = -1; chunk_cache[i].size = 0;
            atomic_store(&chunk_cache[i].valid, CHUNK_EMPTY);
            atomic_store(&chunk_cache[i].refs,  0);
            continue;
        }

        free(chunk_cache[i].data);
        chunk_cache[i].data = (uint8_t *)memalign(32, max_chunk);
        if (!chunk_cache[i].data) {
            printf("❌ [cache] memalign slot %d (%u bytes)\n", i, (unsigned)max_chunk);
            return -1;
        }

        free(chunk_cache[i].frame_off_local);
        free(chunk_cache[i].frame_sz_local);
        chunk_cache[i].frame_off_local = (uint32_t *)malloc(map_cap * 4);
        chunk_cache[i].frame_sz_local  = (uint32_t *)malloc(map_cap * 4);
        if (!chunk_cache[i].frame_off_local || !chunk_cache[i].frame_sz_local) {
            printf("❌ [cache] map alloc slot %d\n", i); return -1;
        }

        uint32_t want_seen = MAX(map_cap, 8u);
        if (chunk_cache[i].seen_cap < want_seen) {
            free(chunk_cache[i].seen_u);
            free(chunk_cache[i].seen_off);
            chunk_cache[i].seen_u   = (uint32_t *)malloc(want_seen * 4);
            chunk_cache[i].seen_off = (uint32_t *)malloc(want_seen * 4);
            if (!chunk_cache[i].seen_u || !chunk_cache[i].seen_off) {
                printf("❌ [cache] seen alloc slot %d\n", i); return -1;
            }
            chunk_cache[i].seen_cap = want_seen;
        }

        chunk_cache[i].size      = max_chunk;
        chunk_cache[i].map_cap   = map_cap;
        chunk_cache[i].chunk_id  = -1;
        chunk_cache[i].last_used = 0;
        chunk_cache[i].video_section = chunk_cache[i].audio_L = chunk_cache[i].audio_R = NULL;
        chunk_cache[i].video_bytes_size = 0;
        atomic_store(&chunk_cache[i].valid, CHUNK_EMPTY);
        atomic_store(&chunk_cache[i].refs,  0);
    }
    return 0;
}

// =============================================================================
// Chunk finalisation
// =============================================================================

static void chunk_finalise(ChunkCache *slot, int chunk_id,
                           uint32_t video_real, uint32_t video_disk) {
    ChunkEntry *e          = &chunk_index[chunk_id];
    slot->chunk_id         = chunk_id;
    slot->video_section    = slot->data;
    slot->video_bytes_size = video_real;

    uint32_t aud_disk_per_ch = align32(e->audio_size);

    slot->audio_L          = slot->data + video_disk;
    slot->audio_R          = (header.channels == 2) ? (slot->audio_L + aud_disk_per_ch) : NULL;

    slot->last_used        = ++global_cache_tick;
    build_chunk_frame_map(slot, chunk_id);
    atomic_store_explicit(&slot->valid, CHUNK_READY, memory_order_release);
    worker_signal();
}

// =============================================================================
// Async IO
// =============================================================================

static int request_chunk_async(int chunk_id, int pin0, int pin1, int pin2) {
    if (chunk_id < 0 || (uint32_t)chunk_id >= header.num_chunks) return -1;

    // Already cached?
    mutex_lock(&chunk_cache_mutex);
    for (int i = 0; i < CHUNK_CACHE_SIZE; i++) {
        if (!chunk_cache[i].data) continue;
        if (atomic_load_explicit(&chunk_cache[i].valid, memory_order_acquire) == CHUNK_READY &&
            chunk_cache[i].chunk_id == chunk_id) {
            chunk_cache[i].last_used = ++global_cache_tick;
            mutex_unlock(&chunk_cache_mutex);
            return 0;
        }
    }
    mutex_unlock(&chunk_cache_mutex);

    mutex_lock(&io_mutex);
    if (io_job.state == IO_LOADING && io_job.chunk_id == chunk_id)
        { mutex_unlock(&io_mutex); return 0; }
    if (io_job.state != IO_IDLE)
        { mutex_unlock(&io_mutex); return 0; }

    mutex_lock(&chunk_cache_mutex);
    ChunkCache *slot = evict_slot(pin0, pin1, pin2);
    if (!slot) { mutex_unlock(&chunk_cache_mutex); mutex_unlock(&io_mutex); return -1; }

    // IMPORTANT: mark slot as "belongs to chunk_id" while LOADING so
    // is_cached_or_loading() can suppress duplicate requests.
    slot->chunk_id = chunk_id;
    atomic_store_explicit(&slot->valid, CHUNK_LOADING, memory_order_release);
    atomic_store_explicit(&slot->refs,  0,             memory_order_release);
    slot->video_section = slot->audio_L = slot->audio_R = NULL;
    slot->video_bytes_size = 0;
    mutex_unlock(&chunk_cache_mutex);

    ChunkEntry *e     = &chunk_index[chunk_id];
    uint32_t vid_real = e->video_section_size;
    uint32_t vid_disk = vid_real + pad32_after(e->chunk_offset + vid_real);
    uint32_t aud_disk = align32(e->audio_size) * header.channels;
    uint32_t total    = vid_disk + aud_disk;

    if (total > slot->size) {
        printf("❌ [io] chunk %d needs %u, slot has %u\n", chunk_id, total, slot->size);
        mutex_lock(&chunk_cache_mutex);
        atomic_store_explicit(&slot->valid, CHUNK_EMPTY, memory_order_release);
        slot->chunk_id = -1;
        mutex_unlock(&chunk_cache_mutex);
        mutex_unlock(&io_mutex);
        return -1;
    }

    io_job = (ChunkIOJob){
        .state = IO_LOADING, .chunk_id = chunk_id,
        .pin0 = pin0, .pin1 = pin1, .pin2 = pin2, .slot = slot,
        .file_off = e->chunk_offset, .total_bytes = total,
        .progress = 0,
        .video_real_bytes = vid_real, .video_disk_bytes = vid_disk, .audio_disk_bytes = aud_disk,
        .cur_off = e->chunk_offset,
        .did_seek = 0,
    };
    mutex_unlock(&io_mutex);
    io_signal();
    return 0;
}

static int is_cached_or_loading(int chunk_id) {
    // Cached or "reserved for load"?
    mutex_lock(&chunk_cache_mutex);
    for (int i = 0; i < CHUNK_CACHE_SIZE; i++) {
        if (!chunk_cache[i].data) continue;
        if (chunk_cache[i].chunk_id != chunk_id) continue;
        int v = atomic_load_explicit(&chunk_cache[i].valid, memory_order_acquire);
        if (v == CHUNK_READY || v == CHUNK_LOADING) {
            mutex_unlock(&chunk_cache_mutex);
            return 1;
        }
    }
    mutex_unlock(&chunk_cache_mutex);

    mutex_lock(&io_mutex);
    int loading = (io_job.state == IO_LOADING && io_job.chunk_id == chunk_id);
    mutex_unlock(&io_mutex);
    return loading;
}

static void prefetch_around(int play_chunk) {
    int aud  = current_audio_chunk;
    int pin0 = aud, pin1 = play_chunk, pin2 = play_chunk + 1;

    int want[10];
    int n = 0;

    if ((uint32_t)aud < header.num_chunks) want[n++] = aud;
    if ((uint32_t)(aud + 1) < header.num_chunks) want[n++] = aud + 1;

    int runway = MAX(1, ACTIVE_CHUNK_CACHE_SLOTS - 2);
    for (int k = 0; k <= runway && n < (int)(sizeof(want)/sizeof(want[0])); k++) {
        int c = play_chunk + k;
        if ((uint32_t)c < header.num_chunks) want[n++] = c;
    }

    for (int i = 0; i < n; i++) {
        if (!is_cached_or_loading(want[i]))
            request_chunk_async(want[i], pin0, pin1, pin2);
    }
}

static void io_pump_slice(void) {
    mutex_lock(&io_mutex);
    if (io_job.state != IO_LOADING) { mutex_unlock(&io_mutex); return; }
    ChunkIOJob job = io_job;
    mutex_unlock(&io_mutex);

    uint32_t remaining = job.total_bytes - job.progress;
    if (!remaining) return;

    uint32_t slice = MIN(remaining, (uint32_t)CHUNK_IO_SLICE);
    if (slice >= 32) slice &= ~31u; // keep big slices aligned

    mutex_lock(&file_mutex);
    if (!job.did_seek) {
        fs_seek(video_fd, job.cur_off, SEEK_SET);
        job.did_seek = 1;
    }
    ssize_t got = fs_read(video_fd, job.slot->data + job.progress, slice);
    mutex_unlock(&file_mutex);

    if (got != (ssize_t)slice) {
        printf("❌ [io] read chunk=%d got=%d exp=%u\n", job.chunk_id, (int)got, slice);
        mutex_lock(&io_mutex);  io_job.state = IO_IDLE;  mutex_unlock(&io_mutex);
        mutex_lock(&chunk_cache_mutex);
        atomic_store_explicit(&job.slot->valid, CHUNK_EMPTY, memory_order_release);
        job.slot->chunk_id = -1;
        mutex_unlock(&chunk_cache_mutex);
        return;
    }

    job.progress += slice;
    job.cur_off  += slice;

    mutex_lock(&io_mutex);
    if (io_job.state == IO_LOADING && io_job.chunk_id == job.chunk_id) {
        io_job.progress = job.progress;
        io_job.cur_off  = job.cur_off;
        io_job.did_seek = job.did_seek;

        if (io_job.progress >= io_job.total_bytes) {
            mutex_lock(&chunk_cache_mutex);
            chunk_finalise(io_job.slot, io_job.chunk_id,
                           io_job.video_real_bytes, io_job.video_disk_bytes);
            mutex_unlock(&chunk_cache_mutex);
            io_job.state = IO_IDLE;
        }
    }
    mutex_unlock(&io_mutex);
}

static void *io_thread(void *arg) {
    (void)arg;
    ThreadProf prof; prof_init(&prof, "io");
    int last_state = IO_IDLE;
    uint32_t last_io_seq = atomic_load_explicit(&io_wake_seq, memory_order_acquire);

    while (1) {
        double t0 = psTimer();
        if (!atomic_load_explicit(&io_enabled, memory_order_acquire)) {
            thd_pass();
            prof_tick(&prof, 0.0, psTimer() - t0);
            continue;
        }

        mutex_lock(&io_mutex);
        int busy = (io_job.state != IO_IDLE);
        int cur  = io_job.state;
        mutex_unlock(&io_mutex);

        if (last_state == IO_LOADING && cur == IO_IDLE) prof.c1++;
        last_state = cur;

        if (!busy) {
            wait_on_seq(&io_wake_seq, &last_io_seq, 0);
            prof_tick(&prof, 0.0, psTimer() - t0);
            continue;
        }

        double w0 = psTimer();
        io_pump_slice();
        prof.c0++;
        thd_pass();
        prof_tick(&prof, psTimer() - w0, 0.0);
    }
    return NULL;
}

// =============================================================================
// Synchronous chunk load (bootstrapping)
// =============================================================================

static int load_chunk_sync(int chunk_id, int pin0, int pin1, int pin2) {
    if (chunk_id < 0 || (uint32_t)chunk_id >= header.num_chunks) return -1;

    mutex_lock(&chunk_cache_mutex);
    for (int i = 0; i < CHUNK_CACHE_SIZE; i++) {
        if (!chunk_cache[i].data) continue;
        if (atomic_load_explicit(&chunk_cache[i].valid, memory_order_acquire) == CHUNK_READY &&
            chunk_cache[i].chunk_id == chunk_id) {
            chunk_cache[i].last_used = ++global_cache_tick;
            mutex_unlock(&chunk_cache_mutex);
            return 0;
        }
    }
    ChunkCache *slot = evict_slot(pin0, pin1, pin2);
    if (!slot) { mutex_unlock(&chunk_cache_mutex); return -1; }

    // Reserve slot for this chunk immediately (same reason as async path)
    slot->chunk_id = chunk_id;
    atomic_store_explicit(&slot->valid, CHUNK_EMPTY, memory_order_release);
    mutex_unlock(&chunk_cache_mutex);

    ChunkEntry *e     = &chunk_index[chunk_id];
    uint32_t vid_real = e->video_section_size;
    uint32_t vid_disk = vid_real + pad32_after(e->chunk_offset + vid_real);
    uint32_t aud_disk = align32(e->audio_size) * header.channels;
    uint32_t total    = vid_disk + aud_disk;

    if (total > slot->size) {
        printf("❌ [boot] chunk %d needs %u, slot has %u\n", chunk_id, total, slot->size);
        mutex_lock(&chunk_cache_mutex);
        slot->chunk_id = -1;
        mutex_unlock(&chunk_cache_mutex);
        return -1;
    }

    mutex_lock(&file_mutex);
    fs_seek(video_fd, e->chunk_offset, SEEK_SET);
    uint8_t *dst = slot->data; uint32_t left = total;
    while (left) {
        uint32_t n = MIN(left, 32768u);
        ssize_t got = fs_read(video_fd, dst, n);
        if (got <= 0) { mutex_unlock(&file_mutex); return -1; }
        dst += got; left -= (uint32_t)got;
        thd_pass();
    }
    mutex_unlock(&file_mutex);

    mutex_lock(&chunk_cache_mutex);
    chunk_finalise(slot, chunk_id, vid_real, vid_disk);
    mutex_unlock(&chunk_cache_mutex);
    return 0;
}

// =============================================================================
// Frame decompression
// =============================================================================

static int load_frame(int total_frame, int buf_index, int tid) {
    int chunk_id = find_chunk_for_frame(total_frame);
    ChunkCache *c = cache_acquire(chunk_id);
    if (!c) {
        int pc = find_chunk_for_frame(atomic_load(&frame_index));
        request_chunk_async(chunk_id, pc, pc+1, pc+2);
        return -1;
    }

    ChunkEntry *e = &chunk_index[chunk_id];
    int local = total_frame - (int)e->start_frame;
    if (local < 0 || (uint32_t)local >= e->num_frames || (uint32_t)local >= c->map_cap) {
        printf("❌ frame %d not in chunk %d\n", total_frame, chunk_id);
        cache_release(c); return -1;
    }

    uint32_t off = c->frame_off_local[local];
    uint32_t sz  = c->frame_sz_local[local];
    if (off + sz > c->video_bytes_size) {
        printf("❌ frame %d overflows chunk %d (off=%u sz=%u vid=%u)\n",
               total_frame, chunk_id, off, sz, c->video_bytes_size);
        cache_release(c); return -1;
    }

    const uint8_t *src = c->video_section + off;
    uint8_t       *dst = frame_buffer[buf_index];
    int ok = 0;

    if (header.compression_type == 1) {
        ZSTD_DCtx *dctx = (tid >= 0 && tid < DECODE_THREADS) ? g_zstd_dctx[tid] : NULL;
        if (!dctx) {
            printf("❌ ZSTD no dctx tf=%d tid=%d\n", total_frame, tid);
            ok = -1;
        } else {
            ZSTD_DCtx_reset(dctx, ZSTD_reset_session_only);
            ZSTD_DCtx_setParameter(dctx, ZSTD_d_format, ZSTD_f_zstd1_magicless);
            
            ZSTD_inBuffer  in  = { src, (size_t)sz, 0 };
            ZSTD_outBuffer out = { dst, (size_t)header.uncompressed_frame_size, 0 };
            
            size_t ret = 1;
            while (ret != 0 && out.pos < out.size) {
                ret = ZSTD_decompressStream(dctx, &out, &in);
                if (ZSTD_isError(ret)) {
                    printf("❌ ZSTD decompress error tf=%d: %s\n",
                        total_frame, ZSTD_getErrorName(ret));
                    ok = -1;
                    break;
                }
            }

            if (ok == 0 && out.pos != (size_t)header.uncompressed_frame_size) {
                printf("❌ ZSTD tf=%d decoded %zu/%u\n",
                    total_frame, out.pos, (unsigned)header.uncompressed_frame_size);
                ok = -1;
            }
        }
    } else {
        int out = LZ4_decompress_safe((const char *)src, (char *)dst,
                                      (int)sz, (int)header.uncompressed_frame_size);
        if (out != (int)header.uncompressed_frame_size) {
            printf("❌ LZ4 frame %d out=%d exp=%u\n",
                   total_frame, out, header.uncompressed_frame_size);
            ok = -1;
        }
    }
    cache_release(c);
    return ok;
}

// =============================================================================
// Audio helpers
// =============================================================================

static void write_silence(uintptr_t dst, size_t bytes) {
    // spu_memload requires multiples of 32 bytes.
    static uint8_t silence[AUDIO_BUFFER_SIZE] __attribute__((aligned(32)));
    bytes &= ~31u;
    while (bytes) {
        size_t n = MIN(bytes, sizeof(silence)) & ~31u;
        if (!n) break;
        // Silence buffer is static; safe to flush once in practice, but cheap here.
        dcache_flush_range((uint32)silence, (uint32)n);
        spu_memload(dst, silence, n);
        dst += n; bytes -= n;
    }
}

static void audio_seek_to_frame(int total_frame) {
    int cid = find_chunk_for_frame(total_frame);
    ChunkEntry *e = &chunk_index[cid];

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

    current_audio_chunk  = cid;
    audio_chunk_read_pos = pos;

    if (audio_chunk_read_pos >= e->audio_size && (uint32_t)(cid + 1) < header.num_chunks) {
        current_audio_chunk++;
        audio_chunk_read_pos = 0;
    }
}

static void audio_ring_clear(void) {
    g_audio_ring_read_pos = 0;
    atomic_store_explicit(&audio_read_idx,      0, memory_order_release);
    atomic_store_explicit(&audio_write_idx,     0, memory_order_release);
    atomic_store_explicit(&audio_refill_needed, 0, memory_order_release);
    for (int i = 0; i < AUDIO_RING_SIZE; i++) {
        atomic_store_explicit(&audio_ring[i].valid, 0, memory_order_release);
        audio_ring[i].valid_bytes = 0;
    }
}

static void refill_audio_ring(void) {
    const size_t WANT = ((size_t)AUDIO_BUFFER_SIZE) & ~31u;

    int wi  = atomic_load_explicit(&audio_write_idx, memory_order_acquire);
    int ri  = atomic_load_explicit(&audio_read_idx,  memory_order_acquire);
    int fill = (wi - ri + AUDIO_RING_SIZE) % AUDIO_RING_SIZE;
    int tgt  = target_audio_buffers();

    atomic_store_explicit(&audio_refill_needed, 0, memory_order_release);

    int tf = atomic_load_explicit(&frame_index, memory_order_acquire);
    int pc = find_chunk_for_frame(tf);

    while (fill < tgt) {
        int next = (wi + 1) % AUDIO_RING_SIZE;
        if (next == ri) break;
        if ((uint32_t)current_audio_chunk >= header.num_chunks) break;

        memset(audio_ring[wi].left, 0, AUDIO_BUFFER_SIZE);
        if (header.channels == 2) memset(audio_ring[wi].right, 0, AUDIO_BUFFER_SIZE);

        size_t done = 0;

        while (done < WANT) {
            if ((uint32_t)current_audio_chunk >= header.num_chunks) break;

            if (!is_cached_or_loading(current_audio_chunk))
                request_chunk_async(current_audio_chunk, current_audio_chunk, pc, pc + 1);
            if ((uint32_t)(current_audio_chunk + 1) < header.num_chunks &&
                !is_cached_or_loading(current_audio_chunk + 1))
                request_chunk_async(current_audio_chunk + 1, current_audio_chunk, pc, pc + 1);

            ChunkCache *c = cache_acquire(current_audio_chunk);
            if (!c) break;

            ChunkEntry *e = &chunk_index[current_audio_chunk];

            if (audio_chunk_read_pos >= e->audio_size) {
                cache_release(c);
                current_audio_chunk++;
                audio_chunk_read_pos = 0;
                continue;
            }

            size_t rem = (size_t)e->audio_size - audio_chunk_read_pos;

            if (rem < 32) {
                cache_release(c);
                current_audio_chunk++;
                audio_chunk_read_pos = 0;
                continue;
            }

            size_t need = WANT - done;
            size_t take = (rem < need) ? rem : need;
            take &= ~31u;

            if (!take) {
                cache_release(c);
                break;
            }

            memcpy(audio_ring[wi].left + done, c->audio_L + audio_chunk_read_pos, take);
            if (header.channels == 2)
                memcpy(audio_ring[wi].right + done, c->audio_R + audio_chunk_read_pos, take);

            audio_chunk_read_pos += take;
            done += take;

            if (audio_chunk_read_pos >= e->audio_size) {
                cache_release(c);
                current_audio_chunk++;
                audio_chunk_read_pos = 0;
                continue;
            }

            if ((uint32_t)(current_audio_chunk + 1) < header.num_chunks) {
                size_t left_in_chunk = (size_t)e->audio_size - audio_chunk_read_pos;
                if (left_in_chunk < (WANT * 3)) {
                    request_chunk_async(current_audio_chunk + 1,
                                        current_audio_chunk, pc, pc + 1);
                }
            }

            cache_release(c);
        }

        done &= ~31u;
        if (done < 32) break;

        audio_ring[wi].valid_bytes = done;

        // Flush the CPU-written region so SPU sees it.
        dcache_flush_range((uint32)audio_ring[wi].left,  (uint32)done);
        if (header.channels == 2)
            dcache_flush_range((uint32)audio_ring[wi].right, (uint32)done);

        atomic_store_explicit(&audio_ring[wi].valid, 1, memory_order_release);

        wi = next;
        atomic_store_explicit(&audio_write_idx, wi, memory_order_release);

        ri   = atomic_load_explicit(&audio_read_idx, memory_order_acquire);
        fill = (wi - ri + AUDIO_RING_SIZE) % AUDIO_RING_SIZE;
    }
}

static size_t audio_cb(snd_stream_hnd_t hnd, uintptr_t l, uintptr_t r, size_t req) {
    (void)hnd;

    int ch = (int)header.channels;
    if (ch != 1 && ch != 2) return 0;

    // Direct callback `req` is TOTAL bytes across channels (common KOS behavior).
    // Always honor 32-byte alignment for spu_memload.
    size_t req_total = req & ~31u;
    if (!req_total) return 0;

    size_t per_chan = (ch == 2) ? ((req_total / 2) & ~31u) : req_total;
    if (!per_chan) return 0;

    if (atomic_load_explicit(&audio_muted, memory_order_acquire)) {
        write_silence(l, per_chan);
        if (ch == 2) write_silence(r, per_chan);
        return (ch == 2) ? (per_chan * 2) : per_chan;
    }

    size_t pos = g_audio_ring_read_pos;
    size_t remain = per_chan;
    size_t copied = 0;

    while (remain) {
        int ri = atomic_load_explicit(&audio_read_idx, memory_order_acquire);

        if (!atomic_load_explicit(&audio_ring[ri].valid, memory_order_acquire)) {
            atomic_store_explicit(&audio_refill_needed, 1, memory_order_release);
            worker_signal();

            write_silence(l + copied, remain);
            if (ch == 2) write_silence(r + copied, remain);

            copied += remain;
            remain = 0;
            break;
        }

        size_t valid = audio_ring[ri].valid_bytes; // bytes PER CHANNEL
        if (pos >= valid) {
            atomic_store_explicit(&audio_ring[ri].valid, 0, memory_order_release);
            atomic_store_explicit(&audio_read_idx, (ri + 1) % AUDIO_RING_SIZE, memory_order_release);
            pos = 0;
            continue;
        }

        size_t to_copy = MIN(valid - pos, remain) & ~31u;
        if (!to_copy) {
            atomic_store_explicit(&audio_ring[ri].valid, 0, memory_order_release);
            atomic_store_explicit(&audio_read_idx, (ri + 1) % AUDIO_RING_SIZE, memory_order_release);
            pos = 0;
            continue;
        }

        // Ensure SPU reads fresh data (paranoia + avoids “stale cache” pops).
        dcache_flush_range((uint32)(audio_ring[ri].left + pos), (uint32)to_copy);
        spu_memload(l + copied, audio_ring[ri].left + pos, to_copy);

        if (ch == 2) {
            dcache_flush_range((uint32)(audio_ring[ri].right + pos), (uint32)to_copy);
            spu_memload(r + copied, audio_ring[ri].right + pos, to_copy);
        }

        pos    += to_copy;
        copied += to_copy;
        remain -= to_copy;

        if (pos >= valid) {
            atomic_store_explicit(&audio_ring[ri].valid, 0, memory_order_release);
            atomic_store_explicit(&audio_read_idx, (ri + 1) % AUDIO_RING_SIZE, memory_order_release);
            pos = 0;
        }
    }

    g_audio_ring_read_pos = pos;

    // Return TOTAL bytes provided (KOS expects total in stereo).
    return (ch == 2) ? (copied * 2) : copied;
}

// =============================================================================
// Decode thread pool
// =============================================================================

static int schedule_decode(int total_frame) {
    if (total_frame < 0 || total_frame >= (int)header.num_total_frames) return 0;
    int uid = total_to_unique(total_frame);
    int buf = uid % NUM_BUFFERS;
    int gen = atomic_load_explicit(&GSeekGeneration, memory_order_acquire);
    int st  = atomic_load_explicit(&buf_state[buf],     memory_order_acquire);
    int bu  = atomic_load_explicit(&buf_unique_id[buf], memory_order_acquire);

    if ((st == BUF_READY || st == BUF_LOADING || st == BUF_QUEUED) && bu == uid) return 0;

    if (st == BUF_READY && bu != uid) {
        int old_tf = atomic_load_explicit(&buf_total_frame[buf], memory_order_acquire);
        if (old_tf >= 0 && old_tf < total_frame) {
            atomic_store_explicit(&buf_state[buf], BUF_EMPTY, memory_order_release);
            st = BUF_EMPTY;
        }
    }

    int expected = BUF_EMPTY;
    if (!atomic_compare_exchange_strong(&buf_state[buf], &expected, BUF_QUEUED)) return 0;
    atomic_store_explicit(&buf_total_frame[buf], total_frame, memory_order_release);
    atomic_store_explicit(&buf_unique_id[buf],   uid,         memory_order_release);

    DecodeJob j = { total_frame, uid, buf, gen };
    if (!decode_q_push(&j)) {
        atomic_store_explicit(&buf_state[buf], BUF_EMPTY, memory_order_release);
        return 0;
    }
    return 1;
}

static void *decode_thread(void *arg) {
    DecodeThreadArg *A = (DecodeThreadArg *)arg;
    int tid = A ? A->tid : 0;

    ThreadProf prof; prof_init(&prof, "decode");
    uint32_t last_seq = atomic_load_explicit(&decode_wake_seq, memory_order_acquire);

    while (1) {
        double t0 = psTimer();
        int did_work = 0;
        double w0 = t0;

        for (int n = 0; n < 8; n++) {
            DecodeJob job;
            if (!decode_q_pop(&job)) break;
            did_work = 1;

            int cur_gen = atomic_load_explicit(&GSeekGeneration, memory_order_acquire);
            if (job.generation != cur_gen) {
                if (atomic_load_explicit(&buf_total_frame[job.buf], memory_order_acquire) == job.total_frame)
                    atomic_store_explicit(&buf_state[job.buf], BUF_EMPTY, memory_order_release);
                continue;
            }
            if (atomic_load_explicit(&buf_total_frame[job.buf], memory_order_acquire) != job.total_frame ||
                atomic_load_explicit(&buf_unique_id[job.buf],   memory_order_acquire) != job.unique_id) {
                atomic_store_explicit(&buf_state[job.buf], BUF_EMPTY, memory_order_release);
                continue;
            }
            int expected = BUF_QUEUED;
            if (!atomic_compare_exchange_strong(&buf_state[job.buf], &expected, BUF_LOADING)) continue;

            int ok = load_frame(job.total_frame, job.buf, tid);
            cur_gen = atomic_load_explicit(&GSeekGeneration, memory_order_acquire);
            if (ok == 0 && job.generation == cur_gen &&
                atomic_load_explicit(&buf_total_frame[job.buf], memory_order_acquire) == job.total_frame &&
                atomic_load_explicit(&buf_unique_id[job.buf],   memory_order_acquire) == job.unique_id) {
                atomic_store_explicit(&buf_state[job.buf], BUF_READY, memory_order_release);
                prof.c0++;
            } else {
                atomic_store_explicit(&buf_state[job.buf], BUF_EMPTY, memory_order_release);
            }
        }

        if (did_work) {
            thd_pass();
            prof_tick(&prof, psTimer() - w0, 0.0);
        } else {
            wait_on_seq(&decode_wake_seq, &last_seq, 0);
            prof_tick(&prof, 0.0, psTimer() - t0);
        }
    }
    return NULL;
}

static void decode_reset(void) {
    atomic_fetch_add_explicit(&GSeekGeneration, 1, memory_order_acq_rel);
    decode_q_flush();
    for (int i = 0; i < NUM_BUFFERS; i++) {
        atomic_store_explicit(&buf_state[i],       BUF_EMPTY, memory_order_release);
        atomic_store_explicit(&buf_total_frame[i], -1,        memory_order_release);
        atomic_store_explicit(&buf_unique_id[i],   -1,        memory_order_release);
    }
    pending_free_buf = last_unique_frame_drawn = -1;
}

// =============================================================================
// Worker thread: audio refill + chunk prefetch
// =============================================================================
static void *worker_thread(void *arg) {
    (void)arg;

    ThreadProf prof;
    prof_init(&prof, "worker");

    int last_prefetch_chunk = -2;
    uint32_t last_worker_seq =
        atomic_load_explicit(&worker_wake_seq, memory_order_acquire);

    while (1) {
        double loop_t0 = psTimer();
        double work_ms = 0.0;

        /* ------------------------------------------------------------ */
        /* Audio refill                                                  */
        /* ------------------------------------------------------------ */
        {
            double w0 = psTimer();

            int wi = atomic_load_explicit(&audio_write_idx, memory_order_acquire);
            int ri = atomic_load_explicit(&audio_read_idx,  memory_order_acquire);
            int buf = (wi - ri + AUDIO_RING_SIZE) % AUDIO_RING_SIZE;

            atomic_store_explicit(&audio_refill_needed, 0, memory_order_release);

            if (buf < target_audio_buffers()) {
                refill_audio_ring();
                prof.c0++;
            }

            work_ms += (psTimer() - w0);
        }

        /* ------------------------------------------------------------ */
        /* Chunk prefetch (with early boundary trigger)                 */
        /* ------------------------------------------------------------ */
        {
            double w0 = psTimer();

            int tf = atomic_load_explicit(&frame_index, memory_order_acquire);
            int play_chunk = find_chunk_for_frame(tf);
            int prefetch_chunk = play_chunk;

            if ((uint32_t)play_chunk < header.num_chunks) {
                ChunkEntry *e = &chunk_index[play_chunk];

                int local_frame = tf - (int)e->start_frame;
                if (local_frame < 0)
                    local_frame = 0;

                if ((uint32_t)local_frame < e->num_frames) {
                    if (e->num_frames > PREFETCH_BOUNDARY_FRAMES &&
                        (uint32_t)local_frame >= (e->num_frames - PREFETCH_BOUNDARY_FRAMES)) {
                        if ((uint32_t)(play_chunk + 1) < header.num_chunks) {
                            prefetch_chunk = play_chunk + 1;
                        }
                    }
                }
            }

            if (prefetch_chunk != last_prefetch_chunk) {
                prefetch_around(prefetch_chunk);
                last_prefetch_chunk = prefetch_chunk;
                prof.c1++;
            }

            work_ms += (psTimer() - w0);
        }

        /* ------------------------------------------------------------ */
        /* Idle wait                                                     */
        /* ------------------------------------------------------------ */
        double idle_t0 = psTimer();
        wait_on_seq(&worker_wake_seq, &last_worker_seq, WORKER_POLL_MS);
        double idle_ms = psTimer() - idle_t0;

        prof_tick(&prof, work_ms, idle_ms);

        (void)loop_t0;
    }

    return NULL;
}

// =============================================================================
// File loading
// =============================================================================

static long get_file_size(file_t fd) {
    long cur = fs_tell(fd); fs_seek(fd, 0, SEEK_END);
    long sz  = fs_tell(fd); fs_seek(fd, cur, SEEK_SET);
    return sz;
}

static int load_header(void) {
    fs_seek(video_fd, 0, SEEK_SET);
    if (fs_read(video_fd, &header, sizeof(header)) != (ssize_t)sizeof(header))
        { printf("❌ header read\n"); return -1; }
    if (memcmp(header.magic, DCMV_MAGIC, 4) != 0)
        { printf("❌ bad magic\n"); return -1; }
    if (header.version != 1)
        { printf("❌ bad version %lu\n", (unsigned long)header.version); return -1; }

    printf("📦 DCMV v1.0  %dx%d @ %.2f fps  Audio: %dHz %dch  "
           "Frames: %lu  Chunks: %lu (%.2fs)  Codec: %s\n",
           header.tex_width, header.tex_height, header.fps,
           header.sample_rate, header.channels,
           (unsigned long)header.num_total_frames,
           (unsigned long)header.num_chunks, header.chunk_duration,
           header.compression_type ? "Zstd" : "LZ4");
    return 0;
}

static int load_chunk_index(void) {
    long fsz      = get_file_size(video_fd);
    uint32_t need = header.num_chunks * 20u;
    uint8_t *raw  = (uint8_t *)malloc(need);
    if (!raw) { printf("❌ [idx] malloc\n"); return -1; }

    fs_seek(video_fd, header.chunk_index_offset, SEEK_SET);
    if (fs_read(video_fd, raw, need) != (ssize_t)need)
        { printf("❌ [idx] read\n"); free(raw); return -1; }

    ChunkEntry *ci = (ChunkEntry *)calloc(header.num_chunks, sizeof(*ci));
    if (!ci) { free(raw); return -1; }

    for (uint32_t i = 0; i < header.num_chunks; i++) {
        memcpy(&ci[i].chunk_offset,       raw + i*20 +  0, 4);
        memcpy(&ci[i].video_section_size, raw + i*20 +  4, 4);
        memcpy(&ci[i].audio_size,         raw + i*20 +  8, 4);
        memcpy(&ci[i].start_frame,        raw + i*20 + 12, 4);
        memcpy(&ci[i].num_frames,         raw + i*20 + 16, 4);
    }
    free(raw);

    uint32_t min_off = (uint32_t)sizeof(DCMVHeader) +
                       (header.num_unique_frames + 1) * 4u +
                        header.num_unique_frames * 2u;
    if (min_off < 0x80u) min_off = 0x80u;

    uint32_t prev = 0;
    for (uint32_t i = 0; i < header.num_chunks; i++) {
        ChunkEntry *e = &ci[i];
        uint64_t end = (uint64_t)e->chunk_offset + e->video_section_size +
                       pad32_after(e->chunk_offset + e->video_section_size) +
                       (uint64_t)align32(e->audio_size) * header.channels;
        if (e->chunk_offset < min_off || (long)e->chunk_offset >= fsz ||
            (i > 0 && e->chunk_offset < prev) ||
            !e->video_section_size || !e->audio_size || !e->num_frames ||
            e->start_frame >= header.num_total_frames || end > (uint64_t)fsz) {
            printf("❌ [idx] chunk %u invalid\n", i); free(ci); return -1;
        }
        prev = e->chunk_offset;
    }

    free(chunk_index);
    chunk_index = ci;

    printf("[idx] %lu chunks\n", (unsigned long)header.num_chunks);
    for (int i = 0; i < 5 && (uint32_t)i < header.num_chunks; i++) {
        ChunkEntry *e = &ci[i];
        printf("[pad] chunk %d off=%u vid=%u pad=%u aud=%u start=%u n=%u\n",
               i, e->chunk_offset, e->video_section_size,
               pad32_after(e->chunk_offset + e->video_section_size),
               e->audio_size, e->start_frame, e->num_frames);
    }
    return 0;
}

// =============================================================================
// PVR
// =============================================================================

static int init_pvr(void) {
    pvr_init_defaults();
    pvr_txr = pvr_mem_malloc(header.frame_type == 1
        ? header.tex_width * header.tex_height * 2
        : header.uncompressed_frame_size);
    if (!pvr_txr) return -1;

    int pot_w = 1, pot_h = 1;
    while (pot_w < header.tex_width)  pot_w <<= 1;
    while (pot_h < header.tex_height) pot_h <<= 1;
    int strided = (pot_w != header.tex_width || pot_h != header.tex_height);

    pvr_poly_cxt_t cxt;
    int fmt = (header.frame_type == 1) ? PVR_TXRFMT_YUV422 : PVR_TXRFMT_RGB565;

    if (strided) {
        fmt |= PVR_TXRFMT_VQ_ENABLE | (1 << 25) | PVR_TXRFMT_NONTWIDDLED;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, fmt, pot_w, pot_h, pvr_txr, PVR_FILTER_NEAREST);
        pvr_poly_compile(&poly_hdr, &cxt);
        PVR_SET(PVR_TEXTURE_MODULO, header.tex_width / 32);
        float uw = (float)header.content_width  / pot_w;
        float vh = (float)header.content_height / pot_h;
        vert[0]=(pvr_vertex_t){PVR_CMD_VERTEX,    0,   0,   1, 0,  0,  0xffffffff};
        vert[1]=(pvr_vertex_t){PVR_CMD_VERTEX,    640, 0,   1, uw, 0,  0xffffffff};
        vert[2]=(pvr_vertex_t){PVR_CMD_VERTEX,    0,   480, 1, 0,  vh, 0xffffffff};
        vert[3]=(pvr_vertex_t){PVR_CMD_VERTEX_EOL,640, 480, 1, uw, vh, 0xffffffff};
    } else {
        fmt |= PVR_TXRFMT_TWIDDLED | PVR_TXRFMT_VQ_ENABLE;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, fmt,
                         header.tex_width, header.tex_height, pvr_txr, PVR_FILTER_NEAREST);
        pvr_poly_compile(&poly_hdr, &cxt);
        float umin = (float)(header.tex_width  - header.content_width)  / (2.0f * header.tex_width);
        float vmin = (float)(header.tex_height - header.content_height) / (2.0f * header.tex_height);
        float umax = 1.f - umin, vmax = 1.f - vmin;
        vert[0]=(pvr_vertex_t){PVR_CMD_VERTEX,    0,   0,   1, umin,vmin,0xffffffff};
        vert[1]=(pvr_vertex_t){PVR_CMD_VERTEX,    640, 0,   1, umax,vmin,0xffffffff};
        vert[2]=(pvr_vertex_t){PVR_CMD_VERTEX,    0,   480, 1, umin,vmax,0xffffffff};
        vert[3]=(pvr_vertex_t){PVR_CMD_VERTEX_EOL,640, 480, 1, umax,vmax,0xffffffff};
    }
    return 0;
}

// =============================================================================
// Input
// =============================================================================

static void poll_input(void) {
    maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    if (!dev) return;
    cont_state_t *st = (cont_state_t *)maple_dev_status(dev);
    if (!st) return;
    static int held = 0;
    if (st->buttons & CONT_START) held = 1;
    else if (held) { printf("🛑 exit\n"); arch_exit(); }
}

// =============================================================================
// Playback clock + wait
// =============================================================================

static inline double playback_now_ms(void) {
    return psTimer() - atomic_load_explicit(&playback_t0_ms, memory_order_acquire);
}

static void fmv_wait_until(double deadline_wall_ms) {
    const double SLEEP_SAFE_MS  = 3.0;
    const double SPIN_MARGIN_MS = 1.5;
    while (1) {
        double remain = deadline_wall_ms - psTimer();
        if (remain <= 0.0) break;
        snd_stream_poll(stream);
        if      (remain > SLEEP_SAFE_MS)  thd_sleep(1);
        else if (remain > SPIN_MARGIN_MS) thd_pass();
        else                              { thd_pass(); break; }
    }
}

// =============================================================================
// Main tick
// =============================================================================

static void fmv_tick(void) {
    if (pending_free_buf >= 0) {
        if (atomic_load_explicit(&buf_state[pending_free_buf], memory_order_acquire) == BUF_READY)
            atomic_store_explicit(&buf_state[pending_free_buf], BUF_EMPTY, memory_order_release);
        pending_free_buf = -1;
    }

    int cur = atomic_load(&frame_index);
    if (cur >= (int)header.num_total_frames) return;

    int    active = atomic_load_explicit(&playback_started, memory_order_acquire);
    double aud    = active ? playback_now_ms() : 0.0;

    if (active) {
        double late = aud - (double)cur * frame_duration;
        if (late > 1.5 * frame_duration) {
            int target = MIN((int)(aud / frame_duration), (int)header.num_total_frames - 1);
            target = MIN(target, cur + 6);
            if (target > cur) {
                printf("[skip] cur=%d -> %d late=%.2fms aud=%.2f\n", cur, target, late, aud);
                cur = target;
                atomic_store(&frame_index, cur);
            }
        }
    }

    int uid = total_to_unique(cur);
    int buf = uid % NUM_BUFFERS;
    int st  = atomic_load_explicit(&buf_state[buf], memory_order_acquire);

    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);

    int displayed = 0;

    if (st == BUF_READY && atomic_load_explicit(&buf_unique_id[buf], memory_order_acquire) == uid) {
        if (uid != last_unique_frame_drawn) {
            dcache_flush_range((uint32)frame_buffer[buf], header.uncompressed_frame_size);
            pvr_txr_load_dma(frame_buffer[buf], pvr_txr,
                             header.uncompressed_frame_size, 1, NULL, 0);
            last_unique_frame_drawn = uid;
        }

        sq_fast_cpy((void *)SQ_MASK_DEST(PVR_TA_INPUT), &poly_hdr, sizeof(poly_hdr)/32);
        sq_fast_cpy((void *)SQ_MASK_DEST(PVR_TA_INPUT), vert,      sizeof(vert)/32);
        pending_free_buf = buf;

        if (!active) {
            atomic_store_explicit(&audio_muted, 1, memory_order_release);
            audio_ring_clear();
            audio_seek_to_frame(cur);
            refill_audio_ring();

            snd_stream_start_adpcm(stream, header.sample_rate,
                                   header.channels == 2 ? 1 : 0);
            thd_pass();

            double now = psTimer();
            atomic_store_explicit(&playback_t0_ms,   now, memory_order_release);
            atomic_store_explicit(&playback_started, 1,   memory_order_release);
            atomic_store_explicit(&audio_muted,      0,   memory_order_release);

            int wi = atomic_load(&audio_write_idx), ri = atomic_load(&audio_read_idx);
            printf("[sync] started tf=%d chunk=%d pos=%lu rb=%d\n",
                   cur, current_audio_chunk, (unsigned long)audio_chunk_read_pos,
                   (wi - ri + AUDIO_RING_SIZE) % AUDIO_RING_SIZE);
            active = 1; aud = 0.0;
        }

        atomic_store(&frame_index, cur + 1);
        displayed = 1;

        for (int i = 1; i <= PREFETCH_AHEAD; i++) {
            if (cur + i >= (int)header.num_total_frames) break;
            schedule_decode(cur + i);
        }
    } else {
        static int stall_count = 0;
        if ((stall_count++ % 30) == 0)
            printf("[stall] tf=%d uid=%d buf=%d st=%d\n", cur, uid, buf, st);
        schedule_decode(cur);
    }

    pvr_list_finish();
    pvr_scene_finish();

    double aud_now  = active ? playback_now_ms() : 0.0;
    int    next     = cur + (displayed ? 1 : 0);
    double next_tgt = (double)next * frame_duration;
    double wait     = next_tgt - aud_now;

    if (active && wait > 0.0)
        fmv_wait_until(psTimer() + wait);
    else {
        snd_stream_poll(stream);
        thd_pass();
        if (!active) fmv_wait_until(psTimer() + frame_duration);
    }

    if ((cur % 30) == 0) {
        int wi = atomic_load(&audio_write_idx), ri = atomic_load(&audio_read_idx);
        printf("[sync] tf=%d aud=%.2f exp=%.2f drift=%.2f to_next=%.2f rb=%d\n",
               cur, aud_now, (double)cur * frame_duration,
               aud_now - (double)cur * frame_duration, wait,
               (wi - ri + AUDIO_RING_SIZE) % AUDIO_RING_SIZE);
    }
}

// =============================================================================
// Sleep calibration
// =============================================================================

static void calibrate_sleep_quantum(void) {
    uint64_t sum = 0;
    const int n = 16;
    thd_sleep(1); thd_pass();
    for (int i = 0; i < n; i++) {
        uint64_t t0 = timer_us_gettime64();
        thd_sleep(1);
        sum += timer_us_gettime64() - t0;
        thd_pass();
    }
    g_sleep_quantum_ms = MAX(0.5, MIN((double)sum / (double)n / 1000.0, 50.0));
    printf("[timing] thd_sleep(1) ~= %.3f ms\n", g_sleep_quantum_ms);
}

// =============================================================================
// Main
// =============================================================================

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("🎬 DCMV v1.0 Player\n");

    video_fd = fs_open("/cd/movie.dcmv", O_RDONLY);
    if (video_fd >= 0) VIDEO_FILE = "/cd/movie.dcmv";
    else {
        video_fd = fs_open("/pc/movie.dcmv", O_RDONLY);
        if (video_fd >= 0) VIDEO_FILE = "/pc/movie.dcmv";
    }
    if (video_fd < 0) { printf("❌ movie.dcmv not found\n"); return -1; }
    printf("✅ %s\n", VIDEO_FILE);

    if (load_header() < 0) return -1;

    if (!VIDEO_FILE || strncmp(VIDEO_FILE, "/pc/", 4) != 0)
        vid_set_mode(DM_640x480, PM_RGB565);
    else
        printf("[boot] /pc: skipping vid_set_mode\n");

    // Zstd: one DCtx per decode thread (Zstd ctx is NOT thread-safe).
    if (header.compression_type == 1) {
        for (int i = 0; i < DECODE_THREADS; i++) {
            g_zstd_dctx[i] = ZSTD_createDCtx();
            if (!g_zstd_dctx[i]) { printf("❌ ZSTD_createDCtx[%d]\n", i); return -1; }
        }
    }

    fs_seek(video_fd, sizeof(DCMVHeader), SEEK_SET);
    frame_sizes     = (uint32_t *)malloc((header.num_unique_frames + 1) * 4);
    frame_durations = (uint16_t *)malloc(header.num_unique_frames * 2);
    if (!frame_sizes || !frame_durations) { printf("❌ frame table alloc\n"); return -1; }
    fs_read(video_fd, frame_sizes,     (header.num_unique_frames + 1) * 4);
    fs_read(video_fd, frame_durations,  header.num_unique_frames * 2);

    if (load_chunk_index() < 0) return -1;

    memset(chunk_cache, 0, sizeof(chunk_cache));
    if (init_chunk_cache() < 0) return -1;

    t2u_lut = (uint16_t *)malloc(header.num_total_frames * 2);
    if (!t2u_lut) { printf("❌ t2u_lut alloc\n"); return -1; }
    {
        int t = 0;
        for (uint32_t u = 0; u < header.num_unique_frames; u++) {
            uint16_t dur = frame_durations[u]; if (!dur) dur = 1;
            for (uint16_t rr = 0; rr < dur && t < (int)header.num_total_frames; rr++)
                t2u_lut[t++] = (uint16_t)u;
        }
        uint16_t last = header.num_unique_frames ? (uint16_t)(header.num_unique_frames - 1) : 0;
        while (t < (int)header.num_total_frames) t2u_lut[t++] = last;
    }

    for (int i = 0; i < NUM_BUFFERS; i++) {
        frame_buffer[i] = (uint8_t *)memalign(32, header.uncompressed_frame_size);
        if (!frame_buffer[i]) { printf("❌ frame_buffer[%d]\n", i); return -1; }
        atomic_store(&buf_state[i],       BUF_EMPTY);
        atomic_store(&buf_total_frame[i], -1);
        atomic_store(&buf_unique_id[i],   -1);
    }

    if (init_pvr() < 0) return -1;

    snd_stream_init_ex(header.channels, AUDIO_BUFFER_SIZE);
    stream = snd_stream_alloc(NULL, AUDIO_BUFFER_SIZE);
    snd_stream_set_callback_direct(stream, audio_cb);
    snd_stream_volume(stream, 255);

    atomic_store(&audio_muted,      1);
    atomic_store(&playback_started, 0);
    atomic_store_explicit(&playback_t0_ms, 0.0, memory_order_release);

    audio_ring_clear();
    current_audio_chunk = 0;
    audio_chunk_read_pos = 0;

    printf("🔄 Preloading chunks...\n");
    for (int i = 0; i < MIN(4, (int)header.num_chunks); i++) {
        if (load_chunk_sync(i, 0, 1, 2) < 0) {
            printf("❌ sync load chunk %d\n", i);
            return -1;
        }
    }

    thd_set_hz(1000);
    thd_set_prio(thd_current, 1);
    calibrate_sleep_quantum();

    atomic_store_explicit(&io_enabled, 1, memory_order_release);

    // ---- FIX: avoid priority collisions ----
    // decode: 2..(1+DECODE_THREADS)
    // worker: 2+DECODE_THREADS
    // io    : 3+DECODE_THREADS
    int pr_decode_base = 2;
    int pr_worker      = pr_decode_base + DECODE_THREADS;
    int pr_io          = pr_worker + 1;

    for (int i = 0; i < DECODE_THREADS; i++) {
        g_decode_args[i].tid = i;
        thd_create(pr_decode_base + i, decode_thread, &g_decode_args[i]);
    }
    thd_create(pr_worker, worker_thread, NULL);
    thd_create(pr_io,     io_thread,     NULL);

    for (int i = 3; i < MIN(3 + ACTIVE_CHUNK_CACHE_SLOTS - 3, (int)header.num_chunks); i++)
        request_chunk_async(i, 0, 1, 2);

    printf("🔄 Warming frame cache...\n");
    decode_reset();
    int warm = MIN(INITIAL_PRELOAD, (int)header.num_total_frames);
    for (int tf = 0; tf < warm; tf++) {
        int uf = total_to_unique(tf), b = uf % NUM_BUFFERS;
        if (atomic_load(&buf_unique_id[b]) == uf && atomic_load(&buf_state[b]) == BUF_READY) continue;
        atomic_store(&buf_state[b],       BUF_LOADING);
        atomic_store(&buf_total_frame[b], tf);
        atomic_store(&buf_unique_id[b],   uf);
        if (load_frame(tf, b, 0) == 0) atomic_store(&buf_state[b], BUF_READY);
        else                           atomic_store(&buf_state[b], BUF_EMPTY);
    }

    frame_duration = 1000.0 / (double)header.fps;
    printf("✅ Starting playback\n");

    while (atomic_load(&frame_index) < (int)header.num_total_frames) {
        fmv_tick();
        snd_stream_poll(stream);
        poll_input();
    }

    printf("🏁 Done\n");
    atomic_store(&audio_muted, 1);
    snd_stream_stop(stream);
    snd_stream_destroy(stream);
    fs_close(video_fd);

    for (int i = 0; i < CHUNK_CACHE_SIZE; i++) {
        free(chunk_cache[i].data);
        free(chunk_cache[i].frame_off_local);
        free(chunk_cache[i].frame_sz_local);
        free(chunk_cache[i].seen_u);
        free(chunk_cache[i].seen_off);
    }
    for (int i = 0; i < NUM_BUFFERS; i++) free(frame_buffer[i]);
    free(frame_sizes); free(frame_durations); free(chunk_index); free(t2u_lut);

    for (int i = 0; i < DECODE_THREADS; i++) {
        if (g_zstd_dctx[i]) ZSTD_freeDCtx(g_zstd_dctx[i]);
        g_zstd_dctx[i] = NULL;
    }

    arch_exit();
    return 0;
}