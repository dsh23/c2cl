#pragma once
/*
 * utils.h — CPU affinity, CPUID helpers, delay, volatile black-box
 */

#include <stdint.h>
#include <sched.h>

/* ------------------------------------------------------------------ */
/* Compiler black-box: prevents the compiler from optimising away a    */
/* value. Equivalent to Rust's std::ptr::read_volatile / black_box.   */
/* ------------------------------------------------------------------ */
static inline void black_box(const volatile void *p)
{
    __asm__ volatile ("" : : "r"(p) : "memory");
}

/* ------------------------------------------------------------------ */
/* Delay loop: spin for approximately `n` iterations of light work.    */
/* Used by benchmark 3 to let the receiver reach its spin point before */
/* the sender writes each slot.                                         */
/*                                                                      */
/* Uses a plain volatile read — same as the Rust original's            */
/* read_volatile(&VALUE). PAUSE is intentionally NOT used here because  */
/* PAUSE serialises the pipeline and makes each iteration ~100 cycles   */
/* rather than ~5, turning a calibrated time delay into a much longer   */
/* stall that causes the sender to overshoot the receiver.              */
/* ------------------------------------------------------------------ */
static volatile uint64_t _delay_sink = 0;

static inline void delay_cycles(uint64_t n)
{
    for (uint64_t i = 0; i < n; i++) {
        (void)_delay_sink;   /* volatile load — prevents loop elimination */
    }
}

/* ------------------------------------------------------------------ */
/* Enumerate all online CPUs via /sys/devices/system/cpu/online        */
/* Returns count of cores, fills out[].                                */
/* ------------------------------------------------------------------ */
int get_available_cores(int *out, int max_out);

/* ------------------------------------------------------------------ */
/* Pin the calling thread to a specific core. Aborts on failure.       */
/* ------------------------------------------------------------------ */
void pin_thread_to_core(int core_id);

/* ------------------------------------------------------------------ */
/* HT sibling detection.                                               */
/*                                                                      */
/* Returns 1 if logical CPUs a and b are HT siblings (share a         */
/* physical core), 0 otherwise. Uses                                   */
/* /sys/devices/system/cpu/cpuN/topology/thread_siblings_list.         */
/* Returns 0 on any error — conservative, unknown pairs treated as    */
/* non-siblings so they are never excluded from min reporting.         */
/* ------------------------------------------------------------------ */
int are_ht_siblings(int cpu_a, int cpu_b);

/* ------------------------------------------------------------------ */
/* CPUID helpers                                                        */
/* ------------------------------------------------------------------ */

void show_cpuid_info(void);

/*
 * Assert RDTSC is usable:
 *   - Invariant TSC (CPUID 0x80000007 EDX bit 8)
 *   - Per-read overhead in plausible range [0.1, 1000] ns
 * Aborts with a diagnostic message on failure.
 */
void assert_rdtsc_usable(void);
