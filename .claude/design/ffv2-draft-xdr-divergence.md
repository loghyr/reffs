<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# FFv2 Draft XDR Divergence

## Context

`draft-haynes-nfsv4-flexfiles-v2` moved through a substantial
refactor in 2026-08 to resolve three tier-1 review blockers
(M1/M2/M3) plus tier-2 folds (M4/M5), and again on 2026-08-05
for the fresh review round (encoding-name rename,
NFS4ERR_ENCODING_NOT_SUPPORTED rename, attr 90, CHUNK_ESCROW
ops, ffv2_device_versions4 fork).  Reffs's
`lib/xdr/nfsv42_xdr.x` is now largely in sync with the current
draft; this document records what is aligned and what is
deliberately deferred.

## Aligned as of 2026-08-05 (R1-R5 sync slices on wip/ffv2-sync-r2)

- **R1 mechanical renames** (676a2cf, 61ad8de, 92af495):
  - `FFV2_ENCODING_MIRRORED` -> `FFV2_ENCODING_REPLICATED`
    (value 0x5 unchanged).
  - `NFS4ERR_CODING_NOT_SUPPORTED` -> `NFS4ERR_ENCODING_NOT_SUPPORTED`
    (10097 unchanged).
  - `FFV2_DS_FLAGS_SPARE` retired; wire value 0x2 held as reserved
    gap comment.

- **R2 draft typedefs + errors + tight-coupling field**:
  - Five new error codes (10103-10107): `NFS4ERR_NO_PREDECESSOR`,
    `NFS4ERR_NO_ADOPTABLE_LOCK`, `NFS4ERR_STALE_ESCROW`,
    `NFS4ERR_STALE_MDS_EPOCH`, `NFS4ERR_PARTIAL` (b9d03ad).
  - Typedefs `chunk_state_flags4`, `ffv2_layoutstats_flags4`,
    `escrow_id4` (a5cccda).
  - Wire swaps: `bool chrr_locked<>` / `bool cr_locked<>` ->
    `chunk_state_flags4<>` in CHUNK_HEADER_READ4resok and
    read_chunk4 (2e1ec0a).
  - TRUST_STATEID4args gained `uint32_t tsa_client_id` after
    `tsa_layout_stateid` (3ff2bf2).
  - Field renames: `ffm_client_id` -> `ffv2m_client_id` in
    `ffv2_mirror4`; every `ffl_*` field in `ffv2_layoutupdate4`
    -> `ffv2l_*` (including `ffl_local` -> `ffv2l_flags`) plus
    const `FFV2_LAYOUTSTATS_FLAGS_LOCAL = 0x00000001` (c5b2fef).

- **R3 fattr4_chunked_data_file (attribute 90)** (0cb6db6):
  - `typedef bool fattr4_chunked_data_file;`
  - `const FATTR4_CHUNKED_DATA_FILE = 90;`
  - Handler in `lib/nfs4/server/attr.c` mirrors the attr 89
    (coding_block_size) shape (count/xdr/equal + nao[] entry +
    bitmap init) plus decode + apply-to-inode + populate-from-
    inode + settable-list entry.
  - Per-inode storage: `INODE_IS_CHUNKED_DATA_FILE = 1ULL << 6`
    in `i_attr_flags`; persists via `id_attr_flags` in inode_disk
    with no format bump (no deployed storage).

- **R4 metadata-server SETs fattr4_chunked_data_file on
  NFSv4.2 DS file creation** (c40d4a8):
  - `lib/nfs4/dstore/dstore_ops_nfsv4.c` OPEN(CREATE) createattrs
    now carries attribute 90 = TRUE.
  - Wire encoding: bitmap word 2 bit 26 (attr 90) + XDR bool TRUE
    (0x00000001).
  - NFSv3 dstore and combined-mode local dstore are unchanged
    (only NFSv4.2 goes through this createattrs path).
  - NOT_NOW_BROWN_COW: ATTRNOTSUPP retry-without-createattrs plus
    per-dstore capability latch, needed for interop with non-reffs
    NFSv4.2 data servers that do not advertise attribute 90.

- **R5a PROXY_* + EXCHANGE_RANGE renumber** (f974657):
  - `OP_PROXY_REGISTRATION` 92 -> 96, `_PROGRESS` 93 -> 97,
    `_DONE` 94 -> 98, `_CANCEL` 95 -> 99.
  - `OP_EXCHANGE_RANGE` 96 -> 100.
  - `REFFS_NFS4_OP_MAX` bumped 104 -> 108.
  - Makes room for the CHUNK_ESCROW block at 92-95.  Both drafts
    (proxy-server, swap) are in-flight and owner-controlled per
    `feedback_xdr_proposed_vs_established`.

- **R5b CHUNK_ESCROW ops 92-95 skeleton** (e9d8c79):
  - `OP_CHUNK_ESCROW_INSTALL/RELEASE/ENUMERATE/TAKEOVER` at
    92-95 with wire arg/res struct/union pairs and dispatch-union
    arms in nfs_argop4 / nfs_resop4.
  - Supporting types: `proof_profile_id4` typedef; consts
    `PROOF_PROFILE_UNSPECIFIED/HA_AUTHORITY_ED25519`,
    `CETA_INCARNATION_PROOF_MAX4`, `CHUNK_ESCROW_ENUMERATE_MAX4`,
    `CHUNK_ESCROW_ENUMERATE_COOKIE_MAX4`; struct `escrow_enum_entry4`.
  - Handlers in `lib/nfs4/server/chunk.c` return NFS4ERR_NOTSUPP
    (same shape as the pre-existing CHUNK_HEADER_READ etc. stubs).
    Semantics land in follow-up implementation slices.

- **R5c ffv2_device_versions4 / ffv2_device_addr4 fork** (eb2c1d5):
  - Consts `FFV2_COUPLING_SYNTHETIC_UIDS/TIGHTLY_COUPLED/TRUSTED_STATEID`.
  - Struct `ffv2_device_versions4` with `ffv2dv_coupling` uint32
    bitmask replacing the FFv1 `bool ffdv_tightly_coupled`.
  - Struct `ffv2_device_addr4` wrapping the versions array.
  - FFv1 `ff_device_versions4` / `ff_device_addr4` unchanged
    (RFC 8435 wire); consumers still use them for both v1 and v2
    layouts pending a follow-up implementation slice to flip the
    v2 encoder in `lib/nfs4/server/layout.c` and matching decoder
    in `lib/nfs4/client/mds_layout.c`.

## Aligned as of 2026-08-02

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

## Deferred (implementation slices, not draft divergence)

The R3-R5 slices above landed the XDR wire skeleton; the
runtime code paths that make the skeleton mean something at the
wire level are separate follow-up slices, not draft-divergence
items:

- **CHUNK_ESCROW semantics**: handlers return NFS4ERR_NOTSUPP.
  Full install/release/enumerate/takeover implementation is a
  separate design slice (with an escrow table, metadata-server
  epoch tracking, and proof-profile validation).
- **ffv2_device_addr4 emit**: the v2 GETDEVICEINFO encoder in
  `lib/nfs4/server/layout.c` still emits `ff_device_addr4`.
  Flipping it to emit `ffv2_device_addr4` with a coupling
  bitmask (and matching decoder in `lib/nfs4/client/mds_layout.c`)
  is a follow-up.
- **fattr4_chunked_data_file enforcement on the DS**: the R3+R4
  slices set and read the attribute per file, but the CHUNK ops
  do not yet reject requests against `chunked_data_file == FALSE`
  files.  Enforcement follows once the layout-vs-attribute
  semantics are wired end-to-end.
- **ATTRNOTSUPP retry**: `nfsv4_create` currently fails hard if
  the DS does not advertise attribute 90.  Adds a per-dstore
  capability latch and retry-without-createattrs when interop
  with non-reffs data servers becomes a live requirement.

## References

- Draft (2026-08-05):
  `~/Documents/ietf/flexfiles-v2/draft-haynes-nfsv4-flexfiles-v2.md`
  head of the current review round (encoding-name rename +
  attr 90 + CHUNK_ESCROW block).
- Draft (2026-08-02):
  same repo at commit `2de7b49e7711` (M4/M5 tier-2 fold) --
  baseline for the M2/M3/M5 items above.
- Sync plan for the R1-R5 slices:
  `.claude/design/ffv2-sync-plan-2026-08-05.md`.
- Codex fresh-semantics review with all M1/M2/M3/M4/M5 findings:
  `~/Documents/reffs-docs/flexfiles-v2-fresh-semantics-review-codex.md`.
- Trust-stateid design (M3 counterpart):
  `.claude/design/trust-stateid.md`.
