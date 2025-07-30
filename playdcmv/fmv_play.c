/**
 * fmv_play_v5.c - Dreamcast FMV Player (DCMV v5)
 * -----------------------------------------------------
 * Supports:
 *  - DCMV v5 header (unique + total frames)
 *  - Frame duration table for deduplicated frames
 *  - Mapping total frame indices to unique frames
 *  - All original sync, audio, controller, and rendering logic
 *
 * Author: Troy Davis (GPF) — https://github.com/GPF
 * License: Public Domain / MIT-style — use freely with attribution.
 */

#include <kos.h>
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
#include <lz4/lz4.h>

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

static atomic_int audio_bytes_fed = 0;
snd_stream_hnd_t stream;
static kthread_t *wthread;
static atomic_int audio_muted = 0;
static double frame_duration = 1.0f / 30.0f;
static _Atomic double audio_start_time_ms = 0.0;
static atomic_int seek_request = -1;
static double frame_start_time = 0.0;

pvr_ptr_t pvr_txr;
pvr_poly_hdr_t hdr;
pvr_vertex_t vert[4];
char screenshotfilename[256];

#define VIDEO_START_FRAME 0
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))

#define NUM_BUFFERS 24
static uint8_t *frame_buffer[NUM_BUFFERS];

enum BufState { BUF_EMPTY = 0, BUF_LOADING = 1, BUF_READY = 2 };
#define RING_CAPACITY NUM_BUFFERS + 1

typedef struct {
    int frame;       // total frame index
    int generation;
} PreloadJob;

static PreloadJob preload_ring[RING_CAPACITY];
static int GSeekGeneration = 0;
static atomic_int preload_ring_head = 0;
static atomic_int preload_ring_tail = 0;

#define PREFETCH_AHEAD 20
#define INITIAL_PRELOAD 3

_Atomic int buf_state[NUM_BUFFERS] = { BUF_EMPTY };

static volatile int audio_started = 0;
int soundbufferalloc = 4096;
static volatile float current_audio_frame = 0;

static inline double psTimer(void) {
    #define AICA_MEM_CLOCK 0x021000
    uint32_t jiffies = g2_read_32(SPU_RAM_UNCACHED_BASE + AICA_MEM_CLOCK);
    return jiffies / 4.410f;
}

// Map total frame index -> unique frame index
static int total_to_unique_frame(int total_frame) {
    int accum = 0;
    for (int i = 0; i < num_unique_frames; i++) {
        accum += frame_durations[i];
        if (total_frame < accum) return i;
    }
    return num_unique_frames - 1;
}

static int load_frame(int unique_frame, int buf_index) {
    uint32_t offset = frame_offsets[unique_frame];
    uint32_t next_offset = frame_offsets[unique_frame + 1];
    uint32_t compressed_size = next_offset - offset;

    fs_seek(video_fd, offset, SEEK_SET);
    fs_read(video_fd, compressed_buffer, compressed_size);

    int res = LZ4_decompress_fast(
        (const char *)compressed_buffer,
        (char *)frame_buffer[buf_index],
        video_frame_size);

    if (res < 0) {
        printf("❌ LZ4 decompression failed on unique frame %d\n", unique_frame);
        return -1;
    }
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
        atomic_fetch_add(&audio_bytes_fed, lbytes + rbytes);
        return lbytes + rbytes;
    } else {
        size_t bytes = fs_read(audio_fd_left, (void *)l, req);
        atomic_fetch_add(&audio_bytes_fed, bytes);
        return bytes;
    }
}

static int load_header(void) {
    char magic[4];
    fs_read(video_fd, magic, 4);
    if (memcmp(magic, DCMV_MAGIC, 4)) return -1;

    uint32_t version;
    fs_read(video_fd, &version, 4);
    if (version != 5) {
        printf("❌ Unsupported DCMV version: %d (expected 5)\n", version);
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

    printf("📦 Header v5: %s %dx%d (content: %dx%d) @ %.2ffps, %dHz, %dch, unique=%d, total=%d, frame_size=%d, max_compressed_size=%d, audio_offset=0x%X\n",
        frame_type == 1 ? "YUV422" : "RGB565",
        video_width, video_height, content_width, content_height,
        fps, sample_rate, audio_channels,
        num_unique_frames, num_total_frames,
        video_frame_size, max_compressed_size, audio_offset);

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
            ? PVR_TXRFMT_YUV422 | PVR_TXRFMT_VQ_ENABLE | PVR_TXRFMT_NONTWIDDLED | (1 << 25)
            : PVR_TXRFMT_RGB565 | PVR_TXRFMT_VQ_ENABLE | PVR_TXRFMT_NONTWIDDLED | (1 << 25);

        int pot_width = 1, pot_height = 1;
        while (pot_width < video_width) pot_width <<= 1;
        while (pot_height < video_height) pot_height <<= 1;

        pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, txr_format,
                         pot_width, pot_height, pvr_txr, PVR_FILTER_BILINEAR);
        pvr_poly_compile(&hdr, &cxt);
        PVR_SET(PVR_TEXTURE_MODULO, (video_width / 32));

        vert[0] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX,.x=0,.y=0,.z=1,.u=0,.v=0,.argb=0xffffffff};
        vert[1] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX,.x=640,.y=0,.z=1,.u=(float)content_width/pot_width,.v=0,.argb=0xffffffff};
        vert[2] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX,.x=0,.y=480,.z=1,.u=0,.v=(float)content_height/pot_height,.argb=0xffffffff};
        vert[3] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX_EOL,.x=640,.y=480,.z=1,.u=(float)content_width/pot_width,.v=(float)content_height/pot_height,.argb=0xffffffff};
    } else {
        pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY,
                         (frame_type == 1 ? PVR_TXRFMT_YUV422 : PVR_TXRFMT_RGB565) |
                         PVR_TXRFMT_TWIDDLED | PVR_TXRFMT_VQ_ENABLE,
                         video_width, video_height, pvr_txr, PVR_FILTER_BILINEAR);
        pvr_poly_compile(&hdr, &cxt);
        vert[0] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX,.x=0,.y=0,.z=1,.u=0,.v=0,.argb=0xffffffff};
        vert[1] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX,.x=640,.y=0,.z=1,.u=1,.v=0,.argb=0xffffffff};
        vert[2] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX,.x=0,.y=480,.z=1,.u=0,.v=1,.argb=0xffffffff};
        vert[3] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX_EOL,.x=640,.y=480,.z=1,.u=1,.v=1,.argb=0xffffffff};
    }
    return 0;
}

void draw_frame(int buf_index) {
    pvr_txr_load(frame_buffer[buf_index], pvr_txr, video_frame_size);
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
    if (atomic_load(&buf_state[buf]) != BUF_EMPTY) return false;

    int head = atomic_load(&preload_ring_head);
    int tail = atomic_load(&preload_ring_tail);
    int next_head = (head + 1) % RING_CAPACITY;
    if (next_head == tail) return false;

    for (int i = tail; i != head; i = (i + 1) % RING_CAPACITY) {
        if (preload_ring[i].frame == frame) return false;
    }

    preload_ring[head].frame = frame;
    preload_ring[head].generation = GSeekGeneration;
    atomic_store(&preload_ring_head, next_head);
    return true;
}

static void prefetch_frames(int current_frame) {
    for (int i = 1; i <= PREFETCH_AHEAD; i++) {
        int next = current_frame + i;
        if (next >= num_total_frames) break;
        schedule_frame_preload(next);
    }
}

void seek_to_frame(int new_frame) {
    if (new_frame < 0) new_frame = 0;
    if (new_frame >= num_total_frames) new_frame = num_total_frames - 1;

    atomic_store(&audio_muted, 1);
    GSeekGeneration++;

    for (int i = 0; i < NUM_BUFFERS; i++)
        atomic_store(&buf_state[i], BUF_EMPTY);

    atomic_store(&preload_ring_head, 0);
    atomic_store(&preload_ring_tail, 0);

    // Audio seek (same logic as original)
    int samples_per_frame = (int)(sample_rate / fps);
    int bytes_per_frame_per_channel = (samples_per_frame + 1) / 2;
    bytes_per_frame_per_channel = (bytes_per_frame_per_channel + 15) & ~0xF;

    long left_offset = audio_offset + ((long)new_frame * bytes_per_frame_per_channel);
    if (left_offset > (audio_offset + left_channel_size))
        left_offset = audio_offset + left_channel_size;
    fs_seek(audio_fd_left, left_offset, SEEK_SET);

    if (audio_channels == 2) {
        long right_offset = audio_offset + left_channel_size + ((long)new_frame * bytes_per_frame_per_channel);
        if (right_offset > (audio_offset + left_channel_size * 2))
            right_offset = audio_offset + left_channel_size * 2;
        fs_seek(audio_fd_right, right_offset, SEEK_SET);
    }

    atomic_store(&frame_index, new_frame);
    frame_start_time = psTimer();
    double new_audio_time = (double)(new_frame * samples_per_frame) * 1000.0 / sample_rate;
    atomic_store(&audio_start_time_ms, new_audio_time);

    for (int i = 0; i < MIN(NUM_BUFFERS, 4) && (new_frame + i) < num_total_frames; i++)
        schedule_frame_preload(new_frame + i);

    atomic_store(&audio_muted, 0);
}

static void wait_exit(void) {
    static uint16_t prev_buttons = 0;
    maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    if (!dev) return;

    cont_state_t *state = (cont_state_t *)maple_dev_status(dev);
    if (!state || !dev->status_valid) return;

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
    while (1) {
        snd_stream_poll(stream);
        int tail = atomic_load(&preload_ring_tail);
        int head = atomic_load(&preload_ring_head);

        if (tail != head) {
            PreloadJob job = preload_ring[tail];
            if (job.generation != GSeekGeneration) {
                atomic_store(&preload_ring_tail, (tail + 1) % RING_CAPACITY);
                continue;
            }
            int total_frame = job.frame;
            int unique_frame = total_to_unique_frame(total_frame);
            int buf = unique_frame % NUM_BUFFERS;

            int expected = BUF_EMPTY;
            if (atomic_compare_exchange_strong(&buf_state[buf], &expected, BUF_LOADING)) {
                if (load_frame(unique_frame, buf) == 0) {
                    atomic_store(&buf_state[buf], BUF_READY);
                } else {
                    atomic_store(&buf_state[buf], BUF_EMPTY);
                }
            }
            atomic_store(&preload_ring_tail, (tail + 1) % RING_CAPACITY);
        }
        wait_exit();
        thd_pass();
    }
    return NULL;
}

int main(int argc, char **argv) {
    atomic_store(&frame_index, 0);
    int current_frame = atomic_load(&frame_index);
    
    video_fd = fs_open(VIDEO_FILE, O_RDONLY);
    if (video_fd < 0 || load_header() < 0) return -1;

    // Read frame offsets + durations for deduplicated format
    fs_seek(video_fd, 49, SEEK_SET);
    frame_offsets = malloc((num_unique_frames + 1) * sizeof(uint32_t));
    fs_read(video_fd, frame_offsets, (num_unique_frames + 1) * sizeof(uint32_t));
    frame_durations = malloc(num_unique_frames * sizeof(uint16_t));
    fs_read(video_fd, frame_durations, num_unique_frames * sizeof(uint16_t));

    frame_duration = 1.0f / (float)fps;

    // Save current position and calculate audio size
    off_t current_pos = fs_tell(video_fd);
    fs_seek(video_fd, 0, SEEK_END);
    off_t file_end = fs_tell(video_fd);
    off_t total_audio_size = file_end - audio_offset;
    if (audio_channels == 2) {
        left_channel_size = total_audio_size / 2;
    } else {
        left_channel_size = total_audio_size;  // Mono: whole chunk
    }
    fs_seek(video_fd, current_pos, SEEK_SET);

    // Open audio file descriptors
    audio_fd_left = fs_open(VIDEO_FILE, O_RDONLY);
    if (audio_channels == 2)
        audio_fd_right = fs_open(VIDEO_FILE, O_RDONLY);

    // Allocate buffers
    compressed_buffer = memalign(32, max_compressed_size);
    if (!compressed_buffer) return -1;

    for (int i = 0; i < NUM_BUFFERS; i++) {
        frame_buffer[i] = memalign(32, video_frame_size);
        if (!frame_buffer[i]) return -1;
    }

    // Initialize the PVR for rendering
    if (init_pvr(frame_type) < 0) return -1;

    // Initialize audio stream
    snd_stream_init_ex(audio_channels, soundbufferalloc);
    stream = snd_stream_alloc(NULL, soundbufferalloc);
    snd_stream_set_callback_direct(stream, audio_cb);

    // Calculate timing in milliseconds
    double frame_time_ms = 1000.0 / (double)fps; // ~43.48ms for 23fps
    printf("Frame timing: %.2fms per frame\n", frame_time_ms);
    printf("✅ Starting playback with %d total frames, %d unique frames\n", num_total_frames, num_unique_frames);
    
    atomic_store(&audio_muted, 1);
    snd_stream_start_adpcm(stream, sample_rate, audio_channels == 2 ? 1 : 0);
    
    // ✅ Start worker before pushing preload jobs
    wthread = thd_create(0, worker_thread, NULL);

    // 🔁 Let worker run at least a frame
    thd_sleep(30);  // Allow ~30ms for worker to dequeue from ring
    atomic_store(&seek_request, current_frame);

    printf("✅ All initial frames ready. Starting at frame %d\n", current_frame);

    double accumulated_frame_debt = 0.0;
    int frames_dropped = 0;
    double max_frame_time = 0.0;
    double avg_frame_time = 0.0;
    double frame_time_samples = 0.0;
    int stall_count = 0;

    while (atomic_load(&frame_index) < num_total_frames) {
        int requested_seek = atomic_exchange(&seek_request, -1);
        current_frame = atomic_load(&frame_index);        
        double loop_timer_ms = psTimer();
        
        if (requested_seek != -1) {
            printf("Seeking to frame %d\n", requested_seek);
            seek_to_frame(requested_seek);
            
            // **CRITICAL FIX**: Reset state after seek
            current_frame = atomic_load(&frame_index);
            accumulated_frame_debt = 0.0;
            stall_count = 0;
            frame_start_time = psTimer();
            
            continue;
        }

        double current_audio_start_ms = atomic_load(&audio_start_time_ms);
        double current_audio_time_ms = current_audio_start_ms + (loop_timer_ms - frame_start_time);
        double expected_video_time = current_frame * frame_time_ms;

        // Calculate target time with debt compensation
        double target_time_ms = expected_video_time;    
        if (accumulated_frame_debt > 0.0) {
            target_time_ms += MIN(accumulated_frame_debt, frame_time_ms * 0.5);
        } else if (accumulated_frame_debt < 0.0) {
            target_time_ms += MAX(accumulated_frame_debt, -frame_time_ms * 0.5);
        }
        
        // Frame skipping logic adapted for deduplicated format
        int frames_to_skip = 0;
        int temp_frame = current_frame;

        while ((temp_frame < num_total_frames) &&
               ((temp_frame * frame_time_ms) < (current_audio_start_ms))) {    
            temp_frame++;
            frames_to_skip++;
            accumulated_frame_debt = 0.0;
        }
        
        if (frames_to_skip > 0) {
            printf("⚠️ Skipping %d frame(s): %d → %d (audio ahead by %.1fms)\n",
                frames_to_skip,
                current_frame,
                temp_frame,
                current_audio_time_ms - expected_video_time);

            for (int f = current_frame; f < temp_frame; f++) {
                int unique_f = total_to_unique_frame(f);
                printf("⏩ Would have drawn frame %d (unique %d, buf %d)\n", f, unique_f, unique_f % NUM_BUFFERS);
            }

            atomic_fetch_add(&frame_index, frames_to_skip);
            frames_dropped += frames_to_skip;
            current_frame = temp_frame;
        }
        
        double frame_render_start = psTimer();
        
        if (current_audio_time_ms >= target_time_ms) {
        if (current_audio_time_ms >= target_time_ms) {
            int draw_frame_id = atomic_load(&frame_index);
            
            // Convert total frame to unique frame for buffer indexing
            int unique_frame_id = total_to_unique_frame(draw_frame_id);
            int buf_index = unique_frame_id % NUM_BUFFERS;
            
            if (atomic_load_explicit(&buf_state[buf_index], memory_order_acquire) == BUF_READY) {
                draw_frame(buf_index);
                atomic_store_explicit(&buf_state[buf_index], BUF_EMPTY, memory_order_release);

                stall_count = 0;
                atomic_fetch_add(&frame_index, 1);

                // New: Always prefetch ahead after a successful draw
                // Schedule preloads for upcoming unique frames
                for (int i = 1; i < NUM_BUFFERS; i++) {
                    int next_total_frame = draw_frame_id + i;
                    if (next_total_frame >= num_total_frames) break;
                    schedule_frame_preload(next_total_frame);
                }
            } else {
                stall_count++;
                if (stall_count > 5) {
                    printf("⚠️ Emergency advancing past stalled frame %d (unique %d)\n", draw_frame_id, unique_frame_id);
                    atomic_store(&buf_state[buf_index], BUF_EMPTY);
                    atomic_fetch_add(&frame_index, 1);
                    prefetch_frames(draw_frame_id);
                    stall_count = 0;
                }
            }
        
                stall_count++;
                if (stall_count > 5) {
                    printf("⚠️ Emergency advancing past stalled frame %d (unique %d)\n", draw_frame_id, unique_frame_id);
                    atomic_store(&buf_state[buf_index], BUF_EMPTY);
                    atomic_fetch_add(&frame_index, 1);
                    prefetch_frames(draw_frame_id);
                    stall_count = 0;
                }
            }
        }

        // Timing tracking...
        double frame_render_end = psTimer();
        double this_frame_time = frame_render_end - frame_render_start;

        if (this_frame_time > max_frame_time)
            max_frame_time = this_frame_time;

        avg_frame_time = (avg_frame_time * frame_time_samples + this_frame_time) / (frame_time_samples + 1);
        frame_time_samples++;

        // Step 6: Adjust sync debt
        double frame_overrun = this_frame_time - frame_time_ms;
        if (frame_overrun > 0.0) {
            accumulated_frame_debt -= frame_overrun;
        } else {
            accumulated_frame_debt += (-frame_overrun * 0.1);
        }
        accumulated_frame_debt *= 0.95;

        // Optional: debug log
        if (this_frame_time > frame_time_ms * 0.8) {
            printf("⚠️ Frame %d took %.1fms (%.1f%%), debt: %.2fms\n",
                current_frame, this_frame_time,
                (this_frame_time / frame_time_ms) * 100.0,
                accumulated_frame_debt);
        } else {
            // Waiting logic
            double wait_ms = target_time_ms - current_audio_time_ms;
            
            if (accumulated_frame_debt < -10.0) {
                wait_ms = MAX(0.0, wait_ms + accumulated_frame_debt * 0.1);
            }
            
            if (wait_ms > 8.0) {
                int sleep_ms = (int)(wait_ms - 3.0);
                if (sleep_ms > 0) {
                    thd_sleep(sleep_ms);
                }
            } else if (wait_ms > 1.0) {
                thd_pass();
            }
        }
    }

    atomic_store(&audio_muted, 1);
    
    // Clean up
    thd_join(wthread, NULL);
    snd_stream_stop(stream);
    snd_stream_destroy(stream);
    fs_close(video_fd);
    fs_close(audio_fd_left);
    if (audio_channels == 2)
        fs_close(audio_fd_right);    
        
    free(compressed_buffer);
    free(frame_offsets);
    free(frame_durations);

    return 0;
}