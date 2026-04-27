<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Proxy Server Phase 6c-x Revision: per-instance migration + proxy_stateid

> **Status**: revision proposal (2026-04-27).  Supersedes the
> "two-layout L1/L2 swap" framing in
> `.claude/design/proxy-server-phase6c.md` (lines 287-298, 425-470,
> 518-521).  Settles three issues raised against the original
> slice 6c-x scope; touches the IETF draft as well as the reffs
> implementation.

## What's being revised

Three problems with the original 6c-x design surfaced while
planning the implementation:

1. **Layout stateid as the migration handle is wrong.**
   `stateid4.other` is opaque per RFC 8881 S8.2.2.  Uniqueness
   across PSes / clients is implementation-defined; nothing in
   the protocol prevents collision.  Even if reffs happens to
   guarantee uniqueness today, two consequences follow:
   - Any PS that learns the layout stateid (e.g., from a leaked
     log, a packet capture, or simply by also holding a layout
     on the same file) could send `PROXY_DONE(FAIL)` and roll
     back the migration -- **no authorization gate**.
   - Reusing layout stateid for a control-plane handle conflates
     "client is permitted to do I/O" with "PS is permitted to
     commit a migration record".  Two different lifetimes, two
     different owners.
2. **"Two layouts L1 -> L2 swap" is too coarse.**  Real
   migrations are per-instance, not whole-layout.  Example: an
   FFv1 mirror=3 layout `(A, B, C)` where the admin is
   decommissioning DS A.  The PS has at least two ways to do
   the move:
   - **Whole-layout swap**: hand the client `L2 = (D)` and let D
     fan out writes to `(A, B, C, E)`.  D is the funnel; A and
     E both receive every write; once E is in sync, D's fanout
     becomes `(B, C, E)` and A is removed.
   - **Per-instance swap**: hand the client `L2 = (D, B, C)`.
     The client writes to all three.  D internally fans out to
     `(A, E)` only -- B and C continue to receive their writes
     directly from the client.  When E is in sync, D's fanout
     becomes `(E)` and the layout becomes `(E, B, C)`.
   The L1/L2 swap framing only describes the first variant.  The
   second is what the deployed mirror-lifecycle slices (Slice E
   "drain autopilot") actually need; the PS replaces *which DS
   the client targets for one instance position* while the other
   instance positions stay put.  And FFv2 extrapolates further --
   a stripe-position swap, not a mirror-position swap.
3. **PROXY_CANCEL takes a layout stateid.**  The proxy ops are
   *file-scoped operations*, not *layout-scoped operations*.  A
   PROXY_CANCEL on a file_fh expresses the intent ("drop the
   move on this file") without coupling to any one layout the
   PS may or may not hold at the moment of cancel.  If the
   protocol needs a server-issued handle for the migration job
   (so the PS can refer back to it across reconnect, and the
   MDS can distinguish "the move I started" from "another move
   on the same file"), that handle is a **proxy_stateid** -- a
   new stateid type minted by the MDS, distinct from the layout
   stateid.

## Revised model: per-instance migration deltas + proxy_stateid

### proxy_stateid -- new MDS-issued handle

The MDS mints a proxy_stateid when it accepts a migration
assignment from the PS via `PROXY_PROGRESS`.  The proxy_stateid
shape reuses the standard NFSv4 `stateid4`:

```
struct stateid4 {
    uint32_t seqid;
    opaque   other[12];
};
```

`other[12]` is allocated by the MDS from a fresh namespace
distinct from open / lock / layout / delegation stateids.  The
MDS guarantees uniqueness within a (server_state, boot_seq) tuple
and persists `other` with the migration record so the same value
survives MDS restart (slice 6c-zz dependency).

The proxy_stateid is the canonical handle for the migration job:
- `PROXY_PROGRESS` reply assigns it (slice 6c-y mints, this
  slice 6c-x defines the type).
- `PROXY_DONE(proxy_stateid, ok|fail)` references it.
- `PROXY_CANCEL(proxy_stateid)` references it.
- A subsequent `PROXY_PROGRESS` poll from the same PS may carry
  the proxy_stateid as "I am still working on this assignment;
  give me lease renewal and any newer progress info" (slice 6c-y).

The proxy_stateid is **not** an I/O stateid.  The PS still uses
the file's layout stateid (LAYOUTGET'd separately) for
CHUNK_WRITE / CHUNK_READ / READ / WRITE on the source / target
DSes.  proxy_stateid is purely the MDS<->PS migration-control
handle.

### Authorization: registered-PS identity on the migration record

The migration record carries the **registered-PS identity** that
owns it -- specifically, the `prr_registration_id` (or, when that
is empty, the GSS principal / mTLS fingerprint that
`PROXY_REGISTRATION` matched against `[[allowed_ps]]`).  Recording
the per-EXCHANGE_ID `clientid4` instead would be **wrong**: a PS
that reconnects (new EXCHANGE_ID -> new clientid4) but presents
the same `prr_registration_id` is the same logical PS per slice
6b-iii (squat-guard) and must be permitted to commit / cancel its
own in-flight migrations.

At assignment time (`PROXY_PROGRESS` mints), the MDS records:

```
migration_record {
    proxy_stateid   stid;          // canonical handle
    nfs_fh4         file_fh;       // file scope
    char            owner_reg[REGID_MAX]; // registered-PS identity
                                          //   (prr_registration_id or
                                          //   matched principal/fingerprint)
    instance_delta  deltas[];      // per-instance state changes
    _Atomic enum    phase;         // PENDING, IN_PROGRESS, COMMITTING, ABANDONED
    uint64_t        last_progress_mono_ns; // CLOCK_MONOTONIC (two-clock pattern)
};
```

`phase` is `_Atomic` because the layout-build path (LAYOUTGET)
reads it concurrently with the DONE / CANCEL handlers (per
`.claude/standards.md` C11 atomics rule).  `last_progress`
follows the two-clock pattern from `.claude/design/trust-stateid.md`:
`CLOCK_MONOTONIC` ns for the lease accounting itself; only the
wire-encoded `nfstime4` values use `CLOCK_REALTIME`.

Every `PROXY_DONE` and `PROXY_CANCEL` request:
1. Validates that the caller's session is registered-PS.
2. Looks up the record by `proxy_stateid.other`.
3. Validates `record.owner_reg == session->registered_ps_identity`
   (the identity captured at PROXY_REGISTRATION time, which
   survives reconnect).
4. Validates `record.file_fh == compound->c_curr_nfh.nfh_*`
   (PUTFH precondition).

Mismatch on (2) -- record not found -- returns
`NFS4ERR_BAD_STATEID` for the in-boot case OR
`NFS4ERR_STALE_STATEID` for the post-reboot case (see
"proxy_stateid lifetime and post-reboot semantics" below).
Mismatch on (4) returns `NFS4ERR_BAD_STATEID` (file_fh does not
match the migration record).
Mismatch on (3) returns `NFS4ERR_PERM` (record exists but is
owned by a different registered PS).

A PS attempting to DONE / CANCEL another PS's migration always
fails -- no PS can impact another's job by guessing or
intercepting a stateid.

### Per-instance migration deltas

The migration record carries an array of per-instance deltas,
not a whole second layout.  Each delta describes a transformation
on one position in the file's current layout:

```
enum instance_state {
    STABLE      = 0,  // unchanged; client and PS write here directly
    DRAINING    = 1,  // being decommissioned; PS shadows writes elsewhere
    INCOMING    = 2,  // new instance; PS funnels writes here
    INTERPOSED  = 3,  // a PS-owned funnel; client writes go through it,
                      //   PS fans out per fanout_targets[]
};

struct instance_delta_fanout_entry {
    uint32_t fld_dstore_id;
    uint8_t  fld_fh[NFS4_FHSIZE];
    uint32_t fld_fh_len;
};

struct instance_delta {
    uint32_t            ld_seg_index;      // which segment in i_layout_segments
    uint32_t            ld_instance_index; // which instance within the segment
    enum instance_state ld_state;
    /* INCOMING / INTERPOSED only: targets the PS fans new writes to.
     * Out-of-line so instance_delta has fixed size and can live in a
     * flat array on the migration record. */
    uint32_t            ld_fanout_n;
    struct instance_delta_fanout_entry *ld_fanout; // array of ld_fanout_n
    /* DRAINING only: intra-record cross-reference into the parent
     * record's deltas[] array, naming the matching INCOMING delta
     * that shadows this slot.  Sibling entries in the same record;
     * NOT an index into this delta's ld_fanout[]. */
    uint32_t            ld_replacement_delta_idx;
};
```

The currently-published layout (`i_layout_segments`) is built
**through** the deltas: when LAYOUTGET runs while a migration is
active, the layout-build code consults the migration record and
emits the "during-migration view":

- For `STABLE` instances, the original entry is emitted unchanged.
- For `DRAINING` instances, the slice 6c-x policy is
  **omit-and-replace** (the client never sees the draining DS):
  the entry is omitted and replaced by an `INCOMING` slot.  See
  the "In-flight write coherence at migration start" section
  below for the recall ordering that makes this safe.
  Keep-and-shadow (where the client keeps writing to the DRAINING
  DS while the PS shadows the writes elsewhere) is a deferred
  alternative -- it requires PS-as-DS plumbing that does not
  exist in this slice; see the deferral list.
- For `INCOMING` instances added on top of STABLE positions, the
  new entry is emitted in place of the slot the admin chose to
  vacate.
- For `INTERPOSED` instances, the entry points at the PS's own
  proxy I/O endpoint; the client writes to the PS, which fans out
  per `fld_fanout`.  This is the "(D, B, C) where D fans to (A, E)"
  variant.

`PROXY_DONE(OK)` collapses the deltas into `i_layout_segments`
permanently:
- `DRAINING` slots are removed from the segment.
- `INCOMING` and `INTERPOSED` slots become STABLE.
- The migration record is unhashed and freed.

`PROXY_DONE(FAIL)` discards all deltas without touching
`i_layout_segments`; the layout reverts to the pre-migration
shape.  Any in-flight INCOMING DSes the PS allocated need cleanup
(the PS owns that responsibility; the MDS just drops the record).

`PROXY_CANCEL` is the same as `PROXY_DONE(FAIL)` but represents
"PS gave up, did not even attempt", whereas `PROXY_DONE(FAIL)`
represents "PS attempted, ran into a recoverable error".  Both
discard the record; the difference is purely intent / accounting.

### Layout-build path during migration

The existing `nfs4_op_layoutget` builds a layout from
`i_layout_segments` directly.  With the migration record in
play:

```
build_layout_for_client(inode, segment_index):
    record = migration_record_find(inode)
    base_seg = inode->i_layout_segments->lss_segs[segment_index]
    if not record:
        return base_seg verbatim
    apply_deltas(base_seg, record->deltas[seg_index]) -> view_seg
    return view_seg
```

`apply_deltas` is the per-instance transformation: walk
`base_seg.ls_files`, for each instance look up a matching delta
by `(seg_index, instance_index)`, substitute / drop / add per the
delta's state.  The original `i_layout_segments` is never mutated
until `PROXY_DONE(OK)`.

This means LAYOUTGET during migration **always returns a single
coherent view** -- never a mix of pre-migration and post-migration
state.  Concurrent client writes happen against whatever view the
client most recently received via LAYOUTGET; if a delta changes
mid-flight, the client gets the new view on its next LAYOUTGET
(typical NFSv4 layout-recall semantics already cover this).

### Concurrent writes during migration

The design's "stay on L1 throughout" comment in the original 6c-x
plan is replaced by:

- **Source-of-truth for the layout shape** is always
  `i_layout_segments`; the migration record is a *view* on top.
- Client writes go to whichever instances appear in the view;
  the PS handles fan-out / shadowing per the delta semantics.
- On `PROXY_DONE(OK)`: a layout-recall pass is required for any
  client holding a layout that included a now-removed DRAINING
  slot.  Standard CB_LAYOUTRECALL machinery; the next LAYOUTGET
  reflects the post-migration shape.
- On `PROXY_DONE(FAIL)` / `PROXY_CANCEL`: no recall needed --
  the published view did not change shape, only the PS's
  fan-out behavior did, and that behavior was internal to the PS.
  (The PS owns cleanup of any half-written INCOMING data.)

### In-flight write coherence at migration start (B1)

A client holding a **pre-migration layout** still has the
DRAINING DS in its cached layout and will continue writing to it
for the duration of the in-flight window between
`PROXY_PROGRESS` mint and `PROXY_DONE(OK)`.  Without
intervention, those writes hit the DS that the migration is
decommissioning -- the exact CSM coherence trap that `mds.md`
"WCC and write layout checking" warns against.

The slice ships **omit-and-replace** as its only policy:

At `PROXY_PROGRESS` accept time, **before** the migration record
is hashed, the MDS issues CB_LAYOUTRECALL on every layout
outstanding for the inode whose composition includes the
DRAINING DS.  The accept call returns success only after the
recalls have either:
- been ACK'd via LAYOUTRETURN by the holding client, or
- exceeded the recall timeout and been administratively
  revoked via the existing layout-revocation path.

Only after that recall window closes does the migration record
become visible to the LAYOUTGET view-build path; subsequent
LAYOUTGETs see the post-image (DRAINING slot replaced by
INCOMING).  This guarantees no client writes hit the DRAINING DS
after the record exists; the recall window is the only point
where a client's pre-migration layout could still target it,
and the recall is mandatory before the migration starts.

Cost: a hard dependency on responsive CB_LAYOUTRECALL across
every client.  A client whose CB back-channel is broken will
have its layout revoked (existing infra), at which point its
next op gets `NFS4ERR_BADHANDLE` / `NFS4ERR_NO_GRACE` per the
revocation rules and it falls back to non-pNFS I/O through the
MDS.  This is acceptable for slice 6c-x; the alternative
(keep-and-shadow) requires PS-as-DS plumbing that this slice
does not deliver.

**Keep-and-shadow** is the deferred alternative.  It would
let the client keep writing to the DRAINING DS while the PS
shadows the writes to the INCOMING DS, avoiding the hard CB
dependency.  The natural mechanism is the `INTERPOSED` state
(the PS appears in the layout in place of the DRAINING DS, the
client writes to the PS, the PS fans out to the DRAINING and
INCOMING DSes).  This requires the PS to expose itself as a
flex-files data server -- a role it does not have today.
Wiring lives behind the `INTERPOSED` state in the state
machine; the state is defined for forward-compat but no slice
6c-x code path emits `INTERPOSED` deltas.  Tracked as a
follow-up that can land in any slice once the PS-as-DS plumbing
is available.

### Lease accounting + PS-crash reaper (B2)

A migration record is leased: the PS renews by sending
`PROXY_PROGRESS` on its registered-PS session.  The lease length
matches the standard NFSv4 lease (90s by default; same as
`ss_lease_period`).  `record.last_progress_mono_ns` is updated on
every renewal.

A dedicated **migration reaper thread** (modelled on
`lib/utils/lease_reaper.c`) scans the migration table on the
same cadence as the lease reaper:

```
for each record under rcu_read_lock + Rule 6 ref:
    expiry = record.last_progress_mono_ns + (lease_period * 1.5) ns
    if reffs_now_ns() > expiry:
        atomic_store(&record.phase, ABANDONED)
        unhash from inode-keyed and stateid-keyed tables
        deltas[] never collapsed into i_layout_segments;
        i_layout_segments stays at the pre-migration shape.
        LOG one line: "migration record abandoned: file_fh=..."
        put creation ref -> RCU-deferred free
```

`ABANDONED` is the terminal state for a reaped record.  Half-
written INCOMING data on DSes is **NOT_NOW_BROWN_COW**:
the slice does not yet garbage-collect orphan DS data; the
operator is expected to identify and clean up via the existing
mirror-lifecycle drain tooling (or, eventually, an admin op
that catalogs orphan files by `mds_uuid`/listener).  This is
called out explicitly in `proxy_stateid.c` and
`migration_record.c` source comments, and tracked in
`mirror-lifecycle.md` follow-ups.

If the **MDS** restarts while a migration is in flight, slice
6c-x ships in-memory only (B3 below); every record is dropped on
restart.  The PS observes its `PROXY_PROGRESS` lease expire (the
new MDS has no record of the registration) and abandons the
move.  Any half-written INCOMING data is, again,
NOT_NOW_BROWN_COW.

### proxy_stateid lifetime and post-reboot semantics (B3)

`proxy_stateid` follows the NFSv4 stateid invariants in
RFC 8881 S8.2.2:

- **Within a single (server_state, boot_seq) tuple**: the MDS
  guarantees uniqueness across all minted proxy_stateids for the
  duration of that boot.  `other[12]` is allocated from
  `getrandom(2)`; collision is cryptographically negligible at
  the volume of expected migrations (typically << 2^48 over the
  lifetime of an MDS).
- **Across reboot, with persistence (slice 6c-zz)**: the MDS
  reloads the migration record on restart and the proxy_stateid
  remains valid; PROXY_DONE / PROXY_CANCEL on the same
  proxy_stateid succeed against the reloaded record.
- **Across reboot, without persistence (slice 6c-x ship state)**:
  the MDS has no record of any proxy_stateid minted before the
  reboot.  Per RFC 8881 S8.2.2, the MDS MUST distinguish
  "stateid I never issued" (`NFS4ERR_BAD_STATEID`) from "stateid
  I issued in a prior boot but no longer have"
  (`NFS4ERR_STALE_STATEID`).  The MDS's `boot_seq` is encoded in
  the stateid lookup path: any `proxy_stateid.other` whose
  high-order 8 bytes do not match the current `boot_seq` returns
  `NFS4ERR_STALE_STATEID`.  This is wire-visible and the IETF
  draft must specify it.

Implementation: `proxy_stateid.other[12]` is laid out as
`{ uint16_t boot_seq | uint16_t reserved | uint64_t opaque }`,
matching reffs's existing 16-bit `sps_boot_seq` width (see
`lib/include/reffs/server_persist.h` and the `clientid4`
partition in CLAUDE.md).  The opaque tail is 64 bits of
`getrandom(2)` output; collision is cryptographically
negligible at the volume of expected migrations (<< 2^32 over
the lifetime of a single boot).  The `reserved` field is zero;
implementations MUST emit zero and MUST NOT reject non-zero
on receipt (left for future widening of `boot_seq` if ever
needed).  The boot_seq prefix lets the lookup code reject
stale stateids cheaply without a hash probe.  This is the same
trick used by `clientid4` (RFC 8881 S2.4.1) -- not new
mechanism, just standard NFSv4 stateid hygiene applied to a
new stateid type.

The IETF draft section "Define proxy_stateid as a new stateid
type" calls this layout out as **informative implementation
guidance** -- the wire-visible requirement is only that
post-reboot proxy_stateids return `NFS4ERR_STALE_STATEID`, not
how the MDS detects "post-reboot".

### State-machine completeness (W2)

`instance_state` transitions form a small DAG:

```
(slot starts as STABLE, exists in i_layout_segments)

  STABLE
     |
     +-- DRAINING (admin marks DS for decommission)
     |       |
     |       +-- DRAINING_RECALL (E is in sync; CB_LAYOUTRECALL
     |       |     issued on clients holding the DRAINING slot)
     |       |
     |       +-- (PROXY_DONE/OK)  -> slot removed from layout
     |       +-- (PROXY_DONE/FAIL or CANCEL) -> back to STABLE
     |
     +-- INTERPOSED (PS funnels writes for this slot)
             |
             +-- (PROXY_DONE/OK)  -> slot becomes STABLE,
             |                       fanout collapsed into the
             |                       new instance set
             +-- (PROXY_DONE/FAIL or CANCEL) -> back to STABLE

  (new slot)
     |
     +-- INCOMING (admin / autopilot adds an instance)
             |
             +-- (PROXY_DONE/OK)  -> slot becomes STABLE
             +-- (PROXY_DONE/FAIL or CANCEL) -> slot removed
```

Invariants enforced by the migration record builder:

1. **One delta per (seg_index, instance_index) per record.**
   Two deltas targeting the same instance position is a builder
   bug; the validator returns `EINVAL` at record-creation time
   so the autopilot never ships such a record.
2. **A given (seg_index, instance_index) appears in at most one
   active migration record.**  The MDS serializes by refusing a
   second `PROXY_PROGRESS` assignment that touches an instance
   already covered by an active record.  This serialization
   point lives in slice 6c-y; slice 6c-x ships an assertion in
   the record-creation API that fires if the autopilot ever
   tries.
3. **No mid-record transitions.**  An instance's `ld_state` is
   immutable once the record is hashed.  The state diagram
   above shows logical transitions (e.g., the keep-and-shadow
   `DRAINING -> DRAINING_RECALL` step) that, when implemented,
   are encoded as **record replacement**: the autopilot
   abandons the current record and creates a fresh one with
   the new instance state.  No in-place mutation of `ld_state`
   ever happens after hashing; this keeps the LAYOUTGET
   view-build path lock-free.  Slice 6c-x ships only the
   subset of the diagram reachable from omit-and-replace
   (STABLE -> DRAINING -> {removed via DONE(OK), restored via
   DONE(FAIL)/CANCEL}; INCOMING -> {STABLE via DONE(OK),
   removed via DONE(FAIL)/CANCEL}).  INTERPOSED transitions
   are defined for forward-compat but unreachable via the
   slice 6c-x autopilot path.
4. **Terminal phases are sticky.**  `phase` transitions
   PENDING -> IN_PROGRESS -> COMMITTING -> {COMMITTED, ABANDONED}.
   Once COMMITTED or ABANDONED, the record is unhashed and
   freed; further DONE / CANCEL on the same proxy_stateid
   return BAD_STATEID (or STALE_STATEID post-reboot).

`PROXY_DONE(FAIL)` and `PROXY_CANCEL` differ in *intent*
(attempted vs. did-not-attempt) but their state-machine effect
is identical: the record transitions to ABANDONED.  The
distinction surfaces in the per-record audit log entry and in
the operator-facing probe stats (separate counters), not in
behavior.  Tests collapse them when only the layout-state
outcome is being asserted; the audit/probe surface tests live
with the probe slice.

### RCU + Rule 6 discipline for migration_record table (W3)

Both the proxy_stateid table (slice 6c-x.1) and the migration
record table (slice 6c-x.2) are `cds_lfht` instances with
ref-counted entries.  Both follow Rule 6 in
`.claude/patterns/ref-counting.md` -- specifically the
"advance iterator before put" pattern that the trust-stateid
work codified.

For the migration record table the dual-index complicates
drain: each record is in two hashes (by `proxy_stateid.other`
and by inode pointer).  Removal must `cds_lfht_del` from both
under the same RCU read-side critical section, then drop the
creation ref to schedule `call_rcu` free.  Lookup paths take
a find ref on the hit side; if the find ref races with removal
on the other index, the find ref still resolves (Rule 6
guarantee) and the caller drops the ref normally.

Mandatory tests in `proxy_migration_test.c`:

- `test_migration_record_drain_no_uaf` -- shutdown drain with
  outstanding find refs; ASAN clean.
- `test_migration_record_remove_during_lookup` -- inject a
  removal between the hash hit and the put on the other index;
  no UAF.
- `test_migration_record_dual_index_consistency` -- after
  remove, neither index returns the record.

### IETF draft delta (W4)

The "Revised IETF draft delta" section below covers the wire
shapes; W4 demands more detail on lifetime, value-space
discrimination, and renewal:

- **proxy_stateid value space**: separate from open / lock /
  layout / delegation by *context* (only PROXY_DONE / PROXY_CANCEL
  / PROXY_PROGRESS args carry a `proxy_stateid`).  No in-band
  type tag; if a leaked proxy_stateid is presented to a non-PROXY
  op the receiving handler returns `NFS4ERR_BAD_STATEID` (the
  per-op stateid lookup tables are disjoint by construction).
- **Lifetime**: minted at `PROXY_PROGRESS` reply; expires at
  PROXY_DONE / PROXY_CANCEL completion *or* at PROXY_PROGRESS
  lease loss (1.5x lease period of silence).
- **Renewal**: `PROXY_PROGRESS` carries the proxy_stateid in the
  request to renew; the response echoes it.  `seqid` semantics
  match other NFSv4 stateids: the MDS bumps on each issuance
  (allocate, renew); the PS sends the most-recent seqid it has;
  out-of-order seqids are answered with the canonical
  `NFS4ERR_OLD_STATEID` (per RFC 8881 S8.2.4).
- **Authorization wire-error contract**: the MDS MUST reject
  PROXY_DONE / PROXY_CANCEL where the calling session's
  registered-PS identity does not match the migration record's
  `owner_reg`, with `NFS4ERR_PERM`.  PUTFH file_fh mismatch is
  `NFS4ERR_BAD_STATEID`.  Stale-boot proxy_stateid is
  `NFS4ERR_STALE_STATEID`.  All three are normative.

## Revised IETF draft delta

The data-mover draft (`draft-haynes-nfsv4-flexfiles-v2-data-mover`)
needs the following updates.  All wire-visible items are
normative ("MUST"); informational items are advisory and live in
their own subsection.

### Normative

1. **Define proxy_stateid as a new stateid type**.  Add a section
   alongside the existing PROXY_REGISTRATION / PROXY_PROGRESS
   surface that defines:
   - **Wire shape**: reuses `stateid4 { uint32_t seqid; opaque
     other[12]; }`.  No new XDR type.
   - **Value-space discrimination**: the proxy_stateid value
     space is disjoint from open / lock / layout / delegation
     stateids by *context* -- only PROXY_DONE / PROXY_CANCEL /
     PROXY_PROGRESS args carry a `proxy_stateid`.  No in-band
     type tag.  An implementation MUST NOT use an open / lock /
     layout / delegation stateid lookup table to resolve a
     proxy_stateid.  Conversely, a leaked proxy_stateid presented
     to a non-PROXY op (SETATTR, READ, WRITE, ...) MUST be
     rejected with `NFS4ERR_BAD_STATEID` by the per-op stateid
     validator (the per-op tables are disjoint by construction).
   - **Boot_seq prefix**: the implementation MAY embed the MDS
     boot_seq in the high-order bytes of `other[12]` (informative
     guidance; not normative).  This enables cheap
     `NFS4ERR_STALE_STATEID` distinction across reboots.
   - **MDS minting at assignment time** (in `PROXY_PROGRESS`
     reply).
   - **Lifetime**: minted at assignment, expires at PROXY_DONE /
     PROXY_CANCEL completion *or* at lease loss
     (1.5x lease period of silence on PROXY_PROGRESS renewals).
   - **Renewal**: PROXY_PROGRESS request carries the
     proxy_stateid; the response echoes it.  `seqid` semantics
     match other NFSv4 stateids (RFC 8881 S8.2.4): the MDS bumps
     `seqid` on each issuance (allocate, renew); the PS sends
     the most-recent `seqid` it has; out-of-order seqids return
     `NFS4ERR_OLD_STATEID`.
2. **Re-shape PROXY_DONE / PROXY_CANCEL args**.  Both currently
   take layout stateid.  Replace with proxy_stateid.  Both
   require PUTFH preceding (file scope is enforced by the
   matched record's `file_fh`, not by an explicit args field):
   ```
   struct PROXY_DONE4args {
       proxy_stateid4     pda_stateid;
       proxy_done_status4 pda_status;   // OK | FAIL
   };
   struct PROXY_CANCEL4args {
       proxy_stateid4     pca_stateid;
   };
   ```
3. **Authorization rule** (normative).  The MDS MUST validate
   that the calling session's *registered-PS identity* (the
   `prr_registration_id` matched at PROXY_REGISTRATION, OR the
   matched GSS principal / mTLS fingerprint when
   `prr_registration_id` is empty) matches the migration record's
   owner before applying DONE / CANCEL.  Note: this is the
   registered-PS identity, NOT the per-EXCHANGE_ID `clientid4`,
   so a PS reconnect (new `clientid4`, same registration) does
   not orphan its own in-flight migrations.  Mismatch returns
   `NFS4ERR_PERM`.
4. **Wire-error contract** (normative).  Listed in priority
   order; the first violated condition determines the error:
   - Caller's session is not registered-PS: `NFS4ERR_PERM`.
   - `pda_stateid.other` / `pca_stateid.other` does not match
     the current `boot_seq`: `NFS4ERR_STALE_STATEID`.
   - No migration record found for the stateid in this boot:
     `NFS4ERR_BAD_STATEID`.
   - Caller's registered-PS identity does not match
     `record.owner`: `NFS4ERR_PERM`.
   - PUTFH file_fh does not match `record.file_fh`:
     `NFS4ERR_BAD_STATEID`.
   - `seqid` out of order vs the most recently issued
     proxy_stateid for this record: `NFS4ERR_OLD_STATEID`.

### Informative

The per-instance delta machinery is purely an implementation
concern; the draft doesn't need to spell out STABLE / DRAINING /
INCOMING / INTERPOSED state names.  It does need to acknowledge
that PROXY_DONE(OK) "commits the migration" without prescribing
*how* the post-migration layout shape is computed -- that's left
to the MDS implementation.  An informative paragraph noting
"per-instance vs whole-layout" as two valid implementation
approaches would help future implementers, and that the choice
between them is constrained by the **drain coherence policy**
(keep-and-shadow vs. omit-and-replace) the MDS picks for each
migration.

A second informative paragraph should call out that the
distinction between PROXY_DONE(FAIL) and PROXY_CANCEL is purely
intent / accounting -- both result in the migration record
being abandoned -- and that the MDS is free to surface the
distinction in operator-facing telemetry but not on the wire.

## Revised slice 6c-x scope (reffs)

### Tests first (per .claude/roles.md planner role)

New unit-test file: `lib/nfs4/server/tests/proxy_migration_test.c`.

| Test | Intent |
|------|--------|
| `test_proxy_stateid_alloc_unique` | Two `proxy_stateid_alloc` calls in same boot return different `other[12]` |
| `test_proxy_stateid_persists_across_lookup` | Alloc, look up by `other`, get same record |
| `test_proxy_stateid_stale_after_boot_change` | Set boot_seq, alloc, change boot_seq, lookup -> `NFS4ERR_STALE_STATEID` (B3) |
| `test_proxy_stateid_old_seqid_rejected` | Mint with seqid=N+1, present seqid=N, lookup -> `NFS4ERR_OLD_STATEID` |
| `test_migration_record_find_by_stateid` | Find returns record; not-found returns NULL |
| `test_migration_record_find_by_inode` | Find-by-inode returns the active record (one per inode) or NULL |
| `test_migration_record_dual_index_consistency` | After remove from one index, lookup via the other index returns NULL (W3) |
| `test_migration_record_drain_no_uaf` | Shutdown drain with outstanding find refs; ASAN clean (W3) |
| `test_migration_record_remove_during_lookup` | Inject removal between hash hit and put on the other index; no UAF (W3) |
| `test_migration_record_owner_mismatch_done` | DONE from non-owner registered-PS identity returns `NFS4ERR_PERM`, record stays |
| `test_migration_record_owner_mismatch_cancel` | CANCEL from non-owner identity returns `NFS4ERR_PERM`, record stays |
| `test_migration_record_owner_reconnect_same_regid` | DONE from a *new* `clientid4` but *same* `prr_registration_id` succeeds (W1) |
| `test_migration_record_file_mismatch_done` | DONE on wrong PUTFH returns `NFS4ERR_BAD_STATEID`, record stays |
| `test_migration_record_builder_rejects_dup_instance` | Two deltas targeting the same (seg_index, instance_index) -> creator returns `EINVAL` (W2 invariant 1) |
| `test_migration_record_builder_rejects_overlap` | Two records both touching the same (inode, seg_index, instance_index) -> second creation returns `EBUSY` (W2 invariant 2) |
| `test_proxy_done_ok_collapses_deltas` | Apply 3-instance delta (D=DRAINING, I=INCOMING, S=STABLE), DONE(OK), verify `i_layout_segments` reflects post-image (D removed, I -> S) |
| `test_proxy_done_fail_discards_deltas` | Same setup, DONE(FAIL), verify `i_layout_segments` unchanged from pre-image |
| `test_proxy_cancel_discards_deltas` | Same setup, CANCEL, verify `i_layout_segments` unchanged |
| `test_proxy_done_idempotent` | DONE(OK), then second DONE on same proxy_stateid -> `NFS4ERR_BAD_STATEID` (record gone) |
| `test_proxy_cancel_idempotent` | CANCEL, then second CANCEL -> `NFS4ERR_BAD_STATEID` (record gone) |
| `test_proxy_done_race_with_cancel` | Two threads send DONE and CANCEL concurrently; exactly one wins, the other gets `NFS4ERR_BAD_STATEID`, no UAF |
| `test_proxy_done_concurrent_with_layoutget` | DONE in one thread races with LAYOUTGET that reads the migration record; either pre- or post-image returned, never torn (N1) |
| `test_proxy_done_ok_concurrent_with_layoutreturn` | Client LAYOUTRETURN happens at the same instant DONE(OK) collapses deltas; post-image attrs reflect the union of both events (N1) |
| `test_apply_deltas_unknown_instance_index` | Delta with `ld_instance_index` past `ls_nfiles` -> `apply_deltas` returns `EINVAL` (N1) |
| `test_layoutget_during_migration_view_omit_and_replace` | Slice 6c-x policy: LAYOUTGET returns post-image (DRAINING removed, INCOMING in its place) only after CB_LAYOUTRECALL completes (B1) |
| `test_proxy_progress_blocks_until_recall_ack` | `PROXY_PROGRESS` accept stalls while CB_LAYOUTRECALL on the in-flight layouts is outstanding; returns once all clients ACK or the recall window expires (B1) |
| `test_proxy_progress_proceeds_on_recall_revoke` | A client whose CB back-channel is broken has its layout revoked; `PROXY_PROGRESS` proceeds rather than stall indefinitely (B1) |
| `test_proxy_done_ok_layout_recall_emitted` | DRAINING slot triggers CB_LAYOUTRECALL on commit (stub: just verify the recall was queued; real CB delivery is existing infra) |
| `test_proxy_done_ok_layout_recall_failure` | CB_LAYOUTRECALL fails (client unreachable); DONE(OK) still completes the layout mutation, the failure is logged for the lease reaper to handle (N1) |
| `test_migration_reaper_abandons_expired` | Record's `last_progress_mono_ns` ages past 1.5x lease; reaper transitions to ABANDONED, unhashes from both tables, `i_layout_segments` unchanged (B2) |
| `test_concurrent_writes_during_migration_use_view` | Client LAYOUTGET sees the per-policy view; writes go to the right DSes per the policy default |

### Test impact on existing tests

| File | Impact |
|------|--------|
| `lib/nfs4/server/tests/layout_test.c` (if exists; otherwise `nfs4_op_layoutget` is exercised by integration only) | PASS -- migration record absent means LAYOUTGET returns base layout verbatim, identical to today |
| `lib/nfs4/tests/reflected_getattr_test.c` | PASS -- independent of migration |
| `lib/nfs4/tests/chunk_test.c` | PASS -- CHUNK ops are post-LAYOUTGET; the view is computed before they fire |
| `lib/nfs4/tests/delegation_lifecycle.c` | PASS -- delegations don't intersect migration |
| `lib/nfs4/ps/tests/*` | PASS -- PS-side; DONE / CANCEL are handler additions |
| All other `make check` tests | PASS |

No existing test is modified.

### Implementation slices within 6c-x

- **6c-x.0** -- prerequisite: capture the registered-PS identity
  on the session at PROXY_REGISTRATION time.  Today
  `nc_is_registered_ps` is a bool on `struct nfs4_client`; this
  slice adds `nc_registered_ps_identity[]` (a copy of
  `prr_registration_id` if non-empty, else the matched GSS
  principal / mTLS fingerprint string).  The migration record's
  `owner_reg` is populated from this field at `PROXY_PROGRESS`
  accept time.  Tests: identity captured at registration,
  preserved across SEQUENCE renewals, copied into migration
  record on accept.
- **6c-x.1** -- proxy_stateid type + allocator + lookup table.
  Lives in `lib/nfs4/server/proxy_stateid.c` (new); header in
  `lib/nfs4/include/nfs4/proxy_stateid.h`.
  Allocator pulls 12 bytes from `getrandom(2)` and hashes them
  into `cds_lfht`.  Same lifecycle pattern as `trust_stateid_test`
  (Rule 6 in `.claude/patterns/ref-counting.md`).
  Tests: alloc-unique, lookup-roundtrip, RCU-safe drain.
- **6c-x.2** -- migration record table.  Per-inode field
  (one record per inode at a time -- multiple concurrent
  migrations on the same file are out of scope; the MDS
  serializes by refusing a second `PROXY_PROGRESS` assignment
  until the first commits / cancels).  The record's
  `proxy_stateid.other` is also indexed in a global hash so
  DONE / CANCEL can look up by stateid in O(1).
  Tests: find-by-stateid, find-by-inode, owner-mismatch.
- **6c-x.3** -- `nfs4_op_proxy_done` and `nfs4_op_proxy_cancel`
  handlers.  Wire into `dispatch.c`.  Args / res structs come
  from the XDR walkback in slice 6c-w (re-encoded per the
  revised draft delta above; minor `xdr-parser` regen).
  Tests: DONE(OK) collapse, DONE(FAIL) discard, CANCEL discard,
  idempotent, race.
- **6c-x.4** -- layout-build "during-migration view" hook.
  Modify `nfs4_op_layoutget`'s build path to consult
  `migration_record_find_by_inode` and apply the deltas
  before encoding the layout body.  No change to
  `i_layout_segments` mutation paths.
  Tests: layoutget-during-migration-view,
  concurrent-writes-use-view.
- **6c-x.5** -- CB_LAYOUTRECALL on DONE(OK) when DRAINING
  removed.  Reuse existing recall infra; just queue the recalls.
  Tests: recall-emitted (stub-level).

### Persistence (deferred to 6c-zz per scope)

Migration records, deltas, and proxy_stateids are in-memory only
in slice 6c-x.  MDS restart drops them; the PS will see its
PROXY_PROGRESS lease expire and the assignment effectively
cancels.  Slice 6c-zz adds persistence + reclaim recognition.
Marked NOT_NOW_BROWN_COW in `proxy_stateid.c` and
`migration_record.c`.

**The in-memory-only behavior is bound to the wire-error contract
in this slice, NOT deferred** (per N2): the proxy_stateid lookup
path checks `boot_seq` on every request, so a stale stateid from
a prior boot returns `NFS4ERR_STALE_STATEID` even though no record
exists.  See "proxy_stateid lifetime and post-reboot semantics
(B3)" above.

### Drain-policy ship state (B1, N2)

Slice 6c-x ships **omit-and-replace as the only policy**.  It
relies on the existing CB_LAYOUTRECALL infrastructure (which is
production-tested) and avoids the PS-as-DS plumbing required by
keep-and-shadow.

This is NOT deferred -- it is the slice 6c-x ship policy.  The
deferral is keep-and-shadow (the `INTERPOSED` state in the state
machine), which lands when PS-as-DS plumbing is available.

## Open questions deferred to follow-up

- **Keep-and-shadow / `INTERPOSED` state**.  Requires the PS to
  appear in flex-files layouts as a data server (the client
  writes to the PS, the PS fans out to the DRAINING and
  INCOMING DSes).  The state is defined in the slice 6c-x state
  machine for forward-compat, but no slice 6c-x autopilot path
  emits `INTERPOSED` deltas.  Useful for FFv2 RS-encoding
  scenarios where the client must continue writing the parity
  stripe to an old DS while the new DS catches up; useful for
  deployments where CB_LAYOUTRECALL coverage is unreliable.
  Lands in any slice once PS-as-DS plumbing is available.
- **Orphan INCOMING DS data cleanup.**  When a migration record
  is abandoned (B2 reaper or PROXY_DONE(FAIL) / PROXY_CANCEL)
  the PS may have written partial data to an INCOMING DS that
  is now unreferenced.  No GC in slice 6c-x; operator cleans up
  via existing mirror-lifecycle tooling.  NOT_NOW_BROWN_COW;
  tracked in `mirror-lifecycle.md`.
- **Multi-segment migrations.**  Today's migrations affect one
  segment at a time.  An admin command that drains a DS used by
  multiple segments creates one migration record per affected
  segment (each with its own proxy_stateid).  The PS sees them
  as independent assignments.  No protocol change needed; the
  autopilot decides batching.
- **PS sees a migration on a file it did not request.**  E.g.,
  the autopilot pre-creates an INCOMING DS for a file before the
  PS polls.  When the PS polls and gets the assignment, the
  proxy_stateid is minted at that moment -- the PS only learns
  about migrations it owns.  No leak.
- **Cross-PS reassignment on lease loss.**  If PS-A's lease
  expires mid-move, the migration record is **abandoned**, not
  reassigned.  The autopilot may issue a fresh assignment to
  PS-B with a brand-new proxy_stateid; PS-B discovers any
  half-finished work via the file's existing layout shape and
  decides whether to retry from scratch.  Simpler than reassign;
  matches the existing 6c-zz design's "don't reassign across PSes"
  decision.

## Next steps (in order)

1. **This document** committed to `.claude/design/`.
2. **Reviewer pass** on this revision (look for mechanism gaps,
   deferred items that should not be deferred, RFC alignment
   issues).
3. **IETF draft update** in `~/Documents/ietf/flexfiles-v2/`
   (or wherever the data-mover draft lives) per the "Revised
   IETF draft delta" section above.  Re-submit as a new revision.
4. **reffs implementation** of slice 6c-x in 5 sub-slices as
   outlined above.  Each sub-slice lands independently with its
   own tests.

The original `proxy-server-phase6c.md` "Revised slice ladder"
keeps slices 6c-y / 6c-z / 6c-zz as-is; only 6c-x is replaced
by the per-instance + proxy_stateid model.
