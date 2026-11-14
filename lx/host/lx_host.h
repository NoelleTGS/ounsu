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

/* lx_thread.c */
int lx_gettid(void);
long lx_clone(int (*fn)(void *), void *stack, long flags, void *arg, int *ptid, void *tls, int *ctid);
long lx_futex(int32_t *uaddr, int op, int32_t val, const struct lx_timespec *timeout, int32_t *uaddr2, int32_t val3);
void lx_thread_exit(int code, uintptr_t unmap_base, size_t unmap_size) __attribute__((noreturn));
long lx_exit_group(int code);
int lx_threads_shutdown(void);
uint32_t lx_process_core_mask(void);

#endif
