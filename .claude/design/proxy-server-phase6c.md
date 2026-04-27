<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Proxy Server Phase 6c: PS-side CB receive path (XDR + dispatch + handler)

> **Status note (2026-04-26)**: this document was originally
> structured around CB_PROXY_* (back-channel) ops and a CB-receive
> infrastructure on the PS.  A subsequent design conversation with
> the draft author concluded that the back-channel architecture is
> wrong for the PS use case -- the PS work-assignment + status
> reporting are all fore-channel, with the MDS replying to PS-
> issued `PROXY_PROGRESS` polls.  The CB_PROXY_* ops landed in
> slice 6c-i (op numbers 95-98) need to be walked back.  See
> "Architecture revision (2026-04-26)" at the end of this doc for
> the new design.  The slice 6c-ii / 6c-iii sketches above the
> revision section are **superseded**; the slice ladder restarts
> with new slice names (6c-w through 6c-z).  Slice 6c-i's XDR
> additions are also revised -- see the revision for the new shape.

## Context

Phase 6b sub-slices (allowlist, bypass, squat-guard, mTLS) all landed
on origin/main.  PROXY_REGISTRATION's fore-channel surface is feature
complete for the AUTH_SYS-rejection / GSS-or-TLS identity model.  The
PROXY_PROGRESS handler is still a NFS4ERR_NOTSUPP stub because no
MDS-initiated CB ops exist for the PS to report progress on.

Phase 6c finishes the original "Phase 6: PROXY_REGISTRATION + minimal
CB back-channel" milestone in `proxy-server.md` by:

1. Defining the four MDS->PS callback ops in the protocol XDR
   (CB_PROXY_STATUS / CB_PROXY_MOVE / CB_PROXY_REPAIR / CB_PROXY_CANCEL).
2. Adding the PS-side CB-receive RPC entry point so an MDS-issued
   CB_COMPOUND can land at the PS.
3. Wiring CB_PROXY_STATUS to return "idle" (the simplest stub the
   protocol allows); MOVE / REPAIR / CANCEL stay NFS4ERR_NOTSUPP
   until phase 8.

## Sub-slicing

| Slice | Adds | Risk |
|-------|------|------|
| **6c-i** (this slice) | XDR-only: op numbers, arg/res structs, union arms in `nfs_cb_argop4` / `nfs_cb_resop4`.  Same "wire-allocated for op-number stability" pattern slice 6a used for PROXY_PROGRESS.  No handler dispatch wiring. | low -- protocol XDR addition only |
| **6c-ii** | PS-side CB-receive RPC infrastructure (CB program registration on the `:4098` listener, CB_COMPOUND parser, CB op dispatcher, CB_COMPOUND4res encoder).  All four CB_PROXY_* handlers stub NFS4ERR_NOTSUPP. | high -- net-new RPC entry point |
| **6c-iii** | CB_PROXY_STATUS handler returns "idle" (replaces the NFS4ERR_NOTSUPP stub from 6c-ii). | low |

This document plans **6c-i** in detail (the next slice to ship) and
sketches 6c-ii / 6c-iii enough to confirm the XDR shape will support
them.

---

## 6c-i: XDR-only addition (this slice)

### Goal

Reserve op numbers 95-98 for the four CB_PROXY_* ops in
`lib/xdr/nfsv42_xdr.x` so the wire format is stable from now on.
A future slice 6c-ii can wire the dispatch path against this XDR
without churning op numbers (which would be observable to deployed
clients/PSes once the receive path lands).

This mirrors slice 6a's treatment of OP_PROXY_PROGRESS: defined in
the wire, no handler yet.

### Op numbers (TBD pending IANA / draft)

```
OP_CB_PROXY_STATUS  = 95
OP_CB_PROXY_MOVE    = 96
OP_CB_PROXY_REPAIR  = 97
OP_CB_PROXY_CANCEL  = 98
```

Same TBD caveat as OP_TRUST_STATEID (90-92) and OP_PROXY_REGISTRATION
(93-94) -- the data-mover draft has not yet been assigned IANA
numbers, so reffs picks sequential values after the trust-stateid /
proxy fore-channel block on the assumption that the data-mover
draft lands after flexfiles-v2 on the IETF datatracker.

### XDR struct shape

The four ops follow the data-mover draft.  Args + res are kept
minimal-but-extensible -- enough wire shape that 6c-ii's dispatcher
can decode them cleanly, with `cb_proxy_*_flags` reserved fields so
future slices can add capability bits without breaking on-wire
compatibility.

```xdr
%/*
% * MDS-to-PS CB ops (draft-haynes-nfsv4-flexfiles-v2-data-mover).
% * Op numbers 95-98 are TBD pending IANA assignment; same caveat as
% * the proxy fore-channel block above.  Slice 6c-i wires the XDR
% * shape only -- the PS-side receive path arrives in slice 6c-ii.
% *
% * CB_PROXY_STATUS asks the PS to report its current operation
% * status (idle, busy with N inflight, etc.).  CB_PROXY_MOVE +
% * CB_PROXY_REPAIR delegate whole-file move / repair work to the
% * PS.  CB_PROXY_CANCEL asks the PS to abort an in-progress move
% * or repair.
% */

const PROXY_OPERATION_ID_MAX = 32;

enum proxy_op_status4 {
    PROXY_OP_STATUS_IDLE     = 0,
    PROXY_OP_STATUS_BUSY     = 1,
    PROXY_OP_STATUS_DRAINING = 2
};

/* CB_PROXY_STATUS -- query PS health */
struct CB_PROXY_STATUS4args {
    uint32_t cpsa_flags; /* MUST be 0 (RFC 8178 reserved) */
};
struct CB_PROXY_STATUS4resok {
    proxy_op_status4 cpsr_status;
    uint32_t cpsr_inflight; /* current count of in-progress moves/repairs */
};
union CB_PROXY_STATUS4res switch (nfsstat4 cpsr_status_code) {
 case NFS4_OK:
     CB_PROXY_STATUS4resok cpsr_resok;
 default:
     void;
};

/*
 * CB_PROXY_MOVE / CB_PROXY_REPAIR -- ask the PS to relocate or
 * repair a file.  Argument bodies are minimal in this slice; a
 * future slice will add the source/destination layout descriptors
 * once the handler is real (today the handler returns NOTSUPP so
 * the body is unread).  An operation_id is included so future
 * CB_PROXY_CANCEL can address a specific in-flight op.
 */
struct CB_PROXY_MOVE4args {
    uint32_t cpma_flags;
    opaque   cpma_operation_id<PROXY_OPERATION_ID_MAX>;
};
struct CB_PROXY_MOVE4res {
    nfsstat4 cpmr_status;
};

struct CB_PROXY_REPAIR4args {
    uint32_t cpra_flags;
    opaque   cpra_operation_id<PROXY_OPERATION_ID_MAX>;
};
struct CB_PROXY_REPAIR4res {
    nfsstat4 cprr_status;
};

/* CB_PROXY_CANCEL -- abort an in-progress move or repair */
struct CB_PROXY_CANCEL4args {
    uint32_t cpca_flags;
    opaque   cpca_operation_id<PROXY_OPERATION_ID_MAX>;
};
struct CB_PROXY_CANCEL4res {
    nfsstat4 cpcr_status;
};
```

### Union arms

In `nfs_cb_argop4`:

```xdr
case OP_CB_PROXY_STATUS:  CB_PROXY_STATUS4args  opcbproxy_status;
case OP_CB_PROXY_MOVE:    CB_PROXY_MOVE4args    opcbproxy_move;
case OP_CB_PROXY_REPAIR:  CB_PROXY_REPAIR4args  opcbproxy_repair;
case OP_CB_PROXY_CANCEL:  CB_PROXY_CANCEL4args  opcbproxy_cancel;
```

In `nfs_cb_resop4`:

```xdr
case OP_CB_PROXY_STATUS:  CB_PROXY_STATUS4res   opcbproxy_status;
case OP_CB_PROXY_MOVE:    CB_PROXY_MOVE4res     opcbproxy_move;
case OP_CB_PROXY_REPAIR:  CB_PROXY_REPAIR4res   opcbproxy_repair;
case OP_CB_PROXY_CANCEL:  CB_PROXY_CANCEL4res   opcbproxy_cancel;
```

### Tests (TDD -- slice scope)

This slice is XDR-only; there is no behaviour to test yet.  The
test discipline is "the suite still builds and existing tests still
pass" -- the regenerated XDR header MUST compile cleanly into every
consumer (server, client, ec_demo, probe), and the pre-existing
test suite MUST be unchanged.

Optional: a tiny encode/decode roundtrip test for each new
arg/res struct in `lib/xdr/tests/` would catch a future XDR
generator regression.  Defer unless there is appetite.

### Test impact

| File | Impact |
|------|--------|
| All existing tests | PASS -- additive XDR only |
| `lib/xdr/build/nfsv42_xdr.[ch]` | regenerated; no manual edits |

### Files changed (6c-i)

| File | Change |
|------|--------|
| `lib/xdr/nfsv42_xdr.x` | 4 new op numbers, 4 new arg/res structs, 8 new union arms |
| `.claude/design/proxy-server-phase6c.md` | This file |

No C code changes -- the regenerated XDR header is consumed by the
existing CB infrastructure but not yet referenced.

### Approval

Per `.claude/roles.md` rule 9, modifications to `lib/xdr/nfsv42_xdr.x`
are normally a BLOCKER.  This slice is **explicitly approved by the
draft author** (the user authored
draft-haynes-nfsv4-flexfiles-v2-data-mover and asked for the CB ops
to be added).  Same approval pattern as the trust-stateid XDR
addition that landed earlier on this branch.

---

## 6c-ii sketch (next-after-this)

The PS-side CB-receive infrastructure.  The PS's `:4098` listener
already accepts NFSv4.2 fore-channel compounds; 6c-ii adds a parallel
NFS4_CALLBACK program (1) on the same listener (the protocol allows
the same connection to carry both fore-channel and back-channel
traffic per RFC 8881 S2.10.3).  Decode CB_COMPOUND, dispatch each
inner op to a handler keyed by `cb_argop`, encode CB_COMPOUND4res.

Per-op handlers all stub NFS4ERR_NOTSUPP for slice 6c-ii; the only
non-stub piece is the dispatcher itself plus the CB_SEQUENCE handler
(needed to consume the slot).

## 6c-iii sketch

CB_PROXY_STATUS handler returns:
```c
res->cpsr_status_code = NFS4_OK;
res->cpsr_resok.cpsr_status = PROXY_OP_STATUS_IDLE;
res->cpsr_resok.cpsr_inflight = 0;
```

Until phase 8 lands CB_PROXY_MOVE / CB_PROXY_REPAIR handling, the PS
genuinely is always idle, so the constant reply is correct.

## Deferred from this slice

- CB receive infrastructure (slice 6c-ii).
- Real CB_PROXY_STATUS reply (slice 6c-iii).
- CB_PROXY_MOVE / REPAIR / CANCEL handlers (phase 8).
- Argument extension for CB_PROXY_MOVE / REPAIR (source / dest
  layout descriptors) -- the current shape is a placeholder; the
  draft will pin the final shape and a future slice will replace
  the opaque cpma_operation_id stub with the full descriptor set.

---

# Architecture revision (2026-04-26)

This revision **supersedes** the slices above.  The conversation
that drove it is summarised here; the resulting design is normative.

## Recognitions

After the drain semantics rewrite in mirror-lifecycle.md (which
walked back its own earlier "exclude D from RW layouts" rule), the
PS protocol architecture got re-examined and several earlier
assumptions were found to be wrong:

1. **The PS does not need a CB-receive infrastructure.**  The
   "MDS sends CB_PROXY_MOVE to PS" pattern can equivalently be
   "PS polls via fore-channel `PROXY_PROGRESS`; MDS replies with
   work assignments inline".  Pure fore-channel.  This avoids
   building the entire CB program registration + CB_COMPOUND
   dispatcher + CB_SEQUENCE handler that slice 6c-ii was about
   to ship.
2. **`CB_PROXY_*` is the wrong direction.**  Op numbers 95-98
   landed in slice 6c-i as back-channel.  All four uses are
   actually fore-channel from PS to MDS, OR MDS-reply-to-poll
   payload.  The CB_PROXY_* shape needs to be retired.
3. **No new claim type is needed for migration.**  The PS uses
   normal `OPEN(CLAIM_NULL)` / `OPEN_RECLAIM(CLAIM_PREVIOUS)`.
   The MDS recognises the registered-PS session and serves a
   special composite layout (L3) that the PS uses for the move.
4. **No new stateid type is needed.**  The layout stateid issued
   by `LAYOUTGET` for L3 IS the per-move identifier.  The MDS
   keys its persisted migration record on this stateid.
5. **`nc_is_registered_ps` is NOT overloaded.**  Slice 6b-ii's
   namespace-discovery bypass stays narrow.  The migration-layout
   grant is gated by session identity (the MDS knows the session
   is from a registered PS), not by extending the bypass.
6. **The two-layout state in the inode is the correctness
   primitive.**  L1 = current layout; L2 = post-migration
   candidate; the PS sees L3 = composite of both for write
   targeting; commit on `PROXY_DONE(OK)` swaps L1 -> L2 atomically;
   `PROXY_DONE(FAIL)` rolls back to L1.  Concurrent client writes
   stay on L1 throughout, so D stays in sync via normal CSM
   without requiring a write barrier.
7. **Reboot recovery is standard NFSv4 layout reclaim, with one
   PS-side persistence requirement.**  PS does
   `RECLAIM_COMPLETE` + `OPEN_RECLAIM(CLAIM_PREVIOUS)` +
   `LAYOUTGET(reclaim=true)`.  For the LAYOUTGET reclaim to
   succeed, the PS must have **persisted the layout stateid
   locally** when the MDS originally granted L3 -- the reclaim
   request carries the stateid as its key, and the MDS matches
   against its own persisted in-flight migration record keyed on
   `(clientid, file_FH, layout_stateid)`.  No new claim type, no
   side-channel grant; this is the standard RFC 8881 S18.43.3
   reclaim path with a registered-PS session attribute on top.
8. **Nothing needs to be a proxy stateid.**  The earlier intuition
   that PROXY_REGISTRATION should mint a "proxy stateid" was
   resolved by recognising that the layout stateid for L3 already
   serves the role.  Proxy stateid as a *new type* is unnecessary;
   the existing layout stateid type does the work.

## Op walkback

| Slice 6c-i (current, on origin/main) | Revised |
|---------|---------|
| `OP_CB_PROXY_STATUS = 95` (back-channel) | **retire as `OP_PROXY_RESERVED_95`** -- reservation only, never reuse the number |
| `OP_CB_PROXY_MOVE = 96` (back-channel) | **retire as `OP_PROXY_RESERVED_96`** -- assignments delivered in `PROXY_PROGRESS` reply |
| `OP_CB_PROXY_REPAIR = 97` (back-channel) | **retire as `OP_PROXY_RESERVED_97`** -- assignments delivered in `PROXY_PROGRESS` reply |
| `OP_CB_PROXY_CANCEL = 98` (back-channel) | **retire as `OP_PROXY_RESERVED_98`** -- the back-channel CANCEL op number is NOT reused on the fore-channel even though the wire programs differ; reusing 98 in `nfs_argop4` while it is also reserved in `nfs_cb_argop4` would trip future audits and confuse implementors who expect op numbers to be globally meaningful. |
| (n/a) | new fore-channel `OP_PROXY_DONE = 99` (PS to MDS, "I am committing this move; commit layout L2") |
| (n/a) | new fore-channel `OP_PROXY_CANCEL = 100` (PS to MDS, "I am dropping this work item; rollback layout to L1") |

`OP_PROXY_PROGRESS = 94` (already wire-allocated in slice 6a) gains
a richer response carrying work assignments:

```xdr
struct proxy_assignment4 {
    enum proxy_op_kind4 pa_kind;     /* MOVE | REPAIR | CANCEL_PRIOR */
    nfs_fh4             pa_file_fh;
    /* dstore identifiers in MDS-internal form -- TBD shape */
    uint64_t            pa_source_dstore_id;
    uint64_t            pa_target_dstore_id;
    /* additional kind-specific descriptors as needed */
};

struct PROXY_PROGRESS4resok {
    /* heartbeat ack (MDS confirms the PS is still registered) */
    uint32_t            ppr_lease_remaining_sec;
    /* zero or more work assignments for this PS to pick up */
    proxy_assignment4   ppr_assignments<>;
};
```

The PS sends `PROXY_PROGRESS` periodically (lease/2 cadence by
default, configurable per PS via a config knob).  In each call it
can include progress reports on in-flight moves AND optionally
request new work.  The MDS replies with the lease ack and any
newly-allocated assignments.

### Polling cadence and assignment latency

`lease/2` is a **steady-state** cadence -- when work is sparse, the
PS poll wakes the MDS once per ~30s per PS to find out there is
nothing to do.  For N registered PSes this is O(N) wake-ups per
half-lease, which is acceptable for small N (1-8 PSes per MDS, the
expected deployment) but does not scale to hundreds.

Two opt-in cadence reductions for low-work environments:

1. **Adaptive backoff** -- after K consecutive empty replies,
   PS doubles its poll interval up to a cap (lease/1 or 2*lease).
   On any non-empty reply, PS resets to the lease/2 baseline.
2. **MDS-driven hint** -- the `PROXY_PROGRESS` reply includes
   `ppr_recommended_next_poll_sec` so the MDS can throttle a
   chatty PS.  Defaults to `lease/2`; can be set higher when the
   MDS has no work for any PS.

Conversely, **work-urgency latency** is bounded by the poll
cadence.  A REPAIR triggered by DSTORE_LOST waits up to lease/2
before any PS picks it up, which is fine for terabyte-scale
repair operations but slow for small files.  The MDS may
optionally use the existing CB infrastructure (CB_RECALL etc.) to
nudge a PS's session, prompting it to poll early; or accept the
latency as the cost of the polling model.  Defer the nudge
mechanism to a future slice; latency caps at ~30s by default,
which is operationally fine.

### Multi-PS assignment fan-out

When multiple PSes are registered against the same MDS, the MDS
must decide which PS gets each assignment.  Required policy:

1. **At most one in-flight migration per (file_FH, target_dstore)
   at any time.**  This is a HARD invariant -- two PSes
   simultaneously moving the same file produces a race the
   protocol cannot recover from cleanly.  The MDS keys its
   "in-flight migrations" registry on `(file_FH, target_dstore)`
   and rejects assignment of a migration when an entry already
   exists for that key.
2. **PS selection: first-fit registered PS, round-robin within
   the registered set.**  The MDS maintains an ordered list of
   currently-registered PSes (insertion order).  For each
   assignment, walk the list starting after the last PS assigned;
   pick the first PS whose `ppr_inflight` count is below a
   per-PS cap (default: 8 in-flight migrations per PS,
   configurable).  If no PS qualifies, the assignment stays in
   the queue; next PS poll re-tries the walk.
3. **No PS affinity.**  The MDS does not try to send "the same
   file always to the same PS".  Affinity could be added later
   for cache-locality reasons but is unneeded for first impl.
4. **Cross-PS reassignment on PS lease expiry.**  If PS-A's
   session expires while it owns an in-flight migration, the MDS:
   - Issues internal `PROXY_DONE(FAIL)` semantics on PS-A's
     migration (commits L1, drops L2/G half-fill)
   - Re-queues the migration as a fresh assignment for any other
     registered PS to pick up
   - The new PS gets a fresh `(file_FH, target_dstore)` invariant
     check -- if PS-B already had an assignment for this file, the
     re-queue is dropped (idempotent)

The per-PS in-flight cap exists to prevent one PS from monopolising
the assignment queue while others sit idle.  It also bounds the
fanout amplification when an MDS has thousands of files to
migrate.

The `(file_FH, target_dstore)` invariant deserves a TSan-style
test: under concurrent PROXY_PROGRESS polls from N PSes the MDS
must never assign two records with the same key.

`PROXY_DONE` and `PROXY_CANCEL` carry the layout stateid as the
per-move identifier:

```xdr
struct PROXY_DONE4args {
    stateid4    pd_layout_stid;  /* identifies the move */
    nfsstat4    pd_status;       /* OK = commit L2; else = commit L1 */
};
struct PROXY_DONE4res {
    nfsstat4    pdr_status;
};

struct PROXY_CANCEL4args {
    stateid4    pc_layout_stid;
};
struct PROXY_CANCEL4res {
    nfsstat4    pcr_status;
};
```

Typical compound shapes:

```
poll for work + report progress on in-flight moves:
  SEQUENCE PROXY_PROGRESS(want_work=true, reports=[...])

start work on assignment A (MDS already issued the in-flight
record when it included A in the PROXY_PROGRESS reply):
  SEQUENCE PUTFH(A.file_fh) OPEN(CLAIM_NULL, ...) GETFH LAYOUTGET
   -> returns layout_stid for L3

shovel bytes (direct to DS, not shown):
  CHUNK_READ from any source mirror in M1
  CHUNK_WRITE to G in M2

commit (LAYOUTRETURN runs first per RFC 8881 S18.51,
releasing L3 cleanly via the standard mechanism; PROXY_DONE
then operates on the persisted in-flight migration record
only and does NOT touch layout state):
  SEQUENCE PUTFH(A.file_fh) LAYOUTRETURN(layout_stid) PROXY_DONE(layout_stid, OK)
   -> MDS atomically swaps inode active layout L1 -> L2

abort (PS bails before completion):
  SEQUENCE PUTFH(A.file_fh) LAYOUTRETURN(layout_stid) PROXY_CANCEL(layout_stid)
   -> MDS keeps L1, drops L2/G

PROXY_DONE / PROXY_CANCEL stick to one role: terminal-status
update on the persisted migration record.  They do not have
side effects on layout stateids; LAYOUTRETURN (existing op)
handles that.  Sticking with established ops keeps the wire
surface predictable and avoids overloading PROXY_* with
state-management semantics that already have a well-defined
op.
```

## Why this is right

- **Symmetry with normal NFSv4 patterns.**  The PS looks like a
  client to the MDS for everything except the small set of fore-
  channel ops gated on `nc_is_registered_ps`.  Reclaim, lease
  renewal, layout-state -- all standard.
- **Minimal new wire format.**  Two new fore-channel ops
  (`PROXY_DONE`, `PROXY_CANCEL`).  One existing op
  (`PROXY_PROGRESS`) gains a richer response struct.  Four
  back-channel ops walked back / retired.
- **No CB-receive infrastructure to build.**  Saves slice 6c-ii
  entirely.  The PS just acts as an NFSv4 client of its upstream
  MDS, polling for work.
- **Atomic commit with no client-visible barrier.**  L1 stays
  active throughout the migration; clients see no disruption; the
  L1 -> L2 swap is instantaneous from the MDS's perspective.
- **Reboot-recoverable via standard NFSv4 mechanisms.**  No
  bespoke restart-from-scratch protocol; PS reclaims its layout
  and continues the move from where it was.

## Revised slice ladder (replaces 6c-ii / 6c-iii / phase 8 above)

The slice numbering restarts to make it clear this is a different
ladder from the (superseded) 6c-ii / 6c-iii / phase 8 sketches:

- **Slice 6c-w**: XDR walkback.  Modify `nfsv42_xdr.x`:
  - Retire `OP_CB_PROXY_STATUS` (95), `OP_CB_PROXY_MOVE` (96),
    `OP_CB_PROXY_REPAIR` (97).  Remove the case arms from
    `nfs_cb_argop4` / `nfs_cb_resop4`.  Keep the constants
    reserved as `OP_PROXY_RESERVED_95` / `_96` / `_97` so the
    op-number space stays stable on the wire.
  - Repurpose `OP_CB_PROXY_CANCEL` (98) as fore-channel
    `OP_PROXY_CANCEL`.  Move from `nfs_cb_argop4` to
    `nfs_argop4`; new arg/res structs as above.
  - Add `OP_PROXY_DONE = 99` (fore-channel) with arg/res.
  - Extend `PROXY_PROGRESS4resok` with `ppr_lease_remaining_sec`
    + `ppr_assignments<>`.  Define `proxy_assignment4` and
    `proxy_op_kind4`.
- **Slice 6c-x**: MDS-side `PROXY_DONE` / `PROXY_CANCEL` handlers
  + two-layout state machinery on the inode.  The bulk of the
  protocol-side work.  Tests cover the L1/L2 swap, rollback on
  FAIL, and the migration record persistence.
- **Slice 6c-y**: MDS-side `PROXY_PROGRESS` reply with
  assignments.  Picks work from the autopilot's queue and pushes
  one or more assignments per poll.  Tests cover assignment
  delivery, multi-assignment-per-poll, and lease renewal.
- **Slice 6c-z**: PS-side migration loop.  Polls
  `PROXY_PROGRESS`; for each assignment, OPENs the file, gets the
  L3 layout, shovels bytes to G, sends PROXY_DONE.  Tests cover
  the full move loop end-to-end against a real MDS.
- **Slice 6c-zz**: Reclaim recognition (MDS-side).  On
  `OPEN_RECLAIM(CLAIM_PREVIOUS)` + `LAYOUTGET(reclaim=true)` from
  a registered-PS session, look up persisted migration records and
  re-grant L3.  Tests cover MDS restart mid-move + PS resumes.

Each slice is sized to land independently.  The autopilot itself
(slice E in mirror-lifecycle.md) is what produces the work that
6c-y assigns -- 6c-y consumes the autopilot's queue, so 6c-y is
gated on slice E's queue infra.

## Open questions

- **Assignment payload completeness.**  Does
  `proxy_assignment4` need to convey the full source-layout
  descriptor (DS device IDs, FHs) so the PS can dial the source
  DSes immediately, or does the PS LAYOUTGET source-layout via a
  separate call once it has the file FH?  Cleaner if the
  assignment is self-contained (no second round-trip) but bloats
  the assignment struct.  Probably worth pinning before slice
  6c-x lands.
- **PROXY_CANCEL preconditions.**  Can the PS cancel a move it
  has not yet OPEN'd (assignment received but no work started)?
  Probably yes, with the layout stateid omitted / set to a
  sentinel; MDS resolves by file_fh.  Or we require PS to do a
  no-op LAYOUTGET to acquire the stateid first.
- **Cross-PS assignment migration.**  If PS-A goes away mid-move
  (lease expiry / squat from PS-B), can the migration be re-
  assigned to PS-B?  The persisted migration record's
  `owning_PS_session` field would need to be reassignable.  Or
  the MDS abandons the move on PS-A's lease expiry and starts a
  fresh one on PS-B with new L1/L2.  Probably the latter --
  simpler.
- **PROXY_REGISTRATION's role unchanged.**  Slice 6a-c-i still
  does the right thing; nothing in this revision affects the
  registration / allowlist / squat-guard / mTLS work.
- **Lease semantics for the migration layout L3.**  Standard
  NFSv4 lease (renewed by SEQUENCE).  No special handling; if
  the PS's session expires mid-move, the L3 layout expires too,
  the autopilot abandons the move (commits L1), and a future
  PROXY_PROGRESS poll from the re-registered PS picks up a fresh
  assignment.

## Other implementation feedback to surface to the draft

These are protocol-shape concerns that surfaced in the slice 6a /
6b / 6c implementation work, independent of the
back-channel-vs-fore-channel revision above.  All worth landing in
the same draft revision.

### From slice 6a (PROXY_REGISTRATION)

- **`prr_flags` reserved-bit handling**: implementation rejects
  any non-zero bit per RFC 8178 S4.4.3.  The draft should
  explicitly cite RFC 8178's reserved-flag-rejection rule rather
  than just calling the field "reserved" -- otherwise an
  implementation might silently ignore unknown bits.
- **`EXCHGID4_FLAG_USE_NON_PNFS` requirement**: implementation
  rejects PROXY_REGISTRATION on a session whose EXCHANGE_ID set
  USE_PNFS_MDS or USE_PNFS_DS.  Draft should explicitly require
  USE_NON_PNFS, not just say the PS is "a regular NFSv4 client".
- **AUTH_SYS rejection**: implementation rejects PROXY_REGISTRATION
  with no GSS principal AND no TLS fingerprint context.  Draft
  should explicitly forbid AUTH_SYS-over-plain-TCP for
  PROXY_REGISTRATION sessions, not just say "secure transport
  recommended".

### From slice 6b-i (allowlist)

- **Default-deny semantics**: an empty `[[allowed_ps]]` allowlist
  on the MDS rejects every PROXY_REGISTRATION.  Draft should call
  out the secure default rather than leaving allowlist policy
  implementation-defined.
- **Exact-string identity match**: no realm fuzz, no DNS
  canonicalization.  Draft should make this normative ("the MDS
  MUST use exact-string comparison for principal matching") to
  prevent an implementation from quietly broadening the trust
  boundary.

### From slice 6b-iii (squat-guard)

- **`prr_registration_id` as renewal key**: the implementation
  uses identity (principal or fingerprint) plus
  `prr_registration_id` to distinguish renewal (same id) from
  squat (different id).  Draft should describe this matching
  semantics explicitly, including:
  - PS MUST reuse the same `prr_registration_id` across reconnects
    within a single PS process lifetime.
  - On PS restart, the `prr_registration_id` MAY change; the MDS
    treats this as a squat and returns `NFS4ERR_DELAY` until the
    prior lease expires (typically one NFSv4 lease period).
  - An empty `prr_registration_id` is permitted but treated as
    "matches any other empty id" -- draft should clarify whether
    empty-id renewal is intended or whether MDS should reject
    empty IDs.
- **Lease semantics for the registration**: implementation
  refreshes the registration lease on each successful matching
  PROXY_REGISTRATION (renewal) or via standard NFSv4 lease
  renewal (SEQUENCE).  Draft should document both renewal paths.

### From slice 6b-iv (mTLS auth context)

- **Identity context plurality**: a registration is permitted if
  EITHER the GSS principal OR the TLS client-cert fingerprint
  matches an allowlist entry.  The draft today (per slice 6a's
  understanding) treats RPCSEC_GSS as the only auth path; it
  should be amended to explicitly permit either context.
- **Fingerprint format**: implementation uses SHA-256 of the
  peer cert's DER encoding, formatted as colon-separated hex.
  Draft should pin this format normatively (or define an OID-
  based hash agility scheme so SHA-384/512 can be added later
  without a wire format break).
- **Per-entry XOR**: an `[[allowed_ps]]` entry sets EXACTLY one
  of `principal` / `tls_cert_fingerprint`, never both.  Draft
  should normatively require this XOR (even though the matcher
  doesn't enforce it today; the implementation tolerates "both
  set" and matches whichever the wire context supplies).

### From slice trust-stateid (cross-cutting)

- **Multi-machine renewal**: trust_stateid.c flags that DS-side
  lease extension on TRUST_STATEID is correct only in combined
  mode (MDS+DS in one process).  In the multi-machine case,
  renewal should be driven by the MDS re-issuing TRUST_STATEID
  before expiry.  Draft should document this MDS-driven renewal
  model explicitly.
- **`te_iomode` as semantic vs informative**: today the field is
  carried but not enforced; CHUNK_WRITE accepts any iomode
  attached to a trusted stateid.  If the draft intends iomode to
  be load-bearing (e.g., RW-trusted stateid SHOULD reject
  CHUNK_READ that targets the read-only path), it needs to say
  so; otherwise drop iomode from the wire to avoid implying
  enforcement.

### From slice 6c-i (op number assignments)

- **Op numbers 95-98 not yet IANA-assigned**: same TBD caveat as
  CHUNK ops 80-89 and trust-stateid 90-92.  Draft should keep a
  consolidated "TBD pending IANA" table at the front so
  implementors don't have to derive the assignments from
  scattered call-outs.

### From clientid4 / state recovery interaction

- **clientid4 partitioning interaction with `prr_registration_id`**:
  reffs's clientid4 is `boot_seq | incarnation | slot` per
  `.claude/patterns/nfs4-protocol.md`.  After MDS reboot the PS
  reconnects via EXCHANGE_ID using its prior `client_owner4` and
  recovers the same `clientid4` via the standard
  `nfs4_client_alloc_or_find()` decision tree (slice 4b client
  recovery).  After a PS-process restart, the PS's
  `client_owner4` may or may not be stable (depends on PS impl);
  if not stable, the PS gets a fresh `clientid4` and the persisted
  in-flight migration records (keyed on the old clientid) are
  orphaned.  The squat-guard's `prr_registration_id` is independent
  of clientid -- it's the operator-meaningful "which PS instance
  is this" identifier.  Draft should clarify:
  - PS implementations SHOULD persist their `client_owner4` to
    survive restart, so post-restart EXCHANGE_ID recovers the
    same clientid and the in-flight migration records remain
    valid.
  - If the PS's `client_owner4` rotates (state loss), the
    `prr_registration_id` is the only identity continuity --
    matching id ON A NEW CLIENTID is renewal of registration, but
    the in-flight migration records on the old clientid cannot
    be claimed.  Those moves get re-assigned fresh by the
    autopilot.
- **PROXY_REGISTRATION + RECLAIM_COMPLETE ordering**: a PS doing
  standard NFSv4 reclaim after MDS reboot MUST issue
  `RECLAIM_COMPLETE` before attempting any per-file migration
  reclaim (so the registered-PS attribute is loaded onto the new
  session before the migration-reclaim path consults it).  PS
  does NOT need to re-issue PROXY_REGISTRATION -- the persisted
  `nc_is_registered_ps` flag is loaded by the MDS at boot.  Draft
  should document this ordering explicitly to avoid implementors
  re-registering needlessly (which would also race with the
  squat-guard).

## What this means for the IETF draft

The current `draft-haynes-nfsv4-flexfiles-v2-data-mover` (per the
slice 6a / 6b assumptions) describes:

- PROXY_REGISTRATION as a fore-channel op (correct, no change)
- CB_PROXY_STATUS / MOVE / REPAIR / CANCEL as back-channel CB ops
  (**WRONG** -- needs walkback)
- PROXY_PROGRESS as the PS reporting interim status
  (correct shape, response needs to grow)

Changes needed in the draft:

1. **Remove** the four `CB_PROXY_*` ops from the CB-op section.
2. **Add** `PROXY_DONE` + `PROXY_CANCEL` as fore-channel ops.
3. **Extend** `PROXY_PROGRESS` to convey work assignments in its
   response.
4. **Add** a "Two-layout migration state" section describing
   L1/L2 + the L3 composite the PS sees.  Include the atomic-
   commit semantics on PROXY_DONE.
5. **Add** a "Reboot recovery" section that documents the standard
   NFSv4 reclaim path being sufficient (PS uses
   OPEN_RECLAIM(CLAIM_PREVIOUS) + LAYOUTGET(reclaim=true); MDS
   matches against persisted migration records).
6. **Add** a "Drain semantics" section pointing back to
   mirror-lifecycle.md's simplified model: drain affects only the
   runway; live I/O continues including to the draining dstore;
   migrations use the two-layout commit.
7. **Clarify** that no new claim type is needed; the PS uses
   normal `OPEN(CLAIM_NULL)`.
8. **Clarify** that no new stateid type is needed; the layout
   stateid for L3 serves as the per-move identifier.
9. **Add** a "Privilege model" section that documents
   `nc_is_registered_ps` as a session attribute set by
   PROXY_REGISTRATION; only this session attribute (not stateid
   type, not principal in the wire) gates access to PROXY_DONE /
   PROXY_CANCEL / PROXY_PROGRESS.  Reuse the existing slice 6b-i
   / 6b-iii / 6b-iv text on allowlist + squat + mTLS for the
   identity model section.

These changes are substantial but uniformly *simplify* the
protocol surface (fewer ops, no CB infrastructure, standard
recovery).  The draft will get noticeably shorter.

