/*
 * lx_audio_out.c: low-latency audio output for osu! on kaiHEN.
 *
 * This shim takes over the one device stream mysoundlib opens (-Wl,--wrap=SDL_OpenAudioDeviceStream): it creates a
 * plain SDL_AudioStream (no device) that converts mysoundlib's format to the console's 48 kHz s16 stereo, keeps
 * mysoundlib's get-callback on it, and runs its own thread that pulls from the stream one period at a time and hands
 * it to the host's audout queue (lx_audio_submit, lx/host/lx_audio.c). Only SDL's public API is used; SDL's own audio
 * driver stays "dummy" (romfs lx-env) and only serves as a fallback.
 *
 * Ported from HBLauncher's hbxl_audio_out.c (which drove nn::audio AudioOut at SDK offsets). Environment (lx-env;
 * osu!'s "Switch 2" settings override period and count through hbxl_audio_set_queue, which also reopens a running
 * output):
 *   LX_AUDIO_PERIOD=<frames>   frames per buffer, 16..1024 (default 128 = 2.7 ms)
 *   LX_AUDIO_BUFFERS=<n>       buffers queued, 1..16 (default 3)  -> latency ~ period x buffers (default 8 ms)
 *   LX_AUDIO_SDL=1             don't take over: SDL's own driver (dummy: silence)
 * A GC pause longer than the queue makes a short dropout (silence, no added latency); raise the period or count if
 * crackles bother you more than delay.
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL3/SDL.h>

extern void hbxl_log(const char *text); /* lx_osu.c */
int lx_audio_open(uint32_t period, uint32_t buffers); /* island runtime -> host */
int lx_audio_submit(const void *pcm, uint32_t bytes);
int lx_audio_queued(void);
void lx_audio_close(void);

SDL_AudioStream *__real_SDL_OpenAudioDeviceStream(SDL_AudioDeviceID devid, const SDL_AudioSpec *spec,
                                                  SDL_AudioStreamCallback callback, void *userdata);
bool __real_SDL_ResumeAudioStreamDevice(SDL_AudioStream *stream);
bool __real_SDL_PauseAudioStreamDevice(SDL_AudioStream *stream);
void __real_SDL_DestroyAudioStream(SDL_AudioStream *stream);
SDL_AudioStream *__wrap_SDL_OpenAudioDeviceStream(SDL_AudioDeviceID devid, const SDL_AudioSpec *spec,
                                                  SDL_AudioStreamCallback callback, void *userdata);
bool __wrap_SDL_ResumeAudioStreamDevice(SDL_AudioStream *stream);
bool __wrap_SDL_PauseAudioStreamDevice(SDL_AudioStream *stream);
void __wrap_SDL_DestroyAudioStream(SDL_AudioStream *stream);
int hbxl_audio_set_queue(int period, int buffers);

#define OUT_RATE 48000
#define OUT_FRAME_BYTES 4 /* s16 stereo */
#define MAX_BUFFERS 16
#define MAX_PERIOD 1024

static struct {
    SDL_AudioStream *stream; /* the stream we own (NULL: none) */
    bool out_open;
    pthread_t thread;
    bool thread_started;
    atomic_bool run;     /* thread keeps going */
    atomic_bool playing; /* resumed by mysoundlib */
    int period, buffers;
    int set_period, set_buffers; /* from hbxl_audio_set_queue (0: not set, use lx-env / defaults) */
    unsigned long underruns;
    unsigned long worst_gap_us;
} g;

/* Open, close and reconfigure exclude each other (audio thread vs. the settings on the update thread). */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    hbxl_log(line);
}

static int env_int(const char *name, int def, int lo, int hi) {
    const char *e = getenv(name);
    if (!e || !*e) return def;
    int v = atoi(e);
    return v < lo ? lo : v > hi ? hi : v;
}

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void *out_thread(void *arg) {
    (void)arg;
    const int bytes = g.period * OUT_FRAME_BYTES;
    const struct timespec nap = { 0, 500 * 1000 }; /* 0.5 ms */
    unsigned long last_report = 0;
    bool primed = false;
    unsigned char *pcm = malloc((size_t)bytes);
    if (!pcm) return NULL;

    while (atomic_load(&g.run)) {
        int queued = lx_audio_queued();
        if (queued >= g.buffers) {
            nanosleep(&nap, NULL);
            continue;
        }
        int got = 0;
        if (atomic_load(&g.playing)) {
            /* The callback runs managed code: during a GC it blocks until the GC ends. If that takes longer than the
               audio still queued, the output ran dry (a dropout). */
            uint64_t t0 = now_us();
            got = SDL_GetAudioStreamData(g.stream, pcm, bytes); /* runs mysoundlib's callback for more input */
            if (got < 0) got = 0;
            uint64_t took = now_us() - t0, queued_us = (uint64_t)queued * g.period * 1000000u / OUT_RATE;
            /* The estimate counts the playing buffer as whole, so gaps under 1 ms are noise (run 11: ~25/s, inaudible). */
            if (primed && took > queued_us + 1000) {
                unsigned long gap = (unsigned long)(took - queued_us);
                g.underruns++;
                if (gap > g.worst_gap_us) g.worst_gap_us = gap;
                if (g.underruns - last_report >= 25 || g.underruns == 1 || gap > 100000) {
                    logf_("lx-audio: dropout #%lu, %lu ms (mixing blocked %lu ms; worst %lu ms)", g.underruns,
                          gap / 1000, (unsigned long)(took / 1000), g.worst_gap_us / 1000);
                    last_report = g.underruns;
                }
            }
        }
        if (got < bytes) memset(pcm + got, 0, (size_t)(bytes - got));
        int r = lx_audio_submit(pcm, (uint32_t)bytes);
        if (r <= 0) {
            nanosleep(&nap, NULL);
            continue;
        }
        if (queued + 1 >= g.buffers) primed = true;
    }
    free(pcm);
    return NULL;
}

static void close_out(void) {
    if (g.thread_started) {
        atomic_store(&g.run, false);
        pthread_join(g.thread, NULL);
        g.thread_started = false;
    }
    if (g.out_open) {
        lx_audio_close();
        g.out_open = false;
    }
}

static bool open_out(void) {
    int r = lx_audio_open((uint32_t)g.period, (uint32_t)g.buffers);
    if (r != 0) {
        logf_("lx-audio: the host's audio output failed to open (%d)", r);
        return false;
    }
    g.out_open = true;
    return true;
}

static bool start_thread(void) {
    atomic_store(&g.run, true);
    if (pthread_create(&g.thread, NULL, out_thread, NULL) != 0) {
        atomic_store(&g.run, false);
        return false;
    }
    g.thread_started = true;
    return true;
}

/*
 * osu!'s "Switch 2" settings: frames per buffer and buffers queued. Before the device is opened the values are just
 * remembered; on a running output it is closed and reopened with the new queue (a short gap), the SDL stream and
 * mysoundlib's callback stay. Returns the queue length in microseconds, 0 when our output isn't in use (yet).
 */
int hbxl_audio_set_queue(int period, int buffers) {
    int result = 0;
    period = period < 16 ? 16 : period > MAX_PERIOD ? MAX_PERIOD : period;
    buffers = buffers < 1 ? 1 : buffers > MAX_BUFFERS ? MAX_BUFFERS : buffers;

    pthread_mutex_lock(&g_lock);
    g.set_period = period;
    g.set_buffers = buffers;
    if (g.stream && (period != g.period || buffers != g.buffers)) {
        int old_period = g.period, old_buffers = g.buffers;
        close_out();
        g.period = period;
        g.buffers = buffers;
        if (!open_out()) {
            g.period = old_period;
            g.buffers = old_buffers;
            if (!open_out()) hbxl_log("lx-audio: the output can't be reopened; no sound until osu! is restarted");
        }
        if (g.out_open && !start_thread()) {
            hbxl_log("lx-audio: can't restart the output thread; no sound until osu! is restarted");
            close_out();
        }
        if (g.out_open)
            logf_("lx-audio: queue now %d x %d frames (%.1f ms) [settings]", g.buffers, g.period,
                  g.buffers * g.period * 1000.0 / OUT_RATE);
    }
    if (g.stream && g.out_open) result = (int)((int64_t)g.buffers * g.period * 1000000 / OUT_RATE);
    pthread_mutex_unlock(&g_lock);
    return result;
}

static SDL_AudioStream *open_stream(SDL_AudioDeviceID devid, const SDL_AudioSpec *spec, SDL_AudioStreamCallback callback,
                                    void *userdata) {
    const char *e = getenv("LX_AUDIO_SDL");
    if ((e && *e == '1') || !spec || g.stream) {
        hbxl_log("lx-audio: using SDL's own audio driver");
        return __real_SDL_OpenAudioDeviceStream(devid, spec, callback, userdata);
    }

    g.period = g.set_period ? g.set_period : env_int("LX_AUDIO_PERIOD", 128, 16, MAX_PERIOD);
    g.buffers = g.set_buffers ? g.set_buffers : env_int("LX_AUDIO_BUFFERS", 3, 1, MAX_BUFFERS);

    const SDL_AudioSpec out = { SDL_AUDIO_S16LE, 2, OUT_RATE };
    SDL_AudioStream *stream = SDL_CreateAudioStream(spec, &out);
    if (!stream) {
        logf_("lx-audio: SDL_CreateAudioStream failed (%s); falling back to SDL's driver", SDL_GetError());
        return __real_SDL_OpenAudioDeviceStream(devid, spec, callback, userdata);
    }
    if (callback && !SDL_SetAudioStreamGetCallback(stream, callback, userdata)) {
        logf_("lx-audio: SDL_SetAudioStreamGetCallback failed (%s)", SDL_GetError());
        __real_SDL_DestroyAudioStream(stream);
        return __real_SDL_OpenAudioDeviceStream(devid, spec, callback, userdata);
    }
    if (!open_out()) {
        __real_SDL_DestroyAudioStream(stream);
        return __real_SDL_OpenAudioDeviceStream(devid, spec, callback, userdata);
    }

    g.stream = stream;
    g.underruns = 0;
    g.worst_gap_us = 0;
    atomic_store(&g.playing, false);
    if (!start_thread()) {
        hbxl_log("lx-audio: can't start the output thread");
        close_out();
        g.stream = NULL;
        __real_SDL_DestroyAudioStream(stream);
        return __real_SDL_OpenAudioDeviceStream(devid, spec, callback, userdata);
    }
    logf_("lx-audio: own output, %d x %d frames (%.1f ms); %d Hz %d ch in, 48000 Hz s16 out", g.buffers, g.period,
          g.buffers * g.period * 1000.0 / OUT_RATE, spec->freq, spec->channels);
    return stream;
}

SDL_AudioStream *__wrap_SDL_OpenAudioDeviceStream(SDL_AudioDeviceID devid, const SDL_AudioSpec *spec,
                                                  SDL_AudioStreamCallback callback, void *userdata) {
    pthread_mutex_lock(&g_lock);
    SDL_AudioStream *stream = open_stream(devid, spec, callback, userdata);
    pthread_mutex_unlock(&g_lock);
    return stream;
}

bool __wrap_SDL_ResumeAudioStreamDevice(SDL_AudioStream *stream) {
    if (stream && stream == g.stream) {
        atomic_store(&g.playing, true);
        return true;
    }
    return __real_SDL_ResumeAudioStreamDevice(stream);
}

bool __wrap_SDL_PauseAudioStreamDevice(SDL_AudioStream *stream) {
    if (stream && stream == g.stream) {
        atomic_store(&g.playing, false);
        return true;
    }
    return __real_SDL_PauseAudioStreamDevice(stream);
}

void __wrap_SDL_DestroyAudioStream(SDL_AudioStream *stream) {
    pthread_mutex_lock(&g_lock);
    if (stream && stream == g.stream) {
        close_out();
        g.stream = NULL;
        logf_("lx-audio: output closed (%lu dropout(s), worst %lu ms)", g.underruns, g.worst_gap_us / 1000);
    }
    pthread_mutex_unlock(&g_lock);
    __real_SDL_DestroyAudioStream(stream);
}
