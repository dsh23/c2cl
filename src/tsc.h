#pragma once
/*
 * tsc.h — RDTSC-based high-resolution timing for Linux x86-64
 *
 * Calibration: busy-waits against CLOCK_MONOTONIC_RAW to eliminate
 * nanosleep jitter and NTP slew from the frequency estimate.
 *
 * Measurement:
 *   tsc_read_start() — LFENCE + RDTSC   (prevents prior loads reordering in)
 *   tsc_read_end()   — RDTSCP + LFENCE  (self-serialising read; LFENCE after
 *                                        prevents subsequent loads reordering out)
 *
 * Using asymmetric serialisation (RDTSC at start, RDTSCP at end) matches the
 * strategy used by the Rust `quanta` crate and minimises serialisation
 * overhead while maintaining correct ordering.
 *
 * For single-point reads (benchmark 3 receiver, overhead measurement) use
 * tsc_read_start() — the LFENCE cost is the same in both directions there.
 */

#include <stdint.h>

/* Global TSC frequency in GHz (ticks per nanosecond). Set by tsc_init(). */
extern double g_tsc_ghz;

/* Calibrate TSC against CLOCK_MONOTONIC_RAW using busy-wait edges.
 * Must be called once before any tsc_ticks_to_ns() conversion. */
void tsc_init(void);

/*
 * tsc_read_start — use at the START of a measured interval.
 * LFENCE ensures all prior loads/stores are retired before RDTSC executes,
 * preventing the interval start from being measured too early.
 */
static inline uint64_t tsc_read_start(void)
{
    uint32_t lo, hi;
    __asm__ volatile (
        "lfence\n\t"
        "rdtsc"
        : "=a"(lo), "=d"(hi)
        :
        : "memory"
    );
    return ((uint64_t)hi << 32) | lo;
}

/*
 * tsc_read_end — use at the END of a measured interval.
 * RDTSCP is self-serialising: it waits for all prior instructions to retire
 * before reading the counter, so the measured code cannot "escape" past it.
 * The trailing LFENCE prevents subsequent loads from reordering before the
 * RDTSCP, which matters if the result is used immediately in a comparison.
 */
static inline uint64_t tsc_read_end(void)
{
    uint32_t lo, hi, aux;   /* aux receives IA32_TSC_AUX (core/socket id) */
    __asm__ volatile (
        "rdtscp\n\t"
        "lfence"
        : "=a"(lo), "=d"(hi), "=c"(aux)
        :
        : "memory"
    );
    (void)aux;
    return ((uint64_t)hi << 32) | lo;
}

/*
 * tsc_read — general-purpose serialised read (LFENCE + RDTSC).
 * Use for single-point readings where direction doesn't matter
 * (e.g. receiver timestamp in benchmark 3, overhead measurement).
 */
static inline uint64_t tsc_read(void)
{
    return tsc_read_start();
}

/*
 * tsc_read_bare — unserialized RDTSC with only a compiler barrier.
 *
 * No LFENCE or RDTSCP. Safe to use for interval endpoints in bench 1
 * when the interval being measured (50–500+ ns) vastly exceeds any
 * CPU reordering window (~1–5 cycles), and where serialising
 * instructions would perturb store-buffer state after a CAS.
 *
 * This matches what quanta::Clock::raw() does on x86-64.
 */
static inline uint64_t tsc_read_bare(void)
{
    uint32_t lo, hi;
    __asm__ volatile (
        "rdtsc"
        : "=a"(lo), "=d"(hi)
        :
        : "memory"
    );
    return ((uint64_t)hi << 32) | lo;
}

/*
 * tsc_drain_stores — MFENCE before the start timestamp of each sample.
 *
 * Drains the store buffer so no dirty lines from the previous sample's
 * final CAS write are still in-flight when we start timing. Costs ~30
 * cycles, paid once per sample — negligible vs num_iterations * RTT.
 */
static inline void tsc_drain_stores(void)
{
    __asm__ volatile ("mfence" ::: "memory");
}

/* Convert a raw tick delta to nanoseconds. */
static inline double tsc_ticks_to_ns(uint64_t ticks)
{
    return (double)ticks / g_tsc_ghz;
}
