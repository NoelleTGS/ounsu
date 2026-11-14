/*
 * The console's window per mode (ABI 7): its size handheld and docked, the frame rate, and the applet messages that
 * switch between the modes.
 *
 * libounce keeps one window size per console mode (nwindowSetModeDimensions; 0x0 is the console's default: 1920x1080
 * handheld, the TV output setting docked) and changes the window's size when appletMainLoop handles the
 * operation-mode message. Nothing else in the host handles applet messages, so display_get, which the island calls
 * once per frame, pumps them. A swapchain of the old size keeps presenting (scaled by the system) until the program
 * recreates it at the size display_get reports.
 *
 * Frame rate: nwindowSetFrameRates. 0 presents every refresh at the highest rate available (120 Hz on the handheld
 * screen, with VRR); OunceVK paces FIFO presents with nwindowGetPresentInterval.
 */
#include <switch.h>

#include "lx_host.h"

static Mutex g_lock;
static struct lx_display g_now;
static uint64_t g_last_pump;
static int g_exit_logged;

/* With g_lock held. */
static void refresh(void) {
    NWindow *nw = nwindowGetDefault();
    u32 w = 0, h = 0;
    if (R_FAILED(nwindowGetDimensions(nw, &w, &h)) || !w || !h) {
        w = 1920;
        h = 1080;
    }
    u16 num = 0, den = 0;
    uint32_t fps = 60;
    if (R_SUCCEEDED(nwindowGetPresentInterval(nw, &num, &den)) && num && den) fps = (den + num / 2) / num;
    uint32_t docked = appletGetOperationMode() == AppletOperationMode_Console;
    if (w != g_now.width || h != g_now.height || fps != g_now.frame_rate || docked != g_now.docked) {
        g_now.width = w;
        g_now.height = h;
        g_now.frame_rate = fps;
        g_now.docked = docked;
        g_now.serial++;
        lx_logf("lx-display: %s, window %ux%u, %u fps (interval %u/%u s)", docked ? "docked" : "handheld",
                (unsigned)w, (unsigned)h, (unsigned)fps, (unsigned)num, (unsigned)den);
    }
}

static uint32_t width_for(uint32_t height) {
    return (height * 16 / 9) & ~1u;
}

int lx_display_set(uint32_t handheld_height, uint32_t docked_height, uint32_t fps) {
    mutexLock(&g_lock);
    NWindow *nw = nwindowGetDefault();
    Result rc = nwindowSetModeDimensions(nw, width_for(handheld_height), handheld_height, width_for(docked_height),
                                         docked_height);
    /* (0, 0): every refresh at the highest rate. (fps, fps): at most fps at either refresh rate; a rate that divides
       60 asks for no 120 Hz. */
    Result frc = nwindowSetFrameRates(nw, fps, fps, true);
    lx_logf("lx-display: set handheld %u, docked %u (0 = console default), %u fps (0 = highest): 0x%x, 0x%x",
            (unsigned)handheld_height, (unsigned)docked_height, (unsigned)fps, rc, frc);
    refresh();
    mutexUnlock(&g_lock);
    return R_FAILED(rc) ? (int)rc : (int)frc;
}

void lx_display_get(struct lx_display *out) {
    mutexLock(&g_lock);
    uint64_t now = armGetSystemTick();
    if (now - g_last_pump >= armNsToTicks(4000000)) { /* the applet messages at most every 4 ms */
        g_last_pump = now;
        if (!appletMainLoop() && !g_exit_logged) {
            /* The system asked the program to close (HOME menu -> Close). Not handled yet: the loader's own
               exit path ends the process. */
            g_exit_logged = 1;
            lx_logf("lx-display: the system asked the program to exit");
        }
    }
    refresh();
    *out = g_now;
    mutexUnlock(&g_lock);
}

/* Before the island starts: the console's default size in each mode, the default frame rate (60), so a program that
   never calls display_set gets a full-screen window either way. */
void lx_display_init(void) {
    mutexInit(&g_lock);
    mutexLock(&g_lock);
    Result rc = nwindowSetModeDimensions(nwindowGetDefault(), 1920, 1080, 0, 0);
    lx_logf("lx-display: window per mode, handheld 1920x1080, docked the TV's (0x%x)", rc);
    refresh();
    mutexUnlock(&g_lock);
}
