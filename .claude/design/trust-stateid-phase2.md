<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Phase 2: NFSv4 dstore vtable bench bring-up

## Context

Slice 1.5 shipped a `tight_coupling = true` config knob on
NFSv3 dstores.  Slice 1.6 fixed the inner+outer retry path
(BAD_STATEID -> -ESTALE remap, SEQ_MISORDERED-on-refresh fix).
But the underlying mechanism cannot actually engage on NFSv3
dstores because TRUST_STATEID is an NFSv4 op and
`dstore_ops_nfsv3` has no `trust_stateid` slot.  The MDS has
no protocol-level mechanism to populate the DS trust table over
an NFSv3 connection.

The trust mechanism only physically works when dstores speak
NFSv4.  `dstore_ops_nfsv4` is fully implemented (13 ops including
the 4 trust ops, `nfsv4_create / _remove / _chmod / _truncate /
_fence / _getattr / _read / _write / _commit /
_probe_tight_coupling / _trust_stateid / _revoke_stateid /
_bulk_revoke_stateid`), and `dstore_alloc` already routes
`protocol = "nfsv4"` to that vtable plus calls
`ds_session_create()`.

This slice closes the gap so a bench `mds.toml` that sets
`protocol = "nfsv4"` actually serves layouts and the trust
mechanism can be exercised end-to-end.

## Diagnosed failure surface (2026-05-05 PM)

With `protocol = "nfsv4"` + `tight_coupling = true` in the bench
mds.toml, the MDS reaches the NFSv4 branch in `dstore_alloc`:

```
[FS] dstore[1]: do_mount=1 protocol=1 (NFSV3=0 NFSV4=1) addr=reffs-ds0
[NFS] dstore_alloc:543: dstore=... id=1 ref=2 state=0x1 addr=reffs-ds0 path=/ fh_len=0
```

`do_mount=1, protocol=1` means the `if (do_mount && protocol ==
REFFS_DS_PROTO_NFSV4)` branch IS entered.  But the post-alloc
state is `0x1` -- DSTORE_IS_HASHED only, **not** DSTORE_IS_MOUNTED
-- and `fh_len=0`.  Per `ds_session.c:131`, a successful
`ds_session_create` would have set DSTORE_IS_MOUNTED and copied
the GETFH-returned root FH into `ds_root_fh`.  Neither happened.

Two interesting absences in the trace:
1. **No** `LOG("dstore[%u]: NFSv4 session to %s failed
   (continuing)")` from `dstore.c:501`.  So `ds_session_create`
   did not return a negative status from the caller's
   perspective.
2. **No** EXCHANGE_ID / CREATE_SESSION traces in the MDS log
   from the MDS->DS direction at startup.  The
   `nfs4_compound:93 OP_EXCHANGE_ID` traces visible later are
   from ec_demo's MDS-facing session, not the MDS->DS path.

Two hypotheses explain both observations:

- (a) `ds_session_create` short-circuits early for some
  reason (config check, missing field, etc.) and returns 0
  without actually opening the session.  The function exists
  per `lib/nfs4/dstore/ds_session.c:38` but the body might
  guard on something the bench config doesn't satisfy.
- (b) The MDS is a server (it has `nfs4_protocol_register`'d
  the server side) but its NFSv4 *client* code path
  (`mds_session_create` in `lib/nfs4/client/mds_session.c`,
  reused by `ds_session_create`) does not log via the same
  trace facility, and the EXCHANGE_ID does happen but is not
  visible in the MDS process's reffsd.log.  Counter-evidence:
  state=0x1 and fh_len=0 still indicate failure.

The third dstore-alloc-time gap (3.5 ms per dstore between the
"do_mount" trace and the trace_dstore line) is consistent with
either: a single failed RPC round-trip + cleanup, or an early
short-circuit + a small constant overhead.  Without further
diagnosis, can't distinguish.

## What this slice ships

A working `protocol = "nfsv4"` bench, end-to-end:

- `ds_session_create` returns success and sets
  `DSTORE_IS_MOUNTED + ds_root_fh + ds_root_fh_len`.
- `runway_create` succeeds at startup (the runway pool
  pre-creates files via the NFSv4 vtable's `.create =
  nfsv4_create`).  Today this fails because
  `dstore_is_available` is false, so `runway_create` is never
  reached.
- LAYOUTGET succeeds and grants a layout backed by NFSv4
  dstores.
- TRUST_STATEID fan-out reaches the DSes, populates their
  trust tables.
- ec_demo CHUNK_WRITE with the real layout stateid is
  ACCEPTED.

## Tests / acceptance

### Step-by-step acceptance

1. **`ds_session_create` works**.  Add a mds_session_init test
   that connects to a running reffs-as-DS and verifies
   EXCHANGE_ID + CREATE_SESSION + PUTROOTFH + GETFH succeed +
   the dstore ends up `state & DSTORE_IS_MOUNTED` with non-zero
   `ds_root_fh_len`.  Lives at
   `lib/nfs4/dstore/tests/ds_session_test.c`; mocks the NFSv4
   server side using the existing test harness.
2. **Runway populates over NFSv4**.  Add a runway test that
   confirms `runway_create` succeeds against an NFSv4 dstore
   and the runway entries are reachable via
   `runway_pop`.
3. **LAYOUTGET grants over NFSv4**.  Functional test: bring up
   the bench docker stack with `protocol = "nfsv4"`, verify a
   single `ec_demo write --layout v2` succeeds with no
   contention.
4. **Trust fan-out lands**.  Diagnostic check that each DS's
   trust table has an entry for the stateid the MDS issued.
   Either via probe protocol or via `docker logs` on a DS with
   trace logging.
5. **N=20 v2 race sweep with mechanism engaged**.  The slice
   1.5 measurement, finally able to run.  Decision matrix:
   - `mixed = 0, b_won >= 18, read_failed = 0` -- slice 1.5
     closeout closes for real (with slice 1.6 retry handling
     the BAD_STATEID->refresh dance).
   - Other distributions -- iterate.

### Existing tests affected

| File | Impact |
|------|--------|
| `lib/nfs4/dstore/tests/dstore_test.c` | Likely needs an NFSv4 dstore variant if not already present. |
| `lib/nfs4/tests/layout_conflict_scan_test.c` | Independent (mocked dstores).  PASS unchanged. |
| All `make check` tests | PASS unchanged unless ds_session_create's signature changes. |

## Implementation steps

### Step 1: Diagnose ds_session_create silent failure

Add a TRACE at the top of `ds_session_create` (lib/nfs4/dstore/ds_session.c:38)
and at every early-return path.  Run the bench with `protocol =
"nfsv4"` and capture the trace to determine *which* path
ds_session_create takes -- short-circuit, or attempts the RPC and
fails, or claims success but doesn't set state.

Likely candidates to check:
- `mds_session_create()` returns success but doesn't actually
  perform EXCHANGE_ID (e.g., guarded on some flag)
- `mds_compound_send()` for EXCHANGE_ID returns -EREMOTEIO due
  to some setup issue (auth flavor mismatch?  the MDS<->DS
  session doesn't use AUTH_SYS by the same path as
  ec_demo<->MDS)
- The PUTROOTFH+GETFH compound returns a 0-length filehandle

The fix from this step likely looks like one of:
- A missed pmap registration on the DS side (similar to the
  earlier register_with_rpcbind investigation).
- A missing EXCHGID4 flag on the MDS->DS session
  (`USE_NON_PNFS` per the design doc, vs `USE_PNFS_MDS` which
  the existing trust-stateid op handlers check on the
  *caller's* session).
- An auth flavor mismatch on the MDS->DS session.

### Step 2: Wire trust-stateid registration on session-up

Once ds_session_create returns true success, the next
expectation is that `probe_tight_coupling` runs (already wired
at `ds_session.c:142-144`) and `ds_tight_coupled` is set on
the dstore.  That gates the trust fan-out at LAYOUTGET.
Verify probe_tight_coupling actually fires and the dstore
records the result correctly.

### Step 3: Verify TRUST_STATEID fan-out at LAYOUTGET

When the MDS issues a layout to a tight-coupled NFSv4 dstore,
the LAYOUTGET path fans out TRUST_STATEID to each mirror DS
via `dstore_fanout` (FANOUT_TRUST_STATEID, slice 1).  This
plumbing already exists per slice 1.  Validate it actually
fires by capturing TRUST_STATEID traces at the DS.

### Step 4: Bench acceptance

Re-enable `protocol = "nfsv4"` in `deploy/benchmark/mds.toml`
on main.  Run the slice 1.5 N=20 v2 race sweep.  Document the
result.

### Step 5: Update progress_report + experiment report

Replace the "open pending Phase 2" notes with the actual
measurement and the slice 1.5 closeout interpretation that
the morning of 2026-05-05 first attempted to write.

## Implementation notes / hazards

- **MDS<->DS session uses single slot** per the design doc
  (`trust-stateid.md` Step 2.4 + `dstore-vtable-v2.md` Step
  3): "Single slot -- serializes ops per DS".  Slice 1.6
  exposed an interaction with slot bookkeeping: the slot's
  seqid must be preserved across layout-refresh
  (`ec_layout_refresh` already preserves DS sessions for the
  client; the MDS's MDS-to-DS sessions need the same care if
  they get re-established for any reason).
- **EXCHGID4 flags**.  Design says MDS-to-DS session sets
  `USE_NON_PNFS` per RFC 8881 S18.35.  The trust-stateid op
  handlers, however, currently guard on `USE_PNFS_MDS`
  (per `trust-stateid.md` Step 1.3).  If the MDS sends
  USE_NON_PNFS, its TRUST_STATEID/REVOKE_STATEID ops will be
  rejected by the DS with NFS4ERR_PERM.  Either the guard
  needs broadening or the MDS-to-DS session must
  set USE_PNFS_MDS (the existing design's
  NOT_NOW_BROWN_COW).  Slice closure must pick one; my
  preference is broaden the guard to "session was
  established by an entity allowed to fan out trust state",
  which is what `[[allowed_ps]]` style allowlist would
  formalise.

## Followups (out of slice 2 scope)

- Multi-slot MDS<->DS sessions (concurrent CHUNK fan-outs).
- DS health monitoring + reconnect on session loss.
- Probe-protocol toggle for tight-coupling (today it's static
  in TOML).

## Cross-references

- `trust-stateid.md` -- parent design.
- `trust-stateid-slice-1-5.md` -- slice that scoped (and
  deferred Phase 2).
- `trust-stateid-slice-1-6.md` -- the retry slice that exposed
  the gap.
- `dstore-vtable-v2.md` -- the broader NFSv4-dstore design.
- `lib/nfs4/dstore/ds_session.c` -- the file under
  investigation in step 1.
- `lib/nfs4/dstore/dstore_ops_nfsv4.c` -- the vtable already
  in tree; not the surface that needs new code, just
  validation.
