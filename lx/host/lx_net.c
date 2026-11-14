/*
 * Sockets, epoll and name resolution for the island, on libnx's BSD sockets.
 *
 * Ported in spirit from the HBLauncher runtime's net.c (whose hardware findings carry over):
 * - Every host socket is non-blocking. A blocking call from the island is emulated with short polls, so no island
 *   thread parks inside the socket service (libnx has few service sessions) and a blocked thread still notices the
 *   program's exit. SO_RCVTIMEO / SO_SNDTIMEO are emulated the same way.
 * - epoll is emulated for .NET's SocketAsyncEngine (EPOLLET on every socket). Each socket has "armed" bits: a
 *   readiness is reported only while armed, reporting disarms it, and any receive (or send/connect) re-arms it, as
 *   new data then makes a new edge on Linux. A spurious report is harmless (the reader gets EAGAIN and waits again);
 *   a lost one would hang a connection.
 * - IPv4 only (AF_INET6 / AF_UNIX: EAFNOSUPPORT; the .NET apps set System.Net.DisableIPv6).
 *
 * The island speaks Linux: struct sockaddr without sin_len, SOL_SOCKET = 1, Linux SO_ and MSG_ values and errno.
 * libnx speaks BSD. Everything is translated here.
 */
#include <switch.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "lx_fd.h"

/* Linux values */
#define L_AF_UNIX 1
#define L_AF_INET 2
#define L_AF_INET6 10
#define L_SOCK_NONBLOCK 04000
#define L_SOCK_CLOEXEC 02000000
#define L_SOL_SOCKET 1
#define L_MSG_OOB 1
#define L_MSG_PEEK 2
#define L_MSG_DONTWAIT 0x40
#define L_MSG_WAITALL 0x100
#define L_MSG_NOSIGNAL 0x4000
#define L_EPOLLIN 0x001
#define L_EPOLLOUT 0x004
#define L_EPOLLERR 0x008
#define L_EPOLLHUP 0x010
#define L_EPOLLRDHUP 0x2000
#define L_EPOLLONESHOT (1u << 30)
#define L_EPOLLET (1u << 31)

#define POLL_STEP_MS 10

struct lx_sockaddr_in {
    uint16_t family;
    uint16_t port;
    uint32_t addr;
    uint8_t zero[8];
};

struct lx_iovec {
    void *base;
    size_t len;
};

struct lx_msghdr {
    void *name;
    uint32_t namelen;
    struct lx_iovec *iov;
    size_t iovlen;
    void *control;
    size_t controllen;
    int flags;
};

struct lx_epoll_event {
    uint32_t events;
    uint64_t data;
} __attribute__((aligned(8)));

/* ---- helpers ------------------------------------------------------------------------------------------------- */

#define ARM(f, bits) __atomic_fetch_or(&(f)->armed, (bits), __ATOMIC_SEQ_CST)
#define DISARM(f, bits) __atomic_fetch_and(&(f)->armed, ~(uint32_t)(bits), __ATOMIC_SEQ_CST)

static long lx_recvfrom_fd(struct fd *f, void *buf, size_t len, int lx_flags, void *addr, uint32_t *addrlen);
static long lx_sendto_fd(struct fd *f, const void *buf, size_t len, int lx_flags, const void *addr, uint32_t addrlen);

static int host_flags(int lx_flags) {
    int f = 0;
    if (lx_flags & L_MSG_OOB) f |= MSG_OOB;
    if (lx_flags & L_MSG_PEEK) f |= MSG_PEEK;
    if (lx_flags & L_MSG_WAITALL) f |= MSG_WAITALL;
    return f | MSG_DONTWAIT;
}

static long to_host_addr(const void *lx, uint32_t len, struct sockaddr_in *out) {
    const struct lx_sockaddr_in *in = lx;
    if (!lx || len < 8) return -LX_EINVAL;
    if (in->family == L_AF_INET6) return -97;
    if (in->family != L_AF_INET) return -97; /* EAFNOSUPPORT */
    memset(out, 0, sizeof *out);
    out->sin_len = sizeof *out;
    out->sin_family = AF_INET;
    out->sin_port = in->port;
    out->sin_addr.s_addr = in->addr;
    return 0;
}

static void to_lx_addr(const struct sockaddr_in *in, void *lx, uint32_t *lx_len) {
    struct lx_sockaddr_in out = { L_AF_INET, in->sin_port, in->sin_addr.s_addr, { 0 } };
    if (!lx || !lx_len) return;
    uint32_t n = *lx_len < sizeof out ? *lx_len : sizeof out;
    memcpy(lx, &out, n);
    *lx_len = sizeof out;
}

static struct fd *sock_get(int fd, long *err) {
    struct fd *f = fd_get(fd);
    *err = !f ? -LX_EBADF : f->kind != FD_SOCKET ? -88 /* ENOTSOCK */ : 0;
    return *err ? NULL : f;
}

/* Waits until the socket is ready for `events` (POLLIN/POLLOUT), up to timeout_ns (0 = forever). Returns 0 when
   ready, -EAGAIN on timeout. Ends the thread if the program exits meanwhile. */
static long wait_ready(struct fd *f, short events, int64_t timeout_ns) {
    int64_t deadline = timeout_ns > 0 ? (int64_t)lx_monotonic_ns() + timeout_ns : -1;
    for (;;) {
        struct pollfd p = { f->host_fd, events, 0 };
        int r = poll(&p, 1, POLL_STEP_MS);
        lx_exit_check();
        if (r > 0) return 0;
        if (r < 0 && errno != EINTR) return lx_errno(errno);
        if (deadline >= 0 && (int64_t)lx_monotonic_ns() >= deadline) return -LX_EAGAIN;
    }
}

/* ---- socket syscalls ------------------------------------------------------------------------------------------ */

long lx_socket(int domain, int type, int protocol) {
    int base = type & 0xf;
    if (domain != L_AF_INET) return -97; /* EAFNOSUPPORT: IPv4 only */
    int h = socket(AF_INET, base, protocol);
    if (h < 0) return lx_errno(errno);
    fcntl(h, F_SETFL, fcntl(h, F_GETFL, 0) | O_NONBLOCK);
    struct fd *f = fd_new(FD_SOCKET);
    if (!f) {
        close(h);
        return -LX_ENOMEM;
    }
    f->host_fd = h;
    f->sock_type = base;
    f->flags = (type & L_SOCK_NONBLOCK) ? L_O_NONBLOCK : 0;
    f->cloexec = !!(type & L_SOCK_CLOEXEC);
    f->armed = L_EPOLLIN | L_EPOLLOUT;
    int fd = fd_install(f, 0);
    if (fd < 0) fd_free(f);
    return fd;
}

long lx_connect(int fd, const void *addr, uint32_t len) {
    long err;
    struct fd *f = sock_get(fd, &err);
    struct sockaddr_in sa;
    if (!f) return err;
    if ((err = to_host_addr(addr, len, &sa))) return err;
    ARM(f, L_EPOLLOUT | L_EPOLLIN);
    if (connect(f->host_fd, (struct sockaddr *)&sa, sizeof sa) == 0) return 0;
    if (errno != EINPROGRESS && errno != EALREADY && errno != EAGAIN) return lx_errno(errno);
    if (f->flags & L_O_NONBLOCK) return -115; /* EINPROGRESS */
    if ((err = wait_ready(f, POLLOUT, f->sndtimeo_ns))) return err == -LX_EAGAIN ? -LX_ETIMEDOUT : err;
    int so_error = 0;
    socklen_t sl = sizeof so_error;
    getsockopt(f->host_fd, SOL_SOCKET, SO_ERROR, &so_error, &sl);
    return so_error ? lx_errno(so_error) : 0;
}

long lx_bind(int fd, const void *addr, uint32_t len) {
    long err;
    struct fd *f = sock_get(fd, &err);
    struct sockaddr_in sa;
    if (!f) return err;
    if ((err = to_host_addr(addr, len, &sa))) return err;
    return bind(f->host_fd, (struct sockaddr *)&sa, sizeof sa) == 0 ? 0 : lx_errno(errno);
}

long lx_listen(int fd, int backlog) {
    long err;
    struct fd *f = sock_get(fd, &err);
    if (!f) return err;
    return listen(f->host_fd, backlog) == 0 ? 0 : lx_errno(errno);
}

long lx_accept4(int fd, void *addr, uint32_t *len, int flags) {
    long err;
    struct fd *f = sock_get(fd, &err);
    if (!f) return err;
    for (;;) {
        struct sockaddr_in sa;
        socklen_t sl = sizeof sa;
        int h = accept(f->host_fd, (struct sockaddr *)&sa, &sl);
        if (h >= 0) {
            fcntl(h, F_SETFL, fcntl(h, F_GETFL, 0) | O_NONBLOCK);
            struct fd *n = fd_new(FD_SOCKET);
            if (!n) {
                close(h);
                return -LX_ENOMEM;
            }
            n->host_fd = h;
            n->sock_type = SOCK_STREAM;
            n->flags = (flags & L_SOCK_NONBLOCK) ? L_O_NONBLOCK : 0;
            n->cloexec = !!(flags & L_SOCK_CLOEXEC);
            n->armed = L_EPOLLIN | L_EPOLLOUT;
            to_lx_addr(&sa, addr, len);
            ARM(f, L_EPOLLIN);
            int nfd = fd_install(n, 0);
            if (nfd < 0) fd_free(n);
            return nfd;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) return lx_errno(errno);
        ARM(f, L_EPOLLIN);
        if (f->flags & L_O_NONBLOCK) return -LX_EAGAIN;
        if ((err = wait_ready(f, POLLIN, f->rcvtimeo_ns))) return err;
    }
}

long lx_sock_recv(struct fd *f, void *buf, size_t len, int lx_flags) {
    return lx_recvfrom_fd(f, buf, len, lx_flags, NULL, NULL);
}

static long lx_recvfrom_fd(struct fd *f, void *buf, size_t len, int lx_flags, void *addr, uint32_t *addrlen) {
    for (;;) {
        struct sockaddr_in sa;
        socklen_t sl = sizeof sa;
        ssize_t n = recvfrom(f->host_fd, buf, len, host_flags(lx_flags), addr ? (struct sockaddr *)&sa : NULL,
                             addr ? &sl : NULL);
        ARM(f, L_EPOLLIN); /* data consumed or none there: a new arrival is a new edge */
        if (n >= 0) {
            if (addr) to_lx_addr(&sa, addr, addrlen);
            return n;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) return lx_errno(errno);
        if ((f->flags & L_O_NONBLOCK) || (lx_flags & L_MSG_DONTWAIT)) return -LX_EAGAIN;
        long err = wait_ready(f, POLLIN, f->rcvtimeo_ns);
        if (err) return err;
    }
}

long lx_sock_send(struct fd *f, const void *buf, size_t len, int lx_flags) {
    return lx_sendto_fd(f, buf, len, lx_flags, NULL, 0);
}

static long lx_sendto_fd(struct fd *f, const void *buf, size_t len, int lx_flags, const void *addr, uint32_t addrlen) {
    struct sockaddr_in sa;
    if (addr) {
        long err = to_host_addr(addr, addrlen, &sa);
        if (err) return err;
    }
    size_t done = 0;
    for (;;) {
        ssize_t n = sendto(f->host_fd, (const uint8_t *)buf + done, len - done, host_flags(lx_flags),
                           addr ? (struct sockaddr *)&sa : NULL, addr ? sizeof sa : 0);
        ARM(f, L_EPOLLOUT);
        if (n >= 0) {
            done += (size_t)n;
            /* A blocking stream send sends everything, like Linux's. */
            if (done == len || f->sock_type != SOCK_STREAM || (f->flags & L_O_NONBLOCK) || (lx_flags & L_MSG_DONTWAIT))
                return (long)done;
            continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) return done ? (long)done : lx_errno(errno);
        if ((f->flags & L_O_NONBLOCK) || (lx_flags & L_MSG_DONTWAIT)) return done ? (long)done : -LX_EAGAIN;
        long err = wait_ready(f, POLLOUT, f->sndtimeo_ns);
        if (err) return done ? (long)done : err;
    }
}

long lx_recvfrom(int fd, void *buf, size_t len, int flags, void *addr, uint32_t *addrlen) {
    long err;
    struct fd *f = sock_get(fd, &err);
    return f ? lx_recvfrom_fd(f, buf, len, flags, addr, addrlen) : err;
}

long lx_sendto(int fd, const void *buf, size_t len, int flags, const void *addr, uint32_t addrlen) {
    long err;
    struct fd *f = sock_get(fd, &err);
    return f ? lx_sendto_fd(f, buf, len, flags, addr, addrlen) : err;
}

/* sendmsg/recvmsg: gathered into / scattered from one buffer (.NET uses them for vectored socket I/O). */
long lx_sendmsg(int fd, const void *msg, int flags) {
    const struct lx_msghdr *m = msg;
    long err;
    struct fd *f = sock_get(fd, &err);
    if (!f) return err;
    size_t total = 0;
    for (size_t i = 0; i < m->iovlen; i++) total += m->iov[i].len;
    uint8_t *buf = malloc(total ? total : 1);
    if (!buf) return -LX_ENOMEM;
    size_t at = 0;
    for (size_t i = 0; i < m->iovlen; i++) {
        memcpy(buf + at, m->iov[i].base, m->iov[i].len);
        at += m->iov[i].len;
    }
    long r = lx_sendto_fd(f, buf, total, flags, m->name, m->namelen);
    free(buf);
    return r;
}

long lx_recvmsg(int fd, void *msg, int flags) {
    struct lx_msghdr *m = msg;
    long err;
    struct fd *f = sock_get(fd, &err);
    if (!f) return err;
    size_t total = 0;
    for (size_t i = 0; i < m->iovlen; i++) total += m->iov[i].len;
    uint8_t *buf = malloc(total ? total : 1);
    if (!buf) return -LX_ENOMEM;
    long r = lx_recvfrom_fd(f, buf, total, flags, m->name, m->name ? &m->namelen : NULL);
    if (r > 0) {
        size_t at = 0;
        for (size_t i = 0; i < m->iovlen && at < (size_t)r; i++) {
            size_t n = m->iov[i].len < (size_t)r - at ? m->iov[i].len : (size_t)r - at;
            memcpy(m->iov[i].base, buf + at, n);
            at += n;
        }
    }
    m->controllen = 0;
    m->flags = 0;
    free(buf);
    return r;
}

long lx_shutdown(int fd, int how) {
    long err;
    struct fd *f = sock_get(fd, &err);
    if (!f) return err;
    return shutdown(f->host_fd, how) == 0 ? 0 : lx_errno(errno);
}

long lx_getsockname(int fd, void *addr, uint32_t *len, int peer) {
    long err;
    struct fd *f = sock_get(fd, &err);
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    if (!f) return err;
    int r = peer ? getpeername(f->host_fd, (struct sockaddr *)&sa, &sl) : getsockname(f->host_fd, (struct sockaddr *)&sa, &sl);
    if (r != 0) return lx_errno(errno);
    to_lx_addr(&sa, addr, len);
    return 0;
}

/* Linux SOL_SOCKET option -> BSD, or -1 for options handled here. */
static int host_sockopt(int name) {
    switch (name) {
    case 2: return SO_REUSEADDR;
    case 6: return SO_BROADCAST;
    case 7: return SO_SNDBUF;
    case 8: return SO_RCVBUF;
    case 9: return SO_KEEPALIVE;
    case 13: return SO_LINGER;
    case 15: return SO_REUSEPORT;
    default: return -1;
    }
}

long lx_setsockopt(int fd, int level, int name, const void *val, uint32_t len) {
    long err;
    struct fd *f = sock_get(fd, &err);
    if (!f) return err;
    if (level == L_SOL_SOCKET) {
        if (name == 20 || name == 21) { /* SO_RCVTIMEO / SO_SNDTIMEO: struct timeval {i64 sec, i64 usec} */
            const int64_t *tv = val;
            int64_t ns = len >= 16 ? tv[0] * 1000000000ll + tv[1] * 1000 : 0;
            if (name == 20) f->rcvtimeo_ns = ns;
            else f->sndtimeo_ns = ns;
            return 0;
        }
        int h = host_sockopt(name);
        if (h < 0) return 0; /* not meaningful here (SO_PRIORITY, SO_NO_CHECK, ...): accepted */
        return setsockopt(f->host_fd, SOL_SOCKET, h, val, len) == 0 ? 0 : lx_errno(errno);
    }
    if (level == IPPROTO_TCP) {
        if (name == 1) /* TCP_NODELAY */
            return setsockopt(f->host_fd, IPPROTO_TCP, TCP_NODELAY, val, len) == 0 ? 0 : lx_errno(errno);
        return 0; /* keep-alive tuning (TCP_KEEPIDLE/INTVL/CNT) and the like: accepted */
    }
    if (level == IPPROTO_IP) return 0;
    return -92; /* ENOPROTOOPT (IPv6 options) */
}

long lx_getsockopt(int fd, int level, int name, void *val, uint32_t *len) {
    long err;
    struct fd *f = sock_get(fd, &err);
    if (!f) return err;
    int out = 0;
    if (level == L_SOL_SOCKET) {
        socklen_t sl = sizeof out;
        if (name == 4) { /* SO_ERROR */
            getsockopt(f->host_fd, SOL_SOCKET, SO_ERROR, &out, &sl);
            out = out ? (int)-lx_errno(out) : 0;
        } else if (name == 3) { /* SO_TYPE */
            out = f->sock_type;
        } else if (name == 20 || name == 21) {
            int64_t ns = name == 20 ? f->rcvtimeo_ns : f->sndtimeo_ns;
            int64_t tv[2] = { ns / 1000000000ll, (ns % 1000000000ll) / 1000 };
            uint32_t n = *len < sizeof tv ? *len : sizeof tv;
            memcpy(val, tv, n);
            *len = n;
            return 0;
        } else {
            int h = host_sockopt(name);
            if (h >= 0 && getsockopt(f->host_fd, SOL_SOCKET, h, &out, &sl) != 0) return lx_errno(errno);
        }
    } else if (level == IPPROTO_TCP && name == 1) {
        socklen_t sl = sizeof out;
        getsockopt(f->host_fd, IPPROTO_TCP, TCP_NODELAY, &out, &sl);
    } else if (level != IPPROTO_TCP && level != IPPROTO_IP) {
        return -92;
    }
    uint32_t n = *len < sizeof out ? *len : sizeof out;
    memcpy(val, &out, n);
    *len = n;
    return 0;
}

long lx_sock_ioctl(int fd, unsigned long req, void *arg) {
    long err;
    struct fd *f = sock_get(fd, &err);
    if (!f) return err;
    if (req == 0x541B) { /* FIONREAD */
        int n = 0;
        if (ioctl(f->host_fd, FIONREAD, &n) != 0) return lx_errno(errno);
        *(int *)arg = n;
        return 0;
    }
    if (req == 0x5421) { /* FIONBIO */
        if (*(int *)arg) f->flags |= L_O_NONBLOCK;
        else f->flags &= ~L_O_NONBLOCK;
        return 0;
    }
    return -LX_ENOTTY;
}

int lx_sock_poll_state(struct fd *f, int events) {
    struct pollfd p = { f->host_fd, (short)(events & (POLLIN | POLLOUT | POLLPRI)), 0 };
    if (poll(&p, 1, 0) <= 0) return 0;
    return p.revents & (POLLIN | POLLPRI | POLLOUT | POLLERR | POLLHUP | POLLNVAL);
}

/* ---- epoll ----------------------------------------------------------------------------------------------------- */

struct epoll_entry {
    int fd;
    uint64_t gen;    /* the open file the fd number meant when it was added (Linux removes an entry on close; a
                        reused fd number must not inherit it) */
    uint32_t events;
    uint64_t data;
    int disabled; /* EPOLLONESHOT after its report */
};

struct epoll_set {
    Mutex lock;
    struct epoll_entry *e;
    int count, cap;
};

void lx_epoll_free(struct epoll_set *set) {
    if (!set) return;
    free(set->e);
    free(set);
}

long lx_epoll_create(int flags) {
    struct fd *f = fd_new(FD_EPOLL);
    struct epoll_set *set = calloc(1, sizeof *set);
    if (!f || !set) {
        free(f);
        free(set);
        return -LX_ENOMEM;
    }
    mutexInit(&set->lock);
    f->epoll = set;
    f->cloexec = !!(flags & L_O_CLOEXEC);
    int fd = fd_install(f, 0);
    if (fd < 0) fd_free(f);
    return fd;
}

long lx_epoll_ctl(int epfd, int op, int fd, const void *event) {
    const struct lx_epoll_event *ev = event;
    struct fd *f = fd_get(epfd);
    if (!f || f->kind != FD_EPOLL) return -LX_EBADF;
    if (!fd_get(fd)) return -LX_EBADF;
    struct epoll_set *set = f->epoll;
    mutexLock(&set->lock);
    struct fd *target = fd_get(fd);
    /* Entries whose fd was closed (and maybe reused for another file) are gone, as on Linux. */
    for (int k = 0; k < set->count; k++) {
        struct fd *cur = fd_get(set->e[k].fd);
        if (!cur || cur->gen != set->e[k].gen) set->e[k--] = set->e[--set->count];
    }
    int i;
    for (i = 0; i < set->count && set->e[i].fd != fd; i++) {
    }
    long r = 0;
    if (op == 1) { /* ADD */
        if (i < set->count) {
            r = -17; /* EEXIST */
        } else {
            if (set->count == set->cap) {
                int cap = set->cap ? set->cap * 2 : 16;
                struct epoll_entry *n = realloc(set->e, sizeof *n * (size_t)cap);
                if (!n) {
                    mutexUnlock(&set->lock);
                    return -LX_ENOMEM;
                }
                set->e = n;
                set->cap = cap;
            }
            set->e[set->count++] = (struct epoll_entry){ fd, target->gen, ev->events, ev->data, 0 };
            struct fd *t = fd_get(fd);
            if (t && t->kind == FD_SOCKET) ARM(t, L_EPOLLIN | L_EPOLLOUT); /* report what is ready now */
        }
    } else if (op == 2) { /* DEL */
        if (i == set->count) r = -LX_ENOENT;
        else set->e[i] = set->e[--set->count];
    } else if (op == 3) { /* MOD */
        if (i == set->count) {
            r = -LX_ENOENT;
        } else {
            set->e[i].events = ev->events;
            set->e[i].data = ev->data;
            set->e[i].disabled = 0;
            struct fd *t = fd_get(fd);
            if (t && t->kind == FD_SOCKET) ARM(t, L_EPOLLIN | L_EPOLLOUT);
        }
    } else {
        r = -LX_EINVAL;
    }
    mutexUnlock(&set->lock);
    return r;
}

/* One pass over the set: fills out with what is ready (and armed). */
static int epoll_collect(struct epoll_set *set, struct lx_epoll_event *out, int max) {
    int n = 0;
    mutexLock(&set->lock);
    for (int i = 0; i < set->count && n < max; i++) {
        struct epoll_entry *e = &set->e[i];
        struct fd *t = fd_get(e->fd);
        if (!t || t->gen != e->gen) { /* closed without EPOLL_CTL_DEL (maybe reused): Linux drops it too */
            set->e[i--] = set->e[--set->count];
            continue;
        }
        if (e->disabled) continue;
        int want = 0;
        if (e->events & L_EPOLLIN) want |= POLLIN;
        if (e->events & L_EPOLLOUT) want |= POLLOUT;
        int ready = lx_fd_poll_state(e->fd, want);
        uint32_t ev = 0;
        if (ready & (POLLIN | POLLPRI)) ev |= L_EPOLLIN;
        if (ready & POLLOUT) ev |= L_EPOLLOUT;
        if (ready & POLLERR) ev |= L_EPOLLERR;
        if (ready & POLLHUP) ev |= L_EPOLLHUP | (e->events & L_EPOLLRDHUP);
        ev &= e->events | L_EPOLLERR | L_EPOLLHUP;
        if ((e->events & L_EPOLLET) && t->kind == FD_SOCKET) {
            /* Edge-triggered: report only what is armed; an error or hangup comes with an armed direction. */
            uint32_t armed = __atomic_load_n(&t->armed, __ATOMIC_SEQ_CST);
            uint32_t report = ev & armed & (L_EPOLLIN | L_EPOLLOUT);
            if ((ev & (L_EPOLLERR | L_EPOLLHUP)) && (armed & (L_EPOLLIN | L_EPOLLOUT)))
                report |= ev & (L_EPOLLERR | L_EPOLLHUP | L_EPOLLRDHUP);
            if (!report) continue;
            DISARM(t, (report & (L_EPOLLERR | L_EPOLLHUP)) ? (L_EPOLLIN | L_EPOLLOUT) : (report & (L_EPOLLIN | L_EPOLLOUT)));
            ev = report;
        }
        if (!ev) continue;
        out[n].events = ev;
        out[n].data = e->data;
        n++;
        if (e->events & L_EPOLLONESHOT) e->disabled = 1;
    }
    mutexUnlock(&set->lock);
    return n;
}

long lx_epoll_wait(int epfd, void *events, int max, int timeout_ms) {
    struct lx_epoll_event *out = events;
    struct fd *f = fd_get(epfd);
    if (!f || f->kind != FD_EPOLL) return -LX_EBADF;
    if (max <= 0) return -LX_EINVAL;
    int64_t deadline = timeout_ms >= 0 ? (int64_t)lx_monotonic_ns() + (int64_t)timeout_ms * 1000000 : -1;
    for (;;) {
        int n = epoll_collect(f->epoll, out, max);
        if (n) return n;
        int64_t now = (int64_t)lx_monotonic_ns();
        if (deadline >= 0 && now >= deadline) return 0;
        /* Sleep until a socket in the set has activity, at most POLL_STEP_MS (pipes and timeouts are checked on the
           next pass). */
        struct pollfd p[64];
        int np = 0;
        mutexLock(&f->epoll->lock);
        for (int i = 0; i < f->epoll->count && np < 64; i++) {
            struct fd *t = fd_get(f->epoll->e[i].fd);
            if (t && t->gen == f->epoll->e[i].gen && t->kind == FD_SOCKET && !f->epoll->e[i].disabled) {
                p[np].fd = t->host_fd;
                /* Only what could be reported (armed, or level-triggered): anything else would wake us at once. */
                uint32_t ev = f->epoll->e[i].events, armed = (ev & L_EPOLLET) ? t->armed : ~0u;
                p[np].events = (short)(((ev & L_EPOLLIN) && (armed & L_EPOLLIN) ? POLLIN : 0) |
                                       ((ev & L_EPOLLOUT) && (armed & L_EPOLLOUT) ? POLLOUT : 0));
                p[np].revents = 0;
                np++;
            }
        }
        mutexUnlock(&f->epoll->lock);
        int step = POLL_STEP_MS;
        if (deadline >= 0 && (deadline - now) / 1000000 < step) step = (int)((deadline - now) / 1000000) + 1;
        if (np) poll(p, (nfds_t)np, step);
        else lx_sleep_ns((int64_t)step * 1000000);
        lx_exit_check();
    }
}

/* ---- name resolution ------------------------------------------------------------------------------------------- */

/* IPv4 addresses for a host name, from the console's resolver. Returns how many (network byte order), 0 if the name
   doesn't resolve, -1 on a resolver failure. */
int lx_resolve_ipv4(const char *name, uint32_t *out, int max) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int r = getaddrinfo(name, NULL, &hints, &res);
    if (r != 0) {
        lx_logf("lx-net: resolve %s failed (%d)", name, r);
        return r == EAI_NONAME ? 0 : -1;
    }
    int n = 0;
    for (struct addrinfo *a = res; a && n < max; a = a->ai_next) {
        if (a->ai_family != AF_INET) continue;
        uint32_t ip = ((struct sockaddr_in *)a->ai_addr)->sin_addr.s_addr;
        int dup = 0;
        for (int k = 0; k < n; k++) dup |= out[k] == ip;
        if (!dup) out[n++] = ip;
    }
    freeaddrinfo(res);
    return n;
}
