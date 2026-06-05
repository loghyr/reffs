<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Realnet Harness + CSV Schema (prereq #4 of 4-variant)

## Why this exists

`ps-encoder-bench-4variant-realnet.md` lists this as **prereq #4**
(2-3 days): the data-collection layer that drives the 3-host
realnet topology (prereq #3) through the 4-variant cell matrix
and emits CSV ready for downstream analysis (deck slide 22 on
`ietf126.md`).

## Scope

This slice ships the harness shell that:

1. Drives **all four variants** against the realnet bringup
   landed by prereq #3.  Variant d goes kernel client -> PS ->
   MDS+DSes via the PS listener on `:4098`; variants a/b/c go
   kernel client -> MDS direct on `:2049` via the dual-mode
   STARTTLS-fallthrough path the MDS already supports (see
   `.claude/design/multi-host-bench-bringup.md` scope
   clarification, 2026-06-05 revision).  Variants a/b/c share
   an implementation (`run_cell_kernel_mds`) until per-variant
   MDS export configs (single-DS FFv1 / multi-DS FFv1 / FFv2)
   land in a follow-up slice; CSV rows are tagged by variant
   letter so the eventual split lands as data without re-running
   prior captures.
2. Writes CSV with a schema compatible with the existing
   `run_ps_vs_client_bench.sh` analysis tooling so the variant-d
   numbers can be cross-correlated with the single-host A/B/C
   numbers in the 2026-06-02 dataset.
3. Provides hooks (commented stubs + a `case` arm per variant)
   that the follow-up slices can drop their implementations into
   without re-architecting.

What this slice does NOT ship:

- Variants a/b/c implementations (blocked on plain-MDS bringup).
- Multi-PS variant d (single-PS only -- the realnet bringup is
  single-PS by design; the existing single-host bringup handles
  multi-PS via NPS env var).
- HTML report generation (`scripts/gen_benchmark_report.py`
  already exists and will pick up the realnet CSV unmodified
  if the columns match).

## CSV schema

Reuses the proven header from `run_ps_vs_client_bench.sh`:

```
variant,codec,geometry,size_bytes,iter,write_ms,read_ms,verify,note
```

Extensions for realnet bench (appended for column-stability with
the single-host CSV):

```
,topology,client_host,ps_host,ds_mds_host,timestamp,bytes_per_sec_write,bytes_per_sec_read
```

Why the extensions:

| Column | Why |
|--------|-----|
| `topology` | "realnet-3host" or "singlehost-loopback" -- distinguishes the two cohorts in a merged analysis |
| `client_host` | Where the client lived (dreamer for realnet, builtin for single-host) -- the CPU and NIC differ across these |
| `ps_host` | Where the PS lived (adept for realnet, same-host for single-host); blank for variants without a PS |
| `ds_mds_host` | Where the DS + MDS lived (shadow for realnet, same-host for single-host) |
| `timestamp` | ISO-8601 UTC of cell start.  Distinguishes runs taken minutes apart from runs taken weeks apart |
| `bytes_per_sec_write` | size / (write_ms / 1000); derived but cached so spreadsheet pivots stay simple |
| `bytes_per_sec_read` | size / (read_ms / 1000); ditto |

The `gen_benchmark_report.py` HTML report keys on
`variant,codec,geometry,size_bytes` and tolerates extra columns
-- the realnet extensions are additive, not breaking.

## Cell matrix (variant d only this slice)

Same matrix as the 4-variant design document, restricted to d:

| Axis | Values | Count |
|------|--------|-------|
| Codec | rs, mojette-sys, mojette-nonsys | 3 |
| Geometry | 4+2, 8+2 | 2 |
| Size | 4 KB, 16 KB, 64 KB, 256 KB, 1 MB, 4 MB, 16 MB | 7 |
| Iter | 5 per cell | 5 |

**Variant d cell count: 210.**  At ~2 s per cell (real network
latency), the full sweep is ~7 min sustained.  Smoke runs use
`--codec rs --geometry 4+2 --sizes 4096,1048576 --iters 2` for
~30 s end-to-end.

The full 4-variant matrix (525 cells) lands when variants a/b/c
unblock.

## Bringup integration

The harness assumes the realnet topology is up:
- shadow has `reffs-bench-mds` + DS containers running
- adept has reffsd PS listening on 4098
- dreamer can reach both

Detection:
1. `nc -zv ADEPT_LAN_IP 4098` -- PS listener up?
2. `mount -t nfs4 -o port=4098 ADEPT:/ /mnt/realnet-bench` --
   mount works?
3. If either fails: print a "run-realnet-bringup.sh first"
   message and exit 1.

`--bring-up` flag is **NOT** offered in this slice.  The
3-host bringup takes 5-10 minutes and the user has just done it
manually before invoking the harness; auto-bringing-up on every
harness invocation is wasteful.  The follow-up that does multi-
topology orchestration can wrap both scripts.

## Harness shape

```
deploy/benchmark/run-realnet-harness.sh [options]

Options:
  --codecs LIST        Codecs to test (default: rs)
                       Comma-separated: rs,mojette-sys,mojette-nonsys
  --geometries LIST    K+M pairs (default: 4+2)
                       Comma-separated: 4+2,8+2
  --sizes LIST         Byte sizes (default: 4096,65536,1048576)
  --iters N            Iterations per cell (default: 3)
  --variants LIST      Variants (default: d)
                       Comma-separated subset of a,b,c,d
  --client-host HOST   Where to run the kernel client (default: dreamer)
  --ps-host HOST       Where the PS is (default: adept)
  --ds-mds-host HOST   Where DS+MDS are (default: shadow)
  --out PATH           CSV path (default: results/realnet/<ts>.csv)
  --mount-point PATH   Where to mount on client (default: /mnt/realnet-bench)
```

Defaults are deliberately conservative for a first smoke
(rs/4+2/3 sizes/3 iters = 9 cells, ~30 s).  Override for full
sweeps.

## Per-cell flow (variant d)

For each (codec, geometry, size, iter):

1. Generate `cell_id = "d_<codec>_<k>_<m>_<size>_<iter>"`.
2. Pre-create random input on client host:
   `/tmp/realnet_in_<size>.bin` (one per size, cached).
3. **Write timing**: wall-clock around
   `dd if=<input> of=<mountpoint>/<cell_id>.bin bs=<size> count=1 conv=fsync`.
   The `conv=fsync` forces the kernel to flush + COMMIT before
   `dd` returns, matching how the single-host harness uses
   `--end_fsync=1` on fio.
4. **Read timing**: wall-clock around
   `dd if=<mountpoint>/<cell_id>.bin of=<output> bs=<size>`.
   Drop client page cache between write and read:
   `echo 3 > /proc/sys/vm/drop_caches`.  Without this the read
   measures cache latency, not the codec round-trip.
5. **Verify**: `cmp -s input output` -> "OK" or "FAIL"; on FAIL
   include the first-diff offset in the `note` column.
6. Compute `bytes_per_sec_*` and emit CSV row.
7. Unlink `<cell_id>.bin` from the mount before moving on (keep
   the mount free of cell-state between iters).

Per-cell rationale:
- `dd` over `fio` for variant d: the read flow needs cache-drop
  control, which fio's internal cache flags do not expose
  uniformly across kernel versions.  `dd` + `drop_caches` is
  unambiguous.
- `conv=fsync` on write matches fio's `--end_fsync=1` and
  ensures the COMMIT round-trip is part of the measured
  duration.

## Codec / geometry plumbing

The MDS issues the codec from its per-export `default_coding`
config (`.claude/design/per-export-default-coding.md`, shipped
in `default-coding-toml` slice 2026-06-03).  The bench MDS
config has `default_coding = "rs:4+2"` -- the harness either:

(a) Accepts the MDS's default and runs only that codec/geometry
    combination, OR
(b) Sends `SB_SET_DEFAULT_CODING` probe RPCs to the MDS to
    switch codec/geometry between cells.

For this slice: **(b)**.  Between cell iterations the harness
invokes `reffs-probe.py sb-set-default-coding --id 1 --codec
<codec> --k <k> --m <m>` against `MDS:20490`.  The probe op
already exists (per-export-default-coding step 9 / sb_set_default_coding
client RPC).

Why (b): without it, multi-codec sweeps require restarting the
MDS between codecs to pick up a new config -- 15-30 s of dead
time per codec, breaks the cell-matrix structure of the sweep.

Failure mode: if the probe RPC fails (e.g., MDS is gone), the
harness emits `note=set_default_coding_failed` and skips the
cell.

## Variants a/b/c implementation

The harness mounts the realnet MDS once at `$MOUNT_POINT_MDS`
(default `/mnt/realnet-mds-bench`) via NFSv4.2 + AUTH_SYS on
the MDS's native `:2049` port and runs `run_cell_kernel_mds`
per cell.  The implementation is identical to `run_cell_d`
except for the mount point and the variant-specific subdir:
kernel does the WRITE + fsync COMMIT, kernel drops page cache,
kernel does the READ, harness `cmp`s for `verify=OK`.

Per-variant MDS exports (landed 2026-06-05) make the three
variants measure distinct paths:

- `/a` — FFv1, dstores=`[1]`, `default_coding="passthrough"`.
  Single DS, no parity, no mirroring.  Baseline measurement.
- `/b` — FFv1, dstores=`[1, 2, 3, 4]`, `default_coding=
  "passthrough"`.  K-way mirror (CSM); kernel writes the file
  to every dstore.  Measures multi-mirror cost vs single-DS.
- `/c` — FFv2, dstores=`[1, 2, 3, 4, 5, 6]`, `default_coding=
  "rs:4+2"`.  Server-side EC via CHUNK ops; the DSes receive
  encoded shards.  The natural counterpart to variant d (client-
  side EC via PS).

The harness writes to `$MOUNT_POINT_MDS/<variant>/<cell_id>.bin`;
NFSv4.2 sub-export traversal routes I/O to the right export
under the single root mount.  The MDS auto-creates the `/a`,
`/b`, `/c` mount-crossing points at boot when it processes the
matching `[[export]]` blocks in `mds-realnet.toml`.

## Output organization

```
deploy/benchmark/results/realnet/
  realnet-d-shadow-20260604-053200.csv
  realnet-d-shadow-20260604-064100.csv
  ...
```

Filename pattern: `realnet-<variants>-<ds_mds_host>-<utc_ts>.csv`.

Multiple invocations accumulate in the same directory.  No
roll-up / merge tooling in this slice -- a follow-up
`scripts/realnet_merge_csv.py` can stitch and dedupe by
`(variant, codec, geometry, size_bytes, iter, timestamp)`.

## Test plan

### Existing tests affected: NONE

All changes are new files (a shell script + a design doc).
No code in `lib/` moves, no XDR, no probe ops added.

### Smoke (run as part of slice shipping)

1. Confirm realnet topology is up (`nc -zv` checks).
2. Run harness with `--codecs rs --geometries 4+2 --sizes 4096,1048576 --iters 2 --variants d`.
   8 cells (2 sizes * 2 iters * 2 ops + iter overhead = ~30 s).
3. Open the CSV and verify:
   - 4 rows (2 sizes * 2 iters, one row per cell).
   - All rows: `verify=OK`, `write_ms > 0`, `read_ms > 0`,
     `bytes_per_sec_* > 0`.
   - Topology fields match (`topology=realnet-3host`,
     `client_host=dreamer`, `ps_host=adept`,
     `ds_mds_host=shadow`).
4. Run again with `--variants a` and verify all rows have
   `verify=SKIP, note=needs_plain_mds_bringup`.

### Unit tests

None.  The harness is pure orchestration -- no per-step logic
that warrants unit-test coverage beyond what the shell
interpreter already enforces.  Real validation is the smoke
above + the future full-sweep numbers.

## Failure modes covered

| Failure                          | Detection                                    | Behaviour |
|----------------------------------|----------------------------------------------|-----------|
| PS listener not reachable       | `nc -zv ADEPT_LAN_IP 4098` fails             | exit 1 with "run-realnet-bringup.sh first" |
| Mount fails                      | non-zero `mount` exit                        | exit 1 with mount stderr |
| `dd` input-gen fails             | non-zero exit on /dev/urandom dd            | row with `verify=FAIL, note=input_gen_failed_<rc>` |
| `dd write` fails                 | non-zero exit                                | row with `verify=FAIL, note=dd_write_failed_<rc>` |
| `dd read` fails                  | non-zero exit                                | row with `verify=FAIL, note=dd_read_failed_<rc>` |
| `cmp` reports diff               | `cmp` non-zero                               | row with `verify=FAIL, note=bytes_mismatch` |
| `reffs-probe sb-set-default-coding` fails | probe exit non-zero | row with `verify=FAIL, note=set_default_coding_failed` for every cell in the affected (codec, geom) iteration |
| `drop_caches` fails              | sudo tee non-zero (typically permission)     | note column gets `drop_caches_failed`; verify stays OK; read timing is cache-influenced |
| Result line stripped / non-numeric | regex `^[0-9]+$` fails in emit_row          | row with `verify=FAIL, note=parse_failed` (belt-and-suspenders guard) |

## Deferred / NOT_NOW_BROWN_COW

- **Variants a/b/c implementations.**  Blocked on plain-MDS
  realnet bringup follow-up.  Stubbed with `verify=SKIP` so
  full-matrix runs already record the right cell IDs.
- **Multi-PS variant d** (NPS > 1).  The realnet bringup is
  single-PS by design.  Multi-PS would test PS-side
  parallelism, which is orthogonal to the variant-d-vs-c
  question and best done as a separate experiment.
- **Auto-bringup.**  Operator runs `run-realnet-bringup.sh`
  before invoking the harness.  Composing the two is the
  domain of a future driver script.
- **HTML report generation.**  `gen_benchmark_report.py`
  already exists; ensure the CSV column-superset is preserved
  so it Just Works on realnet data.  Touch the report
  generator only if the realnet smoke surfaces a real
  formatting gap.
- **Bench harness on dreamer's macOS side** (no benchmarks ever
  run on Darwin -- dreamer's Fedora VM does the kernel-mount
  work; macOS only orchestrates via ssh).
- **A `realnet_merge_csv.py`** for stitching multi-invocation
  outputs.  Easy to add when the operator actually does
  multi-day runs.

## First smoke observation (2026-06-04)

Initial 4-cell smoke (rs/4+2/{4 KB,1 MB}/2 iters) against the
live realnet topology returned:

```
variant,codec,geometry,size_bytes,iter,write_ms,read_ms,verify
d,rs,4+2,4096,1,61915,10487,OK
d,rs,4+2,4096,2,61912,10383,OK
d,rs,4+2,1048576,1,61999,10458,OK
d,rs,4+2,1048576,2,61998,10445,OK
```

Two things to note for downstream analysis:

1. **Write timings are uniform across sizes** (~62 s for both
   4 KB and 1 MB).  That fixed-cost shape, plus the `conv=fsync`
   that forces the COMMIT into the measured window, points at
   a per-write COMMIT-side delay rather than data-rate
   throughput.  A manual `time dd ... conv=fsync` on dreamer
   reproduces the same ~62 s -- the harness is faithfully
   measuring wall-clock end-to-end; the latency lives in the
   PS-MDS COMMIT path or the PS's per-stripe buffer flush.
   Investigating that is downstream of this slice.
2. **Read timings are also uniform** (~10.4 s) regardless of
   size, suggesting similar per-op overhead dominates the
   real-network round-trip.  Either codec-decode buffering or
   the PS<->DS session steady state.

Neither is a harness bug; they ARE the first variant-d realnet
data points and the bench is correctly recording them.  Slide 22
on `ietf126.md` will land with the actual measured numbers (and
a "why so slow" annotation) once the follow-up triages PS write
latency on realnet.

## Reference

- `.claude/design/ps-encoder-bench-4variant-realnet.md` -- the
  4-variant design this slice unblocks (prereq #4).
- `.claude/design/multi-host-bench-bringup.md` -- the prereq #3
  bringup that this harness drives against.
- `deploy/benchmark/run_ps_vs_client_bench.sh` -- the
  single-host A/B/C harness this slice's CSV schema and timing
  model derive from.
- `.claude/design/per-export-default-coding.md` -- the probe op
  the harness uses to switch codec/geometry between cells.
