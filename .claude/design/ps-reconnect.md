---
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
---

# PS Upstream Session Reconnect

## Context

Today the keepalive thread (`lib/nfs4/ps/ps_renewal.c`) detects
upstream-session-killer wire status (NFS4ERR_BADSESSION, etc.) by
observing `mds_session_renew_lease()` failure, **logs once**, and
leaves `pls->pls_session` in place.  Subsequent forwarded ops hit
the dead session and return `NFS4ERR_IO` to end clients.  The PS
is unrecoverable until restart.

The header even calls this out explicitly:

> "Reconnect on BADSESSION:
>   NOT_NOW_BROWN_COW.  When a renewal returns NFS4ERR_BADSESSION
>   (lease already expired between ticks), the thread logs once and
>   leaves pls_session in place.  Subsequent forwarded ops will hit
>   the same status; reconnect logic that ties to the [[proxy_mds]]
>   config to recreate the TLS session is its own slice."
>   -- lib/nfs4/ps/ps_renewal.h

This slice ships that NOT_NOW_BROWN_COW: when the renewal thread
sees BADSESSION/DEADSESSION/STALE_CLIENTID, tear down the dead
session and rebuild a fresh one (TLS + EXCHANGE_ID + CREATE_SESSION
+ PROXY_REGISTRATION).  Cached proxy-SB upstream FHs survive --
they are inode-on-disk identifiers that don't change with the
upstream MDS's session lifetime.

## Tests first

### Unit (`lib/nfs4/ps/tests/ps_reconnect_test.c`, NEW)

| Test | Intent |
|------|--------|
| `test_classify_session_dead` | `ps_session_is_dead(-EREMOTEIO, NFS4ERR_BADSESSION)` returns true; `ps_session_is_dead(-EREMOTEIO, NFS4ERR_DEADSESSION)` true; `ps_session_is_dead(-EREMOTEIO, NFS4ERR_STALE_CLIENTID)` true; `ps_session_is_dead(0, NFS4_OK)` false; `ps_session_is_dead(-EREMOTEIO, NFS4ERR_DELAY)` false (per-op transient, not session-killer). |
| `test_classify_connection_lost` | `ps_session_is_dead(-EIO, 0)` true; `ps_session_is_dead(-EPIPE, 0)` true; `ps_session_is_dead(-ECONNRESET, 0)` true; `ps_session_is_dead(-ETIMEDOUT, 0)` true. |
| `test_backoff_progression` | First call to `ps_reconnect_backoff_next(state)` returns 0 (no wait); second returns 1s; third 2s; ...; capped at 60s.  After `ps_reconnect_backoff_reset(state)`, returns to 0. |
| `test_listener_session_swap_atomic` | Spawn 4 reader threads that loop calling `ps_listener_session_borrow()` + `ps_listener_session_release()`.  Spawn 1 writer that calls `ps_listener_session_replace()` 100 times.  Run for 1s.  Assert: every borrow/release pair sees a non-NULL session, and `mds_session_destroy` was called exactly the same number of times as `replace`.  TSAN clean. |
| `test_session_destroy_quiesces_in_flight` | Mock `mds_session_destroy` so it sets `destroyed = true` instead of freeing.  Hold a `ps_listener_session_borrow()` ref on session A; spawn a thread that calls `ps_listener_session_replace()` with session B.  While the borrow is held, assert: replace() has NOT returned (block on `pthread_join` with a deadline that proves blocking, not just slow); A's `destroyed` is still false.  Release the borrow.  After release, replace() returns; A's `destroyed` becomes true; `pls_session == B`. |
| `test_concurrent_worker_during_replace` | Spawn 4 reader threads that loop borrow/use/release continuously.  In the main thread, call replace() 50 times back-to-back.  No reader observes a NULL session inside the borrow critical section (the rwlock blocks readers during the destroy/swap window so they wait, then resume on the new session).  TSAN clean. |
| `test_shutdown_during_reconnect` | Stub the reconnect path so `mds_session_create_tls` blocks on a condvar the test controls.  Trigger a reconnect (set listener as dead).  While reconnect is mid-handshake (waiting on the test's condvar), call `ps_renewal_stop()`.  Signal the condvar to unblock the handshake.  Assert: the renewal thread observes `s_renewal_running == 0`, frees the partially-built session without leaking it (LSAN clean), and the listener's `pls_session` remains the original (never replaced with the partially-built one). |

### Functional (`lib/nfs4/ps/tests/ps_reconnect_functional_test.c`, NEW)

Uses an in-process mock MDS (existing `mds_mock` if present; else minimal
NFSv4.2 server stub that can be bounced).

| Test | Intent |
|------|--------|
| `test_reconnect_on_badsession` | Bring up PS against mock MDS.  Mock starts returning NFS4ERR_BADSESSION on next SEQUENCE.  PS renewal thread observes, tears down session, rebuilds.  Mock observes EXCHANGE_ID + CREATE_SESSION + PROXY_REGISTRATION on a fresh connection.  `pls->pls_session` is replaced; subsequent forwarded ops succeed. |
| `test_reconnect_backoff_when_mds_down` | Mock MDS goes silent (drops all connections).  PS attempts reconnect; first attempt fails immediately; second waits 1s; sequence follows backoff schedule.  When mock comes back online, reconnect succeeds and backoff resets. |
| `test_reconnect_squat_delay_retry` | Mock MDS returns NFS4ERR_DELAY on PROXY_REGISTRATION (squat-guard simulating an unexpired prior registration).  PS retries with backoff and eventually succeeds.  Inflight forwarded ops during the gap return NFS4ERR_DELAY to clients. |
| `test_no_reconnect_on_per_op_error` | Forwarded LAYOUTGET returns NFS4ERR_LAYOUTUNAVAILABLE.  Renewal does NOT trigger a reconnect (per-op error, not session-killer). |
| `test_cached_fhs_survive_reconnect` | Pre-populate proxy SB with upstream root FH.  Force reconnect.  After reconnect, the same root FH is still on the SB (no re-traversal needed). |

### Soak (`scripts/ci_ps_reconnect_soak.sh`, NEW)

30-minute (CI) / 8-hour (BAT) loop:
- One client thread driving reads/writes through PS continuously.
- Bounce the upstream MDS every 90s (longer than the lease so the
  renewal definitely sees BADSESSION on the next tick).
- Pass criteria match `ps_soak_test.sh` from `proxy-server.md`:
  ASAN/UBSAN clean, RSS bounded, FD count bounded.  Every bounce
  must result in a successful reconnect within 5 lease periods
  (sufficient slack for backoff + PROXY_REGISTRATION squat-guard
  on the MDS side).

### Test impact on existing tests

| File | Impact |
|------|--------|
| `lib/nfs4/ps/tests/ps_renewal_test.c` (does not exist yet, but planned) | If created in parallel: add ONE test that verifies a non-session-killer renewal failure does NOT trigger reconnect.  No existing tests modified. |
| All other `make check` tests | PASS -- additive change, no existing behaviour modified. |

## Design

### State machine

| State | Trigger | Action | Next state |
|-------|---------|--------|-----------|
| ACTIVE | none (steady state) | tick: send SEQUENCE | ACTIVE |
| ACTIVE | renewal returns (err, sr_status) classified as session-killer | mark dead, schedule reconnect at `pls_reconnect_next_attempt_ns` | DEAD |
| DEAD | `now < pls_reconnect_next_attempt_ns` | skip tick | DEAD |
| DEAD | `now >= pls_reconnect_next_attempt_ns` | run reconnect (TLS + EXCHANGE_ID + CREATE_SESSION + PROXY_REGISTRATION) | RECONNECTING |
| RECONNECTING | reconnect step fails | bump backoff, schedule next attempt | DEAD |
| RECONNECTING | reconnect succeeds | replace() session, reset backoff | ACTIVE |
| any | `ps_renewal_stop()` (s_renewal_running -> 0) | abort current step, free new_ms if any, drop wlock | STOPPED |

The state is implicit in the listener's `pls_session != NULL`,
`pls_reconnect_backoff_sec`, and `pls_reconnect_next_attempt_ns`
fields -- no explicit enum needed.  The renewal thread reads
these on each tick to decide which branch to take.

### RFC references

- RFC 8881 S18.46 (SEQUENCE: the renewal vehicle).
- RFC 8881 S2.10.6 (lease period semantics).
- RFC 8881 S18.36.3 (NFS4ERR_BADSESSION on a session whose
  state has been destroyed; NFS4ERR_DEADSESSION on a session
  marked dead by the server pending DESTROY_SESSION;
  NFS4ERR_STALE_CLIENTID on a clientid the server no longer
  recognizes).

### Concurrency model: per-listener rwlock

Add `pthread_rwlock_t pls_session_rwlock` to `struct ps_listener_state`.

- **Workers** (forward path) take a read lock around the entire
  `ps_proxy_forward_*` call (which internally calls
  `mds_compound_send_with_auth`).  Multiple workers may forward
  concurrently -- the existing `ms_call_mutex` inside
  `struct mds_session` serializes the slot-1 SEQUENCE bookkeeping;
  the rwlock only protects session **lifetime**, not RPC ordering.
- **Renewal thread** takes a write lock during reconnect.  Write
  lock waits for all in-flight forwarders to finish their current
  RPC; existing forwarders see a coherent session-pointer-and-state
  for the duration of their op.  New forwarders during the gap
  block waiting for the write lock to release.

This is `Option A` from the planning notes: simplest correct option.
Reader stall during reconnect is bounded by `clnt_call` timeout +
TLS handshake + EXCHANGE_ID + CREATE_SESSION + PROXY_REGISTRATION
(typically <2 s).  An RCU- or refcount-based zero-stall variant is
NOT_NOW_BROWN_COW.

**Lock order rule** (must be respected in code; verified by TSAN
soak):

```
pls_session_rwlock (read or write)
  -> ms_call_mutex (only acquired inside mds_compound_send_with_auth
                    or mds_session_destroy)
```

The renewal thread acquires the rwlock for write FIRST, then
calls `mds_session_destroy` which internally takes the
`ms_call_mutex` of the *old* session.  Workers acquire the rwlock
for read FIRST, then call into `mds_compound_send_with_auth` which
takes the `ms_call_mutex` of the *current* session.  No path
acquires `ms_call_mutex` before the rwlock.  Reverse order would
deadlock.

### Encapsulating accessors (helpers, not raw rwlock calls)

Two helpers live in `lib/nfs4/ps/ps_state.c`:

```c
/*
 * Borrow the listener's current upstream session under read-lock.
 * Returns NULL if the listener has no session (boot before first
 * connect, or reconnect in progress and reader gave up waiting).
 * Callers MUST pair with ps_listener_session_release().
 */
struct mds_session *ps_listener_session_borrow(uint32_t listener_id);
void ps_listener_session_release(uint32_t listener_id);

/*
 * Replace the listener's upstream session under write-lock.
 * Drains in-flight readers, sets the new session pointer (which
 * may be NULL while the renewal thread is between destroy and
 * create), and destroys the old session if non-NULL.
 *
 * Caller is responsible for the caller-allocated new_session;
 * after replace returns the listener owns it.  If the caller
 * passes new_session == NULL, the listener's session is cleared
 * (used during the destroy half of a reconnect cycle).
 */
void ps_listener_session_replace(uint32_t listener_id,
                                 struct mds_session *new_session);
```

Worker call sites (see survey question 2) change from:
```c
if (!pls || !pls->pls_session)
    return NFS4ERR_DELAY;
ret = ps_proxy_forward_X(pls->pls_session, ..., creds, ...);
```
to:
```c
struct mds_session *ms = ps_listener_session_borrow(listener_id);
if (!ms)
    return NFS4ERR_DELAY;
ret = ps_proxy_forward_X(ms, ..., creds, ...);
ps_listener_session_release(listener_id);
```

### Trigger: renewal thread classifies session-killer wire codes

Add a helper that classifies the result of a renewal call:

```c
/*
 * True if (err, sr_status) indicates the upstream session is dead
 * and must be rebuilt.  Distinguishes session-killers from per-op
 * transients like NFS4ERR_DELAY.
 *
 * Inputs:
 *   err       -- return value from mds_session_renew_lease()
 *                (-EREMOTEIO, -EIO, -EPIPE, -ECONNRESET, etc.)
 *   sr_status -- the sr_status field of the SEQUENCE response,
 *                or 0 if no response was decoded
 *
 * Session-killer wire codes:
 *   NFS4ERR_BADSESSION, NFS4ERR_DEADSESSION,
 *   NFS4ERR_STALE_CLIENTID, NFS4ERR_BAD_SESSION_DIGEST
 *
 * Connection-killer errno codes:
 *   -EIO (RPC layer dropped), -EPIPE, -ECONNRESET, -ETIMEDOUT,
 *   -ENOTCONN, -ENETUNREACH
 */
bool ps_session_is_dead(int err, nfsstat4 sr_status);
```

The renewal-tick callback inspects `(err, sr_status)`.  If
`ps_session_is_dead`, kick the reconnect for that listener.

### Plumbing sr_status to the renewal callback

`mds_session_renew_lease()` today returns `-EREMOTEIO` for any
NFS4ERR -- the wire status is lost.  We need to extend the
contract.  Two options:

**(a)** Add `mds_session_renew_lease_ex(ms, &sr_status)` which
fills the SEQUENCE op's status code on output.  The renewal
caller consults `sr_status` to classify.

**(b)** Embed the status in the negative errno encoding (out of
range; risks colliding with future errno values).

(a) wins.  Add the new helper next to the existing one in
`lib/nfs4/client/mds_session.c`; route `ps_renewal.c` to use
the `_ex` variant.  Existing `mds_session_renew_lease()` keeps
working unchanged (no other callers today).

### Reconnect sequence

When the renewal thread classifies a session as dead, it calls
`ps_listener_reconnect(listener_id)` which:

1. Read the listener's `[[proxy_mds]]` config (already cached on
   `ps_listener_state` -- pls_upstream/pls_upstream_port/etc.).
2. Allocate + initialize a fresh `struct mds_session`
   (`calloc`, `mds_session_set_owner`).
3. Call `mds_session_create_tls()` with the cached config.  May
   take 100s of ms (TLS handshake + EXCHANGE_ID + CREATE_SESSION).
4. Call `mds_session_send_proxy_registration()` with the same
   `prr_registration_id` as the initial bring-up (preserves
   identity for squat-guard renewal semantics on the MDS).
5. Call `ps_listener_session_replace(listener_id, new_ms)` --
   this is the atomic write-locked swap.  Old session is
   destroyed by `replace()`.

Failure handling (any step fails):
- Free `new_ms` (don't replace).
- Apply backoff (see below) before next attempt.
- Leave the existing (dead) session in place -- worker forwards
  continue to fail with `-EREMOTEIO`, returning DELAY/IO to
  end clients.  Acceptable: if the MDS is down, the PS can't
  serve anyway.

### Backoff schedule

Per-listener state on `struct ps_listener_state`:

```c
uint32_t pls_reconnect_backoff_sec;     /* current wait, 0..60 */
uint64_t pls_reconnect_next_attempt_ns; /* CLOCK_MONOTONIC */
```

Schedule (capped exponential):

| Attempt | Delay before attempt |
|---------|---------------------|
| 1 (first BADSESSION) | 0s (immediate) |
| 2 | 1s |
| 3 | 2s |
| 4 | 4s |
| 5 | 8s |
| 6 | 16s |
| 7 | 32s |
| 8+ | 60s (cap) |

Reset to 0 on successful reconnect.

The renewal thread integrates the backoff: each tick, before
attempting reconnect on a dead session, check
`now_ns >= pls_reconnect_next_attempt_ns`.  If not, skip this
tick.

### Re-discovery on reconnect

**Not in this slice.**  Cached upstream FHs on proxy SBs are
expected to remain valid because:
- The upstream MDS inode numbering is stable across session
  lifetimes.
- An upstream MDS *reboot* (boot_seq change) would also
  invalidate FHs, but that surfaces as `NFS4ERR_STALE` on
  forwarded ops -- which the existing code already maps to
  `NFS4ERR_STALE` for end clients (correct behaviour).

If the upstream MDS reboot triggers wholesale re-discovery,
that's a separate slice (per `proxy-server.md` Phase 2 on-demand
triggers, already scoped there).

### What worker-path triggers do

The forward path observes its own session-killer errors when
forwarding fails on a session that just died (race between
renewal-thread detection and worker mid-RPC).  In this slice,
workers do **nothing extra**: they return DELAY/IO to the
end client and rely on the next renewal tick to detect.

Adding a "kick the renewal thread early when a worker sees
BADSESSION" path is **NOT_NOW_BROWN_COW** -- worth doing if
worst-case 30s recovery proves too slow in practice.  The wire
already exists for the kick: `pthread_cond_broadcast` on
`s_renewal_cv` (used today only for shutdown).

## Failure modes and what end clients see

| Scenario | End client sees | Reason |
|----------|----------------|--------|
| MDS healthy, session expires by idle, PS keepalive misses one tick | NFS4_OK | Reconnect happens within one renewal interval (default 30s); transient blip likely <2s when reconnect runs. |
| MDS goes hard down | NFS4ERR_DELAY (forwarders), then NFS4ERR_IO (after a few attempts) | PS retries with backoff; end clients should retry their own ops; once MDS comes back, normal service resumes. |
| MDS reboot (boot_seq change, FHs invalid) | NFS4ERR_STALE on forwarded ops referencing pre-reboot FHs | Correct; client re-mounts or refreshes. PS reconnect succeeds once MDS is back and PROXY_REGISTRATION re-runs. |
| Squat-guard delays new PROXY_REGISTRATION | NFS4ERR_DELAY (forwarders) | PS retries PROXY_REGISTRATION on backoff; once old MDS-side lease expires, new registration succeeds. |

## Sequence: BADSESSION → reconnect → resume

```
worker             renewal thread          mds_session            upstream MDS
  |                       |                      |                       |
  |---borrow()-->         |                      |                       |
  |  forward op           |                      |                       |
  |---------------------->|--->mds_compound_send_with_auth-->...          |
  |                       |                      |---SEQUENCE----------->|
  |                       |                      |<------BADSESSION------|
  |  release()<----       |                      |                       |
  |                       |                      |                       |
  |   :: meanwhile ::     |                      |                       |
  |                       |---tick                                        |
  |                       |---renew_lease_ex                              |
  |                       |   sr_status=BADSESSION                       |
  |                       |---ps_session_is_dead -> true                  |
  |                       |---reconnect():                                |
  |                       |   alloc new_ms                                |
  |                       |   create_tls (handshake + EXCHANGE_ID + CS)   |
  |                       |---send PROXY_REGISTRATION                     |
  |                       |---replace(new_ms)                             |
  |                       |   <wlock> destroy(old_ms) </wlock>            |
  |                       |   pls_session = new_ms                        |
  |  next worker hits read-lock on new_ms -- forward succeeds             |
```

## Files modified

| File | Change |
|------|--------|
| `lib/nfs4/ps/ps_state.h` | Add `pthread_rwlock_t pls_session_rwlock`, `uint32_t pls_reconnect_backoff_sec`, `uint64_t pls_reconnect_next_attempt_ns` to `struct ps_listener_state`; declare `ps_listener_session_{borrow,release,replace}` and `ps_session_is_dead`. |
| `lib/nfs4/ps/ps_state.c` | Init/destroy rwlock; implement borrow/release/replace; backoff helpers. |
| `lib/nfs4/ps/ps_renewal.c` | Replace plain renewal-tick with `(err, sr_status)` classification; on dead session, run reconnect with backoff. |
| `lib/nfs4/ps/ps_renewal.h` | Strike NOT_NOW_BROWN_COW; document reconnect behaviour, backoff schedule, end-client visible errors. |
| `lib/nfs4/ps/ps_proxy_ops.c` | Convert `pls->pls_session` access sites to borrow/release. |
| `lib/nfs4/ps/ps_inode.c` | Same conversion at the lookup forwarder check. |
| `lib/nfs4/ps/ps_discovery.c` | Same conversion at the discovery lookup. |
| `lib/nfs4/client/mds_session.c` | Add `mds_session_renew_lease_ex(ms, &sr_status)` returning the SEQUENCE op's wire status. |
| `lib/nfs4/client/ec_client.h` | Declare `mds_session_renew_lease_ex`. |
| `lib/nfs4/ps/tests/ps_reconnect_test.c` | NEW unit tests (Group A above). |
| `lib/nfs4/ps/tests/ps_reconnect_functional_test.c` | NEW functional tests (Group B). |
| `scripts/ci_ps_reconnect_soak.sh` | NEW soak test. |
| `lib/nfs4/ps/tests/Makefile.am` | Add `ps_reconnect_test` and `ps_reconnect_functional_test` to `check_PROGRAMS`; add `_SOURCES`, `_CFLAGS`, `_LDADD` lines matching the existing `ps_state_test` pattern. |

## Risks

1. **TSAN on `pls_session_rwlock`**: rwlock + `mds_session->ms_call_mutex`
   nesting must be correct.  Order: rwlock(read) -> ms_call_mutex.
   Reverse order would deadlock against the renewal thread's
   rwlock(write).  Renewal thread takes wlock without ever holding
   ms_call_mutex.  Document the order; validate with a TSAN soak.

2. **PROXY_REGISTRATION squat-guard pile-up**: if the MDS holds the
   prior PS registration valid for the full lease (e.g., 90s), the
   first reconnect attempt sees DELAY.  Backoff naturally handles
   this -- by attempt 6 (16s) we're past most realistic squat
   windows; by attempt 8 (60s) we're past the lease.  Functional
   test `test_reconnect_squat_delay_retry` verifies.

3. **Reconnect during shutdown**: `ps_renewal_stop()` flips
   `s_renewal_running` to 0 and broadcasts the cv.  The reconnect
   path must check `s_renewal_running` between TLS handshake and
   `replace()`, and abort if shutdown started -- otherwise we
   leak a partially-built session.  Implementation detail; covered
   by the destroy-quiesce unit test.

4. **Lock contention on hot listeners**: a busy PS where many
   workers are forwarding constantly will see brief wlock waits
   on every reconnect.  Bounded by RPC-in-flight count + RPC
   timeout.  Acceptable for v1; the RCU/refcount path remains
   the optimization escape hatch.

5. **mds_session_destroy after replace**: the replace helper
   destroys the old session under wlock.  `mds_session_destroy`
   sends DESTROY_SESSION + DESTROY_CLIENTID before close.  If the
   old session is already dead, those wire ops will fail -- that's
   fine, `clnt_destroy` still cleans up locally.  Verify no
   ASAN/LSAN issue when the destroy path errors.

## Admin diagnostics

For BAT scope, **logging is sufficient**.  The renewal thread
emits one `LOG()` line on session-killer detection (which the
soak harness greps for) and one `TRACE()` line per reconnect
attempt with backoff state.  A probe op
(`probe1_op_ps_listener_status`) returning per-listener state
(session pointer non-NULL, current backoff, last reconnect
timestamp) would be cleaner for runtime introspection but is
deferred to a later slice -- it's an admin-introspection
nice-to-have, not a correctness item.

## Deferred / NOT_NOW_BROWN_COW

- Worker-path "kick renewal early on observed BADSESSION" --
  cuts worst-case recovery from `interval` seconds to TLS-handshake
  time.  Wire already exists (`s_renewal_cv`); add a
  `ps_listener_kick_reconnect()` helper.
- Zero-stall reader path via RCU or refcount on `mds_session`.
- Wholesale re-discovery on upstream MDS reboot (boot_seq change).
- Per-listener config knob for backoff schedule (today: hardcoded
  exponential with 60s cap).
- Probe visibility for reconnect state (`probe_listener_status`
  showing last reconnect timestamp, attempt count, current backoff)
  -- nice-to-have, not on the BAT critical path.
- Per-listener configurable backoff schedule (today: hardcoded
  exponential capped at 60s; non-standard MDS leases would need
  to wait 2 attempts past the cap to clear squat-guard).

## Verification

1. `make check` -- all existing + new tests pass.
2. New unit tests: TSAN clean.
3. New functional tests pass against the in-process mock MDS.
4. `ci_ps_reconnect_soak.sh` 30-min run on bench: ASAN/UBSAN clean,
   bounded RSS/FD, every MDS bounce results in a reconnect within
   5 lease periods.
5. `make -f Makefile.reffs license` + `style` -- clean.
6. Reviewer agent pass before commit (this slice meets multiple
   triggers: cross-layer boundary, ~150-200 LOC, lock-discipline
   change).
