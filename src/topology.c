/*
 * topology.c — AMD Zen 4+ topology detection implementation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <cpuid.h>
#include <dirent.h>
#include <unistd.h>

#include "topology.h"
#include "utils.h"

/* ------------------------------------------------------------------ */
/* Zen generation detection                                             */
/* ------------------------------------------------------------------ */

/*
 * AMD CPUID family/model decoding:
 *
 * Extended family = (EAX >> 20) & 0xFF
 * Extended model  = (EAX >> 16) & 0xF
 * Base family     = (EAX >> 8)  & 0xF
 * Base model      = (EAX >> 4)  & 0xF
 *
 * AMD Zen generations:
 *   Zen 1:  Family 0x17, Model 0x00–0x0F
 *   Zen+:   Family 0x17, Model 0x10–0x1F
 *   Zen 2:  Family 0x17, Model 0x30–0x7F
 *   Zen 3:  Family 0x19, Model 0x00–0x0F, 0x20–0x5F
 *   Zen 4:  Family 0x19, Model 0x10–0x1F, 0x60–0x7F
 *   Zen 5:  Family 0x1A
 */
int detect_zen_gen(void)
{
    uint32_t eax, ebx, ecx, edx;

    /* Check vendor = "AuthenticAMD" */
    if (!__get_cpuid(0, &eax, &ebx, &ecx, &edx)) return 0;
    /* ebx="Auth", edx="enti", ecx="cAMD" */
    if (ebx != 0x68747541 || edx != 0x69746e65 || ecx != 0x444d4163)
        return 0;  /* not AMD */

    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) return 0;

    int ext_family  = (eax >> 20) & 0xFF;
    int ext_model   = (eax >> 16) & 0xF;
    int base_family = (eax >> 8)  & 0xF;
    int base_model  = (eax >> 4)  & 0xF;

    int family = base_family + (base_family == 0xF ? ext_family : 0);
    int model  = base_model  | (ext_model << 4);

    if (family == 0x17) {
        if (model <= 0x0F) return 1;  /* Zen 1 */
        if (model <= 0x1F) return 1;  /* Zen+  */
        return 2;                      /* Zen 2 */
    }
    if (family == 0x19) {
        /* Zen 3 vs Zen 4 determined by model range */
        if (model <= 0x0F || (model >= 0x20 && model <= 0x5F)) return 3;
        return 4;                      /* Zen 4 */
    }
    if (family == 0x1A) return 5;      /* Zen 5 */
    if (family >= 0x15) return -1;     /* pre-Zen AMD */
    return 0;
}

/* ------------------------------------------------------------------ */
/* sysfs helpers                                                        */
/* ------------------------------------------------------------------ */

static int read_int_from_file(const char *path, int *out)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int ok = (fscanf(f, "%d", out) == 1);
    fclose(f);
    return ok;
}

/*
 * Read the L3 cache domain ID for a CPU.
 *
 * The kernel exposes each cache via /sys/devices/system/cpu/cpuN/cache/indexM/
 * with attributes:
 *   type   — "Data", "Instruction", or "Unified"
 *   level  — "1", "2", "3", ...
 *   id     — domain identifier; CPUs sharing a cache share an id
 *
 * On AMD Zen, the L2 cache is per-physical-core (Unified) and the L3 cache
 * is per-CCD (Unified). Filtering only on type=="Unified" therefore returns
 * the L2 domain (per-core) on Zen, which is wrong — we need the L3.
 *
 * This function explicitly checks the level attribute and returns the id
 * of the first cache at level 3.
 *
 * Returns -1 on failure.
 */
static int get_l3_id(int cpu)
{
    char path[256];

    for (int idx = 0; idx <= 7; idx++) {
        char level_path[256];
        snprintf(level_path, sizeof(level_path),
                 "/sys/devices/system/cpu/cpu%d/cache/index%d/level",
                 cpu, idx);
        FILE *lf = fopen(level_path, "r");
        if (!lf) continue;
        int level = 0;
        int got = fscanf(lf, "%d", &level);
        fclose(lf);
        if (got != 1 || level != 3) continue;

        /* Found L3 — read its id */
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/cache/index%d/id",
                 cpu, idx);
        int id;
        if (read_int_from_file(path, &id))
            return id;
    }
    return -1;
}

static int get_numa_node(int cpu)
{
    /*
     * Find which NUMA node this CPU belongs to by checking
     * /sys/devices/system/node/nodeN/cpulist.
     */
    char path[256];
    for (int node = 0; node < MAX_NUMA; node++) {
        snprintf(path, sizeof(path),
                 "/sys/devices/system/node/node%d/cpumap", node);
        /* Simpler: check if the cpu symlink exists under nodeN */
        char cpu_path[256];
        snprintf(cpu_path, sizeof(cpu_path),
                 "/sys/devices/system/node/node%d/cpu%d", node, cpu);
        if (access(cpu_path, F_OK) == 0) return node;
    }
    /* Fallback: read from cpu topology */
    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%d/topology/physical_package_id",
             cpu);
    int id = 0;
    read_int_from_file(path, &id);
    return id;
}

/* ------------------------------------------------------------------ */
/* topology_build                                                       */
/* ------------------------------------------------------------------ */

int topology_build(topology_t *t, const int *cores, int n_cores)
{
    memset(t, 0, sizeof(*t));

    for (int i = 0; i < MAX_CORES; i++) {
        t->cpu_ccd [i] = INVALID_CCD;
        t->cpu_numa[i] = INVALID_NUMA;
    }

    t->zen_gen = detect_zen_gen();

    /* ---- Per-CPU CCD and NUMA assignment -------------------------- */
    int max_cpu = 0;
    int l3_ids[MAX_CORES];   /* raw L3 domain IDs, need normalising */
    for (int i = 0; i < MAX_CORES; i++) l3_ids[i] = INVALID_CCD;

    for (int i = 0; i < n_cores; i++) {
        int cpu = cores[i];
        if (cpu >= MAX_CORES) continue;
        if (cpu > max_cpu) max_cpu = cpu;

        l3_ids[cpu]      = get_l3_id(cpu);
        t->cpu_numa[cpu] = get_numa_node(cpu);
    }

    /* Normalise raw L3 IDs to 0-based CCD indices */
    int id_map[65536];
    memset(id_map, -1, sizeof(id_map));
    t->n_ccds = 0;

    for (int i = 0; i < n_cores; i++) {
        int cpu = cores[i];
        if (cpu >= MAX_CORES) continue;
        int raw = l3_ids[cpu];
        if (raw < 0 || raw >= 65536) {
            t->cpu_ccd[cpu] = INVALID_CCD;
            continue;
        }
        if (id_map[raw] < 0) {
            id_map[raw] = t->n_ccds++;
        }
        t->cpu_ccd[cpu] = id_map[raw];
    }

    /* Build CCD → CPU lists */
    for (int i = 0; i < n_cores; i++) {
        int cpu = cores[i];
        if (cpu >= MAX_CORES) continue;
        int ccd = t->cpu_ccd[cpu];
        if (ccd < 0 || ccd >= MAX_CCDS) continue;
        if (t->ccd_size[ccd] < MAX_CORES)
            t->ccd_cpus[ccd][t->ccd_size[ccd]++] = cpu;
    }

    /* ---- NUMA distances ------------------------------------------- */
    t->n_numa = 0;
    for (int i = 0; i < n_cores; i++) {
        int node = t->cpu_numa[cores[i]];
        if (node >= 0 && node + 1 > t->n_numa)
            t->n_numa = node + 1;
    }

    for (int a = 0; a < t->n_numa && a < MAX_NUMA; a++) {
        char path[256];
        snprintf(path, sizeof(path),
                 "/sys/devices/system/node/node%d/distance", a);
        FILE *f = fopen(path, "r");
        if (!f) {
            for (int b = 0; b < MAX_NUMA; b++)
                t->numa_dist[a][b] = (a == b) ? 10 : 20;
            continue;
        }
        for (int b = 0; b < MAX_NUMA; b++) {
            if (fscanf(f, "%d", &t->numa_dist[a][b]) != 1)
                t->numa_dist[a][b] = (a == b) ? 10 : 20;
        }
        fclose(f);
    }

    t->n_cpus  = max_cpu + 1;
    t->fclk_mhz = 0;

    return (t->n_ccds > 0);
}

/* ------------------------------------------------------------------ */
/* topology_check_fclk                                                  */
/* ------------------------------------------------------------------ */

void topology_check_fclk(topology_t *t)
{
    /*
     * Try to read Infinity Fabric clock from hwmon.
     * AMD exposes fclk under /sys/class/hwmon/hwmonN/freq*_label
     * where the label is "Fabric Clk" or similar.
     * Also try /sys/devices/system/cpu/amd_pstate/fclk.
     */
    int fclk = 0;

    /* Method 1: amd_pstate */
    {
        FILE *f = fopen("/sys/devices/system/cpu/amd_pstate/fclk", "r");
        if (f) {
            int rc = fscanf(f, "%d", &fclk); (void)rc;
            fclose(f);
            if (fclk > 0) { t->fclk_mhz = fclk / 1000; goto done; }
        }
    }

    /* Method 2: scan hwmon for "Fabric Clk" or "fclk" */
    {
        DIR *d = opendir("/sys/class/hwmon");
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                if (de->d_name[0] == '.') continue;
                /* Scan freq*_label */
                for (int n = 1; n <= 32; n++) {
                    char label_path[512], input_path[512];
                    snprintf(label_path, sizeof(label_path),
                             "/sys/class/hwmon/%s/freq%d_label",
                             de->d_name, n);
                    FILE *lf = fopen(label_path, "r");
                    if (!lf) break;
                    char label[64] = {0};
                    char *rg = fgets(label, sizeof(label), lf); (void)rg;
                    fclose(lf);
                    if (strstr(label, "Fabric") || strstr(label, "fclk") ||
                        strstr(label, "FCLK")) {
                        snprintf(input_path, sizeof(input_path),
                                 "/sys/class/hwmon/%s/freq%d_input",
                                 de->d_name, n);
                        long hz = 0;
                        FILE *inf = fopen(input_path, "r");
                        if (inf) { int ri = fscanf(inf, "%ld", &hz); (void)ri; fclose(inf); }
                        if (hz > 0) { fclk = (int)(hz / 1000000); }
                    }
                }
            }
            closedir(d);
        }
    }

    t->fclk_mhz = fclk;

done:
    if (t->fclk_mhz > 0) {
        /*
         * Zen 4 standard FCLK: 1800 MHz (coupled UCLK=MCLK=FCLK).
         * Warn if below 1600 MHz — indicates decoupled / power-save mode.
         */
        int threshold = 1600;
        if (t->fclk_mhz < threshold) {
            fprintf(stderr,
                "  WARN: Infinity Fabric running at %d MHz (expected >= %d MHz).\n"
                "  Cross-CCD latency will be elevated. Set FCLK in BIOS/AGESA\n"
                "  to coupled mode (UCLK = MCLK = FCLK = 1800 MHz for DDR5-3600).\n",
                t->fclk_mhz, threshold);
        } else {
            fprintf(stderr,
                "  Infinity Fabric: %d MHz\n", t->fclk_mhz);
        }
    }
}

/* ------------------------------------------------------------------ */
/* topology_print                                                       */
/* ------------------------------------------------------------------ */

void topology_print(const topology_t *t, const int *cores, int n_cores)
{
    /* Zen generation label */
    const char *zen_label = "Unknown";
    switch (t->zen_gen) {
    case 1:  zen_label = "Zen / Zen+";  break;
    case 2:  zen_label = "Zen 2";       break;
    case 3:  zen_label = "Zen 3";       break;
    case 4:  zen_label = "Zen 4";       break;
    case 5:  zen_label = "Zen 5";       break;
    case -1: zen_label = "AMD (pre-Zen)"; break;
    case 0:  zen_label = "Non-AMD / undetected"; break;
    }
    fprintf(stderr, "  AMD architecture: %s\n", zen_label);

    if (t->n_ccds == 0) {
        fprintf(stderr, "  CCD topology: not available\n");
        return;
    }

    fprintf(stderr, "  CCDs detected: %d\n", t->n_ccds);
    for (int ccd = 0; ccd < t->n_ccds && ccd < MAX_CCDS; ccd++) {
        fprintf(stderr, "    CCD %d (%d cores):", ccd, t->ccd_size[ccd]);
        for (int k = 0; k < t->ccd_size[ccd]; k++)
            fprintf(stderr, " %d", t->ccd_cpus[ccd][k]);
        fprintf(stderr, "\n");
    }

    if (t->n_numa > 1) {
        fprintf(stderr, "  NUMA nodes: %d\n", t->n_numa);
        fprintf(stderr, "  NUMA distance matrix:\n");
        fprintf(stderr, "      ");
        for (int b = 0; b < t->n_numa; b++)
            fprintf(stderr, " %4d", b);
        fprintf(stderr, "\n");
        for (int a = 0; a < t->n_numa; a++) {
            fprintf(stderr, "    %2d:", a);
            for (int b = 0; b < t->n_numa; b++)
                fprintf(stderr, " %4d", t->numa_dist[a][b]);
            fprintf(stderr, "\n");
        }
    }
    (void)cores; (void)n_cores;
}

/* ------------------------------------------------------------------ */
/* topology_classify                                                    */
/* ------------------------------------------------------------------ */

pair_class_t topology_classify(const topology_t *t, int cpu_a, int cpu_b)
{
    if (!t || cpu_a >= MAX_CORES || cpu_b >= MAX_CORES)
        return PAIR_UNKNOWN;

    /* HT sibling check first */
    if (are_ht_siblings(cpu_a, cpu_b))
        return PAIR_HT_SIBLING;

    int ccd_a  = t->cpu_ccd [cpu_a];
    int ccd_b  = t->cpu_ccd [cpu_b];
    int numa_a = t->cpu_numa[cpu_a];
    int numa_b = t->cpu_numa[cpu_b];

    if (ccd_a == INVALID_CCD || ccd_b == INVALID_CCD)
        return PAIR_UNKNOWN;

    if (ccd_a == ccd_b)
        return PAIR_INTRA_CCD;

    /* Different CCDs: check NUMA */
    if (numa_a == INVALID_NUMA || numa_b == INVALID_NUMA)
        return PAIR_CROSS_CCD;   /* assume same socket if NUMA unknown */

    if (numa_a != numa_b)
        return PAIR_CROSS_NUMA;

    return PAIR_CROSS_CCD;
}

const char *pair_class_name(pair_class_t cls)
{
    switch (cls) {
    case PAIR_HT_SIBLING: return "HT-sibling";
    case PAIR_INTRA_CCD:  return "intra-CCD";
    case PAIR_CROSS_CCD:  return "cross-CCD";
    case PAIR_CROSS_NUMA: return "cross-NUMA";
    default:              return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/* topology_print_tiered_summary                                        */
/* ------------------------------------------------------------------ */

void topology_print_tiered_summary(const topology_t *t,
                                   const int *cores, int n,
                                   const double *means)
{
    if (!t) return;

    /* Track min and count per tier */
    double tier_min[5];
    int    tier_ai[5], tier_bi[5];
    long   tier_count[5] = {0};
    for (int c = 0; c < 5; c++) {
        tier_min[c] = 1e18;
        tier_ai[c] = tier_bi[c] = -1;
    }

    for (int i = 0; i < n; i++) {
        for (int j = 0; j < i; j++) {   /* lower triangle only */
            double m = means[i * n + j];
            pair_class_t cls = topology_classify(t, cores[i], cores[j]);
            tier_count[cls]++;
            if (isnan(m) || m <= 0.0) continue;
            if (m < tier_min[cls]) {
                tier_min[cls] = m;
                tier_ai[cls]  = i;
                tier_bi[cls]  = j;
            }
        }
    }

    fprintf(stderr, "\n  Latency by tier:\n");
    const pair_class_t tiers[] = {
        PAIR_HT_SIBLING, PAIR_INTRA_CCD, PAIR_CROSS_CCD, PAIR_CROSS_NUMA
    };
    for (int t2 = 0; t2 < 4; t2++) {
        pair_class_t cls = tiers[t2];
        if (tier_count[cls] == 0) {
            fprintf(stderr, "    %-14s  (no pairs in this run)\n",
                    pair_class_name(cls));
            continue;
        }
        if (tier_ai[cls] < 0) {
            fprintf(stderr, "    %-14s  %ld pairs, no valid measurements\n",
                    pair_class_name(cls), tier_count[cls]);
            continue;
        }
        int ci = cores[tier_ai[cls]];
        int cj = cores[tier_bi[cls]];
        int ccd_i = (t && ci < MAX_CORES) ? t->cpu_ccd[ci] : -1;
        int ccd_j = (t && cj < MAX_CORES) ? t->cpu_ccd[cj] : -1;
        fprintf(stderr, "    %-14s  min %.1f ns  cores (%d,%d)  [%ld pairs]",
                pair_class_name(cls), tier_min[cls], ci, cj, tier_count[cls]);
        if (ccd_i >= 0 && ccd_j >= 0) {
            if (ccd_i == ccd_j)
                fprintf(stderr, "  [CCD %d]", ccd_i);
            else
                fprintf(stderr, "  [CCD %d → CCD %d]", ccd_i, ccd_j);
        }
        fprintf(stderr, "\n");
    }
}
