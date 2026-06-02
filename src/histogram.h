#pragma once
/*
 * histogram.h — fixed-width latency histogram for bench_direct.
 *
 * Buckets are 1 ns wide, covering [0, HIST_MAX_NS).
 * Samples >= HIST_MAX_NS are counted in an overflow bucket and excluded
 * from percentile calculations (they are OS preemptions, not cache events).
 *
 * All operations are single-threaded; no atomics needed here.
 */

#include <stdint.h>
#include <string.h>
#include <math.h>

#define HIST_MAX_NS   2000          /* 2 µs ceiling — anything above is noise  */
#define HIST_BUCKETS  HIST_MAX_NS

typedef struct {
    uint32_t counts[HIST_BUCKETS]; /* counts[i] = samples with latency in [i, i+1) ns */
    uint64_t overflow;             /* samples >= HIST_MAX_NS                           */
    uint64_t total;                /* total samples added (including overflow)         */
} histogram_t;

static inline void hist_init(histogram_t *h)
{
    memset(h, 0, sizeof(*h));
}

static inline void hist_add(histogram_t *h, double ns)
{
    h->total++;
    int bucket = (int)ns;
    if (bucket < 0) bucket = 0;
    if (bucket >= HIST_BUCKETS) {
        h->overflow++;
    } else {
        h->counts[bucket]++;
    }
}

/*
 * Return the value at the p-th percentile (0.0–1.0).
 * Returns -1.0 if the histogram is empty.
 */
static inline double hist_percentile(const histogram_t *h, double p)
{
    uint64_t valid = h->total - h->overflow;
    if (valid == 0) return -1.0;

    uint64_t target = (uint64_t)(p * (double)valid);
    if (target >= valid) target = valid - 1;

    uint64_t cumulative = 0;
    for (int i = 0; i < HIST_BUCKETS; i++) {
        cumulative += h->counts[i];
        if (cumulative > target)
            return (double)i + 0.5;   /* midpoint of bucket */
    }
    return (double)(HIST_BUCKETS - 1);
}

static inline double hist_min(const histogram_t *h)
{
    for (int i = 0; i < HIST_BUCKETS; i++)
        if (h->counts[i]) return (double)i + 0.5;
    return -1.0;
}

static inline double hist_max(const histogram_t *h)
{
    for (int i = HIST_BUCKETS - 1; i >= 0; i--)
        if (h->counts[i]) return (double)i + 0.5;
    return -1.0;
}

static inline double hist_mean(const histogram_t *h)
{
    uint64_t valid = h->total - h->overflow;
    if (valid == 0) return -1.0;
    double sum = 0.0;
    for (int i = 0; i < HIST_BUCKETS; i++)
        sum += (double)i * h->counts[i];
    return sum / (double)valid;
}
