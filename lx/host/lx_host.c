/*
 * The host program: a libounce .nro that runs one musl island (lx_island_main) and answers its Linux syscalls.
 *
 * Start-up: sockets and nxlink (stdout/stderr go to `nxlink -s` on the PC when the app was sent that way), a report of
 * the memory the process has, then the island on its own 8 MiB main stack. Its exit status is logged and becomes
 * main's return value, after which the loader goes back to hbmenu.
 */
#include <switch.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lx_host.h"

#define LX_MAIN_STACK (8u << 20)

/* Linker symbols from libounce's switch.ld. */
extern char __code_start[], __rodata_start[], __data_start[], __bss_end__[];
extern char __tdata_lma[], __tdata_lma_end[], __tls_start[], __tls_end[];
extern const uint64_t __tls_align[];
extern char __eh_frame_hdr_start[], __eh_frame_hdr_end[];

void lx_log_raw(const char *text, size_t len) {
    fwrite(text, 1, len, stderr);
}

void lx_logf(const char *format, ...) {
    char line[512];
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(line, sizeof line - 1, format, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n > sizeof line - 2) n = sizeof line - 2;
    line[n++] = '\n';
    lx_log_raw(line, (size_t)n);
}

static void island_log(const char *text, size_t len) {
    lx_log_raw(text, len);
    lx_log_raw("\n", 1);
}

/* Phase 0 question: how much memory does a homebrew app get under kaiHEN? */
static void report_memory(void) {
    u64 total = 0, used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    lx_logf("lx: process memory: total %lu MiB, used %lu MiB", (unsigned long)(total >> 20),
            (unsigned long)(used >> 20));

    /* Largest single malloc, by binary search in MiB (the GC wants one big reservation). */
    size_t lo = 0, hi = 16384;
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        void *p = malloc(mid << 20);
        if (p) {
            free(p);
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    lx_logf("lx: largest malloc: %zu MiB", lo);
}

int main(int argc, char **argv) {
    static char *const default_env[] = { "HOME=/", "PWD=/", "LANG=C.UTF-8", NULL };
    static uint8_t random16[16];
    struct lx_host host;
    int code;

    socketInitializeDefault();
    int sock = nxlinkStdio();
    setvbuf(stderr, NULL, _IONBF, 0);
    lx_logf("lx: host up (nxlink %s), core mask 0x%x", sock >= 0 ? "connected" : "not used", lx_process_core_mask());
    report_memory();

    memset(&host, 0, sizeof host);
    host.version = LX_ABI_VERSION;
    host.size = sizeof host;
    host.syscall = lx_syscall;
    host.clone = lx_clone;
    host.log = island_log;
    host.text_start = (uintptr_t)__code_start;
    host.text_end = (uintptr_t)__rodata_start;
    host.data_start = (uintptr_t)__data_start;
    host.data_end = (uintptr_t)__bss_end__;
    host.tls_image = (uintptr_t)__tdata_lma;
    host.tls_filesz = (uintptr_t)(__tdata_lma_end - __tdata_lma);
    host.tls_memsz = (uintptr_t)(__tls_end - __tls_start);
    host.tls_align = __tls_align[0];
    host.eh_frame_hdr = (uintptr_t)__eh_frame_hdr_start;
    host.eh_frame_hdr_size = (uintptr_t)(__eh_frame_hdr_end - __eh_frame_hdr_start);
    host.argc = argc > 0 ? argc : 1;
    host.argv = argc > 0 ? argv : (char *const[]){ "lx-app", NULL };
    host.envp = default_env;
    randomGet(random16, sizeof random16);
    host.random16 = random16;

    lx_logf("lx: TLS image 0x%lx (%lu bytes, %lu in memory, align %lu), eh_frame_hdr %lu bytes",
            (unsigned long)host.tls_image, (unsigned long)host.tls_filesz, (unsigned long)host.tls_memsz,
            (unsigned long)host.tls_align, (unsigned long)host.eh_frame_hdr_size);
    if (host.tls_align && host.tls_image % host.tls_align)
        lx_logf("lx: WARNING: TLS image not aligned to its alignment: musl's TLS offsets would be wrong");

    void *stack = aligned_alloc(4096, LX_MAIN_STACK);
    if (!stack) {
        lx_logf("lx: no memory for the main stack");
        return 1;
    }
    lx_mem_set_main_stack((uintptr_t)stack, (uintptr_t)stack + LX_MAIN_STACK);

    uint64_t t0 = lx_monotonic_ns();
    code = lx_island_main(&host, stack, LX_MAIN_STACK);
    lx_logf("lx: program exited with status %d after %lu ms (%zu KiB still mapped)", code,
            (unsigned long)((lx_monotonic_ns() - t0) / 1000000), lx_mem_mapped() >> 10);

    if (lx_threads_shutdown() == 0)
        free(stack); /* a thread still running may be on memory we'd free; leak it then */

    lx_logf("lx: back to hbmenu");
    socketExit();
    return code;
}
