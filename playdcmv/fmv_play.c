/**
 * fmv_play_v6.c - Dreamcast FMV Player (DCMV v6)
 * -----------------------------------------------------
 * Supports:
 *  - DCMV v6 header (unique + total frames)
 *  - Frame duration table for deduplicated frames
 *  - Mapping total frame indices to unique frames
 *  - All original sync, audio, controller, and rendering logic
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
#include <fastmem/fastmem.h>
#define LZ4_memcpy(d,s,n) memcpy_fast((d),(s),(n))
#define LZ4_memmove(d,s,n) memmove_fast((d),(s),(n))
#define LZ4_memset(d,s,n) memset_fast((d),(s),(n))
#define LZ4_FREESTANDING 1
// #include <sh4zam/shz_sh4zam.h>
// #define LZ4_memcpy(d,s,n) shz_memcpy((d),(s),(n))
// #define LZ4_memmove(d,s,n) memmove((d),(s),(n))  // Still need standard memmove
// #define LZ4_memset(d,s,n) shz_memset8((d),(s),(n))  
#include <lz4/lz4.h>
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd/zstd.h>
static ZSTD_DCtx *dctx = NULL;
// ZSTD_DDict *ddict = NULL;
// void *dict_buf = NULL;
// size_t dict_size = 0;

#define DCMV_MAGIC "DCMV"
#define VIDEO_FILE "/pc/movie.dcmv"

static file_t video_fd = -1;
static file_t audio_fd_left = -1, audio_fd_right = -1;

static long left_channel_size = 0;
static uint8_t *compressed_buffer = NULL;
static uint32_t *frame_offsets = NULL;
static uint16_t *frame_durations = NULL;

static atomic_int frame_index = 0;    // total frame index
static float fps;
static int frame_type, video_width, video_height, content_width, content_height;
static int sample_rate, audio_channels;
static int num_unique_frames = 0, num_total_frames = 0;
static int video_frame_size, max_compressed_size, audio_offset;

// static atomic_int audio_bytes_fed = 0;
snd_stream_hnd_t stream;
// static kthread_t *wthread;
static atomic_int audio_muted = 0;
// static _Atomic double audio_start_time_ms = 0.0;
// static double frame_timer_anchor = 0.0;  
// static double frame_duration = 0.0;  
// static _Atomic float audio_start_time_ms = 0.0f;
static atomic_int seek_request = -1;  // This one's fine as-is
static float frame_start_time = 0.0f;
int use_zstd = 0;
pvr_ptr_t pvr_txr;
pvr_poly_hdr_t hdr;
pvr_vertex_t vert[4];
char screenshotfilename[256];
// --- timing/sync ---
static _Atomic double audio_start_time_ms = 0.0; // already present? keep as _Atomic double
static double frame_timer_anchor = 0.0;          // psTimer() anchor when (re)syncing audio clock
static double frame_duration = 0.0;              // **milliseconds per total frame (1000.0 / fps)**
static int last_unique_frame_drawn = -1;         // last unique frame we actually drew
static _Atomic int displayed_total_frame = 0;    // how many total frames presented (for stats/UI)

// --- tick loop scratch (all internal to the loop, but static to keep history) ---
// static double accumulated_frame_debt = 0.0;
// static int frames_dropped = 0;
// static int stall_count = 0;
// static double max_frame_time = 0.0;
// static double avg_frame_time = 0.0;
// static double frame_time_samples = 0.0;

// --- dedup display counters (unique vs duration repeats) ---
static int unique_display_count = 0;
static int expected_display_count = 0;

#define VIDEO_START_FRAME 0
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))

#define NUM_BUFFERS 4
static uint8_t *frame_buffer[NUM_BUFFERS];

enum BufState { BUF_EMPTY = 0, BUF_LOADING = 1, BUF_READY = 2 };
#define RING_CAPACITY (NUM_BUFFERS + 1)

static inline int ring_inc(int x) { return (x + 1) % RING_CAPACITY; }

typedef struct {
    int frame;       // total frame index
    int generation;
} PreloadJob;

static PreloadJob preload_ring[RING_CAPACITY];
static int GSeekGeneration = 0;
static atomic_int preload_ring_head = 0;
static atomic_int preload_ring_tail = 0;

#define PREFETCH_AHEAD (MIN(NUM_BUFFERS, (int)(fps * 2.5f)))
#define INITIAL_PRELOAD 4

_Atomic int buf_state[NUM_BUFFERS] = { BUF_EMPTY };
static atomic_int buf_ref_count[NUM_BUFFERS] = { 0 };  // Reference count per buffer
// static int last_unique_frame_drawn = -1;  // Track last unique frame we actually drew
static volatile int audio_started = 0;
int soundbufferalloc = 4096;
static volatile float current_audio_frame = 0;
// static _Atomic uint32_t video_seq_pos = 0;   // next expected byte offset in video_fd
// static _Atomic int      video_pos_valid = 0; // 0 = unknown/invalid → force seek

static inline float psTimer(void) {
    #define AICA_MEM_CLOCK 0x021000
    uint32_t jiffies = g2_read_32(SPU_RAM_UNCACHED_BASE + AICA_MEM_CLOCK);
    const float AICA_TICKS_PER_MS = 4.410f; 
    return jiffies / AICA_TICKS_PER_MS;
}

static uint16_t *t2u_lut = NULL;
// Map total frame index -> unique frame index
static inline int total_to_unique_frame(int total_frame) {
    return t2u_lut[total_frame];
}
static uint32_t vfd_last_end  = 0;
static int load_frame(int unique_frame, int buf_index) {
    uint32_t offset = frame_offsets[unique_frame];
    uint32_t next_offset = frame_offsets[unique_frame + 1];
    uint32_t compressed_size = next_offset - offset;
    // float t_seekread = psTimer();
    // long current_pos = fs_tell(video_fd);
    // uint32_t aligned_offset = offset & ~0x7FF; 
    if (vfd_last_end != (long)offset) {
        // fs_seek(video_fd, aligned_offset, SEEK_SET);
        // fs_read(video_fd, temp_buffer, offset - aligned_offset);
        fs_seek(video_fd, offset, SEEK_SET);

        // printf("load_frame: Seeking to frame %d at offset %u (current pos: %ld)\n", unique_frame, offset, vfd_last_end);
    }
    fs_read(video_fd, compressed_buffer, compressed_size);
    vfd_last_end = offset + compressed_size;
    // t_seekread = psTimer() - t_seekread;
    // // dbglog(DBG_INFO, "Frame %d compressed size: %u\n", unique_frame, compressed_size);
    // float t_decomp = psTimer();
    if (use_zstd == 1) {
        /* ✅ streaming decode stops at end-of-frame and ignores your 32/2048-byte padding */
        ZSTD_DCtx_reset(dctx, ZSTD_reset_session_only);

        ZSTD_inBuffer  in  = { compressed_buffer, compressed_size, 0 };
        ZSTD_outBuffer out = { frame_buffer[buf_index], (size_t)video_frame_size, 0 };

        size_t ret = 1;  /* >0 while frame not finished, 0 at frame end */
        while (ret != 0 && out.pos < out.size) {
            ret = ZSTD_decompressStream(dctx, &out, &in);
            if (ZSTD_isError(ret)) {
                printf("❌ ZSTD decompress error on frame %d: %s\n",
                       unique_frame, ZSTD_getErrorName(ret));
                return -1;
            }
        }

        if (out.pos != (size_t)video_frame_size) {
            /* If this ever trips, your compressed frame didn’t expand to the expected size */
            printf("❌ ZSTD frame %d decoded %zu/%d bytes\n",
                   unique_frame, out.pos, video_frame_size);
            return -1;
        }
    } else {
        int res = LZ4_decompress_fast(
            (const char *)compressed_buffer,
            (char *)frame_buffer[buf_index],
            video_frame_size);

        if (res < 0) {
            printf("❌ LZ4 decompression failed on unique frame %d\n", unique_frame);
            return -1;
        }
    }
    // t_decomp = psTimer() - t_decomp;
    // printf("Frame %d load: read=%.2fms, decompress=%.2fms\n", unique_frame, t_seekread, t_decomp);
    return 0;
}

static size_t audio_cb(snd_stream_hnd_t hnd, uintptr_t l, uintptr_t r, size_t req) {
    if (atomic_load(&audio_muted)) {
        memset((void *)l, 0, req);
        if (audio_channels == 2)
            memset((void *)r, 0, req);
        return req;
    }
    if (audio_channels == 2) {
        size_t lbytes = fs_read(audio_fd_left, (void *)l, req / 2);
        size_t rbytes = fs_read(audio_fd_right, (void *)r, req / 2);
        // atomic_fetch_add(&audio_bytes_fed, lbytes + rbytes);
        return lbytes + rbytes;
    } else {
        size_t bytes = fs_read(audio_fd_left, (void *)l, req);
        // atomic_fetch_add(&audio_bytes_fed, bytes);
        return bytes;
    }
}

static int load_header(void) {
    char magic[4];
    fs_read(video_fd, magic, 4);
    if (memcmp(magic, DCMV_MAGIC, 4)) return -1;

    uint32_t version;
    fs_read(video_fd, &version, 4);
    if (version != 6) {
        printf("❌ Unsupported DCMV version: %ld (expected 6)\n", version);
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

    uint8_t compression_type = 0;  // default to LZ4
    fs_read(video_fd, &compression_type, 1);

    // interpret compression_type
    const char *compression_str = "LZ4";
    if (compression_type == 1) {
        compression_str = "Zstandard";
    }
    frame_duration = 1000.0 / (double)fps;
    printf("📦 Header v%ld: %s %dx%d (content: %dx%d) @ %.2ffps, %dHz, %dch, unique=%d, total=%d\n",
        version,
        frame_type == 1 ? "YUV422" : "RGB565",
        video_width, video_height, content_width, content_height,
        fps, sample_rate, audio_channels,
        num_unique_frames, num_total_frames);

    printf("   Frame size: %d, Max compressed: %d, Audio offset: 0x%X, Compression: %s\n",
        video_frame_size, max_compressed_size, audio_offset, compression_str);

    // store this for later decompression choice
    use_zstd = (compression_type == 1);  // <-- declare this global or static as needed

    return 0;
}

static int is_power_of_2(int n) { return n > 0 && (n & (n - 1)) == 0; }

static int init_pvr(int frame_type) {
    pvr_init_defaults();
    int use_strided = !is_power_of_2(video_width) || !is_power_of_2(video_height);
    if (frame_type == 1)
        pvr_txr = pvr_mem_malloc(video_width * video_height * 2);
    else
        pvr_txr = pvr_mem_malloc(video_frame_size);
    if (!pvr_txr) return -1;

    pvr_poly_cxt_t cxt;
    if (use_strided) {
        int txr_format = (frame_type == 1)
            ? PVR_TXRFMT_YUV422 | PVR_TXRFMT_VQ_ENABLE | (1 << 25) | PVR_TXRFMT_NONTWIDDLED
            : PVR_TXRFMT_RGB565 | PVR_TXRFMT_VQ_ENABLE | (1 << 25) | PVR_TXRFMT_NONTWIDDLED;

        int pot_width = 1, pot_height = 1;
        while (pot_width < video_width) pot_width <<= 1;
        while (pot_height < video_height) pot_height <<= 1;

        pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, txr_format,
                         pot_width, pot_height, pvr_txr, PVR_FILTER_NEAREST);
        pvr_poly_compile(&hdr, &cxt);
        PVR_SET(PVR_TEXTURE_MODULO, (video_width / 32));
        // pvr_txr_set_stride(video_width);

        int display_width = (video_width == 320) ? 320 : 640;
        int display_height = (video_width == 320) ? 240 : 480;

        vert[0] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX,.x=0,.y=0,.z=1,.u=0,.v=0,.argb=0xffffffff};
        vert[1] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX,.x=display_width,.y=0,.z=1,.u=(float)content_width/pot_width,.v=0,.argb=0xffffffff};
        vert[2] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX,.x=0,.y=display_height,.z=1,.u=0,.v=(float)content_height/pot_height,.argb=0xffffffff};
        vert[3] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX_EOL,.x=display_width,.y=display_height,.z=1,.u=(float)content_width/pot_width,.v=(float)content_height/pot_height,.argb=0xffffffff};        
     
    } else {
        pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY,
                         (frame_type == 1 ? PVR_TXRFMT_YUV422 : PVR_TXRFMT_RGB565) |
                         PVR_TXRFMT_TWIDDLED | PVR_TXRFMT_VQ_ENABLE,
                         video_width, video_height, pvr_txr, PVR_FILTER_NEAREST);
        pvr_poly_compile(&hdr, &cxt);

        // Twiddled + center-padded texture: crop out the black pad
        float umin = (float)(video_width  - content_width)  / (2.0f * video_width);
        float vmin = (float)(video_height - content_height) / (2.0f * video_height);
        float umax = 1.0f - umin;
        float vmax = 1.0f - vmin;

        vert[0] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX,    .x=0,  .y=0,   .z=1, .u=umin, .v=vmin, .argb=0xffffffff};
        vert[1] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX,    .x=640,.y=0,   .z=1, .u=umax, .v=vmin, .argb=0xffffffff};
        vert[2] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX,    .x=0,  .y=480, .z=1, .u=umin, .v=vmax, .argb=0xffffffff};
        vert[3] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX_EOL,.x=640,.y=480, .z=1, .u=umax, .v=vmax, .argb=0xffffffff};

    }
    return 0;
}

void draw_frame(int buf_index) {
    // pvr_txr_load(frame_buffer[buf_index], pvr_txr, video_frame_size);
    pvr_txr_load_dma(frame_buffer[buf_index], pvr_txr, video_frame_size, -1, NULL, 0);

    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_dr_state_t dr;
    pvr_dr_init(&dr);
    uintptr_t sq_dest_addr = (uintptr_t)SQ_MASK_DEST(PVR_TA_INPUT);
    sq_fast_cpy((void *)sq_dest_addr, &hdr, 1);
    sq_fast_cpy((void *)sq_dest_addr, vert, 4);
    pvr_dr_finish();
    pvr_list_finish();
    pvr_scene_finish();
}

bool schedule_frame_preload(int frame) {
    if (frame >= num_total_frames) return false;
    int unique_frame = total_to_unique_frame(frame);
    int buf = unique_frame % NUM_BUFFERS;
    
    // Don't schedule if buffer is not empty
    if (atomic_load(&buf_state[buf]) != BUF_EMPTY) return false;

    int head = atomic_load(&preload_ring_head);
    int tail = atomic_load(&preload_ring_tail);
    int next_head = ring_inc(head);
    if (next_head == tail) return false;

    // Check if this UNIQUE frame is already queued (not just total frame)
    for (int i = tail; i != head; i = (i + 1) % RING_CAPACITY) {
        int queued_unique = total_to_unique_frame(preload_ring[i].frame);
        if (queued_unique == unique_frame) return false;  // Already queued
    }

    preload_ring[head].frame = frame;
    preload_ring[head].generation = GSeekGeneration;
    atomic_store(&preload_ring_head, next_head);
    return true;
}

// static void prefetch_frames(int current_frame) {
//     for (int i = 1; i <= PREFETCH_AHEAD; i++) {
//         int next = current_frame + i;
//         if (next >= num_total_frames) break;
//         schedule_frame_preload(next);
//     }
// }

void seek_to_frame(int new_frame) {
    if (new_frame < 0) new_frame = 0;
    if (new_frame >= num_total_frames) new_frame = num_total_frames - 1;

    atomic_store(&audio_muted, 1);
    GSeekGeneration++;
    last_unique_frame_drawn = -1;

    // clear decode buffers/refs
    for (int i = 0; i < NUM_BUFFERS; i++) {
        atomic_store(&buf_state[i], BUF_EMPTY);
        atomic_store(&buf_ref_count[i], 0);
    }

    // clear preload ring
    atomic_store(&preload_ring_head, 0);
    atomic_store(&preload_ring_tail, 0);

    // ---------- precise audio seek ----------
    // samples up to the start of this TOTAL frame
    double samples_exact = ((double)new_frame * (double)sample_rate) / (double)fps;
    uint32_t samples_i   = (uint32_t)(samples_exact + 0.5);  // round to nearest

    // AICA ADPCM: 2 samples/byte; align to 16-byte boundary
    uint32_t bytes_per_channel = (samples_i / 2);
    bytes_per_channel = (bytes_per_channel + 15) & ~0xF;

    // left channel
    long left_off = audio_offset + (long)bytes_per_channel;
    long left_end = audio_offset + left_channel_size;
    if (left_off > left_end) left_off = left_end;
    fs_seek(audio_fd_left, left_off, SEEK_SET);

    // right channel
    if (audio_channels == 2) {
        long right_off = audio_offset + left_channel_size + (long)bytes_per_channel;
        long right_end = audio_offset + (long)left_channel_size * 2;
        if (right_off > right_end) right_off = right_end;
        fs_seek(audio_fd_right, right_off, SEEK_SET);
    }

    // ---------- prime VIDEO stream to the unique frame ----------
    int uf = total_to_unique_frame(new_frame);
    uint32_t off = frame_offsets[uf];

    fs_seek(video_fd, off, SEEK_SET);
    vfd_last_end = off; 

    // ---------- timing anchors ----------
    atomic_store(&frame_index, new_frame);                         // TOTAL frame
    frame_start_time = psTimer();                                // ms "now"
    atomic_store(&audio_start_time_ms, (double)new_frame * frame_duration);

    // ---------- initial preloads ----------
    int preloads_scheduled = 0;
    const int runway = MIN(NUM_BUFFERS/2, 8);
    for (int i = 0; i < num_total_frames && preloads_scheduled < runway; i++) {
        int t = new_frame + i;
        if (t >= num_total_frames) break;

        int u = total_to_unique_frame(t);
        int b = u % NUM_BUFFERS;

        if (atomic_load(&buf_state[b]) == BUF_EMPTY) {
            if (schedule_frame_preload(t)) {
                preloads_scheduled++;
            }
        }
    }

    atomic_store(&audio_muted, 0);
}


static void wait_exit(void) {
    static uint16_t prev_buttons = 0;
    maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    if (!dev) return;

    cont_state_t *state = (cont_state_t *)maple_dev_status(dev);
    if (!state ) return;

    if (state->buttons == prev_buttons) return;
    prev_buttons = state->buttons;

    int current_frame = atomic_load(&frame_index);
    if (state->buttons & CONT_DPAD_RIGHT)
        atomic_store(&seek_request, current_frame + 500);
    else if (state->buttons & CONT_DPAD_LEFT)
        atomic_store(&seek_request, current_frame - 500);
    else if (state->buttons & CONT_A) {
        sprintf(screenshotfilename, "/pc/screenshot%d.ppm", current_frame);
        vid_screen_shot(screenshotfilename);
    } else if (state->buttons)
        arch_exit();
}

void *worker_thread(void *p) {
    (void)p;
    for (;;) {
        int did_work = 0;

        // Drain the queue completely each pass
        for (;;) {
            int tail = atomic_load(&preload_ring_tail);
            int head = atomic_load(&preload_ring_head);
            if (tail == head) break;  // empty

            PreloadJob job = preload_ring[tail];

            // Skip stale jobs from a previous seek
            if (job.generation != GSeekGeneration) {
                atomic_store(&preload_ring_tail, ring_inc(tail));
                continue;
            }

            int total_frame  = job.frame;
            int unique_frame = total_to_unique_frame(total_frame);
            int buf          = unique_frame % NUM_BUFFERS;

            int expected = BUF_EMPTY;
            if (atomic_compare_exchange_strong(&buf_state[buf], &expected, BUF_LOADING)) {
                if (load_frame(unique_frame, buf) == 0) {
                    atomic_store(&buf_state[buf], BUF_READY);
                } else {
                    atomic_store(&buf_state[buf], BUF_EMPTY);
                }
            }

            atomic_store(&preload_ring_tail, ring_inc(tail));
            did_work = 1;
        }

        if (!did_work) {
            // Nothing to do right now—let the main thread run
            thd_pass();
        }
    }
    return NULL;
}


// void debug_buffer_state(int current_frame) {
//     printf("Frame %d status: ", current_frame);
//     for (int i = 0; i < 8; i++) {  // Show first 8 buffers
//         int total_f = current_frame + i;
//         if (total_f >= num_total_frames) break;
//         int unique_f = total_to_unique_frame(total_f);
//         int buf = unique_f % NUM_BUFFERS;
//         int state = atomic_load(&buf_state[buf]);
//         printf("T%d(U%d,B%d)=%s ", total_f, unique_f, buf, 
//                state == BUF_EMPTY ? "E" : state == BUF_LOADING ? "L" : "R");
//     }
//     printf("\n");
// }

// Drive one iteration of AV sync + rendering + prefetch
static void fmv_tick(void) {
    static double accumulated_frame_debt = 0.0;
    static int frames_dropped = 0;
    static int stall_count = 0;
    static double max_frame_time = 0.0;
    static double avg_frame_time = 0.0;
    static double frame_time_samples = 0.0;

    int current_total = atomic_load(&frame_index);

    // audio clock
    double now = psTimer();
    double elapsed_ms = (now - frame_timer_anchor);
    double current_audio_time_ms = atomic_load(&audio_start_time_ms) + elapsed_ms;

    // expected video time (ms)
    double expected_video_time = current_total * frame_duration;

    // mild debt shaping
    double target_time_ms = expected_video_time
                          + (accumulated_frame_debt > 0.0 ? MIN(accumulated_frame_debt, frame_duration * 0.5)
                                                          : MAX(accumulated_frame_debt, -frame_duration * 0.5));

    // skip if audio is ~5 frames ahead
    int frames_to_skip = 0;
    int tmp = current_total;
    const double lead_allow_ms = frame_duration * 5.0;
    while ((tmp < num_total_frames) &&
           ((tmp * frame_duration) < (current_audio_time_ms - lead_allow_ms))) {
        tmp++;
        frames_to_skip++;
        accumulated_frame_debt *= 0.5;
    }

    if (frames_to_skip > 0) {
        printf("⚠️ Skipping %d frame(s): %d → %d (audio ahead by %.1fms)\n",
               frames_to_skip, current_total, tmp,
               current_audio_time_ms - expected_video_time);
        atomic_fetch_add(&frame_index, frames_to_skip);
        frames_dropped += frames_to_skip;
        current_total = tmp;
    }

    double t0 = psTimer();

    if (current_audio_time_ms >= target_time_ms) {
        // map total → unique
        int draw_total = current_total;
        int unique_id  = total_to_unique_frame(draw_total);
        int buf        = unique_id % NUM_BUFFERS;

        if (atomic_load(&buf_state[buf]) == BUF_READY) {
            if (unique_id != last_unique_frame_drawn) {
                // first time this unique frame is drawn
                draw_frame(buf);
                last_unique_frame_drawn = unique_id;
                unique_display_count    = 1;
                expected_display_count  = frame_durations[unique_id];
            } else {
                // repeat draw
                unique_display_count++;
            }

            // free buffer after last repeat
            if (unique_display_count >= expected_display_count) {
                atomic_store(&buf_state[buf], BUF_EMPTY);
            }

            stall_count = 0;
            atomic_fetch_add(&frame_index, 1);
            atomic_fetch_add(&displayed_total_frame, 1);

            // prefetch a few ahead
            for (int i = 1; i <= PREFETCH_AHEAD; i++) {
                int next_total = draw_total + i;
                if (next_total >= num_total_frames) break;
                int next_unique = total_to_unique_frame(next_total);
                int nb = next_unique % NUM_BUFFERS;
                if (atomic_load(&buf_state[nb]) == BUF_EMPTY)
                    schedule_frame_preload(next_total);
            }
        } else {
            // not ready yet — requeue
            if (atomic_load(&buf_state[buf]) == BUF_EMPTY)
                schedule_frame_preload(draw_total);

            if (++stall_count > 10) {
                printf("⚠️ Emergency advance past stalled total %d (unique %d)\n",
                       draw_total, unique_id);
                atomic_store(&buf_state[buf], BUF_EMPTY);
                atomic_fetch_add(&frame_index, 1);
                stall_count = 0;
                accumulated_frame_debt = 0.0;
            }
        }
    }

    // timing stats
    double t1 = psTimer();
    double render_ms = t1 - t0;

    if (render_ms > max_frame_time) max_frame_time = render_ms;
    avg_frame_time = (avg_frame_time * frame_time_samples + render_ms) / (frame_time_samples + 1.0);
    frame_time_samples += 1.0;

    double overrun = render_ms - frame_duration;
    if (overrun > 0.0) accumulated_frame_debt -= overrun;
    else               accumulated_frame_debt += (-overrun * 0.1);
    accumulated_frame_debt *= 0.95;

    // gentle waiting
    double wait_ms = target_time_ms - current_audio_time_ms;
    if (accumulated_frame_debt < -10.0) wait_ms = MAX(0.0, wait_ms + accumulated_frame_debt * 0.1);

    if (wait_ms > 8.0) {
        int sleep_ms = (int)(wait_ms - 3.0);
        if (sleep_ms > 0) thd_sleep(sleep_ms);
    } else if (wait_ms > 1.0) {
        thd_pass();
    }
}


int main(int argc, char **argv) {
    (void)argc; (void)argv;

    atomic_store(&frame_index, 0);

    // --- open + header ---
    video_fd = fs_open(VIDEO_FILE, O_RDONLY);
    if (video_fd < 0 || load_header() < 0) return -1;

    vid_set_mode(video_width == 320 ? DM_320x240 : DM_640x480, PM_RGB565);

    // --- (optional) Zstd dctx ---
    if (use_zstd == 1) {
        dctx = ZSTD_createDCtx();
        ZSTD_DCtx_setParameter(dctx, ZSTD_d_format, ZSTD_f_zstd1_magicless);
        ZSTD_DCtx_setParameter(dctx, ZSTD_d_windowLogMax, 16);
        ZSTD_DCtx_setParameter(dctx, ZSTD_d_maxBlockSize, 65536);
        ZSTD_DCtx_setParameter(dctx, ZSTD_d_forceIgnoreChecksum, 1);
        ZSTD_DCtx_refDDict(dctx, NULL);
        ZSTD_DCtx_reset(dctx, ZSTD_reset_session_only);
    }

    // --- index tables (match writer layout: start @ 50) ---
    fs_seek(video_fd, 50, SEEK_SET);
    frame_offsets   = malloc((num_unique_frames + 1) * sizeof(uint32_t));
    frame_durations = malloc(num_unique_frames * sizeof(uint16_t));
    fs_read(video_fd, frame_offsets,   (num_unique_frames + 1) * sizeof(uint32_t));
    fs_read(video_fd, frame_durations,  num_unique_frames       * sizeof(uint16_t));

    // --- durations & timing (NOTE: milliseconds per frame) ---
    frame_duration = 1000.0 / (double)fps;

    // --- figure audio sizes ---
    off_t save_pos = fs_tell(video_fd);
    fs_seek(video_fd, 0, SEEK_END);
    off_t file_end = fs_tell(video_fd);
    off_t total_audio_size = file_end - audio_offset;
    left_channel_size = (audio_channels == 2) ? (total_audio_size / 2) : total_audio_size;
    fs_seek(video_fd, save_pos, SEEK_SET);

    // Open audio file descriptors
    audio_fd_left = fs_open(VIDEO_FILE, O_RDONLY);
    if (audio_fd_left < 0) {
        printf("❌ Failed to open audio left fd\n");
        return -1;
    }
    fs_seek(audio_fd_left, audio_offset, SEEK_SET);

    if (audio_channels == 2) {
        audio_fd_right = fs_open(VIDEO_FILE, O_RDONLY);
        if (audio_fd_right < 0) {
            printf("❌ Failed to open audio right fd\n");
            return -1;
        }
        fs_seek(audio_fd_right, audio_offset + left_channel_size, SEEK_SET);
    }

    // --- buffers ---
    compressed_buffer = memalign(32, max_compressed_size);
    if (!compressed_buffer) return -1;

    for (int i = 0; i < NUM_BUFFERS; i++) {
        frame_buffer[i] = memalign(32, video_frame_size);
        if (!frame_buffer[i]) return -1;
        atomic_store(&buf_state[i], BUF_EMPTY);
    }

    if (init_pvr(frame_type) < 0) return -1;

    // --- audio stream ---
    snd_stream_init_ex(audio_channels, soundbufferalloc);
    stream = snd_stream_alloc(NULL, soundbufferalloc);
    snd_stream_set_callback_direct(stream, audio_cb);
    atomic_store(&audio_muted, 1);
    snd_stream_start_adpcm(stream, sample_rate, audio_channels == 2 ? 1 : 0);
    // --- open audio FDs ---
    audio_fd_left = fs_open(VIDEO_FILE, O_RDONLY);
    if (audio_channels == 2)
        audio_fd_right = fs_open(VIDEO_FILE, O_RDONLY);

    // ✅ position both channels at the start of the ADPCM data
    fs_seek(audio_fd_left, audio_offset, SEEK_SET);
    if (audio_channels == 2)
        fs_seek(audio_fd_right, audio_offset + left_channel_size, SEEK_SET);

    // Build TOTAL→UNIQUE lookup table
    t2u_lut = malloc(num_total_frames * sizeof(uint16_t));
    int t = 0;
    for (int u = 0; u < num_unique_frames; ++u) {
        uint16_t reps = frame_durations[u];
        for (uint16_t r = 0; r < reps && t < num_total_frames; ++r)
            t2u_lut[t++] = (uint16_t)u;
    }
    for (; t < num_total_frames; ++t) t2u_lut[t] = (uint16_t)(num_unique_frames - 1);    

    // --- worker ---
    kthread_t *wthread = thd_create(0, worker_thread, NULL);

    // --- preload a few UNIQUE frames synchronously ---
    printf("🔄 Loading initial frames synchronously...\n");
    for (int uf = 0; uf < MIN(INITIAL_PRELOAD, num_unique_frames); ++uf) {
        int buf = uf % NUM_BUFFERS;
        atomic_store(&buf_state[buf], BUF_LOADING);
        if (load_frame(uf, buf) == 0) {
            atomic_store(&buf_state[buf], BUF_READY);
        } else {
            atomic_store(&buf_state[buf], BUF_EMPTY);
        }
    }

    // --- then queue a runway by TOTAL frames (worker maps TOTAL→UNIQUE) ---
    int start_total = atomic_load(&frame_index);  // usually 0 at startup
    int end_total   = MIN(start_total + PREFETCH_AHEAD, num_total_frames);
    for (int t = start_total; t < end_total; ++t) {
        schedule_frame_preload(t);
    
    }
    // --- anchor & go ---
    frame_timer_anchor = psTimer();
    atomic_store(&audio_start_time_ms, 0.0);
    atomic_store(&audio_muted, 0);

    printf("✅ Starting playback @ %.2fms/frame, total=%d, unique=%d\n",
           frame_duration, num_total_frames, num_unique_frames);

    while (atomic_load(&frame_index) < num_total_frames) {
        // keep audio DMA moving and handle input here
        fmv_tick();
        snd_stream_poll(stream);
        wait_exit();

        
        
    }
    printf("🏁 Playback finished\n");
        arch_exit();
    // --- shutdown ---
    atomic_store(&audio_muted, 1);
    thd_join(wthread, NULL);
    snd_stream_stop(stream);
    snd_stream_destroy(stream);
    fs_close(video_fd);
    fs_close(audio_fd_left);
    if (audio_channels == 2) fs_close(audio_fd_right);
    free(compressed_buffer);
    free(frame_offsets);
    free(frame_durations);
    free(t2u_lut);

    return 0;
}
