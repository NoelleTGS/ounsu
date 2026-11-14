/*
 * hello-island: the first musl program on the lx host (Phase 1). Built with the aarch64-linux-musl toolchain.
 *
 * Checks what everything later depends on: stdio through the host, malloc (small, large, many), clocks and sleep,
 * random bytes, and above all threads with thread-local storage. The island owns tpidr_el0 while libnx keeps its own
 * thread data off tpidrro_el0: four workers each write their own __thread values and must read them back unchanged
 * after the others ran. Ends with a summary line and the number of failed checks as the exit status.
 */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

static int failures, checks;

#define CHECK(cond, ...)                         \
    do {                                         \
        checks++;                                \
        if (!(cond)) {                           \
            failures++;                          \
            printf("hello: FAIL: " __VA_ARGS__); \
            printf("\n");                        \
        }                                        \
    } while (0)

static __thread int tls_value = 7;  /* .tdata: initialised per thread */
static __thread char tls_name[32];  /* .tbss */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int started;

static uint64_t now_ms(clockid_t clock) {
    struct timespec ts;
    clock_gettime(clock, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void *worker(void *arg) {
    int id = (int)(intptr_t)arg;
    int ok_initial = tls_value == 7 && tls_name[0] == 0;

    tls_value = 100 + id;
    snprintf(tls_name, sizeof tls_name, "worker %d", id);

    /* Everyone writes, then everyone waits for the others before reading back. */
    pthread_mutex_lock(&lock);
    started++;
    pthread_cond_broadcast(&cond);
    while (started < 4) pthread_cond_wait(&cond, &lock);
    pthread_mutex_unlock(&lock);
    nanosleep(&(struct timespec){ 0, 20 * 1000000 }, NULL);

    char expect[32];
    snprintf(expect, sizeof expect, "worker %d", id);
    int ok = ok_initial && tls_value == 100 + id && strcmp(tls_name, expect) == 0;
    printf("hello: %s: thread-local values %s (tid-local value %d)\n", expect, ok ? "ok" : "WRONG", tls_value);
    return (void *)(intptr_t)ok;
}

static volatile int detached_ran;

static void *detached(void *arg) {
    (void)arg;
    detached_ran = 1;
    return NULL;
}

int main(int argc, char **argv) {
    printf("hello: musl island running, argc %d, argv[0] %s\n", argc, argc > 0 ? argv[0] : "(none)");
    CHECK(getenv("HOME") != NULL, "environment missing HOME");

    /* malloc: many small, one large */
    void *small[2000];
    for (int i = 0; i < 2000; i++) {
        small[i] = malloc(16 + (i % 200) * 8);
        CHECK(small[i] != NULL, "small malloc %d", i);
        if (small[i]) memset(small[i], i & 0xff, 16);
    }
    for (int i = 0; i < 2000; i++) free(small[i]);
    size_t big_size = 64u << 20;
    char *big = malloc(big_size);
    CHECK(big != NULL, "64 MiB malloc");
    if (big) {
        memset(big, 0x5a, big_size);
        CHECK(big[0] == 0x5a && big[big_size - 1] == 0x5a, "64 MiB block contents");
        free(big);
    }

    /* clocks */
    uint64_t t0 = now_ms(CLOCK_MONOTONIC);
    nanosleep(&(struct timespec){ 0, 100 * 1000000 }, NULL);
    uint64_t slept = now_ms(CLOCK_MONOTONIC) - t0;
    printf("hello: slept 100 ms, monotonic clock says %llu ms\n", (unsigned long long)slept);
    CHECK(slept >= 95 && slept < 200, "nanosleep(100 ms) took %llu ms", (unsigned long long)slept);
    time_t wall = time(NULL);
    printf("hello: wall clock %s", ctime(&wall));
    CHECK(wall > 1700000000, "wall clock looks unset (%lld)", (long long)wall);

    /* random */
    unsigned char rnd[16] = { 0 };
    CHECK(getrandom(rnd, sizeof rnd, 0) == (ssize_t)sizeof rnd, "getrandom");
    printf("hello: random bytes %02x%02x%02x%02x...\n", rnd[0], rnd[1], rnd[2], rnd[3]);

    /* threads + TLS */
    printf("hello: %ld CPUs online\n", sysconf(_SC_NPROCESSORS_ONLN));
    tls_value = 1;
    strcpy(tls_name, "main");
    pthread_t threads[4];
    for (int i = 0; i < 4; i++)
        CHECK(pthread_create(&threads[i], NULL, worker, (void *)(intptr_t)i) == 0, "pthread_create %d", i);
    for (int i = 0; i < 4; i++) {
        void *ok = NULL;
        CHECK(pthread_join(threads[i], &ok) == 0, "pthread_join %d", i);
        CHECK(ok == (void *)1, "worker %d thread-local values", i);
    }
    CHECK(tls_value == 1 && strcmp(tls_name, "main") == 0, "main thread's thread-local values changed");

    /* a detached thread that unmaps its own stack when it ends */
    pthread_t d;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    CHECK(pthread_create(&d, &attr, detached, NULL) == 0, "detached pthread_create");
    nanosleep(&(struct timespec){ 0, 50 * 1000000 }, NULL);
    CHECK(detached_ran, "detached thread didn't run");

    /* a timed wait that must time out */
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += 50 * 1000000;
    if (until.tv_nsec >= 1000000000) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000;
    }
    pthread_mutex_lock(&lock);
    int r = pthread_cond_timedwait(&cond, &lock, &until);
    pthread_mutex_unlock(&lock);
    CHECK(r == ETIMEDOUT, "pthread_cond_timedwait returned %d, expected ETIMEDOUT", r);

    printf("hello: done, %d of %d checks passed\n", checks - failures, checks);
    return failures;
}
