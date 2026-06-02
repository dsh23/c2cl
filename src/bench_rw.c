/*
 * bench_rw.c — Benchmark 2: single-writer single-reader on two separate
 * cache lines.
 *
 * Ping owns one cache line, pong owns another. Each thread writes to its
 * own line and reads from the other's. This removes the CAS contest and
 * measures clean point-to-point ownership transfer latency.
 *
 * Fixes applied:
 *   - tsc_read_start() / tsc_read_end() for accurate interval measurement.
 *   - WARMUP_SAMPLES = 5.
 *   - Barrier explicitly padded onto its own cache line (separate from
 *     the flag lines being measured).
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

#define WARMUP_SAMPLES 5

/* ------------------------------------------------------------------ */
/* Cache-line padded atomic flag                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    _Atomic int value;
    char _pad[CACHE_LINE_SIZE - sizeof(_Atomic int)];
} CACHE_ALIGNED padded_flag_t;

/* ------------------------------------------------------------------ */
/* Shared state                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    /* Barrier on its own cache line */
    pthread_barrier_t barrier;
    char _barrier_pad[CACHE_LINE_SIZE];

    padded_flag_t owned_by_ping;  /* ping writes; pong reads */
    padded_flag_t owned_by_pong;  /* pong writes; ping reads */

    uint32_t num_iterations;
    uint32_t num_samples;
    double  *results;
} rw_state_t;

/* ------------------------------------------------------------------ */
/* Thread argument                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    rw_state_t *s;
    int         core_id;
    int         is_ping;
} rw_arg_t;

/* ------------------------------------------------------------------ */
/* Worker                                                               */
/* ------------------------------------------------------------------ */

static void *rw_thread(void *arg)
{
    rw_arg_t   *a = arg;
    rw_state_t *s = a->s;
    pin_thread_to_core(a->core_id);

    pthread_barrier_wait(&s->barrier);

    if (a->is_ping) {
        /*
         * Ping waits for owned_by_pong == v, then stores v to owned_by_ping.
         * Starts at v=1: pong's initial value is 0, so ping waits for pong's
         * first reply before beginning the timed loop.
         */
        int v = 1;
        for (uint32_t sample = 0; sample < s->num_samples; sample++) {
            uint64_t t0 = tsc_read_start();

            for (uint32_t i = 0; i < s->num_iterations; i++) {
                while (atomic_load_explicit(&s->owned_by_pong.value,
                                            memory_order_acquire) != v) {}
                atomic_store_explicit(&s->owned_by_ping.value, v,
                                      memory_order_release);
                v ^= 1;
            }

            uint64_t t1 = tsc_read_end();
            s->results[sample] = tsc_ticks_to_ns(t1 - t0)
                                  / s->num_iterations / 2.0;
        }
    } else {
        /* Pong: wait for ping's store, reply, repeat */
        int v = 0;
        uint64_t total = (uint64_t)s->num_iterations * s->num_samples;
        for (uint64_t k = 0; k < total; k++) {
            while (atomic_load_explicit(&s->owned_by_ping.value,
                                        memory_order_acquire) != v) {}
            atomic_store_explicit(&s->owned_by_pong.value, !v,
                                  memory_order_release);
            v ^= 1;
        }
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
    rw_state_t *s = calloc(1, sizeof(rw_state_t));
    if (!s) { perror("calloc"); exit(1); }

    pthread_barrier_init(&s->barrier, NULL, 2);
    atomic_init(&s->owned_by_ping.value, 0);
    atomic_init(&s->owned_by_pong.value, 0);
    s->num_iterations = num_iterations;
    s->num_samples    = num_samples;
    s->results        = out_results;

    rw_arg_t ping_arg = { .s = s, .core_id = core_ping, .is_ping = 1 };
    rw_arg_t pong_arg = { .s = s, .core_id = core_pong, .is_ping = 0 };

    pthread_t t_ping, t_pong;
    pthread_create(&t_ping, NULL, rw_thread, &ping_arg);
    pthread_create(&t_pong, NULL, rw_thread, &pong_arg);

    pthread_join(t_ping, NULL);
    pthread_join(t_pong, NULL);

    pthread_barrier_destroy(&s->barrier);
    free(s);
}

/* ------------------------------------------------------------------ */
/* Public entry point                                                   */
/* ------------------------------------------------------------------ */

void run_bench_rw(const bench_config_t *cfg)
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
