/*
 * bench_direct.c — Benchmark 4: parallel core-to-core latency with
 *                  optional AMD Zen topology-aware mode.
 *
 * TOPOLOGY-AWARE MODE (enabled via cfg->topo != NULL)
 * ─────────────────────────────────────────────────────
 * When --topology is passed and an AMD Zen 4+ CPU is detected:
 *
 *  1. CCD-aware scheduling: the round-robin schedule is reordered so
 *     that no two simultaneously running pairs share a CCD. Intra-CCD
 *     pairs run in rounds that contain only cross-CCD partners for the
 *     other slots, preventing L3 contention from inflating intra-CCD
 *     measurements.
 *
 *  2. AMD-aware spin loop: a conditional PAUSE is inserted after 8
 *     failed loads. On Zen 3+ PAUSE costs ~140 cycles; spinning without
 *     it floods the coherency bus and inflates the partner pair's
 *     latency. Issuing PAUSE every 8 iterations avoids ~87% of the bus
 *     traffic while adding only ~1 cycle average overhead per iteration.
 *
 *  3. L3 prefetch warm-up: before the warmup samples, each active
 *     thread issues a write prefetch to pull the slot into its local
 *     L3. Ensures the first warmup sample is representative rather than
 *     measuring a cold DRAM fetch.
 *
 *  4. Tiered output: results are reported grouped by pair class
 *     (HT-sibling / intra-CCD / cross-CCD / cross-NUMA) in addition to
 *     the standard full matrix.
 *
 * All four optimisations are no-ops when topo == NULL.
 *
 * PARALLEL SCHEDULING (always active)
 * ─────────────────────────────────────
 * Round-robin tournament: n cores → n-1 rounds × n/2 simultaneous pairs.
 * All n(n-1)/2 pairs covered in optimal time. Each core active every round.
 * Per-worker semaphores: idle workers sleep; no O(n) barrier broadcast.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <semaphore.h>
#include <sched.h>
#include <errno.h>
#include <math.h>
#include <unistd.h>

#include "c2c.h"
#include "tsc.h"
#include "utils.h"
#include "histogram.h"
#include "topology.h"

/* ------------------------------------------------------------------ */
/* Tuning                                                               */
/* ------------------------------------------------------------------ */

#define WARMUP_SAMPLES       5
#define RT_PRIORITY         10
#define FLAG_PING          0ULL
#define FLAG_PONG          1ULL
#define AMD_PAUSE_INTERVAL   8   /* issue PAUSE every N failed spin loads */

/* ------------------------------------------------------------------ */
/* Shared measurement slot                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    volatile uint64_t flag;
    char _pad[CACHE_LINE_SIZE - sizeof(uint64_t)];
} CACHE_ALIGNED slot_t;

/* ------------------------------------------------------------------ */
/* Per-pair samples                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t *ticks;
    uint32_t  n;
} pair_samples_t;

/* ------------------------------------------------------------------ */
/* Per-worker assignment                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    slot_t         *slot;
    pair_samples_t *samples;
    uint32_t        num_iters;
    uint32_t        total_samples;
    int             is_ping;
    int             active;
    int             use_amd_pause;   /* 1 = conditional PAUSE spin    */
    int             use_prefetch;    /* 1 = write-prefetch warm-up    */
} assignment_t;

/* ------------------------------------------------------------------ */
/* Worker                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    int          core_id;
    int          thread_idx;
    assignment_t assign;
    sem_t        sem_start;
    sem_t        sem_done;
} worker_t;

/* ------------------------------------------------------------------ */
/* Hot loops                                                            */
/*                                                                      */
/* Four variants, selected at runtime:                                  */
/*   generic ping/pong     — no AMD optimisations                      */
/*   amd ping/pong         — conditional PAUSE in spin                 */
/*                                                                      */
/* noinline prevents the compiler merging role bodies and introducing  */
/* spill code between the timed rdtsc and the first store/load.        */
/* ------------------------------------------------------------------ */

__attribute__((noinline))
static void hot_ping_generic(volatile uint64_t *flag,
                              uint32_t num_iters,
                              uint32_t total_samples,
                              uint64_t *out_ticks)
{
    for (uint32_t s = 0; s < total_samples; s++) {
        uint64_t t0 = tsc_read_bare();
        for (uint32_t i = 0; i < num_iters; i++) {
            *flag = FLAG_PING;
            while (*flag != FLAG_PONG) {}
        }
        out_ticks[s] = tsc_read_bare() - t0;
    }
}

__attribute__((noinline))
static void hot_pong_generic(volatile uint64_t *flag,
                              uint32_t num_iters,
                              uint32_t total_samples)
{
    for (uint32_t s = 0; s < total_samples; s++) {
        for (uint32_t i = 0; i < num_iters; i++) {
            while (*flag != FLAG_PING) {}
            *flag = FLAG_PONG;
        }
    }
}

/*
 * AMD variant: conditional PAUSE every AMD_PAUSE_INTERVAL failed loads.
 *
 * On Zen 3/4, PAUSE costs ~140 cycles. Issuing it on every spin
 * iteration would add ~140/num_iters ns to each sample — unacceptable.
 * Issuing it every 8 iterations adds only 140/8 = 17.5 cycles average
 * per iteration, but reduces coherency bus traffic by ~87%.
 *
 * The spin counter resets on each outer iteration so a fast cache-line
 * transfer (1–2 iterations) never hits the PAUSE path at all.
 */
__attribute__((noinline))
static void hot_ping_amd(volatile uint64_t *flag,
                          uint32_t num_iters,
                          uint32_t total_samples,
                          uint64_t *out_ticks)
{
    for (uint32_t s = 0; s < total_samples; s++) {
        uint64_t t0 = tsc_read_bare();
        for (uint32_t i = 0; i < num_iters; i++) {
            *flag = FLAG_PING;
            int spin = 0;
            while (*flag != FLAG_PONG) {
                if (++spin >= AMD_PAUSE_INTERVAL) {
                    __asm__ volatile ("pause" ::: "memory");
                    spin = 0;
                }
            }
        }
        out_ticks[s] = tsc_read_bare() - t0;
    }
}

__attribute__((noinline))
static void hot_pong_amd(volatile uint64_t *flag,
                          uint32_t num_iters,
                          uint32_t total_samples)
{
    for (uint32_t s = 0; s < total_samples; s++) {
        for (uint32_t i = 0; i < num_iters; i++) {
            int spin = 0;
            while (*flag != FLAG_PING) {
                if (++spin >= AMD_PAUSE_INTERVAL) {
                    __asm__ volatile ("pause" ::: "memory");
                    spin = 0;
                }
            }
            *flag = FLAG_PONG;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Worker thread                                                        */
/* ------------------------------------------------------------------ */

static void try_set_realtime(int core_id)
{
    struct sched_param sp = { .sched_priority = RT_PRIORITY };
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0
            && core_id == 0)
        fprintf(stderr,
            "  INFO: SCHED_FIFO unavailable. Run as root or:\n"
            "        sudo setcap cap_sys_nice+ep ./c2c\n");
}

static void *worker_thread(void *arg)
{
    worker_t *w = arg;
    pin_thread_to_core(w->core_id);
    try_set_realtime(w->core_id);

    for (;;) {
        sem_wait(&w->sem_start);
        if (w->assign.active < 0) break;

        if (w->assign.active) {
            assignment_t *a = &w->assign;

            /* L3 prefetch warm-up (AMD topology mode only) */
            if (a->use_prefetch)
                __builtin_prefetch((void *)a->slot, 1, 3);

            if (a->is_ping)
                a->slot->flag = FLAG_PONG;

            if (a->is_ping) {
                if (a->use_amd_pause)
                    hot_ping_amd(&a->slot->flag, a->num_iters,
                                  a->total_samples, a->samples->ticks);
                else
                    hot_ping_generic(&a->slot->flag, a->num_iters,
                                      a->total_samples, a->samples->ticks);
            } else {
                if (a->use_amd_pause)
                    hot_pong_amd(&a->slot->flag, a->num_iters,
                                  a->total_samples);
                else
                    hot_pong_generic(&a->slot->flag, a->num_iters,
                                      a->total_samples);
            }
        }
        sem_post(&w->sem_done);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Round-robin tournament scheduler                                     */
/* ------------------------------------------------------------------ */

typedef struct { int a; int b; } edge_t;

static edge_t **build_schedule(int n, int *out_rounds, int *out_ppr)
{
    int n_pad = (n % 2 == 0) ? n : n + 1;
    int nr = n_pad - 1, ppr = n_pad / 2;

    edge_t **rounds = malloc(nr * sizeof(edge_t *));
    for (int r = 0; r < nr; r++)
        rounds[r] = malloc(ppr * sizeof(edge_t));

    int *pos = malloc(n_pad * sizeof(int));
    for (int i = 0; i < n_pad; i++) pos[i] = i;

    for (int r = 0; r < nr; r++) {
        for (int p = 0; p < ppr; p++) {
            int a = pos[p], b = pos[n_pad - 1 - p];
            if (a > b) { int t = a; a = b; b = t; }
            rounds[r][p] = (edge_t){a, b};
        }
        int tmp = pos[1];
        for (int i = 1; i < n_pad - 1; i++) pos[i] = pos[i + 1];
        pos[n_pad - 1] = tmp;
    }
    free(pos);
    *out_rounds = nr;
    *out_ppr    = ppr;
    return rounds;
}

/*
 * CCD-aware schedule reordering.
 *
 * After building the round-robin schedule, scan for rounds that contain
 * two or more intra-CCD pairs. Swap those pairs with cross-CCD pairs
 * from later rounds, spreading intra-CCD pairs across distinct rounds.
 *
 * This is a greedy heuristic, not an optimal solver, but it works well
 * in practice: with n CCDs of 8 cores each, intra-CCD pairs are a small
 * fraction of all pairs, so conflicts are rare after one pass.
 */
static void reorder_for_ccd(edge_t **rounds, int nr, int ppr,
                              const int *cores, int n,
                              const topology_t *topo)
{
    if (!topo || topo->n_ccds <= 1) return;

    for (int r = 0; r < nr; r++) {
        /* Count intra-CCD pairs in this round */
        for (int p = 0; p < ppr; p++) {
            int ai = rounds[r][p].a, bi = rounds[r][p].b;
            if (ai >= n || bi >= n) continue;

            pair_class_t cls = topology_classify(topo, cores[ai], cores[bi]);
            if (cls != PAIR_INTRA_CCD) continue;

            /* Check if this CCD already appears in this round */
            int ccd = topo->cpu_ccd[cores[ai]];
            int conflict = 0;
            for (int q = 0; q < p; q++) {
                int qa = rounds[r][q].a, qb = rounds[r][q].b;
                if (qa >= n || qb >= n) continue;
                pair_class_t qcls = topology_classify(topo,
                                        cores[qa], cores[qb]);
                if (qcls == PAIR_INTRA_CCD &&
                        topo->cpu_ccd[cores[qa]] == ccd) {
                    conflict = 1; break;
                }
            }
            if (!conflict) continue;

            /* Find a later round with a cross-CCD pair in same slot */
            for (int r2 = r + 1; r2 < nr; r2++) {
                int ai2 = rounds[r2][p].a, bi2 = rounds[r2][p].b;
                if (ai2 >= n || bi2 >= n) continue;
                pair_class_t cls2 = topology_classify(topo,
                                        cores[ai2], cores[bi2]);
                if (cls2 == PAIR_CROSS_CCD || cls2 == PAIR_CROSS_NUMA) {
                    /* Swap */
                    edge_t tmp   = rounds[r][p];
                    rounds[r][p] = rounds[r2][p];
                    rounds[r2][p] = tmp;
                    break;
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* RDTSC floor                                                          */
/* ------------------------------------------------------------------ */

static uint64_t measure_rdtsc_floor(void)
{
    uint64_t mn = UINT64_MAX;
    for (int i = 0; i < 10000; i++) {
        uint64_t t0 = tsc_read_bare(), t1 = tsc_read_bare();
        uint64_t d = t1 - t0;
        if (d > 0 && d < mn) mn = d;
    }
    return mn;
}

/* ------------------------------------------------------------------ */
/* CPU frequency check                                                  */
/* ------------------------------------------------------------------ */

static void check_cpu_freq(int core_id)
{
    char pc[128], pm[128];
    snprintf(pc, sizeof(pc),
             "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", core_id);
    snprintf(pm, sizeof(pm),
             "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq", core_id);
    FILE *fc = fopen(pc, "r"), *fm = fopen(pm, "r");
    if (!fc || !fm) { if (fc) fclose(fc); if (fm) fclose(fm); return; }
    long cur = 0, max = 0;
    if (fscanf(fc, "%ld", &cur) != 1) cur = 0;
    if (fscanf(fm, "%ld", &max) != 1) max = 0;
    fclose(fc); fclose(fm);
    if (max > 0 && cur < max * 95 / 100)
        fprintf(stderr,
            "  WARN core %d: %ld / %ld kHz. "
            "Pin: cpupower frequency-set -g performance\n",
            core_id, cur, max);
}

/* ------------------------------------------------------------------ */
/* Post-processing                                                      */
/* ------------------------------------------------------------------ */

static void ticks_to_histogram(const uint64_t *ticks, uint32_t n,
                                uint32_t num_iters, histogram_t *h)
{
    hist_init(h);
    for (uint32_t i = WARMUP_SAMPLES; i < n + WARMUP_SAMPLES; i++)
        hist_add(h, tsc_ticks_to_ns(ticks[i]) / (double)num_iters / 2.0);
}

/* ------------------------------------------------------------------ */
/* Output                                                               */
/* ------------------------------------------------------------------ */

static void print_results(const bench_config_t *cfg,
                           pair_samples_t *pair_samples)
{
    int      n  = cfg->n_cores;
    uint32_t ni = cfg->num_iterations;
    uint32_t ns = cfg->num_samples;

    int col = isatty(fileno(stderr));
#define B  (col ? "\033[1m"  : "")
#define D  (col ? "\033[2m"  : "")
#define R  (col ? "\033[0m"  : "")
#define YL (col ? "\033[33m" : "")
#define RD (col ? "\033[31m" : "")

    fprintf(stderr, "  %smed%s/%sp99%s/%smin%s  (ns, one-way)\n\n",
            B, R, D, R, D, R);
    fprintf(stderr, "    %3s", "");
    for (int j = 0; j < n; j++)
        fprintf(stderr, "  %-11d", cfg->cores[j]);
    fprintf(stderr, "\n");

    /* Build means matrix for topology summary */
    double *means = calloc((size_t)n * n, sizeof(double));
    for (int i = 0; i < n * n; i++) means[i] = (double)NAN;

    double g_min = 1e18, g_max = -1.0, g_sum = 0.0;
    double g_min_non_ht = 1e18;
    int g_count = 0;
    int g_min_i = -1, g_min_j = -1, g_max_i = -1, g_max_j = -1;
    int g_mnh_i = -1, g_mnh_j = -1;
    uint64_t total_overflow = 0, total_n = 0;

    for (int i = 0; i < n; i++) {
        fprintf(stderr, "    %3d", cfg->cores[i]);
        for (int j = 0; j < n; j++) {
            if (i <= j) { fprintf(stderr, "  %-11s", ""); continue; }

            pair_samples_t *ps = &pair_samples[i * n + j];
            histogram_t h;
            ticks_to_histogram(ps->ticks, ns, ni, &h);

            total_overflow += h.overflow;
            total_n        += h.total;

            double med = hist_percentile(&h, 0.50);
            double p99 = hist_percentile(&h, 0.99);
            double mn  = hist_min(&h);

            means[i * n + j] = means[j * n + i] = med;

            fprintf(stderr, "  %s%4.1f%s/%s%4.1f%s/%s%3.1f%s ",
                    B, med, R, D, p99, R, D, mn, R);

            if (med < g_min) { g_min = med; g_min_i = i; g_min_j = j; }
            if (med > g_max) { g_max = med; g_max_i = i; g_max_j = j; }
            g_sum += med; g_count++;

            if (!are_ht_siblings(cfg->cores[i], cfg->cores[j])
                    && med < g_min_non_ht) {
                g_min_non_ht = med; g_mnh_i = i; g_mnh_j = j;
            }
        }
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "\n");

    if (total_n > 0 && total_overflow > 0) {
        double pct = 100.0 * (double)total_overflow / (double)total_n;
        fprintf(stderr,
            "  %s%.2f%% of samples > %d ns (OS preemptions).%s\n"
            "  Tip: isolcpus, SCHED_FIFO, cpupower performance governor.\n\n",
            pct > 1.0 ? RD : YL, pct, HIST_MAX_NS, R);
    }

    if (g_min_i >= 0)
        fprintf(stderr, "  Min median:        %s%.1f%s ns  cores (%d,%d)\n",
                B, g_min, R, cfg->cores[g_min_i], cfg->cores[g_min_j]);
    if (g_mnh_i >= 0)
        fprintf(stderr, "  Min non-HT median: %s%.1f%s ns  cores (%d,%d)\n",
                B, g_min_non_ht, R,
                cfg->cores[g_mnh_i], cfg->cores[g_mnh_j]);
    if (g_max_i >= 0)
        fprintf(stderr, "  Max median:        %s%.1f%s ns  cores (%d,%d)\n",
                B, g_max, R, cfg->cores[g_max_i], cfg->cores[g_max_j]);
    if (g_count > 0)
        fprintf(stderr, "  Mean of medians:   %s%.1f%s ns\n",
                B, g_sum / g_count, R);

    /* Topology-aware tiered summary */
    if (cfg->topo)
        topology_print_tiered_summary(cfg->topo, cfg->cores, n, means);

    /* CSV */
    if (cfg->csv) {
        FILE *f = fopen("output.csv", "w");
        if (!f) {
            perror("fopen output.csv");
        } else {
            for (int i = 0; i < n; i++) {
                for (int j = 0; j < n; j++) {
                    if (j) fprintf(f, ",");
                    if (i > j) {
                        pair_samples_t *ps = &pair_samples[i * n + j];
                        histogram_t h;
                        ticks_to_histogram(ps->ticks, ns, ni, &h);
                        fprintf(f, "%.2f", hist_percentile(&h, 0.50));
                    }
                }
                fprintf(f, "\n");
            }
            fclose(f);
            fprintf(stderr, "  CSV saved to output.csv\n");
        }
    }

    free(means);
#undef B
#undef D
#undef R
#undef YL
#undef RD
}

/* ------------------------------------------------------------------ */
/* Public entry point                                                   */
/* ------------------------------------------------------------------ */

void run_bench_direct(const bench_config_t *cfg)
{
    int      n  = cfg->n_cores;
    uint32_t ni = cfg->num_iterations;
    uint32_t ns = cfg->num_samples;
    uint32_t total_samples = ns + WARMUP_SAMPLES;
    int      use_amd = (cfg->topo && cfg->topo->zen_gen >= 3);

    /* ---- Diagnostics ---------------------------------------------- */
    fprintf(stderr, "  Checking CPU frequencies...\n");
    for (int i = 0; i < n; i++)
        check_cpu_freq(cfg->cores[i]);

    if (cfg->topo) {
        topology_print(cfg->topo, cfg->cores, n);
        topology_check_fclk((topology_t *)cfg->topo);
        if (use_amd)
            fprintf(stderr,
                "  AMD mode: CCD-aware scheduling, conditional PAUSE "
                "spin, L3 prefetch warm-up\n");
    }

    uint64_t rdtsc_floor = measure_rdtsc_floor();
    fprintf(stderr,
        "  RDTSC floor: %lu ticks = %.1f ns. "
        "Per-result overhead at N=%u: %.4f ns\n",
        rdtsc_floor, tsc_ticks_to_ns(rdtsc_floor),
        ni, tsc_ticks_to_ns(rdtsc_floor) / ni / 2.0);

    /* ---- Build parallel schedule ---------------------------------- */
    int nr, ppr;
    edge_t **schedule = build_schedule(n, &nr, &ppr);

    /* CCD-aware reordering when topology mode is active */
    if (use_amd)
        reorder_for_ccd(schedule, nr, ppr, cfg->cores, n, cfg->topo);

    fprintf(stderr,
        "  Parallel schedule: %d rounds × %d simultaneous pairs"
        " (%.0f× speedup)\n",
        nr, ppr, (double)ppr);

    /* ---- Allocate storage ---------------------------------------- */
    pair_samples_t *pair_samples = calloc((size_t)n * n, sizeof(pair_samples_t));
    if (!pair_samples) { perror("calloc"); exit(1); }

    for (int i = 0; i < n; i++)
        for (int j = 0; j < i; j++) {
            pair_samples[i * n + j].n     = ns;
            pair_samples[i * n + j].ticks = malloc(total_samples * sizeof(uint64_t));
            if (!pair_samples[i * n + j].ticks) { perror("malloc"); exit(1); }
        }

    slot_t *slots = aligned_alloc(CACHE_LINE_SIZE,
                                  (size_t)ppr * sizeof(slot_t));
    if (!slots) { perror("aligned_alloc"); exit(1); }

    /* ---- Thread pool ---------------------------------------------- */
    worker_t  *workers = calloc(n, sizeof(worker_t));
    pthread_t *threads = malloc(n * sizeof(pthread_t));
    if (!workers || !threads) { perror("calloc"); exit(1); }

    for (int t = 0; t < n; t++) {
        workers[t].core_id    = cfg->cores[t];
        workers[t].thread_idx = t;
        sem_init(&workers[t].sem_start, 0, 0);
        sem_init(&workers[t].sem_done,  0, 0);
        pthread_create(&threads[t], NULL, worker_thread, &workers[t]);
    }

    /* ---- Run rounds ----------------------------------------------- */
    for (int r = 0; r < nr; r++) {
        int slot_idx = 0;

        for (int p = 0; p < ppr; p++) {
            int ai = schedule[r][p].a;
            int bi = schedule[r][p].b;
            if (ai >= n || bi >= n) continue;

            /* ai = larger index = ping in lower-triangle convention */
            if (ai < bi) { int tmp = ai; ai = bi; bi = tmp; }

            slot_t         *sl = &slots[slot_idx++];
            pair_samples_t *ps = &pair_samples[ai * n + bi];
            sl->flag = FLAG_PONG;

            /* Determine if AMD optimisations apply to this pair */
            int pair_amd = use_amd;

            workers[ai].assign = (assignment_t){
                .slot          = sl,
                .samples       = ps,
                .num_iters     = ni,
                .total_samples = total_samples,
                .is_ping       = 1,
                .active        = 1,
                .use_amd_pause = pair_amd,
                .use_prefetch  = pair_amd,
            };
            workers[bi].assign = (assignment_t){
                .slot          = sl,
                .samples       = ps,
                .num_iters     = ni,
                .total_samples = total_samples,
                .is_ping       = 0,
                .active        = 1,
                .use_amd_pause = pair_amd,
                .use_prefetch  = pair_amd,
            };
        }

        /* Mark idle workers */
        for (int t = 0; t < n; t++) {
            int assigned = 0;
            for (int p = 0; p < ppr; p++) {
                int a = schedule[r][p].a, b = schedule[r][p].b;
                if (a >= n || b >= n) continue;
                if (t == a || t == b) { assigned = 1; break; }
            }
            if (!assigned) workers[t].assign.active = 0;
        }

        for (int t = 0; t < n; t++) sem_post(&workers[t].sem_start);
        for (int t = 0; t < n; t++) sem_wait(&workers[t].sem_done);
    }

    /* ---- Shut down ------------------------------------------------ */
    for (int t = 0; t < n; t++) {
        workers[t].assign.active = -1;
        sem_post(&workers[t].sem_start);
    }
    for (int t = 0; t < n; t++) {
        pthread_join(threads[t], NULL);
        sem_destroy(&workers[t].sem_start);
        sem_destroy(&workers[t].sem_done);
    }

    for (int r = 0; r < nr; r++) free(schedule[r]);
    free(schedule);

    fprintf(stderr, "\n");
    print_results(cfg, pair_samples);

    for (int i = 0; i < n; i++)
        for (int j = 0; j < i; j++)
            free(pair_samples[i * n + j].ticks);
    free(pair_samples);
    free(slots);
    free(workers);
    free(threads);
}
