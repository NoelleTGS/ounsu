/* The host's file descriptor table, shared by lx_fd.c (files, pipes, devices) and lx_net.c (sockets, epoll). */
#ifndef LX_FD_H
#define LX_FD_H

#include <switch.h>

#include <dirent.h>

#include "lx_host.h"

#define MAX_FDS 1024

/* Linux open/status flags (aarch64) */
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

enum fd_kind { FD_FREE, FD_CONSOLE, FD_NULL, FD_ZERO, FD_RANDOM, FD_FILE, FD_DIR, FD_PIPE_R, FD_PIPE_W, FD_SOCKET, FD_EPOLL };

struct pipe_buf {
    Mutex lock;
    CondVar cond;
    uint8_t *data;
    size_t head, len;
    int readers, writers;
};

struct epoll_set; /* lx_net.c */

struct fd {
    enum fd_kind kind;
    int host_fd;      /* FD_FILE: newlib fd; FD_SOCKET: libnx socket (always non-blocking on the host side) */
    DIR *dir;         /* FD_DIR */
    long dir_pos;     /* FD_DIR: entries returned so far */
    char *path;       /* FD_FILE/FD_DIR: host path (for fstat on directories, getdents, relative lookups) */
    char *lx_path;    /* the island's path, for *at() relative to this fd */
    int flags;        /* Linux status flags (O_APPEND, O_NONBLOCK) */
    int cloexec;
    struct pipe_buf *pipe;
    int refs;         /* fds sharing this description (dup) */
    uint64_t gen;     /* unique per open file description (epoll entries compare it: fd numbers and addresses get reused) */

    /* FD_SOCKET */
    int sock_type;                    /* SOCK_STREAM / SOCK_DGRAM */
    volatile uint32_t armed;          /* epoll edge-trigger bits (EPOLLIN/EPOLLOUT) that may be reported again */
    int64_t rcvtimeo_ns, sndtimeo_ns; /* SO_RCVTIMEO / SO_SNDTIMEO, emulated (0 = forever) */

    /* FD_EPOLL */
    struct epoll_set *epoll;
};

struct fd *fd_get(int fd);
struct fd *fd_new(enum fd_kind kind);
int fd_install(struct fd *f, int min);
void fd_free(struct fd *f);

/* lx_net.c, called by lx_fd.c for socket and epoll fds */
long lx_sock_recv(struct fd *f, void *buf, size_t len, int lx_flags);
long lx_sock_send(struct fd *f, const void *buf, size_t len, int lx_flags);
int lx_sock_poll_state(struct fd *f, int events);
void lx_epoll_free(struct epoll_set *set);

#endif
