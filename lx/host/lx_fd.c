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

#include "lx_fd.h"

#define PIPE_SIZE (64u << 10)

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
    case EPIPE: return -32;
    case ENOTSOCK: return -88;
    case EMSGSIZE: return -90;
    case ENOPROTOOPT: return -92;
    case EPROTONOSUPPORT: return -93;
    case EAFNOSUPPORT: return -97;
    case EADDRINUSE: return -98;
    case EADDRNOTAVAIL: return -99;
    case ENETDOWN: return -100;
    case ENETUNREACH: return -101;
    case ECONNABORTED: return -103;
    case ECONNRESET: return -104;
    case ENOBUFS: return -105;
    case EISCONN: return -106;
    case ENOTCONN: return -107;
    case ETIMEDOUT: return -110;
    case ECONNREFUSED: return -111;
    case EHOSTDOWN: return -112;
    case EHOSTUNREACH: return -113;
    case EALREADY: return -114;
    case EINPROGRESS: return -115;
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

/* Files bundled in the .nro's romfs (a build can put small read-only files there, e.g. a CA bundle) are found
   when the SD card has no file of that name. */
static int g_romfs;

void lx_fd_use_romfs(int mounted) {
    g_romfs = mounted;
}

static int romfs_path(const char *lx_path, char *out, size_t cap) {
    if (!g_romfs) return 0;
    snprintf(out, cap, "romfs:%s", lx_path);
    struct stat st;
    return stat(out, &st) == 0;
}

/* ---- the table ---------------------------------------------------------------------------------------------- */

int fd_install(struct fd *f, int min) {
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

struct fd *fd_get(int fd) {
    return fd >= 0 && fd < MAX_FDS ? g_fds[fd] : NULL;
}

struct fd *fd_new(enum fd_kind kind) {
    struct fd *f = calloc(1, sizeof *f);
    if (f) {
        f->kind = kind;
        f->host_fd = -1;
        f->refs = 1;
        static uint64_t next_gen;
        f->gen = __atomic_add_fetch(&next_gen, 1, __ATOMIC_RELAXED);
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

/* ---- shared card handles -----------------------------------------------------------------------------------------

   The console's file system refuses to open a file again while it is open for writing (EIO). Linux allows it and
   libraries rely on it (Realm opens its lock file twice). So the host keeps ONE card handle per file, shared by every
   island descriptor of that file, each with its own position: every read and write is a positioned transfer on the
   shared handle, under its lock. A file first opened read-only and later for writing gets its handle reopened
   read-write.

   The same file system also fails stat (ENOENT) and unlink (EIO) on a file that is open. So stat and open look for a
   shared handle first, and unlinking an open file only marks its handle: the path then reads as gone, and the card
   file is deleted when the last descriptor closes (Linux semantics; osu! deletes a download it still has open). */

struct hfile {
    char *path;
    int fd;
    int refs;
    int writable;
    int unlinked;
    Mutex lock;
    struct hfile *next;
};

static struct hfile *g_hfiles;
static Mutex g_hfiles_lock;

/* Opens (or joins) the shared handle of path. nf: newlib open flags. Returns the handle or NULL with *err. */
static struct hfile *hfile_open(const char *path, int nf, long *err) {
    int want_write = (nf & O_ACCMODE) != O_RDONLY;
    mutexLock(&g_hfiles_lock);
    struct hfile *h;
    for (h = g_hfiles; h && strcmp(h->path, path); h = h->next) {
    }
    if (h && h->unlinked && !(nf & O_CREAT)) {
        mutexUnlock(&g_hfiles_lock);
        *err = -2; /* ENOENT */
        return NULL;
    }
    if (h) {
        if (h->unlinked) {
            lx_logf("lx: %s created again while its unlinked predecessor is open: sharing it", path);
            h->unlinked = 0;
        }
        if ((nf & O_CREAT) && (nf & O_EXCL)) {
            mutexUnlock(&g_hfiles_lock);
            *err = -17; /* EEXIST */
            return NULL;
        }
        mutexLock(&h->lock);
        if (want_write && !h->writable) {
            int n = open(path, O_RDWR);
            if (n < 0) {
                *err = lx_errno(errno);
                mutexUnlock(&h->lock);
                mutexUnlock(&g_hfiles_lock);
                return NULL;
            }
            close(h->fd);
            h->fd = n;
            h->writable = 1;
        }
        if (nf & O_TRUNC) ftruncate(h->fd, 0);
        mutexUnlock(&h->lock);
        h->refs++;
        mutexUnlock(&g_hfiles_lock);
        return h;
    }
    /* Writers open read-write, so a later reader or writer can share the handle as it is. */
    int flags = (want_write ? O_RDWR : O_RDONLY) | (nf & (O_CREAT | O_EXCL | O_TRUNC));
    int fd = open(path, flags, 0666);
    if (fd < 0 && want_write && (nf & O_ACCMODE) == O_WRONLY && errno == EACCES) fd = open(path, nf & ~O_APPEND, 0666);
    if (fd < 0) {
        *err = lx_errno(errno);
        mutexUnlock(&g_hfiles_lock);
        return NULL;
    }
    h = calloc(1, sizeof *h);
    h->path = strdup(path);
    h->fd = fd;
    h->refs = 1;
    h->writable = want_write;
    mutexInit(&h->lock);
    h->next = g_hfiles;
    g_hfiles = h;
    mutexUnlock(&g_hfiles_lock);
    return h;
}

static void hfile_release(struct hfile *h) {
    mutexLock(&g_hfiles_lock);
    if (--h->refs == 0) {
        for (struct hfile **pp = &g_hfiles; *pp; pp = &(*pp)->next) {
            if (*pp == h) {
                *pp = h->next;
                break;
            }
        }
        close(h->fd);
        if (h->unlinked && unlink(h->path) != 0) lx_logf("lx: deferred unlink of %s failed (%d)", h->path, errno);
        free(h->path);
        free(h);
    }
    mutexUnlock(&g_hfiles_lock);
}

/* stat through an open shared handle. Returns 1 (found, *r = result), or 0 when the file isn't open. */
static int hfile_stat_path(const char *path, struct stat *st, long *r) {
    mutexLock(&g_hfiles_lock);
    struct hfile *h;
    for (h = g_hfiles; h && strcmp(h->path, path); h = h->next) {
    }
    if (h) {
        if (h->unlinked) {
            *r = -2;
        } else {
            mutexLock(&h->lock);
            *r = fstat(h->fd, st) == 0 ? 0 : lx_errno(errno);
            mutexUnlock(&h->lock);
        }
    }
    mutexUnlock(&g_hfiles_lock);
    return h != NULL;
}

/* unlink of a file that may be open: returns 1 when it was open (*r = result), 0 otherwise. */
static int hfile_unlink_path(const char *path, long *r) {
    mutexLock(&g_hfiles_lock);
    struct hfile *h;
    for (h = g_hfiles; h && strcmp(h->path, path); h = h->next) {
    }
    if (h) *r = h->unlinked ? -2 : (h->unlinked = 1, 0);
    mutexUnlock(&g_hfiles_lock);
    return h != NULL;
}

static struct hfile *hfile_find(const char *path) {
    mutexLock(&g_hfiles_lock);
    struct hfile *h;
    for (h = g_hfiles; h && strcmp(h->path, path); h = h->next) {
    }
    mutexUnlock(&g_hfiles_lock);
    return h;
}

/* A positioned transfer on the shared handle. off < 0: at the end (O_APPEND). Returns bytes or -errno; *at gets the
   offset used. */
static long hfile_rw(struct hfile *h, void *buf, size_t len, int64_t off, int is_write, uint64_t *at) {
    mutexLock(&h->lock);
    off_t where = off < 0 ? lseek(h->fd, 0, SEEK_END) : lseek(h->fd, off, SEEK_SET);
    long r;
    if (where < 0) {
        r = lx_errno(errno);
    } else {
        ssize_t n = is_write ? write(h->fd, buf, len) : read(h->fd, buf, len);
        r = n < 0 ? lx_errno(errno) : n;
    }
    mutexUnlock(&h->lock);
    if (at) *at = where < 0 ? 0 : (uint64_t)where;
    return r;
}

static long hfile_stat(struct hfile *h, struct stat *st) {
    mutexLock(&h->lock);
    int r = fstat(h->fd, st);
    int e = errno;
    mutexUnlock(&h->lock);
    return r == 0 ? 0 : lx_errno(e);
}

static long hfile_truncate(struct hfile *h, uint64_t len) {
    mutexLock(&h->lock);
    int r = ftruncate(h->fd, (off_t)len);
    int e = errno;
    mutexUnlock(&h->lock);
    return r == 0 ? 0 : lx_errno(e);
}

static void shm_detach(struct fd *f);

void fd_free(struct fd *f) {
    if (--f->refs > 0) return;
    /* Close the host file first: a write-back on detach opens it again, and the console's file system refuses a
       second write handle to a file that is still open (EIO). */
    if (f->shm) shm_detach(f);
    if (f->kind == FD_FILE && f->hf) hfile_release(f->hf);
    f->hf = NULL;
    if (f->kind == FD_DIR && f->dir) closedir(f->dir);
    if (f->kind == FD_DIR) free(f->dir_stash);
    if (f->kind == FD_PIPE_R || f->kind == FD_PIPE_W) pipe_release(f->pipe, f->kind == FD_PIPE_W);
    if (f->kind == FD_SOCKET && f->host_fd >= 0) close(f->host_fd);
    if (f->kind == FD_EPOLL) lx_epoll_free(f->epoll);
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

/* ---- shared file mappings (MAP_SHARED), for Realm --------------------------------------------------------------

   Heap memory can't be placed at a chosen address or aliased, so a file mapped MAP_SHARED gets ONE buffer holding the
   whole file: every mapping of it returns buffer + offset (so mappings alias each other, as pages would), and read,
   write, ftruncate, lseek and fstat of the file go through the same buffer while it exists. It is written back to the
   SD card on msync/fsync and when the last mapping and descriptor are gone. It can't grow in place, so it reserves
   room up front (4x the file, 64 MiB .. 2 GiB); new room is zeroed when the file grows into it, not before (the heap
   commits memory when it is touched). The HBLauncher runtime's design (vfs.c), on the SD card. */

#define SHM_MIN_CAP (64ull << 20)
#define SHM_MAX_CAP (2048ull << 20)
#define SHM_ROUND (1ull << 20)
#define MAX_SHM 32

struct shm {
    char *path; /* host path */
    uint8_t *data;
    uint64_t size, cap;
    int maps, refs;
};

static struct shm *g_shm[MAX_SHM];
static Mutex g_shm_lock;

static struct shm *shm_for_path(const char *path) {
    for (int i = 0; i < MAX_SHM; i++)
        if (g_shm[i] && !strcmp(g_shm[i]->path, path)) return g_shm[i];
    return NULL;
}

static struct shm *shm_for_addr(uintptr_t a) {
    for (int i = 0; i < MAX_SHM; i++)
        if (g_shm[i] && a >= (uintptr_t)g_shm[i]->data && a < (uintptr_t)g_shm[i]->data + g_shm[i]->cap) return g_shm[i];
    return NULL;
}

/* Lock held. Writes the buffer back to the file: through a descriptor the program has open for writing if there is
   one (the console's file system refuses a second write handle to an open file: EIO, seen with Realm's lock file),
   else through a new one. */
static long shm_flush(struct shm *m) {
    /* Through the file's shared handle if it is open (made writable if need be), else a handle of our own. */
    struct hfile *h = hfile_find(m->path);
    int own = !h;
    long err = 0;
    if (own) {
        h = hfile_open(m->path, O_RDWR | O_CREAT, &err);
        if (!h) {
            lx_logf("lx: shm write-back of %s failed: open (%ld)", m->path, err);
            return err;
        }
    } else if (!h->writable) {
        mutexLock(&h->lock);
        int n = open(m->path, O_RDWR);
        if (n >= 0) {
            close(h->fd);
            h->fd = n;
            h->writable = 1;
        }
        mutexUnlock(&h->lock);
    }
    uint64_t done = 0;
    while (done < m->size) {
        size_t chunk = m->size - done < (8u << 20) ? (size_t)(m->size - done) : (8u << 20);
        long n = hfile_rw(h, m->data + done, chunk, (int64_t)done, 1, NULL);
        if (n <= 0) break;
        done += (uint64_t)n;
    }
    hfile_truncate(h, m->size);
    mutexLock(&h->lock);
    fsync(h->fd);
    mutexUnlock(&h->lock);
    if (own) hfile_release(h);
    if (done != m->size) {
        lx_logf("lx: shm write-back of %s failed after %llu of %llu bytes", m->path, (unsigned long long)done,
                (unsigned long long)m->size);
        return -LX_EIO;
    }
    return 0;
}

/* Lock held: a buffer nobody maps or has open any more is written back and freed. */
static void shm_release_if_unused(struct shm *m) {
    if (m->maps > 0 || m->refs > 0) return;
    shm_flush(m);
    for (int i = 0; i < MAX_SHM; i++)
        if (g_shm[i] == m) g_shm[i] = NULL;
    lx_logf("lx: shm released %s (%llu bytes)", m->path, (unsigned long long)m->size);
    free(m->data);
    free(m->path);
    free(m);
}

static void shm_detach(struct fd *f) {
    mutexLock(&g_shm_lock);
    f->shm->refs--;
    shm_release_if_unused(f->shm);
    f->shm = NULL;
    mutexUnlock(&g_shm_lock);
}

/* Lock held: makes f read and write through m. */
static void shm_attach(struct fd *f, struct shm *m) {
    if (f->shm) return;
    f->shm = m; /* f->pos is already this descriptor's own position */
    m->refs++;
}

/* Lock held: room for size bytes (zeroing what the file grows into). */
static long shm_resize(struct shm *m, uint64_t size) {
    if (size > m->cap) {
        lx_logf("lx: shm %s: %llu bytes is beyond its %llu MiB reservation", m->path, (unsigned long long)size,
                (unsigned long long)(m->cap >> 20));
        return -28; /* ENOSPC */
    }
    if (size > m->size) memset(m->data + m->size, 0, (size_t)(size - m->size));
    m->size = size;
    return 0;
}

void *lx_fd_map_shared(int fd, uint64_t len, uint64_t off, long *err) {
    struct fd *f = fd_get(fd);
    if (!f || f->kind != FD_FILE) {
        *err = -LX_EBADF;
        return NULL;
    }
    mutexLock(&g_shm_lock);
    struct shm *m = f->shm ? f->shm : shm_for_path(f->path);
    if (!m) {
        struct stat st;
        uint64_t size = hfile_stat(f->hf, &st) == 0 ? (uint64_t)st.st_size : 0;
        uint64_t need = size > off + len ? size : off + len;
        uint64_t cap = (need * 4 + SHM_ROUND - 1) & ~(SHM_ROUND - 1);
        if (cap < SHM_MIN_CAP) cap = SHM_MIN_CAP;
        if (cap > SHM_MAX_CAP) cap = (need + SHM_ROUND - 1) & ~(SHM_ROUND - 1);
        int slot;
        for (slot = 0; slot < MAX_SHM && g_shm[slot]; slot++) {
        }
        m = calloc(1, sizeof *m);
        uint8_t *data = slot < MAX_SHM && m ? aligned_alloc(4096, (size_t)cap) : NULL;
        if (!data) {
            free(m);
            mutexUnlock(&g_shm_lock);
            *err = -LX_ENOMEM;
            return NULL;
        }
        m->path = strdup(f->path);
        m->data = data;
        m->cap = cap;
        /* Read the file in through its shared handle (a second handle would be refused while the file is open for
           writing: Realm then saw an empty lock file and retried forever). */
        uint64_t got = 0;
        while (got < size) {
            long n = hfile_rw(f->hf, data + got, (size_t)(size - got), (int64_t)got, 0, NULL);
            if (n <= 0) break;
            got += (uint64_t)n;
        }
        if (got < size) {
            lx_logf("lx: shm %s: read %llu of %llu bytes", f->path, (unsigned long long)got, (unsigned long long)size);
            memset(data + got, 0, (size_t)(size - got));
            got = size;
        }
        m->size = got;
        g_shm[slot] = m;
        lx_logf("lx: shm %s: %llu bytes, reserved %llu MiB", m->path, (unsigned long long)got,
                (unsigned long long)(cap >> 20));
        /* Every open descriptor of the file goes through the buffer from now on. */
        mutexLock(&g_fd_lock);
        for (int i = 0; i < MAX_FDS; i++)
            if (g_fds[i] && g_fds[i]->kind == FD_FILE && !g_fds[i]->shm && !strcmp(g_fds[i]->path, m->path))
                shm_attach(g_fds[i], m);
        mutexUnlock(&g_fd_lock);
    }
    shm_attach(f, m);
    if (off + len > m->cap) {
        lx_logf("lx: shm %s: mapping %llu+%llu is beyond its %llu MiB reservation", m->path, (unsigned long long)off,
                (unsigned long long)len, (unsigned long long)(m->cap >> 20));
        mutexUnlock(&g_shm_lock);
        *err = -LX_ENOMEM;
        return NULL;
    }
    m->maps++;
    mutexUnlock(&g_shm_lock);
    return m->data + off;
}

int lx_fd_unmap_shared(uintptr_t addr) {
    mutexLock(&g_shm_lock);
    struct shm *m = shm_for_addr(addr);
    if (m) {
        if (m->maps > 0) m->maps--;
        shm_release_if_unused(m);
    }
    mutexUnlock(&g_shm_lock);
    return m != NULL;
}

/* mremap of a shared mapping: the same buffer serves any length within its reservation. Returns the address, or 0
   if addr isn't a shared mapping (-ENOMEM beyond the reservation). */
long lx_fd_remap_shared(uintptr_t addr, uint64_t new_len) {
    mutexLock(&g_shm_lock);
    struct shm *m = shm_for_addr(addr);
    long r = 0;
    if (m) r = addr + new_len <= (uintptr_t)m->data + m->cap ? (long)addr : -LX_ENOMEM;
    mutexUnlock(&g_shm_lock);
    return r;
}

long lx_fd_msync(uintptr_t addr) {
    mutexLock(&g_shm_lock);
    struct shm *m = shm_for_addr(addr);
    long r = m ? shm_flush(m) : 0;
    mutexUnlock(&g_shm_lock);
    return r;
}

/* For MAP_PRIVATE file mappings: copies len bytes at off into buf (the shared buffer's view if there is one). */
long lx_fd_read_at(int fd, void *buf, uint64_t len, uint64_t off) {
    long total = 0;
    while ((uint64_t)total < len) {
        long n = lx_pread(fd, (uint8_t *)buf + total, (size_t)(len - (uint64_t)total), (long)(off + (uint64_t)total), 0);
        if (n <= 0) return total ? total : n;
        total += n;
    }
    return total;
}

static long shm_rw(struct fd *f, void *buf, size_t len, uint64_t at, int is_write, int advance) {
    mutexLock(&g_shm_lock);
    struct shm *m = f->shm;
    long r;
    if (is_write) {
        if ((f->flags & L_O_APPEND) && advance) at = m->size;
        r = at + len > m->size ? shm_resize(m, at + len) : 0;
        if (!r) {
            memcpy(m->data + at, buf, len);
            r = (long)len;
        }
    } else {
        r = at < m->size ? (long)(m->size - at < len ? m->size - at : len) : 0;
        if (r) memcpy(buf, m->data + at, (size_t)r);
    }
    if (r > 0 && advance) f->pos = at + (uint64_t)r;
    mutexUnlock(&g_shm_lock);
    return r;
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
        long hr = 0;
        int exists = hfile_stat_path(hp, &st, &hr) ? hr == 0 : stat(hp, &st) == 0;
        if (!exists && !hr && (flags & L_O_ACCMODE) == 0 && !(flags & L_O_CREAT) && romfs_path(lp, hp, sizeof hp))
            exists = stat(hp, &st) == 0;
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
            long err = 0;
            struct hfile *hf = hfile_open(hp, nf, &err);
            if (!hf) return err;
            f = fd_new(FD_FILE);
            f->hf = hf;
            f->writable = (flags & L_O_ACCMODE) != 0;
        }
        f->path = strdup(hp);
        if (f->kind == FD_FILE) {
            mutexLock(&g_shm_lock);
            struct shm *m = shm_for_path(hp);
            if (m) {
                shm_attach(f, m);
                f->pos = 0;
                if (flags & L_O_TRUNC) shm_resize(m, 0);
            }
            mutexUnlock(&g_shm_lock);
        }
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
        if (f->shm) return shm_rw(f, buf, len, f->pos, 0, 1);
        long n = hfile_rw(f->hf, buf, len, (int64_t)f->pos, 0, NULL);
        if (n > 0) f->pos += (uint64_t)n;
        return n;
    }
    case FD_DIR: return -21; /* EISDIR */
    case FD_PIPE_R: return pipe_read(f, buf, len);
    case FD_SOCKET: return lx_sock_recv(f, buf, len, 0);
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
        if (f->shm) return shm_rw(f, (void *)buf, len, f->pos, 1, 1);
        uint64_t at;
        long n = hfile_rw(f->hf, (void *)buf, len, (f->flags & L_O_APPEND) ? -1 : (int64_t)f->pos, 1, &at);
        if (n >= 0) f->pos = at + (uint64_t)n;
        return n;
    }
    case FD_PIPE_W: return pipe_write(f, buf, len);
    case FD_SOCKET: return lx_sock_send(f, buf, len, 0);
    default: return -LX_EBADF;
    }
}

long lx_pread(int fd, void *buf, size_t len, long off, int is_write) {
    struct fd *f = fd_get(fd);
    if (!f) return -LX_EBADF;
    if (f->kind != FD_FILE) return -LX_ESPIPE;
    if (f->shm) return shm_rw(f, buf, len, (uint64_t)off, is_write, 0);
    return hfile_rw(f->hf, buf, len, off, is_write, NULL);
}

long lx_lseek(int fd, long off, int whence) {
    struct fd *f = fd_get(fd);
    if (!f) return -LX_EBADF;
    if (f->kind == FD_DIR) {
        if (off == 0 && whence == 0) {
            rewinddir(f->dir);
            f->dir_pos = 0;
            free(f->dir_stash);
            f->dir_stash = NULL;
            return 0;
        }
        return -LX_EINVAL;
    }
    if (f->kind != FD_FILE) return -LX_ESPIPE;
    if (f->shm) {
        mutexLock(&g_shm_lock);
        int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t)f->pos : (int64_t)f->shm->size;
        int64_t to = base + off;
        if (to >= 0) f->pos = (uint64_t)to;
        mutexUnlock(&g_shm_lock);
        return to < 0 ? -LX_EINVAL : (long)to;
    }
    int64_t base = 0;
    if (whence == 1) {
        base = (int64_t)f->pos;
    } else if (whence == 2) {
        struct stat st;
        long e = hfile_stat(f->hf, &st);
        if (e) return e;
        base = st.st_size;
    } else if (whence != 0) {
        return -LX_EINVAL;
    }
    if (base + off < 0) return -LX_EINVAL;
    f->pos = (uint64_t)(base + off);
    return (long)f->pos;
}

long lx_fstat(int fd, void *out) {
    struct fd *f = fd_get(fd);
    struct stat st;
    if (!f) return -LX_EBADF;
    switch (f->kind) {
    case FD_CONSOLE: special_stat(0020000 | 0620, 10 + (uint64_t)fd, out); return 0; /* character device */
    case FD_NULL: case FD_ZERO: case FD_RANDOM: special_stat(0020000 | 0666, 20 + f->kind, out); return 0;
    case FD_PIPE_R: case FD_PIPE_W: special_stat(0010000 | 0600, (uintptr_t)f->pipe, out); return 0; /* FIFO */
    case FD_SOCKET: special_stat(0140000 | 0777, 100000 + (uint64_t)f->host_fd, out); return 0;
    case FD_EPOLL: special_stat(0600, (uintptr_t)f->epoll, out); return 0;
    case FD_FILE: {
        long e = hfile_stat(f->hf, &st);
        if (e) return e;
        to_lx_stat(&st, f->path, out);
        if (f->shm) ((struct lx_stat *)out)->size = (int64_t)f->shm->size;
        return 0;
    }
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
    long hr;
    if (hfile_stat_path(hp, &st, &hr)) {
        if (hr) return hr;
    } else if (stat(hp, &st) != 0) {
        int e = errno;
        if (!romfs_path(lp, hp, sizeof hp) || stat(hp, &st) != 0) return lx_errno(e);
    }
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
    /* An entry that doesn't fit is kept in f->dir_stash for the next call: newlib's telldir/seekdir don't work on
       libnx's DIR (the first directory listing over one buffer killed the process: osu!'s font cache). */
    for (;;) {
        const char *name;
        unsigned char type;
        if (f->dir_stash) {
            name = f->dir_stash;
            type = f->dir_stash_type;
        } else {
            struct dirent *e = readdir(f->dir);
            if (!e) break;
            name = e->d_name;
            type = e->d_type == DT_DIR ? 4 : e->d_type == DT_REG ? 8 : 0;
        }
        size_t name_len = strlen(name);
        size_t reclen = (19 + name_len + 1 + 7) & ~(size_t)7;
        if (used + reclen > cap) {
            if (!f->dir_stash) {
                f->dir_stash = strdup(name);
                f->dir_stash_type = type;
            }
            if (!used) return -LX_EINVAL;
            break;
        }
        uint8_t *rec = (uint8_t *)buf + used;
        char child[1300];
        snprintf(child, sizeof child, "%s/%s", f->path, name);
        uint64_t ino = path_inode(child);
        int64_t off = ++f->dir_pos;
        uint16_t rl = (uint16_t)reclen;
        memcpy(rec, &ino, 8);
        memcpy(rec + 8, &off, 8);
        memcpy(rec + 16, &rl, 2);
        rec[18] = type;
        memcpy(rec + 19, name, name_len + 1);
        used += reclen;
        if (f->dir_stash) {
            free(f->dir_stash);
            f->dir_stash = NULL;
        }
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
    if (!(flags & L_AT_REMOVEDIR) && hfile_unlink_path(hp, &r)) return r;
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
    if (f->shm) {
        mutexLock(&g_shm_lock);
        long r = len < 0 ? -LX_EINVAL : shm_resize(f->shm, (uint64_t)len);
        mutexUnlock(&g_shm_lock);
        return r;
    }
    return len < 0 ? -LX_EINVAL : hfile_truncate(f->hf, (uint64_t)len);
}

/* fallocate (mode 0) / posix_fallocate: make sure the file is at least off + len bytes long. Realm sizes its lock file
   this way before mapping it; answering success without growing the file made it retry forever (2026-11-14). */
long lx_fallocate(int fd, int mode, long off, long len) {
    struct fd *f = fd_get(fd);
    if (!f || f->kind != FD_FILE) return -LX_EBADF;
    if (mode != 0) return -95; /* EOPNOTSUPP: no punching holes etc. */
    if (off < 0 || len <= 0) return -LX_EINVAL;
    uint64_t end = (uint64_t)off + (uint64_t)len;
    if (f->shm) {
        mutexLock(&g_shm_lock);
        long r = end > f->shm->size ? shm_resize(f->shm, end) : 0;
        mutexUnlock(&g_shm_lock);
        return r;
    }
    struct stat st;
    long e = hfile_stat(f->hf, &st);
    if (e) return e;
    if ((uint64_t)st.st_size >= end) return 0;
    return hfile_truncate(f->hf, end);
}

long lx_fsync(int fd) {
    struct fd *f = fd_get(fd);
    if (!f) return -LX_EBADF;
    if (f->kind == FD_FILE && f->shm) {
        mutexLock(&g_shm_lock);
        long r = shm_flush(f->shm);
        mutexUnlock(&g_shm_lock);
        return r;
    }
    if (f->kind == FD_FILE) {
        mutexLock(&f->hf->lock);
        fsync(f->hf->fd);
        mutexUnlock(&f->hf->lock);
    }
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
    } else if (f->kind == FD_SOCKET) {
        ready = lx_sock_poll_state(f, events);
    } else if (f->kind == FD_EPOLL) {
        ready = 0;
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
