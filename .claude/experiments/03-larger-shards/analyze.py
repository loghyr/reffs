#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Analyse experiment 3 (larger-shard SIMD-vs-scalar matrix) CSVs.

Reads the per-host CSVs produced by `make run-larger-shards` and emits
two views for the progress-report:

  1. SIMD-vs-scalar speedup table (median read_ms ratio) across
     (codec, geom, file_size, shard_size) for each host.
  2. Cross-host x86 reproducibility check (adept N100 vs shadow i5-9500).
  3. NEON corroboration where the partial dreamer CSV has both arms.

Output goes to stdout as Markdown tables.
"""
from __future__ import annotations

import csv
import os
import statistics
import sys
from collections import defaultdict
from typing import Dict, List, Tuple

DATA_DIR = os.path.join(os.path.dirname(__file__), "data")

HOSTS = [
    ("adept",   "larger-shards-adept-final.csv",    "Intel N100",   "AVX2"),
    ("shadow",  "larger-shards-shadow-final.csv",   "Intel i5-9500", "AVX2"),
    ("dreamer", "larger-shards-dreamer-partial.csv", "Apple M4",     "NEON"),
]


def load_rows(path: str) -> List[dict]:
    """Return only OK rows, normalised into a single dict layout.

    The CSV has two record shapes:
      - 14 cols: stripe rows (no shard_size).
      - 15 cols: every other codec, with shard_size at col 14.

    Both share columns 0-9 for: codec,geom,file_size,run,write_ms,
    read_ms,verify,mode,layout,arch.  We don't need anything past
    that except shard_size and simd_mode.
    """
    rows: List[dict] = []
    with open(path, newline="") as fh:
        for raw in csv.reader(fh):
            if not raw or raw[0] == "codec":
                continue
            n = len(raw)
            if n == 14:
                # stripe row: no shard_size
                d = {
                    "codec": raw[0], "geom": raw[1],
                    "size": int(raw[2]), "run": int(raw[3]),
                    "write_ms": int(raw[4]), "read_ms": int(raw[5]),
                    "verify": raw[6], "mode": raw[7],
                    "shard_size": None,
                    "simd_mode": raw[13],
                }
            elif n == 15:
                d = {
                    "codec": raw[0], "geom": raw[1],
                    "size": int(raw[2]), "run": int(raw[3]),
                    "write_ms": int(raw[4]), "read_ms": int(raw[5]),
                    "verify": raw[6], "mode": raw[7],
                    "shard_size": int(raw[13]),
                    "simd_mode": raw[14],
                }
            else:
                continue
            if d["verify"] != "OK":
                continue
            rows.append(d)
    return rows


def median_ms(rows: List[dict], key: str) -> int:
    return int(statistics.median(r[key] for r in rows))


def speedup_table(host_rows: List[dict], host_label: str) -> str:
    """Median read_ms (simd) vs (scalar) for each (codec, geom, size, shard).

    Restricted to mode=healthy (no degraded reads complicating things).
    """
    grouped: Dict[Tuple, Dict[str, List[dict]]] = defaultdict(
        lambda: defaultdict(list))
    for r in host_rows:
        if r["mode"] != "healthy" or r["shard_size"] is None:
            continue
        # collapse simd_mode to "simd" / "scalar" / "scalar-forced" buckets.
        # Earlier runs label scalar arm as "scalar"; some shards as
        # "scalar(forced)".  Treat both as scalar.
        mode = "simd" if r["simd_mode"] == "simd" else "scalar"
        key = (r["codec"], r["geom"], r["size"], r["shard_size"])
        grouped[key][mode].append(r)

    lines = [f"\n### {host_label}\n",
             "| codec | geom | file_size | shard_size | "
             "simd read_ms | scalar read_ms | speedup |",
             "|-------|------|-----------|------------|"
             "--------------|----------------|---------|"]
    for key in sorted(grouped.keys()):
        codec, geom, size, shard = key
        bucket = grouped[key]
        if "simd" not in bucket or "scalar" not in bucket:
            continue
        sm = median_ms(bucket["simd"], "read_ms")
        sc = median_ms(bucket["scalar"], "read_ms")
        ratio = (sc / sm) if sm > 0 else float("inf")
        lines.append(
            f"| {codec} | {geom} | {size//1024} KiB | {shard//1024} KiB | "
            f"{sm} | {sc} | {ratio:.2f}x |")
    return "\n".join(lines)


def cross_host_table(host_rows_map: Dict[str, List[dict]]) -> str:
    """SIMD-mode median read_ms per (codec, geom, file_size, shard_size).

    Shows adept (N100 AVX2) vs shadow (i5-9500 AVX2) -- same ISA,
    different microarchitecture, useful as an x86 reproducibility
    sanity check.  Omits dreamer because partial.
    """
    grouped: Dict[Tuple, Dict[str, List[dict]]] = defaultdict(
        lambda: defaultdict(list))
    for host, rows in host_rows_map.items():
        for r in rows:
            if (r["mode"] != "healthy" or r["shard_size"] is None or
                    r["simd_mode"] != "simd"):
                continue
            key = (r["codec"], r["geom"], r["size"], r["shard_size"])
            grouped[key][host].append(r)

    lines = ["\n## Cross-host x86 reproducibility (SIMD arm)\n",
             "| codec | geom | file_size | shard_size | "
             "adept (N100) | shadow (i5-9500) | shadow/adept |",
             "|-------|------|-----------|------------|"
             "--------------|------------------|--------------|"]
    for key in sorted(grouped.keys()):
        codec, geom, size, shard = key
        bucket = grouped[key]
        if "adept" not in bucket or "shadow" not in bucket:
            continue
        a = median_ms(bucket["adept"], "read_ms")
        s = median_ms(bucket["shadow"], "read_ms")
        ratio = (s / a) if a > 0 else float("inf")
        lines.append(
            f"| {codec} | {geom} | {size//1024} KiB | {shard//1024} KiB | "
            f"{a} | {s} | {ratio:.2f}x |")
    return "\n".join(lines)


def shard_scaling(host_rows: List[dict], host_label: str) -> str:
    """How does SIMD vs scalar gap scale with shard_size at fixed (codec,
    geom, file_size)?  This is the headline experiment 3 result.
    """
    # pick a single file_size for clarity (1 MiB and 16 MiB)
    out = [f"\n### {host_label} — speedup vs shard size (1 MiB files)\n",
           "| codec | geom | 4 KiB | 16 KiB | 64 KiB | 256 KiB |",
           "|-------|------|-------|--------|--------|---------|"]
    for size in [1048576, 16777216]:
        if size != 1048576:
            out.append(f"\n*{size//1024//1024} MiB files*\n")
            out.append("| codec | geom | 4 KiB | 16 KiB | 64 KiB | 256 KiB |")
            out.append("|-------|------|-------|--------|--------|---------|")
        per_codec_geom: Dict[Tuple, Dict[int, Dict[str, List[dict]]]] = (
            defaultdict(lambda: defaultdict(lambda: defaultdict(list))))
        for r in host_rows:
            if (r["mode"] != "healthy" or r["shard_size"] is None or
                    r["size"] != size):
                continue
            mode = "simd" if r["simd_mode"] == "simd" else "scalar"
            per_codec_geom[(r["codec"], r["geom"])][r["shard_size"]][mode] \
                .append(r)
        for (codec, geom), shards in sorted(per_codec_geom.items()):
            cells = [codec, geom]
            for shard in [4096, 16384, 65536, 262144]:
                bucket = shards.get(shard, {})
                if "simd" in bucket and "scalar" in bucket:
                    sm = median_ms(bucket["simd"], "read_ms")
                    sc = median_ms(bucket["scalar"], "read_ms")
                    ratio = (sc / sm) if sm > 0 else 0
                    cells.append(f"{ratio:.2f}x")
                else:
                    cells.append("-")
            out.append("| " + " | ".join(cells) + " |")
    return "\n".join(out)


def main():
    host_rows = {h: load_rows(os.path.join(DATA_DIR, p))
                 for h, p, _, _ in HOSTS}
    print("# Experiment 3: Larger-Shard SIMD-vs-Scalar Matrix")
    print("\nGenerated from CSVs at "
          "`.claude/experiments/03-larger-shards/data/`.")
    print("\n## Run summary\n")
    print("| host | CPU | ISA | rows | status |")
    print("|------|-----|-----|------|--------|")
    for h, p, cpu, isa in HOSTS:
        n = len(host_rows[h])
        status = "complete" if n > 1500 else f"partial ({n} rows)"
        print(f"| {h} | {cpu} | {isa} | {n} | {status} |")

    print("\n## Read-side speedup (SIMD vs scalar)\n")
    print("Median `read_ms` from healthy reads, grouped by "
          "(codec, geometry, file_size, shard_size).  Speedup = "
          "scalar / simd (higher = SIMD wins by more).\n")
    for h, _, cpu, isa in HOSTS:
        if len(host_rows[h]) > 1500:
            print(speedup_table(host_rows[h], f"{h} ({cpu}, {isa})"))

    print(cross_host_table(host_rows))

    print("\n## SIMD-vs-scalar gap as shard size grows\n")
    print("Each cell is (scalar median read_ms) / (SIMD median read_ms) "
          "for the matching tuple.  >1.0 means SIMD is faster.\n")
    for h, _, cpu, isa in HOSTS:
        if len(host_rows[h]) > 1500:
            print(shard_scaling(host_rows[h], f"{h} ({cpu}, {isa})"))


if __name__ == "__main__":
    main()
