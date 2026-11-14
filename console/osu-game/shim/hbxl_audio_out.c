/*
 * hbxl_audio_out.c: low-latency audio output for osu! on HBLauncher (Switch 2).
 *
 * SDL's hbx audio driver (src/native/SDL, which we must not edit) plays through nn::audio AudioOut with a fixed
 * 4 x 1024-frame queue: up to ~85 ms between mixing and the speaker, which players feel as input delay on hitsounds.
 * SDL ignores SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES for that driver.
 *
 * This shim takes over the one device stream mysoundlib opens (-Wl,--wrap=SDL_OpenAudioDeviceStream): it creates a
 * plain SDL_AudioStream (no device) that converts mysoundlib's format to the console's 48 kHz s16 stereo, keeps
 * mysoundlib's get-callback on it, and runs its own thread that pulls from the stream straight into AudioOut buffers.
 * Only SDL's public API is used.
 *
 * hbxl-env knobs (read at device open; osu!'s "Switch 2" settings override period and count through
 * hbxl_audio_set_queue, which also reopens a running output):
 *   HBXL_AUDIO_PERIOD=<frames>   frames per AudioOut buffer, 64..1024 (default 128 = 2.7 ms; was 256 until 2026-11-10)
 *   HBXL_AUDIO_BUFFERS=<n>       buffers queued, 2..16 (default 2; was 3)  → latency ≈ period × buffers (default ~5 ms)
 *   HBXL_AUDIO_SDL=1             don't take over: SDL's own hbx driver (the old ~85 ms path)
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

#include "hbx_sdk.h"

extern const struct hbx_ctx *hbxl_ctx;
extern void hbxl_log(const char *text);
void *hbxl_sdk_memalign(size_t align, size_t size);
void hbxl_sdk_free(void *p);

SDL_AudioStream *__real_SDL_OpenAudioDeviceStream(SDL_AudioDeviceID devid, const SDL_AudioSpec *spec,
                                                  SDL_AudioStreamCallback callback, void *userdata);
bool __real_SDL_ResumeAudioStreamDevice(SDL_AudioStream *stream);
bool __real_SDL_PauseAudioStreamDevice(SDL_AudioStream *stream);
void __real_SDL_DestroyAudioStream(SDL_AudioStream *stream);

#define OUT_RATE 48000
#define OUT_FRAME_BYTES 4            /* s16 stereo */
#define MAX_BUFFERS 16
#define PCM_BYTES 0x1000             /* AudioOut buffers: 4 KiB aligned and sized; data size can be smaller */
#define MAX_PERIOD (PCM_BYTES / OUT_FRAME_BYTES)

typedef void (*init_param_fn)(void *);
typedef int32_t (*open_fn)(void *, const void *);
typedef int32_t (*query_fn)(void *);
typedef void (*set_info_fn)(void *, const void *, uint64_t, uint64_t);
typedef int32_t (*append_fn)(void *, void *);
typedef void *(*released_fn)(void *);
#define API(field, type) ((type)(uintptr_t)hbxl_ctx->audio.field)

struct out_mem {
    unsigned char object[0x800];                   /* nn::audio::AudioOut */
    unsigned char descriptors[MAX_BUFFERS][0x40];  /* nn::audio::AudioOutBuffer */
    unsigned char pad[0x1000 - 0x800 - MAX_BUFFERS * 0x40];
    unsigned char pcm[MAX_BUFFERS][PCM_BYTES];
};
_Static_assert(0x800 + MAX_BUFFERS * 0x40 <= 0x1000, "out_mem header");

static struct {
    SDL_AudioStream *stream;   /* the stream we own (NULL: none) */
    struct out_mem *mem;
    pthread_t thread;
    bool thread_started;
    atomic_bool run;           /* thread keeps going */
    atomic_bool playing;       /* resumed by mysoundlib */
    int period, buffers;
    int set_period, set_buffers;   /* from hbxl_audio_set_queue (0: not set, use hbxl-env / defaults) */
    bool busy[MAX_BUFFERS];
    int queued;
    bool started;
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

static void reclaim(void) {
    for (int guard = 0; guard <= g.buffers; guard++) {
        void *released = API(released_buffer, released_fn)(g.mem->object);
        if (!released) return;
        for (int i = 0; i < g.buffers; i++) {
            if (released == g.mem->descriptors[i] && g.busy[i]) {
                g.busy[i] = false;
                g.queued--;
                break;
            }
        }
    }
}

static void *out_thread(void *arg) {
    (void)arg;
    const int bytes = g.period * OUT_FRAME_BYTES;
    const struct timespec nap = {0, 500 * 1000};   /* 0.5 ms */
    unsigned long last_report = 0;

    while (atomic_load(&g.run)) {
        reclaim();

        int slot = -1;
        for (int i = 0; i < g.buffers; i++)
            if (!g.busy[i]) { slot = i; break; }

        if (slot < 0) {
            nanosleep(&nap, NULL);
            continue;
        }

        unsigned char *pcm = g.mem->pcm[slot];
        int got = 0;
        if (atomic_load(&g.playing)) {
            /* The callback runs managed code: during a GC it blocks until the GC ends. If that takes longer than the audio
             * still queued, AudioOut ran dry (a dropout). Measure it here: an empty-queue check between fills misses it. */
            int queued_before = g.queued;
            uint64_t t0 = now_us();
            got = SDL_GetAudioStreamData(g.stream, pcm, bytes);   /* runs mysoundlib's callback for more input */
            if (got < 0) got = 0;
            uint64_t took = now_us() - t0, queued_us = (uint64_t)queued_before * g.period * 1000000u / OUT_RATE;
            if (g.started && took > queued_us) {
                unsigned long gap = (unsigned long)(took - queued_us);
                g.underruns++;
                if (gap > g.worst_gap_us) g.worst_gap_us = gap;
                if (g.underruns - last_report >= 25 || g.underruns == 1 || gap > 100000) {
                    logf_("hbxl-audio: dropout #%lu, %lu ms (mixing blocked %lu ms; worst %lu ms)", g.underruns, gap / 1000,
                          (unsigned long)(took / 1000), g.worst_gap_us / 1000);
                    last_report = g.underruns;
                }
            }
        }
        if (got < bytes) memset(pcm + got, 0, (size_t)(bytes - got));

        API(set_buffer_info, set_info_fn)(g.mem->descriptors[slot], pcm, PCM_BYTES, (uint64_t)bytes);
        if (API(append_buffer, append_fn)(g.mem->object, g.mem->descriptors[slot]) != 1) {
            nanosleep(&nap, NULL);
            continue;
        }
        g.busy[slot] = true;
        g.queued++;

        if (!g.started && g.queued >= g.buffers) {
            if (API(start, query_fn)(g.mem->object) == 0) {
                g.started = true;
                logf_("hbxl-audio: AudioOut started (%d x %d frames = %.1f ms queue)", g.buffers, g.period,
                      g.buffers * g.period * 1000.0 / OUT_RATE);
            }
        }
    }
    return NULL;
}

static void close_out(void) {
    if (g.thread_started) {
        atomic_store(&g.run, false);
        pthread_join(g.thread, NULL);
        g.thread_started = false;
    }
    if (g.mem) {
        if (g.started) API(stop, query_fn)(g.mem->object);
        API(flush_buffers, query_fn)(g.mem->object);
        for (int tries = 0; tries < 40 && g.queued > 0; tries++) {
            reclaim();
            if (g.queued > 0) SDL_Delay(1);
        }
        if (hbxl_ctx->audio.close) API(close, query_fn)(g.mem->object);
        hbxl_sdk_free(g.mem);
        g.mem = NULL;
    }
    g.started = false;
    g.queued = 0;
    memset(g.busy, 0, sizeof g.busy);
}

static bool open_out(void) {
    uint32_t parameter[4] = {0, 0, 0, 0};

    if (!hbxl_ctx || !hbxl_ctx->audio.open_default_audio_out || !hbxl_ctx->audio.append_buffer) {
        hbxl_log("hbxl-audio: the host provided no AudioOut entrypoints");
        return false;
    }
    g.mem = hbxl_sdk_memalign(0x1000, sizeof(struct out_mem));
    if (!g.mem) {
        hbxl_log("hbxl-audio: no SDK heap memory for AudioOut");
        return false;
    }
    memset(g.mem, 0, sizeof *g.mem);

    API(initialize_parameter, init_param_fn)(parameter);
    parameter[0] = OUT_RATE;
    parameter[1] = (parameter[1] & 0xFFFF0000u) | 2;
    if (API(open_default_audio_out, open_fn)(g.mem->object, parameter) != 0) {
        hbxl_log("hbxl-audio: OpenDefaultAudioOut failed");
        hbxl_sdk_free(g.mem);
        g.mem = NULL;
        return false;
    }
    if (API(sample_rate, query_fn)(g.mem->object) != OUT_RATE || API(channel_count, query_fn)(g.mem->object) != 2 ||
        API(sample_format, query_fn)(g.mem->object) != 2) {
        hbxl_log("hbxl-audio: AudioOut format is not 48 kHz stereo s16");
        if (hbxl_ctx->audio.close) API(close, query_fn)(g.mem->object);
        hbxl_sdk_free(g.mem);
        g.mem = NULL;
        return false;
    }
    return true;
}

static SDL_AudioStream *open_stream(SDL_AudioDeviceID devid, const SDL_AudioSpec *spec, SDL_AudioStreamCallback callback,
                                    void *userdata);

SDL_AudioStream *__wrap_SDL_OpenAudioDeviceStream(SDL_AudioDeviceID devid, const SDL_AudioSpec *spec,
                                                  SDL_AudioStreamCallback callback, void *userdata) {
    SDL_AudioStream *stream;
    pthread_mutex_lock(&g_lock);
    stream = open_stream(devid, spec, callback, userdata);
    pthread_mutex_unlock(&g_lock);
    return stream;
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
 * remembered; on a running output AudioOut is closed and reopened with the new queue (a short gap), the SDL stream
 * and mysoundlib's callback stay. Returns the queue length in microseconds, 0 when our output isn't in use (yet).
 */
int hbxl_audio_set_queue(int period, int buffers) {
    int result = 0;
    period = period < 64 ? 64 : period > MAX_PERIOD ? MAX_PERIOD : period;
    buffers = buffers < 2 ? 2 : buffers > MAX_BUFFERS ? MAX_BUFFERS : buffers;

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
            if (!open_out())
                hbxl_log("hbxl-audio: AudioOut can't be reopened; no sound until osu! is restarted");
        }
        if (g.mem && !start_thread()) {
            hbxl_log("hbxl-audio: can't restart the output thread; no sound until osu! is restarted");
            close_out();
        }
        if (g.mem)
            logf_("hbxl-audio: queue now %d x %d frames (%.1f ms) [settings]", g.buffers, g.period,
                  g.buffers * g.period * 1000.0 / OUT_RATE);
    }
    if (g.stream && g.mem)
        result = (int)((int64_t)g.buffers * g.period * 1000000 / OUT_RATE);
    pthread_mutex_unlock(&g_lock);
    return result;
}

static SDL_AudioStream *open_stream(SDL_AudioDeviceID devid, const SDL_AudioSpec *spec, SDL_AudioStreamCallback callback,
                                    void *userdata) {
    const char *e = getenv("HBXL_AUDIO_SDL");
    if ((e && *e == '1') || !spec || g.stream) {
        hbxl_log("hbxl-audio: using SDL's hbx audio driver (fixed 4 x 1024 frames)");
        return __real_SDL_OpenAudioDeviceStream(devid, spec, callback, userdata);
    }

    g.period = g.set_period ? g.set_period : env_int("HBXL_AUDIO_PERIOD", 128, 64, MAX_PERIOD);
    g.buffers = g.set_buffers ? g.set_buffers : env_int("HBXL_AUDIO_BUFFERS", 2, 2, MAX_BUFFERS);

    const SDL_AudioSpec out = {SDL_AUDIO_S16LE, 2, OUT_RATE};
    SDL_AudioStream *stream = SDL_CreateAudioStream(spec, &out);
    if (!stream) {
        logf_("hbxl-audio: SDL_CreateAudioStream failed (%s); falling back to SDL's driver", SDL_GetError());
        return __real_SDL_OpenAudioDeviceStream(devid, spec, callback, userdata);
    }
    if (callback && !SDL_SetAudioStreamGetCallback(stream, callback, userdata)) {
        logf_("hbxl-audio: SDL_SetAudioStreamGetCallback failed (%s)", SDL_GetError());
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
        hbxl_log("hbxl-audio: can't start the output thread");
        close_out();
        g.stream = NULL;
        __real_SDL_DestroyAudioStream(stream);
        return __real_SDL_OpenAudioDeviceStream(devid, spec, callback, userdata);
    }
    logf_("hbxl-audio: own AudioOut output, %d x %d frames (%.1f ms); %d Hz %d ch in, 48000 Hz s16 out",
          g.buffers, g.period, g.buffers * g.period * 1000.0 / OUT_RATE, spec->freq, spec->channels);
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
        logf_("hbxl-audio: output closed (%lu dropout(s), worst %lu ms)", g.underruns, g.worst_gap_us / 1000);
    }
    pthread_mutex_unlock(&g_lock);
    __real_SDL_DestroyAudioStream(stream);
}
