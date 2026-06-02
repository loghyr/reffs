<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# PS-Encoder vs Client-Encoder A/B/C Benchmark

## Context

Christoph Hellwig (IETF 124, msg of 2026-05-15) and David Black
(same thread) argued that "EC encoding should be on the server,
not on the client."  The reffs FFv2 proxy-server role
(`draft-haynes-nfsv4-flexfiles-v2-proxy-server`) is the
server-side-encoding answer: a registered peer of the MDS that
takes client writes and emits CHUNK_WRITE on the client's
behalf.  Phase 4a + 4b of the PS shipped in 2026-05, so the
data path works end-to-end (see
`.claude/design/proxy-server-phase4a.md` and
`proxy-server-phase4b.md` for the shipped-slice tables).

What we cannot do yet is **quantify the cost trade-off** that
WG members are arguing about.  This harness measures that.

## Variants

Three workload paths against the same MDS + DS topology:

| Variant | Path | What it measures |
|---|---|---|
| **A** "client EC" | `ec_demo write/read` direct to MDS:2049 + DSes:NFSv3 | Today's measured path.  The "encoding on the client" position. |
| **B** "PS EC"     | Kernel NFSv4.2 mount of PS:4098; fio write+verify | The "encoding on the server" position.  PS receives plaintext from the client, encodes, fans CHUNK_WRITE out to the DSes. |
| **C** "MDS inband" | Kernel NFSv4.2 mount of MDS:2049; fio write+verify | No-EC cost floor.  MDS handles READ/WRITE inband against its own local backend; no fan-out, no encoding.  This is **not** what the server-side-encoding position argues for -- it is included as the floor to show what the encoding cost on top of plain NFSv4.2 looks like. |

Variant A is the existing baseline.  Variant B is what the WG
wants quantified.  Variant C bounds the question from below
(plain NFSv4.2 cost without EC).

## Cells

For each (codec, geometry, size, variant) tuple we run N
iterations (default 5) and record:

- `write_ms` -- wall-clock write latency including final sync
- `read_ms` -- wall-clock read latency
- `verify` -- OK or FAIL (bytes match what we wrote)

Codec / geometry axis (applies to variants A and B; variant C
has no codec axis):

- Codec: `rs`, `mojette-sys`, `mojette-nonsys`
- Geometry: `4+2`, `8+2`

Size axis (applies to all three variants):

- 4 KB, 16 KB, 64 KB, 256 KB, 1 MB

Matrix size: 3 codecs * 2 geom * 5 sizes * 5 iter * 2 variants
(A, B) = 300 cells for A+B; plus 5 sizes * 5 iter * 1 variant
(C) = 25 cells.  **325 cells total.**  At ~1 s per cell that
is ~5 min wall clock, ignoring container start-up.

## MVP scoping decision: operator-configured MDS codec for variant B

The PS uses whatever codec the MDS specifies in the layout it
returns at LAYOUTGET time -- the PS does not choose.  Three
options for handling that in the harness:

1. **Operator-configures MDS codec; harness assumes it** -- the
   harness takes a `--codec` argument, passes it to ec_demo for
   variant A, and assumes the operator has already configured
   the MDS to issue that codec for variant B's test files (via
   per-export TOML or via probe ops set out of band).
   Single-codec-per-run; loop externally for the full matrix.

2. **Harness orchestrates MDS codec via probe ops between
   cells** -- needs a probe op for per-export codec selection
   (does not currently exist).

3. **Use ffv2_layouthint4 per-file from the client side** -- the
   Macklem thread item.  Cleanest long-term; deferred.

**Choice for this design: option 1 (MVP).**  Smallest
deliverable that closes the WG question with real numbers.  The
harness drives one (codec, geometry) per invocation; an outer
loop in CI sweeps the full matrix.  Operator pre-configures the
MDS to issue the named codec for variant B's test files via
existing `[[export]]` TOML.

Followups:
- Option 2 is a probe-protocol extension (`SB_SET_CODEC` op);
  separate slice once the layouthint XDR for option 3 lands and
  we know what knobs make sense at the probe layer.
- Option 3 (`ffv2_layouthint4` per-file) is tracked in the
  Macklem-striping-attribute-hint thread; see
  `ietf126-plan.md` Bucket 3a action items.

## Workload driver: fio for variants B + C

Match the existing `run_chunk_collision_track2.sh` idiom: fio
with `--rw=write`, `--bs=$SIZE`, `--do_verify=1`, `--ioengine=
psync`.  Single-process per cell (no `--numjobs`) for
single-writer latency comparability with ec_demo.

Per-cell fio invocation shape:

```bash
fio --name=ps-bench --filename=/mnt/ps/test-<cell-id>.dat \
    --rw=write --bs=${SIZE} --size=${SIZE} \
    --verify=crc32c --do_verify=1 \
    --create_on_open=1 --allow_file_create=1 \
    --thread=0 --ioengine=psync \
    --output-format=json
```

Wall-clock latency comes from the JSON output's
`jobs[0].write.lat_ns.mean` and `read.lat_ns.mean`.  The
harness extracts those, divides by 10^6, and writes the cell.

For variant A, ec_demo is invoked with the same `--codec
--k --m --layout v2 --shard-size 4096` arguments today's
ec_benchmark.sh uses.  Timing is the wall-clock around the
ec_demo command (read 'real' from `time -p`).

## Topology

The existing `run-ps-bench-bringup.sh` brings up:

- 1 MDS container
- 10 DS containers
- N PS containers (N=1 for this bench)

PS 0 listens on host port 4098.  MDS listens on host port 2049
for inband I/O (variant C).

The harness:

1. Bring up bench with `NPS=1` via `run-ps-bench-bringup.sh`.
2. Launch the fio client container with `--network=host`,
   privileged, with kernel-NFSv4.2 mount support.
3. Kernel-mount PS:4098 at `/mnt/ps` and MDS:2049 at `/mnt/mds`.
4. Per cell: run the variant's command, capture timing, append
   to CSV.
5. Tear down container; leave bench up for re-runs.

## CSV output

```
variant,codec,geometry,size_bytes,iter,write_ms,read_ms,verify,note
A,rs,4+2,1048576,1,113,80,OK,
B,rs,4+2,1048576,1,128,95,OK,
C,-,-,1048576,1,116,95,OK,
```

`note` carries any anomalies (mount failure, fio error, etc.).
`-` in codec/geometry for variant C indicates the axis does not
apply.

## Success criteria for the scaffold

1. Harness runs end-to-end for one (codec, geometry) tuple and
   produces a CSV with all three variants populated for the
   full size sweep.
2. Verify column reads OK for every cell.
3. Wall-clock numbers are plausible (variant C is the floor;
   variants A and B are within an order of magnitude of variant
   C; variant A roughly matches today's published ec_demo
   numbers).
4. Tear-down is clean: no leftover containers, no leftover
   mounts.

## Out of scope for the scaffold

- Automated MDS codec orchestration (NOT_NOW_BROWN_COW per the
  scoping decision above).
- Multi-iteration averaging beyond N=5 per cell (the harness
  supports `--iters` but cell-to-cell variance is a separate
  question for a future slice).
- Concurrent writers / readers (the chunk-collision harnesses
  cover that axis; this bench is single-writer-latency).
- Cross-host LAN measurement (the bringup script assumes a
  single Linux host).

## Implementation files

| File | Role |
|------|------|
| `.claude/design/ps-encoder-bench.md` | This document |
| `deploy/benchmark/run_ps_vs_client_bench.sh` | Harness scaffold |
| `deploy/benchmark/results/ps_vs_client/` | CSV output landing place |

## Test plan

The harness itself is shell + fio + ec_demo + docker.  The unit
tests live in the individual layers below (PS unit tests,
ec_demo unit tests, MDS unit tests).  The harness is validated
end-to-end:

1. **Smoke run**: one (codec=rs, geometry=4+2, size=1 MB,
   iter=1) cell across all three variants.  CSV has 3 rows;
   verify=OK on all; latencies in plausible band.
2. **Single-codec full sweep**: one (codec=rs, geometry=4+2)
   tuple, full size sweep, iter=5.  CSV has 5 sizes * 5 iter *
   3 variants = 75 rows.
3. **Full matrix** (manual orchestration, outer loop sweeps
   codec/geometry): 325 rows.  Reserved for the actual
   bench-data-collection slice, not for the scaffold delivery.
