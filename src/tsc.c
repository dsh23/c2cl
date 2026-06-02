#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include "tsc.h"

double g_tsc_ghz = 0.0;

static int cmp_double(const void *a, const void *b)
{
    double da = *(const double *)a;
    double db = *(const double *)b;
    return (da > db) - (da < db);
}

/*
 * Single calibration sample using CLOCK_MONOTONIC_RAW busy-wait edges.
 *
 * Strategy:
 *   1. Spin on CLOCK_MONOTONIC_RAW until the nanosecond field changes —
 *      this aligns the TSC read to a clean clock-tick boundary, removing
 *      the uncertainty of where in a tick period we sampled.
 *   2. Busy-spin for ~10 ms (no sleep, so no scheduler jitter).
 *   3. Capture the end boundary the same way.
 *
 * CLOCK_MONOTONIC_RAW is used instead of CLOCK_MONOTONIC because it is
 * not subject to NTP frequency adjustments, giving a stable reference.
 */
static double calibrate_once(void)
{
    struct timespec t0, t1, tmp;
    uint64_t tsc0, tsc1;

    /* Align to a CLOCK_MONOTONIC_RAW tick edge */
    clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
    do {
        clock_gettime(CLOCK_MONOTONIC_RAW, &tmp);
    } while (tmp.tv_nsec == t0.tv_nsec && tmp.tv_sec == t0.tv_sec);
    tsc0 = tsc_read();
    t0 = tmp;

    /* Busy-spin for ~10 ms */
    const int64_t target_ns = 10000000LL;
    do {
        clock_gettime(CLOCK_MONOTONIC_RAW, &t1);
    } while (((t1.tv_sec  - t0.tv_sec ) * 1000000000LL +
              (t1.tv_nsec - t0.tv_nsec)) < target_ns);
    tsc1 = tsc_read();

    int64_t ns_elapsed = (t1.tv_sec  - t0.tv_sec ) * 1000000000LL
                       + (t1.tv_nsec - t0.tv_nsec);

    if (ns_elapsed <= 0) return 0.0;
    return (double)(tsc1 - tsc0) / (double)ns_elapsed;
}

void tsc_init(void)
{
#define NUM_CALIB_SAMPLES 9
    double samples[NUM_CALIB_SAMPLES];

    for (int i = 0; i < NUM_CALIB_SAMPLES; i++) {
        samples[i] = calibrate_once();
        if (samples[i] <= 0.0) {
            fprintf(stderr, "TSC calibration failed (sample %d)\n", i);
            exit(1);
        }
    }

    qsort(samples, NUM_CALIB_SAMPLES, sizeof(double), cmp_double);
    g_tsc_ghz = samples[NUM_CALIB_SAMPLES / 2];

    if (g_tsc_ghz < 0.1 || g_tsc_ghz > 10.0) {
        fprintf(stderr,
            "TSC calibration produced implausible result: %.4f GHz\n",
            g_tsc_ghz);
        exit(1);
    }
#undef NUM_CALIB_SAMPLES
}
