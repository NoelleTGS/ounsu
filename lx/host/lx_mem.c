/*
 * mmap and friends for the island, from the newlib heap.
 *
 * Anonymous mappings only (file mappings are lx_fd.c's), each one an aligned heap block:
 * - MAP_FIXED anonymous over memory we own = zero-fill and succeed: the .NET GC decommits that way and expects the
 *   pages to read as zero when it re-commits them.
 * - madvise(DONTNEED) zeroes too.
 * - The GC reserves its whole region range up front (System.GC.RegionRange); the heap commits memory when it is
 *   touched, so only what the GC uses costs real memory.
 * - Partial munmap of a block can't give memory back: the range is zeroed and the block stays until all of it is
 *   unmapped.
 */
#include <switch.h>

#include <stdlib.h>
#include <string.h>

#include "lx_host.h"

#define PAGE 4096u
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAX_BLOCKS 8192

struct block {
    uintptr_t base;
    size_t size;
    size_t unmapped; /* bytes of it already munmapped */
};

static struct block g_blocks[MAX_BLOCKS];
static int g_nblocks;
static size_t g_mapped;
static Mutex g_lock;
static uintptr_t g_main_stack_lo, g_main_stack_hi;

void lx_mem_set_main_stack(uintptr_t lo, uintptr_t hi) {
    g_main_stack_lo = lo;
    g_main_stack_hi = hi;
}

size_t lx_mem_mapped(void) {
    return g_mapped;
}

static struct block *find(uintptr_t addr) {
    for (int i = 0; i < g_nblocks; i++)
        if (addr >= g_blocks[i].base && addr < g_blocks[i].base + g_blocks[i].size) return &g_blocks[i];
    return NULL;
}

long lx_mmap(uintptr_t addr, size_t len, int prot, int flags, int fd, long off) {
    (void)prot;
    (void)off;
    if (!len) return -LX_EINVAL;
    len = (len + PAGE - 1) & ~(size_t)(PAGE - 1);
    if (!(flags & MAP_ANONYMOUS)) {
        if (flags & MAP_SHARED) { /* the file's shared buffer (lx_fd.c) */
            long err = 0;
            void *p = lx_fd_map_shared(fd, len, (uint64_t)off, &err);
            return p ? (long)(uintptr_t)p : err;
        }
        /* MAP_PRIVATE: a private copy */
        long r = lx_mmap(0, len, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (r < 0) return r;
        long got = lx_fd_read_at(fd, (void *)r, len, (uint64_t)off);
        if (got < 0) {
            lx_munmap((uintptr_t)r, len);
            return got;
        }
        return r;
    }

    mutexLock(&g_lock);
    if (flags & MAP_FIXED) {
        struct block *b = find(addr);
        long r = -LX_ENOMEM;
        if (b && addr + len <= b->base + b->size) {
            memset((void *)addr, 0, len);
            r = (long)addr;
        }
        mutexUnlock(&g_lock);
        return r;
    }
    if (g_nblocks == MAX_BLOCKS) {
        mutexUnlock(&g_lock);
        lx_logf("lx: mmap: block table full");
        return -LX_ENOMEM;
    }
    void *p = aligned_alloc(PAGE, len);
    if (!p) {
        mutexUnlock(&g_lock);
        return -LX_ENOMEM;
    }
    memset(p, 0, len);
    g_blocks[g_nblocks++] = (struct block){ (uintptr_t)p, len, 0 };
    g_mapped += len;
    mutexUnlock(&g_lock);
    return (long)(uintptr_t)p;
}

long lx_munmap(uintptr_t addr, size_t len) {
    if (lx_fd_unmap_shared(addr)) return 0;
    len = (len + PAGE - 1) & ~(size_t)(PAGE - 1);
    mutexLock(&g_lock);
    struct block *b = find(addr);
    if (!b) {
        mutexUnlock(&g_lock);
        return 0; /* Linux: unmapping nothing is fine */
    }
    if (addr == b->base && len >= b->size) {
        b->unmapped = b->size;
    } else {
        size_t end = addr + len > b->base + b->size ? b->base + b->size - addr : len;
        memset((void *)addr, 0, end);
        b->unmapped += end;
    }
    if (b->unmapped >= b->size) {
        free((void *)b->base);
        g_mapped -= b->size;
        *b = g_blocks[--g_nblocks];
    }
    mutexUnlock(&g_lock);
    return 0;
}

long lx_mremap(uintptr_t addr, size_t old_len, size_t new_len, int flags) {
    (void)old_len;
    (void)new_len;
    (void)flags;
    /* musl's pthread_getattr_np finds the main stack's bottom by mremap probes going down until one fails with
       something other than ENOMEM: say "mapped" (ENOMEM) inside the main stack, "unmapped" (EFAULT) elsewhere. */
    long shared = lx_fd_remap_shared(addr, new_len);
    if (shared) return shared;
    if (addr >= g_main_stack_lo && addr < g_main_stack_hi) return -LX_ENOMEM;
    if (find(addr)) return -LX_ENOMEM; /* can't grow a heap block in place */
    return -LX_EFAULT;
}

long lx_madvise(uintptr_t addr, size_t len, int advice) {
    if (advice == 4 /* MADV_DONTNEED */) {
        mutexLock(&g_lock);
        struct block *b = find(addr);
        if (b && addr + len <= b->base + b->size) memset((void *)addr, 0, len);
        mutexUnlock(&g_lock);
    }
    return 0;
}

/* At exit, once no island thread can run any more: give every mapping back. */
void lx_mem_release_all(void) {
    mutexLock(&g_lock);
    for (int i = 0; i < g_nblocks; i++) free((void *)g_blocks[i].base);
    g_nblocks = 0;
    g_mapped = 0;
    mutexUnlock(&g_lock);
}
