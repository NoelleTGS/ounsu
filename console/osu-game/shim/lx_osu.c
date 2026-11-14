/*
 * osu-game's glue to the lx island runtime: the symbols that HBLauncher's runtime used to provide and that the
 * HBLauncher-built SDL3, osu-framework's patches and console/osu-game still look for.
 *
 * - SDL3's hbx audio and joystick drivers (patches/SDL.diff) read hbxl_ctx. It is NULL here, so both decline: SDL
 *   falls back to its other drivers (audio: SDL_AUDIO_DRIVER=dummy for now), and input will come from the host.
 * - hbx_switch2_vi_window (osu-framework's HbxlNative.ViWindow): the console's window, from the island runtime.
 * - hbxl_log and the diagnostics osu-game's Program.cs calls (thread dumps, stall beats): logging works, the
 *   thread-dump calls report nothing for now.
 * - The "Switch 2" settings' setters (input, audio queue, GC log): no-ops until input and audio are ported
 *   (hbxl_input.c / hbxl_audio_out.c, next steps).
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

void *lx_vi_window(void); /* island runtime */

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

/* ---- the "Switch 2" settings' setters (Switch2Settings.cs) ------------------------------------------------------ */

void hbxl_input_set_gyro(int button, float speed_x, float speed_y, int invert_x, int invert_y) {
    (void)button;
    (void)speed_x;
    (void)speed_y;
    (void)invert_x;
    (void)invert_y;
}

void hbxl_input_set_kbmouse(int enabled, float speed) {
    (void)enabled;
    (void)speed;
}

int hbxl_audio_set_queue(int period, int buffers) {
    (void)period;
    (void)buffers;
    return 0;
}

void hbxl_gc_log_set(int min_pause_ms) {
    (void)min_pause_ms;
}
