/*
 * Threads and futexes for the island.
 *
 * clone: each island thread is a libnx thread. The libnx thread starts on its own small stack (host_entry), widens its
 * core mask (Horizon otherwise keeps every new thread on the process's default core; Dusklight measured that), then
 * switches to the stack and thread pointer musl prepared (lx_thread_enter) and runs musl's start routine there. Its
 * exit (SYS_exit, or __unmapself) longjmps back to host_entry, off musl's stack, and only then does what the Linux
 * kernel does after a thread has left user space: unmap the stack (detached threads) and clear + wake the
 * CLONE_CHILD_CLEARTID word. musl 1.2 points that word at its thread-list lock, which the exiting thread still holds,
 * so the clear must happen after the thread is done with its stack.
 *
 * Finished threads are closed (threadWaitForExit + threadClose) at the next clone and at shutdown: libnx keeps a
 * thread's stack mirrored until then, and Dusklight found that a stack still mirrored at exit locks heap pages the
 * next program (hbmenu) needs.
 *
 * futex: svcWaitForAddress / svcSignalToAddress. Horizon has no requeue: a requeue wakes everyone instead, which is
 * allowed (waiters re-check their condition and wait again).
 */
#include <switch.h>

#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

#include "lx_host.h"

#define CLONE_SETTLS 0x80000
#define CLONE_PARENT_SETTID 0x100000
#define CLONE_CHILD_CLEARTID 0x200000

#define HOST_STACK (64u << 10)

struct lx_thread {
    Thread t;
    int tid;
    int (*fn)(void *);
    void *arg;
    void *stack;
    void *tls;
    int32_t *ctid;
    jmp_buf exit_jb;
    uintptr_t unmap_base;
    size_t unmap_size;
    volatile int done;
    int32_t *volatile waiting_on; /* futex word this thread waits on (for the wake-up at exit) */
    int32_t sleep_word;           /* sleeps wait on this, so the exit can cut them short */
    struct lx_thread *next;
};

void lx_thread_enter(int (*fn)(void *), void *arg, void *sp, void *tls) __attribute__((noreturn)); /* asm */

static __thread struct lx_thread *t_self; /* NULL on the main thread (tid 1) */
static struct lx_thread *g_threads;
static Mutex g_threads_lock;
static int g_next_tid = 2;
static volatile int g_exit_status = -1;
static volatile int g_exiting;

uint32_t lx_process_core_mask(void) {
    static uint32_t mask;
    if (!mask) {
        u64 info = 0;
        mask = R_SUCCEEDED(svcGetInfo(&info, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) && info ? (uint32_t)info : 1u;
    }
    return mask;
}

int lx_gettid(void) {
    return t_self ? t_self->tid : 1;
}

int lx_exiting(void) {
    return g_exiting;
}

/* Once the program has exited, any island thread that enters the host (a syscall, a wait, a sleep) ends there, never
   returning to island code. */
void lx_exit_check(void) {
    if (g_exiting && t_self) lx_thread_exit(0, 0, 0);
}

void lx_sleep_ns(int64_t ns) {
    if (ns <= 0) {
        svcSleepThread(0);
    } else if (t_self) {
        svcWaitForAddress(&t_self->sleep_word, ArbitrationType_WaitIfEqual, 0, ns);
    } else {
        svcSleepThread(ns);
    }
    lx_exit_check();
}

/* Called (on musl's stack) when the start routine returns instead of calling SYS_exit. */
void lx_thread_fn_returned(long code) __attribute__((noreturn));
void lx_thread_fn_returned(long code) {
    lx_thread_exit((int)code, 0, 0);
}

static void host_entry(void *p) {
    struct lx_thread *t = p;
    t_self = t;
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, -1 /* no preferred core */, lx_process_core_mask());
    if (!setjmp(t->exit_jb)) lx_thread_enter(t->fn, t->arg, t->stack, t->tls);
    /* Back on the host stack: the thread is done with musl's memory. */
    if (t->unmap_base) lx_munmap(t->unmap_base, t->unmap_size);
    if (t->ctid) {
        __atomic_store_n(t->ctid, 0, __ATOMIC_SEQ_CST);
        svcSignalToAddress(t->ctid, SignalType_Signal, 0, -1);
    }
    __atomic_store_n(&t->done, 1, __ATOMIC_RELEASE);
}

void lx_thread_exit(int code, uintptr_t unmap_base, size_t unmap_size) {
    struct lx_thread *t = t_self;
    (void)code;
    if (!t) {
        /* The island handles the main thread's exit itself; getting here is a bug. Park it. */
        lx_logf("lx: BUG: thread exit on the main thread");
        for (;;) svcSleepThread(1000000000ll);
    }
    t->unmap_base = unmap_base;
    t->unmap_size = unmap_size;
    longjmp(t->exit_jb, 1);
}

long lx_exit_group(int code) {
    /* From a thread other than the main one (the island returns to the host itself on the main thread). Not handled
       yet: the main thread would have to notice and return. Record it and end this thread. */
    g_exit_status = code & 0xff;
    lx_logf("lx: exit_group(%d) from tid %d: not supported yet, ending only this thread", code, lx_gettid());
    lx_thread_exit(code, 0, 0);
}

/* Closes finished threads. With wait_ms > 0, first waits up to that long for all of them to finish. Returns how many
   are still running. */
static int reap(int wait_ms) {
    int live = 0;
    for (int waited = 0;; waited += 10) {
        live = 0;
        mutexLock(&g_threads_lock);
        for (struct lx_thread **pp = &g_threads; *pp;) {
            struct lx_thread *t = *pp;
            if (__atomic_load_n(&t->done, __ATOMIC_ACQUIRE)) {
                threadWaitForExit(&t->t);
                threadClose(&t->t);
                *pp = t->next;
                free(t);
            } else {
                live++;
                pp = &t->next;
            }
        }
        mutexUnlock(&g_threads_lock);
        if (!live || waited >= wait_ms) return live;
        svcSleepThread(10000000ll);
    }
}

long lx_clone(int (*fn)(void *), void *stack, long flags, void *arg, int *ptid, void *tls, int *ctid) {
    s32 prio = 0x2c;
    Result rc;

    reap(0);
    struct lx_thread *t = calloc(1, sizeof *t);
    if (!t) return -LX_EAGAIN;
    t->tid = __atomic_fetch_add(&g_next_tid, 1, __ATOMIC_RELAXED);
    t->fn = fn;
    t->arg = arg;
    t->stack = stack;
    if (flags & CLONE_SETTLS) {
        t->tls = tls;
    } else {
        uintptr_t tp;
        __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
        t->tls = (void *)tp;
    }
    if (flags & CLONE_CHILD_CLEARTID) t->ctid = (int32_t *)ctid;
    if ((flags & CLONE_PARENT_SETTID) && ptid) *ptid = t->tid;

    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    rc = threadCreate(&t->t, host_entry, t, NULL, HOST_STACK, prio, -2);
    if (R_FAILED(rc)) {
        lx_logf("lx: threadCreate failed: 0x%x", rc);
        free(t);
        return -LX_EAGAIN;
    }
    mutexLock(&g_threads_lock);
    t->next = g_threads;
    g_threads = t;
    mutexUnlock(&g_threads_lock);
    rc = threadStart(&t->t);
    if (R_FAILED(rc)) {
        lx_logf("lx: threadStart failed: 0x%x", rc);
        __atomic_store_n(&t->done, 1, __ATOMIC_RELEASE); /* reaped (closed) at the next clone */
        return -LX_EAGAIN;
    }
    return t->tid;
}

/* After the program's exit: end every island thread. Threads blocked in the host (futex waits, sleeps, pipe reads,
   polls) are woken and end inside the host; a thread still running island code after 2 s is paused for good (a
   thread can't be ended from outside), so it can't run into the next program's memory. Returns how many were
   paused. */
int lx_threads_shutdown(void) {
    g_exiting = 1;
    for (int waited = 0; waited < 2000; waited += 20) {
        int live = 0;
        mutexLock(&g_threads_lock);
        for (struct lx_thread *t = g_threads; t; t = t->next) {
            if (__atomic_load_n(&t->done, __ATOMIC_ACQUIRE)) continue;
            live++;
            int32_t *w = t->waiting_on;
            if (w) svcSignalToAddress(w, SignalType_Signal, 0, -1);
            __atomic_store_n(&t->sleep_word, 1, __ATOMIC_SEQ_CST);
            svcSignalToAddress(&t->sleep_word, SignalType_Signal, 0, -1);
        }
        mutexUnlock(&g_threads_lock);
        if (!live) break;
        svcSleepThread(20000000ll);
    }
    int ended = 0, paused = 0;
    mutexLock(&g_threads_lock);
    for (struct lx_thread *t = g_threads; t; t = t->next) {
        if (__atomic_load_n(&t->done, __ATOMIC_ACQUIRE)) {
            ended++;
            continue;
        }
        svcSetThreadActivity(t->t.handle, ThreadActivity_Paused);
        lx_logf("lx: tid %d still running island code at exit: paused", t->tid);
        paused++;
    }
    mutexUnlock(&g_threads_lock);
    reap(0);
    lx_logf("lx: exit: %d thread(s) ended, %d paused", ended, paused);
    return paused;
}

long lx_futex(int32_t *uaddr, int op, int32_t val, const struct lx_timespec *timeout, int32_t *uaddr2, int32_t val3) {
    int cmd = op & 0x7f; /* without FUTEX_PRIVATE_FLAG (128) and FUTEX_CLOCK_REALTIME (256) */
    (void)uaddr2;
    switch (cmd) {
    case 0:   /* FUTEX_WAIT: relative timeout */
    case 9: { /* FUTEX_WAIT_BITSET: absolute timeout */
        s64 ns = -1;
        if (timeout) {
            ns = timeout->sec * 1000000000ll + timeout->nsec;
            if (cmd == 9) {
                struct lx_timespec now;
                lx_syscall(113, (op & 256) ? 0 : 1, (long)&now, 0, 0, 0, 0);
                ns -= now.sec * 1000000000ll + now.nsec;
            }
            if (ns < 0) ns = 0;
        }
        if (t_self) t_self->waiting_on = uaddr;
        lx_exit_check();
        Result rc = svcWaitForAddress(uaddr, ArbitrationType_WaitIfEqual, val, ns);
        if (t_self) t_self->waiting_on = NULL;
        lx_exit_check();
        if (R_SUCCEEDED(rc)) return 0;
        if (rc == KERNELRESULT(TimedOut)) return -LX_ETIMEDOUT;
        if (rc == KERNELRESULT(InvalidState)) return -LX_EAGAIN; /* the value had already changed */
        if (rc == KERNELRESULT(Cancelled)) return -LX_EINTR;
        lx_logf("lx: futex wait on %p failed: 0x%x", (void *)uaddr, rc);
        return -LX_EINVAL;
    }
    case 1:  /* FUTEX_WAKE */
    case 10: /* FUTEX_WAKE_BITSET */
        svcSignalToAddress(uaddr, SignalType_Signal, 0, val <= 0 || val == 0x7fffffff ? -1 : val);
        return 0;
    case 3: /* FUTEX_REQUEUE */
    case 4: /* FUTEX_CMP_REQUEUE */
        if (cmd == 4 && __atomic_load_n(uaddr, __ATOMIC_SEQ_CST) != val3) return -LX_EAGAIN;
        svcSignalToAddress(uaddr, SignalType_Signal, 0, -1);
        return 0;
    }
    lx_logf("lx: futex op %d not implemented", op);
    return -LX_ENOSYS;
}
