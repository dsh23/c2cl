#pragma once
/*
 * stats.h — descriptive statistics and results display
 *
 * The results matrix is a flat 3D array indexed [i][j][s]:
 *   i, j : core indices (0..n_cores-1)
 *   s    : sample index (0..num_samples-1)
 *
 * Values of NAN indicate that a (i,j) pair was not measured.
 */

#include <stdint.h>
#include "c2c.h"
#include "utils.h"

/* Allocate a zeroed results matrix of shape [n][n][num_samples].
 * Caller must free() it. All entries initialised to NAN. */
double *alloc_results(int n_cores, uint32_t num_samples);

/* Index helper: results[i * n_cores * num_samples + j * num_samples + s] */
static inline double *result_at(double *r, int n_cores, uint32_t num_samples,
                                 int i, int j, uint32_t s)
{
    return &r[(i * (int)n_cores + j) * (int)num_samples + s];
}

/* Compute the mean of an array of doubles, ignoring NAN.
 * Returns NAN if no valid samples. */
double mean_of(const double *v, int n);

/* Compute the sample standard deviation of v, ignoring NAN.
 * ddof = degrees-of-freedom correction (1 for sample stddev). */
double stddev_of(const double *v, int n, double ddof);

/*
 * Print the results table and summary stats to stderr.
 * If cfg->csv is set, print mean matrix as CSV to stdout.
 *
 * is_symmetric: if 1, only the lower triangle was measured (i > j).
 *               if 0, all off-diagonal pairs were measured.
 */
void print_results(const bench_config_t *cfg,
                   const double *results,
                   int is_symmetric);
