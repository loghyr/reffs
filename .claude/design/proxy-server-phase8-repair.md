<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Real Repair via Proxy Server (proxy-server Phase 8)

## 1. Context and scope

Bucket 4 shipped on 2026-06-10: the DS-side `OP_CHUNK_WRITE_REPAIR`,
the MDS-side `OP_CHUNK_REPAIRED`, the layout flag
`FFV2_DS_FLAGS_REPAIR`, and an operator-driven `ec_demo repair`
driver.  The 80-cell bench against dreamer shows the wire shape
works (R1 + R2 tables in `ietf126.md` Bucket 4).

What is missing for repair to be **real** rather than demo-grade:

1. Nothing marks `FFV2_DS_FLAGS_REPAIR` on a mirror today.
   `grep -rn FFV2_DS_FLAGS_REPAIR` finds only the *clear* path
   (`chunk.c::nfs4_op_chunk_repaired`) and unit-test setup in
   `chunk_repair_test.c`.  The bench works because the operator
   runs `ec_demo repair --skip-ds <mask>` and ec_demo sends
   `OP_CHUNK_WRITE_REPAIR` unconditionally; the flag in the
   layout is never read by a real client driver.
2. There is no autopilot.  Whole-file repair is an operator
   verb, not a server-driven recovery.
3. The compute-node client is on the hot path of the repair I/O,
   which is the wrong policy: customer workloads should not pay
   the bulk read+decode+encode+write cost.

**Policy decision (2026-06-30):** customer compute nodes MUST
NOT drive whole-file repair.  Repair work routes to a
registered proxy server (PS) only.  The PS dispatch mechanism
already ships (Phase 6c): the PS polls the MDS with
`PROXY_PROGRESS` (op 93); the MDS reply carries
`proxy_assignment4` records; per-assignment terminal status
is reported back via `PROXY_DONE` (op 94) keyed on the
MDS-minted `proxy_stateid4`.  See
`.claude/design/proxy-server-phase6c-revision.md` for the
authoritative architecture.

### Why this is much smaller than it sounds

The MDS-side dispatch surface is already in place from the
MOVE work:

| Shipped piece | Location |
|---------------|----------|
| `proxy_op_kind4` enum (`MOVE = 0`, `REPAIR = 1`, `CANCEL_PRIOR = 2`) | `lib/xdr/nfsv42_xdr.x:3769` |
| `proxy_assignment4` XDR (kind, stateid, file_fh, source/target deviceid) | `lib/xdr/nfsv42_xdr.x:3790` |
| FIFO assignment queue (push / pop) with `paq_kind` already carrying REPAIR | `lib/nfs4/server/proxy_assignment_queue.c`, `lib/nfs4/include/nfs4/proxy_assignment_queue.h` |
| `nfs4_op_proxy_progress` handler that pops up to 8 items, mints `proxy_stateid4`, creates per-inode migration records, builds the `proxy_assignment4` array on the wire | `lib/nfs4/server/proxy_registration.c:304` |
| `nfs4_op_proxy_done` / `nfs4_op_proxy_cancel` terminal handlers | `lib/nfs4/server/dispatch.c:144-145` |
| Per-inode invariant (one in-flight migration per inode, returns `-EBUSY` otherwise) | `migration_record_create()` |
| `proxy_stateid4` minting with embedded `boot_seq` for cross-reboot stale detection | `proxy_stateid_alloc()` |

The PS side has nothing yet -- `grep -rn PROXY_OP_REPAIR lib/nfs4/ps`
returns empty.  Phase 8 supplies the PS-side handler, the
producer triggers on the MDS side, and the admin probe ops.

### What this means for capability advertisement

- **No new client capability bit.**  Regular clients see
  `FFV2_DS_FLAGS_REPAIR` on a mirror as informational: prefer
  other mirrors for reads, never write to it.  They never act
  on it.
- **No new PS capability bit either, this slice.**  The
  homogeneous-PS-fleet assumption stands -- every registered
  PS is the same reffs binary in a different role and supports
  every encoding the MDS does.  Heterogeneous deployments
  (extending `PROXY_REGISTRATION` with an encoding-set bitmap)
  are deferred to Phase 9 (see Section 10).

### Security ramification of flipping the autopilot on

`ec-repair.md` §12a names four production-hardening markers
in `chunk.c` that gate scaling past the cooperative-client
model:

1. `OP_CHUNK_WRITE_REPAIR` mid-PENDING collision (`chunk.c:1467`)
2. `OP_CHUNK_REPAIRED` rigorous `cpa_stateid` validation (`chunk.c:1086`)
3. `OP_CHUNK_REPAIRED` range matching (`chunk.c:1094`)
4. `OP_CHUNK_REPAIRED` clientid match (`chunk.c:1100`)

Today the operator gate makes these moot -- only `ec_demo repair`
sends `OP_CHUNK_WRITE_REPAIR`, only `ec_demo repair` sends
`OP_CHUNK_REPAIRED`, both of those are sanctioned writers.  An
autopilot that fires on LAYOUTERROR (Slice 1 below) means the
MDS now generates `OP_CHUNK_REPAIRED` traffic in response to
client-reported errors, and the PS now generates
`OP_CHUNK_WRITE_REPAIR` traffic in response to MDS assignments,
without operator involvement.  The PS being the *sole* repair-
writer narrows the exposure window -- only the PS holds the
trust-stateid that gates `OP_CHUNK_WRITE_REPAIR` at the DS, so
a regular client cannot synthesize repair traffic -- but the
four markers must still close before this slice can be flagged
production-grade.

**Decision: Phase 8 keeps the autopilot operator-gated by
default.**  Slice 1 sets the flag on LAYOUTERROR but does NOT
enqueue an assignment; only the probe ops in Slice 2
(`INODE_REPAIR_FORCE`) do.  Slice 5 adds an opt-in config bit
`[proxy_server] autopilot_on_layouterror = true` that wires
the LAYOUTERROR -> enqueue path; that bit stays off until the
§12a markers close in a follow-up slice family (Phase 9a).
Production deployments leave it off until then; the demo and
the integration test flip it on explicitly.

### In scope

- MDS-side: LAYOUTERROR / `INODE_REPAIR_FORCE` set
  `FFV2_DS_FLAGS_REPAIR` on the affected mirror.
- MDS-side: producer that enqueues a `proxy_assignment_item`
  with `paq_kind = PROXY_OP_REPAIR` (admin path always;
  LAYOUTERROR path gated on the new config bit).
- PS-side: new handler that processes `proxy_assignment4`
  records with `pa_kind == PROXY_OP_REPAIR` arriving in
  `PROXY_PROGRESS` replies.
- PS-side: drive the existing `ec_repair_codec()` entry point
  in `lib/nfs4/ps/ec_pipeline.c`.
- PS-side: on completion, issue `OP_CHUNK_REPAIRED` to the
  MDS then `PROXY_DONE(proxy_stateid, status)`.
- Admin: `INODE_REPAIR_FORCE`, `INODE_REPAIR_STATUS`,
  `INODE_REPAIR_CANCEL` probe ops + Python / C CLI.
- Lock-ordering and RCU discipline for the new `ldf_repair_pid`
  field on `struct layout_data_file`.

### Out of scope

- Periodic scrub thread (whole-namespace background sweep that
  triggers repair).  Phase 10.
- DS-side CRC-fail bubbled up to the MDS as a repair trigger.
  Today the DS returns `NFS4ERR_IO` to the client and logs;
  bubbling up needs a new protocol surface.  Phase 11.
- Lost-DS file rebuild (R3 in the bench plan).  Fleet-level
  operation requiring a registry scan path.  Phase 9b.
- Multi-PS pipelining for very large files.  Phase 9c.
- Heterogeneous PS fleet (encoding-set bitmap in
  `PROXY_REGISTRATION`).  Phase 9d.
- The four `chunk.c` production-hardening markers from
  `ec-repair.md` §12a.  Tracked separately as Phase 9a; gates
  the LAYOUTERROR autopilot opt-in.

## 2. State machines

### 2.1 MDS-side per-mirror repair lifecycle

```
                  trigger                  PROXY_PROGRESS pop
                  (probe op /              + assignment built
                   LAYOUTERROR             + migration_record_create
                   when autopilot on)      |
                  |                         v
  +-------+   set flag    +-------------+    +-------------+
  | CLEAN | -----------> | NEEDS-REPAIR | -> | IN-REPAIR   |
  +-------+               +-------------+    +-------------+
       ^                                          |
       |  CHUNK_REPAIRED                          |
       |  (PS issues before                       |
       |   PROXY_DONE)                            |
       +------------------------------------------+
                                                  |
                                                  | PROXY_DONE(status != NFS4_OK)
                                                  | or PROXY_CANCEL
                                                  | or PS lease expired
                                                  v
                                          +-------------+
                                          | NEEDS-REPAIR | (retry-eligible)
                                          +-------------+
```

States:

- `CLEAN`: `ldf_flags & FFV2_DS_FLAGS_REPAIR == 0` and no
  outstanding `proxy_stateid` for this mirror.
- `NEEDS-REPAIR`: flag set; no assignment dispatched.  Visible
  in client-issued layouts as informational.
- `IN-REPAIR`: flag set AND an assignment has been popped + a
  `proxy_stateid4` minted.  Tracked via a new per-mirror
  `ldf_repair_pid` field (the `proxy_stateid4.other[12]`
  contents), in-memory only.

Transitions:

| From | To | Trigger | Side effects |
|------|----|---------|--------------|
| `CLEAN` | `NEEDS-REPAIR` | LAYOUTERROR (autopilot on) with `NFS4ERR_IO`/`NFS4ERR_BADIOMODE`/`NFS4ERR_ACCESS`/`NFS4ERR_PERM` matching a mirror; or `INODE_REPAIR_FORCE` probe | `ldf->ldf_flags |= FFV2_DS_FLAGS_REPAIR`; `inode_sync_to_disk`; `proxy_assignment_queue_push(REPAIR)`; bump `sb_chunk_stats.cs_repair_needed` |
| `NEEDS-REPAIR` | `IN-REPAIR` | PS poll pops the assignment and `migration_record_create` returns 0 | `proxy_stateid_alloc` fills the migration record; `ldf->ldf_repair_pid` records the `other[12]` for ops + status surface; bump `cs_repair_dispatched` |
| `IN-REPAIR` | `CLEAN` | PS issues `OP_CHUNK_REPAIRED` (existing path clears the flag); then `PROXY_DONE(NFS4_OK)` retires the migration record | `chunk.c::nfs4_op_chunk_repaired` clears the flag; `nfs4_op_proxy_done` looks up + retires the migration record; `ldf_repair_pid` reset; bump `cs_repair_completed` |
| `IN-REPAIR` | `NEEDS-REPAIR` | `PROXY_DONE(status != NFS4_OK)`, `PROXY_CANCEL` from admin, or PS lease expiry without completion | Clear `ldf_repair_pid` only; flag stays set; record retired; eligible for re-enqueue on next trigger |
| `NEEDS-REPAIR` | `NEEDS-REPAIR` | Repeated trigger while already-flagged | Idempotent; don't re-enqueue if `ldf_repair_pid != 0` or the queue already has a matching item (dedup by `(sb_id, ino, mirror_index)`) |

### 2.2 PS-side per-assignment lifecycle

```
        PROXY_PROGRESS                   worker picks up
        reply contains an                from per-PS queue
        assignment with                  |
        pa_kind == REPAIR                v
              |             +--------------+
              |             |  RUNNING     |
              v             |  (ec_repair_ |
        +----------+        |   codec())   |
        | QUEUED   | -----> +------+-------+
        +----------+               |
                                   +--------------+--------------+--------------+
                                   |              |              |              |
                                   v              v              v              v
                              +--------+   +--------+   +--------+   +-----------+
                              | DONE   |   | FAILED |   |CANCELLED|  | LEASE_   |
                              | (ok)   |   |        |   |         |  | EXPIRED  |
                              +---+----+   +---+----+   +----+----+   +-----+----+
                                  |            |             |              |
                                  v            v             v              v
                              CHUNK_REPAIRED   PROXY_DONE   PROXY_DONE     no DONE;
                              + PROXY_DONE     (FAILED)     (DELAY)        MDS reaps
                              (NFS4_OK)                                    when lease
                                                                          expires
```

PS-side states are pure in-memory bookkeeping per assignment;
no on-disk persistence.  On PS restart, in-progress
assignments are abandoned -- the MDS's lease reaper will
transition the matching migration records back to
`NEEDS-REPAIR` and the next pop redispatches.

Transitions:

| From | To | Trigger | Side effects |
|------|----|---------|--------------|
| (no state) | `QUEUED` | `PROXY_PROGRESS` reply parsed; assignment added to per-PS queue | Bump `ps_assignments_queued` |
| `QUEUED` | `RUNNING` | Worker thread picks up | `_Atomic` cancel flag initialised to 0 |
| `RUNNING` | `DONE` | `ec_repair_codec()` returns 0 + verify-back succeeds | Send `OP_CHUNK_REPAIRED` to MDS; on success send `PROXY_DONE(NFS4_OK)` |
| `RUNNING` | `FAILED` | `ec_repair_codec()` returns non-zero, or `OP_CHUNK_REPAIRED` fails | Send `PROXY_DONE(<error>)`; do NOT send `OP_CHUNK_REPAIRED` for a failed repair |
| `RUNNING` | `CANCELLED` | Cancel flag set (by polling between stripes after a fresh `PROXY_PROGRESS` carried a `PROXY_OP_CANCEL_PRIOR` for this `pa_stateid`) | Stop work ASAP; send `PROXY_DONE(NFS4ERR_DELAY)` to signal cancelled-not-broken |
| `RUNNING` | `LEASE_EXPIRED` | PS-side detects its registration lease lapsed | Stop work; do NOT send `PROXY_DONE` (the session is dead); MDS-side migration reaper retires |

The PS-side cancel-flag check uses C11 atomics per
`.claude/standards.md`: write side
`atomic_store_explicit(&assn->cancel, true, memory_order_release)`,
read side
`atomic_load_explicit(&assn->cancel, memory_order_acquire)`.
Check between every stripe and inside any inner loop that runs
longer than ~100 ms.

## 3. On-disk format

`ldf_flags` already persists per `ec-repair.md` §3 (POSIX
`.layouts` + RocksDB ldf record) -- shipped 2026-06-10.

`ldf_repair_pid` is in-memory only -- a 12-byte slot on
`struct layout_data_file` (matches `stateid4.other[12]` width).
On MDS restart it is zeroed; the matching migration record is
also gone (the existing `migration_record` table is in-memory);
existing `ldf_flags` values reload as-is.  Any in-flight PROXY
assignment becomes orphaned at the PS and times out on the
PS's own lease; the next trigger redispatches cleanly.

Migration record persistence is handled by the existing Phase
6c-zz follow-up (see `proxy-server-phase6c-revision.md`); this
slice does not add to that surface.

No new on-disk schema, no version bump.  `SB_REGISTRY_VERSION`
stays at 1.  Per CLAUDE.md "Deployment Status: no persistent
storage deployed", no migration code.  Stated explicitly per
the planner BLOCKER on persistence-story discipline.

## 4. Wire surfaces

### 4.1 Existing ops we extend

All three proxy fore-channel ops are already wire-allocated
and handler-dispatched.  This slice extends their behaviour:

- **`PROXY_PROGRESS` (op 93)** -- the existing
  `nfs4_op_proxy_progress` (`proxy_registration.c:304`) already
  pops queue items, mints `proxy_stateid4`, creates migration
  records, builds `proxy_assignment4` entries with `pa_kind`
  populated from `paq_kind`.  No XDR change.  No handler
  change needed for the REPAIR kind -- the dispatch already
  emits REPAIR assignments verbatim because `paq_kind` is
  carried through.  What is missing is a *producer* of REPAIR
  items (Slice 1, Slice 2).
- **`PROXY_DONE` (op 94)** -- existing terminal-status handler.
  REPAIR-specific consequence: when `pd_status == NFS4_OK`, the
  PS has already issued `OP_CHUNK_REPAIRED` separately to clear
  `FFV2_DS_FLAGS_REPAIR`; `PROXY_DONE` just retires the
  migration record + clears `ldf_repair_pid`.  When
  `pd_status != NFS4_OK`, retire the record and leave
  `ldf_flags` flagged for re-enqueue.  Handler change in Slice 4.
- **`PROXY_CANCEL` (op 95)** -- existing PS-initiated cancel.
  Re-used as-is by the PS when it cannot make progress (e.g.,
  encoding-decoder error).  Admin-initiated cancel (from the
  MDS to a PS) is delivered as a `PROXY_OP_CANCEL_PRIOR`
  assignment in the *next* `PROXY_PROGRESS` reply -- already
  wired through the assignment queue path.  Slice 5 adds the
  admin trigger.

### 4.2 New probe ops

```xdr
struct INODE_REPAIR_FORCE1args {
    unsigned hyper        irf_sb_id;
    unsigned hyper        irf_ino;
    unsigned int          irf_mirror_index;
};
struct INODE_REPAIR_FORCE1res {
    probe_stat1           irf_status;
    /* if status OK: the proxy_stateid.other[12] of the
     * just-minted migration record (or all-zero if the queue
     * is full and the item was dropped). */
    opaque                irf_repair_id[12];
};

struct INODE_REPAIR_STATUS1args {
    /* optional filter; if irs_sb_id == 0, list all sbs */
    unsigned hyper        irs_sb_id;
};
struct INODE_REPAIR_STATUS1record {
    unsigned hyper        irsr_sb_id;
    unsigned hyper        irsr_ino;
    unsigned int          irsr_mirror_index;
    opaque                irsr_repair_id[12];   /* zero if NEEDS-REPAIR */
    unsigned int          irsr_state;           /* NEEDS / IN_REPAIR */
    unsigned hyper        irsr_dispatched_to_ns;/* monotonic ns; 0 if NEEDS */
};
struct INODE_REPAIR_STATUS1res {
    probe_stat1           irs_status;
    INODE_REPAIR_STATUS1record irs_records<>;
};

struct INODE_REPAIR_CANCEL1args {
    opaque                irc_repair_id[12];
};
struct INODE_REPAIR_CANCEL1res {
    probe_stat1           irc_status;
};
```

Field prefixes (`irf_`/`irs_`/`irc_`) match the three-letter
convention in the existing probe op vocabulary
(`probe1_xdr.x:ila_/ilr_`).

CLI surface mirrors:

```
reffs-probe.py inode-repair-force --sb 5 --ino 42 --mirror 0
reffs-probe.py inode-repair-status [--sb 5]
reffs-probe.py inode-repair-cancel --repair-id <hex>
```

## 5. Producer + dispatcher

### 5.1 LAYOUTERROR producer (Slice 1)

In `nfs4_op_layouterror` (`layout.c:2379`), walk `lea_errors`,
match each on `(seg, mirror)`, qualify on error code, and:

1. Under `i_attr_mutex`: if `ldf_flags & FFV2_DS_FLAGS_REPAIR`
   is already set OR `ldf_repair_pid` is non-zero, skip (idempotent).
2. Set the flag: `ldf->ldf_flags |= FFV2_DS_FLAGS_REPAIR`.
3. Call `inode_sync_to_disk`.
4. Drop `i_attr_mutex`.
5. If `cfg.proxy_server.autopilot_on_layouterror`:
   `proxy_assignment_queue_push(&item)` with
   `paq_kind = PROXY_OP_REPAIR`, `paq_sb_id`, `paq_ino`,
   `paq_source_dstore_id` (the broken mirror's dstore),
   `paq_target_dstore_id` (the runway picker's choice).
6. Bump `sb_chunk_stats.cs_repair_needed`.

Qualifying error codes: `NFS4ERR_IO`, `NFS4ERR_BADIOMODE`,
`NFS4ERR_ACCESS`, `NFS4ERR_PERM`.  Non-qualifying codes
(`NFS4ERR_NOSPC`, `NFS4ERR_DELAY`, etc.) get logged but do
not flag.

Step 5 is **outside** `i_attr_mutex` to avoid lock-order
inversions with the queue's mutex.  The queue push is
synchronous but does not block on inode locks; the only
visible coupling is the producer-counter bump.

### 5.2 `INODE_REPAIR_FORCE` producer (Slice 2)

Identical path to 5.1 step 1-6 minus the qualifying-code check
(admin always wins).  Always enqueues regardless of the
autopilot config bit.  Returns the `irf_repair_id` after a
synchronous round-trip with the queue:

- If the queue is full (`-ENOSPC`): return `PROBE1_OK` with
  `irf_repair_id == zeros` -- the flag is set, the queue is
  saturated; admin can retry the FORCE.
- If `ldf_flags` was already flagged AND `ldf_repair_pid` is
  non-zero (already in-flight): return `PROBE1_OK` with the
  existing `repair_id` (idempotent, lets the caller poll
  STATUS for either case).
- Otherwise: enqueue + return the *expected* repair_id derived
  from the queue item's identity (the actual `proxy_stateid4`
  isn't minted until a PS polls -- the FORCE path either
  speculatively mints one and stamps `ldf_repair_pid` early,
  or returns zeros and the caller polls STATUS until it sees
  one).  Initial implementation: stamps early (simpler caller
  contract).

### 5.3 PS selection

Already exists.  The `nfs4_op_proxy_progress` handler pops
items from a single global queue; whichever registered PS polls
first gets them.  Round-robin emerges from PS polling cadence
without any MDS-side scheduler.  Homogeneous-fleet assumption
holds.

A registered-PS lease lapse (`nc_ps_lease_expire_ns` falls
below `reffs_now_ns()`) is handled by the existing lease
reaper, which retires the migration record and reverts the
mirror to `NEEDS-REPAIR`.

## 6. Slices

Four slices, all reviewer-gated (XDR + cross-layer + on-disk
triggers per the gating rules in CLAUDE.md).  Each slice ships
its own libcheck file (TDD discipline -- failing tests in the
same commit as the production code).

### Slice 1: MDS sets `FFV2_DS_FLAGS_REPAIR` on LAYOUTERROR

**Test impact on existing tests** (per planner W2 + reviewer
N7 + N3):

| Existing test | Impact |
|---------------|--------|
| `lib/nfs4/tests/layout_error_test.c` (if extant) | UPDATE -- existing LAYOUTERROR tests still pass; add coverage for the new flag-set side effect |
| `lib/nfs4/tests/chunk_repair_test.c` | PASS -- no change to chunk-store paths; the new flag-set path is parallel |
| `lib/nfs4/tests/nfs4_op_layout_*` | PASS -- LAYOUTGET still emits `ldf->ldf_flags` verbatim per `layout.c:824` |

**New tests** (`lib/nfs4/tests/layouterror_repair_flag_test.c`):

| Test | Intent |
|------|--------|
| `test_layouterror_io_flags_mirror` | LAYOUTERROR with `NFS4ERR_IO` on a specific deviceid -> matching mirror's `ldf_flags` gains `FFV2_DS_FLAGS_REPAIR` |
| `test_layouterror_badiomode_flags_mirror` | Same for `NFS4ERR_BADIOMODE` |
| `test_layouterror_access_perm_flags_mirror` | Same for `NFS4ERR_ACCESS` / `NFS4ERR_PERM` |
| `test_layouterror_unknown_device_noop` | LAYOUTERROR for a deviceid not in any segment -> NFS4_OK, no flag change |
| `test_layouterror_already_flagged_idempotent` | Mirror already flagged -> NFS4_OK, no double set, no second enqueue |
| `test_layouterror_nonqualifying_codes_noop` | LAYOUTERROR with `NFS4ERR_NOSPC` -> NFS4_OK, no flag change |
| `test_layouterror_persists` | Flag set + `inode_sync_to_disk` + reload -> flag still set |
| `test_layouterror_autopilot_off_no_enqueue` | Autopilot bit off -> flag set, queue stays empty |
| `test_layouterror_autopilot_on_enqueues` | Autopilot bit on -> queue gains one REPAIR item with the right (sb_id, ino, source_dstore) |

**Code**:

- `lib/nfs4/server/layout.c::nfs4_op_layouterror`: producer
  per §5.1.
- `lib/include/reffs/settings.h`: `[proxy_server]
  autopilot_on_layouterror` bool, default false.
- `lib/config/config.c`: parse the bit.
- `lib/include/reffs/nfs4_stats.h`: `cs_repair_needed` counter.
- `lib/nfs4/include/nfs4/layout.h`: add `ldf_repair_pid[12]`
  to `struct layout_data_file`.

### Slice 2: probe ops

**Test impact**: none on existing tests (additive).

**New tests** (`lib/probe1/tests/probe_repair_test.c`):

| Test | Intent |
|------|--------|
| `test_repair_force_flags_and_enqueues` | FORCE -> `ldf_flags` set + queue gets one item + non-zero repair_id returned |
| `test_repair_force_idempotent` | Second FORCE on same (sb, ino, mirror) -> returns the same repair_id, no second enqueue |
| `test_repair_status_lists_in_flight` | One FORCE -> STATUS returns one record in IN_REPAIR or NEEDS state with the right fields |
| `test_repair_status_filter_by_sb` | Two FORCEs on different sbs -> STATUS with sb filter returns one |
| `test_repair_cancel_unknown_id_ok` | CANCEL with unknown id -> PROBE1_OK (idempotent) |
| `test_repair_cancel_in_flight_enqueues_cancel_prior` | CANCEL with valid id -> next PROXY_PROGRESS pop yields a `PROXY_OP_CANCEL_PRIOR` for that id |

**Code**:

- `lib/xdr/probe1_xdr.x`: 3 ops + 1 record type per §4.2.
- `lib/probe1/probe1_server.c`: handlers; share the producer
  helper from Slice 1.
- `lib/probe1/probe1_client.c`: C client wrappers.
- `lib/include/reffs/probe1.h`: declarations.
- `scripts/reffs/probe_client.py.in`: Python methods.
- `scripts/reffs-probe.py.in`: CLI subparsers + formatters.

### Slice 3: PS-side assignment handler (REPAIR kind)

**Test impact**:

- `lib/nfs4/ps/tests/proxy_progress_client_test.c` (or
  equivalent if extant): UPDATE if a PS-side `PROXY_PROGRESS`
  client exists; otherwise add it here.

**New tests** (`lib/nfs4/ps/tests/proxy_repair_handler_test.c`):

| Test | Intent |
|------|--------|
| `test_handler_decodes_repair_assignment` | Synthetic `PROXY_PROGRESS` reply with one REPAIR assignment -> handler queues it, returns to poll loop |
| `test_handler_drives_ec_repair_codec` | Mocked `ec_repair_codec` -> assertion that it is called with the assignment's source/target deviceids |
| `test_handler_sends_chunk_repaired_then_done_on_success` | Successful repair -> first OP_CHUNK_REPAIRED, then PROXY_DONE(NFS4_OK), in that order |
| `test_handler_sends_only_done_on_failure` | Mocked `ec_repair_codec` failure -> no OP_CHUNK_REPAIRED, PROXY_DONE(<errno>) only |
| `test_handler_cancel_flag_stops_mid_repair` | Set cancel atomic mid-repair -> worker exits at next stripe boundary, sends PROXY_DONE(NFS4ERR_DELAY) |
| `test_handler_lease_expiry_silent` | Mocked lease lapse -> worker stops without sending PROXY_DONE (MDS-side reaper retires) |

**Code**:

- `lib/nfs4/ps/proxy_repair_handler.c` (NEW): per-PS worker
  pool (single thread for the demo, configurable later);
  in-memory per-assignment state structure with `_Atomic`
  cancel flag.
- `lib/nfs4/ps/proxy_poll_client.c` (NEW if not extant):
  the PS-side `PROXY_PROGRESS` poll loop that decodes
  assignments and dispatches by `pa_kind`.
- Re-uses `lib/nfs4/ps/ec_pipeline.c::ec_repair_codec()`
  (shipped 2026-06-10).
- New PS-side stats: `ps_repair_attempted`,
  `ps_repair_succeeded`, `ps_repair_failed`,
  `ps_repair_cancelled`.

### Slice 4: MDS-side `PROXY_DONE` and `PROXY_CANCEL` consequences for REPAIR

**Test impact**:

- `lib/nfs4/server/tests/proxy_done_test.c` (or equivalent if
  extant): UPDATE -- existing MOVE consequences stay; add
  REPAIR consequences.

**New tests** (`lib/nfs4/tests/proxy_done_repair_test.c`):

| Test | Intent |
|------|--------|
| `test_done_ok_clears_pid` | PROXY_DONE(NFS4_OK) for a REPAIR migration -> `ldf_repair_pid` zeroed; record retired |
| `test_done_failure_leaves_flag_set` | PROXY_DONE(<errno>) -> record retired; `ldf_flags` keeps the REPAIR bit; ready for re-enqueue |
| `test_cancel_clears_pid_keeps_flag` | PROXY_CANCEL -> same as failed-DONE |
| `test_done_wrong_owner_perm` | PS A tries PROXY_DONE on a stateid owned by PS B -> NFS4ERR_PERM |
| `test_done_stale_stateid_after_reboot` | Synthesize a PROXY_DONE for a pre-reboot stateid -> NFS4ERR_STALE_STATEID (already handled by `proxy_stateid_alloc` boot_seq embed; this is regression coverage) |

**Code**:

- `lib/nfs4/server/proxy_done.c`: REPAIR-kind branch on the
  retire path; clear `ldf_repair_pid` on the target mirror.
- `lib/nfs4/server/proxy_cancel.c`: same.

### Slice 5: end-to-end integration test + autopilot opt-in

**Test impact**: none (additive).

**New tests**:

- `lib/nfs4/tests/repair_autopilot_test.c`: with autopilot
  on and a mocked PS that polls, LAYOUTERROR -> queue ->
  PS-poll -> simulated repair -> PROXY_DONE -> flag cleared.
  Pure in-process unit test of the loop.
- `scripts/ci_real_repair_test.sh` (NEW): real-network
  integration test on a multi-host topology (mage as MDS,
  garbo + adept as DS+PS combined, dreamer as client).
  Steps:
  1. Bring up the stack with `autopilot_on_layouterror = true`.
  2. Write a file via ec_demo (4+2 RS, 1 MB).
  3. Use a `tc` rule on adept to drop one DS's traffic.
  4. Read the file from dreamer (degraded read succeeds; the
     client LAYOUTERROR fires).
  5. Restore traffic.
  6. Poll `reffs-probe.py inode-repair-status` until the
     repair_id transitions out (cleared `ldf_repair_pid`).
  7. Verify the previously-degraded mirror now holds the
     reconstructed shard bytes (compare to a healthy mirror's
     shard set).
  8. Verify the file still reads correctly with the formerly-
     missing DS restored to the read pool.

**Code**: none new beyond the test + config glue.

## 7. Reviewer checklist

Per the gating rules in CLAUDE.md, every slice in this phase
triggers the reviewer agent because each crosses an XDR,
on-disk, or cross-layer line.  Reviewer should focus on:

- **Slice 1**: lock ordering for `i_attr_mutex` vs the queue
  mutex (§5.1 step 5 spelled out).  RCU discipline on the
  inode layout segment array (no change vs today, but the
  reviewer should confirm `ldf->ldf_repair_pid` writes don't
  introduce a TOCTOU vs LAYOUTGET reads -- reads are
  informational only).
- **Slice 2**: probe XDR field-prefix consistency with
  existing ops (`irf_`/`irs_`/`irc_` match `ila_`/`ilr_`
  convention).  No version-bump on `probe1_xdr.x` required
  (additive ops only).
- **Slice 3**: PS-side worker thread C11 atomic ordering on
  the cancel flag (acquire/release per `.claude/standards.md`).
  Re-entrancy of `ec_repair_codec()` (per `proxy-server.md`
  Action Item 1, the deeper `lib/nfs4/client/` refactor was
  declared a "do before declaring PS demo done" item; this
  slice is a forcing function -- the reviewer should call
  out anything in the call chain that assumes single-caller).
- **Slice 4**: `proxy_stateid4` ownership check
  (`record.owner_reg` vs session's registered-PS identity)
  per `proxy-server-phase6c-revision.md`.  Confirm
  `NFS4ERR_PERM` returned on owner mismatch, not a false
  cleared state.
- **Slice 5**: integration test reliability under the
  documented `MEMORY.md` "single-host bench has DS-reachability
  gap" -- this is why Slice 5's integration runs on a
  multi-host topology, not docker-compose; the reviewer
  should confirm the test does not regress to in-tree
  docker-compose.

## 8. Open questions

1. **Speculative `proxy_stateid4` mint at `INODE_REPAIR_FORCE`
   time vs. on PS poll?**  §5.2 settles on early-mint for
   simpler caller contract.  Alternative: delay mint until pop,
   return zeros from FORCE, force the admin to poll STATUS.
   Sticking with early-mint unless a concrete bug surfaces.
2. **Dedup key in the queue**: `(sb_id, ino, mirror_index)`
   today.  If we ever issue REPAIR over a byte-range subset
   of an inode (rather than whole-file), the key needs to
   include the range.  Out of scope for Phase 8; flag if
   Phase 11 (DS CRC-fail bubble) lands.
3. **Should `PROXY_OP_CANCEL_PRIOR` for a still-queued (not
   yet popped) item be served by removing the item from the
   queue rather than waiting for a pop + cancel?**  Cleaner;
   needs `proxy_assignment_queue_remove(predicate)` API.
   Defer until the cancel path's load looks worth it.

## 9. Deferred / NOT_NOW_BROWN_COW

- **Periodic scrub thread** (whole-namespace background sweep
  that issues `INODE_REPAIR_FORCE` for everything with stale
  CRC).  Phase 10.
- **DS-side CRC-fail upstream reporter** -- back-channel from
  the DS to the MDS reporting which inode/block hit a CRC
  fail during normal `CHUNK_READ`.  Phase 11.
- **Lost-DS file rebuild (R3)** -- registry scan path that
  iterates every inode in every sb looking for matching
  `ldf_dstore_id`, enqueues REPAIR for each.  Phase 9b.
- **Multi-PS pipelining** for very large files (stripe-range
  partitioning across PSes).  Phase 9c.
- **Heterogeneous PS fleet** -- encoding-set bitmap in
  `PROXY_REGISTRATION`, slot-based selection that prefers
  least-loaded eligible PS.  Phase 9d.
- **`chunk.c` production-hardening** (the four markers in
  `ec-repair.md` §12a).  Phase 9a; gates the autopilot
  config bit being flipped on in production.

## 10. RFC / draft references

- draft-haynes-nfsv4-flexfiles-v2 (`FFV2_DS_FLAGS_REPAIR`
  layout flag, `OP_CHUNK_WRITE_REPAIR`, `OP_CHUNK_REPAIRED`).
  Cite the current draft revision in commit messages; the
  draft section anchors for these are stable across revisions.
- draft-haynes-nfsv4-flexfiles-v2-proxy-server (PROXY_*
  ops, `proxy_stateid4`, `proxy_assignment4`).  Cite the
  current draft revision; section anchors are unstable per
  the 2026-04-26 revision (see Section N4 of the reviewer
  audit -- update references when the draft re-publishes).
- RFC 8881 S20 (NFSv4 callback infrastructure; not used by
  this design but referenced by `cb.c` for the existing
  `PROXY_DONE` recall consequence).
- RFC 8178 S4.4.3 (`ppa_flags` reservation rule -- already
  enforced by the shipped `nfs4_op_proxy_progress`).
- `.claude/design/ec-repair.md` (Bucket 4 shipped slice).
- `.claude/design/proxy-server.md` (PS Phase 6 architecture;
  Phase 8 referenced as deferred until 2026-06-30).
- `.claude/design/proxy-server-phase6c-revision.md` (authority
  for the per-instance migration model + `proxy_stateid4`
  ownership semantics).
- `.claude/design/trust-stateid.md` (PS uses real layout
  stateids for I/O, validated by DS trust table).

## 11. Key files

| File | Change |
|------|--------|
| `lib/nfs4/server/layout.c` | Slice 1: `nfs4_op_layouterror` producer |
| `lib/nfs4/include/nfs4/layout.h` | Slice 1: `ldf_repair_pid[12]` field |
| `lib/include/reffs/settings.h` | Slice 1: `autopilot_on_layouterror` config bit |
| `lib/include/reffs/nfs4_stats.h` | Slices 1/3/4: new counters |
| `lib/config/config.c` | Slice 1: parse config bit |
| `lib/xdr/probe1_xdr.x` | Slice 2: 3 new ops |
| `lib/probe1/probe1_server.c` | Slice 2: handlers |
| `lib/probe1/probe1_client.c` | Slice 2: C client wrappers |
| `lib/include/reffs/probe1.h` | Slice 2: declarations |
| `scripts/reffs/probe_client.py.in` | Slice 2: Python methods |
| `scripts/reffs-probe.py.in` | Slice 2: CLI subparsers |
| `lib/nfs4/ps/proxy_repair_handler.c` | Slice 3 NEW |
| `lib/nfs4/ps/proxy_poll_client.c` | Slice 3 NEW (if not extant) |
| `lib/nfs4/server/proxy_done.c` | Slice 4: REPAIR retire branch |
| `lib/nfs4/server/proxy_cancel.c` | Slice 4: REPAIR retire branch |
| `scripts/ci_real_repair_test.sh` | Slice 5 NEW |
| `lib/nfs4/tests/layouterror_repair_flag_test.c` | Slice 1 NEW |
| `lib/probe1/tests/probe_repair_test.c` | Slice 2 NEW |
| `lib/nfs4/ps/tests/proxy_repair_handler_test.c` | Slice 3 NEW |
| `lib/nfs4/tests/proxy_done_repair_test.c` | Slice 4 NEW |
| `lib/nfs4/tests/repair_autopilot_test.c` | Slice 5 NEW |

## 12. Effort summary

| Slice | Description | Size | Reviewer pass | Risk |
|-------|-------------|------|---------------|------|
| 1 | LAYOUTERROR producer + flag + config bit | S | yes (XDR-adjacent + new field on layout struct) | low |
| 2 | Probe ops + admin CLI | M | yes (new XDR; probe is internal but still reviewer-gated) | low |
| 3 | PS-side handler + worker | M | yes (cross-layer, re-entrancy in `ec_pipeline.c`) | medium (forces the `lib/nfs4/client/` re-entrancy audit that `proxy-server.md` Action Item 1 flagged) |
| 4 | MDS-side PROXY_DONE / CANCEL retire branches | S | yes (touches ownership-check path) | low |
| 5 | Integration test + autopilot opt-in | S | inline (no new code; test + config) | low |

Total: about four focused days of code + one day of
multi-host integration bringup.  Five reviewer-agent passes
(every slice qualifies under the gating rules).  Honest
estimate for "code complete to merged-to-main": one work
week, give or take.

## 13. Acceptance

This phase is "real repair" when:

1. `INODE_REPAIR_FORCE` triggers a complete repair cycle
   end-to-end with no operator intervention beyond the
   one probe call.
2. The PS reconstructs the mirror and the file reads
   correctly afterward.
3. `reffs-probe.py inode-repair-status` lets the operator
   watch the repair complete.
4. With `autopilot_on_layouterror = true` and the §9a
   production-hardening markers closed, a LAYOUTERROR from
   a regular client kicks off the same cycle without any
   operator action -- the integration test in Slice 5
   exercises this path with autopilot on.
5. The compute-node client does nothing except encounter
   the original error and report it via LAYOUTERROR.
