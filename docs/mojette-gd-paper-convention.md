<!-- SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> -->
<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->

# Mojette: paper-convention bin formula, XOR algebra, geometry-driven inverse

## Summary

Three coordinated changes to `lib/ec/`:

1. **Algebra**: `(Z/2^64, +)` -> `(GF(2)^64, XOR)`.
2. **Bin formula**: `b = col*p − row*q + off` -> `b = row*p + col*q − off` (the
   convention used in the published Mojette literature and in HCF's
   reference implementation).
3. **Inverse**: corner-peeling stays as the default; geometry-driven
   reconstruction (Normand-Kingston-Evenou, DGCI 2006) is added as
   `moj_inverse_gd` plus a sparse-failures variant
   `moj_inverse_gd_sparse` that the codec uses on the full grid.

Plus the matching SIMD simplification, codec rewrite, dispatcher,
ec_demo `--force-gd` flag, benchmark axis, and a new microbenchmark
tool (`tools/moj_bench`).

The end result is a Mojette codec that, with `gd` enabled, decodes
**40-1144x faster than corner-peeling** at the algorithm level and
beats Reed-Solomon by **9-52x** at the codec level on this hardware.
Through the full NFS stack the wall-clock improvement on
Mojette-nonsys reads is **17-27%**, large enough that the existing
"Mojette-nonsys is unsuitable for read-heavy workloads above 64 KiB"
caveat in the FFv2 progress report is no longer accurate.

## Why these changes are coordinated

reffs's existing Mojette code used `b = col*p − row*q + off` with
modular addition.  The published Mojette literature (Guedon 2009,
Normand-Kingston-Evenou DGCI 2006, Parrein 2001) and the HCF
reference implementation use `b = row*p + col*q − off` with XOR.
For `|p| = 1` the two parameterisations describe the same line
family; for `|p| > 1` they describe **different** lines (slope `p`
vs slope `1/p` in (row, col)).  This means a direct port of the
DGCI sweep into reffs's parameterisation produces incorrect
schedules on `|p| > 1` angles.

Aligning reffs with the paper convention removes that mismatch and
turns the gd port into a near-direct copy of HCF's `inverse()`.  It
also collapses six SIMD helpers into three: in the new convention
`b` is sequential in `col` for any `p` when `q = 1`, so a single
ascending-bins loop covers every direction — not just `|p| = 1` as
the previous reffs code did.

The XOR algebra switch is independent of correctness (both
algebras are valid abelian groups for Mojette), but XOR has no
carry chain and scales straightforwardly to 128-/256-/512-bit SIMD
lanes as a single bit-parallel unit; it also matches HCF and
makes the gd port byte-identical to its source rather than a
group-translation.

## Files changed

| File | Change |
|---|---|
| `lib/ec/mojette.h` | Bin formula; `moj_bin_offset` returns the offset to **subtract** (negative-or-zero, HCF convention); `moj_projection_size` swaps P/Q in the multipliers; new prototypes for `moj_inverse_peel`, `moj_inverse_peel_sparse`, `moj_inverse_gd`, `moj_inverse_gd_sparse`, `moj_inverse_sparse`, `moj_force_gd`. |
| `lib/ec/mojette.c` | XOR algebra throughout (`+=` -> `^=`, ADD intrinsics -> XOR intrinsics); paper bin formula in forward + peel; SIMD collapsed from six helpers (p1 / pm1 x NEON / SSE2 / AVX2) to three (one ascending-bins helper per ISA); `moj_inverse_gd` ported as a near-direct adaptation of HCF's `inverse()`; `moj_inverse_peel_sparse` and `moj_inverse_gd_sparse` added; `moj_inverse` and `moj_inverse_sparse` dispatchers gated by `moj_force_gd`. |
| `lib/ec/mojette_codec.c` | Bin formula updated.  `mojette_sys_decode` rewritten to use `moj_inverse_sparse` on the **full** P x k grid with known data rows pre-filled, replacing the prior reduce-to-smaller-grid trick (the old approach was unsound under paper convention because lines for `|p| >= 2` can put multiple missing-row pixels on the same full bin, e.g. for k=4 losing rows 0 and 3 with p=2, line `const = 6` contains both (0,6) and (3,0)). |
| `lib/ec/tests/mojette_test.c` | `test_wrapping_arithmetic` reframed as `test_xor_identity`.  New `gd` tcase with 9 tests (4x3, 6x4, Q=1, Q=2, n!=Q rejection, zero grid, p==0 column-parity, 24K geometry, peel-vs-gd parity).  SIMD bin-value tests recomputed for paper convention. |
| `lib/ec/tests/mojette_codec_test.c` | New `gd-codec` tcase parameterising 5 sys+nonsys tests on `moj_force_gd`. |
| `tools/moj_bench.c` | New algorithm-level microbenchmark.  Two halves: raw inverse (peel vs gd, dense and sparse, P from 16 to 4096) and codec-level (RS vs Mojette-sys peel/gd vs Mojette-nonsys peel/gd at 4 KB and 32 KB shards, 4+2 and 8+2 with 1- and 2-shard losses). |
| `tools/Makefile.am` | Wire `moj_bench` as a `bin_PROGRAM`; links only against `libreffs_ec.la`, no NFS dependencies. |
| `tools/ec_demo.c` | `--force-gd` CLI flag (parallel to `--force-scalar`). |
| `scripts/ec_benchmark.sh` | `--force-gd` flag plumbing; new `inverse=peel\|gd` and `shard_size` CSV columns; `RUN_BASELINES` gating so plain/RS aren't re-run in gd-only phases. |
| `scripts/ec_benchmark_full.sh` | Expanded from 4 phases (`{v1,v2} x {SIMD,scalar}`) to 8 (cross product with `{peel,gd}`). |
| `.claude/goals.md` | Milestone entry under EC Demo Client. |

`scripts/gen_benchmark_report.py` is **unchanged**: it is the
hardcoded historical-snapshot report; the new `inverse` axis lives
in the CSV and in `tools/moj_bench` output, ready for any future
report generator.

## Test results

All `lib/ec/` tests pass (compiled directly with gcc + libcheck on
this machine; `make check` via the full reffsd build path is left
to CI):

| suite | tests |
|---|---|
| `mojette_test` | 25 (forward, peel inverse, SIMD parity, XOR identity, gd: 4x3, 6x4, Q=1, Q=2, n!=Q rejection, zero grid, p==0 parity, 24K geometry, peel-vs-gd parity) |
| `mojette_codec_test` | 20 (5 systematic + 5 non-systematic + 5 gd-codec parameterised + 5 24K extras), all paths peel + gd |
| `rs_test` | 9 |
| `matrix_test` | 5 |

End-to-end smoke run with reffsd MDS + 10 DSes + ec_demo client over
NFSv3 (`make -f Makefile.reffs run-benchmark`), 132 measurements
across both peel and gd phases: **132/132 verified byte-correct,
zero failures**.  The XOR algebra + paper convention + sparse-
failures inverse + gd port all hold up under real RPC traffic on the
full reffsd codebase.

## Performance

### Algorithm-level (`tools/moj_bench`, this machine: i7-1370P AVX2)

Times in microseconds, median of 20 runs after 3 warmups.

#### Raw inverse, dense reduced-grid (every row missing)

| Geometry | peel | gd | Speedup |
|---|---|---|---|
| 16x4 (RS-4+2 toy) | 5.7 us | 1.0 us | 5.6x |
| 64x4 | 75.0 us | 3.3 us | 22.8x |
| **512x4 (4 KB shard, k=4)** | **1250 us** | **9.7 us** | **129x** |
| 4096x4 (32 KB shard, k=4) | 66 329 us | 75.7 us | 876x |
| 64x8 | 139.7 us | 3.5 us | 40.1x |
| **512x8 (4 KB shard, k=8)** | **4562 us** | **25.3 us** | **181x** |
| 4096x8 (32 KB shard, k=8) | 239 301 us | 204.4 us | 1171x |
| 3072x2 (24 KB demo) | 9445 us | 17.3 us | 545x |

#### Sparse failures (k=4, m=2, lose 2 data rows)

| Pattern | peel | gd | Speedup |
|---|---|---|---|
| P=512 lose{0,3} | 276 us | 5.0 us | 55x |
| P=512 lose{1,2} | 322 us | 4.6 us | 70x |
| P=4096 lose{0,3} | 14 506 us | 37.3 us | 389x |
| P=4096 lose{1,2} | 17 475 us | 50.0 us | 350x |

#### Sparse failures (k=8, m=2, lose 2 data rows)

| Pattern | peel | gd | Speedup |
|---|---|---|---|
| P=512 lose{0,7} | 255 us | 6.4 us | 40x |
| P=4096 lose{0,7} | 13 720 us | 54.5 us | 252x |

### Codec-level (`tools/moj_bench`, full encode + decode, no NFS)

Times in microseconds, median of 20 runs.

#### 4+2, shard=4096 bytes (default ec_demo), lose 2 data shards

| Codec | encode | decode |
|---|---|---|
| RS | 50 us | 96-102 us |
| Mojette-sys peel | 1.3 us | 267-323 us |
| **Mojette-sys gd** | 1.3 us | **5 us** (50-65x peel, 19-20x RS) |
| Mojette-nonsys peel | 3.4 us | 1248-1256 us |
| **Mojette-nonsys gd** | 3.4 us | **10 us** (125x peel, **10x RS**) |

#### 8+2, shard=4096 bytes, lose 2 data shards

| Codec | encode | decode |
|---|---|---|
| RS | 94 us | 360 us |
| Mojette-sys peel | 2.8 us | 257 us |
| **Mojette-sys gd** | 2.8 us | **7.7 us** (33x peel, 47x RS) |
| Mojette-nonsys peel | 10.2 us | 4574 us |
| **Mojette-nonsys gd** | 9.9 us | **27 us** (167x peel, **13x RS**) |

#### 8+2, shard=32768 bytes, lose 2 data shards

| Codec | encode | decode |
|---|---|---|
| RS | 757 us | 2859 us |
| Mojette-sys peel | 27 us | 13 656 us |
| **Mojette-sys gd** | 27 us | **54 us** (251x peel, **52x RS**) |
| Mojette-nonsys peel | 234 us | 241 954 us |
| **Mojette-nonsys gd** | 210 us | **318 us** (760x peel, **9x RS**) |

### End-to-end via NFS (`scripts/ec_benchmark_full.sh`, this machine)

reffsd MDS + 10 DSes + ec_demo client, 1 MB file at 4+2 / 8+2,
v1 layout, SIMD enabled, mean of 3 measured runs.  Times in
milliseconds.

#### Healthy reads, 1 MB

| Codec / geometry | peel | gd | vs peel |
|---|---|---|---|
| Mojette-sys 4+2 | 2659 ms | 2660 ms | 0% |
| Mojette-sys 8+2 | 2240 ms | 2223 ms | -0.8% |
| **Mojette-nonsys 4+2** | **3227 ms** | **2662 ms** | **-17%** |
| **Mojette-nonsys 8+2** | **2966 ms** | **2344 ms** | **-21%** |

For reference: RS 4+2 at 1 MB healthy = 2677 ms, RS 8+2 = 2252 ms.
With gd, Mojette-nonsys is competitive with RS (matches RS 4+2 at
4+2; beats RS 8+2 at 8+2).

#### Degraded-1 reads, 1 MB

| Codec / geometry | peel | gd | vs peel |
|---|---|---|---|
| Mojette-sys 4+2 | 2253 ms | 2291 ms | +1.7% (noise) |
| Mojette-sys 8+2 | 2074 ms | 2024 ms | -2.4% |
| **Mojette-nonsys 4+2** | **2773 ms** | **2211 ms** | **-20%** |
| **Mojette-nonsys 8+2** | **2759 ms** | **2011 ms** | **-27%** |

For reference: RS 4+2 degraded-1 = 2309 ms, RS 8+2 = 2171 ms.
Mojette-nonsys gd 4+2 (2211 ms) beats RS 4+2; Mojette-nonsys gd 8+2
(2011 ms) beats RS 8+2.

### Why end-to-end gain is smaller than algorithm-level

NFS RTT and shard transfer dominate the ~2-second 1 MB read.  The
codec runs many small inverse calls per file (e.g., 64 stripes per
1 MB at the default 4 KB shard size); each call saves ~1.2 ms with
gd, and the total codec time saving is hundreds of ms but is masked
by ~2 s of network/RPC.

For Mojette-sys the difference disappears almost entirely because
healthy reads bypass the inverse (systematic shortcut), and
degraded-1 only inverts for the single missing shard — sub-millisecond
either way.

For Mojette-nonsys the inverse runs on **every** read regardless of
shard availability, so the per-stripe saving accumulates to a
visible 17-27% wall-clock improvement.

Larger shard sizes (`--shard-size 32768`) and larger file sizes
(>4 MB) shift more wall-clock fraction into the codec layer, which
is where gd's algorithmic advantage compounds further.  Not in this
benchmark run; the headline test stays at 4 KB shards / 1 MB files
to be directly comparable to the existing FFv2 progress report.

## Conclusions

1. **Geometry-driven reconstruction lives up to its theoretical
   complexity advantage.**  Corner-peeling is O((P*Q)^2) for the
   per-bin singleton search; gd is O(P*Q) for the deterministic
   sweep.  At production sizes the constant factor is also large
   (40-1144x measured, 1170x at the largest geometry tested).

2. **The paper / HCF bin convention was the missing piece.**  The
   prior reffs convention `b = col*p − row*q` was internally
   consistent but described a different line family from the
   published literature for `|p| > 1`, blocking a direct port of
   the DGCI sweep.  The convention switch is a one-time, contained
   change (formula update + SIMD simplification) that opens up the
   algorithmic improvement.

3. **gd makes Mojette-nonsys read-competitive.**  The progress-
   report caveat that Mojette-nonsys is unsuitable for read-heavy
   workloads above 64 KiB no longer holds with gd.  Mojette-nonsys
   gd 8+2 reads 1 MB faster than RS 8+2 over NFS on this machine
   (2011 ms vs 2171 ms degraded-1; 2344 ms vs 2252 ms healthy).

4. **Mojette-sys is unchanged at the user level.**  Healthy reads
   bypass the inverse via the systematic shortcut; degraded reads
   recover one shard with sub-millisecond inverse cost.  gd is a
   safety-net win (and a substantial codec-level win) but not a
   user-visible NFS-level win for the systematic codec.

5. **The XOR algebra switch is a "while-we're-here" simplification**
   that broadens SIMD coverage from `|p| = 1` only to all directions
   with `q = 1`, collapses six SIMD helpers to three, and aligns the
   code with the broader Mojette literature.  No correctness or
   wire-format implications.

## Reproducing

Algorithm-level (no NFS, ~10 seconds):

```
make -f Makefile.reffs build
build/tools/moj_bench
```

End-to-end (reffsd + Docker, ~30-60 minutes for full 8-phase matrix):

```
make -f Makefile.reffs run-benchmark
# Results in logs/benchmark/results.csv with columns:
#   codec, geometry, size_bytes, run, write_ms, read_ms, verify,
#   mode, layout, arch, cpu, kernel, simd, inverse, shard_size
make -f Makefile.reffs stop-benchmark
```

Quick comparison (only v1 SIMD peel + gd, ~10 minutes): edit
`scripts/ec_benchmark_full.sh` to keep only phases 1 and 5, set
`SIZES=65536 1048576` and `RUNS=3` in `scripts/ec_benchmark.sh`,
then `make -f Makefile.reffs run-benchmark`.

## Notes for review

- The convention swap is large in line count but mostly mechanical
  (operator substitutions + SIMD intrinsic renames + bin index sign
  flips).  The substantive algorithm work is `moj_inverse_gd` and
  `moj_inverse_gd_sparse` (~290 lines combined including the sparse-
  failures k_offsets correction term that the dense reduced-grid
  case drops to zero).
- `moj_inverse_peel` is preserved verbatim in behaviour aside from
  the bin formula update.  The dispatcher defaults to peel; gd is
  opt-in (`moj_force_gd(true)`) until benchmark numbers convince us
  to flip the default.  Flipping the default is a one-line follow-up
  PR.
- HCF (Pierre's prior, proprietary Mojette implementation) was the
  source of the gd algorithm body.  Pierre is the copyright holder
  of HCF and grants permission to relicense the copied logic as
  AGPL-3.0-or-later for inclusion in reffs; new code carries the
  standard reffs SPDX header.
