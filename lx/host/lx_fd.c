/*
 * File descriptors for the island: the console, a few /dev devices, files and directories on the SD card, and
 * in-process pipes.
 *
 * Paths: the island sees a Linux file system whose root "/" is a folder on the SD card (lx_fd_init's root, e.g.
 * "sdmc:/switch/osu"). /dev/null, /dev/zero, /dev/urandom and /dev/random are built in (.NET reads /dev/urandom early,
 * Guid.NewGuid among others). /proc and /sys answer ENOENT without touching the card: the .NET GC reads
 * /proc/meminfo and /proc/self/statm on every collection, with the world stopped (HBLauncher finding).
 *
 * Pipes are a ring buffer between two fds (System.Console needs pipe2: HBLauncher had none and Console aborted).
 *
 * Linux flag values (aarch64) are translated to newlib's, newlib's errno values back to Linux's.
 */
#include <switch.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "lx_host.h"

#define MAX_FDS 1024
#define PIPE_SIZE (64u << 10)

/* Linux open flags (aarch64) */
#define L_O_ACCMODE 3
#define L_O_CREAT 0100
#define L_O_EXCL 0200
#define L_O_TRUNC 01000
#define L_O_APPEND 02000
#define L_O_NONBLOCK 04000
#define L_O_DIRECTORY 040000
#define L_O_CLOEXEC 02000000
#define L_AT_FDCWD (-100)
#define L_AT_EMPTY_PATH 0x1000
#define L_AT_REMOVEDIR 0x200

enum fd_kind { FD_FREE, FD_CONSOLE, FD_NULL, FD_ZERO, FD_RANDOM, FD_FILE, FD_DIR, FD_PIPE_R, FD_PIPE_W };

struct pipe_buf {
    Mutex lock;
    CondVar cond;
    uint8_t *data;
    size_t head, len;
    int readers, writers;
};

struct fd {
    enum fd_kind kind;
    int host_fd;      /* FD_FILE: newlib fd */
    DIR *dir;         /* FD_DIR */
    long dir_pos;     /* FD_DIR: entries returned so far */
    char *path;       /* FD_FILE/FD_DIR: host path (for fstat on directories, getdents, relative lookups) */
    char *lx_path;    /* the island's path, for *at() relative to this fd */
    int flags;        /* Linux status flags (O_APPEND, O_NONBLOCK) */
    int cloexec;
    struct pipe_buf *pipe;
    int refs;         /* fds sharing this description (dup) */
};

static struct fd *g_fds[MAX_FDS];
static Mutex g_fd_lock;
static char g_root[256] = "sdmc:/switch/lx-root";
static char g_cwd[1024] = "/";

/* ---- errno and stat translation ----------------------------------------------------------------------------- */

long lx_errno(int e) {
    switch (e) {
    case EPERM: return -1;
    case ENOENT: return -2;
    case EINTR: return -4;
    case EIO: return -5;
    case EBADF: return -9;
    case EAGAIN: return -11;
    case ENOMEM: return -12;
    case EACCES: return -13;
    case EFAULT: return -14;
    case EBUSY: return -16;
    case EEXIST: return -17;
    case EXDEV: return -18;
    case ENOTDIR: return -20;
    case EISDIR: return -21;
    case EINVAL: return -22;
    case EMFILE: return -24;
    case ENOSPC: return -28;
    case ESPIPE: return -29;
    case EROFS: return -30;
    case ENAMETOOLONG: return -36;
    case ENOSYS: return -38;
    case ENOTEMPTY: return -39;
    case ELOOP: return -40;
    case EOVERFLOW: return -75;
    case ENOTSUP: return -95;
    default: return -LX_EIO;
    }
}

struct lx_stat {
    uint64_t dev, ino;
    uint32_t mode, nlink, uid, gid;
    uint64_t rdev, pad1;
    int64_t size;
    int32_t blksize, pad2;
    int64_t blocks;
    int64_t atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns;
    uint32_t unused[2];
};

/* Inode numbers from the path (the SD card has none that stay put; Realm and .NET compare them). */
static uint64_t path_inode(const char *path) {
    uint64_t h = 1469598103934665603ull;
    for (; *path; path++) h = (h ^ (uint8_t)*path) * 1099511628211ull;
    return h | 1;
}

static void to_lx_stat(const struct stat *st, const char *path, struct lx_stat *out) {
    memset(out, 0, sizeof *out);
    out->dev = 1;
    out->ino = path ? path_inode(path) : (uint64_t)st->st_ino;
    out->mode = S_ISDIR(st->st_mode) ? (0040000 | 0755) : (0100000 | 0644);
    out->nlink = 1;
    out->size = st->st_size;
    out->blksize = 4096;
    out->blocks = (st->st_size + 511) / 512;
    out->atime = st->st_atime;
    out->mtime = st->st_mtime;
    out->ctime = st->st_ctime;
}

static void special_stat(uint32_t mode, uint64_t ino, struct lx_stat *out) {
    memset(out, 0, sizeof *out);
    out->dev = 2;
    out->ino = ino;
    out->mode = mode;
    out->nlink = 1;
    out->blksize = 4096;
}

/* ---- paths -------------------------------------------------------------------------------------------------- */

void lx_fd_init(const char *root) {
    if (root) snprintf(g_root, sizeof g_root, "%s", root);
    /* Make sure the root exists (each level; mkdir of an existing folder just fails). */
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s", g_root);
    for (char *p = strchr(tmp, '/'); p; p = strchr(p + 1, '/')) {
        if (p == tmp || p[-1] == ':') continue;
        *p = 0;
        mkdir(tmp, 0777);
        *p = '/';
    }
    mkdir(tmp, 0777);
}

const char *lx_fd_root(void) {
    return g_root;
}

/* Normalises an island path (resolving it against dirfd or the cwd) into out (absolute, no "." / ".." / "//"). */
static long lx_resolve(int dirfd, const char *path, char *out, size_t cap) {
    char joined[2048];
    if (!path) return -LX_EFAULT;
    if (path[0] == '/') {
        snprintf(joined, sizeof joined, "%s", path);
    } else {
        const char *base = g_cwd;
        if (dirfd != L_AT_FDCWD) {
            if (dirfd < 0 || dirfd >= MAX_FDS || !g_fds[dirfd] || !g_fds[dirfd]->lx_path) return -LX_EBADF;
            base = g_fds[dirfd]->lx_path;
        }
        snprintf(joined, sizeof joined, "%s/%s", base, path);
    }
    /* Collapse into out. */
    size_t n = 0;
    const char *p = joined;
    out[n++] = '/';
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *seg = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t)(p - seg);
        if (len == 1 && seg[0] == '.') continue;
        if (len == 2 && seg[0] == '.' && seg[1] == '.') {
            if (n > 1) {
                n--;
                while (n > 1 && out[n - 1] != '/') n--;
                if (n > 1) n--;
            }
            continue;
        }
        if (n + len + 2 >= cap) return -36; /* ENAMETOOLONG */
        if (n > 1) out[n++] = '/';
        memcpy(out + n, seg, len);
        n += len;
    }
    out[n] = 0;
    return 0;
}

static int starts_with_dir(const char *path, const char *dir) {
    size_t n = strlen(dir);
    return strncmp(path, dir, n) == 0 && (path[n] == 0 || path[n] == '/');
}

static void host_path(const char *lx_path, char *out, size_t cap) {
    snprintf(out, cap, "%s%s", g_root, strcmp(lx_path, "/") == 0 ? "" : lx_path);
}

/* ---- the table ---------------------------------------------------------------------------------------------- */

static int fd_install(struct fd *f, int min) {
    mutexLock(&g_fd_lock);
    for (int i = min; i < MAX_FDS; i++) {
        if (!g_fds[i]) {
            g_fds[i] = f;
            mutexUnlock(&g_fd_lock);
            return i;
        }
    }
    mutexUnlock(&g_fd_lock);
    return -24; /* EMFILE */
}

static struct fd *fd_get(int fd) {
    return fd >= 0 && fd < MAX_FDS ? g_fds[fd] : NULL;
}

static struct fd *fd_new(enum fd_kind kind) {
    struct fd *f = calloc(1, sizeof *f);
    if (f) {
        f->kind = kind;
        f->host_fd = -1;
        f->refs = 1;
    }
    return f;
}

static void pipe_release(struct pipe_buf *p, int writer) {
    mutexLock(&p->lock);
    if (writer)
        p->writers--;
    else
        p->readers--;
    int gone = !p->readers && !p->writers;
    condvarWakeAll(&p->cond);
    mutexUnlock(&p->lock);
    if (gone) {
        free(p->data);
        free(p);
    }
}

static void fd_free(struct fd *f) {
    if (--f->refs > 0) return;
    if (f->kind == FD_FILE && f->host_fd >= 0) close(f->host_fd);
    if (f->kind == FD_DIR && f->dir) closedir(f->dir);
    if (f->kind == FD_PIPE_R || f->kind == FD_PIPE_W) pipe_release(f->pipe, f->kind == FD_PIPE_W);
    free(f->path);
    free(f->lx_path);
    free(f);
}

void lx_fd_setup_console(void) {
    for (int i = 0; i < 3; i++) {
        struct fd *f = fd_new(FD_CONSOLE);
        f->host_fd = i;
        g_fds[i] = f;
    }
}

/* ---- syscalls ------------------------------------------------------------------------------------------------ */

long lx_openat(int dirfd, const char *path, int flags, int mode) {
    char lp[1024], hp[1300];
    long r = lx_resolve(dirfd, path, lp, sizeof lp);
    if (r) return r;
    (void)mode;

    struct fd *f = NULL;
    if (starts_with_dir(lp, "/proc") || starts_with_dir(lp, "/sys")) return -LX_ENOENT;
    if (starts_with_dir(lp, "/dev")) {
        if (!strcmp(lp, "/dev/null")) f = fd_new(FD_NULL);
        else if (!strcmp(lp, "/dev/zero")) f = fd_new(FD_ZERO);
        else if (!strcmp(lp, "/dev/urandom") || !strcmp(lp, "/dev/random")) f = fd_new(FD_RANDOM);
        else if (!strcmp(lp, "/dev/tty") || !strcmp(lp, "/dev/stdout") || !strcmp(lp, "/dev/stderr")) {
            f = fd_new(FD_CONSOLE);
            if (f) f->host_fd = 1;
        } else return -LX_ENOENT;
        if (!f) return -LX_ENOMEM;
    } else {
        struct stat st;
        host_path(lp, hp, sizeof hp);
        int exists = stat(hp, &st) == 0;
        if (exists && S_ISDIR(st.st_mode)) {
            if ((flags & L_O_ACCMODE) != 0) return -21; /* EISDIR */
            DIR *d = opendir(hp);
            if (!d) return lx_errno(errno);
            f = fd_new(FD_DIR);
            f->dir = d;
        } else {
            if (flags & L_O_DIRECTORY) return exists ? -20 /* ENOTDIR */ : -LX_ENOENT;
            int nf = (flags & L_O_ACCMODE) == 0 ? O_RDONLY : (flags & L_O_ACCMODE) == 1 ? O_WRONLY : O_RDWR;
            if (flags & L_O_CREAT) nf |= O_CREAT;
            if (flags & L_O_EXCL) nf |= O_EXCL;
            if (flags & L_O_TRUNC) nf |= O_TRUNC;
            if (flags & L_O_APPEND) nf |= O_APPEND;
            int hfd = open(hp, nf, 0666);
            if (hfd < 0) return lx_errno(errno);
            f = fd_new(FD_FILE);
            f->host_fd = hfd;
        }
        f->path = strdup(hp);
    }
    f->lx_path = strdup(lp);
    f->flags = flags & (L_O_APPEND | L_O_NONBLOCK);
    f->cloexec = !!(flags & L_O_CLOEXEC);
    int fd = fd_install(f, 0);
    if (fd < 0) fd_free(f);
    return fd;
}

long lx_close(int fd) {
    mutexLock(&g_fd_lock);
    struct fd *f = fd_get(fd);
    if (!f) {
        mutexUnlock(&g_fd_lock);
        return -LX_EBADF;
    }
    g_fds[fd] = NULL;
    mutexUnlock(&g_fd_lock);
    fd_free(f);
    return 0;
}

static long pipe_read(struct fd *f, uint8_t *buf, size_t len) {
    struct pipe_buf *p = f->pipe;
    mutexLock(&p->lock);
    while (!p->len) {
        if (!p->writers) {
            mutexUnlock(&p->lock);
            return 0;
        }
        if (f->flags & L_O_NONBLOCK) {
            mutexUnlock(&p->lock);
            return -LX_EAGAIN;
        }
        condvarWaitTimeout(&p->cond, &p->lock, 100000000ll); /* wakes now and then for the exit check */
        if (lx_exiting()) {
            mutexUnlock(&p->lock);
            lx_exit_check();
        }
    }
    size_t n = len < p->len ? len : p->len;
    for (size_t i = 0; i < n; i++) buf[i] = p->data[(p->head + i) % PIPE_SIZE];
    p->head = (p->head + n) % PIPE_SIZE;
    p->len -= n;
    condvarWakeAll(&p->cond);
    mutexUnlock(&p->lock);
    return (long)n;
}

static long pipe_write(struct fd *f, const uint8_t *buf, size_t len) {
    struct pipe_buf *p = f->pipe;
    size_t done = 0;
    mutexLock(&p->lock);
    while (done < len) {
        if (!p->readers) {
            mutexUnlock(&p->lock);
            return done ? (long)done : -32; /* EPIPE (no SIGPIPE here) */
        }
        if (p->len == PIPE_SIZE) {
            if (f->flags & L_O_NONBLOCK) break;
            condvarWaitTimeout(&p->cond, &p->lock, 100000000ll);
            if (lx_exiting()) {
                mutexUnlock(&p->lock);
                lx_exit_check();
            }
            continue;
        }
        while (done < len && p->len < PIPE_SIZE) {
            p->data[(p->head + p->len) % PIPE_SIZE] = buf[done++];
            p->len++;
        }
        condvarWakeAll(&p->cond);
    }
    mutexUnlock(&p->lock);
    return done ? (long)done : -LX_EAGAIN;
}

long lx_read(int fd, void *buf, size_t len) {
    struct fd *f = fd_get(fd);
    if (!f) return -LX_EBADF;
    switch (f->kind) {
    case FD_CONSOLE: return f->host_fd == 0 ? 0 : -LX_EBADF;
    case FD_NULL: return 0;
    case FD_ZERO: memset(buf, 0, len); return (long)len;
    case FD_RANDOM: randomGet(buf, len); return (long)len;
    case FD_FILE: {
        ssize_t n = read(f->host_fd, buf, len);
        return n < 0 ? lx_errno(errno) : n;
    }
    case FD_DIR: return -21; /* EISDIR */
    case FD_PIPE_R: return pipe_read(f, buf, len);
    default: return -LX_EBADF;
    }
}

long lx_write(int fd, const void *buf, size_t len) {
    struct fd *f = fd_get(fd);
    if (!f) return -LX_EBADF;
    switch (f->kind) {
    case FD_CONSOLE: {
        if (f->host_fd == 0) return -LX_EBADF;
        ssize_t n = write(f->host_fd, buf, len);
        return n < 0 ? (long)len : n; /* a closed nxlink connection must not fail the program */
    }
    case FD_NULL: case FD_ZERO: case FD_RANDOM: return (long)len;
    case FD_FILE: {
        ssize_t n = write(f->host_fd, buf, len);
        return n < 0 ? lx_errno(errno) : n;
    }
    case FD_PIPE_W: return pipe_write(f, buf, len);
    default: return -LX_EBADF;
    }
}

long lx_pread(int fd, void *buf, size_t len, long off, int is_write) {
    struct fd *f = fd_get(fd);
    if (!f) return -LX_EBADF;
    if (f->kind != FD_FILE) return -LX_ESPIPE;
    /* newlib has no pread: seek, transfer, seek back (callers that share an fd across threads serialise anyway). */
    off_t was = lseek(f->host_fd, 0, SEEK_CUR);
    if (lseek(f->host_fd, off, SEEK_SET) < 0) return lx_errno(errno);
    ssize_t n = is_write ? write(f->host_fd, buf, len) : read(f->host_fd, buf, len);
    int e = errno;
    lseek(f->host_fd, was, SEEK_SET);
    return n < 0 ? lx_errno(e) : n;
}

long lx_lseek(int fd, long off, int whence) {
    struct fd *f = fd_get(fd);
    if (!f) return -LX_EBADF;
    if (f->kind == FD_DIR) {
        if (off == 0 && whence == 0) {
            rewinddir(f->dir);
            f->dir_pos = 0;
            return 0;
        }
        return -LX_EINVAL;
    }
    if (f->kind != FD_FILE) return -LX_ESPIPE;
    off_t r = lseek(f->host_fd, off, whence);
    return r < 0 ? lx_errno(errno) : (long)r;
}

long lx_fstat(int fd, void *out) {
    struct fd *f = fd_get(fd);
    struct stat st;
    if (!f) return -LX_EBADF;
    switch (f->kind) {
    case FD_CONSOLE: special_stat(0020000 | 0620, 10 + (uint64_t)fd, out); return 0; /* character device */
    case FD_NULL: case FD_ZERO: case FD_RANDOM: special_stat(0020000 | 0666, 20 + f->kind, out); return 0;
    case FD_PIPE_R: case FD_PIPE_W: special_stat(0010000 | 0600, (uintptr_t)f->pipe, out); return 0; /* FIFO */
    case FD_FILE:
        if (fstat(f->host_fd, &st) != 0) return lx_errno(errno);
        to_lx_stat(&st, f->path, out);
        return 0;
    case FD_DIR:
        if (stat(f->path, &st) != 0) return lx_errno(errno);
        to_lx_stat(&st, f->path, out);
        return 0;
    default: return -LX_EBADF;
    }
}

long lx_fstatat(int dirfd, const char *path, void *out, int flags) {
    char lp[1024], hp[1300];
    struct stat st;
    if ((flags & L_AT_EMPTY_PATH) && path && !path[0]) return lx_fstat(dirfd, out);
    long r = lx_resolve(dirfd, path, lp, sizeof lp);
    if (r) return r;
    if (starts_with_dir(lp, "/proc") || starts_with_dir(lp, "/sys")) return -LX_ENOENT;
    if (starts_with_dir(lp, "/dev")) {
        if (!strcmp(lp, "/dev")) {
            special_stat(0040000 | 0755, 3, out);
            return 0;
        }
        if (!strcmp(lp, "/dev/null") || !strcmp(lp, "/dev/zero") || !strcmp(lp, "/dev/urandom") ||
            !strcmp(lp, "/dev/random") || !strcmp(lp, "/dev/tty")) {
            special_stat(0020000 | 0666, path_inode(lp), out);
            return 0;
        }
        return -LX_ENOENT;
    }
    host_path(lp, hp, sizeof hp);
    if (stat(hp, &st) != 0) return lx_errno(errno);
    to_lx_stat(&st, hp, out);
    return 0;
}

long lx_faccessat(int dirfd, const char *path, int mode) {
    struct lx_stat st;
    (void)mode;
    return lx_fstatat(dirfd, path, &st, 0);
}

long lx_getdents64(int fd, void *buf, size_t cap) {
    struct fd *f = fd_get(fd);
    if (!f) return -LX_EBADF;
    if (f->kind != FD_DIR) return -20; /* ENOTDIR */
    size_t used = 0;
    for (;;) {
        long pos = telldir(f->dir);
        struct dirent *e = readdir(f->dir);
        if (!e) break;
        size_t name_len = strlen(e->d_name);
        size_t reclen = (19 + name_len + 1 + 7) & ~(size_t)7;
        if (used + reclen > cap) {
            seekdir(f->dir, pos);
            if (!used) return -LX_EINVAL;
            break;
        }
        uint8_t *rec = (uint8_t *)buf + used;
        char child[1300];
        snprintf(child, sizeof child, "%s/%s", f->path, e->d_name);
        uint64_t ino = path_inode(child);
        int64_t off = ++f->dir_pos;
        uint16_t rl = (uint16_t)reclen;
        memcpy(rec, &ino, 8);
        memcpy(rec + 8, &off, 8);
        memcpy(rec + 16, &rl, 2);
        rec[18] = e->d_type == DT_DIR ? 4 : e->d_type == DT_REG ? 8 : 0;
        memcpy(rec + 19, e->d_name, name_len + 1);
        used += reclen;
    }
    return (long)used;
}

long lx_mkdirat(int dirfd, const char *path, int mode) {
    char lp[1024], hp[1300];
    long r = lx_resolve(dirfd, path, lp, sizeof lp);
    (void)mode;
    if (r) return r;
    host_path(lp, hp, sizeof hp);
    return mkdir(hp, 0777) == 0 ? 0 : lx_errno(errno);
}

long lx_unlinkat(int dirfd, const char *path, int flags) {
    char lp[1024], hp[1300];
    long r = lx_resolve(dirfd, path, lp, sizeof lp);
    if (r) return r;
    host_path(lp, hp, sizeof hp);
    r = (flags & L_AT_REMOVEDIR) ? rmdir(hp) : unlink(hp);
    return r == 0 ? 0 : lx_errno(errno);
}

long lx_renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath) {
    char lo[1024], ln[1024], ho[1300], hn[1300];
    long r = lx_resolve(olddirfd, oldpath, lo, sizeof lo);
    if (!r) r = lx_resolve(newdirfd, newpath, ln, sizeof ln);
    if (r) return r;
    host_path(lo, ho, sizeof ho);
    host_path(ln, hn, sizeof hn);
    /* POSIX rename replaces an existing target; FAT's doesn't. */
    struct stat st;
    if (stat(hn, &st) == 0 && !S_ISDIR(st.st_mode) && strcmp(ho, hn) != 0) unlink(hn);
    return rename(ho, hn) == 0 ? 0 : lx_errno(errno);
}

long lx_ftruncate(int fd, long len) {
    struct fd *f = fd_get(fd);
    if (!f || f->kind != FD_FILE) return -LX_EBADF;
    return ftruncate(f->host_fd, len) == 0 ? 0 : lx_errno(errno);
}

long lx_fsync(int fd) {
    struct fd *f = fd_get(fd);
    if (!f) return -LX_EBADF;
    if (f->kind == FD_FILE) fsync(f->host_fd);
    return 0;
}

long lx_dup(int fd, int newfd, int flags, int min) {
    mutexLock(&g_fd_lock);
    struct fd *f = fd_get(fd);
    if (!f) {
        mutexUnlock(&g_fd_lock);
        return -LX_EBADF;
    }
    f->refs++;
    mutexUnlock(&g_fd_lock);
    if (newfd >= 0) {
        if (newfd >= MAX_FDS) {
            fd_free(f);
            return -LX_EBADF;
        }
        mutexLock(&g_fd_lock);
        struct fd *old = g_fds[newfd];
        g_fds[newfd] = f;
        mutexUnlock(&g_fd_lock);
        if (old) fd_free(old);
        (void)flags;
        return newfd;
    }
    int r = fd_install(f, min);
    if (r < 0) fd_free(f);
    return r;
}

long lx_fcntl(int fd, int cmd, long arg) {
    struct fd *f = fd_get(fd);
    if (!f) return -LX_EBADF;
    switch (cmd) {
    case 0: return lx_dup(fd, -1, 0, (int)arg);         /* F_DUPFD */
    case 1030: return lx_dup(fd, -1, 0, (int)arg);      /* F_DUPFD_CLOEXEC */
    case 1: return f->cloexec ? 1 : 0;                  /* F_GETFD */
    case 2: f->cloexec = arg & 1; return 0;             /* F_SETFD */
    case 3: {                                           /* F_GETFL */
        int acc = f->kind == FD_PIPE_W ? 1 : f->kind == FD_PIPE_R ? 0 : 2;
        return acc | f->flags;
    }
    case 4: f->flags = (int)arg & (L_O_APPEND | L_O_NONBLOCK); return 0; /* F_SETFL */
    case 5: case 6: case 7: return 0;                   /* F_GETLK/SETLK/SETLKW: granted */
    case 36: case 37: case 38: return 0;                /* OFD locks */
    }
    return -LX_EINVAL;
}

long lx_pipe2(int *fds, int flags) {
    struct pipe_buf *p = calloc(1, sizeof *p);
    struct fd *r = fd_new(FD_PIPE_R), *w = fd_new(FD_PIPE_W);
    if (!p || !r || !w || !(p->data = malloc(PIPE_SIZE))) {
        free(p ? p->data : NULL);
        free(p);
        free(r);
        free(w);
        return -LX_ENOMEM;
    }
    mutexInit(&p->lock);
    condvarInit(&p->cond);
    p->readers = p->writers = 1;
    r->pipe = w->pipe = p;
    r->flags = w->flags = flags & L_O_NONBLOCK;
    r->cloexec = w->cloexec = !!(flags & L_O_CLOEXEC);
    int a = fd_install(r, 0);
    if (a < 0) {
        fd_free(r);
        fd_free(w);
        return a;
    }
    int b = fd_install(w, 0);
    if (b < 0) {
        lx_close(a);
        fd_free(w);
        return b;
    }
    fds[0] = a;
    fds[1] = b;
    return 0;
}

/* Readiness for poll: bit 0 POLLIN, bit 2 POLLOUT, 0x10 POLLHUP, 0x20 POLLNVAL. */
int lx_fd_poll_state(int fd, int events) {
    struct fd *f = fd_get(fd);
    if (!f) return 0x20;
    int ready = 0;
    if (f->kind == FD_PIPE_R) {
        mutexLock(&f->pipe->lock);
        if (f->pipe->len) ready |= 1;
        if (!f->pipe->writers) ready |= 0x10 | 1;
        mutexUnlock(&f->pipe->lock);
    } else if (f->kind == FD_PIPE_W) {
        mutexLock(&f->pipe->lock);
        if (f->pipe->len < PIPE_SIZE) ready |= 4;
        if (!f->pipe->readers) ready |= 0x08; /* POLLERR */
        mutexUnlock(&f->pipe->lock);
    } else if (f->kind == FD_CONSOLE && f->host_fd == 0) {
        ready = 0; /* stdin never has input */
    } else {
        ready = 1 | 4;
    }
    return ready & (events | 0x08 | 0x10 | 0x20);
}

long lx_getcwd(char *buf, size_t size) {
    size_t n = strlen(g_cwd) + 1;
    if (n > size) return -34; /* ERANGE */
    memcpy(buf, g_cwd, n);
    return (long)n;
}

long lx_chdir(const char *path) {
    char lp[1024];
    struct lx_stat st;
    long r = lx_resolve(L_AT_FDCWD, path, lp, sizeof lp);
    if (r) return r;
    r = lx_fstatat(L_AT_FDCWD, lp, &st, 0);
    if (r) return r;
    if ((st.mode & 0170000) != 0040000) return -20;
    snprintf(g_cwd, sizeof g_cwd, "%s", lp);
    return 0;
}

long lx_statfs(void *out) {
    /* struct statfs (aarch64): type, bsize, blocks, bfree, bavail, files, ffree, fsid[2], namelen, frsize, flags,
       spare[4] - all 64-bit except fsid (2 x 32). */
    uint64_t *s = out;
    struct statvfs v;
    memset(out, 0, 120);
    s[0] = 0x4d44; /* MSDOS_SUPER_MAGIC */
    s[1] = 4096;
    if (statvfs(g_root, &v) == 0 && v.f_frsize) {
        s[1] = v.f_frsize;
        s[2] = v.f_blocks;
        s[3] = v.f_bfree;
        s[4] = v.f_bavail;
    }
    s[8] = 255;     /* namelen */
    s[9] = s[1];    /* frsize */
    return 0;
}
