/*
 * core-to-core-latency: C port of https://github.com/nviennot/core-to-core-latency
 *
 * Measures inter-core communication latency using three benchmarks:
 *   1. CAS latency on a single shared cache line (contested spinlock proxy)
 *   2. Single-writer/single-reader ping-pong on two separate cache lines
 *   3. One-way message passing using RDTSC timestamps
 *
 * Target: Linux x86-64, GCC/Clang, _POSIX_C_SOURCE >= 200809L
 *
 * Build:
 *   gcc -O2 -march=native -o c2c main.c bench_cas.c bench_rw.c bench_msg.c \
 *       tsc.c stats.c utils.c -lpthread -lm
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <math.h>

#include "c2c.h"
#include "tsc.h"
#include "utils.h"
#include "stats.h"
#include "topology.h"

/* ------------------------------------------------------------------ */
/* Defaults                                                             */
/* ------------------------------------------------------------------ */
#define DEFAULT_NUM_SAMPLES    300
#define DEFAULT_NUM_ITERATIONS 1000

/* ------------------------------------------------------------------ */
/* Usage                                                                */
/* ------------------------------------------------------------------ */
static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [OPTIONS] [num_iterations [num_samples]]\n"
        "\n"
        "Options:\n"
        "  -b, --bench LIST   Comma-separated benchmark IDs to run (default: 1)\n"
        "                       1: CAS latency on a single shared cache line\n"
        "                       2: Single-writer single-reader on two cache lines\n"
        "                       3: One-way message passing via RDTSC timestamps\n"
        "                       4: Direct store/load, per-iteration histograms (recommended)\n"
        "  -c, --cores LIST   Core IDs to test: individual, ranges, or mixed (default: all)\n"
        "                       Individual:  -c 0,1,4,8\n"
        "                       Range:       -c 0-127\n"
        "                       Mixed:       -c 0-63,128-191\n"
        "      --csv          Save mean latency matrix as CSV to output.csv\n"
        "      --topology     Enable AMD Zen 4+ topology-aware mode (bench 4 only):\n"
        "                       - CCD-aware parallel scheduling\n"
        "                       - Conditional PAUSE in spin loops\n"
        "                       - L3 prefetch warm-up per pair\n"
        "                       - Tiered output (intra-CCD / cross-CCD / cross-NUMA)\n"
        "  -h, --help         Show this help\n"
        "\n"
        "Positional arguments:\n"
        "  num_iterations     Round-trips (or messages) per sample (default: %d)\n"
        "  num_samples        Number of samples per core pair (default: %d)\n"
        "\n"
        "Examples:\n"
        "  %s                              Run benchmark 1 on all cores\n"
        "  %s -b 4                         Run benchmark 4 (recommended) on all cores\n"
        "  %s -b 1 -c 0-127               Socket 0 only on a 2-socket 256-core system\n"
        "  %s -b 1 -c 0-63,128-191        Socket 0 with HT (interleaved logical CPUs)\n"
        "  %s -b 1,4                       Run benchmarks 1 and 4\n"
        "  %s -b 4 --csv                   Run benchmark 4 and save results to output.csv\n"
        "  %s -b 1 1000 50                 1000 iterations, 50 samples (faster run)\n"
        "  %s -b 4 -c 0-15 5000 1000      High-precision run on first 16 cores\n"
        "  %s -b 4 --topology             AMD Zen 4: CCD-aware measurement\n",
        prog, DEFAULT_NUM_ITERATIONS, DEFAULT_NUM_SAMPLES,
        prog, prog, prog, prog, prog, prog, prog, prog, prog);
}

/* ------------------------------------------------------------------ */
/* Parse a comma-separated list of integers and ranges.                */
/* Accepts: "0,1,2", "0-7", "0-3,8-11,16", "0-63,128-191"            */
/* Returns count, fills out[].  out must be pre-allocated large enough.*/
/* ------------------------------------------------------------------ */
static int parse_int_list(const char *s, int *out, int max_out)
{
    int count = 0;
    char *buf = strdup(s);
    char *tok = strtok(buf, ",");
    while (tok && count < max_out) {
        int a, b;
        if (sscanf(tok, "%d-%d", &a, &b) == 2) {
            /* Range: e.g. "0-127" */
            if (a < 0 || b < a) {
                fprintf(stderr, "Invalid range: '%s'\n", tok);
                free(buf);
                return -1;
            }
            for (int v = a; v <= b && count < max_out; v++)
                out[count++] = v;
        } else if (sscanf(tok, "%d", &a) == 1) {
            /* Single value */
            if (a < 0) {
                fprintf(stderr, "Invalid core id: '%s'\n", tok);
                free(buf);
                return -1;
            }
            out[count++] = a;
        } else {
            fprintf(stderr, "Invalid core specification: '%s'\n", tok);
            free(buf);
            return -1;
        }
        tok = strtok(NULL, ",");
    }
    free(buf);
    return count;
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    int opt;
    int csv = 0;
    int use_topology = 0;
    int bench_ids[8]   = {1};
    int n_bench_ids    = 1;
    int req_cores[MAX_CORES];
    int n_req_cores    = 0;
    uint32_t num_iterations = DEFAULT_NUM_ITERATIONS;
    uint32_t num_samples    = DEFAULT_NUM_SAMPLES;

    static struct option long_opts[] = {
        {"bench",    required_argument, 0, 'b'},
        {"cores",    required_argument, 0, 'c'},
        {"csv",      no_argument,       0,  1 },
        {"topology", no_argument,       0,  2 },
        {"help",     no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    while ((opt = getopt_long(argc, argv, "b:c:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'b':
            n_bench_ids = parse_int_list(optarg, bench_ids, 8);
            if (n_bench_ids <= 0) return 1;
            break;
        case 'c':
            n_req_cores = parse_int_list(optarg, req_cores, MAX_CORES);
            if (n_req_cores <= 0) return 1;
            break;
        case 1:
            csv = 1;
            break;
        case 2:
            use_topology = 1;
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    /* Positional args */
    if (optind < argc) num_iterations = (uint32_t)atoi(argv[optind++]);
    if (optind < argc) num_samples    = (uint32_t)atoi(argv[optind++]);

    /* ---- Discover available cores ---------------------------------- */
    int all_cores[MAX_CORES];
    int n_all_cores = get_available_cores(all_cores, MAX_CORES);
    if (n_all_cores < 2) {
        fprintf(stderr, "Need at least 2 cores, found %d\n", n_all_cores);
        return 1;
    }

    int cores[MAX_CORES];
    int n_cores;

    if (n_req_cores > 0) {
        /* Validate requested cores exist */
        n_cores = 0;
        for (int i = 0; i < n_req_cores; i++) {
            int found = 0;
            for (int j = 0; j < n_all_cores; j++) {
                if (all_cores[j] == req_cores[i]) { found = 1; break; }
            }
            if (!found) {
                fprintf(stderr, "Core %d not found. Available:", req_cores[i]);
                for (int j = 0; j < n_all_cores; j++) fprintf(stderr, " %d", all_cores[j]);
                fprintf(stderr, "\n");
                return 1;
            }
            cores[n_cores++] = req_cores[i];
        }
    } else {
        n_cores = n_all_cores;
        memcpy(cores, all_cores, n_cores * sizeof(int));
    }

    /* ---- Print header info ----------------------------------------- */
    show_cpuid_info();
    fprintf(stderr, "Num cores: %d\n", n_cores);
    fprintf(stderr, "Num iterations per sample: %u\n", num_iterations);
    fprintf(stderr, "Num samples: %u\n", num_samples);

    /* ---- Calibrate TSC --------------------------------------------- */
    tsc_init();
    fprintf(stderr, "TSC frequency: %.3f GHz  (1 tick = %.3f ns)\n",
            g_tsc_ghz, 1.0 / g_tsc_ghz);

    /* ---- Topology (optional) --------------------------------------- */
    topology_t topo;
    topology_t *topo_ptr = NULL;

    if (use_topology) {
        int zen = detect_zen_gen();
        if (zen <= 0) {
            fprintf(stderr,
                "WARN: --topology requires AMD Zen 4+. "
                "Detected: %s. Topology mode disabled.\n",
                zen == 0 ? "non-AMD CPU" : "pre-Zen AMD");
        } else if (zen < 4) {
            fprintf(stderr,
                "WARN: --topology is optimised for Zen 4+. "
                "Detected Zen %d — enabling anyway, results may vary.\n",
                zen);
            if (topology_build(&topo, cores, n_cores))
                topo_ptr = &topo;
        } else {
            fprintf(stderr, "Topology mode: AMD Zen %d detected\n", zen);
            if (topology_build(&topo, cores, n_cores))
                topo_ptr = &topo;
            else
                fprintf(stderr,
                    "WARN: topology detection failed. "
                    "Continuing without topology mode.\n");
        }
    }

    /* ---- Run benchmarks -------------------------------------------- */
    for (int bi = 0; bi < n_bench_ids; bi++) {
        int b = bench_ids[bi];
        bench_config_t cfg = {
            .cores          = cores,
            .n_cores        = n_cores,
            .num_iterations = num_iterations,
            .num_samples    = num_samples,
            .csv            = csv,
            .topo           = topo_ptr,
        };

        /* Warn if --topology is passed with a benchmark that ignores it */
        if (use_topology && b != 4)
            fprintf(stderr,
                "WARN: --topology has no effect on benchmark %d. "
                "Use -b 4 --topology for topology-aware measurement.\n", b);

        /* Runtime estimate */
        {
            long n_pairs = (long)n_cores * (n_cores - 1) / 2;
            /* Sequential benchmarks (1,2,3): one pair at a time */
            /* Parallel benchmark (4): n/2 pairs per round, n-1 rounds */
            long effective_pairs = (b == 4)
                ? (n_cores - 1)          /* rounds, each ~same cost as 1 pair */
                : n_pairs;
            /* Conservative avg round-trip: 50 ns covers most topologies */
            double est_ns  = (double)effective_pairs
                             * (num_samples + 5)
                             * num_iterations
                             * 50.0;
            double est_sec = est_ns / 1e9;
            if (est_sec < 60)
                fprintf(stderr, "  Estimated runtime: %.0f seconds\n", est_sec);
            else
                fprintf(stderr, "  Estimated runtime: %.1f minutes\n", est_sec / 60.0);
        }

        switch (b) {
        case 1:
            fprintf(stderr, "\n1) CAS latency on a single shared cache line\n\n");
            run_bench_cas(&cfg);
            break;
        case 2:
            fprintf(stderr, "\n2) Single-writer single-reader latency on two shared cache lines\n\n");
            run_bench_rw(&cfg);
            break;
        case 3:
            assert_rdtsc_usable();
            fprintf(stderr, "\n3) Message passing. One writer and one reader on many cache lines\n\n");
            run_bench_msg(&cfg);
            break;
        case 4:
            assert_rdtsc_usable();
            fprintf(stderr, "\n4) Direct store/load latency — per-iteration histograms\n");
            fprintf(stderr, "   (persistent threads, plain MOV not LOCK CMPXCHG, symmetric)\n");
            run_bench_direct(&cfg);
            break;
        default:
            fprintf(stderr, "Unknown benchmark id %d (must be 1, 2, or 3)\n", b);
            return 1;
        }
    }

    return 0;
}
