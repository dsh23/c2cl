/*
 * bench_cas.c — Benchmark 1: CAS latency on a single shared cache line.
 *
 * Two threads ping-pong a boolean flag via CAS. Both operate on the same
 * cache line — the cache line transfer is the thing being measured.
 * One-way latency = total_time / (2 * num_round_trips).
 *
 * Fixes applied vs original port:
 *   - tsc_read_start() at interval start, tsc_read_end() at end.
 *   - pthread_barrier_t padded onto its own cache line, away from the
 *     flag being measured, so barrier futex traffic can't interfere.
 *   - WARMUP_SAMPLES = 5 (was 1) to allow the cache hierarchy to reach
 *     steady state before recording results.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <math.h>

#include "c2c.h"
#include "tsc.h"
#include "utils.h"
#include "stats.h"

#define PING 0
#define PONG 1
#define WARMUP_SAMPLES 5

/* ------------------------------------------------------------------ */
/* Shared state                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    /*
     * Barrier on its own cache line. Without this padding the futex
     * word inside pthread_barrier_t could share a line with `flag`,
     * causing spurious cache-line invalidations at measurement start.
     */
    pthread_barrier_t barrier;
    char _barrier_pad[CACHE_LINE_SIZE];

    /*
     * The single contested cache line — intentionally NOT padded so
     * both threads fight over the same line. This is the measurement target.
     */
    _Atomic int flag;

    uint32_t num_iterations;
    uint32_t num_samples;
    double  *results;
} cas_state_t;

/* ------------------------------------------------------------------ */
/* Thread argument for the ping thread (pong takes the state directly) */
/* ------------------------------------------------------------------ */

typedef struct {
    cas_state_t *s;
    int          core_id;
} ping_arg_t;

/* pong core id is appended after the cas_state_t allocation */

/* ------------------------------------------------------------------ */
/* Pong thread                                                          */
/* ------------------------------------------------------------------ */

static void *pong_thread(void *arg)
{
    cas_state_t *s = arg;
    int pong_core  = *(int *)((char *)arg + sizeof(cas_state_t));
    pin_thread_to_core(pong_core);

    pthread_barrier_wait(&s->barrier);

    uint64_t total = (uint64_t)s->num_iterations * s->num_samples;
    for (uint64_t k = 0; k < total; k++) {
        int expected = PING;
        while (!atomic_compare_exchange_weak_explicit(
                    &s->flag, &expected, PONG,
                    memory_order_relaxed, memory_order_relaxed)) {
            expected = PING;
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
         * Drain the store buffer before sampling the TSC. This ensures
         * no dirty cache lines from the previous sample's final CAS write
         * are still in-flight when t0 is captured. Cost: ~30 cycles once
         * per sample, amortised over num_iterations round-trips.
         */
        tsc_drain_stores();
        uint64_t t0 = tsc_read_bare();

        for (uint32_t i = 0; i < s->num_iterations; i++) {
            int expected = PONG;
            while (!atomic_compare_exchange_weak_explicit(
                        &s->flag, &expected, PING,
                        memory_order_relaxed, memory_order_relaxed)) {
                expected = PONG;
            }
        }

        /*
         * Plain RDTSC at the end — no serialisation. We deliberately
         * avoid RDTSCP here because its pipeline flush would perturb the
         * store-buffer state left by the final CAS, adding artificial
         * latency to pong's first CAS of the next sample.
         */
        uint64_t t1 = tsc_read_bare();
        double ns = tsc_ticks_to_ns(t1 - t0);
        s->results[sample] = ns / s->num_iterations / 2.0;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Run one core-pair                                                    */
/* ------------------------------------------------------------------ */

static void run_pair(int core_ping, int core_pong,
                     uint32_t num_iterations, uint32_t num_samples,
                     double *out_results)
{
    cas_state_t *s = calloc(1, sizeof(cas_state_t) + sizeof(int));
    if (!s) { perror("calloc"); exit(1); }

    *(int *)((char *)s + sizeof(cas_state_t)) = core_pong;

    pthread_barrier_init(&s->barrier, NULL, 2);
    atomic_init(&s->flag, PING);
    s->num_iterations = num_iterations;
    s->num_samples    = num_samples;
    s->results        = out_results;

    pthread_t t_pong, t_ping;
    ping_arg_t pa = { .s = s, .core_id = core_ping };

    pthread_create(&t_pong, NULL, pong_thread, s);
    pthread_create(&t_ping, NULL, ping_thread, &pa);

    pthread_join(t_pong, NULL);
    pthread_join(t_ping, NULL);

    pthread_barrier_destroy(&s->barrier);
    free(s);
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

    for (int i = 0; i < n; i++) {
        for (int j = 0; j < i; j++) {
            run_pair(cfg->cores[i], cfg->cores[j], ni,
                     ns + WARMUP_SAMPLES, tmp);

            double *dst = result_at(results, n, ns, i, j, 0);
            memcpy(dst, tmp + WARMUP_SAMPLES, ns * sizeof(double));
        }
    }

    free(tmp);
    print_results(cfg, results, /*is_symmetric=*/1);
    free(results);
}
