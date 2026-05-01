#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Analyse experiment 6 (real-network EC benchmark) cross-host CSV.

The cross-host run lives in data/larger-shards-xhost-adept-shadow.csv.
Loopback baseline numbers come from the published EC benchmark report
(deploy/benchmark/results/ec_benchmark_full_report.html) -- specifically
the Fedora 43 aarch64 single-host numbers at 1 MB.

Output: Markdown tables suitable for inclusion in
`.claude/experiments/06-real-network-ec/report.md`.
"""
from __future__ import annotations

import csv
import os
import statistics
from collections import defaultdict

DATA = os.path.join(os.path.dirname(__file__),
                    "data/larger-shards-xhost-adept-shadow.csv")


# Loopback baseline at 1 MB, 4+2, v1, Fedora aarch64 (from
# ec_benchmark_full_report.md section 5.3).  Used for the cross-host /
# loopback ratio table.  These numbers predate the cross-host run by
# weeks; they're the published Tier-1 baseline and serve as the
# reference for the "ratios hold cross-host" check.
LOOPBACK_FEDORA_1MB_4P2 = {
    "plain":          (65,  63),
    "rs":             (110, 96),
    "mojette-sys":    (122, 93),
    "mojette-nonsys": (123, 249),
}


def load(path: str):
    rows = []
    with open(path) as fh:
        for r in csv.reader(fh):
            if not r or r[0] == "codec" or len(r) < 8:
                continue
            if r[6] != "OK":
                continue
            rows.append({
                "codec": r[0], "geom": r[1], "size": int(r[2]),
                "run": int(r[3]),
                "write_ms": int(r[4]), "read_ms": int(r[5]),
                "mode": r[7], "layout": r[8],
            })
    return rows


def median(rs, key):
    return int(statistics.median(r[key] for r in rs))


def group_v1(rows, codec, geom, size, mode):
    return [r for r in rows
            if r["codec"] == codec and r["geom"] == geom
            and r["size"] == size and r["mode"] == mode
            and r["layout"] == "v1"]


def group(rows, codec, geom, size, mode, layout):
    return [r for r in rows
            if r["codec"] == codec and r["geom"] == geom
            and r["size"] == size and r["mode"] == mode
            and r["layout"] == layout]


def main():
    rows = load(DATA)
    print(f"# Experiment 6: Real-Network EC Benchmark\n")
    print(f"Cross-host rows loaded: {len(rows)}\n")

    sizes = sorted({r["size"] for r in rows})
    layouts = sorted({r["layout"] for r in rows})
    codecs_4p2 = ["plain", "rs", "mojette-sys", "mojette-nonsys"]

    print("## Topology\n")
    print("- Client + MDS: adept (Intel N100, AVX2, Fedora 43)")
    print("- 10 DSes (id 1..10): shadow (Intel i5-9500, Fedora 43), each on")
    print("  port 2050+i, registered_with_rpcbind=false, host network")
    print("- Network: 1 GbE LAN, 0.86 ms RTT adept <-> shadow")
    print("- 5 runs per (codec, geom, size, mode, layout)")
    print(f"- Sizes: {sizes}")
    print(f"- Layouts: {layouts}\n")

    print("## Cross-host vs loopback at 1 MB / 4+2 / v1 / healthy\n")
    print("Loopback baseline = Fedora 43 aarch64 single-host bridge "
          "network (ec_benchmark_full_report.md §5.3).\n")
    print("| codec | loopback w | xhost w | w ratio | "
          "loopback r | xhost r | r ratio |")
    print("|-------|-----------:|--------:|--------:|"
          "-----------:|--------:|--------:|")
    for c in codecs_4p2:
        if c == "plain":
            lw, lr = LOOPBACK_FEDORA_1MB_4P2[c]
            xrows = group_v1(rows, "plain", "1+0", 1048576, "healthy")
        else:
            lw, lr = LOOPBACK_FEDORA_1MB_4P2[c]
            xrows = group_v1(rows, c, "4+2", 1048576, "healthy")
        if not xrows:
            print(f"| {c} | {lw} | -- | -- | {lr} | -- | -- |")
            continue
        xw, xr = median(xrows, "write_ms"), median(xrows, "read_ms")
        wr = xw / lw if lw > 0 else 0
        rr = xr / lr if lr > 0 else 0
        print(f"| {c} | {lw} | {xw} | {wr:.1f}x | "
              f"{lr} | {xr} | {rr:.1f}x |")

    print("\n## Codec ordering preservation (v1, healthy, 4+2)\n")
    print("| size | RS read | Msys read | Mnsys read | order preserved? |")
    print("|------|--------:|----------:|-----------:|------------------|")
    for size in sizes:
        rs = group_v1(rows, "rs", "4+2", size, "healthy")
        ms = group_v1(rows, "mojette-sys", "4+2", size, "healthy")
        mn = group_v1(rows, "mojette-nonsys", "4+2", size, "healthy")
        if not (rs and ms and mn):
            continue
        rsr = median(rs, "read_ms")
        msr = median(ms, "read_ms")
        mnr = median(mn, "read_ms")
        # order: Mnsys slowest, RS ~ Msys -- reproducible from loopback
        ok = mnr >= rsr and mnr >= msr
        print(f"| {size//1024} KiB | {rsr} | {msr} | {mnr} | "
              f"{'YES' if ok else 'NO'} |")

    print("\n## Reconstruction overhead at 8+2 (Msys, healthy vs degraded)\n")
    print("| size | Msys 8+2 healthy r | Msys 8+2 degraded r | overhead |")
    print("|------|-------------------:|--------------------:|---------:|")
    for size in sizes:
        h = group_v1(rows, "mojette-sys", "8+2", size, "healthy")
        d = group_v1(rows, "mojette-sys", "8+2", size, "degraded-1")
        if not (h and d):
            continue
        hr = median(h, "read_ms")
        dr = median(d, "read_ms")
        ovh = (dr - hr) / hr * 100 if hr > 0 else 0
        print(f"| {size//1024} KiB | {hr} | {dr} | {ovh:+.1f}% |")

    print("\n## v2 vs v1 write overhead (4+2, healthy)\n")
    print("| codec | size | v1 w | v2 w | overhead |")
    print("|-------|------|-----:|-----:|---------:|")
    for c in ["rs", "mojette-sys", "mojette-nonsys"]:
        for size in sizes:
            v1 = group(rows, c, "4+2", size, "healthy", "v1")
            v2 = group(rows, c, "4+2", size, "healthy", "v2")
            if not (v1 and v2):
                continue
            v1w = median(v1, "write_ms")
            v2w = median(v2, "write_ms")
            ovh = (v2w - v1w) / v1w * 100 if v1w > 0 else 0
            print(f"| {c} | {size//1024} KiB | {v1w} | {v2w} | "
                  f"{ovh:+.1f}% |")


if __name__ == "__main__":
    main()
