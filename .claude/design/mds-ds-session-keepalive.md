<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# MDS-to-DS NFSv4.2 Session Keep-Alive

## Revision history

- **2026-05-26 (initial)**: drafted alongside the buffer-state
  fold-in (committed as `b49f83ddd1a0`).
- **2026-05-27 (revision 1)**: design-stage reviewer pass returned
  2 BLOCKERs + 5 WARNINGs + 9 NOTEs.  Key changes:
  - **B1 fix**: `struct dstore` has no rwlock today (`ds_v4_session`
    is a bare pointer accessed by ~16 sites in
    `dstore_ops_nfsv4.c`).  The PS accessor pattern relies on
    `pls_session_rwlock`.  Slice scope now explicitly includes:
    add `ds_v4_session_rwlock` to `struct dstore` + convert every
    existing `ds->ds_v4_session` dereference in
    `dstore_ops_nfsv4.c` and `ds_session.c` to go through the new
    `dstore_session_borrow/_release` accessor pair.  This is a
    pre-existing latent UAF that the renewal thread would convert
    to active.
  - **B2 fix**: documented that `dstore_session_replace` MUST
    destroy the old session AFTER releasing the wlock, matching
    `ps_listener_session_replace` (rationale: DESTROY_SESSION can
    take RPC-timeout seconds to complete; holding wlock during
    destroy blocks every new borrower).
  - **W2 fix**: `ds_renewal_kick` now explicitly wired into
    `send_and_check_ds` after BADSESSION so worker-observed
    session death gets immediate reconnect rather than waiting
    one full renewal interval.
  - **W3, W4, W5, N1-N9**: incorporated inline; ds_renewal
    uses `s_renewal_running` shutdown check, reuses
    `dstore_collect_all`, drops the unused `max_sec` knob,
    documents `[mds]` as a new TOML section, hardens the
    transient test to assert backoff state unchanged.

## Context

The MDS opens an NFSv4.2 session to each DS at startup (per
`design/dstore-vtable-v2.md`) and uses it for control-plane
operations: runway file CREATE / REMOVE, GETATTR for reflected
attrs, SETATTR for truncate / chmod / fence, TRUST_STATEID issue
(per `design/trust-stateid.md`), etc.

The session today sends SEQUENCE **only** when there is a
control-plane op to issue.  An MDS with no active write layouts
(no LAYOUTRETURN, no truncate, no fence, no reflected GETATTR)
leaves the session idle.  After 600 seconds of socket inactivity
the underlying TCP connection is reaped by
`io_conn_check_timeouts` (per the
`CONNECTION_TIMEOUT_SECONDS` constant in
`lib/include/reffs/io.h:156`); the next control op then has to
reconnect, sometimes against a DS that has GC'd the session and
returns `NFS4ERR_BADSESSION`.

The Track 2 chunk-collision bench on 2026-05-26 surfaced this
gap as the *trigger* for the buffer-state fd-recycle race
(closed by commit `cbe340ca1666` and documented in
`design/io-buffer-state-fd-recycle.md`).  The bench took ~20 wall
minutes to start writing because of fio's first-run `dnf install`
plus 8 PS containers' setup, during which the MDS-to-DS sessions
sat idle.  At T+10min the 600s reaper fired on every DS; the
subsequent burst of CREATE / WRITE / FINALIZE activity drove a
flood of accepts on recycled fds, exposing the
`io_buffer_state` lifecycle race.

The buffer-state slice closed the race; this slice closes the
*trigger*, so future benches (and any production deployment with
long idle gaps) do not have to depend on the buffer_state fix
catching the storm.

## Tests first

Per `.claude/roles.md` planner discipline: tests up front, then
implementation.

### Unit tests

**File**: `lib/nfs4/dstore/tests/ds_renewal_test.c` (NEW)

| Test | Intent |
|------|--------|
| `test_renewal_thread_lifecycle` | `ds_renewal_start(N)` spawns the keepalive thread; `ds_renewal_stop()` joins it cleanly under ASAN.  No leaked pthread, no UAF on the per-dstore state. |
| `test_renewal_tick_one_sends_sequence` | Single-dstore tick: `renewal_tick_one(ds)` calls `mds_session_renew_lease_ex` on the dstore's session, increments the `renewed` counter, returns 0. |
| `test_renewal_tick_skips_dead_session` | `dstore` with `ds_v4_session == NULL` (e.g. boot-time mds_session_create_tls failed): tick records `skipped_no_session`, attempts reconnect honouring the backoff deadline. |
| `test_renewal_tick_handles_transient` | Tick where `mds_session_renew_lease_ex` returns a transient error (`NFS4ERR_DELAY` or `-EAGAIN` on the wire): increment `failed`, log once, leave session alive.  Assert `ds_reconnect_backoff_sec == 0` after the transient (transient must NOT arm backoff -- only session-killer responses do). |
| `test_renewal_tick_kills_dead_session` | Tick where the server returns a session-killer (`NFS4ERR_BADSESSION` / `NFS4ERR_STALE_CLIENTID`): clear `ds_v4_session`, mark for reconnect, set initial backoff. |
| `test_renewal_reconnect_backoff` | Three consecutive failed reconnect attempts: backoff doubles (1s -> 2s -> 4s, clamped at the configured max).  Use `ps_reconnect_backoff_next` template from `lib/nfs4/ps/ps_renewal.c`. |
| `test_renewal_interval_clamp` | `ds_renewal_start(0)` returns -EINVAL.  `ds_renewal_start(1)` accepted; very large value (>= 86400) refused. |
| `test_renewal_kick_wakes_thread` | While the thread is sleeping on its cv between ticks, `ds_renewal_kick()` wakes it so a fresh tick runs immediately.  Asserts the next tick timestamp advances within 50ms of the kick. |
| `test_session_replace_quiesces_in_flight` | B1 regression test (mirrors the PS test of the same name).  Borrower thread holds `dstore_session_borrow(ds)` for an extended window (simulates an in-flight RPC).  A second thread calls `dstore_session_replace(ds, NULL)` which must block on the wrlock until the borrower releases, then complete cleanly.  Verifies the rwlock serialisation prevents a swap-under-active-borrower UAF.  Without B1's rwlock, this test would race; with it, the replace blocks for the borrow duration then succeeds. |

### Test impact analysis

| File | Impact | Reason |
|------|--------|--------|
| `lib/nfs4/dstore/tests/ds_session_test.c` | PASS — no change | Session create/destroy lifecycle unchanged. |
| `lib/nfs4/dstore/tests/dstore_*` | PASS — no change | Control-plane ops still send their own SEQUENCE inline; the keep-alive is additive. |
| `lib/nfs4/ps/tests/ps_renewal_test.c` | PASS — no change | PS renewal is a sibling, not a parent.  Both threads exist; both operate on distinct session pools. |
| `lib/utils/tests/grace_test.c` | PASS — no change | Server-side lease lifecycle, not MDS-as-client. |
| `lib/io/tests/conn_info_test.c` (32 tests after `cbe340ca1666`) | PASS — no change | This slice does not touch lib/io/. |

No existing test is modified by this slice.

### Functional test

Re-use the chunk-collision Track 2 fio harness
(`deploy/benchmark/run_chunk_collision_track2.sh --n 8`).  After
this slice lands, the bench's setup phase should no longer trigger
a 600-second idle close on any MDS-to-DS connection:

- Grep all DS container logs for `"Connection fd=.. timed out
  (601 seconds inactive)"`.  Expected count: **0**.
- Sanity-check the bench still completes Criterion 1 (fio verify
  clean), Criterion 3 (sanitizers clean), Criterion 4 (no
  force-drain).  The buffer-state fold-in already closes
  Criterion 4 structurally; this slice removes the *route* by
  which the 600s timer fires in the first place.

### CI integration

The MDS-to-DS keep-alive is best validated by **soak**: long-idle
sessions are the failure mode.  Extend `scripts/ci_soak_test.sh`
(per stable-bat.md WI-1.6) with a "quiet" period where the MDS
has no active layouts for at least 600 seconds; assert no
`BADSESSION` / `BADSLOT` / `BADSEQUENCE` errors fire on the next
control op.  No new CI script needed — soak coverage extension.

## Implementation

### Architecture

Direct mirror of `lib/nfs4/ps/ps_renewal.c`.  One renewal thread
per `reffsd` process; iterates all dstores; per-dstore tick is
the same shape as `renewal_tick_one(pls)`:

```c
int ds_renewal_tick_one(struct dstore *ds, struct ds_renewal_ctx *ctx)
{
    struct mds_session *ms = dstore_session_borrow(ds);
    if (!ms) {
        ctx->skipped_no_session++;
        /* reconnect path with backoff, same as PS */
        ...
        return 0;
    }

    nfsstat4 sr_status = NFS4_OK;
    int ret = mds_session_renew_lease_ex(ms, &sr_status);
    dstore_session_release(ds);

    if (ret == 0) {
        ctx->renewed++;
        return 0;
    }
    ctx->failed++;
    if (!session_is_dead(ret, sr_status)) {
        /* transient: log once */
        LOG("ds_renewal: dstore_id=%u SEQUENCE renewal failed "
            "(transient): %s sr_status=%u -- session still alive",
            ds->ds_id, strerror(-ret), (unsigned)sr_status);
        return 0;
    }

    /* Session-killer.  Clear ds_v4_session, mark for reconnect. */
    LOG("ds_renewal: dstore_id=%u upstream session is dead "
        "(errno=%s sr_status=%u) -- forcing reconnect",
        ds->ds_id, strerror(-ret), (unsigned)sr_status);
    dstore_session_replace(ds, NULL);
    return 0;
}
```

The PS already provides:
- `ps_listener_session_borrow` / `_release` / `_replace`
  (`lib/nfs4/ps/ps_state.c:507-607`) — **rwlock**-protected
  session accessor (NOT refcount-based; the rwlock is
  `pls_session_rwlock` on `struct ps_listener_state`).
  `borrow` takes `pthread_rwlock_rdlock`; `replace` takes
  `pthread_rwlock_wrlock`, swaps, releases the wlock, then
  destroys the old session **outside** the wlock (a
  DESTROY_SESSION round-trip can take RPC-timeout seconds, and
  holding the wlock during destroy would block every new
  borrower).  Mirror as `dstore_session_borrow` / `_release` /
  `_replace` on `struct dstore`.
- `ps_session_is_dead(ret, sr_status)` — classification helper.
  Promote to `lib/nfs4/client/mds_session.h` so both PS and DS
  consumers share it; rename to `mds_session_is_dead` if the
  function body is identical (verify).
- `ps_reconnect_backoff_next` — exponential backoff scheduler.
  Same promotion.

### BLOCKER B1: pre-existing `ds_v4_session` lock gap

`struct dstore` (`lib/include/reffs/dstore.h`) does NOT have an
analogue of `pls_session_rwlock` today.  `ds_clnt_mutex`
protects the NFSv3 CLIENT handle, not the v4 session.
`ds_v4_session` is a bare pointer with no lock at all.

Today `ds_session_create` and `ds_session_destroy` assign and
free `ds_v4_session` directly
(`lib/nfs4/dstore/ds_session.c:148, 182-183`); every caller in
`dstore_ops_nfsv4.c` reads `ds->ds_v4_session` as a bare
pointer dereference (16+ sites).  `send_and_check_ds` even
calls `ds_session_destroy(ds)` + `ds_session_create(ds)` while
other dstore-vtable threads may simultaneously hold a copy of
the old pointer.  This is a pre-existing latent UAF.

Introducing an asynchronous renewal thread (this slice) would
convert that latent race to active.  Therefore this slice
**must** include:

1. Add `pthread_rwlock_t ds_v4_session_rwlock` field to
   `struct dstore`, mirroring `pls_session_rwlock`.
2. Implement `dstore_session_borrow(ds)` (rdlock + return
   `ds->ds_v4_session`) and `dstore_session_release(ds)`
   (rwlock unlock).
3. Implement `dstore_session_replace(ds, new_ms)` (wrlock +
   swap + unlock + destroy-old-outside-wlock).
4. **Convert every existing `ds->ds_v4_session` dereference**
   in `lib/nfs4/dstore/dstore_ops_nfsv4.c` and
   `lib/nfs4/dstore/ds_session.c` to go through the new
   accessors.  Each call site captures the borrowed pointer
   for the duration of one RPC round-trip, then releases.
5. Add `test_session_replace_quiesces_in_flight` to the
   ds_renewal_test suite — mirroring the PS test of the same
   name — as the regression net for the swap-under-active-
   borrower race.

This step (the call-site sweep) is the largest part of the
slice in LOC terms; it is **not** optional and is the actual
fix for B1.

### State diagram

```
              ds_renewal_start(N)
                     |
                     v
                 RUNNING
                     |
                     | every N seconds (cv-waited):
                     v
              ds_renewal_tick_one(each ds)
                     |
            +--------+------------+
            |        |            |
   session_is_dead   ok        transient
            |        |            |
            v        v            v
   reset session    counters   log + counter
   schedule reconnect  ++       ++; continue
            |
            v
   on next tick: reconnect attempt
   (backoff-gated, same exponential
    schedule as ps_renewal)
            |
            +-> session resurrected -> RUNNING
            +-> still dead -> backoff doubles
```

### State machine transitions

| From | To | Trigger | Action |
|------|----|---------|--------|
| RUNNING (session alive) | RUNNING | tick + renew OK | `renewed++`, sleep until next interval |
| RUNNING | DEAD (session cleared) | tick + session_is_dead | Clear `ds_v4_session`, set backoff=1s, set `reconnect_next=now+1s` |
| DEAD | DEAD (still dead) | tick + reconnect_failed | `backoff *= 2`, clamp; `reconnect_next = now + backoff` |
| DEAD | RUNNING | tick + reconnect_succeeded | New `ds_v4_session` installed via `dstore_session_replace`; backoff cleared |
| ANY | STOPPED | `ds_renewal_stop()` | `pthread_join` the renewal thread |

### Interval choice

PS uses `server_lease_time(ss) / 3`, with a **floor** of 30s
when `lease_time / 3` would be 0 (per reffsd.c:1139-1142 —
correction to revision 0 which mis-stated this as a "cap").
The MDS-as-DS-client should use the **same** value because the
failure mode is identical: a SEQUENCE must arrive at the DS
before `2 * NFS4_LEASE_TIME + epsilon` elapses or the DS
expires the session.  `lease_time / 3` guarantees at least 2
successful SEQUENCEs per lease period.

The 600s socket idle-timeout is **independent** of session lease
expiry — it's a TCP-layer reap.  With `lease_time = 90` (the
reffs default) and `interval = 30`, the socket sees a SEQUENCE
every 30s, well under the 600s reaper threshold.  Both timers
are satisfied with one interval.

Configurable knob (TOML):
```toml
[mds]
ds_session_renewal_interval_sec = 30  # default; 0 = disabled
```

The disabled-mode (`= 0`) is for tests and for the
"no remote DSes" pure-local-VFS combined-mode case.  Default-on
otherwise.

**Note**: `[mds]` is a **new** top-level TOML table.  No
existing `[mds]` table; the closest is `[[proxy_mds]]`
(array-of-tables) and `[server]` (with `role = "mds"`).  Adding
`[mds]` is a one-table parser hook in `lib/config/config.c`,
not "one more field in an existing table".  Earlier draft
included a `ds_session_renewal_max_sec` knob -- dropped from
this revision because it was not wired into the implementation
order; if a true ceiling becomes useful later, add it as a
follow-up.

### EXCHGID4 flag verification

The MDS-to-DS session uses `EXCHGID4_FLAG_USE_NON_PNFS`
(per `lib/nfs4/dstore/ds_session.c:68-78`, set by the
dstore-vtable-v2 slice).  The PS-to-MDS session uses the same
flag.  Lease-renewal SEQUENCE semantics on the DS are
identical to those on the MDS — no divergent semantics that
would invalidate mirroring `ps_session_is_dead` /
`ps_reconnect_backoff_next` to a shared helper.

### Concurrency

`ds_renewal` runs on its own pthread, same as `ps_renewal`.

**Session-pointer protection** (correction to revision 0):
`dstore_session_borrow` takes a per-dstore `pthread_rwlock_rdlock`
on `ds_v4_session_rwlock` and returns the current
`ds->ds_v4_session`.  `dstore_session_release` calls
`pthread_rwlock_unlock`.  The session pointer is valid for the
duration of the lock-held window only — callers must NOT cache
it beyond `dstore_session_release`.  This is the same
rwlock-protected-pointer discipline as `ps_listener_session_borrow`.

`dstore_session_replace(ds, new_ms)` takes
`pthread_rwlock_wrlock`, swaps `ds_v4_session` to `new_ms`,
releases the wlock, then **destroys the old session OUTSIDE
the wlock**.  Mandatory: DESTROY_SESSION + DESTROY_CLIENTID
round-trip can take RPC-timeout-many seconds; holding the
wlock during destroy blocks every new borrower (the renewal
thread, every dstore-vtable thread issuing CREATE / REMOVE /
GETATTR / SETATTR / TRUST_STATEID).  Mirrors
`ps_listener_session_replace` rationale at
`lib/nfs4/ps/ps_state.c:589-605`.

**Worker-driven `ds_renewal_kick`** (W2 fix): when
`send_and_check_ds` observes a session-killer error
(`mds_session_is_dead` returns true) on its own RPC, it calls
`ds_renewal_kick()` to wake the renewal thread immediately
rather than waiting one full renewal interval (~30s) for the
periodic tick to notice.  Without this, the recovery latency
after a BADSESSION storm is one interval per affected dstore —
exactly the failure mode this slice exists to mitigate.

**Shutdown-mid-reconnect** (N1 fix): inside the reconnect path
(the equivalent of `ps_listener_reconnect`), check
`atomic_load_explicit(&s_renewal_running, memory_order_acquire)`
at every step that could block (TCP connect, EXCHANGE_ID,
CREATE_SESSION).  Abort cleanly if shutdown arrived
mid-reconnect.  Without this check, `ds_renewal_stop` would
block for the full RPC timeout when the renewal thread is
mid-reconnect to an unreachable DS.

**Dstore registry iteration**: reuse the existing
`dstore_collect_all(dstores_out, max)` helper
(`lib/nfs4/dstore/dstore.c` -- already RCU-protected per
`design/per-export-dstore.md`) rather than re-rolling the
iteration.  Cap at `DSTORE_REVOKE_MAX = 64`
(`lib/include/reffs/dstore.h:32`) — same bound used by other
bulk operations.  The collected refs are dropped via
`dstore_put_ref(ds)` after each tick.

### Why a single thread (not per-dstore)

Per-dstore renewal threads would scale linearly with dstore
count.  In a 10-DS BAT topology that's 10 idle pthreads doing
near-nothing.  Single thread with per-tick fan-out matches the
PS pattern and is sufficient: the work per tick is
`N_dstores * (one round-trip SEQUENCE)`, each round-trip being
~milliseconds on a healthy LAN.  At interval=30s and N=10,
total CPU per tick is well under 100ms; no contention.

If a future deployment has hundreds of dstores per MDS, a thread
pool over `dstores` is the natural escalation.  Out of scope.

## Persistence

None.  Renewal state is fully in-RAM: thread handle, interval,
per-dstore backoff state on `struct dstore` (which is itself
RAM-only; dstore identity persists via `[[data_server]]` config,
not via this slice's state).

The `dstore` struct gains two fields, parallel to
`pls_reconnect_backoff_sec` / `pls_reconnect_next_attempt_ns` on
`ps_listener_state`:

```c
/* Add to struct dstore: */
_Atomic uint32_t ds_reconnect_backoff_sec;
_Atomic uint64_t ds_reconnect_next_attempt_ns;
```

Both `_Atomic`-qualified per `.claude/standards.md` "Atomic
Operations" (new code = C11 stdatomic).  Accessed by the renewal
thread alone; no cross-thread invariant beyond "read and write
under the same accessor".

## Security model

No new attack surface.  The renewal is an additional periodic
SEQUENCE that exercises a code path already triggered by every
control-plane op.  The session was authenticated at
EXCHANGE_ID time; SEQUENCE on that session inherits the same
auth.  No new credential, no new flavor negotiation.

A misbehaving DS that returns `NFS4ERR_DELAY` indefinitely
will pin the renewal thread at "failed transient" log spam
once per tick interval.  The PS already deals with this by
classifying transients vs. session-killers and rate-limiting
the log.  Same logic applies here.

## Deferred items

Genuinely-deferred follow-ups (not load-bearing for this slice):

1. **DS-side detection of MDS reboot via boot_seq epoch change.**
   When the MDS restarts, the renewal thread on the new boot will
   try to renew with the OLD session id; the DS returns
   BADSESSION; the dstore's reconnect path establishes a fresh
   session.  Per `design/trust-stateid.md` deferred list, future
   work auto-pends trust entries on epoch change.  Not in scope
   here.

2. **CB_GETATTR ride-along.**  When renewing, the MDS could
   issue a CB_GETATTR-back equivalent to detect that the DS has
   advanced its mtime / size.  Today the reflected GETATTR
   covers this on demand.  Premature optimisation.

3. **Adaptive interval.**  If a dstore's renewal counter shows
   N successful renewals with no intervening control ops, the
   interval could grow up to a `max_idle_factor * lease_time`.
   Out of scope; the constant interval is simpler and works.

4. **Per-dstore renewal worker pool.**  Single-thread fan-out
   is fine for the BAT-scale topology (10 DSes).  Hundreds of
   dstores would justify a worker pool.  Tracked as NOT_NOW.

## Admin interface

The renewal thread state is observable via the probe protocol's
existing `dstore-list` op (extend the dstore info struct with
`renewal_running`, `last_renewal_ns`, `renewed_count`,
`failed_count`, `reconnect_attempts`, `reconnect_succeeded`).
No new probe op; the existing `dstore-list` already returns a
per-dstore record.

A future `/reffs-probe.py ds-renewal-poke` op could call
`ds_renewal_kick()` for manual on-demand renewal, mirroring
`ps_renewal_kick()`.  Out of scope for this slice; the periodic
tick is sufficient.

## Implementation order

Single-commit slice (substantially larger than revision 0 due to
the B1 call-site sweep):

1. **TDD: write the 9 unit tests** (8 original + 1 B1
   regression) referencing the new API.  The tests will not
   compile against the pre-fix tree (no `ds_renewal_start`, no
   `ds_renewal_tick_one`, no `dstore_session_borrow`).  Land
   them in the same commit as the implementation.

2. **Promote `ps_session_is_dead` and `ps_reconnect_backoff_next`
   to lib/nfs4/client/mds_session.{c,h}.**  Rename to
   `mds_session_is_dead` and `mds_reconnect_backoff_next`.
   Update the one PS call site.  Pure rename + move, no
   behaviour change.  Precondition for shared helpers.

3. **Add `ds_v4_session_rwlock`** (`pthread_rwlock_t`) to
   `struct dstore` -- the B1 fix.  Initialise in
   `dstore_alloc`, destroy in `dstore_free`.

4. **Implement `dstore_session_borrow / _release / _replace`
   accessors** mirroring `ps_listener_session_*`.  Replace
   acquires wrlock + swaps + releases wrlock + destroys old
   session **outside** the wrlock.

5. **Call-site conversion sweep** (B1 scope expansion):
   convert every `ds->ds_v4_session` dereference in
   `lib/nfs4/dstore/dstore_ops_nfsv4.c` and
   `lib/nfs4/dstore/ds_session.c` to go through the new
   accessors.  Each RPC site does
   `ms = dstore_session_borrow(ds); ...rpc...;
   dstore_session_release(ds);`.  ~16 sites in
   `dstore_ops_nfsv4.c` (use grep to enumerate before editing).

6. **Add atomic backoff fields**
   (`_Atomic uint32_t ds_reconnect_backoff_sec`,
   `_Atomic uint64_t ds_reconnect_next_attempt_ns`) to
   `struct dstore`.  Zero-init in `dstore_alloc`.

7. **Write `lib/nfs4/dstore/ds_renewal.c`** mirroring
   `lib/nfs4/ps/ps_renewal.c` line-by-line:
   - `ds_renewal_thread_fn` loops with `pthread_cond_timedwait`
     on `s_renewal_cv`, ticks all dstores via
     `dstore_collect_all` + per-dstore `ds_renewal_tick_one`.
   - `ds_renewal_tick_one` calls
     `dstore_session_borrow` + `mds_session_renew_lease_ex` +
     `dstore_session_release`; classifies via
     `mds_session_is_dead`; on session-killer calls
     `dstore_session_replace(ds, NULL)` + arms backoff.
   - `ds_renewal_kick` signals `s_renewal_cv`.
   - Reconnect path checks `s_renewal_running` between each
     blocking step (TCP connect, EXCHANGE_ID, CREATE_SESSION)
     so shutdown is bounded.

8. **Wire `ds_renewal_kick` into `send_and_check_ds`** in
   `dstore_ops_nfsv4.c` (W2 fix): when an RPC returns a
   session-killer status, call `ds_renewal_kick()` so recovery
   latency is sub-second rather than one full renewal interval.

9. **Wire `ds_renewal_start` into `reffsd.c`** alongside the
   existing `ps_renewal_start` call.  Read interval from
   TOML config (`mds.ds_session_renewal_interval_sec`).
   Default: `server_lease_time(ss) / 3` with **floor** of 30s
   when `lease_time / 3 == 0` (matches PS).

10. **Wire `ds_renewal_stop` into `reffsd_shutdown`** before
    the dstore registry teardown.

11. **Add TOML knob.**  Add `[mds]` table (new -- first
    `[mds]` field in the parser); parse
    `ds_session_renewal_interval_sec` (default `lease_time / 3`,
    floor 30; value `0` disables).

12. **Run `make check`.**  All 32 conn_info tests + the 9 new
    ds_renewal tests pass.  Pre-existing flake
    `ps_reconnect_test::test_session_replace_quiesces_in_flight`
    continues to be unrelated.

13. **Functional gate**: re-run Track 2 N=8 fio on garbo, grep
    for `"601 seconds inactive"` in any DS log.  Expected count
    after the fix: **0**.  Criterion 1 (fio verify) should
    PASS.  Criterion 4 (force-drain) should remain PASS from
    the buffer-state slice.

14. **Reviewer gate.**  Run the reviewer agent on the code
    diff before commit.  This is a ref-counting / lifecycle
    change touching `lib/nfs4/dstore/` -- standards.md gating
    criterion applies.

## Reviewer rules

| Rule | Applies? | Note |
|------|----------|------|
| Rule 6 (UUID stability) | No | No external-facing object; session ids are server-assigned and ephemeral. |
| Rule 8 (on-disk versioning) | No | No persistence. |
| Rule 9 (XDR file review) | No | No protocol XDR change. |
| Rule 10 (UUID-in-probe) | No | No new probe op. |

The CLAUDE.md "ref-counting lifecycle" trigger applies:
`dstore_session_borrow` / `_release` is a refcount pair.  The
slice must follow Rule 6 in `.claude/patterns/ref-counting.md`.

## RFC references

- **RFC 8881 §8.4.2**: session lease semantics.  SEQUENCE renews
  the client's lease at the server.  Interval must guarantee at
  least one SEQUENCE per lease period; `lease_time / 3` is the
  standard NFSv4 client practice.
- **RFC 8881 §18.46**: SEQUENCE op.  Stateless renewal (no
  arguments beyond session id + slot bookkeeping).

## Key files

| File | Change |
|------|--------|
| `lib/nfs4/dstore/ds_renewal.c` | NEW — mirror of ps_renewal.c |
| `lib/nfs4/dstore/ds_renewal.h` | NEW — public API (start/stop/kick) |
| `lib/nfs4/dstore/Makefile.am` | wire new .c into libdstore |
| `lib/include/reffs/dstore.h` | `ds_v4_session_rwlock`, `ds_reconnect_backoff_sec`, `ds_reconnect_next_attempt_ns` fields |
| `lib/nfs4/dstore/dstore.c` | `dstore_session_borrow`/_release/_replace accessors; rwlock init in `dstore_alloc` + destroy in `dstore_free` |
| `lib/nfs4/dstore/dstore_ops_nfsv4.c` | **B1 sweep**: convert ~16 sites from bare `ds->ds_v4_session` deref to `dstore_session_borrow/_release` pair.  Wire `ds_renewal_kick` into `send_and_check_ds` after BADSESSION (W2) |
| `lib/nfs4/dstore/ds_session.c` | Use accessors in `ds_session_create`/`_destroy` rather than bare assign |
| `lib/nfs4/client/mds_session.h` | declare `mds_session_is_dead`, `mds_reconnect_backoff_next` |
| `lib/nfs4/client/mds_session.c` | move bodies from ps_renewal.c |
| `lib/nfs4/ps/ps_renewal.c` | call the promoted helpers; net delete-only |
| `src/reffsd.c` | `ds_renewal_start` / `_stop` at boot/shutdown |
| `lib/config/config.c` | parse `mds.ds_session_renewal_interval_sec` |
| `lib/include/reffs/settings.h` | `struct reffs_mds_config` extension |
| `lib/nfs4/dstore/tests/ds_renewal_test.c` | NEW — 8 tests |
| `lib/nfs4/dstore/tests/Makefile.am` | wire new test |

## References

- `.claude/design/io-buffer-state-fd-recycle.md` — the BLOCKER
  this slice's trigger surfaced.  Co-author: the bench timeline
  in that doc's "Symptom Chain" is the same one this slice
  removes.  **Close-once gate on `io_socket_close` (deferred
  item #5 in that doc)** retains its priority — the keep-alive
  removes the 600s idle close for MDS-to-DS sockets specifically,
  but the same 600s reap path still applies to client-to-MDS,
  NFSv3-client-to-DS, and PS-forwarder-to-MDS sockets.  The
  close-once gate is one socket class lighter to enforce after
  this slice, but its scope (other socket classes) is unchanged.
- `.claude/design/dstore-vtable-v2.md` — defines the MDS-to-DS
  session that this slice keeps alive.
- `.claude/design/trust-stateid.md` — uses the same MDS-to-DS
  session for TRUST_STATEID issuance.  Deferred item 1 cross-
  references this doc's "DS-side MDS reboot detection".
- `lib/nfs4/ps/ps_renewal.c` — the template this slice mirrors.
