<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# MDS-to-DS NFSv4.2 Session Keep-Alive

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
| `test_renewal_tick_handles_transient` | Tick where `mds_session_renew_lease_ex` returns a transient error (`NFS4ERR_DELAY` or `-EAGAIN` on the wire): increment `failed`, log once, leave session alive. |
| `test_renewal_tick_kills_dead_session` | Tick where the server returns a session-killer (`NFS4ERR_BADSESSION` / `NFS4ERR_STALE_CLIENTID`): clear `ds_v4_session`, mark for reconnect, set initial backoff. |
| `test_renewal_reconnect_backoff` | Three consecutive failed reconnect attempts: backoff doubles (1s -> 2s -> 4s, clamped at the configured max).  Use `ps_reconnect_backoff_next` template from `lib/nfs4/ps/ps_renewal.c`. |
| `test_renewal_interval_clamp` | `ds_renewal_start(0)` returns -EINVAL.  `ds_renewal_start(1)` accepted; very large value (>= 86400) refused. |
| `test_renewal_kick_wakes_thread` | While the thread is sleeping on its cv between ticks, `ds_renewal_kick()` wakes it so a fresh tick runs immediately.  Asserts the next tick timestamp advances within 50ms of the kick. |

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
- `ps_listener_session_borrow` / `_release` / `_replace` — ref-counted
  session accessor pattern under listener wlock.  Mirror as
  `dstore_session_borrow` / `_release` / `_replace` on `struct dstore`.
- `ps_session_is_dead(ret, sr_status)` — classification helper.
  Promote to `lib/nfs4/client/mds_session.h` so both PS and DS
  consumers share it; rename to `mds_session_is_dead` if the
  function body is identical (verify).
- `ps_reconnect_backoff_next` — exponential backoff scheduler.
  Same promotion.

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

PS uses `server_lease_time(ss) / 3`, capped at 30 seconds (per
reffsd.c:1139).  The MDS-as-DS-client should use the **same
interval** because the failure mode is identical: a SEQUENCE
must arrive at the DS before `2 * NFS4_LEASE_TIME + epsilon`
elapses, or the DS expires the session.  `lease_time / 3`
guarantees at least 2 successful SEQUENCEs per lease period.

The 600s socket idle-timeout is **independent** of session lease
expiry — it's a TCP-layer reap.  With `lease_time = 90` (the
reffs default) and `interval = 30`, the socket sees a SEQUENCE
every 30s, well under the 600s reaper threshold.  Both timers
are satisfied with one interval.

Configurable knob (TOML):
```toml
[mds]
ds_session_renewal_interval_sec = 30  # default; 0 = disabled
ds_session_renewal_max_sec      = 60  # clamp; renewal won't sleep longer
```

The disabled-mode (`= 0`) is for tests and for the
"no remote DSes" pure-local-VFS combined-mode case.  Default-on
otherwise.

### Concurrency

`ds_renewal` runs on its own pthread, same as `ps_renewal`.  It
iterates the dstore registry under `rcu_read_lock` (since
`dstore_find` + the dstore table are already RCU-protected per
`design/per-export-dstore.md`).  Per-dstore tick takes the
session-borrow ref (already locked); the renewal thread does
NOT hold any dstore lock across the SEQUENCE round-trip.

`dstore_session_borrow` returns a `struct mds_session *` with a
ref the caller must drop via `dstore_session_release`.  The ref
count guarantees the session lives across the round-trip even
if another path concurrently destroys the dstore's session
(`dstore_session_replace(ds, NULL)` after a BADSESSION).

The renewal thread uses RCU for iterating the dstore list:
1. `rcu_read_lock()`
2. `for each ds in dstore_list: dstore_get_ref(ds)`
3. Collect refs into a stack array (`dstores_to_tick[]`).
4. `rcu_read_unlock()`
5. For each refcounted dstore: `ds_renewal_tick_one(ds)`; then
   `dstore_put_ref(ds)`.

This matches the established `cds_lfht` iteration pattern in
`.claude/patterns/rcu-violations.md` (collect refs under lock,
operate outside).

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

Single-commit slice:

1. **TDD: write the 8 unit tests** referencing the new API.  The
   tests will not compile against the pre-fix tree (no
   `ds_renewal_start`, no `ds_renewal_tick_one`, no
   `dstore_session_borrow`).  Land them in the same commit as
   the implementation.

2. **Promote `ps_session_is_dead` and `ps_reconnect_backoff_next`
   to lib/nfs4/client/mds_session.{c,h}.**  Rename to
   `mds_session_is_dead` and `mds_reconnect_backoff_next`.
   Update the one PS call site.  This is a pure rename + move,
   no behaviour change, but it's a precondition for the new
   ds_renewal sharing the same helpers without copy-paste.

3. **Add `ds_reconnect_backoff_sec` and
   `ds_reconnect_next_attempt_ns` to `struct dstore`** (atomic
   fields).  Add `dstore_session_borrow` / `_release` /
   `_replace` accessors mirroring the PS pattern.

4. **Write `lib/nfs4/dstore/ds_renewal.c`** mirroring
   `lib/nfs4/ps/ps_renewal.c` line-by-line, swapping the
   listener iteration for the dstore iteration.

5. **Wire `ds_renewal_start` into `reffsd.c`** alongside the
   existing `ps_renewal_start` call.  Read interval from
   TOML config (`mds.ds_session_renewal_interval_sec`, default
   `server_lease_time(ss) / 3`).

6. **Wire `ds_renewal_stop` into `reffsd_shutdown`** before
   the dstore registry teardown.

7. **Add TOML knob.**  Parse `[mds]
   ds_session_renewal_interval_sec` (default `lease_time / 3`,
   value `0` disables).

8. **Run `make check`.**  All 32 conn_info tests + the 8 new
   ds_renewal tests pass.  Pre-existing flake
   `ps_reconnect_test::test_session_replace_quiesces_in_flight`
   continues to be unrelated.

9. **Functional gate**: re-run Track 2 N=8 fio on garbo, grep
   for "601 seconds inactive" in any DS log.  Expected count
   after the fix: **0**.

10. **Reviewer gate.**  This is a concurrency / lifecycle slice
    touching session state.  Run the reviewer agent before
    commit per `.claude/CLAUDE.md`.

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
| `lib/include/reffs/dstore.h` | `ds_reconnect_backoff_sec`, `ds_reconnect_next_attempt_ns` fields |
| `lib/nfs4/dstore/dstore.c` | `dstore_session_borrow`/_release/_replace accessors |
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
  removes.
- `.claude/design/dstore-vtable-v2.md` — defines the MDS-to-DS
  session that this slice keeps alive.
- `.claude/design/trust-stateid.md` — uses the same MDS-to-DS
  session for TRUST_STATEID issuance.  Deferred item 1 cross-
  references this doc's "DS-side MDS reboot detection".
- `lib/nfs4/ps/ps_renewal.c` — the template this slice mirrors.
