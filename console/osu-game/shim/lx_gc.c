/*
 * GC tweaks and diagnostics for osu! on kaiHEN.
 *
 * Memory-pressure GCs: GC.AddMemoryPressure (ImageSharp, for every unmanaged pixel buffer) asks for
 * RhCollect(2, NonBlocking). Concurrent GC is off in this build, so each one is a full BLOCKING gen2 with the world
 * stopped: every one paused the game for over 100 ms (up to 312 ms, 42% of all GC time). osu-framework disposes those
 * images explicitly, so a full collection buys nothing: they are downgraded to gen1. Allocation-driven gen2s and an
 * explicit GC.Collect() are untouched. LX_GC_PRESSURE=1 keeps them.
 *
 * One log line per GC (LX_GC_LOG unset: every GC, 0: none, N: pauses of N ms and longer; osu!'s "Switch 2" settings
 * turn it on and off through ounsu_gc_log_set):
 *   lx-gc: #N suspend S us, work W us, resume R us (alloc | induced gen G)
 *
 * Link with -Wl,--wrap=RhpCollect,--wrap=_ZN11ThreadStore17SuspendAllThreadsEb,
 * --wrap=_ZN11ThreadStore16ResumeAllThreadsEb.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

extern void ounsu_log(const char *text); /* lx_osu.c */
void ounsu_gc_log_set(int min_pause_ms);

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* Only one GC suspends at a time (the thread store lock is held), so plain statics are enough. */
static uint64_t t_suspend0, t_suspend1, gc_count;
static int log_mode = -1;             /* -1 unread, -2 off, >= 0 min pause ms */
static __thread int induced_gen = -1; /* generation passed to RhpCollect for the GC in progress */
static __thread int in_collect;       /* per thread: the GC runs on the thread that triggered it */

void ounsu_gc_log_set(int min_pause_ms) {
    log_mode = min_pause_ms < 0 ? -2 : min_pause_ms;
}

void __real__ZN11ThreadStore17SuspendAllThreadsEb(uint64_t a, uint64_t b);
void __wrap__ZN11ThreadStore17SuspendAllThreadsEb(uint64_t a, uint64_t b);
void __wrap__ZN11ThreadStore17SuspendAllThreadsEb(uint64_t a, uint64_t b) {
    t_suspend0 = now_us();
    __real__ZN11ThreadStore17SuspendAllThreadsEb(a, b);
    t_suspend1 = now_us();
}

void __real__ZN11ThreadStore16ResumeAllThreadsEb(uint64_t a, uint64_t b);
void __wrap__ZN11ThreadStore16ResumeAllThreadsEb(uint64_t a, uint64_t b);
void __wrap__ZN11ThreadStore16ResumeAllThreadsEb(uint64_t a, uint64_t b) {
    uint64_t t0 = now_us();
    __real__ZN11ThreadStore16ResumeAllThreadsEb(a, b);
    uint64_t t1 = now_us();
    gc_count++;
    if (log_mode == -1) {
        const char *e = getenv("LX_GC_LOG");
        log_mode = !e || !*e ? 0 : atoi(e) > 0 ? atoi(e) : -2;
    }
    if (log_mode == -2 || t1 - t_suspend0 < (uint64_t)log_mode * 1000u) return;
    char line[160];
    snprintf(line, sizeof line, "lx-gc: #%llu suspend %llu us, work %llu us, resume %llu us (%s %d)",
             (unsigned long long)gc_count, (unsigned long long)(t_suspend1 - t_suspend0),
             (unsigned long long)(t0 - t_suspend1), (unsigned long long)(t1 - t0),
             in_collect ? "induced gen" : "alloc", in_collect ? induced_gen : 0);
    ounsu_log(line);
}

/* RhpCollect(generation, mode, lowMemoryP): the managed GC.Collect/AddMemoryPressure entry. */
void __real_RhpCollect(uint64_t gen, uint64_t mode, uint64_t low);
void __wrap_RhpCollect(uint64_t gen, uint64_t mode, uint64_t low);
void __wrap_RhpCollect(uint64_t gen, uint64_t mode, uint64_t low) {
    static int keep = -1;
    int outer = in_collect;
    if (keep < 0) {
        const char *e = getenv("LX_GC_PRESSURE");
        keep = e && *e == '1';
    }
    /* AddMemoryPressure's request: generation 2, InternalGCCollectionMode.NonBlocking (1), not low-memory. */
    if (!keep && (int32_t)gen == 2 && (uint32_t)mode == 1u && !(uint32_t)low) gen = 1;
    in_collect = 1;
    induced_gen = (int)(int32_t)gen;
    __real_RhpCollect(gen, mode, low);
    in_collect = outer;
}
