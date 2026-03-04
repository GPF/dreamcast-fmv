// fmv_play.c - minimal demo player using dcfmv
// Public Domain / MIT — Troy Davis (GPF)

#include <kos.h>
#include <kos/dbgio.h>

#include <dc/pvr.h>
#include <dc/maple/controller.h>

#include <stdio.h>
#include <fcntl.h>

#include "dcfmv.h"

static void poll_exit(void) {
    maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    if (!dev) return;
    cont_state_t *st = (cont_state_t *)maple_dev_status(dev);
    if (!st) return;
    static int held = 0;
    if (st->buttons & CONT_START) held = 1;
    else if (held) { printf("🛑 exit\n"); arch_exit(); }
}

// static int paused = 0;
static void poll_seek_and_pause(dcfmv_t *mv) {
    maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    if (!dev) return;
    cont_state_t *st = (cont_state_t *)maple_dev_status(dev);
    if (!st) return;
    
    // Debounce: only fire on edge (press), not while held
    static uint32_t prev_buttons = 0;
    uint32_t b = st->buttons;
    uint32_t pressed = (b ^ prev_buttons) & b; // 0->1 transitions
    prev_buttons = b;

    // Pause toggle on A
    if (pressed & CONT_A) {
        dcfmv_toggle_pause(mv);
    }

    // Seek on DPAD left/right (only when not paused is usually nicer,
    // but you can remove this guard if you want seeking while paused)
    if (dcfmv_is_paused(mv)) return;

    const DCMVHeader *h = dcfmv_header(mv);
    if (!h) return;

    int cur = dcfmv_frame_index(mv);
    int target = cur;

    if (pressed & CONT_DPAD_LEFT)  target = cur - 200;
    if (pressed & CONT_DPAD_RIGHT) target = cur + 200;

    if (target != cur) {
        if (target < 0) target = 0;
        if (target >= (int)h->num_total_frames) target = (int)h->num_total_frames - 1;

        printf("[seek] %d -> %d\n", cur, target);
        dcfmv_seek(mv, target);
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    const char *path = NULL;

    // Prefer /cd, fall back /pc
    file_t fd = fs_open("/cd/movie.dcmv", O_RDONLY);
    if (fd >= 0) { fs_close(fd); path = "/cd/movie.dcmv"; }
    else {
        fd = fs_open("/pc/movie.dcmv", O_RDONLY);
        if (fd >= 0) { fs_close(fd); path = "/pc/movie.dcmv"; }
    }

    if (!path) {
        printf("❌ movie.dcmv not found on /cd or /pc\n");
        return -1;
    }

    printf("🎬 dcfmv demo: %s\n", path);

    // If playing from /cd, set video mode (your preference)
    if (strncmp(path, "/pc/", 4) != 0)
        vid_set_mode(DM_640x480, PM_RGB565);
    else
        printf("[boot] /pc: skipping vid_set_mode\n");

    // Init PVR once (DCSinge will already do this)
    pvr_init_defaults();

    dcfmv_t *mv = dcfmv_create(NULL);
    if (!mv) { printf("❌ dcfmv_create\n"); return -1; }

    if (dcfmv_open(mv, path) < 0) {
        printf("❌ dcfmv_open failed\n");
        dcfmv_destroy(mv);      
        return -1;
    }

    printf("✅ Starting playback\n");

    while (dcfmv_frame_index(mv) < (int)dcfmv_header(mv)->num_total_frames -1) {
        // Input FIRST (so seek happens before tick schedules work for the old frame)
        poll_seek_and_pause(mv);
        poll_exit();

        // Tick: schedules decode/audio and returns a recommended deadline
        double deadline = dcfmv_tick(mv);

        // Render
        pvr_scene_begin();
        pvr_list_begin(PVR_LIST_OP_POLY);

        (void)dcfmv_submit(mv);

        pvr_list_finish();
        pvr_scene_finish();

        // Wait/poll (this should keep snd_stream_poll pumping)
        dcfmv_wait_until(mv, deadline);
    }

    printf("🏁 Done\n");
    dcfmv_destroy(mv);
    arch_exit();
    return 0;
}