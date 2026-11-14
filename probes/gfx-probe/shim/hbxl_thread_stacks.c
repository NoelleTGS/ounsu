/*
 * hbxl_thread_stacks.c - give every thread a stack from the SDK heap.
 *
 * Link with -Wl,--wrap=pthread_create.
 *
 * hbxl's mmap serves most memory from the launcher's Vulkan-backed pool once the
 * first 64 MiB SDK-heap chunk is used up, so later threads (osu-framework's draw
 * thread, .NET's worker threads) get stacks there.  The main thread's stack is on
 * the SDK heap (WiiCompiled's main-stack patch).  WiiCompiled found that console
 * drivers/services misbehave when callers' stacks live in the Vulkan-backed pool
 * (vkCreateGraphicsPipelines failing on aurora's worker threads), so every thread
 * gets an SDK-heap stack here, like the main thread.  If the SDK heap is exhausted
 * the thread falls back to musl's own stack allocation.
 *
 * These stacks are never freed (musl doesn't free caller-provided stacks); threads
 * are long-lived here, and the count is logged.
 *
 * Size: HBXL_THREAD_STACK_KIB (hbxl-env), default 1024.  The stacks sit back to back
 * in the SDK heap with NO guard pages, so an overflow silently corrupts the
 * neighbouring thread's stack (random hangs/deaths elsewhere).  Overflow monitor:
 * every stack is painted with a pattern before the thread starts; a checker thread
 * rescans them every 10 s and logs each thread's high-water mark once it passes
 * 384 KiB (and again per extra 64 KiB), and "OVERFLOW" if the lowest bytes were
 * overwritten.
 */
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

void *hbxl_sdk_memalign(size_t align, size_t size);      /* hbxl runtime */

#define DEFAULT_STACK ((size_t)1 << 20)
#define PAINT 0x5a5a5a5a5a5a5a5aull
#define MAX_STACKS 256

int __real_pthread_create(pthread_t *restrict thread, const pthread_attr_t *restrict attr,
                          void *(*start)(void *), void *restrict arg);

static void tlog(const char *fmt, ...) {
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 2) n = sizeof buf - 2;
    buf[n++] = '\n';
    (void)!write(2, buf, (size_t)n);
}

static struct {
    uint64_t *base;           /* lowest address: overflows end here */
    size_t size;
    size_t logged_peak;
    int overflow_logged;
    volatile int tid;         /* hbxl tid using it; -1 while being handed out */
} g_stacks[MAX_STACKS];
static volatile unsigned g_nstacks;
static volatile int g_stacks_lock;

int hbxl_thread_state(int tid);  /* hbxl runtime: 0 unknown/reaped, 1 running, 2 finished, 3 starting */

/* Reuse: .NET's thread pool keeps retiring and creating threads, and stacks used to be leaked
 * (musl doesn't free caller-provided stacks): 107 creations exhausted the SDK heap and the next
 * Thread.Start threw OutOfMemoryException (hardware 2026-11-08). A stack is free again once its
 * thread has finished: hbxl's exit path runs on the thread's own boot stack, so state 2
 * (finished) or 0 (reaped) means nothing uses it.  hbxl tids are never reused. */
static int take_free_stack(size_t size) {
    int found = -1;
    unsigned n = __atomic_load_n(&g_nstacks, __ATOMIC_ACQUIRE);
    while (__atomic_exchange_n(&g_stacks_lock, 1, __ATOMIC_ACQUIRE)) sched_yield();
    for (unsigned i = 0; i < n && found < 0; i++) {
        int tid = g_stacks[i].tid;
        if (g_stacks[i].size == size && tid > 0) {
            int s = hbxl_thread_state(tid);
            if (s == 0 || s == 2) {
                g_stacks[i].tid = -1;
                found = (int)i;
            }
        }
    }
    __atomic_store_n(&g_stacks_lock, 0, __ATOMIC_RELEASE);
    return found;
}

static size_t stack_cap(void) {
    static size_t cap;
    if (!cap) {
        const char *e = getenv("HBXL_THREAD_STACK_KIB");
        long kib = e ? strtol(e, 0, 10) : 0;
        cap = kib >= 256 && kib <= 16384 ? (size_t)kib << 10 : DEFAULT_STACK;
        tlog("hbxl-stacks: thread stack size %zu KiB%s", cap >> 10, e ? " (HBXL_THREAD_STACK_KIB)" : "");
    }
    return cap;
}

static void *checker(void *arg) {
    (void)arg;
    for (;;) {
        struct timespec ts = { 10, 0 };
        nanosleep(&ts, 0);
        unsigned n = __atomic_load_n(&g_nstacks, __ATOMIC_ACQUIRE);
        for (unsigned i = 0; i < n; i++) {
            uint64_t *w = g_stacks[i].base;
            size_t words = g_stacks[i].size / 8, k = 0;
            while (k < words && w[k] == PAINT) k++;
            size_t peak = (words - k) * 8;
            if (k == 0 && !g_stacks[i].overflow_logged) {
                g_stacks[i].overflow_logged = 1;
                tlog("hbxl-stacks: OVERFLOW: thread %u used its whole %zu KiB stack (the neighbouring stack below it is corrupted)",
                     i + 1, g_stacks[i].size >> 10);
            } else if (peak >= (384u << 10) && peak >= g_stacks[i].logged_peak + (64u << 10)) {
                g_stacks[i].logged_peak = peak;
                tlog("hbxl-stacks: thread %u peak %zu KiB of %zu KiB", i + 1, peak >> 10, g_stacks[i].size >> 10);
            }
        }
    }
    return 0;
}

/* Diagnostics for hang dumps: musl's thread-list lock word (= owner tid, 0 free) and malloc lock word. */
extern volatile int __thread_list_lock;
extern volatile int __malloc_lock[];
int hbxl_dbg_tl_lock(void);
int hbxl_dbg_malloc_lock(void);
int hbxl_dbg_tl_lock(void) { return __thread_list_lock; }
int hbxl_dbg_malloc_lock(void) { return __malloc_lock[0]; }

int __wrap_pthread_create(pthread_t *restrict thread, const pthread_attr_t *restrict attr,
                          void *(*start)(void *), void *restrict arg);
int __wrap_pthread_create(pthread_t *restrict thread, const pthread_attr_t *restrict attr,
                          void *(*start)(void *), void *restrict arg) {
    static volatile unsigned count;
    static volatile size_t total;
    static int checker_started;
    pthread_attr_t a;
    void *own = 0;
    size_t own_size = 0, size = 0, cap = stack_cap();

    if (!__atomic_exchange_n(&checker_started, 1, __ATOMIC_RELAXED)) {
        pthread_t t;
        if (__real_pthread_create(&t, 0, checker, 0) == 0) pthread_detach(t);
    }

    if (attr) a = *attr; else pthread_attr_init(&a);
    /* Leave threads that already bring their own stack alone. */
    if (pthread_attr_getstack(&a, &own, &own_size) == 0 && own)
        return __real_pthread_create(thread, attr, start, arg);

    /* .NET asks for 8 MiB per thread; the SDK heap (~240 MiB, also needed by AudioOut and
     * sockets) ran out after 19 threads.  So cap it (default 1 MiB, see the header). */
    pthread_attr_getstacksize(&a, &size);
    if (size > cap) size = cap;
    if (size < (256u << 10)) size = 256u << 10;
    size = (size + 65535u) & ~(size_t)65535u;

    static volatile unsigned reused;
    void *stack;
    int slot = take_free_stack(size);
    unsigned n;
    if (slot >= 0) {
        stack = g_stacks[slot].base;
        g_stacks[slot].logged_peak = 0;
        g_stacks[slot].overflow_logged = 0;
        n = __atomic_add_fetch(&reused, 1, __ATOMIC_RELAXED);
        if (n == 1 || (n & (n - 1)) == 0)
            tlog("hbxl-stacks: reusing a finished thread's stack (%u reuses so far, %u stacks allocated)", n, count);
    } else {
        stack = hbxl_sdk_memalign(4096, size);
        if (!stack) {
            tlog("hbxl-stacks: SDK heap exhausted (%u stacks, %zu KiB so far); thread %u uses the pool",
                 count, total >> 10, count + 1);
            return __real_pthread_create(thread, attr, start, arg);
        }
        n = __atomic_add_fetch(&count, 1, __ATOMIC_RELAXED);
        size_t t = __atomic_add_fetch(&total, size, __ATOMIC_RELAXED);
        if (n <= MAX_STACKS) {
            slot = (int)n - 1;
            g_stacks[slot].base = stack;
            g_stacks[slot].size = size;
            g_stacks[slot].tid = -1;
            __atomic_store_n(&g_nstacks, n, __ATOMIC_RELEASE);
        }
        tlog("hbxl-stacks: thread %u stack %zu KiB on the SDK heap at %p (total %zu KiB)",
             n, size >> 10, stack, t >> 10);
    }
    for (size_t k = 0; k < size / 8; k++) ((uint64_t *)stack)[k] = PAINT;
    pthread_attr_setstack(&a, stack, size);
    int r = __real_pthread_create(thread, &a, start, arg);
    if (slot >= 0) {
        /* Owner's hbxl tid (musl aarch64 struct pthread: self, prev, next, sysinfo, tid). On failure
         * the slot is free again: a tid that never existed reads as state 0 (reaped). */
        int tid = r == 0 ? *(volatile int *)((char *)*thread + 32) : 0x7fffffff;
        __atomic_store_n(&g_stacks[slot].tid, tid > 0 ? tid : 0x7fffffff, __ATOMIC_RELEASE);
    }
    if (r != 0) tlog("hbxl-stacks: pthread_create failed (%d)", r);
    return r;
}
