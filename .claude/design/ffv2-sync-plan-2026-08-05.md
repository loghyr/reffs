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

**Correction 2026-08-06.**  The original sketch for this slice was
wrong in two ways, found while starting implementation.  Recording
both, because the naive version would have broken working setups.

*First:* the draft defines enforcement in **two directions**, and
the sketch described only one.  Re-reading sec-ops-client and
sec-data-file-identification:

- **(A)** A data server that has identified a file **as chunked**
  MUST reject *non-CHUNK* operations on it with NFS4ERR_NOTSUPP.
  This is the direction sec-ops-client leads with, and the sketch
  missed it entirely.
- **(B)** A data server that has identified a file as **non-chunked**
  (`fattr4_chunked_data_file` = FALSE) MUST reject *CHUNK*
  operations with NFS4ERR_NOTSUPP.  This is what the sketch
  described.

*Second:* the sketch's rule -- "reject CHUNK ops when the bit is
clear" -- conflates "the metadata server marked this FALSE" with
"this file was never identified at all".  The draft is explicit
that those are different: sec-data-file-identification says the
MUST-reject rules "apply on the data server side only to files the
data server has identified", and a data server that cannot identify
a file "relies on the client-side MUST NOT as the primary defense".
An unidentified file must **not** be rejected.

reffs's single `INODE_IS_CHUNKED_DATA_FILE` bit cannot express that
three-way distinction (chunked / non-chunked / unidentified); 0
currently means both "FALSE" and "never set".

**Why this matters concretely.**  Only `dstore_ops_nfsv4` sets
attribute 90 (that is what R4 added).  `dstore_ops_local` and
`dstore_ops_nfsv3` do not.  Combined mode uses `dstore_ops_local`
(`lib/nfs4/dstore/dstore.c:516`), and `scripts/test_mirror_local.sh`
drives `ec_demo --layout v2` -- real CHUNK ops -- against exactly
that path.  So a naive "reject when the bit is clear" would return
NFS4ERR_NOTSUPP for every CHUNK op in combined mode, breaking the
mirror test, the v2 benchmark variants, and the ec_demo v2 flow.

**Revised work.**
1. Decide how to represent "identified" separately from
   "identified as FALSE".  Options: a second `i_attr_flags` bit
   (`INODE_CHUNKED_ATTR_PRESENT`), or treating a live TRUST_STATEID
   entry as the identification path (the draft lists it as the
   second-authority means, which reffs already has infrastructure
   for), or gating per export.
2. Implement direction (B) using that representation.
3. Implement direction (A) -- reject non-CHUNK ops on identified
   chunked files -- which is a separate and wider change, since it
   touches READ / WRITE / SETATTR / etc. on the data-server path.
4. Consider extending attribute 90 to `dstore_ops_local` and
   `dstore_ops_nfsv3` so combined and v3-backed setups can be
   identified too, rather than permanently living in the
   unidentified fallback.

Item 1 is the real decision and should be settled before any code.
This slice is **larger than "small"** as originally scoped, and
step 3 may deserve its own slice.

**Tests.** Once the representation is settled: CHUNK ops against an
identified-non-chunked file -> NFS4ERR_NOTSUPP; against an
identified-chunked file -> unchanged; against an **unidentified**
file -> unchanged (explicitly not rejected).  Plus a combined-mode
regression asserting `test_mirror_local.sh` still passes.

### S1b -- reject client SETATTR of attribute 90 [small, do with S1]

**Gap.** Found by the fable audit 2026-08-06.  The draft
(sec-fattr4_chunked_data_file, :8311-8313) says: "Clients MUST NOT
SETATTR this attribute; a data server MUST reject a client SETATTR
of FATTR4_CHUNKED_DATA_FILE with NFS4ERR_INVAL."

R3 added `FATTR4_CHUNKED_DATA_FILE` to `nattr_is_settable()`
(`lib/nfs4/server/attr.c:2520`), which `nattr_from_fattr4()` applies
to **both** the OPEN-createattrs path and the SETATTR path with no
caller discrimination.  Any client can therefore set or clear the
bit.  This is a MUST-level draft violation in shipped code.

**Why it pairs with S1.**  It makes S1's gap strictly worse rather
than merely adjacent: once the CHUNK-op guard lands, a client that
can still SETATTR the bit can strip the classification off a chunked
file and walk straight through the new check, or plant it on a
PASSTHROUGH file.  Landing S1 without S1b would build a lock and
leave the key in it.

**Work.** Keep acceptance on the OPEN(CREATE) createattrs path --
R4 depends on it -- and reject on the SETATTR entry path.  Note the
draft specifies NFS4ERR_INVAL, not NFS4ERR_ATTRNOTSUPP, so simply
removing the attribute from `nattr_is_settable()` is the wrong lever
(that path returns ATTRNOTSUPP).  The alternative shape worth
considering is gating on control-session identity, the way
TRUST_STATEID gates on `EXCHGID4_FLAG_USE_PNFS_MDS` -- that would
also let a genuine metadata server re-SETATTR the bit, which the
create-path-only rule forbids.

**Also resolves** NOTE 6 from the fable audit: the
`suppattr_exclcreat` comment at `attr.c:2361` ("Must match
nattr_is_settable() exactly") is currently inaccurate because
attribute 90 is in one list and not the other.  Whichever shape S1b
takes decides how that comment reconciles.

**Tests.** Client SETATTR of attribute 90 -> NFS4ERR_INVAL; metadata
server OPEN(CREATE) with attribute 90 in createattrs -> still
succeeds (R4 regression).

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

### S7 -- test the R7c behavioral change and the attribute-90 round-trip [small]

**Gap.** Found by the fable audit 2026-08-06.
`lib/nfs4/tests/chunk_test.c` contains **zero** references to
`cr_locked`, `cr_guard`, or `CHUNK_STATE_FLAGS_LOCKED`.  R7c was the
only slice in the whole R1-R7 range that changed runtime behavior,
and nothing covers it.

**Why it matters more than it looks.**  Existing tests exercise only
unlocked blocks, where the calloc'd 0 happens to be the correct
answer.  So a regression that stops populating `cr_locked` entirely
would still pass 25/25 on dreamer.  The green result that gated the
R7 landing is weaker evidence than it appeared.

**Work.**
- CHUNK_READ against a block with `CHUNK_BLOCK_LOCKED` set asserts
  `cr_locked == CHUNK_STATE_FLAGS_LOCKED`; against an unlocked block
  asserts 0.
- Assert the `cr_guard` dual-write equals `cr_owner.co_guard`.
- SETATTR/GETATTR round-trip of attribute 90.  Write this one to the
  **post-S1b** contract, since S1b changes what a client SETATTR is
  allowed to do.

### S8 -- naming and diagnostic cleanup [small, mechanical]

Collected NOTEs from the fable audit; none affect the wire.

- **`data_block_read` failure is reported as success**
  (`lib/nfs4/server/chunk.c:948-953`): the return value is ignored, so
  a failed read yields a zero-filled `cr_chunk` with
  `cr_status = NFS4_OK` and no LOG line.  Only CRC32-verifying clients
  notice.  Pre-existing, but R7c made `cr_status` a per-chunk scalar,
  which is exactly the field this needs -- set `NFS4ERR_IO`.  *This is
  the one item here with real behavioral value; consider splitting it
  out if S8 gets deferred.*
- Stale comments naming retired identifiers (`ffv2l_local`,
  `ffm_coding_type`, `ffm_checksum_algorithm`,
  `ffm_striping_unit_size`) across ~12 sites in `nfsv42_xdr.x`,
  `layout.c`, `probe1_server.c`, `ps/chunk_io.c`, `ec_client.h`,
  `layout_segment.h`, `settings.h`, and three test files.  Doc-only,
  but they break grep for anyone tracing a field.
- Stale draft anchor `sec-encoding-mirrored` cited at
  `lib/include/reffs/ec.h:130` and `lib/ec/tests/mirror_test.c:11`;
  the draft's anchor is now `sec-encoding-replicated`.
- Dead reffs-only `enum ffv2_protection_type` /
  `FFV2_PROTECTION_TYPE_MIRRORED` (`nfsv42_xdr.x:5094-5098`): absent
  from the draft, zero consumers, retired terminology.  Remove or
  mark reserved.
- `op_errors[]` has no entries for the four CHUNK_ESCROW ops and none
  of the five new error codes appear in an `ne_allowed` list.
  Test-only consumer today; fold into S2 rather than doing it here.

### Draft-side follow-ups (not reffs slices)

Two items land in `draft-haynes-nfsv4-flexfiles-v2`, not here:

- **Status-only responses should use the RFC 8881 struct form.**  Both
  reviewers independently reached this.  RFC 8881 itself uses
  `struct { nfsstat4 status; }` for PUTFH4res / SAVEFH4res /
  RENEW4res / DELEGPURGE4res.  The draft's all-void discriminated
  union is wire-identical but nonstandard for an NFSv4 document, and
  lowers to an empty C union that strict clang rejects -- reffs
  already had to work around it twice.  Affects
  `CHUNK_REPAIRED4res`, `CHUNK_UNLOCK4res`,
  `CHUNK_ESCROW_RELEASE4res`, `CHUNK_ESCROW_TAKEOVER4res`.
- **Convention text nuance**: `ffv2ie_` (`ffv2_ioerr4`) does not
  literally follow the stated one-letter-per-word rule, since "ioerr"
  is one word -- it is inherited from RFC 8435's `ffie_`.  A
  half-sentence acknowledging inherited initialisms would close the
  gap between the stated rule and actual practice.

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

**S1b first, alone.**  It is small, unambiguous, and closes a
MUST-level violation in shipped code.  It is now landed.

**S1 needs a design decision before any code** -- see the 2026-08-06
correction in the slice: representing "identified" separately from
"identified as FALSE", and the fact that enforcement runs in two
directions, not one.  Do not start it as a "small" slice.

Original note, kept for context: S1 and S1b are the one live correctness
gap and they must land as a pair -- S1 without S1b builds the
CHUNK-op guard and leaves clients able to SETATTR their way around
it.  Both are small.

Then S7 (tests the already-shipped R7c behavior; small, and it
retires the false confidence in the current 25/25).  Then S3
(self-contained, unblocks the coupling model).  Then S4.

S2 and S5 are the large ones and should each get a design pass
before implementation; S6 closes out behind them.  S8 is
opportunistic cleanup -- except the `data_block_read` status item,
which has real diagnostic value and can ride with S7.

The two draft-side follow-ups are independent of all of the above
and can go whenever the draft is next opened.

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
