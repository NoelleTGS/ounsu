/*
 * What the .NET NativeAOT runtime needs on top of musl in the island. Linked into every .NET island ahead of libc.a,
 * with tools/aot-island.py adding the --wrap flags named below.
 *
 * Delegate thunks (lx_thunks.S): -Wl,--wrap=RhAllocateThunksMapping,--wrap=RhpGetThunkDataBlockAddress,
 * --wrap=RhpGetThunkStubsBlockAddress. The runtime's own versions assume the data block is 0x8000 after the code
 * block (pages mirror each other); ours put the data where the linker did. One block = 2040 thunks (one per
 * marshalled delegate instance); a second request fails with E_OUTOFMEMORY, which the managed side turns into an
 * OutOfMemoryException rather than a silent crash.
 *
 * pthread_kill: NativeAOT suspends threads for a GC with an activation signal. There are no signals here, so the
 * signal is reported as sent and the GC waits for each thread to reach a safepoint by itself (osu-framework's
 * ThrottledFrameClock patch makes sure busy game threads pass one).
 *
 * mlock: the FlushProcessWriteBuffers fallback at start-up uses it; memory is always resident here.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#define PAGE 4096u
#define THUNK_BLOCK (8u * PAGE)

extern char lx_thunks_start[];
extern char lx_thunk_data[];

static void log_line(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    (void)!write(2, s, n);
}

int32_t __wrap_RhAllocateThunksMapping(void **out) {
    static volatile int taken;
    if (__atomic_exchange_n(&taken, 1, __ATOMIC_ACQ_REL)) {
        log_line("lx-dotnet: a second delegate thunk block was requested; none left (E_OUTOFMEMORY)\n");
        return (int32_t)0x8007000e;
    }
    *out = lx_thunks_start;
    log_line("lx-dotnet: delegate thunk block handed out (prebuilt, 2040 thunks)\n");
    return 0;
}

void *__real_RhpGetThunkDataBlockAddress(void *thunk);
void *__real_RhpGetThunkStubsBlockAddress(void *data);

void *__wrap_RhpGetThunkDataBlockAddress(void *thunk) {
    uintptr_t a = (uintptr_t)thunk;
    if (a >= (uintptr_t)lx_thunks_start && a < (uintptr_t)lx_thunks_start + THUNK_BLOCK)
        return lx_thunk_data + ((a & ~(uintptr_t)(PAGE - 1)) - (uintptr_t)lx_thunks_start);
    return __real_RhpGetThunkDataBlockAddress(thunk);
}

void *__wrap_RhpGetThunkStubsBlockAddress(void *data) {
    uintptr_t a = (uintptr_t)data;
    if (a >= (uintptr_t)lx_thunk_data && a < (uintptr_t)lx_thunk_data + THUNK_BLOCK)
        return lx_thunks_start + ((a & ~(uintptr_t)(PAGE - 1)) - (uintptr_t)lx_thunk_data);
    return __real_RhpGetThunkStubsBlockAddress(data);
}

int pthread_kill(pthread_t thread, int sig) {
    static volatile int logged;
    (void)thread;
    if (sig != 0 && !__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED))
        log_line("lx-dotnet: pthread_kill suppressed (no signals; GC waits for safepoints)\n");
    return 0;
}

int mlock(const void *addr, size_t len) {
    (void)addr;
    (void)len;
    return 0;
}
