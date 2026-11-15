/*
 * ounsu!'s native glue between osu! and the lx island runtime (DirectPInvoke "ounsu"):
 *
 * - ounsu_vi_window (osu-framework's OunsuNative.ViWindow): the console's window, from the island runtime.
 * - ounsu_display (OunsuNative.Display): the window's size and frame rate now; ounsu_display_set (the "Switch 2"
 *   settings' Display section): the sizes per console mode and the frame rate.
 * - ounsu_log: osu-game's log lines.
 * - The other "Switch 2" settings' setters live with what they set: lx_input.c, lx_audio_out.c, lx_gc.c.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "lx_abi.h"

void *lx_vi_window(void); /* island runtime */
int lx_display_set(uint32_t handheld_height, uint32_t docked_height, uint32_t fps);
void lx_display_get(struct lx_display *out);

void *ounsu_vi_window(void);
void ounsu_display(uint32_t *out);
int ounsu_display_set(int handheld_height, int docked_height, int fps);
void ounsu_log(const char *text);

void *ounsu_vi_window(void) {
    return lx_vi_window();
}

/* out: width, height, docked, frame rate, serial. Called once per frame by the window thread, which also polls input:
   the host handles the console's applet messages (docking) in here. */
void ounsu_display(uint32_t *out) {
    struct lx_display d;
    lx_display_get(&d);
    out[0] = d.width;
    out[1] = d.height;
    out[2] = d.docked;
    out[3] = d.frame_rate;
    out[4] = d.serial;
}

int ounsu_display_set(int handheld_height, int docked_height, int fps) {
    return lx_display_set((uint32_t)handheld_height, (uint32_t)docked_height, (uint32_t)fps);
}

void ounsu_log(const char *text) {
    size_t n = strlen(text);
    char buf[512];
    if (n > sizeof buf - 1) n = sizeof buf - 1;
    memcpy(buf, text, n);
    buf[n++] = '\n';
    (void)!write(2, buf, n);
}
