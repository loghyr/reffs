<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# PS-Encoder Bench: 4-Variant Real-Network Plan

## Why this exists

The 2026-06-02 single-host-loopback A/B/C harness (see
`ps-encoder-bench.md`) hit two scoping limits at once:

1. **Variant B doesn't actually do PS-side EC** because the
   MDS keys FFv2 codec selection off `ls_m`, which defaults
   to 0 (PASSTHROUGH) for files created over a kernel mount.
2. **Variant C doesn't actually do pNFS** because the bench
   DSes are NFSv4.2-only and the kernel's FFv1 fallback
   needs NFSv3 -- so the kernel falls back further to MDS-
   inband NFSv4.2 WRITE.

So the A/B/C dataset measures (A) client EC via ec_demo,
(B) PS proxying a plain-mirror layout, (C) plain MDS-inband
NFSv4.2 -- *not* the head-to-head "where does encoding
belong" comparison the WG asked about.

This document scopes the follow-on that *does* answer it.

## The four variants (per user direction 2026-06-02)

| # | Variant            | Wire format               | Encoder | Storage layout                      |
|---|--------------------|---------------------------|---------|-------------------------------------|
| **a** | kernel via FFv1 to 1 DS | NFSv4.2 OPEN + LAYOUTGET (FFv1) + NFSv3 WRITE to DS | none    | 1 DS, single mirror                 |
| **b** | kernel via FFv1 striped | NFSv4.2 OPEN + LAYOUTGET (FFv1, striped) + NFSv3 WRITE to (k+m) DSes | none    | k+m DSes, stripe (no parity)        |
| **c** | kernel via FFv2 to EC DSes | NFSv4.2 OPEN + LAYOUTGET (FFv2) + CHUNK_WRITE to (k+m) DSes | kernel  | k+m DSes, EC (RS or Mojette) |
| **d** | kernel via FFv2 to PS, PS encodes | kernel NFSv4.2 WRITE to PS; PS does LAYOUTGET (FFv2) + CHUNK_WRITE | PS      | k+m DSes, EC                        |

Variant (a) is the no-EC baseline.  Variant (b) is the
"parallel I/O without coding" baseline (RFC 5661 file
layout fan-out cost).  Variant (c) is "client does the
encoding."  Variant (d) is "server does the encoding."

(c) requires a kernel that speaks FFv2 + CHUNK ops.  The
Linux stock kernel does **not** -- FFv2 is draft-stage and
unimplemented upstream.  Two paths to a real (c) measurement:

1. **ec_demo as the client surrogate.**  It speaks FFv2.
   Variant (c) becomes ec_demo direct to MDS+DSes.  This
   is what variant A in the A/B/C harness already measures,
   so (c) is effectively in hand for the codecs ec_demo
   supports.
2. **A native FFv2 kernel client implementation.**  Out of
   scope for this experiment; it is its own multi-year
   project.

Variants (a) and (b) need the client to reach NFSv3 on the
DSes.  **NFSv3 is already running on the bench DSes**
(reffsd registers NFSv3 unconditionally; `ec_demo --layout
v1` confirms the path works).  The reason variant C in the
2026-06-02 single-host bench didn't engage pNFS is a network
*reachability* artefact: the `--network=host` client
container cannot resolve docker-compose service names that
the MDS puts in FFv1 layouts (`reffs-ds0`, etc.).  On
real-network topology (3 hosts or HS-lab VMs) the DSes have
routable IPs and the kernel resolves them naturally; no
ds.toml change is needed.

A single-host stop-gap for kernel-pNFS tests is recorded in
`[[reference_bench_ds_nfsv3_gap]]` (attach the client to
both the docker-compose network and `--network=host`, or
have the MDS issue IP-based deviceinfo instead of names);
neither is necessary if the experiment runs on real
hosts.

Variant (d) is what variant B in the A/B/C harness was
*supposed* to measure but couldn't because of the layout-
codec-control limitation.  Unblocking (d) requires the same
fix queued in `ps-encoder-bench.md` "Next slice: unblock
variant B" -- either `fattr4_layout_hint` SETATTR
(RFC 8881 attr 63) or a per-export `default_coding` TOML
field that sets `ls_m` at runway-pool-file creation.

## Topology options (per user direction)

The 2026-06-02 single-host bench masked PS-hop costs by
running everything on loopback.  Either option separates
the PS from the DSes onto distinct hosts; that is the
prerequisite for measuring the hop honestly.

### Option 1: 3 physical / virtual hosts

```
+---------+      +---------+     +-----------------+
| client  | ---> | PS host | --> | DS host         |
| (any)   |      |  +MDS?  |     | 1 MDS container |
+---------+      +---------+     | N DS containers |
                                  +-----------------+
```

- Host 1: client (Linux NFSv4.2 + ec_demo).  Mounts either
  PS (for variant d) or MDS (for variants a, b, c).
- Host 2: PS (single reffsd with `[[proxy_mds]]` config).
  Optionally co-locates the MDS or runs it on host 3.
- Host 3: docker-compose with 1 MDS + N DSes.  N >= 6
  (RS 4+2) or >= 10 (RS 8+2).

Network between hosts: 1 GbE or 10 GbE; doesn't matter for
the comparison shape, just for absolute numbers.

Pros:
- Minimum physical hardware.
- Mirrors the production deployment shape (PS as a separate
  service host).
- Each host's storage is independent (PS holds no data, DSes
  hold all data); no shared-fsync artifact.

Cons:
- DSes co-resident on host 3 still share storage fsync there.
  For separate-DS-storage measurement need option 2.

### Option 2: enough VMs in the HS lab

```
+--------+   +--------+   +-------+   +-------+ ... +-------+
| client |   |  MDS   |   |  PS   |   | DS 1  | ... | DS N  |
+--------+   +--------+   +-------+   +-------+     +-------+
```

- Each role on its own VM (or physical box).  N+3 boxes
  minimum (client, MDS, PS, N DSes).
- DSes on distinct storage (each VM's local disk, or distinct
  block devices).  Removes the shared-fsync artifact.
- Configurable network topology (same rack, different racks,
  WAN simulation).

Pros:
- True per-DS fsync parallelism.
- Configurable PS-to-MDS and PS-to-DS latencies for sensitivity
  analysis.
- Matches what a production reffs deployment would look like.

Cons:
- Needs N+3 VMs.  At N=6 (RS 4+2) that is 9 VMs; at N=10
  (RS 8+2) that is 13 VMs.
- More moving parts to bring up and keep consistent.

### Recommendation

**Start with option 1, validate with option 2.**

Option 1 closes the PS-on-different-host gap (the headline
correction this experiment is for) with minimum lab hardware.
Most of the WG-facing arguments about server-side encoding
are answered by option 1: client sees PS hop, PS sees MDS+DS
hops, both are real network round-trips.

Option 2 is the follow-on that closes the per-DS-storage
caveat -- needed if the data shows shared-fsync artifacts on
option 1's host 3.  Build the data-collection harness on
option 1 first; same harness drives option 2 with one config
swap (DSes per host == 1 instead of DSes per host == N).

## Cell matrix

For each variant in {a, b, c, d}, per (codec, geometry,
size, iter) tuple:

- Codec axis (variants c, d only; a, b have no codec):
  rs, mojette-sys, mojette-nonsys.  (mojette-nonsys is
  declared unsuitable for read-heavy workloads but
  recorded for completeness.)
- Geometry axis (variants b, c, d): 4+2, 8+2.
- Size axis: 4 KB, 16 KB, 64 KB, 256 KB, 1 MB, 4 MB, 16 MB.
- Iter: 5 per cell (median + spread).

Cell count:
- a: 7 sizes * 5 iter = 35 cells (no codec/geometry axes)
- b: 2 geom * 7 sizes * 5 iter = 70 cells (no codec axis)
- c: 3 codecs * 2 geom * 7 sizes * 5 iter = 210 cells
- d: 3 codecs * 2 geom * 7 sizes * 5 iter = 210 cells

Total: 525 cells.  Wall-clock at ~2 s/cell (real-network
latency adds to per-cell time): ~17 min sustained.  Tear-up
+ tear-down + measurement padding: budget 1 hour.

## Prerequisites

To run any of variants a, b, c, d on real-network topology:

1. **DS service must advertise NFSv3.**  Add NFSv3 listener
   to `ds.toml` (server-side change, ~no LOC).  Without this,
   variants (a) and (b) can't happen.

2. **MDS must issue per-codec FFv2 layouts.**  Currently
   `layout.c:610-613` keys coding type off `ls_m` only.
   Wire either:
   - `fattr4_layout_hint` SETATTR support (RFC 8881 attr 63)
     -- standard mechanism, needs server-side attribute
     handler + runway-side codec selector
   - per-export `default_coding` TOML field -- ~50 LOC,
     interim, no XDR change

   Without one of these, variant (d) is stuck at PASSTHROUGH.

3. **Bringup script that does 3 hosts / N+3 VMs.**  The
   existing `run-ps-bench-bringup.sh` is single-host docker.
   Need a multi-host orchestration; ansible / a small shell
   helper that ssh's to each box and runs the right reffsd
   command.

4. **Harness that drives the four variants per cell.**
   The existing `run_ps_vs_client_bench.sh` is a starting
   point but its variant naming (A/B/C) does not align with
   the new naming (a/b/c/d).  Build the new harness reusing
   the fio + ec_demo idioms; rename variants explicitly.

## Scheduling and dependencies

Items, in dependency order:

| # | Item | Effort | Blocker for |
|---|------|--------|-------------|
| 1 | *(WITHDRAWN 2026-06-02)*  NFSv3 listener is already on; no DS-side work needed. | -- | -- |
| 2 | Per-export `default_coding` TOML field (interim codec selector) | ~1 week | variant d |
| 3 | Multi-host bringup script (option 1: 3-host) | 2-3 days | all variants on real network |
| 4 | 4-variant harness script + CSV schema | 2-3 days | data collection |
| 5 | First real-network smoke run | 0.5 day | sanity check |
| 6 | Full 525-cell sweep | 1-2 hours per topology | deck slide |
| 7 | Option 2: HS-lab VM provisioning | 1-3 days | follow-on validation |
| 8 | Repeat sweep on option 2 topology | 1-2 hours | per-DS-storage check |

Total to first slide-ready numbers from option 1: **~10
working days** (was ~2 weeks before item 1 was withdrawn).
Total including option 2 validation: ~2.5 weeks.

Items 1-2 are reffs server changes (XDR-adjacent for item 2;
may need reviewer agent per CLAUDE.md gating rules); items
3-4 are deploy/bench scaffolding; items 5-6 are measurement
runs; items 7-8 are the follow-on validation pass.

## Honest scope on the deck

Slide 22 ("Where does EC encoding belong?") on
`ietf126.md` currently parks with "Bucket 2 -- parked --
numbers land week 2 of IETF 126 plan."  When this real-
network experiment lands, slide 22 gets:

- The 4-variant write/read latency table at 1 MB (the
  headline size).
- The variant-d-vs-variant-c head-to-head (server EC vs
  client EC at matched codec).
- Explicit topology note: which option (1 or 2), how many
  hosts, network type.
- A subordinate slide acknowledging that single-host loopback
  (the 2026-06-02 dataset) overstates the PS-is-free claim
  for the reasons documented in `ps-encoder-bench.md` Take 2.

What slide 22 will NOT claim from this experiment:
- That encoding belongs on the server.  The deck-stated WG
  question is "where does it belong"; the data answers
  "what's the cost trade-off."  Operational placement is a
  deployment choice that depends on the WG audience's own
  trust model.
- Comparable kernel-FFv2 numbers (no native kernel FFv2
  client; ec_demo is the surrogate).

## Out of scope for this design

- A native kernel FFv2 client (years-scale project).
- COPY-offload-shaped variants (covered separately by the
  Lever thread).
- Tiered storage (HDD DSes vs SSD DSes).
- Concurrent-writer variants (Track 1 / Track 1b / Track 2
  cover that axis).

## Reference

- `.claude/design/ps-encoder-bench.md` -- the original A/B/C
  scaffold and the 2026-06-02 single-host findings.
- `~/Documents/reffs-docs/experiments/ps_vs_client_bench-shadow-2026-06-02.csv`
  -- the 75-cell single-host single-codec sweep that
  surfaced the limitations addressed here.
- `lib/nfs4/server/layout.c:610-613` -- the `ls_m`-keyed
  PASSTHROUGH / RS_VANDERMONDE selection that variant (d)
  needs to unblock.
- `ds.toml` -- needs NFSv3 listener for variants (a) and (b).
