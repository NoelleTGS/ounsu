/*
 * Linux syscalls of the island, answered with libounce. Numbers, structs and errno values are Linux aarch64's.
 * Ported in spirit from the HBLauncher runtime (sdk/linux/runtime.c), whose notes say which calls .NET, musl and
 * the libraries need and why.
 *
 * Phase 1 covers what musl's start-up, stdio, malloc and pthreads use. Files (openat & co.) come next; until then
 * only fds 0-2 exist.
 */
#include <switch.h>

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "lx_host.h"

struct lx_iovec {
    void *base;
    size_t len;
};

struct lx_utsname {
    char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65];
};

struct lx_rlimit {
    uint64_t cur, max;
};

struct lx_winsize {
    uint16_t row, col, xpixel, ypixel;
};

uint64_t lx_monotonic_ns(void) {
    return armTicksToNs(armGetSystemTick());
}

static long sys_clock_gettime(long clock, struct lx_timespec *ts) {
    if (!ts) return -LX_EFAULT;
    if (clock == 0 /* REALTIME */ || clock == 5 /* REALTIME_COARSE */ || clock == 11 /* TAI */) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        ts->sec = tv.tv_sec;
        ts->nsec = (int64_t)tv.tv_usec * 1000;
        return 0;
    }
    /* MONOTONIC (1), PROCESS/THREAD_CPUTIME (2, 3: approximated), MONOTONIC_RAW (4), COARSE (6), BOOTTIME (7) */
    uint64_t ns = lx_monotonic_ns();
    ts->sec = (int64_t)(ns / 1000000000u);
    ts->nsec = (int64_t)(ns % 1000000000u);
    return 0;
}

static long sleep_ns(int64_t ns) {
    if (ns > 0) svcSleepThread(ns);
    return 0;
}

static long sys_clock_nanosleep(long clock, long flags, const struct lx_timespec *req) {
    if (!req) return -LX_EFAULT;
    int64_t ns = req->sec * 1000000000ll + req->nsec;
    if (flags & 1 /* TIMER_ABSTIME */) {
        struct lx_timespec now;
        sys_clock_gettime(clock, &now);
        ns -= now.sec * 1000000000ll + now.nsec;
    }
    return sleep_ns(ns);
}

static long sys_write(int fd, const void *buf, size_t len) {
    if (fd == 1 || fd == 2) {
        ssize_t n = write(fd, buf, len);
        return n < 0 ? -LX_EIO : n;
    }
    return -LX_EBADF;
}

static long sys_writev(int fd, const struct lx_iovec *iov, int count) {
    long total = 0;
    for (int i = 0; i < count; i++) {
        if (!iov[i].len) continue;
        long n = sys_write(fd, iov[i].base, iov[i].len);
        if (n < 0) return total ? total : n;
        total += n;
    }
    return total;
}

static long sys_uname(struct lx_utsname *u) {
    memset(u, 0, sizeof *u);
    strcpy(u->sysname, "Linux");
    strcpy(u->nodename, "switch2");
    strcpy(u->release, "6.1.0-lx");
    strcpy(u->version, "#1 lx (kaiHEN)");
    strcpy(u->machine, "aarch64");
    return 0;
}

static long sys_sched_getaffinity(size_t size, uint8_t *mask) {
    uint32_t cores = lx_process_core_mask();
    if (size < 8) return -LX_EINVAL;
    memset(mask, 0, size);
    memcpy(mask, &cores, sizeof cores);
    return 8;
}

static long sys_getrlimit(int resource, struct lx_rlimit *out) {
    if (!out) return 0;
    out->cur = out->max = resource == 3 /* RLIMIT_STACK */ ? 8u << 20 : ~0ull;
    return 0;
}

static void unknown_syscall(long n) {
    static uint8_t seen[512];
    if (n >= 0 && n < (long)sizeof seen) {
        if (seen[n]) return;
        seen[n] = 1;
    }
    lx_logf("lx: syscall %ld not implemented (-ENOSYS)", n);
}

long lx_syscall(long n, long a, long b, long c, long d, long e, long f) {
    switch (n) {
    /* files: only stdin/stdout/stderr for now */
    case 63: return a == 0 ? 0 : -LX_EBADF;                                              /* read */
    case 64: return sys_write((int)a, (const void *)b, (size_t)c);                        /* write */
    case 66: return sys_writev((int)a, (const struct lx_iovec *)b, (int)c);              /* writev */
    case 57: return a >= 0 && a <= 2 ? 0 : -LX_EBADF;                                    /* close */
    case 62: return -LX_ESPIPE;                                                           /* lseek */
    case 56: return -LX_ENOENT;                                                           /* openat */
    case 29:                                                                              /* ioctl */
        if ((a == 1 || a == 2) && b == 0x5413 /* TIOCGWINSZ */) {
            /* Pretend stdout/stderr are terminals: musl then line-buffers them, so log lines leave promptly. */
            struct lx_winsize *w = (struct lx_winsize *)c;
            if (w) *w = (struct lx_winsize){ 25, 120, 0, 0 };
            return 0;
        }
        return -LX_ENOTTY;

    /* memory */
    case 222: return lx_mmap((uintptr_t)a, (size_t)b, (int)c, (int)d, (int)e, f);        /* mmap */
    case 215: return lx_munmap((uintptr_t)a, (size_t)b);                                  /* munmap */
    case 216: return lx_mremap((uintptr_t)a, (size_t)b, (size_t)c, (int)d);              /* mremap */
    case 233: return lx_madvise((uintptr_t)a, (size_t)b, (int)c);                        /* madvise */
    case 226: return 0;                                                                   /* mprotect */
    case 214: return 0;                                                                   /* brk: none */

    /* threads */
    case 98: return lx_futex((int32_t *)a, (int)b, (int32_t)c, (const struct lx_timespec *)d, (int32_t *)e,
                             (int32_t)f);                                                 /* futex */
    case 178: return lx_gettid();                                                         /* gettid */
    case 96: return lx_gettid();                                                          /* set_tid_address */
    case 99: return 0;                                                                    /* set_robust_list */
    case 100:                                                                             /* get_robust_list */
        /* musl's pthread_mutexattr_setrobust probes this; Realm needs robust mutexes to be "supported". */
        if (c) *(size_t *)c = 0;
        if (b) *(void **)b = NULL;
        return 0;
    case 93: lx_thread_exit((int)a, 0, 0);                                                /* exit */
    case LX_SYS_UNMAPSELF: lx_thread_exit(0, (uintptr_t)a, (size_t)b);
    case 94: return lx_exit_group((int)a);                                                /* exit_group */
    case 124: svcSleepThread(0); return 0;                                                /* sched_yield */
    case 123: return sys_sched_getaffinity((size_t)b, (uint8_t *)c);                     /* sched_getaffinity */
    case 122: return 0;                                                                   /* sched_setaffinity */
    case 129: case 130: case 131: return 0;               /* kill/tkill/tgkill: no signals (GC activation off) */
    case 134: case 132: return 0;                                                         /* sigaction/sigaltstack */
    case 135:                                                                             /* rt_sigprocmask */
        if (c) memset((void *)c, 0, d > 0 && d <= 128 ? (size_t)d : 8);
        return 0;

    /* time */
    case 113: return sys_clock_gettime(a, (struct lx_timespec *)b);                      /* clock_gettime */
    case 114:                                                                             /* clock_getres */
        if (b) *(struct lx_timespec *)b = (struct lx_timespec){ 0, 1 };
        return 0;
    case 101: return sys_clock_nanosleep(1, 0, (const struct lx_timespec *)a);           /* nanosleep */
    case 115: return sys_clock_nanosleep(a, b, (const struct lx_timespec *)c);           /* clock_nanosleep */
    case 169: {                                                                           /* gettimeofday */
        struct lx_timespec ts;
        sys_clock_gettime(0, &ts);
        if (a) {
            ((int64_t *)a)[0] = ts.sec;
            ((int64_t *)a)[1] = ts.nsec / 1000;
        }
        return 0;
    }

    /* process */
    case 172: return 1;                                                                   /* getpid */
    case 173: return 0;                                                                   /* getppid */
    case 174: case 175: case 176: case 177: return 0;                                     /* uid/gid */
    case 160: return sys_uname((struct lx_utsname *)a);                                   /* uname */
    case 163: return sys_getrlimit((int)a, (struct lx_rlimit *)b);                        /* getrlimit */
    case 261: return sys_getrlimit((int)b, (struct lx_rlimit *)d);                        /* prlimit64 */
    case 278:                                                                             /* getrandom */
        randomGet((void *)a, (size_t)b);
        return b;
    case 283: return 0;                                                                   /* membarrier */
    }
    unknown_syscall(n);
    (void)e;
    return -LX_ENOSYS;
}
