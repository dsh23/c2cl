# c2cl — Core-to-Core Latency Measurement Tool

Measures the latency of cache line transfers between CPU cores on Linux x86-64.
This is the fundamental unit of inter-thread communication cost: the time it takes
for a value written by one thread to become visible to a spinning thread on another core.

## Example output

```
CPU: 12th Gen Intel(R) Core(TM) i9-12900
Num cores: 24
TSC frequency: 2.419 GHz  (1 tick = 0.413 ns)

1) CAS latency on a single shared cache line

           0       1       2       3       4       5   ...
      0
      1    9±0
      2   29±0    29±0
      3   31±0    31±0     9±0
      4   31±0    31±0    23±0    23±0
      ...

    Min  latency:   8.7±0.0 ns  cores: (9,8)
    Min  non-HT:   23.1±0.0 ns  cores: (4,2)
    Max  latency:  59.9±0.1 ns  cores: (19,16)
    Mean latency:  34.9 ns
```

## Benchmarks

| # | Name | Measures | Recommended for |
|---|------|----------|-----------------|
| 1 | CAS | `LOCK CMPXCHG` round-trip | Spinlock acquisition latency |
| 2 | Read/Write | Plain store/load ping-pong | Producer-consumer flag latency |
| 3 | Message passing | One-way RDTSC-stamped transfer | Asymmetric / directional latency |
| **4** | **Direct store/load** | **Bare cache line transfer** | **General purpose — use this** |

Benchmark 4 is the most accurate: plain `MOV` store/load with no `LOCK` prefix,
amortised timing with negligible RDTSC overhead, and full latency distribution
(min / median / p99 / max) per core pair.

## Requirements

- Linux x86-64
- GCC ≥ 7 or Clang ≥ 6
- CPU with invariant TSC (all Intel/AMD CPUs since ~2008; benchmark 3 and 4 check this at startup)
- `libpthread`, `libm`, `librt` (present on all standard Linux distributions)

## Building

```bash
git clone https://github.com/yourname/c2cl.git
make
```

Using Clang:
```bash
make CC=clang
```

Install to `/usr/local/bin`:
```bash
sudo make install
```

Install to a custom prefix:
```bash
make install PREFIX=~/.local
```

## Usage

```
./c2cl [OPTIONS] [num_iterations [num_samples]]

Options:
  -b, --bench LIST   Benchmarks to run, comma-separated (default: 1)
                       1: CAS on a single shared cache line
                       2: Single-writer single-reader on two cache lines
                       3: One-way message passing via RDTSC timestamps
                       4: Direct store/load — recommended
  -c, --cores LIST   Core IDs: individual, ranges, or mixed (default: all)
  --csv              Save latency matrix to output.csv
  --topology         AMD Zen 4+ topology-aware mode (benchmark 4 only)
  -h, --help         Show full help with examples
```

### Common invocations

```bash
# Run benchmark 1 on all cores (default)
./c2cl

# Recommended: benchmark 4 with full distribution stats
./c2cl -b 4

# Run both benchmarks for comparison
./c2cl -b 1,4

# Single socket on a 2-socket system (cores 0–127)
./c2cl -b 1 -c 0-127

# Interleaved HT layout (socket 0 = logical CPUs 0–63 and 128–191)
./c2cl -b 1 -c 0-63,128-191

# AMD Zen 4: topology-aware mode (CCD scheduling, PAUSE spin, tiered output)
./c2cl -b 4 --topology

# Save CSV and plot heatmap
./c2cl -b 4 --csv
python3 tools/c2c_graph.py

# Fast run: fewer samples
./c2cl -b 4 1000 50

# High-precision run on 16 cores
./c2 -b 4 -c 0-15 5000 1000
```

## System configuration for accurate results

Results are most accurate when the CPU is running at its maximum frequency.
Before benchmarking:

```bash
# Set performance governor
sudo cpupower frequency-set -g performance

# Disable turbo boost for consistent frequency across all core pairs
# Intel:
echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo
# AMD:
echo 0 | sudo tee /sys/devices/system/cpu/cpufreq/boost

# Enable real-time scheduling to reduce OS jitter (optional but recommended)
sudo setcap cap_sys_nice+ep ./c2c
```

Restore afterwards:
```bash
sudo cpupower frequency-set -g powersave
# Intel:
echo 0 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo
# AMD:
echo 1 | sudo tee /sys/devices/system/cpu/cpufreq/boost
```

> **Why this matters**: at idle, CPUs typically park at 800 MHz–1.2 GHz.
> Cache coherence operations complete in fewer TSC ticks at low frequency,
> but the TSC itself runs at the fixed nominal frequency. Without the
> performance governor, results are systematically higher and inconsistent
> across pairs measured at different power states.

## AMD Zen 4+ topology mode

Pass `--topology` with benchmark 4 on an AMD Zen 4 (or later) system for:

- **CCD-aware scheduling**: intra-CCD pairs are isolated to rounds containing
  only cross-CCD partners, preventing shared L3 contention from inflating
  intra-CCD measurements
- **Conditional PAUSE spin**: reduces coherency bus pressure without
  adding measurable overhead to the hot loop
- **L3 prefetch warm-up**: pulls the measurement slot into local L3
  before the warmup samples begin
- **Tiered output**: latency grouped by topology tier

```
  Latency by tier:
    HT-sibling       min  5.1 ns  cores (0,1)    [CCD 0]
    intra-CCD        min 11.2 ns  cores (2,8)    [CCD 0]
    cross-CCD        min 33.4 ns  cores (0,8)    [CCD 0 → CCD 1]
    cross-NUMA       min 89.1 ns  cores (0,64)   [CCD 0 → CCD 8]
```

The tool detects Zen generation automatically from CPUID and will warn
if `--topology` is used on a non-AMD or pre-Zen 3 CPU.


## Performance at scale

The parallel scheduler uses a **round-robin tournament** algorithm to run
`n/2` independent core pairs simultaneously. All `n(n-1)/2` pairs are
covered in `n-1` rounds with zero idle cores.

| Cores | Pairs | Rounds | Simultaneous pairs | Est. runtime (bench 4) |
|-------|-------|--------|--------------------|------------------------|
| 24    | 276   | 23     | 12                 | ~4 s                   |
| 64    | 2,016 | 63     | 32                 | ~8 s                   |
| 128   | 8,128 | 127    | 64                 | ~15 s                  |
| 256   | 32,640| 255    | 128                | ~30 s                  |

Estimates assume 300 samples × 1000 iterations × ~40 ns average round-trip.

## Source layout

```
c2c/
├── README.md
├── LICENSE
├── Makefile
├── DOCUMENTATION.md        Full technical documentation
├── src/
│   ├── main.c              CLI, core enumeration, benchmark dispatch
│   ├── c2c.h               Shared types and bench_config_t
│   ├── tsc.h / tsc.c       TSC calibration and read
│   ├── utils.h / utils.c   CPU affinity, CPUID, HT detection, delay
│   ├── stats.h / stats.c   Mean/stddev, results table, CSV output
│   ├── histogram.h         Fixed-width 1 ns latency histogram
│   ├── topology.h / .c     AMD CCD/NUMA detection and classification
│   ├── bench_cas.c         Benchmark 1: CAS on shared cache line
│   ├── bench_rw.c          Benchmark 2: read/write ping-pong
│   ├── bench_msg.c         Benchmark 3: one-way message passing
│   └── bench_direct.c      Benchmark 4: direct store/load (recommended)
└── tools/
    └── c2c_graph.py        Heatmap plotter (requires matplotlib, pandas)
```

## Documentation

[DOCUMENTATION.md](DOCUMENTATION.md) contains full technical documentation
including:

- Background theory: MESI cache coherence, NUMA, HT, AMD CCD architecture, TSC
- Detailed description of each benchmark and what it measures
- Key code sections: TSC calibration, amortised timing, hot loop design,
  parallel scheduling, AMD topology mode, memory ordering
- Measurement accuracy: sources of error and mitigation
- Interpreting results: typical latency values by platform and topology tier
- Complete verified reference list (17 references)

## Relation to the original Rust implementation

This tool is a C port of [nviennot/core-to-core-latency](https://github.com/nviennot/core-to-core-latency)
with the following differences:

| Feature | Rust original | This C port |
|---------|---------------|-------------|
| Benchmarks | 3 | 4 (adds direct store/load) |
| Timing | Amortised (quanta crate) | Amortised bare RDTSC |
| Scheduling | Sequential | Parallel round-robin tournament |
| Statistics | Mean ± stddev | Min / median / p99 / max / overflow |
| AMD topology | No | Yes (Zen 3+, `--topology` flag) |
| HT detection | Via core_affinity | Via thread_siblings_list sysfs |
| Non-HT minimum | Not reported | Reported in summary |
| CSV output | stdout | output.csv |
| Platform | Linux, macOS | Linux x86-64 |

## License

MIT — see [LICENSE](LICENSE).

## Acknowledgements

Based on a reference implementation is rust [core-to-core-latency](https://github.com/nviennot/core-to-core-latency)
by Nicolas Viennot.
