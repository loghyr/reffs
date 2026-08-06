<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# FFv2 Draft Sync Plan (2026-08-05)

## Context

`draft-haynes-nfsv4-flexfiles-v2` moved through a large review
round in 2026-08 that landed a fresh batch of XDR / constant /
error-code changes on top of what
`.claude/design/ffv2-draft-xdr-divergence.md` last captured
(2026-08-02).  This document enumerates the delta and orders it
into slices for reffs and the Linux kernel client
(`/Volumes/Sensitive/linux`, branch `ffv2-client`).

Companion tree references:
- Reffs: `/Volumes/Sensitive/reffs`, branch `main`, tip
  `a38d7dc542f2` at write time.  XDR at
  `lib/xdr/nfsv42_xdr.x`; server logic at `lib/nfs4/server/`;
  MDS-to-DS creation path at
  `lib/nfs4/dstore/dstore_ops_nfsv4.c`.
- Kernel: `/Volumes/Sensitive/linux`, branch `ffv2-client`, tip
  `1e57ef3dabca`.  FFv2 code at `fs/nfs/flexfilesv2/`; XDR at
  `flexfilesv2_xdr_chunk.[ch]` and `flexfilesv2xdr.c`;
  constants in `flexfilesv2.h`.

## Delta inventory (from Explore pass 2026-08-05)

| # | Item | Reffs | Kernel |
|---|------|-------|--------|
| 1 | `FFV2_ENCODING_REPLICATED` rename (was `_MIRRORED`) | needs rename (7 refs) | needs rename (3 refs) |
| 2 | `fattr4_chunked_data_file` attr 90 | absent | absent |
| 3 | `fattr4_coding_block_size` attr 89 | already synced | absent |
| 4 | `chunk_state_flags4` typedef + `CHUNK_STATE_FLAGS_LOCKED = 0x1`, widen `chrr_locked` / `cr_locked` | still `bool<>` | still `bool<>` (enforced zero-length) |
| 5 | `ffv2_layoutstats_flags4` typedef, widen `ffv2l_local` bool | still `bool ffl_local` (also name delta) | n/a-for-client (MDS-return path) |
| 6 | `chunk_cohort_id4` split + cohort restructure (M2) | **deferred** per divergence doc | **deferred** |
| 7 | CHUNK_ESCROW ops 92-95 (INSTALL / RELEASE / ENUMERATE / TAKEOVER) | absent, **op collision** (92/93 are PROXY_REGISTRATION / PROXY_PROGRESS) | absent |
| 8 | `escrow_id4` opaque[16] | absent | absent |
| 9 | `tsa_client_id` field in TRUST_STATEID4args | ops present but field absent | entire family absent |
| 10 | `ffv2_device_versions4` fork with `ffdv_coupling` bitflags | still uses `ff_device_versions4` with `bool ffdv_tightly_coupled` | still `struct nfs4_ffv2_ds_version { bool tightly_coupled; }` |
| 11 | Retire `FFV2_DS_FLAGS_SPARE` + First-Line Substitution | still `FFV2_DS_FLAGS_SPARE = 0x00000002` | already clean |
| 12 | `NFS4ERR_ENCODING_NOT_SUPPORTED` rename (was `_CODING_`) | needs rename | constant absent entirely |
| 13 | New error codes: `NFS4ERR_STALE_ESCROW` (10105) / `NO_ADOPTABLE_LOCK` (10104) / `STALE_MDS_EPOCH` (10106) / `NO_PREDECESSOR` (10103) / `PARTIAL` (10107) | 5 of 7 absent (`CHUNK_LOCKED`, `PAYLOAD_LOST` already present) | all 7 absent |
| 14 | Reffs MDS SETATTR fattr4_chunked_data_file on DS file creation | absent (`dstore_ops_nfsv4.c:222-231` zeros attrmask) | n/a-for-client |

Name-delta surprises the Explore pass found:
- Reffs field `ffm_client_id` vs draft `ffv2m_client_id` on
  m2m messages.
- Reffs field `ffl_local` vs draft `ffv2l_local` on
  layoutstats.
- Reffs uses ops 92/93 for PROXY_REGISTRATION / PROXY_PROGRESS
  which directly collide with the draft's CHUNK_ESCROW range.
  See "Open decision A" below.

## Slice plan

Ordering by risk and dependency: renames and pure-additive XDR
first, behavior changes and op-number collisions gated on user
decisions, kernel mirror after reffs settles.

### R1 -- reffs XDR mechanical renames [small, mechanical]

Items 1, 11, 12.  Pure text renames of established identifiers.

- `FFV2_ENCODING_MIRRORED` -> `FFV2_ENCODING_REPLICATED` (7
  refs across `lib/xdr/nfsv42_xdr.x` + wherever the constant
  is consumed).
- Remove `FFV2_DS_FLAGS_SPARE = 0x00000002` and its
  First-Line Substitution consumers (grep for the constant to
  bound the surface).
- `NFS4ERR_CODING_NOT_SUPPORTED` -> `NFS4ERR_ENCODING_NOT_SUPPORTED`
  (error code 10097 unchanged).

Verify: `make -f Makefile.reffs license style unit` +
reviewer per project standards.

### R2 -- reffs XDR additive: new typedefs and errors [small]

Items 4, 5, 8, 13, plus the 9-partial (tsa_client_id addition).

- Add `chunk_state_flags4` typedef (bitfield of chunk state
  flags, `CHUNK_STATE_FLAGS_LOCKED = 0x1`) and swap
  `chrr_locked` (line 3513) + `cr_locked` (line 3553) from
  `bool<>` to `chunk_state_flags4<>`.
- Add `ffv2_layoutstats_flags4` typedef and swap `ffl_local`
  (line 4976) -- also rename this reffs field to match the
  draft's authoritative name (see "Open decision B" below).
- Add `escrow_id4` opaque[16] typedef.
- Add 5 missing error constants: `NFS4ERR_NO_PREDECESSOR`
  (10103), `NFS4ERR_NO_ADOPTABLE_LOCK` (10104),
  `NFS4ERR_STALE_ESCROW` (10105), `NFS4ERR_STALE_MDS_EPOCH`
  (10106), `NFS4ERR_PARTIAL` (10107).
- Add `tsa_client_id` (or `ffm_client_id` -- see decision B) to
  `TRUST_STATEID4args`.

Verify: build + tests; trust_stateid tests should still pass
since the new field can default to zero for existing callers.

### R3 -- reffs fattr4_chunked_data_file (attr 90) [small]

Item 2.  Parallel structure to already-synced attr 89.

- Add `typedef bool fattr4_chunked_data_file` in `nfsv42_xdr.x`.
- Add `FATTR4_CHUNKED_DATA_FILE = 90` to the attribute-number
  enum.
- Add handler in `lib/nfs4/server/attr.c` following the pattern
  established at `:153, :182, :2056-2072, :2230-2231, :2335,
  :3049` for attr 89.

Verify: build + attr test coverage.

### R4 -- reffs MDS: SET fattr4_chunked_data_file on DS creation [medium]

Item 14.  The user's explicit request.

- Edit `lib/nfs4/dstore/dstore_ops_nfsv4.c:222-231`: change
  `createattrs.attrmask` from the current zero-attrmask to
  include `FATTR4_CHUNKED_DATA_FILE` when the file is being
  created on an NFSv4.2 DS to hold chunked-encoding data.
- Guard: only set when the target DS has been detected as
  NFSv4.2-capable AND the file's layout uses a
  chunked encoding (`FFV2_ENCODING_RS_VANDERMONDE`, etc. -- any
  non-`PASSTHROUGH` encoding).  Skip for NFSv3 DSes
  (`dstore_ops_nfsv3.c` has no bitmap4 attrmask -- no change
  needed) and for PASSTHROUGH files.
- Graceful degradation: if the DS returns
  `NFS4ERR_ATTRNOTSUPP`, log at debug level, remember the DS
  does not support attr 90, and continue without it.  Do NOT
  fail the file creation.

Verify: single-host bench + real-network 3-host variant a/b
(chunked writes create files on DSes that will start seeing the
setattr).

### R5 -- reffs op-collision resolution + CHUNK_ESCROW skeleton [medium, gated on decision A]

Items 7, 8 (escrow_id4 already covered by R2), 10.

**Depends on Open decision A.**

Once the op-number question is resolved, add:
- Op 92 CHUNK_ESCROW_INSTALL + args/res + escrow_id4-typed
  fields.
- Op 93 CHUNK_ESCROW_RELEASE.
- Op 94 CHUNK_ESCROW_ENUMERATE.
- Op 95 CHUNK_ESCROW_TAKEOVER + proof-profile placeholder
  types.
- New `ffv2_device_versions4` typedef alongside the existing
  `ff_device_versions4`, with `ffdv_coupling` bitflags
  (`FFV2_COUPLING_SYNTHETIC_UIDS`, `_TIGHTLY_COUPLED`,
  `_TRUSTED_STATEID`).

Skeleton only in this slice: XDR types + no-op handlers that
return NFS4ERR_NOTSUPP.  Real behavior lands in a follow-up.

Verify: build + XDR round-trip test that the args/res encode
and decode.

### R6 -- reffs .claude/design/ffv2-draft-xdr-divergence.md refresh [tiny]

Update the divergence doc to move items 1, 4, 5, 8, 11, 12, 13
(and any others that land in R1-R5) from "diverged" to
"aligned", and add M2 (item 6) reminder + this plan doc as a
reference.

### K1 -- kernel client mechanical renames [small]

Items 1, 12.

- `FFV2_ENCODING_MIRRORED` -> `FFV2_ENCODING_REPLICATED`
  (`flexfilesv2.h:150` + 3 refs).
- Add `NFS4ERR_ENCODING_NOT_SUPPORTED` constant (currently
  absent).

Verify: build on dreamer via HGFS overlay.

### K2 -- kernel client attribute decoders [medium]

Items 2, 3.  Wire the two new attributes into the kernel's
NFSv4.2 attribute-parsing table.

- `fattr4_coding_block_size` (attr 89) -- kernel currently
  never sees this attr; wire the bitmap bit and the decoder.
- `fattr4_chunked_data_file` (attr 90) -- decoder + honor the
  bit in the client's per-file classification.

Verify: build; interop with reffs's R3+R4 patches (attr 90
appears on files reffs creates on DSes).

### K3 -- kernel client typedef swaps [small]

Item 4.  Kernel currently enforces `nlocked == 0` on
CHUNK_HEADER_READ and CHUNK_READ.  Flip the wire type from
`bool<>` to `u32<>` (chunk_state_flags4) and remove the
zero-length enforcement so the client can start decoding real
flag words when reffs starts emitting them.

Verify: chunk-lifecycle wire tests with reffs.

### K4 -- kernel client ff_device_versions4 fork [small]

Item 10.  Add `nfs4_ffv2_ds_versions` alongside the existing
FFv1-shared `nfs4_ff_ds_versions`, with `ffdv_coupling`
bitflags.  Update the layout parser to prefer the new shape
when the wire carries it and fall back to the FFv1 shape when
the peer still ships the old wire.

Verify: build + layout-parse tests + interop with reffs.

### K5 -- kernel client error-code constants [tiny]

Item 13.  Add all 7 error constants.  Client-side error
handling: at minimum, translate them into a suitable errno
(likely `-EIO` for the ones the client cannot recover from,
and specific handling for `NFS4ERR_STALE_MDS_EPOCH` that
should provoke a layout refresh).

Verify: fault-injection tests where reffs returns each error.

### Deferred: M2 cohort restructure (item 6)

Explicitly deferred per the existing divergence doc.  Requires
a proper design pass covering:
- reffs `chunk_owner4` XDR restructure + client-side rollback
  loop.
- Kernel `chunk_owner4` restructure.
- Cross-tree wire compatibility strategy during the flip.
- Test coverage for batched-cohort semantics.

Not on this sync's critical path.  Revisit when the STABLE_BAT
Phase 6 work opens bandwidth.

## Decisions (2026-08-05, user gate)

- **A1** chosen for the op-number collision.  Reffs's
  proprietary PROXY_REGISTRATION (op 92) and PROXY_PROGRESS
  (op 93) will be renumbered so the draft-owned CHUNK_ESCROW
  ops 92-95 can land at their draft-assigned numbers.
  Rationale (user): each draft's op family should be
  self-contained inside its own draft's assigned range; reffs
  should follow whatever range the proxy-server draft settles
  on for its ops.  Preparatory work for R5: check the current
  proxy-server draft's op-number allocation and pick reffs's
  new PROXY_* numbers from that range.
- **B1** chosen for field-name alignment.  Reffs renames
  `ffl_local` -> `ffv2l_local` and `ffm_client_id` ->
  `ffv2m_client_id` in R2 (and any other short-form field
  names surfaced during the sweep).

## Open decisions (resolved above)

### A. Op-number collision (item 7)

Reffs uses op 92 for PROXY_REGISTRATION and op 93 for
PROXY_PROGRESS -- both proprietary reffs ops added before the
draft's CHUNK_ESCROW range settled.  The draft is now
authoritative for wire format and needs 92/93 for
CHUNK_ESCROW_INSTALL / CHUNK_ESCROW_RELEASE.

Options:
- **A1. Renumber the PROXY_* ops in reffs** to whatever range
  the proxy-server draft settles on (or an experimental range
  above the CHUNK_ESCROW block).  Preserves the draft's op
  numbers.  Requires a design pass on where proxy ops go.
- **A2. Renumber the CHUNK_ESCROW ops in reffs's XDR only**,
  documenting the reffs-vs-draft delta in the divergence doc,
  until the proxy-server draft settles and cross-standards
  compatibility is achievable.  Preserves reffs's shipped
  proxy wire but forces a reffs-vs-draft interop gap.
- **A3. Not implement CHUNK_ESCROW in reffs yet** -- ship only
  the XDR skeleton at reffs-local placeholder numbers with a
  TODO, and defer the op-number decision until the proxy draft
  and the flexfiles-v2 draft can be aligned.

My recommendation is A1 -- reffs's PROXY_REGISTRATION /
PROXY_PROGRESS are experimental / not standardized, and moving
them lets reffs stay wire-compatible with the flexfiles-v2 draft
that IS being standardized.  But this needs your call.

### B. Field-name alignment (item 5, item 9)

Reffs uses field names `ffl_local` and `ffm_client_id` where
the draft uses `ffv2l_local` and `ffv2m_client_id`.  Since we
are touching these fields anyway in R2:
- **B1. Rename reffs's fields to match the draft**, one-time
  churn, drift closed.
- **B2. Leave reffs's field names alone**, document the delta.

My recommendation is B1 -- the fields are internal to
FFv2-specific handlers and the draft is authoritative.  But
there may be reasons in the reffs codebase to keep the short
form; your call.

## Semantics slices (S1-S6) -- added 2026-08-06

R1-R7 synchronized the **wire format** only.  That was deliberate:
the goal was for reffs and the draft to agree on bytes before any
behavior was built on top.  The consequence is that a good deal of
newly-landed wire surface currently has no code behind it.  This
section is the ledger of that gap, so it is tracked work rather
than an implicit TODO.

Audit basis (2026-08-06, on `fef9c82168a2`): each item below was
confirmed by counting live C consumers of the symbol, not by
recollection.

### S1 -- enforce fattr4_chunked_data_file on CHUNK ops [HIGHEST]

**Gap.** R3 stores attribute 90 as `INODE_IS_CHUNKED_DATA_FILE`
(bit 6 of `i_attr_flags`) and R4 SETs it on every file the
metadata server creates on an NFSv4.2 data server.  Nothing reads
it for enforcement: the symbol appears only in
`lib/nfs4/server/attr.c` (set + report).  `lib/nfs4/server/chunk.c`
has **zero** references.

**Why this ranks first.** The draft's stated purpose for
attribute 90 (sec-fattr4_chunked_data_file, and the reciprocal
rule in sec-ops-client) is that a data server which has
identified a file as non-chunked MUST reject CHUNK operations
against it with NFS4ERR_NOTSUPP.  reffs currently advertises the
classification and then ignores it, so a client can still drive
CHUNK_WRITE / CHUNK_READ at a PASSTHROUGH file and reffs will
process it.  We are shipping the label without the check that
makes the label mean anything.

**Work.** Add a guard at the head of the CHUNK-family handlers
that returns NFS4ERR_NOTSUPP when the current filehandle's inode
does not carry `INODE_IS_CHUNKED_DATA_FILE`.  Decide explicitly
what happens for files that predate the attribute (bit 0 =
"not chunked" is the safe default, but that would reject CHUNK
ops on files created before R4 landed -- so the guard likely
needs to be opt-in per export, or gated on the file having
layout segments).  That decision is the substance of this slice;
the code is small.

**Tests.** CHUNK_WRITE / CHUNK_READ / CHUNK_FINALIZE / CHUNK_COMMIT
against a file with the attribute clear -> NFS4ERR_NOTSUPP; with
it set -> current behavior unchanged.  Plus a regression test that
a PASSTHROUGH-encoding file is rejected.

### S2 -- CHUNK_ESCROW semantics [large]

**Gap.** Ops 92-95 decode and dispatch, and all four handlers
return NFS4ERR_NOTSUPP (`lib/nfs4/server/chunk.c`).  `escrow_id4`
has zero C consumers.

**Work.** An escrow table (lifetime per patterns/ref-counting.md
Rule 6 if it is an lfht), metadata-server epoch tracking for
`ceia_mds_epoch` / `cera_mds_epoch` / `ceea_mds_epoch`, and
proof-profile validation for CHUNK_ESCROW_TAKEOVER
(`PROOF_PROFILE_HA_AUTHORITY_ED25519` is mandatory-to-implement
per the draft).  ENUMERATE needs cookie-based paging bounded by
`CHUNK_ESCROW_ENUMERATE_MAX4`.

**Unblocks.** The four error codes in S6.

**Note.** This is the largest item here and deserves its own
design doc before implementation, not just a slice entry.

### S3 -- emit ffv2_device_addr4 for v2 layouts [medium]

**Gap.** R5c added `ffv2_device_versions4` / `ffv2_device_addr4`
with the `ffv2dv_coupling` bitmask, but nothing uses them
(`ffv2_device_addr4`: 0 C uses, `ffv2dv_coupling`: 0).
`lib/nfs4/server/layout.c:179` still builds an FFv1
`ff_device_addr4` with `bool ffdv_tightly_coupled` even when
answering GETDEVICEINFO for a v2 layout, and
`lib/nfs4/client/mds_layout.c:441` decodes it to match.

**Consequence.** The tri-state coupling model
(SYNTHETIC_UIDS / TIGHTLY_COUPLED / TRUSTED_STATEID) is
unreachable; reffs can only express the FFv1 boolean.

**Work.** Flip the v2 GETDEVICEINFO encoder to emit
`ffv2_device_addr4`, map `ds_tight_coupled` onto the bitmask, and
update the client decoder.  Keep the FFv1 path untouched -- this
is another place where the two families sit in one function, so
scope carefully (see the FFv1/FFv2 trap note in
`migration-review.md`).

### S4 -- consume tsa_client_id in TRUST_STATEID [small]

**Gap.** R2d added `uint32_t tsa_client_id` to
`TRUST_STATEID4args`; the handler ignores it (0 C uses).

**Work.** Decide what the field governs -- most plausibly it
binds the trust entry to a specific `cg_client_id` so that a
CHUNK op's guard client-id must match the registered one -- then
store it on the trust entry and check it in the CHUNK validation
hook alongside the existing expiry and principal checks.  Cross-
reference `.claude/design/trust-stateid.md`, which predates the
field.

### S5 -- CHUNK_HEADER_READ handler [medium]

**Gap.** Handler is NFS4ERR_NOTSUPP.  R7b built the full
five-array response shape (`chrr_status` / `chrr_locked` /
`chrr_owners` / `chrr_guards` / `chrr_predecessors`) plus
`optional_retained4` and `retained_predecessor4`; all have zero
consumers, so the shape is currently inert.

**Work.** Populate the five co-indexed arrays from the chunk
store, bounded by `CHUNK_HEADER_READ_MAX4`.  `chrr_predecessors`
needs retained-generation tracking that the chunk store does not
have yet -- that is the real cost of this slice, not the encoding.

**Unblocks.** `NFS4ERR_NO_PREDECESSOR`.

### S6 -- retire the unreachable error codes [tiny, gated]

**Gap.** Four of the five error codes added in R2a are never
returned: `NFS4ERR_NO_PREDECESSOR`, `NFS4ERR_NO_ADOPTABLE_LOCK`,
`NFS4ERR_STALE_ESCROW`, `NFS4ERR_STALE_MDS_EPOCH`.
(`NFS4ERR_ENCODING_NOT_SUPPORTED` is live at 10 sites.)

This is not itself a defect -- the codes are on the wire because
the draft defines them -- but it is the tell for S2 and S5.  When
those land, confirm each code is raised on the path the draft
names.  No standalone work; this is a checklist item to close out
S2/S5.

### Not in this list

Two items are tracked elsewhere and are deliberately not S-slices:

- **`ffv2_coding_type_data4` union restructure** -- marked
  NOT_NOW_BROWN_COW in `lib/xdr/nfsv42_xdr.x`.  Asserted
  wire-identical (every arm carries `ffv2_data_protection4`), so
  it is an API-shape change, not a semantics gap.
- **M2 `chunk_owner4` cohort restructure** -- long-standing, see
  `.claude/design/ffv2-draft-xdr-divergence.md`.  This is the one
  that breaks interop with a batched-cohort peer, and it needs the
  client-side rollback semantic before it can land safely.

### Suggested order

S1 first and on its own -- it is small, it is the one live
correctness gap, and it does not depend on anything else.  Then
S3 (self-contained, unblocks the coupling model).  Then S4.  S2
and S5 are the big ones and should each get a design pass before
implementation; S6 closes out behind them.

## Verification and rollout

- Every reffs slice runs `make -f Makefile.reffs license style
  unit` per project standards.
- Every kernel slice builds on dreamer via HGFS overlay and
  cross-verifies wire with reffs on psyklo or shadow before
  landing.
- After R1-R6 land: update
  `.claude/design/ffv2-draft-xdr-divergence.md` to reflect the
  new alignment surface.
- After K1-K5 land: rebase and squash the kernel branch for
  LKML readiness per the pattern established at task #600.

## References

- Draft (2026-08-05, tip on origin/main after this session's
  review round): `~/Documents/ietf/flexfiles-v2/draft-haynes-nfsv4-flexfiles-v2.md`.
- Existing divergence doc:
  `.claude/design/ffv2-draft-xdr-divergence.md`.
- Trust-stateid design: `.claude/design/trust-stateid.md`.
