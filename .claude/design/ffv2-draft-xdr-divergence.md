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

- **Base B2 additive fields** (commit `979518ce3c76`):
  - `read_chunk4` gained `chunk_guard4 cr_guard` between
    `cr_owner` and `cr_payload_id`.  The DS sees the per-chunk
    generation on every CHUNK_READ reply, satisfying the CAS
    expected-value contract from the draft's multi-writer rules.
  - `CHUNK_HEADER_READ4resok` gained `chunk_guard4 chrr_guards<>`
    after `chrr_chunks<>`; one guard per chunk header.
  - Server: `lib/nfs4/server/chunk.c` dual-writes into
    `rc->cr_owner.co_guard` and `rc->cr_guard` from the same
    `chunk_block` pair, so the older single-owner shape and the
    new additive guard coexist until M2 lands.  No server
    behavioural change; the extra field is a pure read-path
    observation.
  - Kernel client mirror: `psyklo/ffv2-client` at `06a10ddebbca`
    decodes `cr_guard` into `struct ffv2_read_chunk4.guard` on
    every CHUNK_READ.

## Deferred: Base B4 tight-coupling + tsa_client_id

The draft's Base B4 fix landed as commit `d40c581ed251` on
`origin/main` of `draft-haynes-nfsv4-flexfiles-v2` (2026-08-02).
Two normative changes:

1. **CHUNK_* encodings require tight coupling**: any
   `FFV2_ENCODING_*` value other than `PASSTHROUGH` MUST be
   advertised with `ffdv_tightly_coupled = true`.  Loose coupling
   is no longer a valid pairing with chunked encodings.
2. **`TRUST_STATEID4args` gained `uint32_t tsa_client_id`**: the
   metadata server registers the `ffv2m_client_id` bound to the
   layout stateid so the data server can validate `cwa_client_id`
   on CHUNK_WRITE (and `cg_client_id` in `chunk_guard4` CAS)
   against the trust-table entry.  Prior text hand-waved that the
   DS "knows" the client identity from the layout; the wire had
   no field for it.

Reffs stays on the pre-B4 wire for now:

- `TRUST_STATEID4args` in `lib/xdr/nfsv42_xdr.x` is unchanged
  (four fields: `tsa_layout_stateid, tsa_iomode, tsa_expire,
  tsa_principal`).
- `lib/nfs4/server/chunk.c` does not compare `cwa_client_id`
  against a trust-table `tsa_client_id` -- the trust-table entry
  does not yet carry one.
- The layout constraint at LAYOUTGET time does not enforce
  tight-coupling for chunked encodings; a loose-coupled DS with a
  non-PASSTHROUGH encoding would still be issued today (the
  runtime has no such deployment, so this is currently unreachable
  rather than incorrect).

### Why deferred

- **Slice size**: XDR change plus `trust_stateid.c` renewal path,
  chunk-op validation hook update, MDS-side fanout compound
  reshape, and unit tests for each -- multi-file, multi-day.
  Best queued as a dedicated slice once the K.2 patch 5 kernel
  cross-verify closes (task #555).
- **Kernel client**: the kernel doesn't send TRUST_STATEID (it's
  MDS-to-DS), so no client-side wire mirror is needed for B4.
  The layout-constraint tightening affects deployment posture,
  not the client wire format.
- **BAT posture**: the reffs demo already runs tight-coupling
  paths through the (currently loose-coupling-compatible)
  anonymous-stateid bypass at the DS.  B4's tightening does not
  change the wire the demo exercises today; it forbids a
  configuration nobody uses.

### When to pick this back up

1. K.2 patch 5 dreamer cross-verify closes (task #555).
2. Any deployment starts running non-PASSTHROUGH encodings under
   loose coupling (currently a configuration that never appears).
3. The trust-stateid follow-up work (`trust-stateid.md`) queues
   a slice for the `tsa_client_id` renewal path.

### Follow-up slices

- Reffs XDR: add `uint32_t tsa_client_id` to `TRUST_STATEID4args`.
- Reffs server: extend `trust_entry` with `te_ffv2m_client_id`;
  populate on TRUST_STATEID; validate `cwa_client_id` against it
  in the CHUNK_WRITE stateid hook; same for CAS `cg_client_id`.
- Reffs MDS: include `ffv2m_client_id` in the LAYOUTGET-time
  TRUST_STATEID fanout.
- Reffs LAYOUTGET: enforce `ffdv_tightly_coupled = true` for any
  mirror whose encoding is not PASSTHROUGH; reject inconsistent
  configurations at load time.

## Deferred: encoding-type-data union naming (draft 69f822225425)

The draft finished the `coding` -> `encoding` rename at the union
that switches on `ffv2_encoding_type4`:

| Draft before | Draft now |
|---|---|
| `ffv2_coding_type_data4` | `ffv2_encoding_type_data4` |
| `ffv2m_coding_type_data` | `ffv2m_encoding_type_data` |
| `ffv2ctd_coding` | `ffv2etd_encoding` |
| `ffv2ctd_protection` | `ffv2etd_protection` |

No wire change: same discriminated union, same discriminant type,
same arm types, same values.

Reffs is unaffected in structure because it never adopted the
union -- `ffv2_mirror4` carries `ffv2m_coding_type` plus
`ffv2m_protection` flattened, under the NOT_NOW_BROWN_COW at
`lib/xdr/nfsv42_xdr.x:5181`.  Two things do need doing:

1. **Done 2026-08-07.**  The NOT_NOW_BROWN_COW and the comment
   above `ffv2_data_protection4` named `ffv2_coding_type_data4`,
   `ffv2ctd_coding` and `sec-ffv2_coding_type_data4`, so a reader
   following them landed on identifiers the draft no longer has.
   Swept to the current names.  One mention of the old name
   survives on purpose, as a dated "renamed from" note, so that
   grepping the old identifier still finds the explanation.

   The `sec-` citation was replaced by the section's name rather
   than a new anchor: the draft's `## ffv2_encoding_type_data4`
   heading carries no `sec-` anchor at all, so the old citation
   pointed at something that never existed.
2. `ffv2m_coding_type` is reffs's own field name and carries the
   retired vocabulary independently of the union question.
   `ffv2m_encoding_type` matches the draft's enum rename.  This
   one does touch generated API and every consumer, so it is a
   real slice, not a comment fix.

The kernel client (`psyklo/ffv2-client`) carries one stale
`ffv2_coding_type4` reference (checked 2026-08-07 on dreamer's
tree).  Identifier only, no interop consequence, so it can ride
along with the next K-series patch rather than justify its own.

## OPEN: who assigns the writer identity (three implementations disagree)

Found 2026-08-06 while inventorying the kernel client, after S4b
landed.  This is a coherence problem across the draft and both
clients, not a kernel catch-up item.

**The draft, post-B4:** the metadata server assigns the writer
identity, publishes it in `ffv2m_client_id`, registers it with each
data server as `tsa_client_id`, and the client echoes it as
`cg_client_id` on CHUNK operations.  A mismatch is NFS4ERR_BAD_STATEID
-- "a client that presents a cwa_client_id different from its layout's
ffv2m_client_id is spoofing another writer's identity."

**Both clients predate that and self-assign:**

| implementation | source of cg_client_id |
|---|---|
| reffs proxy/`ec_demo` (`lib/nfs4/ps/chunk_io.c:48`) | `getpid()`, sentinel-rotated |
| kernel `ffv2-client` (`flexfilesv2.h:526`) | `get_random_u32()` at module init, one per kernel |

The kernel header states the old model outright: "cg_client_id is
CLIENT-derived, not from the layout.  Reference client at reffs
lib/nfs4/ps/chunk_io.c uses getpid() with sentinel rotation; we mirror
that."  It is faithfully implementing what reffs did before B4.

Notably the kernel already decodes the right value --
`flexfilesv2xdr.c:258` reads `ffv2m_client_id` into `m->client_id` --
and then ignores it in favour of the seed.

**Consequence, and it is self-inflicted.**  S4b's comparison is
correct per the draft, but the moment any data server is tight
coupled, a client presenting a real layout stateid gets its
self-assigned id compared against the metadata-server-assigned one and
every CHUNK_WRITE fails with NFS4ERR_BAD_STATEID.  That includes reffs's
own client against reffs's own server.

It is latent today only because nothing tight coupled is exercised:
the loopback dstores are NFSv3, so clients use the anonymous stateid
and `chunk_check_trusted_stateid` returns at its `stateid4_is_special`
guard before reaching the comparison.  The 2026-08-06 live run
confirmed this -- zero trust-table hits across 2304 CHUNK_WRITEs.

**The fix is one decision applied three times:** the identity is the
metadata server's to assign, so both clients stop self-assigning and
echo `ffv2m_client_id` from the layout.  Ordering does not matter
between the two clients; neither can be exercised against a
tight-coupled server until it lands.

Do not "fix" this by relaxing S4b -- the escape hatch there is for a
metadata server that registers no binding at all, not for a client
that presents the wrong one.

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
  at commit `d40c581ed251` (Base B4 tight-coupling + tsa_client_id
  fold), head of the current review round (encoding-name rename +
  attr 90 + CHUNK_ESCROW block).  Prior stable point:
  `02be5992b964` (Base B2 additive fields).
- Draft (2026-08-02):
  same repo at commit `2de7b49e7711` (M4/M5 tier-2 fold) --
  baseline for the M2/M3/M5 items above.
- Sync plan for the R1-R5 slices:
  `.claude/design/ffv2-sync-plan-2026-08-05.md`.
- Codex fresh-semantics review with all M1/M2/M3/M4/M5 findings:
  `~/Documents/reffs-docs/flexfiles-v2-fresh-semantics-review-codex.md`.
- Family review with B1-B4 findings:
  `~/Documents/reffs-docs/flexfiles-v2-family-codex-review-2026-08-02.md`
  and its fable counterpart.
- Trust-stateid design (M3 counterpart, B4 will extend):
  `.claude/design/trust-stateid.md`.
