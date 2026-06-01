<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# PS-encoder vs client-encoder benchmark (IETF 126 bucket 2)

## Goal

Produce a head-to-head benchmark that lets us answer the WG
position that encoding should be in the server (not the client)
with numbers rather than rhetoric.  The IETF 126 plan
(`~/Documents/reffs-docs/ietf126-plan.md` bucket 2) identified
this as the highest-leverage missing data point for the 6-week
run-up.

Companion to:

- `proxy-server.md` -- defines the PS surface this benchmark
  exercises (Phase 4 shipped).
- `chunk-collision-validation.md` -- shares the docker-bench
  topology (1 MDS + 10 DSes + N PSes via
  `run-ps-bench-bringup.sh`).
- `~/Documents/reffs-docs/ec_benchmark_full_report.md` -- the
  existing single-variant benchmark report that this slice
  extends.

Some citations point at `~/Documents/reffs-docs/` paths
(private to the author's workstation, not redistributed with
reffs).  External readers can ignore those; the in-tree
companion docs above are sufficient to execute this slice.

## What we measure

Three workload variants against the same MDS + DSes:

| Variant | Path | What encodes |
|---|---|---|
| **A** -- client EC | `ec_demo write/read --mds reffs-mds` (existing) | Client (ec_demo) encodes; CHUNK_WRITE direct to DSes |
| **B** -- PS EC | `dd` or `fio` against an NFSv4.2 mount at PS :4098 | PS encodes; CHUNK_WRITE from PS to DSes |
| **C** -- MDS inband | `dd` or `fio` against an NFSv4.2 mount at MDS :2049 (no layout) | No EC; MDS proxies READ/WRITE inband to one mirror DS |

Each variant runs the **same** 5-size grid (4 KB, 16 KB, 64 KB,
256 KB, 1 MB) and the **same** geometry (RS 4+2, the bench
report's primary cell).  Output is one CSV row per (variant,
size, op, iteration), so the report-generation step in the
shipped bench tooling can ingest it without bespoke parsing.

Mojette / striping / scalar / SIMD axes are **out of scope** for
this slice -- they multiply the cell count without changing the
answer to "where does encoding belong".  Restrict to RS 4+2,
SIMD on (NEON or AVX2 by host).

## Why these three and not more

- **A vs B is the load-bearing comparison.**  Both produce EC
  on disk; the question is who pays the encode cost.  Variant B
  also pays the *extra* NFSv4 hop (client -> PS over the wire),
  so A is the lower-bound for "EC is fast" and B is the
  upper-bound for "PS-mediated EC is acceptable."
- **C is the no-EC cost floor.**  No EC at all -- whatever the
  MDS does inband to the first available mirror.  C is the "do
  nothing" floor for cost comparison; A and B both pay an EC
  cost above it.  Note that variant C is NOT what the
  server-side-encoding WG position argues for; that position
  argues for variant B.  Variant C exists only to bound the
  cost of EC itself.
- **No "client without EC" variant.**  A plain NFSv4.2 client
  doing `dd` to the MDS is variant C; ec_demo without `--codec`
  goes through `cmd_put` (the plain-mirrored path) which is
  semantically equivalent to C.  Adding it would double the
  row count without surfacing a new comparison.

## Topology

Reuse the existing 1 MDS + 10 DS + N PS docker-compose +
`run-ps-bench-bringup.sh`.  Set `NPS=1` -- the client traffic
all flows through one PS for variant B, matching the typical
"one PS per host" deployment shape from `proxy-server.md` (the
N-PS topology exists for chunk-collision Track 2, not for
throughput).

Client work runs from a kernel-NFS-client container (host
networking) that mounts:

- `127.0.0.1:/` over PS port `4098` for variant B.
- `127.0.0.1:/` over MDS port `2049` for variant C.

Variant A runs ec_demo directly against the MDS (no kernel
mount needed; ec_demo is its own client).

## Per-cell measurement protocol

For each (variant, size) cell:

1. Pre-create the test file at the target size (so the timed
   write isn't dominated by namespace allocation).  Pre-create
   uses the **same** variant's path -- this gates the encoder
   bringup state into the measured cell.
2. Drop client caches (`echo 3 > /proc/sys/vm/drop_caches`) and
   server caches (probe op or restart cycle).
3. Run N iterations (default 5) of the operation.  For write,
   `dd if=/dev/urandom of=<mount>/<file> bs=<size> count=1
   conv=fsync`.  For read, `dd if=<mount>/<file> of=/dev/null
   bs=<size>`.
4. Record per-iteration wall-clock (ms) into a CSV row:
   `variant,size,op,iter,ms`.
5. Median + p95 are aggregated post-run, mirroring the existing
   `ec_benchmark_full_report.md` reporting style.

Total cells: 3 variants * 5 sizes * 2 ops * 5 iter = 150 rows
per topology run.  At ~1 second per iteration (worst case)
this is ~3 minutes of actual measurement plus topology bringup.

## Cells INV-1 should explain

The chunk-collision INV-1 counter family (shipped in
`chunk-collision-validation.md` BLOCKER 2) gives us a way to
audit "what work did each variant actually do":

- Variant A: `sb_chunk_writes` increments per CHUNK_WRITE
  from ec_demo client.
- Variant B: same counter increments per CHUNK_WRITE from PS.
- Variant C: `sb_chunk_writes` stays at zero; MDS inband
  writes go through `dstore_ops_*` paths, not CHUNK ops.

The harness snapshots the counters at run start and run end,
so any variant whose counter delta is suspicious (e.g.,
variant B showing 2x the writes of variant A) is flagged for
investigation before the numbers go on a slide.

**Threshold for "suspicious":** for the bucket-2 comparison
to be meaningful, variant A and B at the same (size, op) cell
must agree on `sb_chunk_writes` delta to within +/- 5% (the
ec_demo write fan-out and the PS-mediated write fan-out emit
the same number of CHUNK_WRITE ops by construction).  If the
deltas diverge more than that, root-cause before reporting
medians.  Variant C must show zero `sb_chunk_writes` delta
(no CHUNK ops on the inband path).

## Risks

1. **PS Phase 4 was smoke-tested for chunk-collision Track 2
   but never sustained-benchmarked.**  May surface a soft edge
   under the variant-B load -- write-buffer pressure, session
   lifecycle, slot exhaustion.  If it does, the first run's
   "PS encoder is slow" data is a Phase 4 bug, not a fair
   comparison.  Block on root-cause before reporting.

2. **Variant C may not exist cleanly.**  NFSv4.2 mount of MDS
   :2049 with a file that *has* an EC layout will negotiate the
   layout and the client will encode (turning C into A).  To
   keep C as "no EC," the test file must be in an export with
   no layout configured, or the client must refuse the layout.
   The bench config will set up a `/no-ec` export with
   `sb_ndstores = 0` so LAYOUTGET returns
   `NFS4ERR_LAYOUTUNAVAILABLE` and the kernel client falls
   back to inband I/O.  Setup script must verify this.

3. **`dd` vs `fio`.**  `dd` is simpler and `bs=` matches the
   variant-A I/O size cleanly; `fio` gives latency
   distribution.  Default to `dd` for the first pass; if the
   p95 numbers matter for the slide, swap in `fio --bs
   --size --iodepth=1 --rw=write --thread=1`.  The
   container-side driver in the harness should make this a
   one-line swap.

4. **Sample count.**  N=5 is what the existing bench report
   uses.  For a "PS vs client" slide that's directional we
   probably want N=10 to bound noise -- pull the threshold
   from the existing report's measurement-noise discussion
   (it cites ~7-10ms noise floor at the 1 MB cell).

## Deliverables

- `deploy/benchmark/run_ps_vs_client_bench.sh` -- harness
  scaffold (this slice).
- `.claude/design/ps-encoder-bench.md` -- this doc.
- CSV output: one file per topology run, ingestable by the
  existing report generator with a "variant" axis added.
- Slide: bucket-2 slide in `~/Documents/reffs-docs/ietf126.md`
  ("Where does EC encoding belong?") -- replace the
  parked-placeholder body with the three-variant comparison at
  the 4-64 KB and 1 MB cells, with the noise-floor band from
  `ec_benchmark_full_report.md` overlaid.

## What this slice does NOT do

- Variant B over **Kerberos**.  The PS does PROXY_REGISTRATION
  over mTLS in the demo topology; the *client* mount to the PS
  is AUTH_SYS for this bench.  Kerberos client-to-PS is a
  separate axis that doubles every cell.
- Multi-PS load balancing (NPS > 1).  One PS, one client.
- Repair / collision overlay.  This is throughput, not
  correctness.
- Production deployment numbers.  The bench runs in docker
  containers on one host; numbers are directional, not
  absolute.

## Implementation order

1. **Harness scaffold** -- write the three-variant driver
   skeleton in `deploy/benchmark/run_ps_vs_client_bench.sh`.
   Reuse `run-ps-bench-bringup.sh` for topology.  Output CSV.
2. **Variant C plumbing** -- add the no-EC export to the
   bench TOML (or set it up at runtime via the probe op).
   Verify LAYOUTGET returns `LAYOUTUNAVAILABLE` so the kernel
   falls back to inband.
3. **First smoke run** -- single (variant, size) cell to
   validate the path works end-to-end.  Look at the CSV row
   and the INV-1 counter deltas.
4. **Full 150-cell run** -- N=5 iterations.  Eyeball the medians.
5. **N=10 final run** -- when the numbers look stable.
6. **Slide** -- pull the medians into the bucket-2 slide
   already appended to `ietf126.md` (currently parked-with-
   placeholder).

Steps 3-6 happen on the dreamer / garbo Linux bench host, not
on Darwin.  Steps 1-2 can be drafted on Darwin (no execution
required to scaffold a shell script).

## Open questions

- **Should variant A use `ec_demo write` or `ec_demo bigfile`?**
  `write` is what the existing report measures; `bigfile`
  exercises the chunked-write path which is more representative
  of sustained workloads.  Default to `write` for direct
  comparability with the published numbers; `bigfile` as a
  follow-up if the comparison shows surprising deltas.
- **Should we capture INV-1 counter ratios on the slide?**
  Adds detail but also clutter.  Default: report counter deltas
  in the design-doc appendix; only put a single "PS does the
  same number of CHUNK_WRITEs as client" line on the slide.
