// NOTE: assumes audio_ring[].valid is an atomic_int (or _Atomic int)
// NOTE: assumes audio_ring[].valid is an atomic_int (or _Atomic int)
/**
 * fmv_play.c - Dreamcast FMV Player (DCMV v1.0 Chunked Format)
 * -----------------------------------------------------
 * NEW in v1.0:
 *  - Chunk-based container (time-based segments)
 *  - Chunk cache (3 chunks buffered in RAM)
 *  - Audio ring buffer (no more fs_read in callback!)
 *  - Optimized for CDR sequential reading
 *  - Still supports all v6 features (deduplication, sync, etc.)
 *
 * Author: Troy Davis (GPF) — https://github.com/GPF
 * License: Public Domain / MIT-style — use freely with attribution.
 */

#include <kos.h>
#include <kos/dbgio.h>
#include <dc/sound/stream.h>
#include <dc/sound/sound.h>
#include <dc/pvr.h>
#include <dc/maple/controller.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
// #include <fastmem/fastmem.h>
// #define LZ4_memcpy(d,s,n) memcpy_fast((d),(s),(n))
// #define LZ4_memmove(d,s,n) memmove_fast((d),(s),(n))
// #define LZ4_memset(d,s,n) memset_fast((d),(s),(n))
// #define LZ4_FREESTANDING 1
#include <lz4/lz4.h>
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd/zstd.h>

#define DCMV_MAGIC "DCMV"
static const char *VIDEO_FILE;

// ============================================================================
// V1.0 Header and Chunk Structures
// ============================================================================

typedef struct __attribute__((packed)) {
    char magic[4];
    uint32_t version;
    uint8_t frame_type;
    uint16_t tex_width;
    uint16_t tex_height;
    uint16_t content_width;
    uint16_t content_height;
    float fps;
    uint16_t sample_rate;
    uint16_t channels;
    uint32_t num_unique_frames;
    uint32_t num_total_frames;
    uint32_t uncompressed_frame_size;
    uint32_t max_compressed_frame_size;
    uint8_t compression_type;
    float chunk_duration;
    uint32_t num_chunks;
    uint32_t chunk_index_offset;
    uint8_t padding[10];
} DCMVHeader;

typedef struct __attribute__((packed)) {
    uint32_t chunk_offset;
    uint32_t video_section_size;
    uint32_t audio_size;     // per-channel bytes
    uint16_t start_frame;    // <-- was uint16_t
    uint16_t num_frames;     // <-- was uint16_t
} ChunkIndexEntry;


// ============================================================================
// Chunk Cache
// ============================================================================

#define CHUNK_CACHE_SIZE 4
#define MAX_CHUNK_SIZE (1536 * 1024)  // 1.5MB max per chunk

#define AUDIO_BUFFER_SIZE 4096 // buffer size
typedef struct {
    _Atomic int valid;
    int chunk_id;
    uint8_t *data;
    uint32_t size;

    uint8_t *video_section;
    uint32_t video_bytes_size;   // ✅ add this

    uint8_t *audio_L;
    uint8_t *audio_R;

    uint32_t last_used;          // ✅ add this (optional but recommended)
} ChunkCache;

static ChunkCache chunk_cache[CHUNK_CACHE_SIZE];
static uint32_t chunk_access_counter = 0;

// ============================================================================
// Async Chunk IO (NEW)
// ============================================================================

#define CHUNK_IO_SLICE (64 * 1024)   // 32k..128k are good knobs for GD/CD

typedef enum {
    IO_IDLE = 0,
    IO_LOADING = 1
} IOState;

typedef struct {
    IOState state;

    int chunk_id;
    int pin0, pin1, pin2;

    ChunkCache *slot;
    uint32_t file_off;
    uint32_t total_bytes;
    uint32_t progress;

    // precomputed publish info
    uint32_t video_real_bytes;
    uint32_t video_disk_bytes;
    uint32_t audio_disk_bytes;
} ChunkIOJob;

static mutex_t io_mutex = MUTEX_INITIALIZER;
static ChunkIOJob io_job = {0};

static _Atomic int io_wake = 0;

// Simple helper: tell IO thread to wake up
static inline void io_signal(void) {
    atomic_store(&io_wake, 1);
}

static _Atomic int audio_refill_needed = 0;

// Tune these if you want
#define AUDIO_REFILL_LOW_WATER   2   // request refill when <= this many buffers remain
#define AUDIO_REFILL_BURST_MAX   8   // max refill passes per worker tick
// ============================================================================
// Audio Ring Buffer
// ============================================================================

#define AUDIO_RING_SIZE 48

typedef struct {
    uint8_t left[AUDIO_BUFFER_SIZE] __attribute__((aligned(32)));   // ✅ 32-byte aligned
    uint8_t right[AUDIO_BUFFER_SIZE] __attribute__((aligned(32)));  // ✅ 32-byte aligned
    size_t  valid_bytes;   // bytes valid in left/right for this entry (<= AUDIO_BUFFER_SIZE)
    _Atomic int valid;
} AudioBuffer;


static AudioBuffer audio_ring[AUDIO_RING_SIZE] __attribute__((aligned(32)));  // ✅ 32-byte aligned
static atomic_int audio_write_idx = 0;
static atomic_int audio_read_idx = 0;
static size_t audio_chunk_read_pos = 0;  // Position within current audio chunk
static int current_audio_chunk = 0;

#define BYTES_PER_SAMPLE 2
#define TARGET_AUDIO_BUFFER_MS 120.0

// ✅ Alignment helper macro for 32-byte boundaries
#define ALIGN_32(x) (((x) + 31) & ~31)

static inline int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}
// ============================================================================
// Global State (similar to v6)
// ============================================================================

static file_t video_fd = -1;
static ZSTD_DCtx *dctx = NULL;

// Header data
static DCMVHeader header;
static ChunkIndexEntry *chunk_index = NULL;
static uint32_t *frame_offsets = NULL;
static uint32_t *frame_prefix = NULL; // ✅ new prefix sum array for frame offsets
static uint16_t *frame_durations = NULL;
static uint16_t *t2u_lut = NULL;
snd_stream_hnd_t stream;

static inline double audio_entry_ms(void) {
    // AUDIO_BUFFER_SIZE bytes per channel (16-bit samples)
    return (1000.0 * (double)AUDIO_BUFFER_SIZE) /
           ((double)header.sample_rate * (double)BYTES_PER_SAMPLE);
}

// Video state
static uint8_t *compressed_buffer = NULL;
static pvr_ptr_t pvr_txr;
static pvr_poly_hdr_t hdr;
static pvr_vertex_t vert[4];

// Playback state
static atomic_int frame_index = 0;
static atomic_int audio_muted = 0;
static atomic_int seek_request = -1;
static _Atomic double audio_start_time_ms = 0.0;
static double frame_timer_anchor = 0.0;
static double frame_duration = 0.0;
static int last_unique_frame_drawn = -1;
static int pending_free_buf = -1;  // buffer whose DMA hasn't completed yet
static _Atomic int displayed_total_frame = 0;
static uint32_t vfd_last_end = 0;

enum BufState { BUF_EMPTY = 0, BUF_LOADING = 1, BUF_READY = 2 };

#define NUM_BUFFERS 30
#define RING_CAPACITY (NUM_BUFFERS + 1)
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))

static uint8_t *frame_buffer[NUM_BUFFERS];
static _Atomic int buf_state[NUM_BUFFERS];
// Preload ring (same as v6)
typedef struct {
    int frame;
    int generation;
} PreloadJob;

static PreloadJob preload_ring[RING_CAPACITY];
static int GSeekGeneration = 0;
static atomic_int preload_ring_head = 0;
static atomic_int preload_ring_tail = 0;

#define PREFETCH_AHEAD (NUM_BUFFERS - 3)
#define INITIAL_PRELOAD (NUM_BUFFERS)

static int unique_display_count = 0;
static int expected_display_count = 0;

char screenshotfilename[256];

// ============================================================================
// Helper Functions
// ============================================================================

static inline float psTimer(void) {
    #define AICA_MEM_CLOCK 0x021000
    uint32_t jiffies = g2_read_32(SPU_RAM_UNCACHED_BASE + AICA_MEM_CLOCK);
    const float AICA_TICKS_PER_MS = 4.410f; 
    return jiffies / AICA_TICKS_PER_MS;
}

static inline int total_to_unique_frame(int total_frame) {
    if (total_frame < 0 || total_frame >= (int)header.num_total_frames)
        return 0;
    return t2u_lut[total_frame];
}

static inline int find_chunk_for_frame(int total_frame) {
    // Robust for 23.976, variable chunk sizes, or packer-side rounding.
    if (header.num_chunks == 0 || !chunk_index)
        return 0;

    // Clamp frame into valid range (defensive)
    if (total_frame < 0) total_frame = 0;
    if (total_frame >= (int)header.num_total_frames)
        total_frame = (int)header.num_total_frames - 1;

    for (uint32_t i = 0; i < header.num_chunks; i++) {
        const ChunkIndexEntry *e = &chunk_index[i];
        int start = (int)e->start_frame;
        int end   = start + (int)e->num_frames;
        if (total_frame >= start && total_frame < end)
            return (int)i;
    }

    // If we didn't find it (corrupt index / edge rounding), clamp to last chunk.
    return (int)(header.num_chunks - 1);
}


// ============================================================================
// Chunk Cache Management (FIXED: only use ACTIVE slots + correct pad math)
// ============================================================================
static uint32_t global_cache_tick = 0;
static mutex_t file_mutex        = MUTEX_INITIALIZER;
static mutex_t chunk_cache_mutex = MUTEX_INITIALIZER;
static _Atomic int chunk_prefetch_request = -1;

static inline uint32_t align32_u32(uint32_t x) {
    return (x + 31u) & ~31u;
}


static inline uint32_t pad32_after(uint32_t abs_end) {
    return (uint32_t)((32u - (abs_end & 31u)) & 31u);
}

static inline uint32_t video_disk_bytes_for_chunk(const ChunkIndexEntry *e) {
    uint32_t end_of_video = e->chunk_offset + e->video_section_size;
    return e->video_section_size + pad32_after(end_of_video);
}

// How many cache slots we *actually* keep allocated to avoid heap fragmentation.
// 2 is usually enough (current + next).
#ifndef ACTIVE_CHUNK_CACHE_SLOTS
#define ACTIVE_CHUNK_CACHE_SLOTS 4
#endif

static int active_cache_slots(void) {
    int slots = ACTIVE_CHUNK_CACHE_SLOTS;
    if (slots < 1) slots = 1;
    if (slots > CHUNK_CACHE_SIZE) slots = CHUNK_CACHE_SIZE;
    return slots;
}

// -----------------------------------------------------------------------------
// get_cached_chunk (thread-safe for _Atomic valid)
//  - returns only READY chunks (valid == 1)
//  - uses acquire so readers see fully-published pointers/fields
// -----------------------------------------------------------------------------
static ChunkCache* get_cached_chunk(int chunk_id) {
    const int CACHE_SLOTS = (int)(sizeof(chunk_cache) / sizeof(chunk_cache[0]));

    for (int i = 0; i < CACHE_SLOTS; i++) {
        if (!chunk_cache[i].data) continue;  // inactive/disabled slot

        int v = atomic_load_explicit(&chunk_cache[i].valid, memory_order_acquire);
        if (v == 1 && chunk_cache[i].chunk_id == chunk_id) {
            return &chunk_cache[i];
        }
    }
    return NULL;
}

// -----------------------------------------------------------------------------
// evict_and_get_slot_for_chunk_pinned (updated for _Atomic valid)
//  - MUST be called with chunk_cache_mutex held
//  - never returns a LOADING slot (valid == -1)
//  - will not evict pinned chunks
// -----------------------------------------------------------------------------
static ChunkCache *evict_and_get_slot_for_chunk_pinned(int want_chunk,
                                                       int pin0, int pin1, int pin2) {
    (void)want_chunk;
    const int CACHE_SLOTS = (int)(sizeof(chunk_cache) / sizeof(chunk_cache[0]));

    // 1) Prefer an empty slot (valid==0) among active slots
    for (int i = 0; i < CACHE_SLOTS; i++) {
        if (chunk_cache[i].data == NULL) continue; // inactive/disabled slot

        int v = atomic_load_explicit(&chunk_cache[i].valid, memory_order_relaxed);
        if (v == 0) {
            return &chunk_cache[i];
        }
    }

    // 2) Otherwise evict LRU, but NEVER evict pinned chunks.
    //    Also never evict LOADING (valid == -1).
    int lru = -1;
    uint32_t best = 0xFFFFFFFF;

    for (int i = 0; i < CACHE_SLOTS; i++) {
        if (chunk_cache[i].data == NULL) continue;

        int v = atomic_load_explicit(&chunk_cache[i].valid, memory_order_relaxed);
        if (v != 1) continue; // only consider READY chunks

        int cid = chunk_cache[i].chunk_id;
        if (cid == pin0 || cid == pin1 || cid == pin2)
            continue;

        if (chunk_cache[i].last_used < best) {
            best = chunk_cache[i].last_used;
            lru = i;
        }
    }

    if (lru < 0) {
        // Everything is pinned or LOADING. ACTIVE_SLOTS may be too small.
        return NULL;
    }

    // Invalidate first (publish that it's unusable), then scrub pointers.
    atomic_store_explicit(&chunk_cache[lru].valid, 0, memory_order_release);

    chunk_cache[lru].chunk_id         = -1;
    chunk_cache[lru].video_section    = NULL;
    chunk_cache[lru].audio_L          = NULL;
    chunk_cache[lru].audio_R          = NULL;
    chunk_cache[lru].video_bytes_size = 0;
    chunk_cache[lru].last_used        = 0;

    return &chunk_cache[lru];
}



static uint32_t compute_max_chunk_bytes_on_disk(void) {
    uint32_t max_bytes = 0;

    for (uint32_t i = 0; i < header.num_chunks; i++) {
        ChunkIndexEntry *e = &chunk_index[i];

        // On disk, video is padded to 32 before audio.
        uint32_t video_disk = video_disk_bytes_for_chunk(e);
        uint32_t audio_disk = e->audio_size * (uint32_t)header.channels;

        uint32_t total_disk = video_disk + audio_disk;
        if (total_disk > max_bytes)
            max_bytes = total_disk;
    }

    // Extra slack
    return ALIGN_32(max_bytes + 64);
}

static int init_chunk_cache_buffers(void) {
    const int ACTIVE_SLOTS = active_cache_slots(); // <-- use the function

    uint32_t max_chunk = compute_max_chunk_bytes_on_disk();
    printf("[cache] max chunk bytes (disk, incl pad+audio) = %u, active slots=%d\n",
           (unsigned)max_chunk, ACTIVE_SLOTS);

    for (int i = 0; i < CHUNK_CACHE_SIZE; i++) {
        // Disable extra slots beyond ACTIVE_SLOTS to save RAM
        if (i >= ACTIVE_SLOTS) {
            if (chunk_cache[i].data) {
                free(chunk_cache[i].data);
                chunk_cache[i].data = NULL;
            }
            chunk_cache[i].valid = 0;
            chunk_cache[i].chunk_id = -1;
            chunk_cache[i].size = 0;
            chunk_cache[i].video_section = NULL;
            chunk_cache[i].audio_L = NULL;
            chunk_cache[i].audio_R = NULL;
            chunk_cache[i].last_used = 0;
            continue;
        }

        if (chunk_cache[i].data) {
            free(chunk_cache[i].data);
            chunk_cache[i].data = NULL;
        }

        chunk_cache[i].data = (uint8_t *)memalign(32, max_chunk);
        if (!chunk_cache[i].data) {
            printf("❌ [cache] memalign failed for slot %d (%u bytes)\n",
                   i, (unsigned)max_chunk);
            return -1;
        }

        chunk_cache[i].size = max_chunk;
        chunk_cache[i].valid = 0;
        chunk_cache[i].chunk_id = -1;
        chunk_cache[i].last_used = 0;
        chunk_cache[i].video_section = NULL;
        chunk_cache[i].audio_L = NULL;
        chunk_cache[i].audio_R = NULL;
        chunk_cache[i].video_bytes_size = 0;
    }

    return 0;
}

static int request_chunk_async(int chunk_id, int pin0, int pin1, int pin2) {
    if (chunk_id < 0 || (uint32_t)chunk_id >= header.num_chunks)
        return -1;

    // already cached?
    mutex_lock(&chunk_cache_mutex);
    ChunkCache *cached = get_cached_chunk(chunk_id);
    if (cached) {
        cached->last_used = ++global_cache_tick;
        mutex_unlock(&chunk_cache_mutex);
        return 0;
    }
    mutex_unlock(&chunk_cache_mutex);

    // already loading this chunk?
    mutex_lock(&io_mutex);
    if (io_job.state == IO_LOADING && io_job.chunk_id == chunk_id) {
        mutex_unlock(&io_mutex);
        return 0;
    }
    mutex_unlock(&io_mutex);

    // Create a new IO job if idle
    mutex_lock(&io_mutex);
    if (io_job.state != IO_IDLE) {
        // IO busy — just bail; we'll request again next tick/worker pass
        mutex_unlock(&io_mutex);
        return 0;
    }

    // Choose an evictable slot (respect pins)
    mutex_lock(&chunk_cache_mutex);
    ChunkCache *slot = evict_and_get_slot_for_chunk_pinned(chunk_id, pin0, pin1, pin2);
    if (!slot || !slot->data) {
        mutex_unlock(&chunk_cache_mutex);
        mutex_unlock(&io_mutex);
        return -1;
    }

    // Invalidate slot now (publish unusable)
    atomic_store_explicit(&slot->valid, 0, memory_order_release);
    slot->chunk_id = -1;
    slot->video_section = NULL;
    slot->audio_L = NULL;
    slot->audio_R = NULL;
    slot->video_bytes_size = 0;

    mutex_unlock(&chunk_cache_mutex);

    ChunkIndexEntry *entry = &chunk_index[chunk_id];

    uint32_t video_real = entry->video_section_size;
    uint32_t pad_bytes  = pad32_after(entry->chunk_offset + video_real);
    uint32_t video_disk = video_real + pad_bytes;
    uint32_t audio_disk = entry->audio_size * (uint32_t)header.channels;
    uint32_t total_disk = video_disk + audio_disk;

    if (total_disk > slot->size) {
        mutex_unlock(&io_mutex);
        printf("❌ [io] chunk %d needs %u bytes but slot has %u\n",
               chunk_id, (unsigned)total_disk, (unsigned)slot->size);
        return -1;
    }

    // Fill IO job
    io_job.state          = IO_LOADING;
    io_job.chunk_id       = chunk_id;
    io_job.pin0           = pin0;
    io_job.pin1           = pin1;
    io_job.pin2           = pin2;
    io_job.slot           = slot;
    io_job.file_off       = entry->chunk_offset;
    io_job.total_bytes    = total_disk;
    io_job.progress       = 0;

    io_job.video_real_bytes = video_real;
    io_job.video_disk_bytes = video_disk;
    io_job.audio_disk_bytes = audio_disk;

    mutex_unlock(&io_mutex);

    io_signal();
    return 0;
}


static void prefetch_chunks_around_play(int play_chunk) {
    int pin0 = play_chunk;
    int pin1 = play_chunk + 1;
    int pin2 = play_chunk + 2;

    // Request only what we *might* need soon.
    // IMPORTANT: do NOT request 4 chunks every time; that just saturates IO.
    if ((uint32_t)pin0 < header.num_chunks) request_chunk_async(pin0, pin0, pin1, pin2);
    if ((uint32_t)pin1 < header.num_chunks) request_chunk_async(pin1, pin0, pin1, pin2);
    if ((uint32_t)pin2 < header.num_chunks) request_chunk_async(pin2, pin0, pin1, pin2);

    // "next+1" is optional — request it only if you have spare cache slots
    int pin3 = play_chunk + 3;
    if ((uint32_t)pin3 < header.num_chunks) {
        int active = 0;
        for (int i = 0; i < CHUNK_CACHE_SIZE; i++) if (chunk_cache[i].data) active++;
        if (active >= 4) request_chunk_async(pin3, pin0, pin1, pin2);
    }
}


static void chunk_io_pump_one_slice(void) {
    mutex_lock(&io_mutex);
    if (io_job.state != IO_LOADING) {
        mutex_unlock(&io_mutex);
        return;
    }

    ChunkIOJob job = io_job; // local copy (slot pointer ok)
    mutex_unlock(&io_mutex);

    uint32_t remaining = job.total_bytes - job.progress;
    if (remaining == 0) return;

    uint32_t slice = remaining;
    if (slice > CHUNK_IO_SLICE) slice = CHUNK_IO_SLICE;

    // 32-byte align slice down (CD drivers tend to like it; also matches your audio alignment habits)
    slice &= ~31u;
    if (slice == 0) slice = remaining; // fallback

    // Do a small read
    mutex_lock(&file_mutex);
    fs_seek(video_fd, job.file_off + job.progress, SEEK_SET);
    ssize_t got = fs_read(video_fd, job.slot->data + job.progress, slice);
    mutex_unlock(&file_mutex);

    if (got != (ssize_t)slice) {
        printf("❌ [io] fs_read chunk=%d got=%d expected=%u (progress=%u)\n",
               job.chunk_id, (int)got, (unsigned)slice, (unsigned)job.progress);

        // abort job
        mutex_lock(&io_mutex);
        io_job.state = IO_IDLE;
        mutex_unlock(&io_mutex);
        return;
    }

    // Update progress
    mutex_lock(&io_mutex);
    if (io_job.state == IO_LOADING && io_job.chunk_id == job.chunk_id) {
        io_job.progress += slice;

        // done?
        if (io_job.progress >= io_job.total_bytes) {
            int chunk_id = io_job.chunk_id;
            ChunkCache *slot = io_job.slot;

            // Publish pointers + valid
            mutex_lock(&chunk_cache_mutex);

            ChunkIndexEntry *entry = &chunk_index[chunk_id];

            slot->chunk_id         = chunk_id;
            slot->video_section    = slot->data;
            slot->video_bytes_size = io_job.video_real_bytes;

            slot->audio_L          = slot->data + io_job.video_disk_bytes;
            slot->audio_R          = (header.channels == 2)
                                       ? (slot->audio_L + entry->audio_size)
                                       : NULL;

            slot->last_used        = ++global_cache_tick;

            atomic_store_explicit(&slot->valid, 1, memory_order_release);

            mutex_unlock(&chunk_cache_mutex);

            // Clear job
            io_job.state = IO_IDLE;

            // (optional) debug
            // printf("[io] chunk %d loaded (%u bytes)\n", chunk_id, (unsigned)io_job.total_bytes);
        }
    }
    mutex_unlock(&io_mutex);
}

static void *io_thread(void *arg) {
    (void)arg;

    while (1) {
        // Sleep until nudged (cheap)
        if (!atomic_exchange(&io_wake, 0)) {
            thd_sleep(1);
        }

        // Pump a few slices then yield
        for (int i = 0; i < 4; i++) {
            chunk_io_pump_one_slice();

            // let main breathe
            thd_pass();

            // if job is idle now, stop early
            mutex_lock(&io_mutex);
            int idle = (io_job.state == IO_IDLE);
            mutex_unlock(&io_mutex);
            if (idle) break;
        }
    }
    return NULL;
}



// ============================================================================
// Frame Loading (from chunk cache)
// ============================================================================
static int load_frame(int total_frame, int buf_index) {
    int chunk_id = find_chunk_for_frame(total_frame);

    ChunkCache *cache = get_cached_chunk(chunk_id);
    if (!cache) {
        // Ask IO thread for it
        int tf_now = atomic_load(&frame_index);
        int play_chunk = find_chunk_for_frame(tf_now);
        request_chunk_async(chunk_id, play_chunk, play_chunk+1, play_chunk+2);
        return -1;
    }

    int v = atomic_load_explicit(&cache->valid, memory_order_acquire);
    if (v != 1) {
        int tf_now = atomic_load(&frame_index);
        int play_chunk = find_chunk_for_frame(tf_now);
        request_chunk_async(chunk_id, play_chunk, play_chunk+1, play_chunk+2);
        return -1;
    }

    ChunkIndexEntry *entry = &chunk_index[chunk_id];

    int local = total_frame - (int)entry->start_frame;
    if (local < 0 || local >= (int)entry->num_frames) {
        printf("❌ frame %d not in chunk %d (start=%u num=%u)\n",
               total_frame, chunk_id, entry->start_frame, entry->num_frames);
        return -1;
    }

    // frame_offsets[] holds per-frame compressed sizes.
    // Sum the sizes of all prior frames in this chunk to get the byte offset.
    int base = (int)entry->start_frame;
    uint32_t offset_in_chunk = frame_prefix[base + local] - frame_prefix[base];
    uint32_t compressed_size = frame_offsets[base + local];

    if (compressed_size == 0) {
        printf("❌ compressed_size==0 for frame %d\n", total_frame);
        return -1;
    }

    if (offset_in_chunk + compressed_size > cache->video_bytes_size) {
        printf("❌ frame %d overflows chunk: off=%lu size=%lu vs %lu\n",
               total_frame, offset_in_chunk, compressed_size, cache->video_bytes_size);
        return -1;
    }

    const uint8_t *src = cache->video_section + offset_in_chunk;
    uint8_t *dst = (uint8_t *)frame_buffer[buf_index];

    if (header.compression_type == 1) { // Zstd
        ZSTD_DCtx_reset(dctx, ZSTD_reset_session_only);
        ZSTD_inBuffer in  = { src, compressed_size, 0 };
        ZSTD_outBuffer out = { dst, header.uncompressed_frame_size, 0 };
        size_t ret = 1;
        while (ret != 0 && out.pos < out.size) {
            ret = ZSTD_decompressStream(dctx, &out, &in);
            if (ZSTD_isError(ret)) {
                printf("❌ ZSTD error frame %d: %s\n", total_frame, ZSTD_getErrorName(ret));
                return -1;
            }
        }
        if (out.pos != header.uncompressed_frame_size) {
            printf("❌ ZSTD size mismatch frame %d\n", total_frame);
            return -1;
        }
    } else { // LZ4
        int out_bytes = LZ4_decompress_safe(
            (const char *)src, (char *)dst,
            (int)compressed_size, (int)header.uncompressed_frame_size
        );
        if (out_bytes != (int)header.uncompressed_frame_size) {
            printf("❌ LZ4 failed frame %d (out=%d expected=%lu in=%lu)\n",
                   total_frame, out_bytes, header.uncompressed_frame_size, compressed_size);
            return -1;
        }
    }
            // ✅ Upload to PVR texture NOW, during preload
        // Ensure the CPU data cache is flushed before DMA reads this buffer.
        // (KOS dcache_flush_range takes address + byte count.)
        // dcache_flush_range((uint32)frame_buffer[buf_index], (uint32)header.uncompressed_frame_size);

        // // DMA upload MUST be synchronized before we mark the buffer READY,
        // // otherwise the draw can sample an in-flight / partially written texture
        // // (green flashes / flicker).
        // pvr_txr_load_dma(frame_buffer[buf_index], pvr_txr,
        //                  header.uncompressed_frame_size,
        //                  1,  // sync
        //                  NULL, 0);
    return 0;
}

// ============================================================================
// Audio Ring Buffer Management (FIXED VERSION - 32-byte aligned for ADPCM)
// ============================================================================

static inline int target_audio_buffers(void) {
    double entry_ms = audio_entry_ms();
    int n = (int)((TARGET_AUDIO_BUFFER_MS + entry_ms - 1.0) / entry_ms); // ceil
    if (n < 2) n = 2;
    if (n > (AUDIO_RING_SIZE - 2)) n = (AUDIO_RING_SIZE - 2);
    return n;
}

static void refill_audio_ring(void) {
    int write_idx = atomic_load(&audio_write_idx);
    int read_idx  = atomic_load(&audio_read_idx);
    int buffered  = (write_idx - read_idx + AUDIO_RING_SIZE) % AUDIO_RING_SIZE;

    const int target = target_audio_buffers();

    while (buffered < target) {

        ChunkCache *cache = get_cached_chunk(current_audio_chunk);
        if (!cache) break;

        int v = atomic_load_explicit(&cache->valid, memory_order_acquire);
        if (v != 1) break;

        ChunkIndexEntry *entry = &chunk_index[current_audio_chunk];

        if (audio_chunk_read_pos >= entry->audio_size) {
            current_audio_chunk++;
            if (current_audio_chunk >= (int)header.num_chunks)
                break;

            audio_chunk_read_pos = 0;
            continue;
        }

        size_t remaining  = entry->audio_size - audio_chunk_read_pos;
        size_t take_raw   = (remaining < AUDIO_BUFFER_SIZE) ? remaining : AUDIO_BUFFER_SIZE;

        // pad up to 32 for spu_memload, but do NOT exceed buffer size
        size_t take_aligned = (take_raw + 31) & ~31u;
        if (take_aligned > AUDIO_BUFFER_SIZE) take_aligned = AUDIO_BUFFER_SIZE;

        // clear whole buffer (or at least the padded tail)
        memset(audio_ring[write_idx].left, 0, AUDIO_BUFFER_SIZE);
        if (header.channels == 2) memset(audio_ring[write_idx].right, 0, AUDIO_BUFFER_SIZE);

        // copy only real bytes
        memcpy(audio_ring[write_idx].left,
               cache->audio_L + audio_chunk_read_pos,
               take_raw);

        if (header.channels == 2) {
            memcpy(audio_ring[write_idx].right,
                   cache->audio_R + audio_chunk_read_pos,
                   take_raw);
        }

        // IMPORTANT: consume real bytes, not aligned bytes
        audio_chunk_read_pos += take_raw;

        // Store how many bytes are safe to memload (aligned up)
        audio_ring[write_idx].valid_bytes = take_aligned;

        // Publish this entry first, then advance the global write index.
        atomic_store_explicit(&audio_ring[write_idx].valid, 1, memory_order_release);

        int next = (write_idx + 1) % AUDIO_RING_SIZE;
        atomic_store_explicit(&audio_write_idx, next, memory_order_release);

        write_idx = next;   // ✅ advance ONCE
        buffered++;
    }
}


// ============================================================================
// Audio Callback helpers (atomic-safe ring)
// ============================================================================

static void write_silence(uintptr_t dst, size_t bytes) {
    static uint8_t silence[4096] __attribute__((aligned(32))) = {0};

    while (bytes) {
        size_t n = (bytes > sizeof(silence)) ? sizeof(silence) : bytes;
        n &= ~31u;                 // spu_memload wants 32B multiples
        if (!n) break;
        spu_memload(dst, silence, n);
        dst   += n;
        bytes -= n;
    }
}

static size_t audio_cb(snd_stream_hnd_t hnd, uintptr_t l, uintptr_t r, size_t req) {
    (void)hnd;

    const int ch = (int)header.channels;
    if (ch <= 0) return req;

    // req is TOTAL bytes for the callback. Split per channel.
    size_t per_chan = req / (size_t)ch;

    // Only ever spu_memload multiples of 32
    per_chan &= ~31u;
    if (per_chan == 0) return req;

    if (atomic_load_explicit(&audio_muted, memory_order_relaxed)) {
        write_silence(l, per_chan);
        if (ch == 2) write_silence(r, per_chan);
        return req;
    }

    static size_t ring_read_pos = 0;   // offset within current ring entry (per channel)
    size_t remaining = per_chan;
    size_t copied = 0;

    while (remaining > 0) {
        int read_idx  = atomic_load_explicit(&audio_read_idx,  memory_order_acquire);
        int write_idx = atomic_load_explicit(&audio_write_idx, memory_order_acquire);

        if (read_idx == write_idx) {
            // underrun -> silence remainder
            write_silence(l + copied, remaining);
            if (ch == 2) write_silence(r + copied, remaining);
            return req;
        }

        // MUST read valid atomically (producer publishes with release)
        int v = atomic_load_explicit(&audio_ring[read_idx].valid, memory_order_acquire);
        if (v == 0) {
            // producer hasn’t published this slot yet -> silence remainder
            write_silence(l + copied, remaining);
            if (ch == 2) write_silence(r + copied, remaining);
            return req;
        }

        size_t valid = audio_ring[read_idx].valid_bytes; // per-channel, published-before valid=1

        if (ring_read_pos >= valid) {
            // consume slot
            atomic_store_explicit(&audio_ring[read_idx].valid, 0, memory_order_release);
            ring_read_pos = 0;
            atomic_store_explicit(&audio_read_idx, (read_idx + 1) % AUDIO_RING_SIZE, memory_order_release);
            continue;
        }

        size_t avail   = valid - ring_read_pos;
        size_t to_copy = (avail < remaining) ? avail : remaining;

        // force spu_memload size multiple-of-32
        to_copy &= ~31u;

        if (!to_copy) {
            // can't copy aligned bytes -> drop this slot
            atomic_store_explicit(&audio_ring[read_idx].valid, 0, memory_order_release);
            ring_read_pos = 0;
            atomic_store_explicit(&audio_read_idx, (read_idx + 1) % AUDIO_RING_SIZE, memory_order_release);
            continue;
        }

        spu_memload(l + copied, audio_ring[read_idx].left  + ring_read_pos, to_copy);
        if (ch == 2)
            spu_memload(r + copied, audio_ring[read_idx].right + ring_read_pos, to_copy);

        ring_read_pos += to_copy;
        copied        += to_copy;
        remaining     -= to_copy;

        if (ring_read_pos >= valid) {
            atomic_store_explicit(&audio_ring[read_idx].valid, 0, memory_order_release);
            ring_read_pos = 0;
            atomic_store_explicit(&audio_read_idx, (read_idx + 1) % AUDIO_RING_SIZE, memory_order_release);
        }
    }

    return req;
}



// ============================================================================
// Worker Thread (similar to v6, but refills audio ring)
// ============================================================================

static inline int ring_inc(int x) { return (x + 1) % RING_CAPACITY; }

static int schedule_frame_preload(int total_frame) {
    int h = atomic_load(&preload_ring_head);
    int t = atomic_load(&preload_ring_tail);
    int next_h = ring_inc(h);
    
    if (next_h == t) return 0;  // Ring full
    
    preload_ring[h].frame = total_frame;
    preload_ring[h].generation = GSeekGeneration;
    atomic_store(&preload_ring_head, next_h);
    return 1;
}

static void* worker_thread(void *arg) {
    (void)arg;
    int last_prefetch_chunk = -1;

    while (1) {
        // Cheap: only memcpy from cached chunk -> audio ring
        refill_audio_ring();

        // Decode budget (tune 2..8)
        for (int n = 0; n < 4; n++) {
            int tail = atomic_load(&preload_ring_tail);
            int head = atomic_load(&preload_ring_head);
            if (tail == head) break;

            PreloadJob job = preload_ring[tail];
            atomic_store(&preload_ring_tail, ring_inc(tail));
            if (job.generation != GSeekGeneration) continue;

            int total_frame  = job.frame;
            int unique_frame = total_to_unique_frame(total_frame);
            int buf          = unique_frame % NUM_BUFFERS;

            int expected = BUF_EMPTY;
            if (atomic_compare_exchange_strong(&buf_state[buf], &expected, BUF_LOADING)) {
                if (load_frame(total_frame, buf) == 0)
                    atomic_store(&buf_state[buf], BUF_READY);
                else
                    atomic_store(&buf_state[buf], BUF_EMPTY);
            }
        }

        // Chunk request window (NO blocking IO here)
        int tf_now = atomic_load(&frame_index);
        int play_chunk = find_chunk_for_frame(tf_now);

        if (play_chunk != last_prefetch_chunk) {
            prefetch_chunks_around_play(play_chunk);
            last_prefetch_chunk = play_chunk;
        }

        // If frame decode asked for a chunk, request it (still async)
        int want = atomic_exchange(&chunk_prefetch_request, -1);
        if (want >= 0) {
            request_chunk_async(want, play_chunk, play_chunk+1, play_chunk+2);
        }

        // yield often; don't hog
        thd_pass();
        thd_sleep(2);
    }

    return NULL;
}



// ============================================================================
// Load Header
// ============================================================================
static int load_header() {
    fs_seek(video_fd, 0, SEEK_SET);
    
    if (fs_read(video_fd, &header, sizeof(DCMVHeader)) != sizeof(DCMVHeader)) {
        printf("❌ Failed to read header\n");
        return -1;
    }

    if (memcmp(header.magic, DCMV_MAGIC, 4) != 0) {
        printf("❌ Invalid magic: %.4s\n", header.magic);
        return -1;
    }

    if (header.version != 1) {
        printf("❌ Unsupported version: %lu (expected 1)\n", header.version);
        return -1;
    }

    printf("📦 DCMV v1.0 Chunked Format\n");
    printf("   %dx%d @ %.2f fps\n", header.tex_width, header.tex_height, header.fps);
    printf("   Audio: %dHz, %d channel(s)\n", header.sample_rate, header.channels);
    printf("   Frames: %lu unique, %lu total\n", 
           header.num_unique_frames, header.num_total_frames);
    printf("   Chunks: %lu (%.2fs each)\n", header.num_chunks, header.chunk_duration);
    printf("   Compression: %s\n", header.compression_type ? "Zstd" : "LZ4");

    return 0;
}

// ============================================================================
// PVR Initialization (from v6, handles strided textures)
// ============================================================================

static inline int is_power_of_2(int x) {
    return (x > 0) && ((x & (x - 1)) == 0);
}

static int init_pvr(int frame_type) {
    pvr_init_defaults();
    int use_strided = !is_power_of_2(header.tex_width) || !is_power_of_2(header.tex_height);
    if (frame_type == 1)
        pvr_txr = pvr_mem_malloc(header.tex_width * header.tex_height * 2);
    else
        pvr_txr = pvr_mem_malloc(header.uncompressed_frame_size);
    if (!pvr_txr) return -1;

    pvr_poly_cxt_t cxt;
    if (use_strided) {
        int txr_format = (frame_type == 1)
            ? PVR_TXRFMT_YUV422 | PVR_TXRFMT_VQ_ENABLE | (1 << 25) | PVR_TXRFMT_NONTWIDDLED
            : PVR_TXRFMT_RGB565 | PVR_TXRFMT_VQ_ENABLE | (1 << 25) | PVR_TXRFMT_NONTWIDDLED;

        int pot_width = 1, pot_height = 1;
        while (pot_width < (int)header.tex_width) pot_width <<= 1;
        while (pot_height < (int)header.tex_height) pot_height <<= 1;

        pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, txr_format,
                         pot_width, pot_height, pvr_txr, PVR_FILTER_NEAREST);
        pvr_poly_compile(&hdr, &cxt);
        PVR_SET(PVR_TEXTURE_MODULO, (header.tex_width / 32));

        int display_width = (header.tex_width == 320) ? 320 : 640;
        int display_height = (header.tex_width == 320) ? 240 : 480;

        vert[0] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX,.x=0,.y=0,.z=1,.u=0,.v=0,.argb=0xffffffff};
        vert[1] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX,.x=display_width,.y=0,.z=1,.u=(float)header.content_width/pot_width,.v=0,.argb=0xffffffff};
        vert[2] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX,.x=0,.y=display_height,.z=1,.u=0,.v=(float)header.content_height/pot_height,.argb=0xffffffff};
        vert[3] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX_EOL,.x=display_width,.y=display_height,.z=1,.u=(float)header.content_width/pot_width,.v=(float)header.content_height/pot_height,.argb=0xffffffff};        
     
    } else {
        pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY,
                         (frame_type == 1 ? PVR_TXRFMT_YUV422 : PVR_TXRFMT_RGB565) |
                         PVR_TXRFMT_TWIDDLED | PVR_TXRFMT_VQ_ENABLE,
                         header.tex_width, header.tex_height, pvr_txr, PVR_FILTER_NEAREST);
        pvr_poly_compile(&hdr, &cxt);

        // Twiddled + center-padded texture: crop out the black pad
        float umin = (float)(header.tex_width  - header.content_width)  / (2.0f * header.tex_width);
        float vmin = (float)(header.tex_height - header.content_height) / (2.0f * header.tex_height);
        float umax = 1.0f - umin;
        float vmax = 1.0f - vmin;

        vert[0] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX,    .x=0,  .y=0,   .z=1, .u=umin, .v=vmin, .argb=0xffffffff};
        vert[1] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX,    .x=640,.y=0,   .z=1, .u=umax, .v=vmin, .argb=0xffffffff};
        vert[2] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX,    .x=0,  .y=480, .z=1, .u=umin, .v=vmax, .argb=0xffffffff};
        vert[3] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX_EOL,.x=640,.y=480, .z=1, .u=umax, .v=vmax, .argb=0xffffffff};
    }

    return 0;
}

// ============================================================================
// Input Handling (same as v6)
// ============================================================================

static void wait_exit() {
    maple_device_t *cont = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    if (!cont) return;

    cont_state_t *state = (cont_state_t *)maple_dev_status(cont);
    if (!state) return;

    static int start_held = 0;
    if (state->buttons & CONT_START) {
        if (!start_held) {
            start_held = 1;
        }
    } else {
        if (start_held) {
            printf("🛑 START released, exiting...\n");
            arch_exit();
        }
        start_held = 0;
    }

    if (state->buttons & CONT_Y) {
        static int screenshot_num = 0;
        snprintf(screenshotfilename, sizeof(screenshotfilename), 
                 "/pc/screenshot%d.ppm", screenshot_num++);
        vid_screen_shot(screenshotfilename);
        printf("📸 Screenshot: %s\n", screenshotfilename);
        thd_sleep(200);
    }
}

// ---------- FMV debug logging ----------
#define FMV_LOG_EVERY_N_FRAMES 30   // 0=disable periodic logs
#define FMV_LOG_STALL_EVENT    1
#define FMV_LOG_DMA_EVENT      1

static inline const char *buf_state_name(int s) {
    switch (s) {
        case BUF_EMPTY:   return "EMPTY";
        case BUF_LOADING: return "LOAD";
        case BUF_READY:   return "READY";
        default:          return "?";
    }
}

static void fmv_log_tick(int total_frame,
                         int unique_id,
                         int buf,
                         double audio_ms,
                         double target_ms,
                         double drift_ms,
                         double render_ms,
                         double wait_ms,
                         int st,
                         int did_dma,
                         int stalled,
                         int udisp,
                         int uexp) {
#if FMV_LOG_EVERY_N_FRAMES
    int do_periodic = (FMV_LOG_EVERY_N_FRAMES > 0) && ((total_frame % FMV_LOG_EVERY_N_FRAMES) == 0);
#else
    int do_periodic = 0;
#endif

    int do_event = 0;
#if FMV_LOG_STALL_EVENT
    if (stalled) do_event = 1;
#endif
#if FMV_LOG_DMA_EVENT
    if (did_dma) do_event = 1;
#endif

    // Also log if drift is big (audio/video diverging)
    if (drift_ms > 50.0 || drift_ms < -50.0) do_event = 1;

    if (!do_periodic && !do_event) return;

    printf("[tick] tf=%d uf=%d buf=%d st=%s udisp=%d/%d "
           "aud=%.2f tgt=%.2f drift=%.2f render=%.2f wait=%.2f %s%s\n",
           total_frame, unique_id, buf, buf_state_name(st),
           udisp, uexp,
           audio_ms, target_ms, drift_ms,
           render_ms, wait_ms,
           did_dma ? "DMA " : "",
           stalled ? "STALL" : "");
}

// ============================================================================
// Main Tick Loop (similar to v6)
// ============================================================================
static double g_play_start_ms = 0.0;

static inline double now_play_ms(void) {
    return psTimer() - g_play_start_ms;
}

static void fmv_tick(void) { 
    double t0 = psTimer();

    // release any buffer we held for a prior frame
    if (pending_free_buf >= 0) {
        atomic_store(&buf_state[pending_free_buf], BUF_EMPTY);
        pending_free_buf = -1;
    }

    int cur_total = atomic_load(&frame_index);
    if (cur_total >= (int)header.num_total_frames) return;

    // --- Master clock = audio timeline (includes queued audio offset) ---
    double master_ms = (psTimer() - frame_timer_anchor) + atomic_load(&audio_start_time_ms);
    if (master_ms < 0.0) master_ms = 0.0;

    int desired_total = (int)(master_ms / frame_duration);

    if (desired_total < 0) desired_total = 0;
    if (desired_total >= (int)header.num_total_frames)
        desired_total = (int)header.num_total_frames - 1;

    // Only skip if we're REALLY behind (avoid constant 0->3->7 style jumps)
    const int SKIP_THRESHOLD = 2;
    if (desired_total > cur_total + SKIP_THRESHOLD) {
        cur_total = desired_total;
        atomic_store(&frame_index, cur_total);
        printf("⏩ Skipping ahead to frame %d (master_ms=%.2f)\n", cur_total, master_ms);
    }

    int unique_id = total_to_unique_frame(cur_total);
    int buf = unique_id % NUM_BUFFERS;

    if (unique_id != last_unique_frame_drawn) {
        unique_display_count = 0;
        expected_display_count = frame_durations[unique_id];
        if (expected_display_count < 1) expected_display_count = 1;
    }

    int st = atomic_load(&buf_state[buf]);

    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_dr_state_t dr;
    pvr_dr_init(&dr);

    int did_dma = 0;
    int stalled = 0;
    int displayed = 0;

    if (st == BUF_READY) {
        if (unique_id != last_unique_frame_drawn) {
            // Upload the exact frame we're about to draw (single VRAM texture model)
            dcache_flush_range((uint32)frame_buffer[buf], (uint32)header.uncompressed_frame_size);
            pvr_txr_load_dma(frame_buffer[buf], pvr_txr,
                             header.uncompressed_frame_size,
                             1,   // sync
                             NULL, 0);
            did_dma = 1;
            last_unique_frame_drawn = unique_id;
        }

        sq_fast_cpy((void *)SQ_MASK_DEST(PVR_TA_INPUT), &hdr,  sizeof(hdr) / 32);
        sq_fast_cpy((void *)SQ_MASK_DEST(PVR_TA_INPUT), vert,  sizeof(vert) / 32);

        unique_display_count++;
        if (unique_display_count >= expected_display_count)
            pending_free_buf = buf;

        // advance one frame; master clock decides if we need to skip later
        atomic_store(&frame_index, cur_total + 1);
        atomic_fetch_add(&displayed_total_frame, 1);

        displayed = 1;

        // prefetch ahead
        for (int i = 1; i <= PREFETCH_AHEAD; i++) {
            int next_total = (cur_total + i);
            if (next_total >= (int)header.num_total_frames) break;

            int next_unique = total_to_unique_frame(next_total);
            int nb = next_unique % NUM_BUFFERS;

            if (atomic_load(&buf_state[nb]) == BUF_EMPTY)
                schedule_frame_preload(next_total);
        }
    } else {
        // if empty, request the frame
        if (st == BUF_EMPTY)
            schedule_frame_preload(cur_total);

        stalled = 1;
    }

    pvr_dr_finish();
    pvr_list_finish();
    pvr_scene_finish();

    double render_ms = psTimer() - t0;

    // ----------------------------
    // Fix #2: sleep target selection
    // ----------------------------
    // If we displayed a frame, sleep toward the NEXT frame boundary.
    // If we stalled, do NOT sleep one frame ahead (that amplifies misses).
    int timing_frame = cur_total + (displayed ? 1 : 0);

    // sleep until next frame boundary based on master clock
    double next_target_ms = (timing_frame) * frame_duration;
    double wait_ms = next_target_ms - master_ms - render_ms;

    if (wait_ms > 0.0) {
        if (wait_ms > 8.0) thd_sleep((int)(wait_ms - 3.0));
        else thd_pass();
    }

    double tgt = cur_total * frame_duration;
    double drift = master_ms - tgt;

    // fmv_log_tick(cur_total, unique_id, buf,
    //              master_ms, tgt, drift,
    //              render_ms, wait_ms,
    //              st, did_dma, stalled,
    //              unique_display_count, expected_display_count);
}



// ============================================================================
// Main
// ============================================================================
static int open_video_file(void) {
    // Try CD first
    int fd = fs_open("/cd/movie.dcmv", O_RDONLY);
    if (fd >= 0) {
        VIDEO_FILE = "/cd/movie.dcmv";
        return fd;
    }
    
    // Fallback to PC
    fd = fs_open("/pc/movie.dcmv", O_RDONLY);
    if (fd >= 0) {
        VIDEO_FILE = "/pc/movie.dcmv";
    }
    
    return fd;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("🎬 DCMV v1.0 Chunked Player\n");

    // Open file and load header
    video_fd = open_video_file();
    if (video_fd < 0 || VIDEO_FILE == NULL) {
        printf("❌ Failed to open video file (tried /cd/movie.dcmv and /pc/movie.dcmv)\n");
        return -1;
    }
    
    printf("✅ Playing from: %s\n", VIDEO_FILE);

    if (load_header() < 0) {
        return -1;
    }

    // Set video mode
    vid_set_mode(header.tex_width == 320 ? DM_320x240 : DM_640x480, PM_RGB565);

    // Initialize Zstd if needed
    if (header.compression_type == 1) {
        dctx = ZSTD_createDCtx();
        ZSTD_DCtx_setParameter(dctx, ZSTD_d_format, ZSTD_f_zstd1_magicless);
        ZSTD_DCtx_setParameter(dctx, ZSTD_d_windowLogMax, 16);
        ZSTD_DCtx_setParameter(dctx, ZSTD_d_maxBlockSize, 65536);
        ZSTD_DCtx_setParameter(dctx, ZSTD_d_forceIgnoreChecksum, 1);
        ZSTD_DCtx_reset(dctx, ZSTD_reset_session_only);
    }

    // Load index tables
    fs_seek(video_fd, sizeof(DCMVHeader), SEEK_SET);
    
    frame_offsets = malloc((header.num_unique_frames + 1) * sizeof(uint32_t));
    frame_durations = malloc(header.num_unique_frames * sizeof(uint16_t));
    chunk_index = malloc(header.num_chunks * sizeof(ChunkIndexEntry));
    
    fs_read(video_fd, frame_offsets, (header.num_unique_frames + 1) * sizeof(uint32_t));
    fs_read(video_fd, frame_durations, header.num_unique_frames * sizeof(uint16_t));
    
    frame_prefix = malloc((header.num_unique_frames + 1) * sizeof(uint32_t));
    frame_prefix[0] = 0;
    for (uint32_t i = 0; i < header.num_unique_frames; i++)
        frame_prefix[i+1] = frame_prefix[i] + frame_offsets[i];    

    fs_seek(video_fd, header.chunk_index_offset, SEEK_SET);
    fs_read(video_fd, chunk_index, header.num_chunks * sizeof(ChunkIndexEntry));

    for (int i = 0; i < MIN(3, (int)header.num_chunks); i++) {
        ChunkIndexEntry *e = &chunk_index[i];
        uint32_t pad = pad32_after(e->chunk_offset + e->video_section_size);
        printf("[pad] chunk %d off=%lu vid=%lu pad=%lu aud=%lu\n",
            i, e->chunk_offset, e->video_section_size, pad, e->audio_size);
    }

    // Initialize chunk cache structs (DO NOT allocate here)
    for (int i = 0; i < CHUNK_CACHE_SIZE; i++) {
        chunk_cache[i].data = NULL;
        chunk_cache[i].valid = 0;
        chunk_cache[i].chunk_id = -1;
        chunk_cache[i].size = 0;
        chunk_cache[i].last_used = 0;
        chunk_cache[i].video_section = NULL;
        chunk_cache[i].audio_L = NULL;
        chunk_cache[i].audio_R = NULL;
    }

    if (init_chunk_cache_buffers() != 0) {
        printf("❌ failed to init chunk cache buffers\n");
        return -1;
    }
    // Build total->unique lookup table
    t2u_lut = malloc(header.num_total_frames * sizeof(uint16_t));
    int t = 0;
    for (uint32_t u = 0; u < header.num_unique_frames; u++) {
        for (uint16_t r = 0; r < frame_durations[u] && t < (int)header.num_total_frames; r++) {
            t2u_lut[t++] = (uint16_t)u;
        }
    }

    // Allocate buffers
    compressed_buffer = memalign(32, header.max_compressed_frame_size);
    for (int i = 0; i < NUM_BUFFERS; i++) {
        frame_buffer[i] = memalign(32, header.uncompressed_frame_size);
        if (!frame_buffer[i]) {
            printf("❌ frame_buffer[%d] alloc failed\n", i);
            return -1;
        }        
        atomic_store(&buf_state[i], BUF_EMPTY);
    }

    // Initialize audio ring
    for (int i = 0; i < AUDIO_RING_SIZE; i++) {
        atomic_store_explicit(&audio_ring[i].valid, 0, memory_order_relaxed);
    }
    atomic_store(&audio_write_idx, 0);
    atomic_store(&audio_read_idx, 0);

    // Initialize PVR
    if (init_pvr(header.frame_type) < 0) {
        return -1;
    }

    // Start audio stream
    // ✅ FIX: AUDIO_BUFFER_SIZE is per-channel, don't divide by channels!
    // For stereo, each channel gets AUDIO_BUFFER_SIZE bytes
    snd_stream_init_ex(header.channels, AUDIO_BUFFER_SIZE);
    stream = snd_stream_alloc(NULL, AUDIO_BUFFER_SIZE);
    snd_stream_set_callback_direct(stream, audio_cb);
    atomic_store(&audio_muted, 1);
    // Start worker thread

    kthread_t *wthread = thd_create(1, worker_thread, NULL);
    kthread_t *iothread = thd_create(2, io_thread, NULL);
    printf("[sanity] chunk0 vid=%lu aud=%lu start=%u n=%u\n",
        chunk_index[0].video_section_size,
        chunk_index[0].audio_size,
        chunk_index[0].start_frame,
        chunk_index[0].num_frames);    

    printf("🔄 Preloading initial chunks...\n");
    // Request initial chunks (0..2) and WAIT until they are in cache
    int want_initial = MIN(3, (int)header.num_chunks);
    for (int i = 0; i < want_initial; i++) {
        atomic_store(&chunk_prefetch_request, i);

        // wait until worker loads it (cache->valid == 1)
        while (1) {
            ChunkCache *c = get_cached_chunk(i);
            if (c) {
                int v = atomic_load_explicit(&c->valid, memory_order_acquire);
                if (v == 1) break;
            }
            thd_sleep(1);   // yield (don’t busy-spin)
        }
    }

    // Preload initial frames
    printf("🔄 Loading initial frames...\n");
    for (int tf = 0; tf < MIN(INITIAL_PRELOAD, (int)header.num_total_frames); tf++) {
        int uf  = total_to_unique_frame(tf);
        int buf = uf % NUM_BUFFERS;
        atomic_store(&buf_state[buf], BUF_LOADING);
        if (load_frame(tf, buf) == 0) {
            atomic_store(&buf_state[buf], BUF_READY);
        } else {
            atomic_store(&buf_state[buf], BUF_EMPTY);
        }
    }



    // Start playback
    frame_duration = 1000.0 / header.fps;
    frame_timer_anchor = psTimer();
    atomic_store(&audio_start_time_ms, 0.0);
    // ensure audio state starts at the beginning
    current_audio_chunk = 0;
    audio_chunk_read_pos = 0;
    atomic_store(&audio_write_idx, 0);
    atomic_store(&audio_read_idx, 0);
    for (int i = 0; i < AUDIO_RING_SIZE; i++) atomic_store_explicit(&audio_ring[i].valid, 0, memory_order_relaxed);

    // preload the first chunks already exists, so now prefill audio ring here:
    refill_audio_ring();

    // compute queued BEFORE starting stream
    double entry_ms = audio_entry_ms();
    int w  = atomic_load(&audio_write_idx);
    int rd = atomic_load(&audio_read_idx);
    int buffered = (w - rd + AUDIO_RING_SIZE) % AUDIO_RING_SIZE;
    double queued_ms = buffered * entry_ms;

    // anchor clock against queued audio
    atomic_store(&audio_start_time_ms, -queued_ms);
    frame_timer_anchor = psTimer();
    snd_stream_start_adpcm(stream, header.sample_rate, header.channels == 2 ? 1 : 0);    
    atomic_store(&audio_muted, 0);

    printf("✅ Starting playback\n");

    // Main loop
    while (atomic_load(&frame_index) < (int)header.num_total_frames) {
        fmv_tick();
        snd_stream_poll(stream);
        wait_exit();
    }

    printf("🏁 Playback finished\n");
    arch_exit();
    // Cleanup
    atomic_store(&audio_muted, 1);
    thd_join(wthread, NULL);
    snd_stream_stop(stream);
    snd_stream_destroy(stream);
    fs_close(video_fd);

    for (int i = 0; i < CHUNK_CACHE_SIZE; i++) {
        if (chunk_cache[i].data) free(chunk_cache[i].data);
    }

    free(compressed_buffer);
    free(frame_offsets);
    free(frame_durations);
    free(chunk_index);
    free(t2u_lut);

    for (int i = 0; i < NUM_BUFFERS; i++) {
        free(frame_buffer[i]);
    }

    if (dctx) ZSTD_freeDCtx(dctx);


    return 0;
}
