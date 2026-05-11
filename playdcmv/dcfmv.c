#include "dcfmv.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <zstd/zstd.h>
#include <lz4/lz4.h>

enum {
    DCFMV_LOG_CHUNK  = 1 << 0,
    DCFMV_LOG_FMV    = 1 << 1,
    DCFMV_LOG_SEEK   = 1 << 2,
    DCFMV_LOG_AUDIO  = 1 << 3,
    DCFMV_LOG_CHUNK_AUDIO = 1 << 4,
    DCFMV_LOG_WORKER = 1 << 5,
    DCFMV_LOG_DECODE = 1 << 6,
    DCFMV_LOG_RENDER = 1 << 7,
    DCFMV_LOG_UPLOAD = 1 << 8,
    DCFMV_LOG_TIMING = 1 << 9
};

#define DCFMV_DEBUG_LOG_MASK_DEFAULT ( \
    ((DCFMV_DEBUG_LOG_CHUNK  ? DCFMV_LOG_CHUNK  : 0) | \
     (DCFMV_DEBUG_LOG_FMV    ? DCFMV_LOG_FMV    : 0) | \
     (DCFMV_DEBUG_LOG_SEEK   ? DCFMV_LOG_SEEK   : 0) | \
     (DCFMV_DEBUG_LOG_AUDIO  ? DCFMV_LOG_AUDIO  : 0) | \
     (DCFMV_DEBUG_LOG_CHUNK_AUDIO ? DCFMV_LOG_CHUNK_AUDIO : 0) | \
     (DCFMV_DEBUG_LOG_WORKER ? DCFMV_LOG_WORKER : 0) | \
     (DCFMV_DEBUG_LOG_DECODE ? DCFMV_LOG_DECODE : 0) | \
     (DCFMV_DEBUG_LOG_RENDER ? DCFMV_LOG_RENDER : 0) | \
     (DCFMV_DEBUG_LOG_UPLOAD ? DCFMV_LOG_UPLOAD : 0) | \
     (DCFMV_DEBUG_LOG_TIMING ? DCFMV_LOG_TIMING : 0)))

static void DCMV_LogMask(unsigned mask, const char *fmt, ...) {
#if !DCFMV_DEBUG_LOGS
    (void)mask;
    (void)fmt;
    return;
#else
    if (!(DCFMV_DEBUG_LOG_MASK_DEFAULT & mask))
        return;

    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
#endif
}

#define DCMV_LOG(mask, ...) DCMV_LogMask((mask), __VA_ARGS__)

static void DCMV_Error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

static ZSTD_DCtx *dcfmv_zstd_dctx = NULL;
static mutex_t dcfmv_state_lock = MUTEX_INITIALIZER;
static mutex_t dcfmv_io_lock = MUTEX_INITIALIZER;
static mutex_t dcfmv_audio_lock = MUTEX_INITIALIZER;

typedef struct dcfmv_backend_ops {
    int (*open)(dcfmv_t *fmv);
    void (*close)(dcfmv_t *fmv);
    int (*decode_frame)(dcfmv_t *fmv, int unique_frame, int buf_index);
    int (*seek_video)(dcfmv_t *fmv, int total_frame);
    int (*seek_audio)(dcfmv_t *fmv, int total_frame);
} dcfmv_backend_ops_t;

typedef struct __attribute__((packed)) {
    char     magic[4];
    uint32_t version;
    uint8_t  frame_type;
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
    uint8_t  compression_type;
    float    chunk_duration;
    uint32_t num_chunks;
    uint32_t chunk_index_offset;
    uint8_t  padding[10];
} dcfmv_chunk_header_t;

typedef struct __attribute__((packed)) {
    uint32_t chunk_offset;
    uint32_t video_section_size;
    uint32_t audio_size;
    uint32_t start_frame;
    uint32_t num_frames;
} dcfmv_chunk_entry_t;

typedef struct {
    int chunk_id;
    uint8_t *data;
    uint32_t size;
    uint8_t *video_section;
    uint8_t *audio_L;
    uint8_t *audio_R;
    uint32_t video_bytes_size;
    uint32_t map_cap;
    uint32_t *frame_off_local;
    uint32_t *frame_sz_local;
    uint32_t seen_cap;
    uint32_t *seen_u;
    uint32_t *seen_off;
    uint32_t last_used;
    int ready;
} dcfmv_chunk_cache_slot_t;

static inline double dcfmv_decode_timer_ms(void) {
    #define AICA_MEM_CLOCK 0x021000
    uint32_t jiffies = g2_read_32(SPU_RAM_UNCACHED_BASE + AICA_MEM_CLOCK);
    const double aica_ticks_per_ms = 4.410;
    return (double)jiffies / aica_ticks_per_ms;
}

dcfmv_t *dcfmv_current = NULL;

void dcfmv_audio_transfer_lock(void) {
    mutex_lock(&dcfmv_audio_lock);
}

void dcfmv_audio_transfer_unlock(void) {
    mutex_unlock(&dcfmv_audio_lock);
}

static int dcfmv_frames_open(dcfmv_t *fmv);
static void dcfmv_frames_close(dcfmv_t *fmv);
static int dcfmv_frames_decode_frame(dcfmv_t *fmv, int total_frame, int buf_index);
static int dcfmv_frames_seek_video(dcfmv_t *fmv, int total_frame);
static int dcfmv_frames_seek_audio(dcfmv_t *fmv, int total_frame);

static int dcfmv_chunks_open(dcfmv_t *fmv);
static void dcfmv_chunks_close(dcfmv_t *fmv);
static int dcfmv_chunks_decode_frame(dcfmv_t *fmv, int total_frame, int buf_index);
static int dcfmv_chunks_seek_video(dcfmv_t *fmv, int total_frame);
static int dcfmv_chunks_seek_audio(dcfmv_t *fmv, int total_frame);

static const dcfmv_backend_ops_t dcfmv_frames_ops = {
    .open = dcfmv_frames_open,
    .close = dcfmv_frames_close,
    .decode_frame = dcfmv_frames_decode_frame,
    .seek_video = dcfmv_frames_seek_video,
    .seek_audio = dcfmv_frames_seek_audio,
};

static const dcfmv_backend_ops_t dcfmv_chunks_ops = {
    .open = dcfmv_chunks_open,
    .close = dcfmv_chunks_close,
    .decode_frame = dcfmv_chunks_decode_frame,
    .seek_video = dcfmv_chunks_seek_video,
    .seek_audio = dcfmv_chunks_seek_audio,
};

static inline int dcfmv_total_to_unique_frame(dcfmv_t *fmv, int total_frame);

static const dcfmv_backend_ops_t *dcfmv_backend_ops(const dcfmv_t *fmv) {
    if (!fmv) return NULL;
    switch (fmv->backend_kind) {
        case DCFMV_BACKEND_CHUNKS:
            return &dcfmv_chunks_ops;
        case DCFMV_BACKEND_FRAMES:
        default:
            return &dcfmv_frames_ops;
    }
}

static int dcfmv_probe_backend(file_t fd, enum dcfmv_backend_kind *backend_kind, uint32_t *version_out) {
    char magic[4];
    uint32_t version = 0;

    if (fd < 0 || !backend_kind)
        return -1;

    fs_seek(fd, 0, SEEK_SET);
    if (fs_read(fd, magic, sizeof(magic)) != (ssize_t)sizeof(magic))
        return -1;
    if (memcmp(magic, DCFMV_MAGIC, sizeof(magic)) != 0)
        return -1;
    if (fs_read(fd, &version, sizeof(version)) != (ssize_t)sizeof(version))
        return -1;

    switch (version) {
        case 1:
            *backend_kind = DCFMV_BACKEND_CHUNKS;
            break;
        case 6:
            *backend_kind = DCFMV_BACKEND_FRAMES;
            break;
        default:
            return -1;
    }

    if (version_out)
        *version_out = version;
    fs_seek(fd, 0, SEEK_SET);
    return 0;
}

static void dcfmv_reset_media_info(dcfmv_t *fmv) {
    if (!fmv) return;
    memset(&fmv->media_info, 0, sizeof(fmv->media_info));
    fmv->backend_kind = DCFMV_BACKEND_FRAMES;
    fmv->chunk_count = 0;
    fmv->chunk_index_offset = 0;
    fmv->chunk_duration = 0.0f;
    fmv->chunk_cache_slots = 0;
    fmv->global_cache_tick = 0;
}

static void dcfmv_init_timebase_from_fps(dcfmv_t *fmv, float fpsf) {
    if (!fmv) return;

    if (fabsf(fpsf - (24000.0f / 1001.0f)) < 0.02f) {
        fmv->fps_num = 24000;
        fmv->fps_den = 1001;
    } else if (fabsf(fpsf - (30000.0f / 1001.0f)) < 0.02f) {
        fmv->fps_num = 30000;
        fmv->fps_den = 1001;
    } else if (fabsf(fpsf - (60000.0f / 1001.0f)) < 0.02f) {
        fmv->fps_num = 60000;
        fmv->fps_den = 1001;
    } else {
        fmv->fps_den = 1000;
        fmv->fps_num = (uint32_t)llroundf(fpsf * fmv->fps_den);
    }

    fmv->frame_duration_ms = (1000.0 * (double)fmv->fps_den) / (double)fmv->fps_num;
}

static inline uint32_t dcfmv_align32(uint32_t x) { return (x + 31u) & ~31u; }
static inline uint32_t dcfmv_pad32_after(uint32_t end) { return (32u - (end & 31u)) & 31u; }
static inline uint32_t dcfmv_frame_comp_size(const dcfmv_t *fmv, uint32_t unique_frame) {
    return fmv->frame_sizes[unique_frame] & 0x1FFFFFFFu;
}

static int dcfmv_read_header_v6(dcfmv_t *fmv) {
    char magic[4];
    uint32_t version;
    uint8_t compression_type = 0;

    if (!fmv || fmv->video_fd < 0) return -1;

    if (fs_read(fmv->video_fd, magic, sizeof(magic)) != (ssize_t)sizeof(magic))
        return -1;
    if (memcmp(magic, DCFMV_MAGIC, sizeof(magic)) != 0)
        return -1;

    if (fs_read(fmv->video_fd, &version, sizeof(version)) != (ssize_t)sizeof(version))
        return -1;
    if (version != 6)
        return -1;

    if (fs_read(fmv->video_fd, &fmv->frame_type, sizeof(uint8_t)) != (ssize_t)sizeof(uint8_t)) return -1;
    if (fs_read(fmv->video_fd, &fmv->video_width, sizeof(uint16_t)) != (ssize_t)sizeof(uint16_t)) return -1;
    if (fs_read(fmv->video_fd, &fmv->video_height, sizeof(uint16_t)) != (ssize_t)sizeof(uint16_t)) return -1;
    if (fs_read(fmv->video_fd, &fmv->content_width, sizeof(uint16_t)) != (ssize_t)sizeof(uint16_t)) return -1;
    if (fs_read(fmv->video_fd, &fmv->content_height, sizeof(uint16_t)) != (ssize_t)sizeof(uint16_t)) return -1;
    if (fs_read(fmv->video_fd, &fmv->fps, sizeof(float)) != (ssize_t)sizeof(float)) return -1;
    if (fs_read(fmv->video_fd, &fmv->sample_rate, sizeof(uint16_t)) != (ssize_t)sizeof(uint16_t)) return -1;
    if (fs_read(fmv->video_fd, &fmv->audio_channels, sizeof(uint16_t)) != (ssize_t)sizeof(uint16_t)) return -1;
    if (fs_read(fmv->video_fd, &fmv->num_unique_frames, sizeof(uint32_t)) != (ssize_t)sizeof(uint32_t)) return -1;
    if (fs_read(fmv->video_fd, &fmv->num_total_frames, sizeof(uint32_t)) != (ssize_t)sizeof(uint32_t)) return -1;
    if (fs_read(fmv->video_fd, &fmv->video_frame_size, sizeof(uint32_t)) != (ssize_t)sizeof(uint32_t)) return -1;
    if (fs_read(fmv->video_fd, &fmv->max_compressed_size, sizeof(uint32_t)) != (ssize_t)sizeof(uint32_t)) return -1;
    if (fs_read(fmv->video_fd, &fmv->audio_offset, sizeof(uint32_t)) != (ssize_t)sizeof(uint32_t)) return -1;
    if (fs_read(fmv->video_fd, &compression_type, sizeof(uint8_t)) != (ssize_t)sizeof(uint8_t)) return -1;

    fmv->use_zstd = (compression_type == 1);
    fmv->frame_duration = 1000.0f / fmv->fps;
    dcfmv_init_timebase_from_fps(fmv, fmv->fps);

    fmv->media_info.version = version;
    fmv->media_info.compression_type = compression_type;
    fmv->media_info.frame_type = (uint8_t)fmv->frame_type;
    fmv->media_info.tex_width = (uint16_t)fmv->video_width;
    fmv->media_info.tex_height = (uint16_t)fmv->video_height;
    fmv->media_info.content_width = (uint16_t)fmv->content_width;
    fmv->media_info.content_height = (uint16_t)fmv->content_height;
    fmv->media_info.fps = fmv->fps;
    fmv->media_info.sample_rate = (uint16_t)fmv->sample_rate;
    fmv->media_info.channels = (uint16_t)fmv->audio_channels;
    fmv->media_info.num_unique_frames = (uint32_t)fmv->num_unique_frames;
    fmv->media_info.num_total_frames = (uint32_t)fmv->num_total_frames;
    fmv->media_info.uncompressed_frame_size = (uint32_t)fmv->video_frame_size;
    fmv->media_info.max_compressed_frame_size = (uint32_t)fmv->max_compressed_size;
    fmv->backend_kind = DCFMV_BACKEND_FRAMES;

    return 0;
}

static int dcfmv_load_frame_tables(dcfmv_t *fmv) {
    int t = 0;

    if (!fmv || fmv->video_fd < 0) return -1;

    fmv->compressed_buffer = memalign(32, fmv->max_compressed_size);
    if (!fmv->compressed_buffer) return -1;

    fmv->frame_offsets = malloc((fmv->num_unique_frames + 1) * sizeof(uint32_t));
    fmv->frame_durations = malloc(fmv->num_unique_frames * sizeof(uint16_t));
    fmv->GTotalToUnique = malloc(fmv->num_total_frames * sizeof(int));
    if (!fmv->frame_offsets || !fmv->frame_durations || !fmv->GTotalToUnique)
        return -1;

    fs_seek(fmv->video_fd, 50, SEEK_SET);
    if (fs_read(fmv->video_fd, fmv->frame_offsets,
                (fmv->num_unique_frames + 1) * sizeof(uint32_t)) < 0)
        return -1;
    if (fs_read(fmv->video_fd, fmv->frame_durations,
                fmv->num_unique_frames * sizeof(uint16_t)) < 0)
        return -1;

    for (int u = 0; u < fmv->num_unique_frames; u++) {
        for (int i = 0; i < fmv->frame_durations[u] && t < fmv->num_total_frames; i++) {
            fmv->GTotalToUnique[t++] = u;
        }
    }
    while (t < fmv->num_total_frames) {
        fmv->GTotalToUnique[t++] = fmv->num_unique_frames > 0 ? (fmv->num_unique_frames - 1) : 0;
    }

    for (int i = 0; i < DCFMV_NUM_BUFFERS; i++) {
        fmv->frame_buffer[i] = memalign(32, fmv->video_frame_size);
        if (!fmv->frame_buffer[i]) return -1;
        atomic_store(&fmv->buf_state[i], DCFMV_BUF_EMPTY);
    }

    return 0;
}

static int dcfmv_alloc_video_buffers(dcfmv_t *fmv) {
    if (!fmv) return -1;

    for (int i = 0; i < DCFMV_NUM_BUFFERS; i++) {
        if (!fmv->frame_buffer[i]) {
            fmv->frame_buffer[i] = memalign(32, fmv->video_frame_size);
            if (!fmv->frame_buffer[i]) {
                printf("❌ [video] frame_buffer[%d] alloc failed (%d bytes)\n",
                       i, fmv->video_frame_size);
                return -1;
            }
        }
        atomic_store(&fmv->buf_state[i], DCFMV_BUF_EMPTY);
    }

    return 0;
}

static int dcfmv_measure_audio_region(dcfmv_t *fmv) {
    long curpos;
    long total_size;
    long audio_bytes_total;

    if (!fmv || fmv->video_fd < 0) return -1;

    curpos = fs_tell(fmv->video_fd);
    fs_seek(fmv->video_fd, 0, SEEK_END);
    total_size = fs_tell(fmv->video_fd);
    fs_seek(fmv->video_fd, curpos, SEEK_SET);

    audio_bytes_total = total_size - fmv->audio_offset;
    fmv->left_channel_size = (fmv->audio_channels == 2)
        ? (audio_bytes_total / 2)
        : audio_bytes_total;
    return 0;
}

static int dcfmv_chunk_find_for_frame(const dcfmv_t *fmv, int total_frame) {
    dcfmv_chunk_entry_t *chunk_index;
    int lo;
    int hi;

    if (!fmv || !fmv->chunk_index_data || !fmv->chunk_count)
        return 0;

    chunk_index = (dcfmv_chunk_entry_t *)fmv->chunk_index_data;
    if (total_frame < 0) total_frame = 0;
    if (total_frame >= fmv->num_total_frames) total_frame = fmv->num_total_frames - 1;

    lo = 0;
    hi = (int)fmv->chunk_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t start = chunk_index[mid].start_frame;
        uint32_t count = chunk_index[mid].num_frames;
        if ((uint32_t)total_frame < start) {
            hi = mid - 1;
        } else if ((uint32_t)total_frame >= start + count) {
            lo = mid + 1;
        } else {
            return mid;
        }
    }
    return (int)fmv->chunk_count - 1;
}

static int dcfmv_chunk_video_preload_limit(const dcfmv_t *fmv, int frame) {
    int chunk_id;
    int limit;

    if (!fmv || !fmv->chunk_index_data || !fmv->chunk_count)
        return 0;

    chunk_id = dcfmv_chunk_find_for_frame(fmv, frame);
    if (chunk_id < 0 || (uint32_t)chunk_id >= fmv->chunk_count)
        return 0;

    limit = (int)((dcfmv_chunk_entry_t *)fmv->chunk_index_data)[chunk_id].num_frames;
    if (limit < 2)
        limit = 2;

    return limit;
}

static uint32_t dcfmv_chunk_max_disk_bytes(dcfmv_t *fmv) {
    dcfmv_chunk_entry_t *chunk_index;
    uint32_t max_bytes = 0;

    if (!fmv || !fmv->chunk_index_data)
        return 0;

    chunk_index = (dcfmv_chunk_entry_t *)fmv->chunk_index_data;
    for (uint32_t i = 0; i < fmv->chunk_count; i++) {
        dcfmv_chunk_entry_t *entry = &chunk_index[i];
        uint32_t video_bytes = entry->video_section_size +
            dcfmv_pad32_after(entry->chunk_offset + entry->video_section_size);
        uint32_t audio_bytes = dcfmv_align32(entry->audio_size) * (uint32_t)fmv->audio_channels;
        uint32_t total = video_bytes + audio_bytes;
        if (total > max_bytes)
            max_bytes = total;
    }
    return dcfmv_align32(max_bytes + 64);
}

static uint32_t dcfmv_chunk_max_frames_per_chunk(dcfmv_t *fmv) {
    uint32_t n = (uint32_t)((double)fmv->fps * (double)fmv->chunk_duration + 8.0);
    return n > 8u ? n : 8u;
}

static void dcfmv_chunk_build_frame_map(dcfmv_t *fmv, dcfmv_chunk_cache_slot_t *slot, int chunk_id) {
    dcfmv_chunk_entry_t *entry = &((dcfmv_chunk_entry_t *)fmv->chunk_index_data)[chunk_id];
    uint32_t n = entry->num_frames < slot->map_cap ? entry->num_frames : slot->map_cap;
    uint32_t seen_cap = slot->seen_cap < n ? slot->seen_cap : n;
    uint32_t seen_cnt = 0;

    for (uint32_t j = 0; j < n; j++) {
        int unique_frame = dcfmv_total_to_unique(fmv, (int)entry->start_frame + (int)j);
        if (unique_frame < 0) unique_frame = 0;
        if (unique_frame >= fmv->num_unique_frames)
            unique_frame = fmv->num_unique_frames > 0 ? (fmv->num_unique_frames - 1) : 0;
        uint32_t size = dcfmv_frame_comp_size(fmv, (uint32_t)unique_frame);
        if (!size) size = 1;
        int found = 0;
        for (uint32_t k = 0; k < seen_cnt; k++) {
            if (slot->seen_u[k] == (uint32_t)unique_frame) {
                found = 1;
                break;
            }
        }
        if (!found) {
            if (seen_cnt < seen_cap)
                slot->seen_u[seen_cnt++] = (uint32_t)unique_frame;
        }
    }

    seen_cnt = 0;
    uint32_t cur_off = 0;
    for (uint32_t j = 0; j < n; j++) {
        int unique_frame = dcfmv_total_to_unique(fmv, (int)entry->start_frame + (int)j);
        if (unique_frame < 0) unique_frame = 0;
        if (unique_frame >= fmv->num_unique_frames)
            unique_frame = fmv->num_unique_frames > 0 ? (fmv->num_unique_frames - 1) : 0;
        uint32_t size = dcfmv_frame_comp_size(fmv, (uint32_t)unique_frame);
        if (!size) size = 1;
        uint32_t off = cur_off;
        int found = 0;

        for (uint32_t k = 0; k < seen_cnt; k++) {
            if (slot->seen_u[k] == (uint32_t)unique_frame) {
                off = slot->seen_off[k];
                found = 1;
                break;
            }
        }
        if (!found) {
            if (seen_cnt < seen_cap) {
                slot->seen_u[seen_cnt] = (uint32_t)unique_frame;
                slot->seen_off[seen_cnt] = cur_off;
                seen_cnt++;
            }
            cur_off += size;
        }
        slot->frame_off_local[j] = off;
        slot->frame_sz_local[j] = size;
    }
}

static int dcfmv_chunk_init_cache(dcfmv_t *fmv) {
    dcfmv_chunk_cache_slot_t *slots;
    uint32_t slot_bytes;
    uint32_t map_cap;

    if (!fmv) return -1;

    fmv->chunk_cache_slots = 12;
    slot_bytes = dcfmv_chunk_max_disk_bytes(fmv);
    map_cap = dcfmv_chunk_max_frames_per_chunk(fmv);
    slots = calloc(fmv->chunk_cache_slots, sizeof(*slots));
    if (!slots) return -1;

    for (int i = 0; i < fmv->chunk_cache_slots; i++) {
        slots[i].data = memalign(32, slot_bytes);
        slots[i].frame_off_local = malloc(map_cap * sizeof(uint32_t));
        slots[i].frame_sz_local = malloc(map_cap * sizeof(uint32_t));
        slots[i].seen_u = malloc(map_cap * sizeof(uint32_t));
        slots[i].seen_off = malloc(map_cap * sizeof(uint32_t));
        if (!slots[i].data || !slots[i].frame_off_local || !slots[i].frame_sz_local ||
            !slots[i].seen_u || !slots[i].seen_off) {
            free(slots[i].data);
            free(slots[i].frame_off_local);
            free(slots[i].frame_sz_local);
            free(slots[i].seen_u);
            free(slots[i].seen_off);
            free(slots);
            return -1;
        }
        slots[i].chunk_id = -1;
        slots[i].size = slot_bytes;
        slots[i].map_cap = map_cap;
        slots[i].seen_cap = map_cap;
        slots[i].ready = 0;
    }

    fmv->chunk_cache_data = slots;
    fmv->global_cache_tick = 0;
    return 0;
}

static dcfmv_chunk_cache_slot_t *dcfmv_chunk_cache_find(dcfmv_t *fmv, int chunk_id) {
    dcfmv_chunk_cache_slot_t *slots = (dcfmv_chunk_cache_slot_t *)fmv->chunk_cache_data;

    if (!slots) return NULL;
    for (int i = 0; i < fmv->chunk_cache_slots; i++) {
        if (slots[i].ready && slots[i].chunk_id == chunk_id) {
            slots[i].last_used = ++fmv->global_cache_tick;
            return &slots[i];
        }
    }
    return NULL;
}

static dcfmv_chunk_cache_slot_t *dcfmv_chunk_cache_evict(dcfmv_t *fmv) {
    dcfmv_chunk_cache_slot_t *slots = (dcfmv_chunk_cache_slot_t *)fmv->chunk_cache_data;
    dcfmv_chunk_cache_slot_t *lru = NULL;
    int current_video_chunk = -1;
    int next_video_chunk = -1;

    if (!slots) return NULL;
    if (fmv->num_total_frames > 0) {
        int current_frame = atomic_load(&fmv->frame_index);

        if (current_frame < 0)
            current_frame = 0;
        if (current_frame >= fmv->num_total_frames)
            current_frame = fmv->num_total_frames - 1;

        current_video_chunk = dcfmv_chunk_find_for_frame(fmv, current_frame);
        if (current_frame + 1 < fmv->num_total_frames)
            next_video_chunk = dcfmv_chunk_find_for_frame(fmv, current_frame + 1);
    }

    for (int i = 0; i < fmv->chunk_cache_slots; i++) {
        if (!slots[i].ready)
            return &slots[i];
        /* Don't evict active audio chunks or the current video runway chunks. */
        if (slots[i].chunk_id == fmv->current_audio_chunk ||
            slots[i].chunk_id == fmv->current_audio_chunk + 1 ||
            slots[i].chunk_id == current_video_chunk ||
            slots[i].chunk_id == next_video_chunk)
            continue;
        if (!lru || slots[i].last_used < lru->last_used)
            lru = &slots[i];
    }
    return lru;
}

static int dcfmv_chunk_load_sync(dcfmv_t *fmv, int chunk_id) {
    dcfmv_chunk_entry_t *entry;
    dcfmv_chunk_cache_slot_t *slot;
    uint32_t video_real;
    uint32_t video_disk;
    uint32_t audio_disk;
    uint32_t total_disk;

    if (!fmv || !fmv->chunk_index_data)
        return -1;
    if (dcfmv_chunk_cache_find(fmv, chunk_id))
        return 0;

    entry = &((dcfmv_chunk_entry_t *)fmv->chunk_index_data)[chunk_id];
    slot = dcfmv_chunk_cache_evict(fmv);
    if (!slot)
        return -1;

    video_real = entry->video_section_size;
    video_disk = video_real + dcfmv_pad32_after(entry->chunk_offset + video_real);
    audio_disk = dcfmv_align32(entry->audio_size) * (uint32_t)fmv->audio_channels;
    total_disk = video_disk + audio_disk;
    if (total_disk > slot->size)
        return -1;

    mutex_lock(&dcfmv_io_lock);
    fs_seek(fmv->video_fd, entry->chunk_offset, SEEK_SET);
    if (fs_read(fmv->video_fd, slot->data, total_disk) != (ssize_t)total_disk) {
        mutex_unlock(&dcfmv_io_lock);
        return -1;
    }
    mutex_unlock(&dcfmv_io_lock);

    slot->chunk_id = chunk_id;
    slot->video_section = slot->data;
    slot->video_bytes_size = video_real;
    slot->audio_L = slot->data + video_disk;
    slot->audio_R = fmv->audio_channels == 2 ? (slot->audio_L + dcfmv_align32(entry->audio_size)) : NULL;
    slot->last_used = ++fmv->global_cache_tick;
    slot->ready = 1;
    dcfmv_chunk_build_frame_map(fmv, slot, chunk_id);
    DCMV_LOG(DCFMV_LOG_CHUNK, "[Chunk] load id=%d start=%u frames=%u off=0x%08lx video_real=%u video_disk=%u audio_disk=%u first=%02x %02x %02x %02x",
             chunk_id,
             (unsigned)entry->start_frame,
             (unsigned)entry->num_frames,
             (unsigned long)entry->chunk_offset,
             (unsigned)video_real,
             (unsigned)video_disk,
             (unsigned)audio_disk,
             slot->data[0],
             slot->data[1],
             slot->data[2],
             slot->data[3]);
    return 0;
}

static int dcfmv_chunk_load_header(dcfmv_t *fmv) {
    dcfmv_chunk_header_t header;

    if (!fmv || fmv->video_fd < 0)
        return -1;

    fs_seek(fmv->video_fd, 0, SEEK_SET);
    if (fs_read(fmv->video_fd, &header, sizeof(header)) != (ssize_t)sizeof(header))
        return -1;
    if (memcmp(header.magic, DCFMV_MAGIC, 4) != 0 || header.version != 1u)
        return -1;
    if (!header.num_unique_frames || !header.num_total_frames || !header.num_chunks ||
        !header.uncompressed_frame_size || !header.max_compressed_frame_size ||
        header.fps <= 0.0f || header.chunk_duration <= 0.0f) {
        DCMV_Error("PANIC: Invalid DCMV v1 header values");
        return -1;
    }

    fmv->frame_type = header.frame_type;
    fmv->video_width = header.tex_width;
    fmv->video_height = header.tex_height;
    fmv->content_width = header.content_width;
    fmv->content_height = header.content_height;
    fmv->fps = header.fps;
    fmv->sample_rate = header.sample_rate;
    fmv->audio_channels = header.channels;
    fmv->num_unique_frames = (int)header.num_unique_frames;
    fmv->num_total_frames = (int)header.num_total_frames;
    fmv->video_frame_size = (int)header.uncompressed_frame_size;
    fmv->max_compressed_size = (int)header.max_compressed_frame_size;
    fmv->audio_offset = 0;
    fmv->chunk_duration = header.chunk_duration;
    fmv->chunk_count = header.num_chunks;
    fmv->chunk_index_offset = header.chunk_index_offset;
    fmv->use_zstd = header.compression_type == 1;
    fmv->frame_duration = 1000.0f / fmv->fps;
    dcfmv_init_timebase_from_fps(fmv, fmv->fps);

    fmv->media_info.version = header.version;
    fmv->media_info.compression_type = header.compression_type;
    fmv->media_info.frame_type = header.frame_type;
    fmv->media_info.tex_width = header.tex_width;
    fmv->media_info.tex_height = header.tex_height;
    fmv->media_info.content_width = header.content_width;
    fmv->media_info.content_height = header.content_height;
    fmv->media_info.fps = header.fps;
    fmv->media_info.sample_rate = header.sample_rate;
    fmv->media_info.channels = header.channels;
    fmv->media_info.num_unique_frames = header.num_unique_frames;
    fmv->media_info.num_total_frames = header.num_total_frames;
    fmv->media_info.uncompressed_frame_size = header.uncompressed_frame_size;
    fmv->media_info.max_compressed_frame_size = header.max_compressed_frame_size;
    fmv->backend_kind = DCFMV_BACKEND_CHUNKS;
    DCMV_LOG(DCFMV_LOG_CHUNK, "[Chunk] hdr v%u frame=%ux%u content=%ux%u fps=%.2f unique=%u total=%u frame_size=%u max_comp=%u chunk_dur=%.2f chunks=%u index_off=0x%08lx",
             (unsigned)header.version,
             (unsigned)header.tex_width,
             (unsigned)header.tex_height,
             (unsigned)header.content_width,
             (unsigned)header.content_height,
             header.fps,
             (unsigned)header.num_unique_frames,
             (unsigned)header.num_total_frames,
             (unsigned)header.uncompressed_frame_size,
             (unsigned)header.max_compressed_frame_size,
             header.chunk_duration,
             (unsigned)header.num_chunks,
             (unsigned long)header.chunk_index_offset);
    return 0;
}

static int dcfmv_chunk_load_tables(dcfmv_t *fmv) {
    int t = 0;
    long file_size;
    uint8_t *raw;
    dcfmv_chunk_entry_t *chunk_index;
    uint32_t min_off;
    uint32_t prev = 0;

    if (!fmv) return -1;

    fs_seek(fmv->video_fd, sizeof(dcfmv_chunk_header_t), SEEK_SET);
    fmv->frame_sizes = malloc((fmv->num_unique_frames + 1) * sizeof(uint32_t));
    fmv->frame_durations = malloc(fmv->num_unique_frames * sizeof(uint16_t));
    fmv->GTotalToUnique = malloc(fmv->num_total_frames * sizeof(int));
    if (!fmv->frame_sizes || !fmv->frame_durations || !fmv->GTotalToUnique)
        return -1;

    if (fs_read(fmv->video_fd, fmv->frame_sizes,
                (fmv->num_unique_frames + 1) * sizeof(uint32_t)) < 0)
        return -1;
    if (fs_read(fmv->video_fd, fmv->frame_durations,
                fmv->num_unique_frames * sizeof(uint16_t)) < 0)
        return -1;

    for (int u = 0; u < fmv->num_unique_frames; u++) {
        uint16_t duration = fmv->frame_durations[u] ? fmv->frame_durations[u] : 1;
        for (uint16_t i = 0; i < duration && t < fmv->num_total_frames; i++) {
            fmv->GTotalToUnique[t++] = u;
        }
    }
    while (t < fmv->num_total_frames) {
        fmv->GTotalToUnique[t++] = fmv->num_unique_frames > 0 ? (fmv->num_unique_frames - 1) : 0;
    }

    raw = malloc(fmv->chunk_count * 20u);
    chunk_index = calloc(fmv->chunk_count, sizeof(*chunk_index));
    if (!raw || !chunk_index) {
        free(raw);
        free(chunk_index);
        return -1;
    }

    fs_seek(fmv->video_fd, fmv->chunk_index_offset, SEEK_SET);
    if (fs_read(fmv->video_fd, raw, fmv->chunk_count * 20u) != (ssize_t)(fmv->chunk_count * 20u)) {
        free(raw);
        free(chunk_index);
        return -1;
    }

    for (uint32_t i = 0; i < fmv->chunk_count; i++) {
        memcpy(&chunk_index[i].chunk_offset, raw + i * 20u + 0u, 4);
        memcpy(&chunk_index[i].video_section_size, raw + i * 20u + 4u, 4);
        memcpy(&chunk_index[i].audio_size, raw + i * 20u + 8u, 4);
        memcpy(&chunk_index[i].start_frame, raw + i * 20u + 12u, 4);
        memcpy(&chunk_index[i].num_frames, raw + i * 20u + 16u, 4);
    }
    free(raw);

    file_size = fs_tell(fmv->video_fd);
    fs_seek(fmv->video_fd, 0, SEEK_END);
    file_size = fs_tell(fmv->video_fd);
    fs_seek(fmv->video_fd, sizeof(dcfmv_chunk_header_t), SEEK_SET);

    min_off = (uint32_t)sizeof(dcfmv_chunk_header_t) +
              (fmv->num_unique_frames + 1) * 4u +
              fmv->num_unique_frames * 2u;
    if (min_off < 0x80u)
        min_off = 0x80u;

    for (uint32_t i = 0; i < fmv->chunk_count; i++) {
        dcfmv_chunk_entry_t *entry = &chunk_index[i];
        uint64_t end = (uint64_t)entry->chunk_offset + entry->video_section_size +
            dcfmv_pad32_after(entry->chunk_offset + entry->video_section_size) +
            (uint64_t)dcfmv_align32(entry->audio_size) * (uint64_t)fmv->audio_channels;

        if (entry->chunk_offset < min_off || (long)entry->chunk_offset >= file_size ||
            (i > 0 && entry->chunk_offset < prev) ||
            !entry->video_section_size || !entry->num_frames ||
            entry->start_frame >= (uint32_t)fmv->num_total_frames ||
            end > (uint64_t)file_size) {
            free(chunk_index);
            return -1;
        }
        prev = entry->chunk_offset;
    }

    fmv->chunk_index_data = chunk_index;
    return 0;
}

static int dcfmv_frames_open(dcfmv_t *fmv) {
    if (dcfmv_read_header_v6(fmv) != 0)
        return -1;
    if (dcfmv_load_frame_tables(fmv) != 0)
        return -1;
    if (dcfmv_measure_audio_region(fmv) != 0)
        return -1;
    return 0;
}

static void dcfmv_frames_close(dcfmv_t *fmv) {
    (void)fmv;
}

static int dcfmv_frames_decode_frame(dcfmv_t *fmv, int total_frame, int buf_index) {
    uint32_t offset;
    uint32_t next_offset;
    uint32_t compressed_size;
    int unique_frame;

    if (!fmv || total_frame < 0 || buf_index < 0 || buf_index >= DCFMV_NUM_BUFFERS)
        return -1;

    unique_frame = dcfmv_total_to_unique(fmv, total_frame);
    offset = fmv->frame_offsets[unique_frame];
    next_offset = fmv->frame_offsets[unique_frame + 1];
    compressed_size = next_offset - offset;

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
        ZSTD_inBuffer in;
        ZSTD_outBuffer out;
        size_t ret = 1;

        if (!dcfmv_zstd_dctx) return -1;
        ZSTD_DCtx_reset(dcfmv_zstd_dctx, ZSTD_reset_session_only);
        in.src = fmv->compressed_buffer;
        in.size = compressed_size;
        in.pos = 0;
        out.dst = fmv->frame_buffer[buf_index];
        out.size = (size_t)fmv->video_frame_size;
        out.pos = 0;

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
        DCMV_LOG(DCFMV_LOG_DECODE, "[LZ4] frame=%d buf=%d compressed=%lu decoded=%d time=%.3fms",
                 unique_frame,
                 buf_index,
                 (unsigned long)compressed_size,
                 fmv->video_frame_size,
                 decode_elapsed_ms);
    }

    atomic_store(&fmv->buf_state[buf_index], DCFMV_BUF_READY);
    return 0;
}

static int dcfmv_frames_seek_video(dcfmv_t *fmv, int total_frame) {
    int uf;
    uint32_t off;

    if (!fmv) return -1;

    mutex_lock(&dcfmv_io_lock);
    fs_close(fmv->video_fd);
    thd_sleep(10);
    fmv->video_fd = fs_open(fmv->path, O_RDONLY);
    mutex_unlock(&dcfmv_io_lock);
    if (fmv->video_fd < 0)
        return -1;

    uf = dcfmv_total_to_unique_frame(fmv, total_frame);
    off = fmv->frame_offsets[uf];

    mutex_lock(&dcfmv_io_lock);
    fs_seek(fmv->video_fd, off, SEEK_SET);
    mutex_unlock(&dcfmv_io_lock);

    fmv->vfd_last_end = off;
    return 0;
}

static int dcfmv_frames_seek_audio(dcfmv_t *fmv, int total_frame) {
    double samples_exact;
    uint32_t samples_i;
    uint32_t bytes_per_channel;
    long left_offset;
    long right_offset;
    long right_limit;

    if (!fmv) return -1;
    if (fmv->audio_channels <= 0)
        return 0;

    samples_exact = ((double)total_frame * (double)fmv->sample_rate) / (double)fmv->fps;
    samples_i = (uint32_t)(samples_exact + 0.5);
    bytes_per_channel = (samples_i / 2);
    bytes_per_channel = (bytes_per_channel + 15) & ~0xF;

    left_offset = fmv->audio_offset + (long)bytes_per_channel;
    if (left_offset > (fmv->audio_offset + fmv->left_channel_size))
        left_offset = fmv->audio_offset + fmv->left_channel_size;

    right_offset = fmv->audio_offset + fmv->left_channel_size + (long)bytes_per_channel;
    right_limit = fmv->audio_offset + (long)fmv->left_channel_size * 2;
    if (right_offset > right_limit)
        right_offset = right_limit;

    mutex_lock(&dcfmv_io_lock);

    fs_close(fmv->audio_fd_left);
    fmv->audio_fd_left = fs_open(fmv->path, O_RDONLY);
    if (fmv->audio_fd_left >= 0)
        fs_seek(fmv->audio_fd_left, left_offset, SEEK_SET);

    if (fmv->audio_channels == 2) {
        fs_close(fmv->audio_fd_right);
        fmv->audio_fd_right = fs_open(fmv->path, O_RDONLY);
        if (fmv->audio_fd_right >= 0)
            fs_seek(fmv->audio_fd_right, right_offset, SEEK_SET);
    }

    mutex_unlock(&dcfmv_io_lock);

    fmv->last_audio_left_pos = left_offset;
    fmv->last_audio_right_pos = right_offset;
    return 0;
}

// static int dcfmv_chunks_open(dcfmv_t *fmv) {
//     if (dcfmv_chunk_load_header(fmv) != 0)
//         return -1;
//     if (dcfmv_chunk_load_tables(fmv) != 0)
//         return -1;
//     if (dcfmv_chunk_init_cache(fmv) != 0)
//         return -1;
//     if (dcfmv_alloc_video_buffers(fmv) != 0)
//         return -1;
//     dcfmv_set_audio_enabled(fmv, 0);
//     for (int i = 0; i < fmv->chunk_cache_slots && i < (int)fmv->chunk_count; i++) {
//         if (dcfmv_chunk_load_sync(fmv, i) != 0)
//             return -1;
//     }
//     return 0;
// }

static void dcfmv_chunks_close(dcfmv_t *fmv) {
    (void)fmv;
}

static int dcfmv_chunks_decode_frame(dcfmv_t *fmv, int total_frame, int buf_index) {
    int chunk_id;
    dcfmv_chunk_entry_t *entry;
    dcfmv_chunk_cache_slot_t *slot;
    int local_frame;
    uint32_t off;
    uint32_t sz;
    int unique_frame;

    if (!fmv || buf_index < 0 || buf_index >= DCFMV_NUM_BUFFERS)
        return -1;

    chunk_id = dcfmv_chunk_find_for_frame(fmv, total_frame);
    if (dcfmv_chunk_load_sync(fmv, chunk_id) != 0)
        return -1;

    slot = dcfmv_chunk_cache_find(fmv, chunk_id);
    if (!slot)
        return -1;

    entry = &((dcfmv_chunk_entry_t *)fmv->chunk_index_data)[chunk_id];
    local_frame = total_frame - (int)entry->start_frame;
    if (local_frame < 0 || (uint32_t)local_frame >= entry->num_frames || (uint32_t)local_frame >= slot->map_cap)
        return -1;

    off = slot->frame_off_local[local_frame];
    sz = slot->frame_sz_local[local_frame];
    if (off + sz > slot->video_bytes_size)
        return -1;

    unique_frame = dcfmv_total_to_unique(fmv, total_frame);
    if (fmv->use_zstd == 1) {
        ZSTD_inBuffer in;
        ZSTD_outBuffer out;
        size_t ret = 1;

        if (!dcfmv_zstd_dctx) return -1;
        ZSTD_DCtx_reset(dcfmv_zstd_dctx, ZSTD_reset_session_only);
        in.src = slot->video_section + off;
        in.size = sz;
        in.pos = 0;
        out.dst = fmv->frame_buffer[buf_index];
        out.size = (size_t)fmv->video_frame_size;
        out.pos = 0;

        while (ret != 0 && out.pos < out.size) {
            ret = ZSTD_decompressStream(dcfmv_zstd_dctx, &out, &in);
            if (ZSTD_isError(ret)) return -1;
        }
        if (out.pos != (size_t)fmv->video_frame_size) return -1;
    } else {
        double decode_start_ms = dcfmv_decode_timer_ms();
        int res = LZ4_decompress_safe(
            (const char *)(slot->video_section + off),
            (char *)fmv->frame_buffer[buf_index],
            (int)sz,
            fmv->video_frame_size);
        double decode_elapsed_ms = dcfmv_decode_timer_ms() - decode_start_ms;
        if (res != fmv->video_frame_size) {
            DCMV_Error("LZ4_decompress_safe failed for total frame %d unique %d (buf %d): out=%d expected=%d",
                       total_frame, unique_frame, buf_index, res, fmv->video_frame_size);
            return -1;
        }
        DCMV_LOG(DCFMV_LOG_DECODE, "[LZ4-chunk] total=%d unique=%d buf=%d compressed=%lu decoded=%d time=%.3fms",
                 total_frame, unique_frame, buf_index, (unsigned long)sz, fmv->video_frame_size, decode_elapsed_ms);
    }

    atomic_store(&fmv->buf_state[buf_index], DCFMV_BUF_READY);
    return 0;
}

static int dcfmv_chunks_seek_video(dcfmv_t *fmv, int total_frame) {
    int chunk_id;

    if (!fmv) return -1;
    for (int i = 0; i < fmv->chunk_cache_slots; i++) {
        dcfmv_chunk_cache_slot_t *slots = (dcfmv_chunk_cache_slot_t *)fmv->chunk_cache_data;
        slots[i].ready = 0;
        slots[i].chunk_id = -1;
    }

    chunk_id = dcfmv_chunk_find_for_frame(fmv, total_frame);
    if (dcfmv_chunk_load_sync(fmv, chunk_id) != 0)
        return -1;
    if ((uint32_t)(chunk_id + 1) < fmv->chunk_count)
        (void)dcfmv_chunk_load_sync(fmv, chunk_id + 1);
    return 0;
}

static void dcfmv_chunk_refill_audio_ring(dcfmv_t *fmv) {
    const size_t WANT = DCFMV_AUDIO_BUFFER_BYTES & ~31u;
    /*
     * Keep the chunk audio runway short so it does not monopolize the shared
     * chunk cache and starve nearby video loads.
     */
    const int TARGET  = 4;

    int wi   = atomic_load(&fmv->chunk_audio_write_idx);
    int ri   = atomic_load(&fmv->chunk_audio_read_idx);
    int fill = (wi - ri + DCFMV_AUDIO_RING_SLOTS) % DCFMV_AUDIO_RING_SLOTS;

    while (fill < TARGET) {
        int next = (wi + 1) % DCFMV_AUDIO_RING_SLOTS;
        if (next == ri) break;
        if ((uint32_t)fmv->current_audio_chunk >= fmv->chunk_count) {
            DCMV_LOG(DCFMV_LOG_CHUNK_AUDIO,
                     "[ChunkAudio] refill eof wi=%d ri=%d fill=%d chunk=%d",
                     wi, ri, fill, fmv->current_audio_chunk);
            break;
        }

        memset(fmv->chunk_audio_ring[wi].left,  0, DCFMV_AUDIO_BUFFER_BYTES);
        if (fmv->audio_channels == 2)
            memset(fmv->chunk_audio_ring[wi].right, 0, DCFMV_AUDIO_BUFFER_BYTES);

        size_t done = 0;
        while (done < WANT) {
            if ((uint32_t)fmv->current_audio_chunk >= fmv->chunk_count) break;

            dcfmv_chunk_cache_slot_t *c =
                dcfmv_chunk_cache_find(fmv, fmv->current_audio_chunk);
            if (!c) {
                /* Pull the needed chunk in immediately instead of underrunning. */
                DCMV_LOG(DCFMV_LOG_CHUNK_AUDIO,
                         "[ChunkAudio] cache miss chunk=%d pos=%lu done=%lu",
                         fmv->current_audio_chunk,
                         (unsigned long)fmv->audio_chunk_read_pos,
                         (unsigned long)done);
                if (dcfmv_chunk_load_sync(fmv, fmv->current_audio_chunk) != 0)
                    break;
                c = dcfmv_chunk_cache_find(fmv, fmv->current_audio_chunk);
                if (!c)
                    break;
            }

            dcfmv_chunk_entry_t *e =
                &((dcfmv_chunk_entry_t *)fmv->chunk_index_data)[fmv->current_audio_chunk];
            size_t audio_size_padded = dcfmv_align32(e->audio_size);

            if (fmv->audio_chunk_read_pos >= e->audio_size) {
                DCMV_LOG(DCFMV_LOG_CHUNK_AUDIO,
                         "[ChunkAudio] advance chunk=%d reason=end pos=%lu size=%u",
                         fmv->current_audio_chunk,
                         (unsigned long)fmv->audio_chunk_read_pos,
                         (unsigned)e->audio_size);
                fmv->current_audio_chunk++;
                fmv->audio_chunk_read_pos = 0;
                continue;
            }

            size_t rem  = audio_size_padded - fmv->audio_chunk_read_pos;
            size_t need = WANT - done;
            size_t take = (rem < need ? rem : need) & ~31u;
            if (!take) {
                /* Consume and pad the tail instead of stalling on <32-byte remainder. */
                DCMV_LOG(DCFMV_LOG_CHUNK_AUDIO,
                         "[ChunkAudio] advance chunk=%d reason=tail pos=%lu size=%u padded=%lu",
                         fmv->current_audio_chunk,
                         (unsigned long)fmv->audio_chunk_read_pos,
                         (unsigned)e->audio_size,
                         (unsigned long)audio_size_padded);
                fmv->current_audio_chunk++;
                fmv->audio_chunk_read_pos = 0;
                continue;
            }

            memcpy(fmv->chunk_audio_ring[wi].left + done,
                   c->audio_L + fmv->audio_chunk_read_pos, take);
            if (fmv->audio_channels == 2)
                memcpy(fmv->chunk_audio_ring[wi].right + done,
                       c->audio_R + fmv->audio_chunk_read_pos, take);

            fmv->audio_chunk_read_pos += take;
            done += take;

            if (fmv->audio_chunk_read_pos >= e->audio_size) {
                DCMV_LOG(DCFMV_LOG_CHUNK_AUDIO,
                         "[ChunkAudio] advance chunk=%d reason=filled pos=%lu size=%u",
                         fmv->current_audio_chunk,
                         (unsigned long)fmv->audio_chunk_read_pos,
                         (unsigned)e->audio_size);
                fmv->current_audio_chunk++;
                fmv->audio_chunk_read_pos = 0;
            }
        }

        done &= ~31u;
        if (done < 32) {
            DCMV_LOG(DCFMV_LOG_CHUNK_AUDIO,
                     "[ChunkAudio] refill short wi=%d ri=%d fill=%d chunk=%d done=%lu",
                     wi, ri, fill, fmv->current_audio_chunk, (unsigned long)done);
            break;
        }

        fmv->chunk_audio_ring[wi].valid_bytes = done;
        dcache_flush_range((uint32)fmv->chunk_audio_ring[wi].left,  (uint32)done);
        if (fmv->audio_channels == 2)
            dcache_flush_range((uint32)fmv->chunk_audio_ring[wi].right, (uint32)done);

        __atomic_store_n(&fmv->chunk_audio_ring[wi].valid, 1, __ATOMIC_RELEASE);
        DCMV_LOG(DCFMV_LOG_CHUNK_AUDIO,
                 "[ChunkAudio] refill slot=%d bytes=%lu next_chunk=%d next_pos=%lu",
                 wi,
                 (unsigned long)done,
                 fmv->current_audio_chunk,
                 (unsigned long)fmv->audio_chunk_read_pos);
        wi = next;
        atomic_store(&fmv->chunk_audio_write_idx, wi);

        ri   = atomic_load(&fmv->chunk_audio_read_idx);
        fill = (wi - ri + DCFMV_AUDIO_RING_SLOTS) % DCFMV_AUDIO_RING_SLOTS;
    }
}

static int dcfmv_chunks_seek_audio(dcfmv_t *fmv, int total_frame) {
    if (!fmv || fmv->audio_channels <= 0) return 0;

    int cid = dcfmv_chunk_find_for_frame(fmv, total_frame);
    dcfmv_chunk_entry_t *entry = &((dcfmv_chunk_entry_t *)fmv->chunk_index_data)[cid];

    int in_chunk = total_frame - (int)entry->start_frame;
    if (in_chunk < 0) in_chunk = 0;
    if ((uint32_t)in_chunk > entry->num_frames) in_chunk = (int)entry->num_frames;

    uint32_t pos = 0;
    if (entry->num_frames > 0) {
        pos = (uint32_t)(((uint64_t)in_chunk * (uint64_t)entry->audio_size) /
                         (uint64_t)entry->num_frames);
    }
    pos &= ~31u;
    if (pos > entry->audio_size) pos = entry->audio_size;

    fmv->current_audio_chunk  = cid;
    fmv->audio_chunk_read_pos = pos;

    /* advance to next chunk if we're at the end */
    if (fmv->audio_chunk_read_pos >= entry->audio_size &&
        (uint32_t)(cid + 1) < fmv->chunk_count) {
        fmv->current_audio_chunk++;
        fmv->audio_chunk_read_pos = 0;
    }

    DCMV_LOG(DCFMV_LOG_CHUNK_AUDIO,
             "[ChunkAudio] seek frame=%d chunk=%d pos=%u size=%u next_chunk=%d",
             total_frame,
             cid,
             (unsigned)pos,
             (unsigned)entry->audio_size,
             fmv->current_audio_chunk);

    /* clear the ring */
    atomic_store(&fmv->chunk_audio_write_idx, 0);
    atomic_store(&fmv->chunk_audio_read_idx,  0);
    fmv->chunk_audio_ring_read_pos = 0;
    for (int i = 0; i < DCFMV_AUDIO_RING_SLOTS; i++) {
        atomic_store(&fmv->chunk_audio_ring[i].valid, 0);
        fmv->chunk_audio_ring[i].valid_bytes = 0;
    }

    return 0;
}

static int dcfmv_clamp_stream_volume(int vol) {
    if (vol < 0) return 0;
    if (vol > 255) return 255;
    return vol;
}

void dcfmv_reset_timing(dcfmv_t *fmv) {
    if (!fmv) return;
    fmv->frame_timer_anchor = 0.0;
    atomic_store(&fmv->audio_start_time_ms, 0.0);
}

void dcfmv_reset_render_tracking(dcfmv_t *fmv) {
    if (!fmv) return;
    fmv->last_unique_frame_drawn = -1;
}

void dcfmv_set_render_resources(dcfmv_t *fmv,
                                pvr_ptr_t txr,
                                const pvr_poly_hdr_t *hdr,
                                const pvr_poly_hdr_t *fallback_hdr,
                                const pvr_vertex_t *vert,
                                const pvr_vertex_t *fallback_vert) {
    if (!fmv) return;
    fmv->pvr_txr = txr;
    if (hdr) memcpy(&fmv->hdr, hdr, sizeof(fmv->hdr));
    if (fallback_hdr) memcpy(&fmv->fallback_hdr, fallback_hdr, sizeof(fmv->fallback_hdr));
    if (vert) memcpy(fmv->vert, vert, sizeof(fmv->vert));
    if (fallback_vert) memcpy(fmv->fallback_vert, fallback_vert, sizeof(fmv->fallback_vert));
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
    DCMV_LOG(DCFMV_LOG_FMV, "[FMV] %s frame=%d paused=%d muted=%d settle=%d hold=%d clock=%d anchor=%.2f base=%.2f stream=%ld L=%d R=%d",
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

    if (fmv->frame_sizes) {
        free(fmv->frame_sizes);
        fmv->frame_sizes = NULL;
    }

    if (fmv->frame_durations) {
        free(fmv->frame_durations);
        fmv->frame_durations = NULL;
    }

    if (fmv->GTotalToUnique) {
        free(fmv->GTotalToUnique);
        fmv->GTotalToUnique = NULL;
    }

    if (fmv->chunk_cache_data) {
        dcfmv_chunk_cache_slot_t *slots = (dcfmv_chunk_cache_slot_t *)fmv->chunk_cache_data;
        for (int i = 0; i < fmv->chunk_cache_slots; i++) {
            free(slots[i].data);
            free(slots[i].frame_off_local);
            free(slots[i].frame_sz_local);
            free(slots[i].seen_u);
            free(slots[i].seen_off);
        }
        free(slots);
        fmv->chunk_cache_data = NULL;
    }

    if (fmv->chunk_index_data) {
        free(fmv->chunk_index_data);
        fmv->chunk_index_data = NULL;
    }

    if (fmv->backend_kind == DCFMV_BACKEND_CHUNKS) {
        for (int i = 0; i < DCFMV_AUDIO_RING_SLOTS; i++) {
            free(fmv->chunk_audio_ring[i].left);
            free(fmv->chunk_audio_ring[i].right);
            fmv->chunk_audio_ring[i].left  = NULL;
            fmv->chunk_audio_ring[i].right = NULL;
        }
    }    
}

static inline int dcfmv_total_to_unique_frame(dcfmv_t *fmv, int total_frame) {
    if (!fmv || (unsigned)total_frame >= (unsigned)fmv->num_total_frames)
        return fmv ? (fmv->num_unique_frames - 1) : 0;
    return fmv->GTotalToUnique[total_frame];
}

static int dcfmv_decode_frame_backend(dcfmv_t *fmv, int unique_frame, int buf_index) {
    const dcfmv_backend_ops_t *ops = dcfmv_backend_ops(fmv);
    return ops && ops->decode_frame ? ops->decode_frame(fmv, unique_frame, buf_index) : -1;
}

static int dcfmv_seek_video_backend(dcfmv_t *fmv, int total_frame) {
    const dcfmv_backend_ops_t *ops = dcfmv_backend_ops(fmv);
    return ops && ops->seek_video ? ops->seek_video(fmv, total_frame) : -1;
}

static int dcfmv_seek_audio_backend(dcfmv_t *fmv, int total_frame) {
    const dcfmv_backend_ops_t *ops = dcfmv_backend_ops(fmv);
    return ops && ops->seek_audio ? ops->seek_audio(fmv, total_frame) : -1;
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
    fmv->audio_start_generation = 0;
    fmv->audio_logged_start_generation = 0;
    fmv->audio_logged_poll_generation = 0;
    fmv->audio_logged_cb_generation = 0;

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
    dcfmv_reset_media_info(fmv);
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

static int dcfmv_chunks_open(dcfmv_t *fmv) {
    if (dcfmv_chunk_load_header(fmv) != 0) return -1;
    if (dcfmv_chunk_load_tables(fmv) != 0) return -1;
    if (dcfmv_chunk_init_cache(fmv) != 0) return -1;
    if (dcfmv_alloc_video_buffers(fmv) != 0) return -1;

    /* DO NOT call dcfmv_set_audio_enabled(fmv, 0) here anymore */

    /* Init chunk audio ring */
    if (fmv->audio_channels > 0) {
        for (int i = 0; i < DCFMV_AUDIO_RING_SLOTS; i++) {
            fmv->chunk_audio_ring[i].left  = memalign(32, DCFMV_AUDIO_BUFFER_BYTES);
            fmv->chunk_audio_ring[i].right = memalign(32, DCFMV_AUDIO_BUFFER_BYTES);
            if (!fmv->chunk_audio_ring[i].left || !fmv->chunk_audio_ring[i].right)
                return -1;
            atomic_store(&fmv->chunk_audio_ring[i].valid, 0);
            fmv->chunk_audio_ring[i].valid_bytes = 0;
        }
        atomic_store(&fmv->chunk_audio_write_idx, 0);
        atomic_store(&fmv->chunk_audio_read_idx,  0);
        fmv->chunk_audio_ring_read_pos = 0;
        fmv->current_audio_chunk = 0;
        fmv->audio_chunk_read_pos = 0;
    }

    for (int i = 0; i < fmv->chunk_cache_slots && i < (int)fmv->chunk_count; i++) {
        if (dcfmv_chunk_load_sync(fmv, i) != 0) return -1;
    }
    return 0;
}

static void dcfmv_close_unlocked(dcfmv_t *fmv);

int dcfmv_open(dcfmv_t *fmv, const char *path) {
    const dcfmv_backend_ops_t *ops;
    uint32_t probed_version = 0;
    int result = -1;

    if (!fmv) return -1;

    mutex_lock(&dcfmv_state_lock);

    dcfmv_close_unlocked(fmv);
    dcfmv_reset_media_info(fmv);

    dcfmv_current = fmv;
    if (path) {
        strncpy(fmv->path, path, sizeof(fmv->path) - 1);
        fmv->path[sizeof(fmv->path) - 1] = '\0';
    } else {
        fmv->path[0] = '\0';
    }

    fmv->video_fd = fs_open(fmv->path, O_RDONLY);
    if (fmv->video_fd < 0) {
        DCMV_Error("PANIC: Failed to open video file: %s", fmv->path);
        goto done;
    }

    if (dcfmv_probe_backend(fmv->video_fd, &fmv->backend_kind, &probed_version) != 0) {
        DCMV_Error("PANIC: Unsupported DCMV container in %s", fmv->path);
        dcfmv_close_unlocked(fmv);
        goto done;
    }

    ops = dcfmv_backend_ops(fmv);
    if (!ops || !ops->open || ops->open(fmv) != 0) {
        DCMV_Error("PANIC: Failed to open DCMV backend v%lu from %s",
                   (unsigned long)probed_version, fmv->path);
        dcfmv_close_unlocked(fmv);
        goto done;
    }

    result = 0;

done:
    mutex_unlock(&dcfmv_state_lock);
    return result;
}

static void dcfmv_close_unlocked(dcfmv_t *fmv) {
    const dcfmv_backend_ops_t *ops;

    if (!fmv) return;
    ops = dcfmv_backend_ops(fmv);
    dcfmv_audio_stop(fmv);
    if (fmv->video_fd >= 0) {
        fs_close(fmv->video_fd);
        fmv->video_fd = -1;
    }
    if (ops && ops->close)
        ops->close(fmv);
    dcfmv_free_buffers(fmv);
    dcfmv_reset_media_info(fmv);
}

void dcfmv_close(dcfmv_t *fmv) {
    if (!fmv) return;
    mutex_lock(&dcfmv_state_lock);
    dcfmv_close_unlocked(fmv);
    mutex_unlock(&dcfmv_state_lock);
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
        DCMV_LOG(DCFMV_LOG_FMV, "[FMV] paused -> %d", paused ? 1 : 0);
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
        DCMV_LOG(DCFMV_LOG_FMV, "[FMV] audio clock mode -> %s", use_audio_clock ? "audio" : "fps");
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
        DCMV_LOG(DCFMV_LOG_TIMING, "[FMV] settle fps-clock: frame=%d remaining=%d paused=%d",
                  atomic_load(&fmv->frame_index), remaining, paused);
    }

    dcfmv_reanchor_clock_to_current_frame(fmv);

    if (remaining <= 0 && !paused && fmv->use_audio_clock) {
        dcfmv_audio_start_stream(fmv);
        fmv->audio_unmute_pending = 1;
    }

    return 1;
}

int dcfmv_load_frame(dcfmv_t *fmv, int total_frame, int buf_index) {
    return dcfmv_decode_frame_backend(fmv, total_frame, buf_index);
}

bool dcfmv_schedule_frame_preload(dcfmv_t *fmv, int frame) {
    if (!fmv || frame < 0 || frame >= fmv->num_total_frames) return false;
    int unique_frame = dcfmv_total_to_unique_frame(fmv, frame);
    int buf = unique_frame % DCFMV_NUM_BUFFERS;

    if (atomic_load(&fmv->buf_state[buf]) != DCFMV_BUF_EMPTY) return false;

    int head = atomic_load(&fmv->preload_ring_head);
    int tail = atomic_load(&fmv->preload_ring_tail);
    int next_head = (head + 1) % DCFMV_RING_CAPACITY;
    if (next_head == tail) return false;

    for (int i = tail; i != head; i = (i + 1) % DCFMV_RING_CAPACITY) {
        int queued_frame = fmv->preload_ring[i].frame;
        if (queued_frame < 0 || queued_frame >= fmv->num_total_frames)
            continue;
        int queued_unique = dcfmv_total_to_unique_frame(fmv, queued_frame);
        if (queued_unique == unique_frame) return false;
    }

    fmv->preload_ring[head].frame = frame;
    fmv->preload_ring[head].generation = atomic_load(&fmv->GSeekGeneration);
    atomic_store(&fmv->preload_ring_head, next_head);
    return true;
}

bool dcfmv_schedule_frame_preload_with_generation(dcfmv_t *fmv, int frame, int generation) {
    if (!fmv || frame < 0 || frame >= fmv->num_total_frames) return false;
    int unique_frame = dcfmv_total_to_unique_frame(fmv, frame);
    int buf = unique_frame % DCFMV_NUM_BUFFERS;

    if (atomic_load(&fmv->buf_state[buf]) != DCFMV_BUF_EMPTY) return false;

    int head = atomic_load(&fmv->preload_ring_head);
    int tail = atomic_load(&fmv->preload_ring_tail);
    int next_head = (head + 1) % DCFMV_RING_CAPACITY;
    if (next_head == tail) return false;

    for (int i = tail; i != head; i = (i + 1) % DCFMV_RING_CAPACITY) {
        int queued_frame = fmv->preload_ring[i].frame;
        if (queued_frame < 0 || queued_frame >= fmv->num_total_frames)
            continue;
        int queued_unique = dcfmv_total_to_unique_frame(fmv, queued_frame);
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
        if (fmv->audio_logged_poll_generation != fmv->audio_start_generation) {
            DCMV_LOG(DCFMV_LOG_AUDIO,
                     "[Audio] first poll after start gen=%u frame=%d t=%.2f muted=%d started=%d",
                     fmv->audio_start_generation,
                     atomic_load(&fmv->frame_index),
                     dcfmv_ps_ms(),
                     atomic_load(&fmv->audio_muted),
                     fmv->audio_started);
            fmv->audio_logged_poll_generation = fmv->audio_start_generation;
        }
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
        spu_memset_sq(l, 0, req);
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
            spu_memset_sq(l, 0, req);
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
            spu_memset_sq(l, 0, half);
            lbytes = half;
            fmv->last_audio_left_pos += lbytes;
        }

        if (atomic_load(&fmv->g_audio_right_on)) {
            mutex_lock(&dcfmv_io_lock);
            rbytes = fs_read(fmv->audio_fd_right, (void *)r, half);
            mutex_unlock(&dcfmv_io_lock);
            fmv->last_audio_right_pos += rbytes;
        } else {
            spu_memset_sq(r, 0, half);
            rbytes = half;
            fmv->last_audio_right_pos += rbytes;
        }

        total_bytes = lbytes + rbytes;
    }

    return total_bytes;
}

static size_t dcfmv_chunk_audio_cb(snd_stream_hnd_t hnd, uintptr_t l,
                                    uintptr_t r, size_t req) {
    (void)hnd;
    dcfmv_t *fmv = dcfmv_current;
    if (!fmv || fmv->audio_channels <= 0) return 0;

    size_t per_chan = (fmv->audio_channels == 2)
        ? ((req / 2) & ~31u)
        : (req & ~31u);
    if (!per_chan) return 0;

    if (fmv->audio_logged_cb_generation != fmv->audio_start_generation) {
        DCMV_LOG(DCFMV_LOG_AUDIO,
                 "[Audio] first chunk cb after start gen=%u frame=%d t=%.2f req=%lu per_chan=%lu muted=%d started=%d",
                 fmv->audio_start_generation,
                 atomic_load(&fmv->frame_index),
                 dcfmv_ps_ms(),
                 (unsigned long)req,
                 (unsigned long)per_chan,
                 atomic_load(&fmv->audio_muted),
                 fmv->audio_started);
        fmv->audio_logged_cb_generation = fmv->audio_start_generation;
    }
    if (atomic_load(&fmv->audio_muted)) {
        spu_memset_sq(l, 0, per_chan);
        if (fmv->audio_channels == 2) spu_memset_sq(r, 0, per_chan);
        return fmv->audio_channels == 2 ? per_chan * 2 : per_chan;
    }

    size_t pos    = fmv->chunk_audio_ring_read_pos;
    size_t remain = per_chan;
    size_t copied = 0;

    while (remain) {
        int ri = atomic_load(&fmv->chunk_audio_read_idx);

        if (!__atomic_load_n(&fmv->chunk_audio_ring[ri].valid, __ATOMIC_ACQUIRE)) {
            atomic_store(&fmv->chunk_audio_refill_needed, 1);
            DCMV_LOG(DCFMV_LOG_CHUNK_AUDIO,
                     "[ChunkAudio] underrun ri=%d pos=%lu remain=%lu req=%lu chunk=%d",
                     ri,
                     (unsigned long)pos,
                     (unsigned long)remain,
                     (unsigned long)per_chan,
                     fmv->current_audio_chunk);
            spu_memset_sq(l + copied, 0, remain);
            if (fmv->audio_channels == 2)
                spu_memset_sq(r + copied, 0, remain);
            copied += remain;
            remain = 0;
            break;
        }

        size_t valid = fmv->chunk_audio_ring[ri].valid_bytes;
        if (pos >= valid) {
            atomic_store(&fmv->chunk_audio_ring[ri].valid, 0);
            atomic_store(&fmv->chunk_audio_read_idx,
                         (ri + 1) % DCFMV_AUDIO_RING_SLOTS);
            DCMV_LOG(DCFMV_LOG_CHUNK_AUDIO,
                     "[ChunkAudio] consume slot=%d valid=%lu -> next=%d",
                     ri,
                     (unsigned long)valid,
                     (ri + 1) % DCFMV_AUDIO_RING_SLOTS);
            pos = 0;
            continue;
        }

        size_t to_copy = ((valid - pos) < remain ? (valid - pos) : remain) & ~31u;
        if (to_copy < 32) {
            DCMV_LOG(DCFMV_LOG_CHUNK_AUDIO,
                     "[ChunkAudio] callback short ri=%d pos=%lu valid=%lu remain=%lu",
                     ri,
                     (unsigned long)pos,
                     (unsigned long)valid,
                     (unsigned long)remain);
            spu_memset_sq(l + copied, 0, remain);
            if (fmv->audio_channels == 2)
                spu_memset_sq(r + copied, 0, remain);
            copied += remain;
            remain = 0;
            break;
        }

        spu_memload(l + copied, fmv->chunk_audio_ring[ri].left  + pos, to_copy);
        if (fmv->audio_channels == 2)
            spu_memload(r + copied, fmv->chunk_audio_ring[ri].right + pos, to_copy);

        pos    += to_copy;
        copied += to_copy;
        remain -= to_copy;

        if (pos >= valid) {
            atomic_store(&fmv->chunk_audio_ring[ri].valid, 0);
            atomic_store(&fmv->chunk_audio_read_idx,
                         (ri + 1) % DCFMV_AUDIO_RING_SLOTS);
            DCMV_LOG(DCFMV_LOG_CHUNK_AUDIO,
                     "[ChunkAudio] consume slot=%d valid=%lu -> next=%d",
                     ri,
                     (unsigned long)valid,
                     (ri + 1) % DCFMV_AUDIO_RING_SLOTS);
            pos = 0;
        }
    }

    fmv->chunk_audio_ring_read_pos = pos;
    return fmv->audio_channels == 2 ? copied * 2 : copied;
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
    if (fmv->audio_channels <= 0) return 0;

    if (fmv->backend_kind == DCFMV_BACKEND_CHUNKS) {
        /* Chunk backend — use ring buffer callback, no fd's needed */
        snd_stream_init_ex(fmv->audio_channels, fmv->soundbufferalloc);
        fmv->stream = snd_stream_alloc(NULL, fmv->soundbufferalloc);
        if (fmv->stream == SND_STREAM_INVALID) return -1;
        snd_stream_set_callback_direct(fmv->stream, dcfmv_chunk_audio_cb);
        dcfmv_chunk_refill_audio_ring(fmv);  /* prime before starting */
        mutex_lock(&dcfmv_audio_lock);
        snd_stream_start_adpcm(fmv->stream, fmv->sample_rate,
                               fmv->audio_channels == 2 ? 1 : 0);
        mutex_unlock(&dcfmv_audio_lock);
        fmv->audio_started = 1;
        dcfmv_set_audio_muted(fmv, 1);
        return 0;
    }

    /* Frames backend — original fd-based path unchanged */
    fmv->audio_fd_left = fs_open(fmv->path, O_RDONLY);
    if (fmv->audio_fd_left < 0) return -1;
    fs_seek(fmv->audio_fd_left, fmv->audio_offset, SEEK_SET);
    fmv->last_audio_left_pos = fmv->audio_offset;

    if (fmv->audio_channels == 2) {
        fmv->audio_fd_right = fs_open(fmv->path, O_RDONLY);
        if (fmv->audio_fd_right < 0) {
            fs_close(fmv->audio_fd_left);
            fmv->audio_fd_left = -1;
            return -1;
        }
        fs_seek(fmv->audio_fd_right,
                fmv->audio_offset + fmv->left_channel_size, SEEK_SET);
        fmv->last_audio_right_pos = fmv->audio_offset + fmv->left_channel_size;
    }

    snd_stream_init_ex(fmv->audio_channels, fmv->soundbufferalloc);
    fmv->stream = snd_stream_alloc(NULL, fmv->soundbufferalloc);
    if (fmv->stream == SND_STREAM_INVALID) return -1;
    snd_stream_set_callback_direct(fmv->stream, dcfmv_audio_cb);
    mutex_lock(&dcfmv_audio_lock);
    snd_stream_start_adpcm(fmv->stream, fmv->sample_rate,
                           fmv->audio_channels == 2 ? 1 : 0);
    mutex_unlock(&dcfmv_audio_lock);
    fmv->audio_started = 1;
    dcfmv_set_audio_muted(fmv, 1);
    return 0;
}

static int dcfmv_chunk_audio_recreate_stream(dcfmv_t *fmv) {
    if (!fmv || fmv->backend_kind != DCFMV_BACKEND_CHUNKS || fmv->audio_channels <= 0)
        return 0;

    if (fmv->stream != SND_STREAM_INVALID) {
        mutex_lock(&dcfmv_audio_lock);
        snd_stream_destroy(fmv->stream);
        mutex_unlock(&dcfmv_audio_lock);
        fmv->stream = SND_STREAM_INVALID;
        fmv->audio_started = 0;
    }

    snd_stream_init_ex(fmv->audio_channels, fmv->soundbufferalloc);
    fmv->stream = snd_stream_alloc(NULL, fmv->soundbufferalloc);
    if (fmv->stream == SND_STREAM_INVALID)
        return -1;

    snd_stream_set_callback_direct(fmv->stream, dcfmv_chunk_audio_cb);
    fmv->audio_started = 0;
    fmv->audio_logged_poll_generation = 0;
    fmv->audio_logged_cb_generation = 0;
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
    fmv->audio_start_generation++;
    fmv->audio_logged_start_generation = fmv->audio_start_generation;
    fmv->audio_logged_poll_generation = 0;
    fmv->audio_logged_cb_generation = 0;
    dcfmv_audio_apply_volume(fmv);
    DCMV_LOG(DCFMV_LOG_AUDIO,
             "[Audio] start_adpcm gen=%u frame=%d t=%.2f muted=%d started=%d",
             fmv->audio_start_generation,
             atomic_load(&fmv->frame_index),
             dcfmv_ps_ms(),
             atomic_load(&fmv->audio_muted),
             fmv->audio_started);
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

const dcfmv_media_info_t *dcfmv_media_info(const dcfmv_t *fmv) {
    return fmv ? &fmv->media_info : NULL;
}

enum dcfmv_backend_kind dcfmv_backend(const dcfmv_t *fmv) {
    return fmv ? fmv->backend_kind : DCFMV_BACKEND_FRAMES;
}

int dcfmv_frame_index(dcfmv_t *fmv) {
    if (!fmv) return 0;
    return atomic_load(&fmv->frame_index);
}

int dcfmv_total_to_unique(const dcfmv_t *fmv, int total_frame) {
    if (!fmv || !fmv->GTotalToUnique) return 0;
    if ((unsigned)total_frame >= (unsigned)fmv->num_total_frames)
        return fmv->num_unique_frames > 0 ? (fmv->num_unique_frames - 1) : 0;
    return fmv->GTotalToUnique[total_frame];
}

int dcfmv_ms_to_total_frame_floor(const dcfmv_t *fmv, uint32_t ms) {
    uint64_t num;
    uint64_t den;
    int f;

    if (!fmv || fmv->fps_den == 0 || fmv->num_total_frames <= 0)
        return 0;

    num = (uint64_t)ms * (uint64_t)fmv->fps_num;
    den = (uint64_t)fmv->fps_den * 1000ULL;
    f = (int)(num / den);
    if (f < 0) f = 0;
    if (f >= fmv->num_total_frames) f = fmv->num_total_frames - 1;
    return f;
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

int dcfmv_audio_channels(const dcfmv_t *fmv) {
    if (!fmv) return 0;
    return fmv->audio_channels;
}

int dcfmv_audio_muted(const dcfmv_t *fmv) {
    if (!fmv) return 1;
    return atomic_load(&fmv->audio_muted);
}

int dcfmv_audio_volume(const dcfmv_t *fmv) {
    if (!fmv) return 0;
    return atomic_load(&fmv->g_audio_movie_vol);
}

uint32_t dcfmv_audio_offset(const dcfmv_t *fmv) {
    if (!fmv) return 0;
    return (uint32_t)fmv->audio_offset;
}

void dcfmv_set_audio_enabled(dcfmv_t *fmv, int enabled) {
    if (!fmv) return;
    if (!enabled) {
        fmv->audio_channels = 0;
        fmv->media_info.channels = 0;
    }
}

void dcfmv_set_audio_channel_enabled(dcfmv_t *fmv, int channel, int enabled) {
    if (!fmv) return;
    switch (channel) {
        case 1:
            atomic_store(&fmv->g_audio_left_on, enabled ? 1 : 0);
            break;
        case 2:
            atomic_store(&fmv->g_audio_right_on, enabled ? 1 : 0);
            break;
        default:
            break;
    }
}

int dcfmv_audio_channel_enabled(const dcfmv_t *fmv, int channel) {
    if (!fmv) return 0;
    switch (channel) {
        case 1:
            return atomic_load(&fmv->g_audio_left_on);
        case 2:
            return atomic_load(&fmv->g_audio_right_on);
        default:
            return 0;
    }
}

int dcfmv_seek_active(const dcfmv_t *fmv) {
    if (!fmv) return 0;
    return atomic_load(&fmv->seek_request) >= 0 || atomic_load(&fmv->seek_in_progress);
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
            DCMV_LOG(DCFMV_LOG_TIMING, "[FMV] seek request fps-clock: frame=%d paused=%d settle=%d",
                      req, fmv->g_is_paused, atomic_load(&fmv->seek_settle_frames));
        }
        dcfmv_seek_to_frame(fmv, req);
        atomic_store(&fmv->frame_index, req);
        atomic_store(&fmv->seek_in_progress, 0);

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
            DCMV_LOG(DCFMV_LOG_TIMING, "[FMV] fps-clock tick: frame=%d elapsed=%.2fms settle=%d paused=%d",
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
                DCMV_LOG(DCFMV_LOG_TIMING, "[FMV] fps-clock paused redraw: frame=%d settle=%d",
                          paused_frame, atomic_load(&fmv->seek_settle_frames));
                last_noaudio_pause_frame = paused_frame;
            }
        }
        fmv->frame_timer_anchor = dcfmv_ps_ms();
        return 0.0;
    }

    int frames_to_skip = 0;
    (void)frames_to_skip;

    int expected_frame = (int)(current_playback_time_ms / fmv->frame_duration);
    if (expected_frame < 0)
        expected_frame = 0;

    if (current_playback_time_ms >= target_time_ms) {
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
    int window = (DCFMV_NUM_BUFFERS / 2 < 8) ? (DCFMV_NUM_BUFFERS / 2) : 8;
    if (fmv->chunk_index_data && fmv->chunk_count) {
        int chunk_window = dcfmv_chunk_video_preload_limit(fmv, cur_frame);
        if (chunk_window > 0 && chunk_window < window)
            window = chunk_window;
    }
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
// DCSinge owns pacing/vblank now. Do not sleep inside dcfmv_tick().
#if 0
    double wait_ms = target_time_ms - current_playback_time_ms;
    if (wait_ms > 1.0) {
        if (wait_ms > fmv->frame_duration) wait_ms = fmv->frame_duration;
        thd_sleep((int)wait_ms);
    } else if (wait_ms > 0.0) {
        thd_pass();
    }
#endif
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

    DCMV_LOG(DCFMV_LOG_SEEK, "[Seek] >>> Begin seek_to_frame(%d)", new_frame);
    dcfmv_log_state("seek(begin)", fmv);

    for (int i = 0; i < DCFMV_NUM_BUFFERS; i++) {
        atomic_store(&fmv->buf_state[i], DCFMV_BUF_EMPTY);
    }

    atomic_store(&fmv->preload_ring_head, 0);
    atomic_store(&fmv->preload_ring_tail, 0);
    memset(fmv->preload_ring, 0, sizeof(fmv->preload_ring));

    fmv->last_unique_frame_drawn = -1;
    atomic_store(&fmv->seek_request, -1);

    if (dcfmv_seek_video_backend(fmv, new_frame) != 0) {
        DCMV_Error("[Seek] Failed to seek backend video to frame %d", new_frame);
        return;
    }

    if (dcfmv_seek_audio_backend(fmv, new_frame) != 0) {
        DCMV_Error("[Seek] Failed to seek backend audio to frame %d", new_frame);
        return;
    }

    if (fmv->backend_kind == DCFMV_BACKEND_CHUNKS &&
        fmv->audio_channels > 0 &&
        dcfmv_chunk_audio_recreate_stream(fmv) != 0) {
        DCMV_Error("[Seek] Failed to recreate chunk audio stream for frame %d", new_frame);
        return;
    }

    if (fmv->backend_kind == DCFMV_BACKEND_CHUNKS &&
        fmv->audio_channels > 0 &&
        !fmv->g_is_paused) {
        /*
         * Chunk ADPCM startup still needs a short warmup after seek. Keep this
         * small so we absorb the startup gap without reintroducing the old
         * multi-second audio hold.
         */
        if (atomic_load(&fmv->seek_settle_frames) < 7) {
            dcfmv_set_seek_settle_frames(fmv, 7);
        }
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
        DCMV_LOG(DCFMV_LOG_TIMING, "[FMV] seek fps-clock anchor: frame=%d anchor=%.2f",
                 new_frame, fmv->frame_timer_anchor);
    }

    DCMV_LOG(DCFMV_LOG_SEEK, "[Seek] anchor=%.2f base=%.2f (frame=%d, fps=%.2f, frame_dur=%.2fms)",
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
        fmv->preload_ring[i].generation = -1;
    }

    DCMV_LOG(DCFMV_LOG_SEEK, "[Seek] Incremented GSeekGeneration -> %d (flushed ring)", cur_gen);

    /*
     * Prime the exact target frame synchronously.
     */
    int first_unique = dcfmv_total_to_unique_frame(fmv, new_frame);
    int first_buf = first_unique % DCFMV_NUM_BUFFERS;

    if (atomic_load(&fmv->buf_state[first_buf]) == DCFMV_BUF_EMPTY) {
        atomic_store(&fmv->buf_state[first_buf], DCFMV_BUF_LOADING);

        if (dcfmv_load_frame(fmv, new_frame, first_buf) == 0) {
            atomic_store(&fmv->buf_state[first_buf], DCFMV_BUF_READY);
            DCMV_LOG(DCFMV_LOG_SEEK, "[Seek] Primed initial frame %d (unique=%d buf=%d)",
                     new_frame, first_unique, first_buf);
        } else {
            atomic_store(&fmv->buf_state[first_buf], DCFMV_BUF_EMPTY);
            DCMV_LOG(DCFMV_LOG_SEEK, "[Seek] Failed to prime initial frame %d (unique=%d buf=%d)",
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
    if (fmv->chunk_index_data && fmv->chunk_count) {
        int chunk_window = dcfmv_chunk_video_preload_limit(fmv, new_frame);
        if (chunk_window > 0 && chunk_window < max_preloads)
            max_preloads = chunk_window;
    }

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

    DCMV_LOG(DCFMV_LOG_SEEK, "[Seek] <<< Completed seek_to_frame(%d)", new_frame);
    dcfmv_log_state("seek(end)", fmv);
}

void dcfmv_worker_step(dcfmv_t *fmv) {
    if (!fmv) return;

    mutex_lock(&dcfmv_state_lock);
    if (atomic_load(&fmv->preload_paused)) {
        mutex_unlock(&dcfmv_state_lock);
        thd_sleep(2);
        return;
    }

    dcfmv_audio_poll(fmv);

    /* Keep the active chunk pair resident so audio refill can avoid misses. */
    if (fmv->backend_kind == DCFMV_BACKEND_CHUNKS && fmv->audio_channels > 0) {
        if ((uint32_t)fmv->current_audio_chunk < fmv->chunk_count)
            (void)dcfmv_chunk_load_sync(fmv, fmv->current_audio_chunk);
        if ((uint32_t)(fmv->current_audio_chunk + 1) < fmv->chunk_count)
            (void)dcfmv_chunk_load_sync(fmv, fmv->current_audio_chunk + 1);
    }

    /* Chunk audio refill */
    if (fmv->backend_kind == DCFMV_BACKEND_CHUNKS &&
        fmv->audio_channels > 0 &&
        !atomic_load(&fmv->audio_muted)) {
        int wi = atomic_load(&fmv->chunk_audio_write_idx);
        int ri = atomic_load(&fmv->chunk_audio_read_idx);
        int fill = (wi - ri + DCFMV_AUDIO_RING_SLOTS) % DCFMV_AUDIO_RING_SLOTS;
        if (fill < (DCFMV_AUDIO_RING_SLOTS / 2) ||
            atomic_load(&fmv->chunk_audio_refill_needed)) {
            atomic_store(&fmv->chunk_audio_refill_needed, 0);
            dcfmv_chunk_refill_audio_ring(fmv);
        }
    }
    int tail = atomic_load(&fmv->preload_ring_tail);
    int head = atomic_load(&fmv->preload_ring_head);
    int cur_gen = (int)atomic_load(&fmv->GSeekGeneration);
    if (tail != head) {
        PreloadJob job = fmv->preload_ring[tail];
        atomic_store(&fmv->preload_ring_tail, (tail + 1) % DCFMV_RING_CAPACITY);

        if (job.generation == cur_gen) {
            int total_frame = job.frame;
            if (total_frame < 0 || total_frame >= fmv->num_total_frames) {
                fmv->worker_idle_ticks = 0;
                goto done;
            }
            int unique_frame = dcfmv_total_to_unique_frame(fmv, total_frame);
            int buf = unique_frame % DCFMV_NUM_BUFFERS;

            int expected = DCFMV_BUF_EMPTY;
            if (atomic_compare_exchange_strong(&fmv->buf_state[buf], &expected, DCFMV_BUF_LOADING)) {
                int res = dcfmv_load_frame(fmv, total_frame, buf);
                if (res != 0) {
                    atomic_store(&fmv->buf_state[buf], DCFMV_BUF_EMPTY);
                    DCMV_LOG(DCFMV_LOG_WORKER, "[Worker] load_frame failed for %d (unique=%d buf=%d)",
                           total_frame, unique_frame, buf);
                }
            }

            fmv->worker_idle_ticks = 0;
        }
    }

    //     /* After video preload scheduling, also ensure next audio chunks are cached */
    // if (fmv->backend_kind == DCFMV_BACKEND_CHUNKS && fmv->audio_channels > 0) {
    //     if ((uint32_t)fmv->current_audio_chunk < fmv->chunk_count)
    //         dcfmv_chunk_load_sync(fmv, fmv->current_audio_chunk);
    //     if ((uint32_t)(fmv->current_audio_chunk + 1) < fmv->chunk_count)
    //         dcfmv_chunk_load_sync(fmv, fmv->current_audio_chunk + 1);
    // }

    int current = atomic_load(&fmv->frame_index);
    int max_preloads = (DCFMV_NUM_BUFFERS < 16) ? DCFMV_NUM_BUFFERS : 16;
    if (fmv->chunk_index_data && fmv->chunk_count) {
        int chunk_window = dcfmv_chunk_video_preload_limit(fmv, current);
        if (chunk_window > 0 && chunk_window < max_preloads)
            max_preloads = chunk_window;
    }
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
            DCMV_LOG(DCFMV_LOG_WORKER, "[Worker] Idle/stalled (cur=%d gen=%d). Re-seeding preload window.", cur, cur_gen);
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

done:
    mutex_unlock(&dcfmv_state_lock);
    thd_sleep(1);
}

void dcfmv_upload_current_video(dcfmv_t *fmv) {
    if (!fmv) return;

    int cur_total = atomic_load(&fmv->frame_index);
    int unique = dcfmv_total_to_unique_frame(fmv, cur_total);
    int buf = unique % DCFMV_NUM_BUFFERS;
    int state = atomic_load(&fmv->buf_state[buf]);

    atomic_store(&fmv->displayed_total_frame, cur_total);

    if (unique != fmv->last_unique_frame_drawn && state == DCFMV_BUF_READY) {
        dcache_flush_range((uint32)fmv->frame_buffer[buf], fmv->video_frame_size);
        pvr_txr_load_dma(fmv->frame_buffer[buf], fmv->pvr_txr, fmv->video_frame_size, 1, NULL, 0);
        fmv->last_unique_frame_drawn = unique;
    }
}

void dcfmv_submit_current_video(dcfmv_t *fmv) {
    if (!fmv) return;

    int cur_total = atomic_load(&fmv->frame_index);
    int unique = dcfmv_total_to_unique_frame(fmv, cur_total);
    int buf = unique % DCFMV_NUM_BUFFERS;
    int state = atomic_load(&fmv->buf_state[buf]);
    static int last_render_logged = -1;

    if (unique != last_render_logged) {
        DCMV_LOG(DCFMV_LOG_RENDER, "[Render] frame=%d unique=%d buf=%d state=%d last=%d",
                  cur_total, unique, buf, state, fmv->last_unique_frame_drawn);
        last_render_logged = unique;
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

void dcfmv_render_current_video(dcfmv_t *fmv) {
    if (fmv && fmv->present_mode == DCFMV_PRESENT_OWNED) {
        pvr_wait_ready();
        pvr_wait_render_done();
    }

    dcfmv_upload_current_video(fmv);
    dcfmv_submit_current_video(fmv);
}
