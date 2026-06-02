
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sched.h>
#include <pthread.h>
#include <cpuid.h>   /* GCC/Clang built-in */

#include "utils.h"
#include "tsc.h"
#include "c2c.h"

/* ------------------------------------------------------------------ */
/* Core enumeration                                                     */
/* ------------------------------------------------------------------ */

/*
 * Parse a kernel CPU list string of the form "0-3,5,7-9" into an array
 * of individual CPU IDs.  Returns the number of CPUs parsed.
 */
static int parse_cpu_list(const char *s, int *out, int max_out)
{
    int count = 0;
    char *buf = strdup(s);
    char *tok = strtok(buf, ",");

    while (tok && count < max_out) {
        int a, b;
        if (sscanf(tok, "%d-%d", &a, &b) == 2) {
            for (int i = a; i <= b && count < max_out; i++)
                out[count++] = i;
        } else if (sscanf(tok, "%d", &a) == 1) {
            out[count++] = a;
        }
        tok = strtok(NULL, ",");
    }

    free(buf);
    return count;
}

int get_available_cores(int *out, int max_out)
{
    FILE *f = fopen("/sys/devices/system/cpu/online", "r");
    if (!f) {
        perror("open /sys/devices/system/cpu/online");
        exit(1);
    }

    char buf[4096];
    if (!fgets(buf, sizeof(buf), f)) {
        fclose(f);
        fprintf(stderr, "Failed to read cpu list\n");
        exit(1);
    }
    fclose(f);

    /* Strip trailing newline */
    buf[strcspn(buf, "\n")] = '\0';

    return parse_cpu_list(buf, out, max_out);
}

/* ------------------------------------------------------------------ */
/* Thread pinning                                                       */
/* ------------------------------------------------------------------ */

void pin_thread_to_core(int core_id)
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);

    int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    if (rc != 0) {
        fprintf(stderr, "pthread_setaffinity_np(core %d): %s\n",
                core_id, strerror(rc));
        exit(1);
    }
}

/* ------------------------------------------------------------------ */
/* HT sibling detection                                                 */
/* ------------------------------------------------------------------ */

int are_ht_siblings(int cpu_a, int cpu_b)
{
    /*
     * /sys/devices/system/cpu/cpuN/topology/thread_siblings_list
     * contains a CPU list string (e.g. "0,4" or "2-3") of all logical
     * CPUs that share the same physical core as cpu N.
     * cpu_a and cpu_b are siblings iff cpu_b appears in cpu_a's list.
     */
    char path[128];
    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list",
             cpu_a);

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char buf[256];
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return 0; }
    fclose(f);

    buf[strcspn(buf, "\n")] = '\0';

    /* Reuse parse_cpu_list via a local parse of the siblings string */
    int siblings[MAX_CORES];
    int count = 0;
    char *tmp = strdup(buf);
    char *tok = strtok(tmp, ",");
    while (tok && count < MAX_CORES) {
        int a, b;
        if (sscanf(tok, "%d-%d", &a, &b) == 2) {
            for (int i = a; i <= b && count < MAX_CORES; i++)
                siblings[count++] = i;
        } else if (sscanf(tok, "%d", &a) == 1) {
            siblings[count++] = a;
        }
        tok = strtok(NULL, ",");
    }
    free(tmp);

    for (int i = 0; i < count; i++)
        if (siblings[i] == cpu_b) return 1;

    return 0;
}

/* ------------------------------------------------------------------ */
/* CPUID helpers                                                        */
/* ------------------------------------------------------------------ */

void show_cpuid_info(void)
{
    uint32_t eax, ebx, ecx, edx;

    /* Brand string is in leaves 0x80000002..0x80000004, 12 bytes each */
    uint32_t brand[12];
    memset(brand, 0, sizeof(brand));

    /* Check extended CPUID support */
    if (__get_cpuid(0x80000000, &eax, &ebx, &ecx, &edx) && eax >= 0x80000004) {
        __get_cpuid(0x80000002,
                    &brand[0], &brand[1], &brand[2],  &brand[3]);
        __get_cpuid(0x80000003,
                    &brand[4], &brand[5], &brand[6],  &brand[7]);
        __get_cpuid(0x80000004,
                    &brand[8], &brand[9], &brand[10], &brand[11]);

        /* Brand string may have leading spaces */
        const char *s = (const char *)brand;
        while (*s == ' ') s++;
        fprintf(stderr, "CPU: %s\n", s);
    }
}

/*
 * Check CPUID 0x80000007 EDX bit 8 = InvariantTSC.
 * Returns 1 if invariant TSC is present, 0 otherwise.
 */
static int has_invariant_tsc(void)
{
    uint32_t eax, ebx, ecx, edx;
    if (!__get_cpuid(0x80000007, &eax, &ebx, &ecx, &edx))
        return 0;
    return (edx >> 8) & 1;
}

void assert_rdtsc_usable(void)
{
    if (!has_invariant_tsc()) {
        fprintf(stderr,
            "ERROR: This CPU does not have an invariant TSC "
            "(CPUID 0x80000007 EDX bit 8 = 0).\n"
            "Benchmark 3 requires invariant TSC for accurate one-way "
            "latency measurement.\n");
        exit(1);
    }

    /* Measure the overhead of reading the TSC 10000 times */
    const int N = 10000;
    uint64_t t0 = tsc_read();
    for (int i = 0; i < N - 1; i++) {
        uint64_t t = tsc_read();
        black_box(&t);
    }
    uint64_t t1 = tsc_read();

    double overhead_ns = tsc_ticks_to_ns(t1 - t0) / N;
    fprintf(stderr, "Reading the clock via RDTSC takes %.2f ns\n", overhead_ns);

    if (overhead_ns < 0.1 || overhead_ns > 1000.0) {
        fprintf(stderr,
            "ERROR: RDTSC overhead (%.2f ns) is outside expected range "
            "[0.1, 1000] ns.\n"
            "TSC may not be usable as a timing source on this system.\n",
            overhead_ns);
        exit(1);
    }
}
