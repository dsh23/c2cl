
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

#include "stats.h"
#include "c2c.h"

/* ------------------------------------------------------------------ */
/* Allocation                                                           */
/* ------------------------------------------------------------------ */

double *alloc_results(int n_cores, uint32_t num_samples)
{
    size_t n = (size_t)n_cores * n_cores * num_samples;
    double *r = malloc(n * sizeof(double));
    if (!r) { perror("malloc results"); exit(1); }
    for (size_t i = 0; i < n; i++) r[i] = (double)NAN;
    return r;
}

/* ------------------------------------------------------------------ */
/* Basic statistics                                                     */
/* ------------------------------------------------------------------ */

double mean_of(const double *v, int n)
{
    double sum = 0.0;
    int count = 0;
    for (int i = 0; i < n; i++) {
        if (!isnan(v[i])) { sum += v[i]; count++; }
    }
    return count > 0 ? sum / count : (double)NAN;
}

double stddev_of(const double *v, int n, double ddof)
{
    double m = mean_of(v, n);
    if (isnan(m)) return (double)NAN;

    double sumsq = 0.0;
    int count = 0;
    for (int i = 0; i < n; i++) {
        if (!isnan(v[i])) {
            double d = v[i] - m;
            sumsq += d * d;
            count++;
        }
    }
    if (count <= ddof) return 0.0;
    return sqrt(sumsq / (count - ddof));
}

/* ------------------------------------------------------------------ */
/* Results display                                                      */
/* ------------------------------------------------------------------ */

/*
 * ANSI colour helpers: bold white for mean, dim for stddev.
 * We check isatty so that piped output doesn't get escape codes.
 */
#include <unistd.h>
static int use_color = -1;

static void ensure_color_init(void)
{
    if (use_color < 0) use_color = isatty(fileno(stderr));
}

static void bold(void)   { if (use_color) fprintf(stderr, "\033[1m");  }
static void dim(void)    { if (use_color) fprintf(stderr, "\033[2m");  }
static void reset(void)  { if (use_color) fprintf(stderr, "\033[0m");  }

void print_results(const bench_config_t *cfg, const double *results,
                   int is_symmetric)
{
    int n = cfg->n_cores;
    uint32_t ns = cfg->num_samples;
    const int *cores = cfg->cores;

    ensure_color_init();

    /* ---- Column header ------------------------------------------- */
    fprintf(stderr, "    %3s", "");
    for (int j = 0; j < n; j++)
        fprintf(stderr, " %4d   ", cores[j]);
    fprintf(stderr, "\n");

    /* ---- Per-pair means ------------------------------------------- */
    /* means[i*n+j] = mean over samples for pair (i,j), NAN if not run */
    double *means   = malloc(n * n * sizeof(double));
    double *stddevs = malloc(n * n * sizeof(double));
    if (!means || !stddevs) { perror("malloc"); exit(1); }

    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            const double *samples = result_at((double*)results, n, ns, i, j, 0);
            means  [i*n+j] = mean_of  (samples, (int)ns);
            /* CLT-adjusted stddev: σ/√N estimates uncertainty of the mean */
            double s = stddev_of(samples, (int)ns, 1.0);
            stddevs[i*n+j] = (s < 99.0 ? s : 99.0) / sqrt((double)ns);
        }
    }

    /* ---- Print table --------------------------------------------- */
    for (int i = 0; i < n; i++) {
        fprintf(stderr, "    %3d", cores[i]);
        for (int j = 0; j < n; j++) {
            int skip = 0;
            if (is_symmetric) {
                skip = (i <= j);
            } else {
                skip = (i == j);
            }

            if (skip) {
                fprintf(stderr, " %7s", "");
                continue;
            }

            double m = means  [i*n+j];
            double s = stddevs[i*n+j];

            if (isnan(m)) {
                fprintf(stderr, " %7s", "");
            } else {
                bold();  fprintf(stderr, " %4.0f", m);
                reset();
                dim();   fprintf(stderr, "±%-2.0f ", s);
                reset();
            }
        }
        fprintf(stderr, "\n");
    }

    fprintf(stderr, "\n");

    /* ---- Min / max latency --------------------------------------- */
    double min_val = DBL_MAX, max_val = -DBL_MAX;
    int min_i = -1, min_j = -1, max_i = -1, max_j = -1;

    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            double m = means[i*n+j];
            if (isnan(m)) continue;
            if (m < min_val) { min_val = m; min_i = i; min_j = j; }
            if (m > max_val) { max_val = m; max_i = i; max_j = j; }
        }
    }

    if (min_i >= 0) {
        bold(); fprintf(stderr, "    Min  latency: %5.1f", min_val); reset();
        dim();  fprintf(stderr, "±%-4.1f", stddevs[min_i*n+min_j]); reset();
        fprintf(stderr, "ns  cores: (%d,%d)\n", cores[min_i], cores[min_j]);
    }

    /* ---- Min non-HT latency --------------------------------------- */
    {
        double min_non_ht = DBL_MAX;
        int    mnh_i = -1, mnh_j = -1;

        for (int i = 0; i < n; i++) {
            for (int j = 0; j < n; j++) {
                double m = means[i*n+j];
                if (isnan(m)) continue;
                if (are_ht_siblings(cores[i], cores[j])) continue;
                if (m < min_non_ht) {
                    min_non_ht = m; mnh_i = i; mnh_j = j;
                }
            }
        }

        if (mnh_i >= 0) {
            bold(); fprintf(stderr, "    Min  non-HT: %5.1f", min_non_ht); reset();
            dim();  fprintf(stderr, "±%-4.1f", stddevs[mnh_i*n+mnh_j]); reset();
            fprintf(stderr, "ns  cores: (%d,%d)\n", cores[mnh_i], cores[mnh_j]);
        }
    }
    if (max_i >= 0) {
        bold(); fprintf(stderr, "    Max  latency: %5.1f", max_val); reset();
        dim();  fprintf(stderr, "±%-4.1f", stddevs[max_i*n+max_j]); reset();
        fprintf(stderr, "ns  cores: (%d,%d)\n", cores[max_i], cores[max_j]);
    }

    /* ---- Overall mean -------------------------------------------- */
    {
        int count = 0;
        double sum = 0.0;
        for (int i = 0; i < n*n; i++) {
            if (!isnan(means[i])) { sum += means[i]; count++; }
        }
        if (count > 0) {
            bold(); fprintf(stderr, "    Mean latency: %5.1f", sum/count); reset();
            fprintf(stderr, "ns\n");
        }
    }

    /* ---- CSV output ---------------------------------------------- */
    if (cfg->csv) {
        FILE *f = fopen("output.csv", "w");
        if (!f) {
            perror("fopen output.csv");
        } else {
            for (int i = 0; i < n; i++) {
                for (int j = 0; j < n; j++) {
                    if (j) fprintf(f, ",");
                    double m = means[i*n+j];
                    if (!isnan(m)) fprintf(f, "%.6f", m);
                }
                fprintf(f, "\n");
            }
            fclose(f);
            fprintf(stderr, "    CSV saved to output.csv\n");
        }
    }

    free(means);
    free(stddevs);
}
