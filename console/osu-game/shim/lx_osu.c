/*
 * osu-game's glue to the lx island runtime: the symbols that HBLauncher's runtime used to provide and that the
 * HBLauncher-built SDL3, osu-framework's patches and console/osu-game still look for.
 *
 * - SDL3's hbx audio and joystick drivers (patches/SDL.diff) read hbxl_ctx. It is NULL here, so both decline: SDL
 *   falls back to its other drivers (audio: SDL_AUDIO_DRIVER=dummy for now), and input will come from the host.
 * - hbx_switch2_vi_window (osu-framework's HbxlNative.ViWindow): the console's window, from the island runtime.
 * - hbx_switch2_display (HbxlNative.Display): the window's size and frame rate now; hbxl_display_set (the "Switch 2"
 *   settings' Display section): the sizes per console mode and the frame rate.
 * - hbxl_log and the diagnostics osu-game's Program.cs calls (thread dumps, stall beats): logging works, the
 *   thread-dump calls report nothing for now.
 * - The "Switch 2" settings' setters live with what they set: lx_input.c, lx_audio_out.c, lx_gc.c.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "lx_abi.h"

void *lx_vi_window(void); /* island runtime */
int lx_display_set(uint32_t handheld_height, uint32_t docked_height, uint32_t fps);
void lx_display_get(struct lx_display *out);

/* ---- SDL3's hbx drivers ---------------------------------------------------------------------------------------- */

const void *hbxl_ctx = NULL;

void *hbxl_sdk_memalign(size_t align, size_t size) {
    return aligned_alloc(align, (size + align - 1) & ~(align - 1));
}

void hbxl_sdk_free(void *p) {
    free(p);
}

void hbx_input_poll(const void *ctx, void *state) {
    (void)ctx;
    (void)state;
}

/* ---- osu-framework --------------------------------------------------------------------------------------------- */

void *hbx_switch2_vi_window(void) {
    return lx_vi_window();
}

/* out: width, height, docked, frame rate, serial. Called once per frame by the window thread, which also polls input:
   the host handles the console's applet messages (docking) in here. */
void hbx_switch2_display(uint32_t *out) {
    struct lx_display d;
    lx_display_get(&d);
    out[0] = d.width;
    out[1] = d.height;
    out[2] = d.docked;
    out[3] = d.frame_rate;
    out[4] = d.serial;
}

int hbxl_display_set(int handheld_height, int docked_height, int fps) {
    return lx_display_set((uint32_t)handheld_height, (uint32_t)docked_height, (uint32_t)fps);
}

/* ---- osu-game's Program.cs ------------------------------------------------------------------------------------- */

void hbxl_log(const char *text) {
    size_t n = strlen(text);
    char buf[512];
    if (n > sizeof buf - 1) n = sizeof buf - 1;
    memcpy(buf, text, n);
    buf[n++] = '\n';
    (void)!write(2, buf, n);
}

long hbxl_gettid(void) {
    return syscall(SYS_gettid);
}

int hbxl_thread_backtrace(int tid, uint64_t *out, int max) {
    (void)tid;
    (void)out;
    (void)max;
    return 0;
}

int hbxl_thread_state(int tid) {
    (void)tid;
    return 0;
}

void hbxl_clone_progress(int *out) {
    out[0] = out[1] = out[2] = 0;
}

int hbxl_dbg_tl_lock(void) {
    return 0;
}

int hbxl_dbg_malloc_lock(void) {
    return 0;
}

void hbxl_stall_beat(int which) {
    (void)which;
}
