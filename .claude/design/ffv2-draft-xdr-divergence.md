<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# FFv2 Draft XDR Divergence

## Context

`draft-haynes-nfsv4-flexfiles-v2` moved through a substantial
refactor in 2026-08 to resolve three tier-1 review blockers
(M1/M2/M3) plus tier-2 folds (M4/M5).  Reffs's `lib/xdr/nfsv42_xdr.x`
is now partially in sync with the current draft; this document
records what is aligned and what is deliberately deferred.

## Aligned as of 2026-08-02

- **B2 additive fields** (this slice, branch `ffv2-b2-mirror`):
  - `read_chunk4` gained `chunk_guard4 cr_guard;` between
    `cr_owner` and `cr_payload_id`.  Mirrors draft Base B2 at
    commit `02be5992b964` in `~/Documents/ietf/flexfiles-v2`.
  - `CHUNK_HEADER_READ4resok` gained a fifth co-indexed
    `chunk_guard4 chrr_guards<>` after `chrr_chunks<>`.  Handler
    remains stubbed as `NFS4ERR_NOTSUPP`; the field is wire-only
    until a real handler lands (which should carry the co-indexing
    invariant docs across all four `<>` arrays).
  - Server: `nfs4_op_chunk_read` in `lib/nfs4/server/chunk.c`
    dual-writes `cr_owner.co_guard.{cg_gen_id, cg_client_id}` and
    `cr_guard.{cg_gen_id, cg_client_id}` from
    `blk->cb_gen_id/cb_client_id` -- redundant while
    `chunk_owner4` still embeds `chunk_guard4 co_guard`; the
    embedded copy goes away with the M2 restructure below and
    `cr_guard` becomes the sole guard on the read path.

- **M3 lifecycle stateids** (branch `xdr-sync-m2-m3-m5`, sync
  session):
  - `CHUNK_COMMIT4args` gained `stateid4 cca_stateid`.
  - `CHUNK_FINALIZE4args` gained `stateid4 cfa_stateid`.
  - `CHUNK_ROLLBACK4args` gained `stateid4 crb_stateid`.
  - Server: `chunk_lifecycle_check_stateid()` in
    `lib/nfs4/server/chunk.c` gates all three lifecycle handlers
    on the trust table, matching the existing CHUNK_WRITE /
    CHUNK_READ pattern.  Special (anonymous) stateids bypass the
    check.
  - Client: `mds_compound_add_op` zero-initialises the slot, so
    the new stateid fields default to the anonymous stateid on
    the wire; the DS's bypass rule preserves current PS demo
    behaviour.  Real layout-stateid plumbing is
    `NOT_NOW_BROWN_COW` in `lib/nfs4/ps/chunk_io.c`.

- **M5 array-cardinality bounds**:
  - `CHUNK_MAX_CHUNKS_PER_OP` = 4096,
    `CHUNK_MAX_PAYLOAD_BYTES` = 4 MiB,
    `CHUNK_MAX_OWNERS/CHECKSUMS/STATUS_PER_OP` = 4096 (in
    `lib/xdr/nfsv42_xdr.x`).
  - Server: `chunk_lifecycle_check_bounds()` rejects over-length
    arrays with `NFS4ERR_INVAL` before mutation.  Response-size
    gating against `ca_maxresponsesize` is a NOT_NOW_BROWN_COW;
    the request-side array bound is the sharper of the two
    knobs.

## Deferred: M2 chunk_owner4 restructure

The draft moved from single-owner-per-CHUNK_WRITE to
**batched-cohort** semantics:
- New opaque `chunk_cohort_id4` type.
- `chunk_owner4` = `(co_cohort_id, co_client_id, co_id)` (three
  fields; no embedded `chunk_guard4`).
- `CHUNK_WRITE4args` carries `(cwa_cohort_id, cwa_client_id,
  cwa_co_ids<>)` -- shared cohort for the batch plus per-chunk
  co_ids array.
- `chunk_guard4` remains but is now purely per-chunk CAS state,
  no longer an owner-triple carrier.

Reffs stays on the older single-owner shape:
- `chunk_owner4` = `{ chunk_guard4 co_guard, uint32 co_id }`
  (two fields; embedded chunk_guard4).
- `CHUNK_WRITE4args` = `{ ..., chunk_owner4 cwa_owner, ... }` --
  ONE owner per op, not a batched cohort.

### Why deferred

- **Refactor scope**: multi-day, touching
  `lib/xdr/nfsv42_xdr.x`, `lib/nfs4/server/chunk.c` (15 refs),
  `lib/nfs4/ps/chunk_io.c` (10 refs),
  `lib/nfs4/ps/ec_pipeline.c` (3 refs),
  `lib/nfs4/client/mds_layout.c` (2 refs),
  `lib/nfs4/tests/chunk_test.c` (20 refs),
  `lib/nfs4/tests/chunk_repair_test.c` (11 refs).
- **Missing client semantic**: the draft's M1 fix requires the
  CLIENT to rollback and retry under a fresh cohort_id on
  `NFS4ERR_CHUNK_GUARDED`.  Reffs's PS-side client does not yet
  implement client-driven rollback; adding cohort semantics
  without the retry loop would leave a live regression on
  multi-writer paths.
- **BAT priority**: STABLE_BAT is Phase 3 (NFSv4.2 ops) and
  Phase 6 (BAT demo).  M2 restructure would push those back
  without unblocking any deliverable currently on the BAT
  critical path.
- **In-flight draft rule**: per user memory
  `feedback_xdr_proposed_vs_established`, in-flight draft
  fields revise freely.  Reffs staying on the older shape is
  fine while the draft settles; the wire is not deployed.

### When to pick this back up

Any of the following would move M2 restructure onto the
schedule:

1. Reffs's PS client starts driving multi-writer scenarios
   where the M1 client-rollback semantic is a correctness
   requirement.
2. The Linux kernel client (`fs/nfs/flexfilesv2/`) adopts the
   batched-cohort wire and reffs needs to interop with it.
3. STABLE_BAT Phase 6 is complete and there is bandwidth for
   a proper M2 restructure design + implementation slice.

### Contract for interim behaviour

- The current reffs CHUNK ops are self-consistent: server and
  client both use the single-owner shape.  Interop is fine
  between reffs and the ec_demo client that ships with reffs.
- Interop with a batched-cohort peer would break.  A client or
  server built against the current draft cannot talk to a reffs
  built from this branch.  Deployments that want cross-family
  interop MUST wait for the M2 restructure to land.
- Test coverage: existing chunk tests exercise the single-owner
  shape.  New tests for the batched-cohort shape will land with
  the M2 restructure.

## References

- Draft (2026-08-02):
  `~/Documents/ietf/flexfiles-v2/draft-haynes-nfsv4-flexfiles-v2.md`
  at commit `2de7b49e7711` (M4/M5 tier-2 fold).
- Codex fresh-semantics review with all M1/M2/M3/M4/M5 findings:
  `~/Documents/reffs-docs/flexfiles-v2-fresh-semantics-review-codex.md`.
- Trust-stateid design (M3 counterpart):
  `.claude/design/trust-stateid.md`.
