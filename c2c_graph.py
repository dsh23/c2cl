#! /usr/bin/env python3
import matplotlib
import pandas as pd
import numpy as np
from matplotlib import pyplot as plt


def load_data(filename):
    m = np.array(pd.read_csv(filename, header=None))
    return np.tril(m) + np.tril(m).transpose()


def find_non_ht_min(m):
    """
    Return (min_value, row, col) for the lowest-latency pair that is NOT
    an HT sibling.

    HT sibling pairs have latency well below any inter-core transfer — on
    typical x86 they cluster tightly around the global minimum, separated
    from the next tier by a clear gap.  We identify them by finding all
    entries within a 2× multiple of the global minimum; anything above
    that threshold is treated as a genuine inter-core transfer.

    The 2× multiplier is conservative: HT siblings are typically 4–10 ns
    while the nearest inter-core pairs are 20–35 ns, so there is always a
    large gap.  Using a data-driven threshold avoids hard-coding any
    machine-specific value.
    """
    # Work on the lower triangle only (matrix is symmetric; diagonal is 0)
    flat = m[np.tril_indices_from(m, k=-1)]
    flat = flat[~np.isnan(flat)]
    if len(flat) == 0:
        return (np.nan, -1, -1)

    global_min = np.nanmin(flat)
    # HT threshold: anything <= 2x global_min is considered an HT sibling
    ht_threshold = global_min * 2.0

    non_ht_min = np.inf
    best_i, best_j = -1, -1

    n = m.shape[0]
    for i in range(n):
        for j in range(i):          # lower triangle, i > j
            v = m[i, j]
            if np.isnan(v):
                continue
            if v <= ht_threshold:   # skip HT siblings
                continue
            if v < non_ht_min:
                non_ht_min = v
                best_i, best_j = i, j

    return (non_ht_min, best_i, best_j)


def show_heatmap(m, title=None, subtitle=None, vmin=None, vmax=None,
                 yticks=True, figsize=None):
    vmin = np.nanmin(m) if vmin is None else vmin
    vmax = np.nanmax(m) if vmax is None else vmax
    black_at = (vmin + 3 * vmax) / 4
    subtitle = "Core-to-Core Latency" if subtitle is None else subtitle

    non_ht_min, nh_i, nh_j = find_non_ht_min(m)

    isnan = np.isnan(m)
    plt.rcParams['xtick.bottom'] = plt.rcParams['xtick.labelbottom'] = False
    plt.rcParams['xtick.top']    = plt.rcParams['xtick.labeltop']    = True
    figsize = (np.array(m.shape) * 0.3 + np.array([6, 1])
               if figsize is None else figsize)
    fig, ax = plt.subplots(figsize=figsize, dpi=130)

    fig.patch.set_facecolor('w')

    plt.imshow(np.full_like(m, 0.7), vmin=0, vmax=1, cmap='gray')
    plt.imshow(m, cmap=plt.get_cmap('viridis'), vmin=vmin, vmax=vmax)

    # Highlight the lowest non-HT pair with a white border
    if nh_i >= 0:
        for (pi, pj) in [(nh_i, nh_j), (nh_j, nh_i)]:
            rect = matplotlib.patches.Rectangle(
                (pj - 0.5, pi - 0.5), 1, 1,
                linewidth=1.5, edgecolor='white', facecolor='none'
            )
            ax.add_patch(rect)

    fontsize = 9 if vmax >= 100 else 10
    for (i, j) in np.ndindex(m.shape):
        t = ("" if isnan[i, j]
             else f"{m[i,j]:.1f}" if vmax < 10.0
             else f"{m[i,j]:.0f}")
        c = "w" if m[i, j] < black_at else "k"
        plt.text(j, i, t, ha="center", va="center",
                 color=c, fontsize=fontsize)

    plt.xlabel("CPU Core")
    plt.ylabel("CPU Core")
    plt.xticks(np.arange(m.shape[1]),
               labels=[f"{i}" for i in range(m.shape[1])], fontsize=9)
    if yticks:
        plt.yticks(np.arange(m.shape[0]),
                   labels=[f"{i}" for i in range(m.shape[0])], fontsize=9)
    else:
        plt.yticks([])

    plt.tight_layout()

    # Build stats line
    non_ht_str = (f"  Min(non-HT)={non_ht_min:.1f}ns"
                  if not np.isinf(non_ht_min) else "")
    plt.title(
        f"{title}\n"
        f"{subtitle}\n"
        f"Min={vmin:.1f}ns  Median={np.nanmedian(m):.1f}ns"
        f"  Max={vmax:.1f}ns{non_ht_str}",
        fontsize=11, linespacing=1.5
    )

    plt.savefig('c2c_latency.png', bbox_inches='tight', dpi=1000)
    print(f"Saved c2c_latency.png")
    if not np.isinf(non_ht_min):
        print(f"Min non-HT latency: {non_ht_min:.1f} ns  "
              f"(cores {nh_j} ↔ {nh_i})")


# ── entry point ────────────────────────────────────────────────────────
cpu   = "Intel i9-12900"
fname = "coutput.csv"
m     = load_data(fname)
show_heatmap(m, title=cpu)
