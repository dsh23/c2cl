#pragma once
/*
 * topology.h — AMD Zen 4+ topology detection and advisory.
 *
 * Provides:
 *   - CCD assignment per logical CPU (from L3 cache domain)
 *   - NUMA distance matrix (from sysfs)
 *   - Infinity Fabric frequency check
 *   - Zen generation detection (CPUID family/model)
 *   - Pair classification: HT sibling / intra-CCD / cross-CCD / cross-NUMA
 *   - Tiered latency summary (intra-CCD vs cross-CCD vs cross-socket)
 *
 * All topology information is read once at startup and stored in
 * topology_t. Passing topology_t* = NULL to any function disables
 * topology-aware behaviour and falls back to generic operation.
 *
 * AMD Zen 4 CCD layout (reference):
 *   - 8 cores per CCD, shared 32 MB L3
 *   - Zen 3+: full CCD = single CCX (no intra-CCD CCX split)
 *   - Zen 2:  CCD = 2 × 4-core CCX with separate L3 per CCX
 *   - Infinity Fabric connects CCDs; latency proportional to hop count
 */

#include <stdint.h>
#include "c2c.h"

/* ------------------------------------------------------------------ */
/* Pair classification                                                  */
/* ------------------------------------------------------------------ */

typedef enum {
    PAIR_HT_SIBLING  = 0,   /* same physical core, HT threads           */
    PAIR_INTRA_CCD   = 1,   /* different cores, same CCD (shared L3)    */
    PAIR_CROSS_CCD   = 2,   /* different CCDs, same NUMA node / socket  */
    PAIR_CROSS_NUMA  = 3,   /* different NUMA nodes / sockets           */
    PAIR_UNKNOWN     = 4,   /* topology info not available              */
} pair_class_t;

/* ------------------------------------------------------------------ */
/* Topology structure                                                   */
/* ------------------------------------------------------------------ */

#define MAX_CCDS   64
#define MAX_NUMA   16
#define INVALID_CCD  -1
#define INVALID_NUMA -1

typedef struct topology_t {
    /* Per-logical-CPU data, indexed by CPU ID */
    int  cpu_ccd [MAX_CORES];   /* CCD index (0-based, normalised)     */
    int  cpu_numa[MAX_CORES];   /* NUMA node                           */

    /* CCD → list of member CPUs */
    int  ccd_cpus[MAX_CCDS][MAX_CORES];
    int  ccd_size[MAX_CCDS];
    int  n_ccds;

    /* NUMA distance matrix */
    int  numa_dist[MAX_NUMA][MAX_NUMA];
    int  n_numa;

    /* CPU count this topology covers */
    int  n_cpus;       /* highest CPU ID + 1                           */

    /* AMD Zen generation (4 = Zen4, 3 = Zen3, etc.; 0 = unknown/non-AMD) */
    int  zen_gen;

    /* Infinity Fabric frequency in MHz (0 = not readable) */
    int  fclk_mhz;
} topology_t; /* matches forward decl in c2c.h */

/* ------------------------------------------------------------------ */
/* API                                                                  */
/* ------------------------------------------------------------------ */

/*
 * Detect AMD Zen generation from CPUID.
 * Returns 4 for Zen4, 3 for Zen3, etc.
 * Returns 0 if not AMD or unrecognised.
 * Returns -1 if AMD but older than Zen (pre-Zen).
 */
int detect_zen_gen(void);

/*
 * Build topology for the given set of core IDs.
 * Reads /sys/devices/system/cpu/cpuN/cache/index3/id for CCD,
 * /sys/devices/system/cpu/cpuN/topology/physical_package_id for socket,
 * /sys/devices/system/node/nodeN/distance for NUMA distances.
 * Returns 1 on success, 0 if topology could not be determined.
 */
int topology_build(topology_t *t, const int *cores, int n_cores);

/*
 * Print a topology summary to stderr:
 *   - Zen generation
 *   - CCD layout (which CPUs are in each CCD)
 *   - NUMA distances
 *   - Infinity Fabric frequency (if readable)
 */
void topology_print(const topology_t *t, const int *cores, int n_cores);

/*
 * Classify a pair of logical CPUs.
 * Returns PAIR_UNKNOWN if t is NULL or CPUs are out of range.
 */
pair_class_t topology_classify(const topology_t *t, int cpu_a, int cpu_b);

/*
 * Return a short label string for a pair class.
 */
const char *pair_class_name(pair_class_t cls);

/*
 * Check and warn about Infinity Fabric frequency.
 * Reads /sys/devices/system/cpu/amd_pstate or hwmon fclk.
 * Prints advisory to stderr if fclk is below maximum.
 */
void topology_check_fclk(topology_t *t);

/*
 * Print tiered latency summary:
 *   Min intra-CCD latency: X ns  (cores A,B  CCD N)
 *   Min cross-CCD latency: X ns  (cores A,B)
 *   Min cross-NUMA latency: X ns (cores A,B)
 * means[i*n+j] is the mean latency for pair (cores[i], cores[j]).
 * n is the number of cores.
 */
void topology_print_tiered_summary(const topology_t *t,
                                   const int *cores, int n,
                                   const double *means);
