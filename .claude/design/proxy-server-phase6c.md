<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Proxy Server Phase 6c: PS-side CB receive path (XDR + dispatch + handler)

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
