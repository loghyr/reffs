<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Option B: delta-parity wire-format for sub-stripe RMW parallelism

## Context

Track 1b chunk-collision validation shipped Option C
(`write_chunk_guard4` CAS) in commit `03d91554a34c` and
follow-ons through `d3ba0342e008`.  Option C makes sub-stripe
RMW correct by **serialising** on contended chunk-blocks:
concurrent writers race for the same byte range, one wins the
CAS, the other gets `NFS4ERR_DELAY` and retries against the
winner's committed state.  Disjoint sub-stripe ranges across
different DATA shards proceed in parallel; the **parity shards**
are the serialisation bottleneck because every sub-stripe writer
must update every parity shard.

For HPC `IOR -F 0` shared-file workloads with N writers each
writing different data shards, Option C caps parallel throughput
at "one writer at a time on every parity shard."

Option B decomposes the parity update so multiple writers can
XOR-merge their parity contributions in parallel.  Same RAID-5
RMW pattern that software-RAID has used for decades: read OLD
data, compute delta, send delta-parity per parity shard, server
XORs deltas into existing parity blocks.

This document captures the wire-format design space at the point
of "explained, not committed."  Implementation gating + IETF
draft revision both block on a green-light from the draft author.

## Math (systematic Reed-Solomon)

For a 4+2 systematic RS layout, parity shard `j` is:

```
P_j = sum_{i=0..k-1} (alpha_ij * D_i)
```

When a writer changes byte range `[r0, r1)` of data shard `D_i`
from `D_i` to `D_i'`:

```
delta_D_i  = D_i'[r0:r1) XOR D_i[r0:r1)      (zero outside [r0:r1))
delta_P_j  = alpha_ij * delta_D_i              (GF(2^8) mult per byte)
P_j'[r0:r1) = P_j[r0:r1) XOR delta_P_j
P_j' elsewhere = P_j elsewhere                 (unchanged)
```

XOR commutes: two writers with non-overlapping `delta_D_i`
contributions to the same parity shard can both successfully
apply their deltas independently, and the resulting parity is
identical to what a serialised re-encode would produce.

## Two new wire facts

Option C's CHUNK_WRITE today carries:
- `cwa_offset` (block offset, in chunk-size units)
- `cwa_chunk_size`
- `cwa_chunks<>` (opaque payload, whole-block absolute bytes)
- `cwa_owner.co_guard` + `cwa_guard` (for the CAS)

Option B adds:

1. **Dirty-range annotation**: a per-chunk sub-range tells the
   server which bytes of the block this CHUNK_WRITE actually
   touches.  Without this, partial-shard writes are wire-
   indistinguishable from whole-block writes.

2. **Payload-kind selector**: a flag bit tells the server
   whether `cwa_chunks` is absolute bytes (overwrite) or an XOR
   delta (apply with `^=`).  Without this, the server has no
   way to know which operation to perform.

Both interact with the existing `cwa_guard` CAS, just at a
finer scope.

## Concrete XDR sketch

Minimal additions in `lib/xdr/nfsv42_xdr.x` (and the matching
draft section):

```xdr
/*
 * Per-chunk dirty-range annotation.  Parallel array to
 * cwa_chunks: when cwa_dirty_ranges is present, entry [i]
 * applies to cwa_chunks[i].  When absent (empty array),
 * payloads cover the whole cwa_chunk_size block -- the
 * legacy wire shape.
 */
struct chunk_dirty_range4 {
        uint32_t   dr_byte_off;    /* offset within the
                                      cwa_chunk_size block */
        uint32_t   dr_byte_len;    /* number of bytes;
                                      cwa_chunks[i] payload
                                      MUST be exactly this
                                      length when present */
};

/*
 * Kind of payload.  Wire-encoded via cwa_flags bit, not a
 * standalone field, so the cost of the new feature is zero
 * bytes on the wire for legacy whole-block absolute writes.
 */
enum chunk_write_kind4 {
        CHUNK_WRITE_ABSOLUTE  = 0,  /* legacy: payload IS the new bytes */
        CHUNK_WRITE_XOR_DELTA = 1   /* payload XORs into existing bytes */
};

/*
 * cwa_flags bit assignments.  Existing bit stays at 0x01;
 * two new bits indicate the OPTIONAL trailing fields are
 * present.  Servers without delta-parity support reject
 * CHUNK_WRITE_FLAGS_DELTA_PAYLOAD with NFS4ERR_NOTSUPP at
 * compound dispatch time.
 */
const CHUNK_WRITE_FLAGS_ACTIVATE_IF_EMPTY = 0x00000001;
const CHUNK_WRITE_FLAGS_DIRTY_RANGES      = 0x00000002;  /* NEW */
const CHUNK_WRITE_FLAGS_DELTA_PAYLOAD     = 0x00000004;  /* NEW */

struct CHUNK_WRITE4args {
        /* CURRENT_FH: file */
        stateid4               cwa_stateid;
        offset4                cwa_offset;
        stable_how4            cwa_stable;
        chunk_owner4           cwa_owner;
        uint32_t               cwa_payload_id;
        uint32_t               cwa_flags;
        write_chunk_guard4     cwa_guard;
        uint32_t               cwa_chunk_size;
        checksum4              cwa_checksums<>;
        opaque                 cwa_chunks<>;

        /*
         * Empty unless cwa_flags & CHUNK_WRITE_FLAGS_DIRTY_RANGES.
         * MUST have the same length as cwa_chunks when present;
         * dr_byte_off + dr_byte_len <= cwa_chunk_size per entry.
         */
        chunk_dirty_range4     cwa_dirty_ranges<>;            /* NEW */
};
```

The `chunk_write_kind4` enum is in the protocol vocabulary for
the draft text; on the wire, the flag bit does the
selection in one bit instead of a 32-bit enum.

## Capability negotiation

Servers MUST NOT silently accept unknown `cwa_flags` bits.
The draft section MUST require either:

- a new EXCHANGE_ID server-capability flag advertising the
  feature, with clients required to check it before sending; OR
- a per-op test (server returns `NFS4ERR_NOTSUPP` on unknown
  flag bits, client retries with the bits cleared)

The EXCHANGE_ID path is cleaner and matches how
`EXCHGID4_FLAG_USE_PNFS_DS` etc. are negotiated.  The per-op
path is more forgiving of layered deployments.  Default
recommendation: EXCHANGE_ID capability flag plus a server-side
hard reject for delta + dirty-range flags from a
non-negotiating client.

## Composition with `cwa_guard` CAS

The CAS scope shrinks to the dirty range:

- **Data shard, absolute partial write**: `cwa_guard` still
  applies.  Mismatch on the read-time version still fails the
  write.  Disjoint sub-ranges within the same block from
  different writers can proceed in parallel (each writer's
  guard covers its own dirty range).

- **Parity shard, delta payload**: `cwa_guard` semantics
  change.  XOR commutes, so the server can apply ANY writer's
  delta without requiring CAS on the parity block.  The CAS-
  miss-as-retry-signal still fires for data shards; if a
  data-shard write rejects, the writer's matching parity-shard
  delta is never sent (data shard write fails first; the
  pipeline bails).

This means delta-parity writes can be sent with
`cwa_guard.cwg_check = FALSE` -- the XOR semantics guarantee
correctness independent of arrival order, as long as the
data-side guards still catch concurrent overlapping range
writes.

## Server-side bookkeeping

Two implementation choices for `struct chunk_block` in
`lib/nfs4/server/chunk_store.h`:

**Choice A: per-block state, per-range pending tracking.**
Block state machine stays PENDING / FINALIZED / COMMITTED at
the block granularity.  Add a list of pending sub-ranges to
each block; FINALIZE collapses all pending ranges from a given
writer into the FINALIZED state.  Disjoint range writers
proceed in parallel; same-range writers serialise via the CAS.
Simpler, less concurrency on the parity-merge case.

**Choice B: per-range state, sub-range PENDING tracking.**
Track each `[byte_off, byte_len)` slice as its own state-machine
unit within the block.  Maximum concurrency but substantial
bookkeeping.

Choice A is the right starting point.  Choice B is the
"if-this-becomes-the-bottleneck" follow-on.

## CHUNK_READ symmetry

Readers don't need a new wire shape:

- The server applies XOR-deltas to its on-disk parity bytes
  before responding to any CHUNK_READ for that range.  Readers
  see post-merge bytes only.

- Option C's `NFS4ERR_DELAY-on-PENDING` rule still applies: if
  the block has in-flight deltas from a writer that hasn't yet
  FINALIZED, the read returns DELAY and the client retries.

## Codec coverage

**Systematic Reed-Solomon (RS 4+2, RS 8+2, etc.)**:
straightforward.  The Vandermonde coefficient row per parity
shard times the dirty-range XOR delta gives the parity
contribution.  Textbook-backed.

**Mojette systematic**: the projection-bin geometry doesn't
map 1:1 to data offsets, so the naive "XOR delta into existing
parity at the same byte range" approach doesn't work directly.
An internal proposal exists for a Mojette-specific delta
construction (project the delta-`D_i` into the appropriate
projection bins, then XOR those bin contributions into the
existing parity shards).  Math has not been published in the
project's source headers yet; treat the Mojette delta path as
**research-track** until the math is written down.

**Mirrored (FFV2_ENCODING_MIRRORED)**: not affected.  Mirrored
writes are absolute by construction; no parity to merge.

The minimum-viable first revision of Option B can ship
systematic-RS only, gate `CHUNK_WRITE_FLAGS_DELTA_PAYLOAD` on a
codec match in the LAYOUTGET response, and add Mojette delta as
a follow-on once the math is reviewed.

## Where this lands in the draft

`draft-haynes-nfsv4-flexfiles-v2` already has
`sec-CHUNK_WRITE` + `sec-write_chunk_guard4`.  Option B adds:

- `sec-chunk-dirty-range` — `chunk_dirty_range4`, the
  parallel-array semantics, the legal-range MUSTs.
- `sec-delta-parity` — `CHUNK_WRITE_FLAGS_DELTA_PAYLOAD`, the
  XOR-merge math, systematic-RS worked example, the codec
  restriction.
- `sec-EXCHANGE_ID` MUST gain a capability flag for the
  feature (or text saying servers reject unknown flag bits with
  `NFS4ERR_NOTSUPP`).
- `sec-CHUNK_FINALIZE` / `sec-CHUNK_COMMIT` text must be
  reviewed for per-range bookkeeping interactions.

The wire format extends backward-compatibly: clients that
don't set the new flag bits and don't populate
`cwa_dirty_ranges` get today's exact behaviour.

## Implementation sequencing

If green-lighted:

1. **Draft**: write `sec-chunk-dirty-range` + `sec-delta-parity`,
   submit next FFv2 draft revision.  Includes WG review cycle.
2. **XDR**: add the new fields + flag bits to
   `lib/xdr/nfsv42_xdr.x`, regenerate C + Python stubs.
   Mandatory reviewer pass per `.claude/CLAUDE.md` "Review
   before commit" -- XDR / wire-format changes are BLOCKER-bait.
3. **Server**: extend `chunk_store_write` and the per-block
   state machine for the choice-A bookkeeping; add the
   XOR-delta path; reject delta flag bits when the codec
   doesn't support them.
4. **Client (ec_demo + ec_pipeline)**: at encode time, compute
   per-parity-shard delta = `alpha_ij * (D_i' XOR D_i)`; emit
   one CHUNK_WRITE per parity shard with the dirty range +
   delta flag; data-shard writes carry the matching dirty
   range (absolute, with guards).
5. **Harness**: extend `run_chunk_collision_t1b.sh` with a
   `chunk-split-heavy-contention` mode (N=8/16 writers on the
   same stripe, expect deterministic PASS and **measured
   throughput improvement** over Option C's serialised path).

Estimated total: 3-4 weeks of careful implementation, plus
draft review cycles.

## Open questions

- **Per-range PENDING bookkeeping vs per-block** (choice A vs B
  above).  Choice A keeps bookkeeping cost low; chooses
  parity-side parallelism with sub-range coexistence on
  COMMITTED blocks.  Choice B is the maximum-concurrency
  variant.
- **Mojette delta math**.  An internal proposal exists asserting
  Mojette delta-parity is feasible; the published math is not
  yet in the project's source headers and the systematic-RS
  path stays the load-bearing reference for the first revision.
- **Capability negotiation form**.  EXCHANGE_ID flag (cleanest)
  vs per-op NFS4ERR_NOTSUPP fallback (more forgiving).
- **Interaction with FINALIZE / COMMIT**.  Choice A: ALL
  pending ranges from a writer transition together at FINALIZE.
  Choice B: per-range transitions.

## Status

**NOT STARTED.**  Option C's serialised path is the shipped and
validated correctness story (Track 1b 30/30 stable on shadow).
Option B unblocks the parallel-write throughput story for HPC
`IOR -F 0` once that workload becomes a measured priority.

The decision to start is gated on:
- Workload priority (IOR `-F 0` parallel throughput becoming a
  measured bench target)
- Draft author capacity (FFv2 draft revision is the
  load-bearing serialised cost)
- Codec coverage choice (systematic-RS only first, or wait for
  the Mojette delta math)
