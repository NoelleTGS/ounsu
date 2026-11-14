/*
 * hbxl_dotnet_shim.c - libc overrides and runtime wraps that let the .NET NativeAOT runtime
 * run on HBLauncher's Linux personality.  Linked ahead of musl's libc.a, so these definitions
 * win and musl's members are never pulled in.
 *
 * GC thread activation (link with -Wl,--wrap=_Z9PalHijackP6Thread):
 *   To suspend a thread for a GC, NativeAOT's PalHijack(Thread*) marks it activation-pending and
 *   sends it a signal; the signal handler runs Thread::HijackCallback on the interrupted context,
 *   which suspends the thread in place or hijacks the return address of the managed method it is
 *   in, and then clears the pending flag.  hbxl has no signals (tkill is a fatal exit), and
 *   without an activation the GC can only wait for each cooperative-mode thread to reach a
 *   safepoint on its own.  On the console a thread that never does (a game loop that skips its
 *   sleep, long reflection metadata decoding, a spin in Thread.Yield) stalls the GC and then every
 *   other thread for a long time or forever (hardware dumps 2026-11-08).
 *   __wrap_PalHijack emulates the activation the way Windows does it: pause the target
 *   (nn::os::SuspendThread via hbxl_thread_pause), build a ucontext_t from its registers, call
 *   HijackCallback(ctx, thread, doInlineSuspend=false) from the GC thread, resume the target.
 *   Return-address hijacks on the target's stack are plain memory writes and take effect; a hijack
 *   of the LR *register* cannot be written back (no SetThreadContext), so it is a no-op that the
 *   GC retries on its next activation round, when the thread is somewhere else.
 *
 * pthread_kill: anything else that signals a thread is reported as delivered without sending.
 *
 * mlock: used by the FlushProcessWriteBuffers fallback during startup.  hbxl memory is always
 *   resident, so success is the truthful answer.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

static void shim_log_once(volatile int *flag, const char *msg, size_t len) {
    if (__atomic_exchange_n(flag, 1, __ATOMIC_RELAXED)) return;
    (void)!write(2, msg, len);
}

/* hbxl runtime (sdk/linux). */
void *hbxl_thread_pause(int tid, uint64_t regs[34]);
void hbxl_thread_resume_nn(void *nn);
void hbxl_logf(const char *format, ...);
/* hbxl_gc_timing.c (osu-game only): per-GC pause breakdown. Weak, so other apps sharing this shim link without it. */
void hbxl_gc_note_hijack(int tid, uint64_t ticks) __attribute__((weak));
uint64_t hbxl_gc_ticks(void) __attribute__((weak));

/* NativeAOT runtime (libRuntime.*.a).  NATIVE_CONTEXT is the signal's ucontext_t (its
 * ActivationHandler passes the handler's context pointer straight through). */
void rt_HijackCallback(void *context, void *thread, _Bool doInlineSuspend)
    __asm__("_ZN6Thread14HijackCallbackEP14NATIVE_CONTEXTPS_b");
void rt_SetActivationPending(void *thread, _Bool pending) __asm__("_ZN6Thread20SetActivationPendingEb");
void __real_PalHijack(void *thread) __asm__("__real__Z9PalHijackP6Thread");
void __wrap_PalHijack(void *thread) __asm__("__wrap__Z9PalHijackP6Thread");

#define THREAD_OS_HANDLE_OFFSET 136   /* Thread::m_hOSThread (pthread_t), from PalHijack's disassembly */
#define PTHREAD_TID_OFFSET 32         /* musl aarch64 (TLS_ABOVE_TP): self, prev, next, sysinfo, tid */

void __wrap_PalHijack(void *thread) {
    /* Only the suspending GC thread calls this (under the thread store lock): one at a time. */
    static ucontext_t uc;
    static uint64_t regs[34];
    static unsigned long done, skipped;
    pthread_t pt = thread ? *(pthread_t *)((char *)thread + THREAD_OS_HANDLE_OFFSET) : 0;
    int tid = pt ? *(volatile int *)((char *)pt + PTHREAD_TID_OFFSET) : 0;
    uint64_t t0 = hbxl_gc_ticks ? hbxl_gc_ticks() : 0;
    void *nn = tid > 0 ? hbxl_thread_pause(tid, regs) : 0;

    if (!nn) {
        if (++skipped == 1) hbxl_logf("hbxl-gc: activation of tid %d skipped (not an hbxl thread)", (int64_t)tid);
        return;
    }

    memset(&uc, 0, sizeof uc);
    for (int i = 0; i < 31; i++) uc.uc_mcontext.regs[i] = regs[i];
    uc.uc_mcontext.sp = regs[31];
    uc.uc_mcontext.pc = regs[32];
    uc.uc_mcontext.pstate = regs[33];

    rt_SetActivationPending(thread, 1);
    rt_HijackCallback(&uc, thread, 0);
    rt_SetActivationPending(thread, 0);
    hbxl_thread_resume_nn(nn);
    if (hbxl_gc_note_hijack && hbxl_gc_ticks) hbxl_gc_note_hijack(tid, hbxl_gc_ticks() - t0);

    done++;
    if (done == 1 || (done & (done - 1)) == 0)
        hbxl_logf("hbxl-gc: emulated activation #%d (tid %d, pc %x)", (int64_t)done, (int64_t)tid, regs[32]);
}

int pthread_kill(pthread_t thread, int sig) {
    static volatile int logged;
    (void)thread;
    if (sig != 0) {
        static const char msg[] = "hbxl-dotnet-shim: pthread_kill suppressed\n";
        shim_log_once(&logged, msg, sizeof msg - 1);
    }
    return 0;
}

int mlock(const void *addr, size_t len) {
    (void)addr; (void)len;
    return 0;
}
