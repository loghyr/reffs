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

### S1 -- fattr4_chunked_data_file: settle it, freeze it, enforce it

Rewritten 2026-08-06 against the draft invariant added in
`e5871d41fb16`.  The earlier version of this slice was wrong twice
over; both errors and the reasoning are kept at the end so the
correction is auditable.

The draft now states: the attribute describes the **format of the
file's content**, so its value is fixed once the file holds data.
A metadata server MUST NOT change it on a non-empty file, and a
data server MUST reject such a SETATTR with NFS4ERR_INVAL --
including one arriving over the metadata server's control session.
A metadata server that allocates data files before choosing their
encoding MAY set the final value at any point while the file is
still empty.  How emptiness is determined is explicitly left to the
implementation.

That reframes the whole slice.  The attribute is not a label to be
enforced against; it is a commitment made once, before any data
exists, and honored thereafter.  The work splits along that line.

#### S1.1 -- settle the value while the file is empty [small]

R4 sets the attribute TRUE from `nfsv4_create`, which is reached
through `runway_batch_create` -- the runway **pre-create** path.
The runway builds a pool of empty files at startup; `runway_pop`
hands one out later at LAYOUTGET, which is where
`default_coding_resolve_segment()` finally picks the encoding.

So today every NFSv4.2 runway file is marked TRUE before anyone
knows whether it will hold chunks or passthrough data.  The
attribute is therefore *wrong* on passthrough files right now, not
merely unenforced -- enforcement built on it today would reject
correct client behavior.

Under the new invariant this is not a misplacement to relocate but
a value left unsettled.  Runway files are empty until popped, so
the correction is legal exactly where it is needed: at layout
assignment, set the attribute to its final value -- TRUE for a
chunked encoding, FALSE for PASSTHROUGH -- while the file still
holds no data.

**Implementation design (mapped 2026-08-06, not yet built).**

The layout-assignment path today is one loop (`layout.c:1626-1648`)
that, per file, pops from the runway and then calls
`dstore_data_file_fence()` and `dstore_data_file_chmod()` -- two
SETATTR round-trips per file.  The encoding is not resolved until
`default_coding_resolve_segment()` at `:1693`, *after* that loop,
because it needs `nfiles` (how many files the runway actually
yielded).

That ordering is the whole difficulty.  Settling the attribute
where the encoding becomes known means a **third** SETATTR per
file on every LAYOUTGET -- for a 4+2 layout, six extra round-trips,
raising LAYOUTGET's data-server traffic by half.  That is a real
hot-path cost, not an implementation detail.

The right shape is therefore a restructure rather than an addition:

    pop loop           (fill files[], count nfiles)
    resolve encoding   (default_coding_resolve_segment)
    settle loop        (fence + chmod + attribute 90, one SETATTR)

With that ordering, attribute 90 rides the SETATTR that fence
already sends and costs nothing extra.  It also puts the fence and
chmod after the geometry check, which incidentally narrows the
existing NOT_NOW_BROWN_COW at `:1697` -- today a geometry shortfall
returns LAYOUTUNAVAILABLE *after* the files were already fenced and
chmod'd, leaking them; deferring those calls until after the check
means a shortfall leaks unmodified pool files instead.

Required pieces:

- A `set_chunked` entry in `struct dstore_ops`
  (`lib/include/reffs/dstore_ops.h`), or an extension of `fence` to
  carry the attribute, so the value can be set through the vtable.
- `dstore_ops_nfsv4.c`: SETATTR carrying attribute 90, modelled on
  `nfsv4_truncate`'s bitmap/attrlist encoding.  Word 2, bit 26.
- `dstore_ops_local.c`: set `INODE_IS_CHUNKED_DATA_FILE` directly;
  this is also S1.3.
- `dstore_ops_nfsv3.c`: leave NULL -- v3 data files are non-chunked
  by construction.
- `layout.c`: the loop split described above.

Decide before building: whether to fold the attribute into `fence`
(fewer moving parts, but overloads an operation whose name means
credential rotation) or add a distinct vtable op (clearer, but the
loop restructure is required either way to avoid the third
round-trip).

Also settle what happens to R4's create-time TRUE.  Simply deleting
it is safe on its own terms -- an absent attribute is "unidentified",
which the draft permits and which is strictly better than today's
wrong value -- but it would leave the attribute unset until the
settle loop lands, so the two should land together.

Tests: a passthrough layout leaves the attribute FALSE on its data
files; a chunked layout leaves it TRUE.

#### S1.2 -- reject SETATTR on a non-empty file, from either origin [small]

S1b already rejects **client** SETATTR of the attribute
unconditionally (landed, `e6058df22df4`).  The draft's new rule is
wider: a SETATTR against a **non-empty** file MUST be rejected with
NFS4ERR_INVAL whoever sends it, metadata server included.

Work: add the non-empty check.  reffs must choose its own emptiness
predicate -- the draft deliberately does not specify one.  The
conservative reading is the union of "has file content" and "has
any non-EMPTY chunk-store block", since a chunked data file can
plausibly hold chunk state while its plain size is uninformative,
and a false "empty" is the dangerous direction: it would admit a
format change to a file that already holds data.  Confirm against
`lib/nfs4/server/chunk_store.c` before settling on the predicate.

Tests: SETATTR of the attribute on a populated file -> NFS4ERR_INVAL,
exercised from both a client compound and a control-session
compound; on an empty file from the control session -> permitted.

#### S1.3 -- close the combined-mode identification gap [small]

Only `dstore_ops_nfsv4` sets the attribute.  `dstore_ops_local`
does not, so every combined-mode data file is unidentified.
Enforcement that has to special-case which vtable created a file
is the kind of conditional that rots, so close this before any
enforcement lands.

NFSv3 dstores are **not** in scope and need no gate: CHUNK
operations are NFSv4.2 operations, and chunked encodings require
tight coupling (B4.1), which NFSv3 cannot provide.  A v3-backed
data file is non-chunked by construction, not "unidentified pending
a decision."

#### S1.4 -- represent "unidentified" distinctly [small, gated on S1.1-S1.3]

Only once the above land is this still needed, and possibly not.
`sec-data-file-identification` says the MUST-reject rules apply
only to files the data server has **identified**, and that a data
server which cannot identify a file relies on the client-side MUST
NOT instead.  A single bit cannot say chunked / non-chunked /
unidentified -- 0 currently means both of the last two.

If S1.1 and S1.3 together mean every data file reffs creates is
explicitly settled, then "unidentified" becomes an empty category
for files reffs allocated, and no second bit is needed.  Decide at
that point rather than now.  If it is still needed, a second
`i_attr_flags` bit (`INODE_CHUNKED_ATTR_PRESENT`) is the cheapest
form -- persistent, unlike the trust table, which a data-server
restart clears and which would make enforcement evaporate
unpredictably.

#### S1.5 -- enforce, in two directions [medium; (A) deserves its own review]

The draft enforces both ways, and the earlier version of this slice
described only one:

- **(B)** A data server that has identified a file as **non-chunked**
  MUST reject **CHUNK** operations on it with NFS4ERR_NOTSUPP.
  Contained; the CHUNK handlers are one file.
- **(A)** A data server that has identified a file as **chunked**
  MUST reject **non-CHUNK** operations on it with NFS4ERR_NOTSUPP.
  This is the direction `sec-ops-client` leads with.  Much wider
  blast radius -- READ, WRITE, SETATTR, COMMIT on the data-server
  path -- and it should be its own slice with its own review and
  its own dreamer run.

Do (B) first.  Do not fold (A) into the same commit.

**(A) LANDED.**  The gate is in the dispatch loop
(`lib/nfs4/server/dispatch.c`), not in the handlers, because the
rule covers every operation rather than a named few.

The draft states the rule default-deny -- restrict what is sent to
the listed operations, reject anything else -- so the code is an
allowlist, `op_allowed_on_chunked_data_file()`.  That also fails
safe: an operation added to `op_table` later is refused on chunked
data files until someone decides it belongs, rather than
inheriting permission silently.  Allowed: session and filehandle
plumbing, GETATTR, the CHUNK family, and the three trust ops --
those last because the draft wants NFS4ERR_PERM from their own
guard, and gating them here would answer NFS4ERR_NOTSUPP instead.

The gate sits inside the known-opcode branch, so an unknown opcode
still reaches `nfs4_op_illegal` and is answered NFS4ERR_OP_ILLEGAL
rather than being reported as a chunked-file violation.

The control session is exempt: the restriction is on what a client
sends, and the metadata server reaches the same files to do what
the draft assigns to it.  Identified as in S1.6.

Two mutations on dreamer:

| mutation | result |
|---|---|
| gate keyed on UNIDENTIFIED instead of YES | 3 red, in both directions -- the chunked file stops being refused and the unidentified file starts being refused |
| control-session exemption removed | 1 red |

**Debt (A) leaves.**  The gate consults only the current
filehandle.  Operations whose current filehandle is the parent
directory -- REMOVE, RENAME, LINK, CREATE -- are not caught, and
neither is a chunked file that is only the saved filehandle
(LINK's source).  The draft forbids those against a data file too.
Catching them needs name resolution or a saved-filehandle check,
which is a separate slice.

The ACL-scoped attribute bits (FATTR4_ACL, _DACL, _SACL) are
forbidden on a data file per-bit rather than per-operation, so
GETATTR stays allowed as a whole and the bit-level rule is a
NOT_NOW_BROWN_COW in the allowlist.

**Not yet verified end to end.**  Same gap S1.6 leaves: the unit
tests drive `dispatch_compound()` directly.  A live metadata
server plus data server would show the settle, the identification,
and both enforcement directions working together.

**(B) LANDED ab4f1509c25a.**  The gate is
`chunk_op_on_non_chunked()` in `lib/nfs4/server/chunk.c`, keyed on
`inode_chunked_state()` so UNIDENTIFIED stays unenforced.  Six call
sites cover the seven working ops:
`chunk_write_validate_payload` carries it for CHUNK_WRITE and
CHUNK_WRITE_REPAIR; CHUNK_READ, CHUNK_FINALIZE, CHUNK_COMMIT,
CHUNK_ROLLBACK and CHUNK_REPAIRED carry it in their own prologues.

Two mutations on dreamer confirmed the tests are load-bearing:
forcing the gate off turns 2 tests red, and encoding the naive
"UNIDENTIFIED means FALSE" rule turns **33** red across the whole
chunk suite -- so the failure mode the plan predicted below is not
hypothetical, and it is not quiet either.

**Debt (B) leaves:** CHUNK_ERROR, CHUNK_HEADER_READ, CHUNK_LOCK and
CHUNK_UNLOCK are unconditional `NFS4ERR_NOTSUPP` stubs.  They already
return the right answer for a non-chunked file, so the gate would be
dead code today -- but S5 implements CHUNK_HEADER_READ, and that
slice must add the gate as part of the implementation or it will
silently open a hole.  Same for S2's escrow ops if they become
reachable.

#### S1.6 -- control-session identification [small; LANDED f1b2d0c3d326]

Found while scoping S1.5(A), because (A) needs the same "is this
the metadata server?" predicate that the attribute-90 gate uses.

`ds_session_create` left `mds_session_create` at its USE_NON_PNFS
default, so the metadata-server-to-data-server session presented the
same EXCHANGE_ID flag an ordinary client does.  Two consequences,
both live:

1. The attribute-90 settle step at layout assignment is an ordinary
   SETATTR over that session, and its gate checks USE_PNFS_MDS only.
   The settle was answered NFS4ERR_INVAL, so no file served from a
   remote NFSv4.2 dstore was ever identified as chunked and S1.5(B)
   -- which sits downstream of identification -- could not fire on
   that path.  S1.5(B)'s tests set the flag by hand, so they passed
   over the gap.  Combined mode was never affected:
   `local_set_chunked` sets the inode flags directly and never
   crosses the wire, so it never met the gate.
2. `require_mds_client()` had been widened to accept USE_NON_PNFS to
   get the control plane working again.  Since that is what a client
   presents, any client on the data server could register trust
   entries and revoke another client's via BULK_REVOKE_STATEID.

Fix: ask for USE_PNFS_MDS in `ds_session_create`, narrow the guard
back to it.  `sec-tight-coupling-control-session` is explicit that
this flag is the sole access control on the three trust ops.

Mutation: restoring the widened guard turns exactly the three new
tests red, each reporting NFS4_OK where NFS4ERR_PERM is required --
the plain client's control-plane call succeeding.

**Still open.** The flag is a self-declaration; it separates a
metadata server from a client that follows the protocol, not from
one that does not.  The draft puts the real control in the
deployment (GSS machine principal, TLS client certificate, or an
isolated control-session network).  The reffs-side match is a local
allowlist like `[[allowed_ps]]`; not written.

**Not yet verified end to end.** The unit tests cover the guard.
That the settle now succeeds over a real NFSv4 dstore, and that
files consequently come out identified, needs a live metadata
server plus data server -- the natural place is the S1.5(A) run.

#### Ordering

S1.1 -> S1.2 -> S1.3 -> (S1.4 if still required) -> S1.5(B) ->
S1.6 -> S1.5(A).  S1.1 is independently worth doing even if
enforcement is never built: the attribute is currently wrong on
passthrough files, and a wrong label is worse than an absent one
because it invites exactly the enforcement that would then misfire.

S1.6 landed after S1.5(B) because that is when it was found, but it
gates both: until the control session is identifiable, nothing sets
attribute 90 over the wire and neither enforcement direction has
anything to act on.

#### What the earlier version of this slice got wrong

Recorded because both errors would have shipped breakage.

1. It described enforcement in one direction only, missing (A) --
   the direction the draft's own operations section leads with.
2. Its rule, "reject CHUNK ops when the bit is clear", conflated
   "marked FALSE" with "never identified".  Files reach a data
   server by paths that never settle the attribute -- an NFSv3
   dstore has no `set_chunked` entry at all -- and the naive rule
   answers NFS4ERR_NOTSUPP for every CHUNK operation against them.
   The mutation measured 33 failures across the chunk suite.

   Correction (2026-08-06, from the review of `f1b2d0c3d326`):
   an earlier version of this entry blamed combined mode, on the
   belief that `dstore_ops_local` never settles the attribute.  It
   does -- `local_set_chunked` sets the same inode flags the wire
   path sets, deliberately, so that enforcement cannot tell
   combined mode from a real data server.  The naive rule is still
   wrong, and `scripts/test_mirror_local.sh` and the v2 benchmark
   variants still exercise the code it would have broken; the
   reason is unsettled files generally, not combined mode.

It was also scoped "small".  It is five slices.

### S1b -- reject client SETATTR of attribute 90 [LANDED e6058df22df4]

**Status.** Landed 2026-08-06.  Superseded in part by S1.2, which
widens the same rejection to any SETATTR against a non-empty file
regardless of origin -- S1b rejects client SETATTR unconditionally,
which remains correct and is the narrower rule.

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

**What it governs -- settled, from the draft.**  The guess in the
earlier version of this entry was right, and the draft is
explicit at two sites: the metadata server registers the writer's
`ffv2m_client_id` via `tsa_client_id`, the data server compares
the client id presented on a CHUNK operation against it, and a
mismatch MUST be rejected with NFS4ERR_BAD_STATEID -- "a client
that presents a cwa_client_id different from its layout's
ffv2m_client_id is spoofing another writer's identity."

In reffs the presented value is `chunk_guard4.cg_client_id`, not
`cwa_client_id`: reffs is still on the single-owner shape and the
draft's batched-cohort restructure is the deferred M2 item.  The
comparison is the same one.

**This is two slices, and the order matters.**

S4a (metadata server, must land first):
:  `layout.c:1045` hardcodes `mirror->ffv2m_client_id = 0` and the
   TRUST_STATEID fanout never sets `tsa_client_id`.  The metadata
   server has to assign a real per-writer identity and carry it
   into the fanout.  Open question that makes this a design item
   rather than a mechanical change: what generates the id, whether
   it is stable across successive LAYOUTGETs by the same client,
   and how it is allocated when several clients hold layouts on
   one file.  The clientid4 slot field is the obvious source.

S4b (data server): store `tsa_client_id` on the trust entry and
   compare it in the CHUNK validation hook.

**Landing S4b first would break the data path.**  Every trust
entry today registers 0 (CHUNK_GUARD_CLIENT_ID_NONE), while a
conforming client must present a non-sentinel `cg_client_id`, so
a strict comparison would reject every CHUNK operation with
NFS4ERR_BAD_STATEID.

If S4b is wanted before S4a, the safe form is to treat a
registered NONE as "no binding recorded" and skip the comparison,
exactly as an empty `te_principal` means no principal constraint.
That makes enforcement switch on by itself the moment S4a starts
supplying real ids.  It is a deliberate permissiveness, not
conformance: the draft does not contemplate a metadata server
that registers the reserved value.

Cross-reference `.claude/design/trust-stateid.md`, which predates
the field.

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

**Also required.** Add the S1.5(B) gate.  CHUNK_HEADER_READ is one
of the ops the draft names, and today its unconditional
`NFS4ERR_NOTSUPP` happens to satisfy the rule by accident.  The
moment this slice makes the handler answer for real, the accident
stops covering it: call `chunk_op_on_non_chunked()` after the
filehandle and inode checks, and add a reject test alongside the
existing four in `chunk_test.c`.

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

**S1b is landed** (`e6058df22df4`) -- client SETATTR of attribute
90 is rejected.

**S1 is now five sub-slices** (S1.1-S1.5), rewritten 2026-08-06
against the draft invariant in `e5871d41fb16`.  Start with **S1.1**:
the attribute is currently *wrong* on passthrough data files, since
R4 sets it TRUE at runway pre-create, before the encoding is known.
A wrong label is worse than an absent one, because it invites the
enforcement that would then misfire.  S1.1 is worth doing even if
enforcement is never built.

Then S1.2, S1.3, and only then decide whether S1.4 is still needed.
Enforcement (S1.5) comes last, direction (B) before (A), never in
one commit.

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
