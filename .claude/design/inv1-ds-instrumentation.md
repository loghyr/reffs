<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# INV-1: DS-Side Write-Pattern Instrumentation

## Context

`experiments.md` Group B (IETF reviewer requests):

> **INV-1**: What I/O pattern does a partial-stripe write produce
> on the DSes, and how much fragmentation does it cause (DS
> filesystem + SSD FTL)?  Can partial-stripe writes be reduced
> to full-stripe writes cheaply?

Source: Hellwig msg 5 (in-place update semantics) + msg 9 (NFS
block size).  Haynes committed (msg 13) to answering "the
questions you have raised" using the reference implementation's
POSIX-DS interfaces.

INV-1 was gated on Track 2's first clean run; Track 2 PASSED
2026-05-20 (commit `109134da1af8`).  This design is now
**ungated**.

## What this slice answers

The on-list question reduces to four numbers per workload
(T1b, T2-write, T2-read+write, T2-shared-file):

1. **Partial vs full block ratio** -- of all blocks written
   to the chunk store, what fraction was a full `chunk_size`
   block vs a short tail block (`chunk_size > len > 0`)?
2. **First-write vs in-place overwrite ratio** -- of blocks
   written, what fraction targeted an EMPTY slot (first write)
   vs an existing PENDING/FINALIZED/COMMITTED slot (read-
   modify-write or retry)?
3. **Per-write block-count histogram** -- distribution of
   `cwa_chunks_len` per CHUNK_WRITE op (does the client batch
   16 chunks at a time, or 1?).
4. **End-of-workload fragmentation** -- across the entire DS
   chunk store, count of contiguous runs of non-EMPTY blocks
   separated by EMPTY gaps.  A defragmented file is 1 run; a
   shared-file workload with 4 PSes interleaving writes
   produces many more.

(1)-(3) are flow counters incremented on every CHUNK_WRITE.

(4) is a per-`chunk_store` sweep -- and the chunk_store lives
on the inode, behind `i_attr_mutex`.  Reading it lock-free from
the probe path would race `chunk_store_grow()` (which is
calloc+memcpy+free) and risk UAF.  So:

- Slice 1 ships `chunk_store_count_runs()` (a per-store sweep
  function) with unit tests for the math.  Its only caller in
  slice 1 is the unit test -- the wire field
  `pcs_fragmentation_runs` is plumbed but populated as 0.
- A follow-up slice adds a new probe op (e.g.
  `INODE_CHUNK_STATS`) that takes the inode lock, calls
  `chunk_store_count_runs()`, and returns the per-inode (and
  sb-summed) fragmentation number.  That op is the right home
  for any future per-inode breakdown anyway -- pairing it
  with fragmentation means we only build the inode-enumeration
  path once.

(1)-(3) alone answer Hellwig msg 5 and msg 9 directly.  (4)
strengthens the prose answer but is not strictly required for
the WG-list reply.

## Non-goals

- **Per-event tracing** (one log line per CHUNK_WRITE).  Too
  much data; not what the WG list needs.  If we ever do need
  this, it goes through the existing TRACE framework, not a
  separate path.
- **DS-filesystem-level fragmentation** (xfs_db `frag`, ext4
  `e4defrag -c`, etc.).  Out of scope for the reffs-side
  measurement -- it's a host-fs concern Christoph already
  knows the shape of.  The PS-FTL story for the WG response
  will cite `filefrag` output captured by the harness, not
  instrumented inside reffs.
- **Per-inode breakdown**.  The probe is per-sb, summed over
  all files in that sb.  A per-inode probe op is a follow-up
  (NOT_NOW_BROWN_COW).

## Tests first

### Existing tests affected: NONE

All changes are additive:

- New fields are appended to `probe_chunk_stats1` (probe is
  internal-only, client+server ship together -- standards.md
  reviewer rule 9 carve-out).
- New counters incremented in `nfs4_op_chunk_write` next to
  the existing `cs_pending_displaced` site, same atomic pattern.
- New fragmentation sweep is read-only over `cs_blocks[]`
  with `cs->cs_lock` held (existing reader lock).

All current `make check` must continue to pass.

### New unit tests

**File**: `lib/nfs4/tests/inv1_instrument_test.c` (NEW)

| Test | Intent |
|------|--------|
| `test_inv1_full_block_counted` | CHUNK_WRITE with `cwa_chunks_len=1`, `len == chunk_size` -- `pcs_blocks_full` advances by 1, `pcs_blocks_partial` unchanged |
| `test_inv1_partial_tail_counted` | CHUNK_WRITE with two blocks where `total_data % chunk_size != 0` -- last block counted as partial, first as full |
| `test_inv1_first_write_counted` | CHUNK_WRITE into an empty `chunk_store` -- `pcs_blocks_first_write += N`, `pcs_blocks_overwrite` unchanged |
| `test_inv1_overwrite_counted` | Two back-to-back CHUNK_WRITEs to the same offset (different owner so `cs_pending_displaced` also fires) -- second write counts as `pcs_blocks_overwrite`; both counters move in lockstep with the existing `cs_pending_displaced` axis |
| `test_inv1_batch_histogram` | Three CHUNK_WRITEs with `cwa_chunks_len = 1, 4, 16`.  Verify `pcs_writes_1block += 1`, `pcs_writes_2to7 += 1`, `pcs_writes_8to31 += 1` (power-of-two-ish bucket boundaries chosen below) |
| `test_inv1_fragmentation_one_run` | Fill blocks [0..15] PENDING.  Read `pcs_fragmentation_runs` -- expect 1 |
| `test_inv1_fragmentation_three_runs` | Fill [0..3], [8..11], [16..19] PENDING (EMPTY gaps).  Expect 3 |
| `test_inv1_fragmentation_zero_runs` | Empty store -- expect 0 |

All eight fit in the 2-second per-test budget (no I/O, just
in-memory state mutations).

### Harness wiring

The Track 2 harness (`scripts/run_chunk_collision.sh` for T1b,
the T2 PS-bench script for IOR) already calls
`reffs-probe.py sb-list` at end-of-run for the existing
`cs_pending_displaced` assertion.  The new fields show up in
the same response automatically once the wire format is
extended; the harness only needs to log them.

**Harness change** (Slice 1 follow-on, not part of the core
slice): add a `--inv1-report` flag to `run_chunk_collision.sh`
that prints the four numbers in a quotable two-line format
after the workload completes.

## Implementation plan

### Slice 1: counters + fragmentation sweep + harness flag

**Estimated size**: ~120-150 LOC of substantive change, plus
test file.  Single-file refactor under 150 LOC + tests, no XDR
new ops (only field append), no RCU change, no on-disk format.
Reviewer agent **not required** per standards.md gating --
inline review against standards.md is sufficient.  But the
fragmentation sweep touches a hot path that could regress
reads, so the slice should be benchmarked on the same harness
that produced the INV-1 numbers.

**Files**:

| File | Change |
|------|--------|
| `lib/xdr/probe1_xdr.x` | Append 7 fields to `probe_chunk_stats1`; rebuild |
| `lib/include/reffs/super_block.h` (or wherever `reffs_chunk_stats` lives) | Add 7 `_Atomic uint64_t` counter fields |
| `lib/nfs4/server/chunk.c` | Increment counters in the per-block loop of `nfs4_op_chunk_write` |
| `lib/nfs4/server/chunk_store.c` | Add `chunk_store_count_runs()` -- sweep `cs_blocks[]`, return run count |
| `lib/probe1/probe1_server.c` | Populate new `probe_chunk_stats1` fields in `fill_sb_info` |
| `lib/nfs4/tests/inv1_instrument_test.c` | 8 unit tests |
| `scripts/run_chunk_collision.sh` | `--inv1-report` flag prints quotable summary |

**Counter additions** (`probe_chunk_stats1` and the matching
in-process `reffs_chunk_stats`):

```c
unsigned hyper pcs_blocks_full;        /* len == chunk_size */
unsigned hyper pcs_blocks_partial;     /* 0 < len < chunk_size */
unsigned hyper pcs_blocks_first_write; /* prev cb_state == EMPTY */
unsigned hyper pcs_blocks_overwrite;   /* prev cb_state != EMPTY */
unsigned hyper pcs_writes_1block;      /* cwa_chunks_len == 1 */
unsigned hyper pcs_writes_2to7;        /* 2..7 */
unsigned hyper pcs_writes_8to31;       /* 8..31 */
unsigned hyper pcs_writes_32plus;      /* >= 32 */
unsigned hyper pcs_fragmentation_runs; /* end-of-sweep snapshot */
```

(That's 9, not 7 -- corrected.)  The four bucket boundaries
match the practical range observed in T2 IOR runs (1, 4, 16,
64 chunks per write) without over-segmenting.

**Memory ordering**: `memory_order_relaxed` on every counter
(matches the existing `cs_pending_displaced` site -- these are
diagnostic counters, not synchronization).

**Fragmentation sweep**: walk `cs->cs_blocks[0..cs_nblocks-1]`,
count transitions from EMPTY to non-EMPTY.  O(N) in chunk
count; for typical T2 runs (64 MiB / 4 KiB = 16384 blocks per
file) this is a few microseconds.  Called from `fill_sb_info`,
which is already off the data path.

### Slice 2 (deferred / follow-up)

- Per-inode breakdown via a new probe op `INODE_CHUNK_STATS`.
  Lets the harness report fragmentation per file rather than
  summed over the sb.  Useful for T1b's 8-file sub-chunk
  interleave story.
- Offset-distribution HdrHistogram.  The four bucket counters
  above are coarse; HdrHistogram would give the precise
  distribution.  Defer unless the WG response specifically
  asks for it.

## How the numbers get cited

The WG-list response will quote the four ratios from a fresh
T2 run plus a T1b sub-chunk-interleave run:

```
T2 (IOR -F 0, 4 PSes, 4+2 RS, 4 KiB chunks):
  blocks_full / blocks_partial      = 16380 / 4       (>99.9% full)
  blocks_first_write / overwrite    = 16384 / 0       (no in-place RMW)
  writes_1block / 2-7 / 8-31 / 32+  = 0 / 2048 / 0 / 0 (every write is 4 chunks)
  fragmentation_runs                = 1               (contiguous file)

T1b (sub-chunk interleave, 8 writers, 4 KiB chunks):
  blocks_full / blocks_partial      = 14336 / 2048
  blocks_first_write / overwrite    = 8192 / 8192    (RMW under contention)
  writes_1block / 2-7 / 8-31 / 32+  = 8192 / 0 / 0 / 0
  fragmentation_runs                = 24             (per file, 8 files)
```

(Numbers above are illustrative -- the actual run will produce
the citable values.)

The prose answer then explains the partial-write story: in T2
the partial-stripe path turns into full-chunk writes (because
the PS encodes a full stripe before issuing CHUNK_WRITE), and
the RMW that Christoph was worried about does not appear on
the DS at all.  T1b is where the RMW shows up, because there
the harness deliberately drives sub-chunk byte interleave to
exercise the contention path.

## Risks

- **Counter overhead on hot path**.  Two atomic increments per
  block (full/partial + first/overwrite).  Already paying one
  per block (`cs_pending_displaced`); two more is in noise but
  the slice should benchmark to confirm.
- **Fragmentation sweep cost on a huge sb**.  A sb with many
  large files has a long sweep.  Mitigation: the sweep runs
  from the probe path, off the data path.  If it becomes a
  problem, cache the result and invalidate on CHUNK_WRITE
  (NOT_NOW_BROWN_COW).
- **None on wire format**.  Probe XDR is internal; appending
  new fields is the documented carve-out.

## Cross-references

- [`experiments.md`](experiments.md) -- INV-1 ungated entry,
  Group B
- [`chunk-collision-track2.md`](chunk-collision-track2.md) --
  Track 2 harness that hosts the measurement
- [`chunk-collision-validation.md`](chunk-collision-validation.md)
  -- the original `cs_pending_displaced` slice that
  established the counter-append pattern
- `reffs-docs/christoph.md` (private) -- the question and the
  on-list commitment
