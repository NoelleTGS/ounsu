/*
 * Linux syscalls of the island, answered with libounce. Numbers, structs and errno values are Linux aarch64's.
 * Ported in spirit from the HBLauncher runtime (sdk/linux/runtime.c), whose notes say which calls .NET, musl and
 * the libraries need and why.
 *
 * Files, directories, pipes and /dev are in lx_fd.c; memory in lx_mem.c; threads and futexes in lx_thread.c.
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
    lx_sleep_ns(ns);
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

struct lx_pollfd {
    int32_t fd;
    int16_t events, revents;
};

/* preadv/pwritev(2): one positioned transfer per iovec; offset -1 (the v2 calls) = the current position. */
static long sys_prw_vec(int fd, const struct lx_iovec *iov, int count, long off, int is_write) {
    long total = 0;
    for (int i = 0; i < count; i++) {
        if (!iov[i].len) continue;
        long n = off < 0 ? (is_write ? lx_write(fd, iov[i].base, iov[i].len) : lx_read(fd, iov[i].base, iov[i].len))
                         : lx_pread(fd, iov[i].base, iov[i].len, off + total, is_write);
        if (n < 0) return total ? total : n;
        total += n;
        if ((size_t)n < iov[i].len) break;
    }
    return total;
}

static long sys_rw_vec(int fd, const struct lx_iovec *iov, int count, int is_write) {
    long total = 0;
    for (int i = 0; i < count; i++) {
        if (!iov[i].len) continue;
        long n = is_write ? lx_write(fd, iov[i].base, iov[i].len) : lx_read(fd, iov[i].base, iov[i].len);
        if (n < 0) return total ? total : n;
        total += n;
        if ((size_t)n < iov[i].len) break;
    }
    return total;
}

/* ppoll over the fd layer: pipes block until ready, everything else is always ready. Polls in 1 ms steps (sockets
   will get a proper wait with the network layer). */
static long sys_ppoll(struct lx_pollfd *fds, unsigned long n, const struct lx_timespec *timeout) {
    int64_t deadline = -1;
    if (timeout) deadline = (int64_t)lx_monotonic_ns() + timeout->sec * 1000000000ll + timeout->nsec;
    for (;;) {
        long ready = 0;
        for (unsigned long i = 0; i < n; i++) {
            fds[i].revents = fds[i].fd < 0 ? 0 : (int16_t)lx_fd_poll_state(fds[i].fd, fds[i].events);
            if (fds[i].revents) ready++;
        }
        if (ready) return ready;
        if (deadline >= 0 && (int64_t)lx_monotonic_ns() >= deadline) return 0;
        lx_sleep_ns(1000000ll);
    }
}

static long sys_sysinfo(void *out) {
    /* struct sysinfo (64-bit): uptime, loads[3], totalram, freeram, sharedram, bufferram, totalswap, freeswap,
       procs (u16) + pad, totalhigh, freehigh, mem_unit (u32). 112 bytes. */
    u64 total = 0, used = 0;
    int64_t *s = out;
    memset(out, 0, 112);
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    s[0] = (int64_t)(lx_monotonic_ns() / 1000000000u);
    s[4] = (int64_t)total;
    s[5] = (int64_t)(total > used ? total - used : 0);
    ((uint16_t *)out)[40] = 1;     /* procs at byte 80 */
    ((uint32_t *)out)[26] = 1;     /* mem_unit at byte 104 */
    return 0;
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
    lx_exit_check();
    switch (n) {
    /* files (lx_fd.c) */
    case 56: return lx_openat((int)a, (const char *)b, (int)c, (int)d);                 /* openat */
    case 57: return lx_close((int)a);                                                     /* close */
    case 63: return lx_read((int)a, (void *)b, (size_t)c);                                /* read */
    case 64: return lx_write((int)a, (const void *)b, (size_t)c);                         /* write */
    case 65: return sys_rw_vec((int)a, (const struct lx_iovec *)b, (int)c, 0);           /* readv */
    case 66: return sys_rw_vec((int)a, (const struct lx_iovec *)b, (int)c, 1);           /* writev */
    case 67: return lx_pread((int)a, (void *)b, (size_t)c, d, 0);                         /* pread64 */
    case 68: return lx_pread((int)a, (void *)b, (size_t)c, d, 1);                         /* pwrite64 */
    case 62: return lx_lseek((int)a, b, (int)c);                                          /* lseek */
    case 69: return sys_prw_vec((int)a, (const struct lx_iovec *)b, (int)c, d, 0);       /* preadv */
    case 70: return sys_prw_vec((int)a, (const struct lx_iovec *)b, (int)c, d, 1);       /* pwritev */
    case 286: return sys_prw_vec((int)a, (const struct lx_iovec *)b, (int)c, d, 0);      /* preadv2 */
    case 287: return sys_prw_vec((int)a, (const struct lx_iovec *)b, (int)c, d, 1);      /* pwritev2 */
    case 80: return lx_fstat((int)a, (void *)b);                                          /* fstat */
    case 79: return lx_fstatat((int)a, (const char *)b, (void *)c, (int)d);              /* newfstatat */
    case 291: return -LX_ENOSYS;                                          /* statx: musl falls back to fstatat */
    case 48: case 439: return lx_faccessat((int)a, (const char *)b, (int)c);             /* faccessat(2) */
    case 61: return lx_getdents64((int)a, (void *)b, (size_t)c);                         /* getdents64 */
    case 34: return lx_mkdirat((int)a, (const char *)b, (int)c);                         /* mkdirat */
    case 35: return lx_unlinkat((int)a, (const char *)b, (int)c);                        /* unlinkat */
    case 38: case 276: return lx_renameat((int)a, (const char *)b, (int)c, (const char *)d); /* renameat(2) */
    case 46: return lx_ftruncate((int)a, b);                                              /* ftruncate */
    case 47: return 0;                                                                    /* fallocate */
    case 82: case 83: return lx_fsync((int)a);                                            /* fsync/fdatasync */
    case 23: return lx_dup((int)a, -1, 0, 0);                                             /* dup */
    case 24: return a == b ? -LX_EINVAL : lx_dup((int)a, (int)b, (int)c, 0);              /* dup3 */
    case 25: return lx_fcntl((int)a, (int)b, c);                                          /* fcntl */
    case 59: return lx_pipe2((int *)a, (int)b);                                           /* pipe2 */
    case 73: return sys_ppoll((struct lx_pollfd *)a, (unsigned long)b, (const struct lx_timespec *)c); /* ppoll */
    case 17: return lx_getcwd((char *)a, (size_t)b);                                      /* getcwd */
    case 49: return lx_chdir((const char *)a);                                            /* chdir */
    case 78: return -LX_ENOENT;                                           /* readlinkat (/proc/self/exe) */
    case 43: case 44: return lx_statfs((void *)b);                                        /* statfs/fstatfs */
    case 32: return 0;                                                                    /* flock: granted */
    case 52: case 53: case 88: return 0;                                  /* fchmod(at)/utimensat */
    case 29: {                                                                            /* ioctl */
        if ((a == 1 || a == 2) && b == 0x5413 /* TIOCGWINSZ */) {
            /* Pretend stdout/stderr are terminals: musl then line-buffers them, so log lines leave promptly. */
            struct lx_winsize *w = (struct lx_winsize *)c;
            if (w) *w = (struct lx_winsize){ 25, 120, 0, 0 };
            return 0;
        }
        long r = lx_sock_ioctl((int)a, (unsigned long)b, (void *)c);
        return r == -88 /* not a socket */ ? -LX_ENOTTY : r;
    }

    /* sockets and epoll (lx_net.c) */
    case 198: return lx_socket((int)a, (int)b, (int)c);                                    /* socket */
    case 199: return -97;                                                  /* socketpair: EAFNOSUPPORT */
    case 200: return lx_bind((int)a, (const void *)b, (uint32_t)c);                       /* bind */
    case 201: return lx_listen((int)a, (int)b);                                           /* listen */
    case 202: return lx_accept4((int)a, (void *)b, (uint32_t *)c, 0);                     /* accept */
    case 242: return lx_accept4((int)a, (void *)b, (uint32_t *)c, (int)d);                /* accept4 */
    case 203: return lx_connect((int)a, (const void *)b, (uint32_t)c);                    /* connect */
    case 204: return lx_getsockname((int)a, (void *)b, (uint32_t *)c, 0);                 /* getsockname */
    case 205: return lx_getsockname((int)a, (void *)b, (uint32_t *)c, 1);                 /* getpeername */
    case 206: return lx_sendto((int)a, (const void *)b, (size_t)c, (int)d, (const void *)e, (uint32_t)f); /* sendto */
    case 207: return lx_recvfrom((int)a, (void *)b, (size_t)c, (int)d, (void *)e, (uint32_t *)f);       /* recvfrom */
    case 208: return lx_setsockopt((int)a, (int)b, (int)c, (const void *)d, (uint32_t)e); /* setsockopt */
    case 209: return lx_getsockopt((int)a, (int)b, (int)c, (void *)d, (uint32_t *)e);     /* getsockopt */
    case 210: return lx_shutdown((int)a, (int)b);                                         /* shutdown */
    case 211: return lx_sendmsg((int)a, (const void *)b, (int)c);                         /* sendmsg */
    case 212: return lx_recvmsg((int)a, (void *)b, (int)c);                               /* recvmsg */
    case 20: return lx_epoll_create((int)a);                                              /* epoll_create1 */
    case 21: return lx_epoll_ctl((int)a, (int)b, (int)c, (const void *)d);                /* epoll_ctl */
    case 22: return lx_epoll_wait((int)a, (void *)b, (int)c, (int)d);                     /* epoll_pwait */

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
    case 124: lx_sleep_ns(0); return 0;                                                   /* sched_yield */
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
    case 179: return sys_sysinfo((void *)a);                                              /* sysinfo */
    case 168:                                                                             /* getcpu */
        if (a) *(uint32_t *)a = (uint32_t)svcGetCurrentProcessorNumber();
        if (b) *(uint32_t *)b = 0;
        return 0;
    case 167: return 0;                                                                   /* prctl */
    case 165:                                                                             /* getrusage */
        if (b) memset((void *)b, 0, 144);
        return 0;
    case 236: case 237: return -LX_ENOSYS;                                /* get_mempolicy/mbind */
    }
    unknown_syscall(n);
    return -LX_ENOSYS;
}
