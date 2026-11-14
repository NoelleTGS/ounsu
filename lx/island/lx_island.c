/*
 * The island runtime (built with the musl toolchain, linked into the island ahead of libc.a).
 *
 * The toolchain's musl is patched (musl-hbx-syscall-hooks.diff, from the HBLauncher era) so that every syscall calls
 * __hbx_syscall and the assembly that would issue `svc 0` itself (__clone, __syscall_cp_asm, __unmapself, vfork) is
 * weak. This file supplies all of them and forwards to the host through struct lx_host. It also builds the
 * argv/envp/auxv vector musl's __libc_start_main expects, with synthetic program headers for TLS and unwinding.
 */
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "lx_abi.h"

__attribute__((noreturn)) void lx_island_start(const struct lx_host *host, void *stack_lo, size_t stack_size);
__attribute__((visibility("hidden"), noreturn)) void lx_island_return(long code);
__attribute__((visibility("hidden"), noreturn)) void lx_call_on_stack(void (*fn)(void *), void *arg, void *sp);

int main(int, char **, char **);
int __libc_start_main(int (*)(int, char **, char **), int, char **, void (*)(void), void (*)(void), void (*)(void));

static const struct lx_host *g_host;

/* C++ code registers its static destructors against this (normally from crtbegin.o, which the island doesn't use). */
void *__dso_handle = &__dso_handle;

#define SYS_exit 93
#define SYS_exit_group 94
#define SYS_gettid 178

static void island_log(const char *text) {
    size_t n = 0;
    while (text[n]) n++;
    g_host->log(text, n);
}

/* ---- syscalls ---------------------------------------------------------------------------------------------- */

long __hbx_syscall(long n, long a, long b, long c, long d, long e, long f) {
    /* The program ends when its main thread (tid 1) exits: go back to the host from here, whatever stack this is. */
    if ((n == SYS_exit_group || n == SYS_exit) && g_host->syscall(SYS_gettid, 0, 0, 0, 0, 0, 0) == 1)
        lx_island_return(a & 0xff);
    return g_host->syscall(n, a, b, c, d, e, f);
}

/* Cancellation points: no thread cancellation here, so a plain syscall. */
long __syscall_cp_asm(volatile int *cancel, long n, long a, long b, long c, long d, long e, long f) {
    (void)cancel;
    return __hbx_syscall(n, a, b, c, d, e, f);
}

int __clone(int (*fn)(void *), void *stack, int flags, void *arg, ...) {
    va_list ap;
    va_start(ap, arg);
    int *ptid = va_arg(ap, int *);
    void *tls = va_arg(ap, void *);
    int *ctid = va_arg(ap, int *);
    va_end(ap);
    return (int)g_host->clone(fn, stack, flags, arg, ptid, tls, ctid);
}

__attribute__((noreturn)) void __unmapself(void *base, size_t size) {
    g_host->syscall(LX_SYS_UNMAPSELF, (long)base, (long)size, 0, 0, 0, 0);
    for (;;) {
    }
}

int vfork(void) {
    errno = ENOSYS;
    return -1;
}

/* ---- start-up ---------------------------------------------------------------------------------------------- */

struct phdr64 {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

enum { PT_LOAD = 1, PT_TLS = 7, PT_GNU_EH_FRAME = 0x6474e550, PT_GNU_STACK = 0x6474e551 };
enum { AT_NULL = 0, AT_PHDR = 3, AT_PHENT = 4, AT_PHNUM = 5, AT_PAGESZ = 6, AT_UID = 11, AT_EUID = 12, AT_GID = 13,
       AT_EGID = 14, AT_HWCAP = 16, AT_SECURE = 23, AT_RANDOM = 25, AT_HWCAP2 = 26 };

/* Cortex-A78C (Armv8.2): fp asimd aes pmull sha1 sha2 crc32 atomics fphp asimdhp asimdrdm lrcpc dcpop asimddp. */
#define LX_HWCAP ((1ul << 0) | (1ul << 1) | (1ul << 3) | (1ul << 4) | (1ul << 5) | (1ul << 6) | (1ul << 7) | \
                  (1ul << 8) | (1ul << 9) | (1ul << 10) | (1ul << 12) | (1ul << 15) | (1ul << 16) | (1ul << 20))

static struct phdr64 g_phdrs[5];
static int g_argc;

static void run_libc(void *vector) {
    __libc_start_main(main, g_argc, (char **)vector, 0, 0, 0);
    lx_island_return(0); /* not reached: __libc_start_main calls exit */
}

void lx_island_start(const struct lx_host *host, void *stack_lo, size_t stack_size) {
    int np = 0, envc = 0, i, k = 0;
    long *vector;
    size_t words;

    g_host = host;
    if (host->version != LX_ABI_VERSION || host->size != sizeof *host) {
        /* Can't log through a host we don't understand; give up with a distinctive status. */
        lx_island_return(250);
    }

    g_phdrs[np++] = (struct phdr64){ .type = PT_LOAD, .flags = 5, .vaddr = host->text_start, .paddr = host->text_start,
                                     .filesz = host->text_end - host->text_start,
                                     .memsz = host->text_end - host->text_start, .align = 0x1000 };
    g_phdrs[np++] = (struct phdr64){ .type = PT_LOAD, .flags = 6, .vaddr = host->data_start, .paddr = host->data_start,
                                     .filesz = host->data_end - host->data_start,
                                     .memsz = host->data_end - host->data_start, .align = 0x1000 };
    if (host->eh_frame_hdr_size)
        g_phdrs[np++] = (struct phdr64){ .type = PT_GNU_EH_FRAME, .flags = 4, .vaddr = host->eh_frame_hdr,
                                         .paddr = host->eh_frame_hdr, .filesz = host->eh_frame_hdr_size,
                                         .memsz = host->eh_frame_hdr_size, .align = 4 };
    if (host->tls_memsz)
        g_phdrs[np++] = (struct phdr64){ .type = PT_TLS, .flags = 4, .vaddr = host->tls_image, .paddr = host->tls_image,
                                         .filesz = host->tls_filesz, .memsz = host->tls_memsz,
                                         .align = host->tls_align };
    /* musl's default thread stack size comes from PT_GNU_STACK (else 128 KiB). */
    g_phdrs[np++] = (struct phdr64){ .type = PT_GNU_STACK, .flags = 6, .memsz = 8u << 20 };

    /* argv, envp and auxv, laid out at the top of the main stack as the Linux kernel would (musl's
       pthread_getattr_np looks for the main stack's extent from there). */
    while (host->envp && host->envp[envc]) envc++;
    g_argc = host->argc;
    words = (size_t)g_argc + 1 + (size_t)envc + 1 + 2 * 14;
    vector = (long *)(((uintptr_t)stack_lo + stack_size - words * sizeof(long) - 256) & ~(uintptr_t)15);
    for (i = 0; i < g_argc; i++) vector[k++] = (long)host->argv[i];
    vector[k++] = 0;
    for (i = 0; i < envc; i++) vector[k++] = (long)host->envp[i];
    vector[k++] = 0;
#define AUX(t, v) (vector[k++] = (t), vector[k++] = (long)(v))
    AUX(AT_PHDR, g_phdrs);
    AUX(AT_PHENT, sizeof(struct phdr64));
    AUX(AT_PHNUM, np);
    AUX(AT_PAGESZ, 4096);
    AUX(AT_HWCAP, LX_HWCAP);
    AUX(AT_HWCAP2, 0);
    AUX(AT_UID, 0);
    AUX(AT_EUID, 0);
    AUX(AT_GID, 0);
    AUX(AT_EGID, 0);
    AUX(AT_SECURE, 0);
    AUX(AT_RANDOM, host->random16);
    AUX(AT_NULL, 0);
#undef AUX

    island_log("lx-island: starting the program");
    lx_call_on_stack(run_libc, vector, vector);
}

/* ---- Vulkan and dynamic loading ---------------------------------------------------------------------------- */

/* The loader entry point, as libvulkan exports it. Everything else comes from it (the host's wrapper keeps
   vkGetInstanceProcAddr/vkGetDeviceProcAddr lookups going through itself). NULL results when the host has no Vulkan. */
typedef void (*lx_vk_void_fn)(void);

lx_vk_void_fn vkGetInstanceProcAddr(void *instance, const char *name) {
    if (!g_host->vk_get_instance_proc_addr) return NULL;
    return (lx_vk_void_fn)g_host->vk_get_instance_proc_addr(instance, name);
}

/* The console's default window (NWindow *) for VkViSurfaceCreateInfoNN.window. */
void *lx_vi_window(void) {
    return g_host->vi_window;
}

/* ---- the fake libdl -----------------------------------------------------------------------------------------

   A static island has no shared libraries, but .NET and its libraries look for some by name and dlsym() their
   functions: libvulkan (SDL, Veldrid's bindings), libssl/libcrypto (System.Security.Cryptography.Native.OpenSsl),
   FFmpeg's libraries (FFmpeg.AutoGen). Each is served from what is linked in:
   - Vulkan through lx_vk_dlsym (lx/dotnet/lx_vk.c: lazy trampolines, needed because Veldrid's bindings dlsym every
     command before an instance exists); without it, straight from vkGetInstanceProcAddr (global commands only).
   - OpenSSL and FFmpeg from sorted symbol tables generated at build time (lx_ssl_syms, lx_ffmpeg_syms).
   The defaults below are weak definitions that the real ones replace in the island link. */
struct lx_dl_sym {
    const char *name;
    void *addr;
};

static int str_eq(const char *a, const char *b) {
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}

__attribute__((weak)) void *lx_vk_dlsym(const char *name) {
    return str_eq(name, "vkGetInstanceProcAddr") ? (void *)vkGetInstanceProcAddr : (void *)vkGetInstanceProcAddr(NULL, name);
}
__attribute__((weak)) const struct lx_dl_sym lx_ssl_syms[1];
__attribute__((weak)) const unsigned lx_ssl_count;
__attribute__((weak)) const struct lx_dl_sym lx_ffmpeg_syms[1];
__attribute__((weak)) const unsigned lx_ffmpeg_count;

static int g_dl_handles[4]; /* addresses only: 0 the program, 1 Vulkan, 2 OpenSSL, 3 FFmpeg */
static const char *g_dlerror;

static int contains(const char *s, const char *sub) {
    for (; *s; s++) {
        const char *a = s, *b = sub;
        while (*a && *b && *a == *b) a++, b++;
        if (!*b) return 1;
    }
    return 0;
}

static void *table_lookup(const struct lx_dl_sym *tab, unsigned n, const char *name) {
    unsigned lo = 0, hi = n; /* sorted by strcmp */
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        const char *a = name, *b = tab[mid].name;
        while (*a && *a == *b) a++, b++;
        int c = (unsigned char)*a - (unsigned char)*b;
        if (!c) return tab[mid].addr;
        if (c < 0) hi = mid;
        else lo = mid + 1;
    }
    return NULL;
}

void *dlopen(const char *file, int mode) {
    (void)mode;
    g_dlerror = NULL;
    if (!file) return &g_dl_handles[0];
    if (contains(file, "vulkan") && g_host->vk_get_instance_proc_addr) return &g_dl_handles[1];
    if ((contains(file, "libssl") || contains(file, "libcrypto")) && lx_ssl_count) return &g_dl_handles[2];
    if ((contains(file, "libavcodec") || contains(file, "libavformat") || contains(file, "libavutil") ||
         contains(file, "libswresample") || contains(file, "libswscale")) && lx_ffmpeg_count)
        return &g_dl_handles[3];
    g_dlerror = "lx: dlopen: no such library in the island";
    return NULL;
}

void *dlsym(void *handle, const char *name) {
    void *p = NULL;
    if (handle == &g_dl_handles[1]) p = lx_vk_dlsym(name);
    else if (handle == &g_dl_handles[2]) p = table_lookup(lx_ssl_syms, lx_ssl_count, name);
    else if (handle == &g_dl_handles[3]) p = table_lookup(lx_ffmpeg_syms, lx_ffmpeg_count, name);
    g_dlerror = p ? NULL : "lx: dlsym: symbol not found";
    return p;
}

int dlclose(void *handle) {
    (void)handle;
    return 0;
}

char *dlerror(void) {
    const char *e = g_dlerror;
    g_dlerror = NULL;
    return (char *)e;
}
