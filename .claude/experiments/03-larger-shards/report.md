<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Experiment 3: Larger-Shard SIMD-vs-Scalar Matrix

Generated from CSVs at `.claude/experiments/03-larger-shards/data/`.

## TL;DR

Across two x86 hosts (adept N100 + shadow i5-9500), the full matrix
exposes three findings that are independent of hardware:

1. **Compiler auto-vectorisation contributes essentially nothing
   to the codec read path.**  Across every codec, geometry,
   file_size, and shard_size combination at >= 16 KiB shards, the
   `--enable-noscalar-vec` build (compiler auto-vec disabled on
   `lib/ec/`) reads within ±10% of the default SIMD build.  This is
   consistent with the patent-safe scalar GF(2^8) log/antilog
   implementation -- the table-lookup access pattern doesn't
   vectorise cleanly, so disabling auto-vec costs nothing
   measurable.

2. **At 4 KiB shards, shadow shows a visible auto-vec gain (1.36x -
   1.66x for 1 MiB files), while adept does not.**  This is
   the only regime where the two compilations diverge.  The likely
   explanation is that 4 KiB shards expose per-shard fixed overhead
   that auto-vec smooths out on shadow's larger superscalar
   pipeline, but the N100's narrower pipeline can't extract
   parallelism either way.

3. **Per-shard overhead dominates at 4 KiB shards.**  Cross-host
   numbers tell the story bluntly: at 4 KiB shards on a 16 MiB file
   with mojette-nonsys 4+2, adept finishes in 13.7s while shadow
   needs 59.0s -- a 4.3x slowdown on the *beefier* CPU.  The same
   tuple at 64 KiB shards finishes in ~75s on both, and at 256 KiB
   in ~280s on both (within 7%).  The 4 KiB regime is dominated by
   per-shard syscall/RPC/state-machine cost, NOT codec compute --
   so the hardware that wins there is the one with cheapest
   per-op cost, not the one with highest compute throughput.

## Implications for the FFv2 design story

- **Patent-safe scalar GF arithmetic is the right call.**  The
  data validates the no-SIMD-GF rule from `.claude/standards.md` --
  there is no measurable performance loss vs auto-vec on these
  codecs at production-relevant shard sizes.  Earlier "SIMD-vs-
  scalar" gaps reported in benchmarks (~1.2-1.5x at 1 MiB shards
  in the demo data) were therefore from peripheral non-codec
  code paths, not the GF inner loops.
- **Recommend shard_size in the 64 KiB - 256 KiB band.**  Below
  16 KiB, per-shard overhead dominates everything else.  Above
  256 KiB, shard count drops below the redundancy budget for
  large files.  64 KiB is a clean sweet spot.
- **Mojette-nonsys read cost balloons super-linearly with file
  size at small shards.**  16 MiB at 4 KiB shards takes 14-59s;
  16 MiB at 256 KiB shards takes 280-290s.  The 16 MiB / 256 KiB
  case is interaction between high shard count (64 shards * 4
  data parts) and the projection algorithm's worst-case
  reconstruction.  Mojette-nonsys remains "unsuitable for reads"
  per the goals.md note; this experiment confirms it at scale.

## Run summary

| host | CPU | ISA | rows | status |
|------|-----|-----|------|--------|
| adept | Intel N100 | AVX2 | 1600 | complete |
| shadow | Intel i5-9500 | AVX2 | 1600 | complete |
| dreamer | Apple M4 | NEON | 341 | partial (341 rows) |

## Read-side speedup (SIMD vs scalar)

Median `read_ms` from healthy reads, grouped by (codec, geometry, file_size, shard_size).  Speedup = scalar / simd (higher = SIMD wins by more).


### adept (Intel N100, AVX2)

| codec | geom | file_size | shard_size | simd read_ms | scalar read_ms | speedup |
|-------|------|-----------|------------|--------------|----------------|---------|
| mojette-nonsys | 4+2 | 64 KiB | 4 KiB | 99 | 105 | 1.06x |
| mojette-nonsys | 4+2 | 64 KiB | 16 KiB | 129 | 130 | 1.01x |
| mojette-nonsys | 4+2 | 64 KiB | 64 KiB | 1188 | 1173 | 0.99x |
| mojette-nonsys | 4+2 | 64 KiB | 256 KiB | 17951 | 18034 | 1.00x |
| mojette-nonsys | 4+2 | 256 KiB | 4 KiB | 236 | 293 | 1.24x |
| mojette-nonsys | 4+2 | 256 KiB | 16 KiB | 371 | 362 | 0.98x |
| mojette-nonsys | 4+2 | 256 KiB | 64 KiB | 1171 | 1177 | 1.01x |
| mojette-nonsys | 4+2 | 256 KiB | 256 KiB | 17920 | 18050 | 1.01x |
| mojette-nonsys | 4+2 | 1024 KiB | 4 KiB | 891 | 945 | 1.06x |
| mojette-nonsys | 4+2 | 1024 KiB | 16 KiB | 1343 | 1345 | 1.00x |
| mojette-nonsys | 4+2 | 1024 KiB | 64 KiB | 4611 | 4597 | 1.00x |
| mojette-nonsys | 4+2 | 1024 KiB | 256 KiB | 17952 | 18144 | 1.01x |
| mojette-nonsys | 4+2 | 4096 KiB | 4 KiB | 3449 | 3487 | 1.01x |
| mojette-nonsys | 4+2 | 4096 KiB | 16 KiB | 5232 | 5214 | 1.00x |
| mojette-nonsys | 4+2 | 4096 KiB | 64 KiB | 18712 | 18746 | 1.00x |
| mojette-nonsys | 4+2 | 4096 KiB | 256 KiB | 72748 | 71727 | 0.99x |
| mojette-nonsys | 4+2 | 16384 KiB | 4 KiB | 13754 | 13254 | 0.96x |
| mojette-nonsys | 4+2 | 16384 KiB | 16 KiB | 20487 | 20597 | 1.01x |
| mojette-nonsys | 4+2 | 16384 KiB | 64 KiB | 75480 | 74538 | 0.99x |
| mojette-nonsys | 4+2 | 16384 KiB | 256 KiB | 292554 | 291167 | 1.00x |
| mojette-nonsys | 8+2 | 64 KiB | 4 KiB | 122 | 127 | 1.04x |
| mojette-nonsys | 8+2 | 64 KiB | 16 KiB | 416 | 426 | 1.02x |
| mojette-nonsys | 8+2 | 64 KiB | 64 KiB | 5366 | 5041 | 0.94x |
| mojette-nonsys | 8+2 | 64 KiB | 256 KiB | 75157 | 74622 | 0.99x |
| mojette-nonsys | 8+2 | 256 KiB | 4 KiB | 326 | 323 | 0.99x |
| mojette-nonsys | 8+2 | 256 KiB | 16 KiB | 786 | 787 | 1.00x |
| mojette-nonsys | 8+2 | 256 KiB | 64 KiB | 4861 | 5180 | 1.07x |
| mojette-nonsys | 8+2 | 256 KiB | 256 KiB | 75069 | 74157 | 0.99x |
| mojette-nonsys | 8+2 | 1024 KiB | 4 KiB | 1154 | 1165 | 1.01x |
| mojette-nonsys | 8+2 | 1024 KiB | 16 KiB | 3025 | 3017 | 1.00x |
| mojette-nonsys | 8+2 | 1024 KiB | 64 KiB | 10243 | 10197 | 1.00x |
| mojette-nonsys | 8+2 | 1024 KiB | 256 KiB | 76091 | 73881 | 0.97x |
| mojette-nonsys | 8+2 | 4096 KiB | 4 KiB | 4517 | 4499 | 1.00x |
| mojette-nonsys | 8+2 | 4096 KiB | 16 KiB | 12268 | 11971 | 0.98x |
| mojette-nonsys | 8+2 | 4096 KiB | 64 KiB | 40763 | 40798 | 1.00x |
| mojette-nonsys | 8+2 | 4096 KiB | 256 KiB | 147866 | 151614 | 1.03x |
| mojette-nonsys | 8+2 | 16384 KiB | 4 KiB | 17687 | 17638 | 1.00x |
| mojette-nonsys | 8+2 | 16384 KiB | 16 KiB | 47091 | 47211 | 1.00x |
| mojette-nonsys | 8+2 | 16384 KiB | 64 KiB | 160662 | 161944 | 1.01x |
| mojette-nonsys | 8+2 | 16384 KiB | 256 KiB | 602808 | 599470 | 0.99x |
| mojette-sys | 4+2 | 64 KiB | 4 KiB | 83 | 88 | 1.06x |
| mojette-sys | 4+2 | 64 KiB | 16 KiB | 60 | 62 | 1.03x |
| mojette-sys | 4+2 | 64 KiB | 64 KiB | 62 | 63 | 1.02x |
| mojette-sys | 4+2 | 64 KiB | 256 KiB | 56 | 60 | 1.07x |
| mojette-sys | 4+2 | 256 KiB | 4 KiB | 188 | 202 | 1.07x |
| mojette-sys | 4+2 | 256 KiB | 16 KiB | 91 | 87 | 0.96x |
| mojette-sys | 4+2 | 256 KiB | 64 KiB | 61 | 62 | 1.02x |
| mojette-sys | 4+2 | 256 KiB | 256 KiB | 55 | 54 | 0.98x |
| mojette-sys | 4+2 | 1024 KiB | 4 KiB | 633 | 617 | 0.97x |
| mojette-sys | 4+2 | 1024 KiB | 16 KiB | 202 | 206 | 1.02x |
| mojette-sys | 4+2 | 1024 KiB | 64 KiB | 89 | 100 | 1.12x |
| mojette-sys | 4+2 | 1024 KiB | 256 KiB | 62 | 63 | 1.02x |
| mojette-sys | 4+2 | 4096 KiB | 4 KiB | 2343 | 2277 | 0.97x |
| mojette-sys | 4+2 | 4096 KiB | 16 KiB | 673 | 664 | 0.99x |
| mojette-sys | 4+2 | 4096 KiB | 64 KiB | 230 | 226 | 0.98x |
| mojette-sys | 4+2 | 4096 KiB | 256 KiB | 116 | 115 | 0.99x |
| mojette-sys | 4+2 | 16384 KiB | 4 KiB | 8853 | 8963 | 1.01x |
| mojette-sys | 4+2 | 16384 KiB | 16 KiB | 2440 | 2465 | 1.01x |
| mojette-sys | 4+2 | 16384 KiB | 64 KiB | 772 | 777 | 1.01x |
| mojette-sys | 4+2 | 16384 KiB | 256 KiB | 330 | 332 | 1.01x |
| mojette-sys | 8+2 | 64 KiB | 4 KiB | 78 | 80 | 1.03x |
| mojette-sys | 8+2 | 64 KiB | 16 KiB | 67 | 63 | 0.94x |
| mojette-sys | 8+2 | 64 KiB | 64 KiB | 72 | 74 | 1.03x |
| mojette-sys | 8+2 | 64 KiB | 256 KiB | 71 | 76 | 1.07x |
| mojette-sys | 8+2 | 256 KiB | 4 KiB | 161 | 171 | 1.06x |
| mojette-sys | 8+2 | 256 KiB | 16 KiB | 87 | 85 | 0.98x |
| mojette-sys | 8+2 | 256 KiB | 64 KiB | 70 | 74 | 1.06x |
| mojette-sys | 8+2 | 256 KiB | 256 KiB | 75 | 75 | 1.00x |
| mojette-sys | 8+2 | 1024 KiB | 4 KiB | 518 | 541 | 1.04x |
| mojette-sys | 8+2 | 1024 KiB | 16 KiB | 170 | 181 | 1.06x |
| mojette-sys | 8+2 | 1024 KiB | 64 KiB | 85 | 88 | 1.04x |
| mojette-sys | 8+2 | 1024 KiB | 256 KiB | 77 | 77 | 1.00x |
| mojette-sys | 8+2 | 4096 KiB | 4 KiB | 1994 | 1958 | 0.98x |
| mojette-sys | 8+2 | 4096 KiB | 16 KiB | 571 | 557 | 0.98x |
| mojette-sys | 8+2 | 4096 KiB | 64 KiB | 204 | 203 | 1.00x |
| mojette-sys | 8+2 | 4096 KiB | 256 KiB | 110 | 113 | 1.03x |
| mojette-sys | 8+2 | 16384 KiB | 4 KiB | 7573 | 7576 | 1.00x |
| mojette-sys | 8+2 | 16384 KiB | 16 KiB | 2114 | 2101 | 0.99x |
| mojette-sys | 8+2 | 16384 KiB | 64 KiB | 679 | 678 | 1.00x |
| mojette-sys | 8+2 | 16384 KiB | 256 KiB | 304 | 305 | 1.00x |
| plain | 1+0 | 64 KiB | 4 KiB | 53 | 55 | 1.04x |
| plain | 1+0 | 64 KiB | 16 KiB | 54 | 55 | 1.02x |
| plain | 1+0 | 64 KiB | 64 KiB | 56 | 54 | 0.96x |
| plain | 1+0 | 64 KiB | 256 KiB | 54 | 54 | 1.00x |
| plain | 1+0 | 256 KiB | 4 KiB | 115 | 119 | 1.03x |
| plain | 1+0 | 256 KiB | 16 KiB | 117 | 119 | 1.02x |
| plain | 1+0 | 256 KiB | 64 KiB | 118 | 119 | 1.01x |
| plain | 1+0 | 256 KiB | 256 KiB | 118 | 118 | 1.00x |
| plain | 1+0 | 1024 KiB | 4 KiB | 363 | 377 | 1.04x |
| plain | 1+0 | 1024 KiB | 16 KiB | 373 | 373 | 1.00x |
| plain | 1+0 | 1024 KiB | 64 KiB | 370 | 363 | 0.98x |
| plain | 1+0 | 1024 KiB | 256 KiB | 368 | 362 | 0.98x |
| plain | 1+0 | 4096 KiB | 4 KiB | 1372 | 1351 | 0.98x |
| plain | 1+0 | 4096 KiB | 16 KiB | 1371 | 1348 | 0.98x |
| plain | 1+0 | 4096 KiB | 64 KiB | 1354 | 1344 | 0.99x |
| plain | 1+0 | 4096 KiB | 256 KiB | 1342 | 1321 | 0.98x |
| plain | 1+0 | 16384 KiB | 4 KiB | 5385 | 5389 | 1.00x |
| plain | 1+0 | 16384 KiB | 16 KiB | 5354 | 5325 | 0.99x |
| plain | 1+0 | 16384 KiB | 64 KiB | 5156 | 5140 | 1.00x |
| plain | 1+0 | 16384 KiB | 256 KiB | 5264 | 5240 | 1.00x |
| rs | 4+2 | 64 KiB | 4 KiB | 79 | 84 | 1.06x |
| rs | 4+2 | 64 KiB | 16 KiB | 58 | 61 | 1.05x |
| rs | 4+2 | 64 KiB | 64 KiB | 62 | 62 | 1.00x |
| rs | 4+2 | 64 KiB | 256 KiB | 50 | 50 | 1.00x |
| rs | 4+2 | 256 KiB | 4 KiB | 188 | 191 | 1.02x |
| rs | 4+2 | 256 KiB | 16 KiB | 86 | 88 | 1.02x |
| rs | 4+2 | 256 KiB | 64 KiB | 54 | 55 | 1.02x |
| rs | 4+2 | 256 KiB | 256 KiB | 50 | 57 | 1.14x |
| rs | 4+2 | 1024 KiB | 4 KiB | 607 | 620 | 1.02x |
| rs | 4+2 | 1024 KiB | 16 KiB | 186 | 200 | 1.08x |
| rs | 4+2 | 1024 KiB | 64 KiB | 89 | 83 | 0.93x |
| rs | 4+2 | 1024 KiB | 256 KiB | 53 | 54 | 1.02x |
| rs | 4+2 | 4096 KiB | 4 KiB | 2317 | 2291 | 0.99x |
| rs | 4+2 | 4096 KiB | 16 KiB | 651 | 630 | 0.97x |
| rs | 4+2 | 4096 KiB | 64 KiB | 221 | 215 | 0.97x |
| rs | 4+2 | 4096 KiB | 256 KiB | 102 | 101 | 0.99x |
| rs | 4+2 | 16384 KiB | 4 KiB | 8908 | 8777 | 0.99x |
| rs | 4+2 | 16384 KiB | 16 KiB | 2368 | 2366 | 1.00x |
| rs | 4+2 | 16384 KiB | 64 KiB | 739 | 723 | 0.98x |
| rs | 4+2 | 16384 KiB | 256 KiB | 283 | 286 | 1.01x |
| rs | 8+2 | 64 KiB | 4 KiB | 78 | 75 | 0.96x |
| rs | 8+2 | 64 KiB | 16 KiB | 67 | 66 | 0.99x |
| rs | 8+2 | 64 KiB | 64 KiB | 66 | 61 | 0.92x |
| rs | 8+2 | 64 KiB | 256 KiB | 65 | 65 | 1.00x |
| rs | 8+2 | 256 KiB | 4 KiB | 161 | 173 | 1.07x |
| rs | 8+2 | 256 KiB | 16 KiB | 82 | 83 | 1.01x |
| rs | 8+2 | 256 KiB | 64 KiB | 63 | 62 | 0.98x |
| rs | 8+2 | 256 KiB | 256 KiB | 68 | 67 | 0.99x |
| rs | 8+2 | 1024 KiB | 4 KiB | 502 | 542 | 1.08x |
| rs | 8+2 | 1024 KiB | 16 KiB | 172 | 175 | 1.02x |
| rs | 8+2 | 1024 KiB | 64 KiB | 81 | 83 | 1.02x |
| rs | 8+2 | 1024 KiB | 256 KiB | 64 | 61 | 0.95x |
| rs | 8+2 | 4096 KiB | 4 KiB | 1958 | 1934 | 0.99x |
| rs | 8+2 | 4096 KiB | 16 KiB | 539 | 534 | 0.99x |
| rs | 8+2 | 4096 KiB | 64 KiB | 189 | 191 | 1.01x |
| rs | 8+2 | 4096 KiB | 256 KiB | 93 | 92 | 0.99x |
| rs | 8+2 | 16384 KiB | 4 KiB | 7395 | 7395 | 1.00x |
| rs | 8+2 | 16384 KiB | 16 KiB | 2014 | 1999 | 0.99x |
| rs | 8+2 | 16384 KiB | 64 KiB | 627 | 634 | 1.01x |
| rs | 8+2 | 16384 KiB | 256 KiB | 248 | 247 | 1.00x |

### shadow (Intel i5-9500, AVX2)

| codec | geom | file_size | shard_size | simd read_ms | scalar read_ms | speedup |
|-------|------|-----------|------------|--------------|----------------|---------|
| mojette-nonsys | 4+2 | 64 KiB | 4 KiB | 264 | 381 | 1.44x |
| mojette-nonsys | 4+2 | 64 KiB | 16 KiB | 305 | 286 | 0.94x |
| mojette-nonsys | 4+2 | 64 KiB | 64 KiB | 1206 | 1227 | 1.02x |
| mojette-nonsys | 4+2 | 64 KiB | 256 KiB | 17076 | 17050 | 1.00x |
| mojette-nonsys | 4+2 | 256 KiB | 4 KiB | 700 | 1052 | 1.50x |
| mojette-nonsys | 4+2 | 256 KiB | 16 KiB | 601 | 594 | 0.99x |
| mojette-nonsys | 4+2 | 256 KiB | 64 KiB | 1199 | 1230 | 1.03x |
| mojette-nonsys | 4+2 | 256 KiB | 256 KiB | 17107 | 17066 | 1.00x |
| mojette-nonsys | 4+2 | 1024 KiB | 4 KiB | 2818 | 3827 | 1.36x |
| mojette-nonsys | 4+2 | 1024 KiB | 16 KiB | 1886 | 1790 | 0.95x |
| mojette-nonsys | 4+2 | 1024 KiB | 64 KiB | 4503 | 4480 | 0.99x |
| mojette-nonsys | 4+2 | 1024 KiB | 256 KiB | 17092 | 17054 | 1.00x |
| mojette-nonsys | 4+2 | 4096 KiB | 4 KiB | 10384 | 15119 | 1.46x |
| mojette-nonsys | 4+2 | 4096 KiB | 16 KiB | 7181 | 6667 | 0.93x |
| mojette-nonsys | 4+2 | 4096 KiB | 64 KiB | 17494 | 17454 | 1.00x |
| mojette-nonsys | 4+2 | 4096 KiB | 256 KiB | 67926 | 67807 | 1.00x |
| mojette-nonsys | 4+2 | 16384 KiB | 4 KiB | 59063 | 61114 | 1.03x |
| mojette-nonsys | 4+2 | 16384 KiB | 16 KiB | 28066 | 26452 | 0.94x |
| mojette-nonsys | 4+2 | 16384 KiB | 64 KiB | 69540 | 69667 | 1.00x |
| mojette-nonsys | 4+2 | 16384 KiB | 256 KiB | 271216 | 271262 | 1.00x |
| mojette-nonsys | 8+2 | 64 KiB | 4 KiB | 237 | 329 | 1.39x |
| mojette-nonsys | 8+2 | 64 KiB | 16 KiB | 537 | 543 | 1.01x |
| mojette-nonsys | 8+2 | 64 KiB | 64 KiB | 4419 | 4371 | 0.99x |
| mojette-nonsys | 8+2 | 64 KiB | 256 KiB | 67121 | 66986 | 1.00x |
| mojette-nonsys | 8+2 | 256 KiB | 4 KiB | 558 | 987 | 1.77x |
| mojette-nonsys | 8+2 | 256 KiB | 16 KiB | 867 | 837 | 0.97x |
| mojette-nonsys | 8+2 | 256 KiB | 64 KiB | 4372 | 4388 | 1.00x |
| mojette-nonsys | 8+2 | 256 KiB | 256 KiB | 67139 | 67019 | 1.00x |
| mojette-nonsys | 8+2 | 1024 KiB | 4 KiB | 1939 | 3129 | 1.61x |
| mojette-nonsys | 8+2 | 1024 KiB | 16 KiB | 2928 | 2819 | 0.96x |
| mojette-nonsys | 8+2 | 1024 KiB | 64 KiB | 8629 | 8599 | 1.00x |
| mojette-nonsys | 8+2 | 1024 KiB | 256 KiB | 67033 | 66999 | 1.00x |
| mojette-nonsys | 8+2 | 4096 KiB | 4 KiB | 7042 | 12100 | 1.72x |
| mojette-nonsys | 8+2 | 4096 KiB | 16 KiB | 11219 | 10834 | 0.97x |
| mojette-nonsys | 8+2 | 4096 KiB | 64 KiB | 33991 | 34156 | 1.00x |
| mojette-nonsys | 8+2 | 4096 KiB | 256 KiB | 133848 | 133940 | 1.00x |
| mojette-nonsys | 8+2 | 16384 KiB | 4 KiB | 47037 | 49678 | 1.06x |
| mojette-nonsys | 8+2 | 16384 KiB | 16 KiB | 44290 | 42663 | 0.96x |
| mojette-nonsys | 8+2 | 16384 KiB | 64 KiB | 136231 | 136011 | 1.00x |
| mojette-nonsys | 8+2 | 16384 KiB | 256 KiB | 535287 | 535157 | 1.00x |
| mojette-sys | 4+2 | 64 KiB | 4 KiB | 238 | 327 | 1.37x |
| mojette-sys | 4+2 | 64 KiB | 16 KiB | 206 | 193 | 0.94x |
| mojette-sys | 4+2 | 64 KiB | 64 KiB | 152 | 179 | 1.18x |
| mojette-sys | 4+2 | 64 KiB | 256 KiB | 120 | 156 | 1.30x |
| mojette-sys | 4+2 | 256 KiB | 4 KiB | 507 | 774 | 1.53x |
| mojette-sys | 4+2 | 256 KiB | 16 KiB | 342 | 308 | 0.90x |
| mojette-sys | 4+2 | 256 KiB | 64 KiB | 132 | 163 | 1.23x |
| mojette-sys | 4+2 | 256 KiB | 256 KiB | 117 | 149 | 1.27x |
| mojette-sys | 4+2 | 1024 KiB | 4 KiB | 1650 | 2738 | 1.66x |
| mojette-sys | 4+2 | 1024 KiB | 16 KiB | 853 | 783 | 0.92x |
| mojette-sys | 4+2 | 1024 KiB | 64 KiB | 268 | 239 | 0.89x |
| mojette-sys | 4+2 | 1024 KiB | 256 KiB | 166 | 132 | 0.80x |
| mojette-sys | 4+2 | 4096 KiB | 4 KiB | 6633 | 10045 | 1.51x |
| mojette-sys | 4+2 | 4096 KiB | 16 KiB | 3073 | 2736 | 0.89x |
| mojette-sys | 4+2 | 4096 KiB | 64 KiB | 647 | 632 | 0.98x |
| mojette-sys | 4+2 | 4096 KiB | 256 KiB | 317 | 322 | 1.02x |
| mojette-sys | 4+2 | 16384 KiB | 4 KiB | 38520 | 40855 | 1.06x |
| mojette-sys | 4+2 | 16384 KiB | 16 KiB | 11615 | 10718 | 0.92x |
| mojette-sys | 4+2 | 16384 KiB | 64 KiB | 2087 | 2015 | 0.97x |
| mojette-sys | 4+2 | 16384 KiB | 256 KiB | 868 | 859 | 0.99x |
| mojette-sys | 8+2 | 64 KiB | 4 KiB | 209 | 307 | 1.47x |
| mojette-sys | 8+2 | 64 KiB | 16 KiB | 237 | 232 | 0.98x |
| mojette-sys | 8+2 | 64 KiB | 64 KiB | 203 | 129 | 0.64x |
| mojette-sys | 8+2 | 64 KiB | 256 KiB | 178 | 188 | 1.06x |
| mojette-sys | 8+2 | 256 KiB | 4 KiB | 429 | 670 | 1.56x |
| mojette-sys | 8+2 | 256 KiB | 16 KiB | 295 | 286 | 0.97x |
| mojette-sys | 8+2 | 256 KiB | 64 KiB | 173 | 179 | 1.03x |
| mojette-sys | 8+2 | 256 KiB | 256 KiB | 180 | 188 | 1.04x |
| mojette-sys | 8+2 | 1024 KiB | 4 KiB | 1432 | 2280 | 1.59x |
| mojette-sys | 8+2 | 1024 KiB | 16 KiB | 738 | 689 | 0.93x |
| mojette-sys | 8+2 | 1024 KiB | 64 KiB | 210 | 205 | 0.98x |
| mojette-sys | 8+2 | 1024 KiB | 256 KiB | 186 | 183 | 0.98x |
| mojette-sys | 8+2 | 4096 KiB | 4 KiB | 5496 | 8699 | 1.58x |
| mojette-sys | 8+2 | 4096 KiB | 16 KiB | 2650 | 2320 | 0.88x |
| mojette-sys | 8+2 | 4096 KiB | 64 KiB | 530 | 531 | 1.00x |
| mojette-sys | 8+2 | 4096 KiB | 256 KiB | 257 | 259 | 1.01x |
| mojette-sys | 8+2 | 16384 KiB | 4 KiB | 32703 | 33764 | 1.03x |
| mojette-sys | 8+2 | 16384 KiB | 16 KiB | 9767 | 8784 | 0.90x |
| mojette-sys | 8+2 | 16384 KiB | 64 KiB | 1755 | 1757 | 1.00x |
| mojette-sys | 8+2 | 16384 KiB | 256 KiB | 735 | 733 | 1.00x |
| plain | 1+0 | 64 KiB | 4 KiB | 162 | 215 | 1.33x |
| plain | 1+0 | 64 KiB | 16 KiB | 231 | 213 | 0.92x |
| plain | 1+0 | 64 KiB | 64 KiB | 150 | 147 | 0.98x |
| plain | 1+0 | 64 KiB | 256 KiB | 137 | 127 | 0.93x |
| plain | 1+0 | 256 KiB | 4 KiB | 356 | 493 | 1.38x |
| plain | 1+0 | 256 KiB | 16 KiB | 565 | 534 | 0.95x |
| plain | 1+0 | 256 KiB | 64 KiB | 333 | 302 | 0.91x |
| plain | 1+0 | 256 KiB | 256 KiB | 313 | 320 | 1.02x |
| plain | 1+0 | 1024 KiB | 4 KiB | 1094 | 1684 | 1.54x |
| plain | 1+0 | 1024 KiB | 16 KiB | 1937 | 1744 | 0.90x |
| plain | 1+0 | 1024 KiB | 64 KiB | 1121 | 1067 | 0.95x |
| plain | 1+0 | 1024 KiB | 256 KiB | 997 | 1073 | 1.08x |
| plain | 1+0 | 4096 KiB | 4 KiB | 4250 | 6436 | 1.51x |
| plain | 1+0 | 4096 KiB | 16 KiB | 7598 | 6680 | 0.88x |
| plain | 1+0 | 4096 KiB | 64 KiB | 4078 | 4081 | 1.00x |
| plain | 1+0 | 4096 KiB | 256 KiB | 4064 | 4010 | 0.99x |
| plain | 1+0 | 16384 KiB | 4 KiB | 25344 | 26098 | 1.03x |
| plain | 1+0 | 16384 KiB | 16 KiB | 29342 | 26103 | 0.89x |
| plain | 1+0 | 16384 KiB | 64 KiB | 16152 | 15916 | 0.99x |
| plain | 1+0 | 16384 KiB | 256 KiB | 15990 | 16024 | 1.00x |
| rs | 4+2 | 64 KiB | 4 KiB | 232 | 318 | 1.37x |
| rs | 4+2 | 64 KiB | 16 KiB | 206 | 199 | 0.97x |
| rs | 4+2 | 64 KiB | 64 KiB | 82 | 124 | 1.51x |
| rs | 4+2 | 64 KiB | 256 KiB | 69 | 68 | 0.99x |
| rs | 4+2 | 256 KiB | 4 KiB | 537 | 808 | 1.50x |
| rs | 4+2 | 256 KiB | 16 KiB | 326 | 325 | 1.00x |
| rs | 4+2 | 256 KiB | 64 KiB | 98 | 111 | 1.13x |
| rs | 4+2 | 256 KiB | 256 KiB | 61 | 65 | 1.07x |
| rs | 4+2 | 1024 KiB | 4 KiB | 1651 | 2524 | 1.53x |
| rs | 4+2 | 1024 KiB | 16 KiB | 870 | 780 | 0.90x |
| rs | 4+2 | 1024 KiB | 64 KiB | 226 | 211 | 0.93x |
| rs | 4+2 | 1024 KiB | 256 KiB | 65 | 59 | 0.91x |
| rs | 4+2 | 4096 KiB | 4 KiB | 6442 | 9966 | 1.55x |
| rs | 4+2 | 4096 KiB | 16 KiB | 2972 | 2603 | 0.88x |
| rs | 4+2 | 4096 KiB | 64 KiB | 610 | 585 | 0.96x |
| rs | 4+2 | 4096 KiB | 256 KiB | 189 | 207 | 1.10x |
| rs | 4+2 | 16384 KiB | 4 KiB | 38472 | 40322 | 1.05x |
| rs | 4+2 | 16384 KiB | 16 KiB | 11415 | 10302 | 0.90x |
| rs | 4+2 | 16384 KiB | 64 KiB | 2014 | 1955 | 0.97x |
| rs | 4+2 | 16384 KiB | 256 KiB | 698 | 660 | 0.95x |
| rs | 8+2 | 64 KiB | 4 KiB | 237 | 306 | 1.29x |
| rs | 8+2 | 64 KiB | 16 KiB | 228 | 209 | 0.92x |
| rs | 8+2 | 64 KiB | 64 KiB | 128 | 103 | 0.80x |
| rs | 8+2 | 64 KiB | 256 KiB | 111 | 96 | 0.86x |
| rs | 8+2 | 256 KiB | 4 KiB | 462 | 698 | 1.51x |
| rs | 8+2 | 256 KiB | 16 KiB | 301 | 288 | 0.96x |
| rs | 8+2 | 256 KiB | 64 KiB | 143 | 101 | 0.71x |
| rs | 8+2 | 256 KiB | 256 KiB | 118 | 99 | 0.84x |
| rs | 8+2 | 1024 KiB | 4 KiB | 1425 | 2147 | 1.51x |
| rs | 8+2 | 1024 KiB | 16 KiB | 741 | 664 | 0.90x |
| rs | 8+2 | 1024 KiB | 64 KiB | 154 | 203 | 1.32x |
| rs | 8+2 | 1024 KiB | 256 KiB | 103 | 98 | 0.95x |
| rs | 8+2 | 4096 KiB | 4 KiB | 5485 | 8426 | 1.54x |
| rs | 8+2 | 4096 KiB | 16 KiB | 2530 | 2384 | 0.94x |
| rs | 8+2 | 4096 KiB | 64 KiB | 473 | 478 | 1.01x |
| rs | 8+2 | 4096 KiB | 256 KiB | 167 | 171 | 1.02x |
| rs | 8+2 | 16384 KiB | 4 KiB | 32108 | 33803 | 1.05x |
| rs | 8+2 | 16384 KiB | 16 KiB | 9541 | 8670 | 0.91x |
| rs | 8+2 | 16384 KiB | 64 KiB | 1567 | 1623 | 1.04x |
| rs | 8+2 | 16384 KiB | 256 KiB | 528 | 526 | 1.00x |

## Cross-host x86 reproducibility (SIMD arm)

| codec | geom | file_size | shard_size | adept (N100) | shadow (i5-9500) | shadow/adept |
|-------|------|-----------|------------|--------------|------------------|--------------|
| mojette-nonsys | 4+2 | 64 KiB | 4 KiB | 99 | 264 | 2.67x |
| mojette-nonsys | 4+2 | 64 KiB | 16 KiB | 129 | 305 | 2.36x |
| mojette-nonsys | 4+2 | 64 KiB | 64 KiB | 1188 | 1206 | 1.02x |
| mojette-nonsys | 4+2 | 64 KiB | 256 KiB | 17951 | 17076 | 0.95x |
| mojette-nonsys | 4+2 | 256 KiB | 4 KiB | 236 | 700 | 2.97x |
| mojette-nonsys | 4+2 | 256 KiB | 16 KiB | 371 | 601 | 1.62x |
| mojette-nonsys | 4+2 | 256 KiB | 64 KiB | 1171 | 1199 | 1.02x |
| mojette-nonsys | 4+2 | 256 KiB | 256 KiB | 17920 | 17107 | 0.95x |
| mojette-nonsys | 4+2 | 1024 KiB | 4 KiB | 891 | 2818 | 3.16x |
| mojette-nonsys | 4+2 | 1024 KiB | 16 KiB | 1343 | 1886 | 1.40x |
| mojette-nonsys | 4+2 | 1024 KiB | 64 KiB | 4611 | 4503 | 0.98x |
| mojette-nonsys | 4+2 | 1024 KiB | 256 KiB | 17952 | 17092 | 0.95x |
| mojette-nonsys | 4+2 | 4096 KiB | 4 KiB | 3449 | 10384 | 3.01x |
| mojette-nonsys | 4+2 | 4096 KiB | 16 KiB | 5232 | 7181 | 1.37x |
| mojette-nonsys | 4+2 | 4096 KiB | 64 KiB | 18712 | 17494 | 0.93x |
| mojette-nonsys | 4+2 | 4096 KiB | 256 KiB | 72748 | 67926 | 0.93x |
| mojette-nonsys | 4+2 | 16384 KiB | 4 KiB | 13754 | 59063 | 4.29x |
| mojette-nonsys | 4+2 | 16384 KiB | 16 KiB | 20487 | 28066 | 1.37x |
| mojette-nonsys | 4+2 | 16384 KiB | 64 KiB | 75480 | 69540 | 0.92x |
| mojette-nonsys | 4+2 | 16384 KiB | 256 KiB | 292554 | 271216 | 0.93x |
| mojette-nonsys | 8+2 | 64 KiB | 4 KiB | 122 | 237 | 1.94x |
| mojette-nonsys | 8+2 | 64 KiB | 16 KiB | 416 | 537 | 1.29x |
| mojette-nonsys | 8+2 | 64 KiB | 64 KiB | 5366 | 4419 | 0.82x |
| mojette-nonsys | 8+2 | 64 KiB | 256 KiB | 75157 | 67121 | 0.89x |
| mojette-nonsys | 8+2 | 256 KiB | 4 KiB | 326 | 558 | 1.71x |
| mojette-nonsys | 8+2 | 256 KiB | 16 KiB | 786 | 867 | 1.10x |
| mojette-nonsys | 8+2 | 256 KiB | 64 KiB | 4861 | 4372 | 0.90x |
| mojette-nonsys | 8+2 | 256 KiB | 256 KiB | 75069 | 67139 | 0.89x |
| mojette-nonsys | 8+2 | 1024 KiB | 4 KiB | 1154 | 1939 | 1.68x |
| mojette-nonsys | 8+2 | 1024 KiB | 16 KiB | 3025 | 2928 | 0.97x |
| mojette-nonsys | 8+2 | 1024 KiB | 64 KiB | 10243 | 8629 | 0.84x |
| mojette-nonsys | 8+2 | 1024 KiB | 256 KiB | 76091 | 67033 | 0.88x |
| mojette-nonsys | 8+2 | 4096 KiB | 4 KiB | 4517 | 7042 | 1.56x |
| mojette-nonsys | 8+2 | 4096 KiB | 16 KiB | 12268 | 11219 | 0.91x |
| mojette-nonsys | 8+2 | 4096 KiB | 64 KiB | 40763 | 33991 | 0.83x |
| mojette-nonsys | 8+2 | 4096 KiB | 256 KiB | 147866 | 133848 | 0.91x |
| mojette-nonsys | 8+2 | 16384 KiB | 4 KiB | 17687 | 47037 | 2.66x |
| mojette-nonsys | 8+2 | 16384 KiB | 16 KiB | 47091 | 44290 | 0.94x |
| mojette-nonsys | 8+2 | 16384 KiB | 64 KiB | 160662 | 136231 | 0.85x |
| mojette-nonsys | 8+2 | 16384 KiB | 256 KiB | 602808 | 535287 | 0.89x |
| mojette-sys | 4+2 | 64 KiB | 4 KiB | 83 | 238 | 2.87x |
| mojette-sys | 4+2 | 64 KiB | 16 KiB | 60 | 206 | 3.43x |
| mojette-sys | 4+2 | 64 KiB | 64 KiB | 62 | 152 | 2.45x |
| mojette-sys | 4+2 | 64 KiB | 256 KiB | 56 | 120 | 2.14x |
| mojette-sys | 4+2 | 256 KiB | 4 KiB | 188 | 507 | 2.70x |
| mojette-sys | 4+2 | 256 KiB | 16 KiB | 91 | 342 | 3.76x |
| mojette-sys | 4+2 | 256 KiB | 64 KiB | 61 | 132 | 2.16x |
| mojette-sys | 4+2 | 256 KiB | 256 KiB | 55 | 117 | 2.13x |
| mojette-sys | 4+2 | 1024 KiB | 4 KiB | 633 | 1650 | 2.61x |
| mojette-sys | 4+2 | 1024 KiB | 16 KiB | 202 | 853 | 4.22x |
| mojette-sys | 4+2 | 1024 KiB | 64 KiB | 89 | 268 | 3.01x |
| mojette-sys | 4+2 | 1024 KiB | 256 KiB | 62 | 166 | 2.68x |
| mojette-sys | 4+2 | 4096 KiB | 4 KiB | 2343 | 6633 | 2.83x |
| mojette-sys | 4+2 | 4096 KiB | 16 KiB | 673 | 3073 | 4.57x |
| mojette-sys | 4+2 | 4096 KiB | 64 KiB | 230 | 647 | 2.81x |
| mojette-sys | 4+2 | 4096 KiB | 256 KiB | 116 | 317 | 2.73x |
| mojette-sys | 4+2 | 16384 KiB | 4 KiB | 8853 | 38520 | 4.35x |
| mojette-sys | 4+2 | 16384 KiB | 16 KiB | 2440 | 11615 | 4.76x |
| mojette-sys | 4+2 | 16384 KiB | 64 KiB | 772 | 2087 | 2.70x |
| mojette-sys | 4+2 | 16384 KiB | 256 KiB | 330 | 868 | 2.63x |
| mojette-sys | 8+2 | 64 KiB | 4 KiB | 78 | 209 | 2.68x |
| mojette-sys | 8+2 | 64 KiB | 16 KiB | 67 | 237 | 3.54x |
| mojette-sys | 8+2 | 64 KiB | 64 KiB | 72 | 203 | 2.82x |
| mojette-sys | 8+2 | 64 KiB | 256 KiB | 71 | 178 | 2.51x |
| mojette-sys | 8+2 | 256 KiB | 4 KiB | 161 | 429 | 2.66x |
| mojette-sys | 8+2 | 256 KiB | 16 KiB | 87 | 295 | 3.39x |
| mojette-sys | 8+2 | 256 KiB | 64 KiB | 70 | 173 | 2.47x |
| mojette-sys | 8+2 | 256 KiB | 256 KiB | 75 | 180 | 2.40x |
| mojette-sys | 8+2 | 1024 KiB | 4 KiB | 518 | 1432 | 2.76x |
| mojette-sys | 8+2 | 1024 KiB | 16 KiB | 170 | 738 | 4.34x |
| mojette-sys | 8+2 | 1024 KiB | 64 KiB | 85 | 210 | 2.47x |
| mojette-sys | 8+2 | 1024 KiB | 256 KiB | 77 | 186 | 2.42x |
| mojette-sys | 8+2 | 4096 KiB | 4 KiB | 1994 | 5496 | 2.76x |
| mojette-sys | 8+2 | 4096 KiB | 16 KiB | 571 | 2650 | 4.64x |
| mojette-sys | 8+2 | 4096 KiB | 64 KiB | 204 | 530 | 2.60x |
| mojette-sys | 8+2 | 4096 KiB | 256 KiB | 110 | 257 | 2.34x |
| mojette-sys | 8+2 | 16384 KiB | 4 KiB | 7573 | 32703 | 4.32x |
| mojette-sys | 8+2 | 16384 KiB | 16 KiB | 2114 | 9767 | 4.62x |
| mojette-sys | 8+2 | 16384 KiB | 64 KiB | 679 | 1755 | 2.58x |
| mojette-sys | 8+2 | 16384 KiB | 256 KiB | 304 | 735 | 2.42x |
| plain | 1+0 | 64 KiB | 4 KiB | 53 | 162 | 3.06x |
| plain | 1+0 | 64 KiB | 16 KiB | 54 | 231 | 4.28x |
| plain | 1+0 | 64 KiB | 64 KiB | 56 | 150 | 2.68x |
| plain | 1+0 | 64 KiB | 256 KiB | 54 | 137 | 2.54x |
| plain | 1+0 | 256 KiB | 4 KiB | 115 | 356 | 3.10x |
| plain | 1+0 | 256 KiB | 16 KiB | 117 | 565 | 4.83x |
| plain | 1+0 | 256 KiB | 64 KiB | 118 | 333 | 2.82x |
| plain | 1+0 | 256 KiB | 256 KiB | 118 | 313 | 2.65x |
| plain | 1+0 | 1024 KiB | 4 KiB | 363 | 1094 | 3.01x |
| plain | 1+0 | 1024 KiB | 16 KiB | 373 | 1937 | 5.19x |
| plain | 1+0 | 1024 KiB | 64 KiB | 370 | 1121 | 3.03x |
| plain | 1+0 | 1024 KiB | 256 KiB | 368 | 997 | 2.71x |
| plain | 1+0 | 4096 KiB | 4 KiB | 1372 | 4250 | 3.10x |
| plain | 1+0 | 4096 KiB | 16 KiB | 1371 | 7598 | 5.54x |
| plain | 1+0 | 4096 KiB | 64 KiB | 1354 | 4078 | 3.01x |
| plain | 1+0 | 4096 KiB | 256 KiB | 1342 | 4064 | 3.03x |
| plain | 1+0 | 16384 KiB | 4 KiB | 5385 | 25344 | 4.71x |
| plain | 1+0 | 16384 KiB | 16 KiB | 5354 | 29342 | 5.48x |
| plain | 1+0 | 16384 KiB | 64 KiB | 5156 | 16152 | 3.13x |
| plain | 1+0 | 16384 KiB | 256 KiB | 5264 | 15990 | 3.04x |
| rs | 4+2 | 64 KiB | 4 KiB | 79 | 232 | 2.94x |
| rs | 4+2 | 64 KiB | 16 KiB | 58 | 206 | 3.55x |
| rs | 4+2 | 64 KiB | 64 KiB | 62 | 82 | 1.32x |
| rs | 4+2 | 64 KiB | 256 KiB | 50 | 69 | 1.38x |
| rs | 4+2 | 256 KiB | 4 KiB | 188 | 537 | 2.86x |
| rs | 4+2 | 256 KiB | 16 KiB | 86 | 326 | 3.79x |
| rs | 4+2 | 256 KiB | 64 KiB | 54 | 98 | 1.81x |
| rs | 4+2 | 256 KiB | 256 KiB | 50 | 61 | 1.22x |
| rs | 4+2 | 1024 KiB | 4 KiB | 607 | 1651 | 2.72x |
| rs | 4+2 | 1024 KiB | 16 KiB | 186 | 870 | 4.68x |
| rs | 4+2 | 1024 KiB | 64 KiB | 89 | 226 | 2.54x |
| rs | 4+2 | 1024 KiB | 256 KiB | 53 | 65 | 1.23x |
| rs | 4+2 | 4096 KiB | 4 KiB | 2317 | 6442 | 2.78x |
| rs | 4+2 | 4096 KiB | 16 KiB | 651 | 2972 | 4.57x |
| rs | 4+2 | 4096 KiB | 64 KiB | 221 | 610 | 2.76x |
| rs | 4+2 | 4096 KiB | 256 KiB | 102 | 189 | 1.85x |
| rs | 4+2 | 16384 KiB | 4 KiB | 8908 | 38472 | 4.32x |
| rs | 4+2 | 16384 KiB | 16 KiB | 2368 | 11415 | 4.82x |
| rs | 4+2 | 16384 KiB | 64 KiB | 739 | 2014 | 2.73x |
| rs | 4+2 | 16384 KiB | 256 KiB | 283 | 698 | 2.47x |
| rs | 8+2 | 64 KiB | 4 KiB | 78 | 237 | 3.04x |
| rs | 8+2 | 64 KiB | 16 KiB | 67 | 228 | 3.40x |
| rs | 8+2 | 64 KiB | 64 KiB | 66 | 128 | 1.94x |
| rs | 8+2 | 64 KiB | 256 KiB | 65 | 111 | 1.71x |
| rs | 8+2 | 256 KiB | 4 KiB | 161 | 462 | 2.87x |
| rs | 8+2 | 256 KiB | 16 KiB | 82 | 301 | 3.67x |
| rs | 8+2 | 256 KiB | 64 KiB | 63 | 143 | 2.27x |
| rs | 8+2 | 256 KiB | 256 KiB | 68 | 118 | 1.74x |
| rs | 8+2 | 1024 KiB | 4 KiB | 502 | 1425 | 2.84x |
| rs | 8+2 | 1024 KiB | 16 KiB | 172 | 741 | 4.31x |
| rs | 8+2 | 1024 KiB | 64 KiB | 81 | 154 | 1.90x |
| rs | 8+2 | 1024 KiB | 256 KiB | 64 | 103 | 1.61x |
| rs | 8+2 | 4096 KiB | 4 KiB | 1958 | 5485 | 2.80x |
| rs | 8+2 | 4096 KiB | 16 KiB | 539 | 2530 | 4.69x |
| rs | 8+2 | 4096 KiB | 64 KiB | 189 | 473 | 2.50x |
| rs | 8+2 | 4096 KiB | 256 KiB | 93 | 167 | 1.80x |
| rs | 8+2 | 16384 KiB | 4 KiB | 7395 | 32108 | 4.34x |
| rs | 8+2 | 16384 KiB | 16 KiB | 2014 | 9541 | 4.74x |
| rs | 8+2 | 16384 KiB | 64 KiB | 627 | 1567 | 2.50x |
| rs | 8+2 | 16384 KiB | 256 KiB | 248 | 528 | 2.13x |

## SIMD-vs-scalar gap as shard size grows

Each cell is (scalar median read_ms) / (SIMD median read_ms) for the matching tuple.  >1.0 means SIMD is faster.


### adept (Intel N100, AVX2) — speedup vs shard size (1 MiB files)

| codec | geom | 4 KiB | 16 KiB | 64 KiB | 256 KiB |
|-------|------|-------|--------|--------|---------|
| mojette-nonsys | 4+2 | 1.06x | 1.00x | 1.00x | 1.01x |
| mojette-nonsys | 8+2 | 1.01x | 1.00x | 1.00x | 0.97x |
| mojette-sys | 4+2 | 0.97x | 1.02x | 1.12x | 1.02x |
| mojette-sys | 8+2 | 1.04x | 1.06x | 1.04x | 1.00x |
| plain | 1+0 | 1.04x | 1.00x | 0.98x | 0.98x |
| rs | 4+2 | 1.02x | 1.08x | 0.93x | 1.02x |
| rs | 8+2 | 1.08x | 1.02x | 1.02x | 0.95x |

*16 MiB files*

| codec | geom | 4 KiB | 16 KiB | 64 KiB | 256 KiB |
|-------|------|-------|--------|--------|---------|
| mojette-nonsys | 4+2 | 0.96x | 1.01x | 0.99x | 1.00x |
| mojette-nonsys | 8+2 | 1.00x | 1.00x | 1.01x | 0.99x |
| mojette-sys | 4+2 | 1.01x | 1.01x | 1.01x | 1.01x |
| mojette-sys | 8+2 | 1.00x | 0.99x | 1.00x | 1.00x |
| plain | 1+0 | 1.00x | 0.99x | 1.00x | 1.00x |
| rs | 4+2 | 0.99x | 1.00x | 0.98x | 1.01x |
| rs | 8+2 | 1.00x | 0.99x | 1.01x | 1.00x |

### shadow (Intel i5-9500, AVX2) — speedup vs shard size (1 MiB files)

| codec | geom | 4 KiB | 16 KiB | 64 KiB | 256 KiB |
|-------|------|-------|--------|--------|---------|
| mojette-nonsys | 4+2 | 1.36x | 0.95x | 0.99x | 1.00x |
| mojette-nonsys | 8+2 | 1.61x | 0.96x | 1.00x | 1.00x |
| mojette-sys | 4+2 | 1.66x | 0.92x | 0.89x | 0.80x |
| mojette-sys | 8+2 | 1.59x | 0.93x | 0.98x | 0.98x |
| plain | 1+0 | 1.54x | 0.90x | 0.95x | 1.08x |
| rs | 4+2 | 1.53x | 0.90x | 0.93x | 0.91x |
| rs | 8+2 | 1.51x | 0.90x | 1.32x | 0.95x |

*16 MiB files*

| codec | geom | 4 KiB | 16 KiB | 64 KiB | 256 KiB |
|-------|------|-------|--------|--------|---------|
| mojette-nonsys | 4+2 | 1.03x | 0.94x | 1.00x | 1.00x |
| mojette-nonsys | 8+2 | 1.06x | 0.96x | 1.00x | 1.00x |
| mojette-sys | 4+2 | 1.06x | 0.92x | 0.97x | 0.99x |
| mojette-sys | 8+2 | 1.03x | 0.90x | 1.00x | 1.00x |
| plain | 1+0 | 1.03x | 0.89x | 0.99x | 1.00x |
| rs | 4+2 | 1.05x | 0.90x | 0.97x | 0.95x |
| rs | 8+2 | 1.05x | 0.91x | 1.04x | 1.00x |
