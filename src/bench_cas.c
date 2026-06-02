/*
 * bench_cas.c — Benchmark 1: CAS latency on a single shared cache line.
 *
 * BUG FIX: the original pong thread used a flat loop of num_iters*num_samples
 * iterations while ping used nested loops. If they ever got one step out of
 * phase (e.g. due to a spurious CAS failure leaving flag in the wrong state
 * between samples) both threads would spin forever waiting for the other to
 * set the flag they expect. Fixed by:
 *   - Making pong mirror ping's nested structure (outer sample, inner iter)
 *   - Explicitly resetting the flag to PING at the start of each sample
 *   - Adding a per-pair watchdog timeout (PAIR_TIMEOUT_SEC)
 *   - Adding per-pair progress output to stderr
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
#define WARMUP_SAMPLES   5
#define PAIR_TIMEOUT_SEC 10   /* abort pair if it takes longer than this */

/* ------------------------------------------------------------------ */
/* Shared state                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    pthread_barrier_t barrier;
    char _barrier_pad[CACHE_LINE_SIZE];

    _Atomic int flag;
    _Atomic int timed_out;   /* set by watchdog; threads check and bail */

    uint32_t num_iterations;
    uint32_t num_samples;
    double  *results;
} cas_state_t;

typedef struct {
    cas_state_t *s;
    int          core_id;
} ping_arg_t;

/* ------------------------------------------------------------------ */
/* Pong thread — mirrors ping's nested loop structure exactly          */
/* ------------------------------------------------------------------ */

static void *pong_thread(void *arg)
{
    cas_state_t *s      = arg;
    int pong_core       = *(int *)((char *)arg + sizeof(cas_state_t));
    pin_thread_to_core(pong_core);

    pthread_barrier_wait(&s->barrier);

    for (uint32_t sample = 0; sample < s->num_samples; sample++) {
        /* Sync point: wait for ping to reset the flag before each sample */
        pthread_barrier_wait(&s->barrier);

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

    pthread_barrier_wait(&s->barrier);

    for (uint32_t sample = 0; sample < s->num_samples; sample++) {
        /*
         * Reset the flag to PING before each sample and sync with pong.
         * This is the critical fix: without this reset, if the previous
         * sample ended with flag==PING (ping's last successful CAS set it
         * to PING), the new sample starts correctly. But if anything goes
         * wrong between samples, the explicit reset ensures both threads
         * start each sample from a known state.
         */
        atomic_store_explicit(&s->flag, PING, memory_order_seq_cst);
        pthread_barrier_wait(&s->barrier);

        if (atomic_load_explicit(&s->timed_out, memory_order_relaxed))
            return NULL;

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
    cas_state_t *s = calloc(1, sizeof(cas_state_t) + sizeof(int));
    if (!s) { perror("calloc"); exit(1); }

    *(int *)((char *)s + sizeof(cas_state_t)) = core_pong;

    /*
     * The inner per-sample barrier needs 2 threads.
     * Plus the outer start barrier.
     */
    pthread_barrier_init(&s->barrier, NULL, 2);
    atomic_init(&s->flag, PING);
    atomic_init(&s->timed_out, 0);
    s->num_iterations = num_iterations;
    s->num_samples    = num_samples;
    s->results        = out_results;

    pthread_t t_pong, t_ping;
    ping_arg_t pa = { .s = s, .core_id = core_ping };

    pthread_create(&t_pong, NULL, pong_thread, s);
    pthread_create(&t_ping, NULL, ping_thread, &pa);

    /* Watchdog: join with timeout */
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += PAIR_TIMEOUT_SEC;

    int timed_out = 0;
    if (pthread_timedjoin_np(t_ping, NULL, &deadline) != 0) {
        atomic_store(&s->timed_out, 1);
        timed_out = 1;
        fprintf(stderr,
            "\n  TIMEOUT: pair (%d,%d) did not complete in %d seconds. "
            "Skipping.\n", core_ping, core_pong, PAIR_TIMEOUT_SEC);
    }
    pthread_join(t_pong, NULL);
    pthread_join(t_ping, NULL);  /* second join is a no-op if already done */

    pthread_barrier_destroy(&s->barrier);
    free(s);
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

            /* Progress: print every pair so a hang is immediately visible */
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

    fprintf(stderr, "\r  %-60s\r", "");  /* clear progress line */

    if (timeouts > 0)
        fprintf(stderr,
            "  WARN: %d pair(s) timed out and were skipped. "
            "Results matrix will have gaps.\n"
            "  This indicates a deadlock bug — please file an issue "
            "with the pair IDs printed above.\n", timeouts);

    print_results(cfg, results, /*is_symmetric=*/1);
    free(tmp);
    free(results);
}
