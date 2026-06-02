/*
 * bench_msg.c — Benchmark 3: one-way message passing via RDTSC timestamps.
 *
 * Asymmetric: measures sender→receiver latency only.
 * Requires invariant TSC (checked by assert_rdtsc_usable()).
 *
 * Protocol per sample:
 *   1. Barrier: both threads start the sample together.
 *   2. Sender iterates over slots[0..num_iterations):
 *        a. delay_cycles(10000) — lets receiver reach its spin.
 *        b. Stores current TSC (relative to start_tsc) into slot, relaxed.
 *   3. Receiver spins on each slot until non-zero, reads its own TSC,
 *      accumulates (recv_tsc - send_tsc) per slot.
 *   4. Barrier: both signal sample complete.
 *   5. Receiver subtracts TSC-read overhead, converts to ns, stores sample.
 *   6. Sender resets all slots to 0 for the next sample.
 *
 * Fixes applied vs original port:
 *   - delay_cycles() uses plain volatile load (no PAUSE) so the delay is
 *     proportional to wall time, not pipeline stalls.
 *   - tsc_read_start() / tsc_read_end() used for overhead measurement.
 *   - WARMUP_SAMPLES = 5.
 *   - Barrier padded to its own cache line.
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
/* One message slot: 64-bit atomic on its own cache line              */
/* ------------------------------------------------------------------ */

typedef struct {
    _Atomic uint64_t value;
    char _pad[CACHE_LINE_SIZE - sizeof(_Atomic uint64_t)];
} CACHE_ALIGNED msg_slot_t;

/* ------------------------------------------------------------------ */
/* Shared state                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    pthread_barrier_t  barrier;
    char _barrier_pad[CACHE_LINE_SIZE];

    msg_slot_t        *slots;            /* [num_iterations], cache-aligned */
    uint64_t           start_tsc;        /* shared time origin */
    uint64_t           overhead_ticks;   /* cost of num_iterations tsc_read()s */

    uint32_t           num_iterations;
    uint32_t           num_samples;
    double            *results;
} msg_state_t;

/* ------------------------------------------------------------------ */
/* Thread args                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    msg_state_t *s;
    int          core_id;
    int          is_receiver;
} msg_arg_t;

/* ------------------------------------------------------------------ */
/* Spin until slot is non-zero, return value                           */
/* ------------------------------------------------------------------ */

static inline uint64_t wait_nonzero(const _Atomic uint64_t *slot)
{
    uint64_t v;
    do {
        v = atomic_load_explicit(slot, memory_order_relaxed);
    } while (v == 0);
    return v;
}

/* ------------------------------------------------------------------ */
/* Worker                                                               */
/* ------------------------------------------------------------------ */

static void *msg_thread(void *arg)
{
    msg_arg_t   *a = arg;
    msg_state_t *s = a->s;
    pin_thread_to_core(a->core_id);

    pthread_barrier_wait(&s->barrier);  /* initial ready barrier */

    if (a->is_receiver) {
        for (uint32_t sample = 0; sample < s->num_samples; sample++) {
            uint64_t total_ticks = 0;

            pthread_barrier_wait(&s->barrier);  /* sample start */

            for (uint32_t i = 0; i < s->num_iterations; i++) {
                uint64_t send_time = wait_nonzero(&s->slots[i].value);
                uint64_t recv_time = tsc_read() - s->start_tsc;
                uint64_t delta = (recv_time >= send_time)
                                    ? recv_time - send_time : 0;
                total_ticks += delta;
            }

            pthread_barrier_wait(&s->barrier);  /* sample end */

            uint64_t net = (total_ticks > s->overhead_ticks)
                               ? total_ticks - s->overhead_ticks : 0;
            s->results[sample] = tsc_ticks_to_ns(net) / s->num_iterations;
        }

    } else {
        /* Sender */
        for (uint32_t sample = 0; sample < s->num_samples; sample++) {
            pthread_barrier_wait(&s->barrier);  /* sample start */

            for (uint32_t i = 0; i < s->num_iterations; i++) {
                /*
                 * Delay to let the receiver reach wait_nonzero() for this
                 * slot before we write. Uses plain volatile-load loop (no
                 * PAUSE) so the delay scales with wall time, not pipeline
                 * stall cycles.
                 */
                delay_cycles(10000);

                uint64_t t = tsc_read() - s->start_tsc;
                if (t == 0) t = 1;   /* slot value must be non-zero */
                atomic_store_explicit(&s->slots[i].value, t,
                                      memory_order_relaxed);
            }

            pthread_barrier_wait(&s->barrier);  /* sample end */

            /* Reset slots for next sample */
            for (uint32_t i = 0; i < s->num_iterations; i++) {
                atomic_store_explicit(&s->slots[i].value, 0,
                                      memory_order_relaxed);
            }
        }
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/* Measure overhead of num_iterations tsc_read() calls                 */
/* ------------------------------------------------------------------ */

static uint64_t measure_tsc_overhead(uint32_t n)
{
    uint64_t t0 = tsc_read_start();
    for (uint32_t i = 0; i < n - 1; i++) {
        uint64_t t = tsc_read();
        black_box(&t);
    }
    uint64_t t1 = tsc_read_end();
    return t1 - t0;
}

/* ------------------------------------------------------------------ */
/* Run one core-pair                                                    */
/* ------------------------------------------------------------------ */

static void run_pair(int core_recv, int core_send,
                     uint32_t num_iterations, uint32_t num_samples,
                     double *out_results)
{
    msg_state_t *s = calloc(1, sizeof(msg_state_t));
    if (!s) { perror("calloc"); exit(1); }

    s->slots = aligned_alloc(CACHE_LINE_SIZE,
                             num_iterations * sizeof(msg_slot_t));
    if (!s->slots) { perror("aligned_alloc"); exit(1); }
    memset(s->slots, 0, num_iterations * sizeof(msg_slot_t));

    pthread_barrier_init(&s->barrier, NULL, 2);

    s->overhead_ticks = measure_tsc_overhead(num_iterations);
    s->start_tsc      = tsc_read();
    s->num_iterations = num_iterations;
    s->num_samples    = num_samples;
    s->results        = out_results;

    msg_arg_t recv_arg = { .s = s, .core_id = core_recv, .is_receiver = 1 };
    msg_arg_t send_arg = { .s = s, .core_id = core_send, .is_receiver = 0 };

    pthread_t t_recv, t_send;
    pthread_create(&t_recv, NULL, msg_thread, &recv_arg);
    pthread_create(&t_send, NULL, msg_thread, &send_arg);

    pthread_join(t_recv, NULL);
    pthread_join(t_send, NULL);

    pthread_barrier_destroy(&s->barrier);
    free(s->slots);
    free(s);
}

/* ------------------------------------------------------------------ */
/* Public entry point                                                   */
/* ------------------------------------------------------------------ */

void run_bench_msg(const bench_config_t *cfg)
{
    int      n  = cfg->n_cores;
    uint32_t ni = cfg->num_iterations;
    uint32_t ns = cfg->num_samples;

    double *results = alloc_results(n, ns);
    double *tmp     = malloc((ns + WARMUP_SAMPLES) * sizeof(double));
    if (!tmp) { perror("malloc"); exit(1); }

    /* Asymmetric: measure all off-diagonal pairs in both directions */
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            if (i == j) continue;
            run_pair(cfg->cores[i], cfg->cores[j], ni,
                     ns + WARMUP_SAMPLES, tmp);

            double *dst = result_at(results, n, ns, i, j, 0);
            memcpy(dst, tmp + WARMUP_SAMPLES, ns * sizeof(double));
        }
    }

    free(tmp);
    print_results(cfg, results, /*is_symmetric=*/0);
    free(results);
}
