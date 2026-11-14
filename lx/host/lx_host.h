/* Host side of the musl island (devkitA64, libounce/newlib). Internal interfaces between the host's files. */
#ifndef LX_HOST_H
#define LX_HOST_H

#include <stddef.h>
#include <stdint.h>

#include "lx_abi.h"

/* Linux struct timespec (aarch64). */
struct lx_timespec {
    int64_t sec, nsec;
};

/* lx_host.c */
void lx_logf(const char *format, ...) __attribute__((format(printf, 1, 2)));
void lx_log_raw(const char *text, size_t len);

/* lx_syscall.c */
long lx_syscall(long n, long a, long b, long c, long d, long e, long f);
uint64_t lx_monotonic_ns(void);

/* lx_mem.c */
void lx_mem_set_main_stack(uintptr_t lo, uintptr_t hi);
long lx_mmap(uintptr_t addr, size_t len, int prot, int flags, int fd, long off);
long lx_munmap(uintptr_t addr, size_t len);
long lx_mremap(uintptr_t addr, size_t old_len, size_t new_len, int flags);
long lx_madvise(uintptr_t addr, size_t len, int advice);
size_t lx_mem_mapped(void);
void lx_mem_release_all(void);

/* lx_fd.c */
void lx_fd_init(const char *root);
const char *lx_fd_root(void);
void lx_fd_setup_console(void);
long lx_errno(int newlib_errno);
long lx_openat(int dirfd, const char *path, int flags, int mode);
long lx_close(int fd);
long lx_read(int fd, void *buf, size_t len);
long lx_write(int fd, const void *buf, size_t len);
long lx_pread(int fd, void *buf, size_t len, long off, int is_write);
long lx_lseek(int fd, long off, int whence);
long lx_fstat(int fd, void *out);
long lx_fstatat(int dirfd, const char *path, void *out, int flags);
long lx_faccessat(int dirfd, const char *path, int mode);
long lx_getdents64(int fd, void *buf, size_t cap);
long lx_mkdirat(int dirfd, const char *path, int mode);
long lx_unlinkat(int dirfd, const char *path, int flags);
long lx_renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath);
long lx_ftruncate(int fd, long len);
long lx_fsync(int fd);
long lx_dup(int fd, int newfd, int flags, int min);
long lx_fcntl(int fd, int cmd, long arg);
long lx_pipe2(int *fds, int flags);
int lx_fd_poll_state(int fd, int events);
long lx_getcwd(char *buf, size_t size);
long lx_chdir(const char *path);
long lx_statfs(void *out);

/* lx_vulkan.c (hosts built with VULKAN=1) */
void *lx_vk_get_instance_proc_addr(void *instance, const char *name);
void lx_vulkan_destroy_leftovers(void);

/* lx_thread.c */
int lx_gettid(void);
long lx_clone(int (*fn)(void *), void *stack, long flags, void *arg, int *ptid, void *tls, int *ctid);
long lx_futex(int32_t *uaddr, int op, int32_t val, const struct lx_timespec *timeout, int32_t *uaddr2, int32_t val3);
void lx_thread_exit(int code, uintptr_t unmap_base, size_t unmap_size) __attribute__((noreturn));
long lx_exit_group(int code);
int lx_threads_shutdown(void);
int lx_exiting(void);
void lx_exit_check(void);
void lx_sleep_ns(int64_t ns);
uint32_t lx_process_core_mask(void);

#endif
