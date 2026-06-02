#pragma once
/*
 * c2c.h — shared types and bench dispatch interface
 */

#include <stdint.h>

#define MAX_CORES 256
#define CACHE_LINE_SIZE 64

/*
 * Cache-line aligned/padded wrapper macro.
 * Ensures that a field occupies its own cache line.
 */
#define CACHE_ALIGNED __attribute__((aligned(CACHE_LINE_SIZE)))

/* Forward declaration — topology.h provides the full definition */
typedef struct topology_t topology_t;

/* ------------------------------------------------------------------ */
/* Configuration passed to every benchmark runner                       */
/* ------------------------------------------------------------------ */
typedef struct {
    const int    *cores;          /* array of core IDs                  */
    int           n_cores;
    uint32_t      num_iterations; /* round-trips (or msgs) per sample   */
    uint32_t      num_samples;
    int           csv;            /* save CSV to output.csv             */
    topology_t   *topo;           /* NULL = topology-unaware mode       */
} bench_config_t;

/* ------------------------------------------------------------------ */
/* Benchmark entry points                                               */
/* ------------------------------------------------------------------ */
void run_bench_cas   (const bench_config_t *cfg);
void run_bench_rw    (const bench_config_t *cfg);
void run_bench_msg   (const bench_config_t *cfg);
void run_bench_direct(const bench_config_t *cfg);
