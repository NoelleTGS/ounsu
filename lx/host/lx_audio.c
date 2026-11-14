/*
 * Audio output for the island (lx_abi.h, version 5): libnx audout, 48 kHz stereo s16, a small queue of host buffers
 * the island fills one period at a time (osu!'s low-latency output, console/osu-game/shim/lx_audio_out.c).
 *
 * audout's buffers must be 0x1000-aligned and their size a multiple of 0x1000; the data in one can be shorter.
 */
#include <switch.h>

#include <malloc.h>
#include <string.h>

#include "lx_host.h"

#define MAX_BUFFERS 16
#define FRAME_BYTES 4

static AudioOutBuffer g_bufs[MAX_BUFFERS];
static int g_busy[MAX_BUFFERS];
static int g_count, g_queued, g_open;
static uint32_t g_bytes, g_cap;

static void reclaim(void) {
    for (int guard = 0; guard <= g_count; guard++) {
        AudioOutBuffer *released = NULL;
        u32 n = 0;
        if (R_FAILED(audoutGetReleasedAudioOutBuffer(&released, &n)) || n == 0 || !released) return;
        for (int i = 0; i < g_count; i++) {
            if (released == &g_bufs[i] && g_busy[i]) {
                g_busy[i] = 0;
                g_queued--;
                break;
            }
        }
    }
}

void lx_audio_close(void) {
    if (!g_open) return;
    audoutStopAudioOut();
    bool flushed;
    audoutFlushAudioOutBuffers(&flushed);
    for (int tries = 0; tries < 40 && g_queued > 0; tries++) {
        reclaim();
        if (g_queued > 0) svcSleepThread(1000000);
    }
    audoutExit();
    for (int i = 0; i < g_count; i++) {
        free(g_bufs[i].buffer);
        g_bufs[i].buffer = NULL;
    }
    memset(g_busy, 0, sizeof g_busy);
    g_count = g_queued = 0;
    g_open = 0;
    lx_logf("lx-audio: output closed");
}

int lx_audio_open(uint32_t period, uint32_t buffers) {
    if (g_open) lx_audio_close();
    if (period < 16 || buffers < 1 || buffers > MAX_BUFFERS) return -LX_EINVAL;
    g_bytes = period * FRAME_BYTES;
    g_cap = (g_bytes + 0xFFF) & ~0xFFFu;

    Result rc = audoutInitialize();
    if (R_FAILED(rc)) {
        lx_logf("lx-audio: audoutInitialize: 0x%x", rc);
        return -LX_EIO;
    }
    if (audoutGetSampleRate() != 48000 || audoutGetChannelCount() != 2 || audoutGetPcmFormat() != PcmFormat_Int16) {
        lx_logf("lx-audio: audout is %u Hz, %u channels, format %d: not 48 kHz stereo s16", audoutGetSampleRate(),
                audoutGetChannelCount(), (int)audoutGetPcmFormat());
        audoutExit();
        return -LX_EIO;
    }
    for (uint32_t i = 0; i < buffers; i++) {
        void *p = memalign(0x1000, g_cap);
        if (!p) {
            g_count = (int)i;
            g_open = 1;
            lx_audio_close();
            return -LX_ENOMEM;
        }
        memset(p, 0, g_cap);
        memset(&g_bufs[i], 0, sizeof g_bufs[i]);
        g_bufs[i].buffer = p;
        g_bufs[i].buffer_size = g_cap;
    }
    g_count = (int)buffers;
    g_queued = 0;
    memset(g_busy, 0, sizeof g_busy);
    rc = audoutStartAudioOut();
    if (R_FAILED(rc)) {
        lx_logf("lx-audio: audoutStartAudioOut: 0x%x", rc);
        g_open = 1;
        lx_audio_close();
        return -LX_EIO;
    }
    g_open = 1;
    lx_logf("lx-audio: audout started, %u x %u frames", (unsigned)buffers, (unsigned)period);
    return 0;
}

int lx_audio_submit(const void *pcm, uint32_t bytes) {
    if (!g_open) return -LX_EBADF;
    reclaim();
    int slot = -1;
    for (int i = 0; i < g_count; i++) {
        if (!g_busy[i]) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return 0;
    if (bytes > g_cap) bytes = g_cap;
    AudioOutBuffer *b = &g_bufs[slot];
    memcpy(b->buffer, pcm, bytes);
    b->data_size = bytes;
    b->data_offset = 0;
    b->next = NULL;
    armDCacheFlush(b->buffer, bytes);
    Result rc = audoutAppendAudioOutBuffer(b);
    if (R_FAILED(rc)) {
        static int logged;
        if (logged++ < 5) lx_logf("lx-audio: audoutAppendAudioOutBuffer: 0x%x", rc);
        return -LX_EIO;
    }
    g_busy[slot] = 1;
    g_queued++;
    return 1;
}

int lx_audio_queued(void) {
    if (!g_open) return 0;
    reclaim();
    return g_queued;
}
