/*
 * GC pause breakdown for the console (diagnostics; costs a few hundred ns per GC).
 *
 * A NativeAOT GC pause = ThreadStore::SuspendAllThreads (hijack loop: walk threads, hijack the ones
 * still in cooperative mode through our emulated activation, minipal_microdelay(8..100 us) between
 * rounds) + the collection itself + ThreadStore::ResumeAllThreads. On the console a typical gen0/1
 * pause was 25-35 ms (desktop: 1-3 ms); measured 2026-11-08: suspension takes microseconds, the collection
 * itself is the cost. These wraps time each part and log one line per GC:
 *
 *   hbxl-gc: #N tid T suspend S ms (delays D: req R us, slept A us, max M us; hijacks H, H ms,
 *            last tid L; yields Y) work W ms resume R ms
 *
 * Link with -Wl,--wrap=_ZN11ThreadStore17SuspendAllThreadsEb,--wrap=_ZN11ThreadStore16ResumeAllThreadsEb,
 * --wrap=minipal_microdelay,--wrap=_Z17PalSwitchToThreadv. hbxl_dotnet_shim.c reports each emulated
 * activation through hbxl_gc_note_hijack. HBXL_GC_LOG=0 in hbxl-env silences the per-GC lines
 * (HBXL_GC_LOG=<ms> logs only pauses of at least that many ms; default 0 = every GC).
 *
 * Each line also says whether the GC was induced (RhpCollect: GC.Collect, GC.AddMemoryPressure,
 * ...) and with which generation; allocation-triggered GCs show "alloc".
 *
 * Memory-pressure GCs: GC.AddMemoryPressure (ImageSharp, for every unmanaged pixel buffer) asks for
 * RhCollect(2, NonBlocking). Concurrent GC is off in this build, so each one was a full BLOCKING gen2
 * with the world stopped: every pause over 100 ms in the 2026-11-08 run (23 of them, up to 312 ms, 42% of
 * all GC time; they also starved input polling). osu-framework disposes those images explicitly, so a
 * full collection buys nothing; they are downgraded to gen1 (logged as "induced gen 1 (pressure)").
 * Allocation-driven gen2s and explicit GC.Collect() are untouched. HBXL_GC_PRESSURE=1 keeps them.
 *
 * (A priority-boost experiment, HBXL_GC_BOOST, deadlocked on 2026-11-08: the boosted GC thread spun
 * on the vfs lock with svcSleepThread(0), which never yields to the lower-priority holder on the
 * same core. Don't raise thread priorities around anything that can wait on a spin lock.)
 */
#include <stdint.h>
#include <stdlib.h>

#include "hbx_sdk.h"

extern const struct hbx_ctx *hbxl_ctx;
extern void hbxl_logf(const char *format, ...);
extern long hbxl_gettid(void);

extern uint64_t hbxl_svc_tick(void);

/* Tick source, as WiiCompiled's fast-clock patch picks it: a direct counter read only if
 * nn::os::GetSystemTick (sdk +1218112) itself does one; otherwise SVC 0x1e. */
static int tick_mode = -1; /* -1 unknown, 0 svc, 1 cntvct, 2 cntpct */

static uint64_t ticks(void)
{
    uint64_t t;
    if (tick_mode < 0) {
        tick_mode = 0;
        if (hbxl_ctx && hbxl_ctx->input.pad_handheld) {
            const uint32_t *fn = (const uint32_t *)(uintptr_t)(hbxl_ctx->input.pad_handheld - 4845408u + 1218112u);
            for (int k = 0; k < 4; k++) {
                if ((fn[k] & 0xffffffe0u) == 0xd53be040u) { tick_mode = 1; break; }
                if ((fn[k] & 0xffffffe0u) == 0xd53be020u) { tick_mode = 2; break; }
            }
        }
    }
    if (tick_mode == 1) { __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t)); return t; }
    if (tick_mode == 2) { __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t)); return t; }
    return hbxl_svc_tick();
}

static uint64_t us(uint64_t t)
{
    uint64_t hz = hbxl_ctx && hbxl_ctx->tick_hz ? hbxl_ctx->tick_hz : 19200000ull;
    return t * 1000000ull / hz;
}

/* Only one GC suspends at a time (the thread store lock is held), so plain statics are enough. */
static int in_suspend;
static uint64_t t_suspend0, t_suspend1, gc_count;
static uint64_t delays, delay_req_us, delay_ticks, delay_max_ticks;
static uint64_t hijacks, hijack_ticks, yields;
static int last_hijack_tid;
static int log_mode = -1; /* -1 unread, -2 off, >= 0 min pause ms */
static __thread int induced_gen = -1;  /* generation passed to RhpCollect for the GC in progress, -1 = allocation */
static __thread int in_collect; /* per thread: the GC runs on the thread that triggered it */

void hbxl_gc_note_hijack(int tid, uint64_t t)
{
    if (!in_suspend)
        return;
    hijacks++;
    hijack_ticks += t;
    last_hijack_tid = tid;
}

uint64_t hbxl_gc_ticks(void) { return ticks(); }

void __real__ZN11ThreadStore17SuspendAllThreadsEb(uint64_t a, uint64_t b);
void __wrap__ZN11ThreadStore17SuspendAllThreadsEb(uint64_t a, uint64_t b)
{
    delays = delay_req_us = delay_ticks = delay_max_ticks = 0;
    hijacks = hijack_ticks = yields = 0;
    last_hijack_tid = 0;
    in_suspend = 1;
    t_suspend0 = ticks();
    __real__ZN11ThreadStore17SuspendAllThreadsEb(a, b);
    t_suspend1 = ticks();
    in_suspend = 0;
}

/* osu!'s "Switch 2" settings: the hbxl-gc line per collection on (min_pause_ms >= 0) or off (< 0). */
void hbxl_gc_log_set(int min_pause_ms)
{
    log_mode = min_pause_ms < 0 ? -2 : min_pause_ms;
}

void __real__ZN11ThreadStore16ResumeAllThreadsEb(uint64_t a, uint64_t b);
void __wrap__ZN11ThreadStore16ResumeAllThreadsEb(uint64_t a, uint64_t b)
{
    uint64_t t0 = ticks(), t1;
    __real__ZN11ThreadStore16ResumeAllThreadsEb(a, b);
    t1 = ticks();
    gc_count++;

    if (log_mode == -1) {
        const char *e = getenv("HBXL_GC_LOG");
        log_mode = e && *e ? (*e == '0' && !e[1] ? -2 : atoi(e)) : 0;
    }
    if (log_mode == -2 || us(t1 - t_suspend0) < (uint64_t)log_mode * 1000u)
        return;
    /* Times in microseconds; the reader divides. */
    hbxl_logf("hbxl-gc: #%d tid %d suspend %d us (delays %d: req %d us, slept %d us, max %d us; hijacks %d, %d us, "
              "last tid %d; yields %d) work %d us resume %d us, %s %d",
              (int64_t)gc_count, (int64_t)hbxl_gettid(), (int64_t)us(t_suspend1 - t_suspend0), (int64_t)delays,
              (int64_t)delay_req_us, (int64_t)us(delay_ticks), (int64_t)us(delay_max_ticks), (int64_t)hijacks,
              (int64_t)us(hijack_ticks), (int64_t)last_hijack_tid, (int64_t)yields, (int64_t)us(t0 - t_suspend1),
              (int64_t)us(t1 - t0), in_collect ? "induced gen" : "alloc", (int64_t)(in_collect ? induced_gen : 0));
}

void __real_minipal_microdelay(uint32_t usecs, uint32_t *since_yield);
void __wrap_minipal_microdelay(uint32_t usecs, uint32_t *since_yield)
{
    uint64_t t0, t;
    if (!in_suspend) {
        __real_minipal_microdelay(usecs, since_yield);
        return;
    }
    t0 = ticks();
    __real_minipal_microdelay(usecs, since_yield);
    t = ticks() - t0;
    delays++;
    delay_req_us += usecs;
    delay_ticks += t;
    if (t > delay_max_ticks)
        delay_max_ticks = t;
}

int __real__Z17PalSwitchToThreadv(void);
int __wrap__Z17PalSwitchToThreadv(void)
{
    if (in_suspend)
        yields++;
    return __real__Z17PalSwitchToThreadv();
}

/* RhpCollect(generation, mode, lowMemoryP): the managed GC.Collect/AddMemoryPressure entry. */
void __real_RhpCollect(uint64_t gen, uint64_t mode, uint64_t low);
void __wrap_RhpCollect(uint64_t gen, uint64_t mode, uint64_t low)
{
    static int keep = -1;
    int outer = in_collect;
    if (keep < 0) {
        const char *e = getenv("HBXL_GC_PRESSURE");
        keep = e && *e == '1';
    }
    /* AddMemoryPressure's request: generation 2, InternalGCCollectionMode.NonBlocking (1), not low-memory. */
    if (!keep && (int32_t)gen == 2 && (uint32_t)mode == 1u && !(uint32_t)low)
        gen = 1;
    in_collect = 1;
    induced_gen = (int)(int32_t)gen;
    __real_RhpCollect(gen, mode, low);
    in_collect = outer;
}
