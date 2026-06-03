/*
 * bench_cas.c — Benchmark 1: CAS latency on a single shared cache line.
 *
 * DESIGN
 * ──────
 * Two threads share one _Atomic int flag on a single cache line.
 * They take turns owning the line via CAS (LOCK CMPXCHG on x86).
 *
 * Ping measures; pong responds silently.
 *
 * The key invariant that prevents deadlock:
 *   - Flag starts as PONG (not PING).
 *   - Ping waits for PONG → sets PING.  (ping goes first in timed window)
 *   - Pong waits for PING → sets PONG.
 *   - After each sample, flag is PONG  (ping's last timed CAS set PING,
 *     then pong responded setting PONG, so flag ends at PONG).
 *   - The next sample starts correctly: flag == PONG, ping waits for PONG.
 *
 * There is NO barrier inside the sample loop. The only barriers are:
 *   1. One barrier at thread startup (both threads ready)
 *   2. One barrier per sample START so that pong is spinning before
 *      ping starts the timed window — this is a lightweight spin barrier
 *      using an atomic counter, NOT a futex, so it does not cause a
 *      context switch on HT pairs.
 *
 * The futex-based pthread_barrier inside the hot loop was the root cause
 * of inflated HT sibling latency (hundreds of ns of context-switch
 * overhead on every sample for two threads sharing a physical core).
 *
 * WATCHDOG
 * ────────
 * pthread_timedjoin_np aborts any pair that takes longer than
 * PAIR_TIMEOUT_SEC, preventing infinite hangs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <math.h>
#include <time.h>

#include "c2c.h"
#include "tsc.h"
#include "utils.h"
#include "stats.h"

#define PING 0
#define PONG 1
#define WARMUP_SAMPLES    5
#define PAIR_TIMEOUT_SEC 30

/* ------------------------------------------------------------------ */
/* Shared state                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    /* Startup barrier — futex-based, only used once per pair */
    pthread_barrier_t start_barrier;
    char _barrier_pad[CACHE_LINE_SIZE];

    /*
     * Lightweight spin barrier for per-sample synchronisation.
     * Both threads increment arrive_count; the last one resets it.
     * No futex, no syscall, no context switch — safe for HT pairs.
     *
     * _Alignas forces this onto its own cache line regardless of the
     * containing struct's alignment, so the spin barrier's atomic writes
     * cannot share a cache line with the contested flag below.
     */
    _Alignas(CACHE_LINE_SIZE) _Atomic uint32_t arrive_count;
    char _arrive_pad[CACHE_LINE_SIZE - sizeof(_Atomic uint32_t)];

    /*
     * The contested flag — alone on its own cache line.
     * _Alignas ensures it starts at a cache-line boundary.
     */
    _Alignas(CACHE_LINE_SIZE) _Atomic int flag;
    char _flag_pad[CACHE_LINE_SIZE - sizeof(_Atomic int)];

    /*
     * timed_out: written by main thread on watchdog timeout,
     * read by both worker threads in their CAS loops.
     * On its own cache line so the watchdog write does not invalidate
     * any hot line in the worker threads' caches.
     */
    _Alignas(CACHE_LINE_SIZE) _Atomic int timed_out;
    char _timeout_pad[CACHE_LINE_SIZE - sizeof(_Atomic int)];

    /* Cold fields — accessed once at startup */
    uint32_t num_iterations;
    uint32_t num_samples;
    double  *results;
} CACHE_ALIGNED cas_state_t;

typedef struct {
    cas_state_t *s;
    int          core_id;
} ping_arg_t;

/* ------------------------------------------------------------------ */
/* Spin barrier — no syscall, no context switch                        */
/*                                                                      */
/* Two threads arrive; both spin until count reaches 2, then count    */
/* resets to 0 for the next use. Safe to call repeatedly.             */
/* ------------------------------------------------------------------ */

static inline void spin_barrier_wait(_Atomic uint32_t *count)
{
    uint32_t arrived = atomic_fetch_add_explicit(count, 1,
                                                  memory_order_acq_rel) + 1;
    if (arrived == 2) {
        /* Last to arrive: reset for next use */
        atomic_store_explicit(count, 0, memory_order_release);
    } else {
        /* First to arrive: spin until second thread resets */
        while (atomic_load_explicit(count, memory_order_acquire) != 0)
            __asm__ volatile ("pause" ::: "memory");
    }
}

/* ------------------------------------------------------------------ */
/* Pong thread                                                          */
/* ------------------------------------------------------------------ */

static void *pong_thread(void *arg)
{
    cas_state_t *s    = arg;
    int pong_core     = *(int *)((char *)arg + sizeof(cas_state_t));
    pin_thread_to_core(pong_core);

    pthread_barrier_wait(&s->start_barrier);

    for (uint32_t sample = 0; sample < s->num_samples; sample++) {
        /* Sync: ensure pong is spinning before ping starts timing */
        spin_barrier_wait(&s->arrive_count);

        for (uint32_t i = 0; i < s->num_iterations; i++) {
            if (atomic_load_explicit(&s->timed_out, memory_order_relaxed))
                return NULL;
            int expected = PING;
            while (!atomic_compare_exchange_weak_explicit(
                        &s->flag, &expected, PONG,
                        memory_order_relaxed, memory_order_relaxed)) {
                expected = PING;
                if (atomic_load_explicit(&s->timed_out, memory_order_relaxed))
                    return NULL;
            }
        }
        /*
         * After num_iterations: flag == PONG (pong just set it).
         * Ping will observe PONG at the start of the next sample's
         * timed window and begin immediately. Protocol stays in sync.
         */
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Ping thread                                                          */
/* ------------------------------------------------------------------ */

static void *ping_thread(void *arg)
{
    ping_arg_t  *pa = arg;
    cas_state_t *s  = pa->s;
    pin_thread_to_core(pa->core_id);

    pthread_barrier_wait(&s->start_barrier);

    for (uint32_t sample = 0; sample < s->num_samples; sample++) {
        /*
         * Sync: ping arrives here, then spins until pong also arrives.
         * Because spin_barrier_wait uses atomic ops + PAUSE (no futex),
         * both threads remain on-core during the wait — critical for
         * HT pairs where a futex would cause a context switch and add
         * hundreds of ns to the first iteration of every sample.
         */
        spin_barrier_wait(&s->arrive_count);

        if (atomic_load_explicit(&s->timed_out, memory_order_relaxed))
            return NULL;

        /*
         * flag is PONG here (invariant: each sample ends with pong
         * having done the last CAS, setting flag = PONG).
         * Drain the store buffer then start timing.
         */
        tsc_drain_stores();
        uint64_t t0 = tsc_read_bare();

        for (uint32_t i = 0; i < s->num_iterations; i++) {
            if (atomic_load_explicit(&s->timed_out, memory_order_relaxed))
                return NULL;
            int expected = PONG;
            while (!atomic_compare_exchange_weak_explicit(
                        &s->flag, &expected, PING,
                        memory_order_relaxed, memory_order_relaxed)) {
                expected = PONG;
                if (atomic_load_explicit(&s->timed_out, memory_order_relaxed))
                    return NULL;
            }
        }
        /*
         * After num_iterations: flag == PING (ping's last CAS).
         * Pong will do one more CAS (PING→PONG) to restore invariant,
         * but that happens AFTER t1 is recorded — it is NOT timed.
         */
        uint64_t t1 = tsc_read_bare();

        double ns = tsc_ticks_to_ns(t1 - t0);
        s->results[sample] = ns / s->num_iterations / 2.0;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Run one core-pair with watchdog                                      */
/* ------------------------------------------------------------------ */

static int run_pair(int core_ping, int core_pong,
                    uint32_t num_iterations, uint32_t num_samples,
                    double *out_results)
{
    /*
     * Allocate state struct with cache-line alignment.
     * calloc returns only 16-byte aligned memory on most systems, which
     * is insufficient for our CACHE_ALIGNED struct. If misaligned, the
     * carefully-placed flag and arrive_count fields could end up on the
     * SAME cache line, causing the spin barrier's writes to invalidate
     * the flag line on every sample and inflating measured latency by
     * 5-10×. posix_memalign guarantees the requested alignment.
     */
    size_t alloc_size = sizeof(cas_state_t) + sizeof(int);
    /* Round up to multiple of CACHE_LINE_SIZE for posix_memalign */
    alloc_size = (alloc_size + CACHE_LINE_SIZE - 1) & ~(CACHE_LINE_SIZE - 1);

    cas_state_t *s = NULL;
    if (posix_memalign((void **)&s, CACHE_LINE_SIZE, alloc_size) != 0
            || !s) {
        perror("posix_memalign");
        exit(1);
    }
    memset(s, 0, alloc_size);

    *(int *)((char *)s + sizeof(cas_state_t)) = core_pong;

    pthread_barrier_init(&s->start_barrier, NULL, 2);
    atomic_init(&s->arrive_count, 0);
    atomic_init(&s->flag, PONG);   /* PONG: ping goes first in timed window */
    atomic_init(&s->timed_out, 0);
    s->num_iterations = num_iterations;
    s->num_samples    = num_samples;
    s->results        = out_results;

    pthread_t t_pong, t_ping;
    ping_arg_t pa = { .s = s, .core_id = core_ping };

    pthread_create(&t_pong, NULL, pong_thread, s);
    pthread_create(&t_ping, NULL, ping_thread, &pa);

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += PAIR_TIMEOUT_SEC;

    int timed_out = 0;
    if (pthread_timedjoin_np(t_ping, NULL, &deadline) != 0) {
        atomic_store(&s->timed_out, 1);
        timed_out = 1;
        fprintf(stderr,
            "\n  TIMEOUT: pair (%d,%d) did not complete in %d seconds.\n",
            core_ping, core_pong, PAIR_TIMEOUT_SEC);
    }
    pthread_join(t_pong, NULL);
    pthread_join(t_ping, NULL);

    pthread_barrier_destroy(&s->start_barrier);
    free(s);   /* posix_memalign-allocated memory is freed with free() */
    return timed_out ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* Public entry point                                                   */
/* ------------------------------------------------------------------ */

void run_bench_cas(const bench_config_t *cfg)
{
    int      n  = cfg->n_cores;
    uint32_t ni = cfg->num_iterations;
    uint32_t ns = cfg->num_samples;

    double *results = alloc_results(n, ns);
    double *tmp     = malloc((ns + WARMUP_SAMPLES) * sizeof(double));
    if (!tmp) { perror("malloc"); exit(1); }

    long total_pairs = (long)n * (n - 1) / 2;
    long pair_num    = 0;
    int  timeouts    = 0;

    for (int i = 0; i < n; i++) {
        for (int j = 0; j < i; j++) {
            pair_num++;
            fprintf(stderr, "\r  Pair %ld/%ld  cores (%d,%d)    ",
                    pair_num, total_pairs, cfg->cores[i], cfg->cores[j]);
            fflush(stderr);

            int rc = run_pair(cfg->cores[i], cfg->cores[j], ni,
                              ns + WARMUP_SAMPLES, tmp);
            if (rc == 0) {
                double *dst = result_at(results, n, ns, i, j, 0);
                memcpy(dst, tmp + WARMUP_SAMPLES, ns * sizeof(double));
            } else {
                timeouts++;
            }
        }
    }

    fprintf(stderr, "\r  %-60s\r", "");

    if (timeouts > 0)
        fprintf(stderr,
            "  WARN: %d pair(s) timed out and were skipped.\n", timeouts);

    print_results(cfg, results, /*is_symmetric=*/1);
    free(tmp);
    free(results);
}
