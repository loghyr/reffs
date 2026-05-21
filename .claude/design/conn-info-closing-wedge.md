<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# conn_info CONN_CLOSING Slot-Wedge -- BLOCKER

## Context

chunk-collision Track 2's 8-rank scaled case
(`run_chunk_collision_track2.sh --n 8`, 8 Proxy Servers against one
MDS) failed at bringup: 6 of 8 PSes could not establish their
upstream MDS session.  The MDS accept loop was stuck in a failure
loop, logging once every ~3 s:

```
io_handle_accept:567: Failed to register client connection fd=23
```

-- the same `fd=23` indefinitely.  Eight-plus minutes after the run
the MDS was still emitting it; the slot never recovered.

The 4-PS cases (`coll_t2_ior_basic`, `coll_t2_ior_reorder`) pass
clean.  The bug needs the connection churn of N=8 to trigger.

## Severity: BLOCKER

`io_conn_register` keys the connection table by `fd % MAX_CONNECTIONS`
(`MAX_CONNECTIONS == 65536`, so for the usual fd range the slot *is*
the fd).  A slot stuck in `CONN_CLOSING` is **permanently poisoned**:
every subsequent `accept()` the kernel satisfies with that fd number
fails `io_conn_register`, `io_handle_accept` closes the fd, the
kernel hands the same number back on the next accept, and the loop
repeats forever.

On a long-running MDS this is cumulative -- each wedged connection
burns one fd slot for the life of the process.  Enough of them and
the server stops accepting connections on a widening set of fds.
This is not a Track-2-only artifact; any production MDS with PS
clients (or enough NFS-client churn) hits it.

It is **not** caused by the FFv2 / mirror / wire-format work --
`lib/io/` is untouched by all of that.  It is a pre-existing
`conn_info`-lifecycle defect, the same family as INV-5 / INV-6.

## Root cause -- two distinct bugs

### Bug A -- a drain counter can leak

`io_conn_unregister` (`lib/io/conn_info.c:702`) marks a slot
`CONN_CLOSING` but deliberately keeps `ci_fd` and the four in-flight
op counters (`ci_read_count`, `ci_write_count`, `ci_accept_count`,
`ci_connect_count`) plus the `ci_write_active` gate.  This is the
INV-6 Stage-3-Slice-4 anti-UAF design: a stale io_uring CQE for the
old connection must still find *its* `conn_info` and decrement *its*
counter, not corrupt a freshly-reused slot.

`conn_drain_if_idle_locked` (`conn_info.c:689`) completes the
`CONN_CLOSING -> CONN_UNUSED` transition only once all four counters
are zero and `ci_write_active` is false.  It is called from each
`io_conn_remove_*_op` and from `io_conn_unregister`.

The wedge: a counter is incremented by `io_conn_add_*_op` when an
SQE is submitted, but the matching `io_conn_remove_*_op` never runs
-- so the counter never returns to zero and the drain never
completes.  The missing decrement is on some CQE-completion path
that, on seeing the connection closed / closing / "not tracked",
returns early without pairing the add with a remove.  (The INV-6
investigation already saw the adjacent symptom
`io_rpc_trans_cb: Connection not tracked for fd=NN` -- a CQE the
handler could not attribute.)

**The exact leaked counter and the exact early-return path are a
Slice 2 investigation item** -- see below.

### Bug B -- the force-drain backstop never runs on Linux

A backstop already exists.  `io_conn_check_timeouts`
(`conn_info.c:956`) has a dedicated pass (lines 1004-1032) that
force-drains a slot stuck in `CONN_CLOSING` past a timeout: it
zeroes the four counters, clears `ci_write_active`, sets
`CONN_UNUSED`, `ci_fd = -1`, and logs a warning -- without
re-closing the fd.

But `io_conn_check_timeouts` is called from exactly one place:
`kqueue_socket.c:663`, the **macOS / BSD** event loop.  The Linux
**io_uring** path's heartbeat (`heartbeat.c`, the connection-check
block at lines 254-291) runs its own timeout scan that

- only inspects slots in state `CONN_CONNECTED` (`heartbeat.c:261`
  `if (!ci || ci->ci_state != CONN_CONNECTED) continue;`), so it
  skips `CONN_CLOSING` slots entirely, and
- never calls `io_conn_check_timeouts`.

So on Linux -- the deployment platform, and the bench MDS -- the
`CONN_CLOSING` force-drain **never executes**.  Bug A, which on
macOS would self-heal within the timeout, is unrecoverable on
Linux.  This is why the bench MDS stayed wedged for 8+ minutes
with a 60 s nominal timeout.

## CONN_CLOSING state machine

```
   io_conn_register
        |
        v
   CONN_ACCEPTED / CONN_CONNECTING ... CONN_CONNECTED
        |   (add_*_op / remove_*_op move through
        |    READING / WRITING / READWRITE)
        |
        |  io_conn_unregister
        v
   CONN_CLOSING  -- ci_fd kept, counters kept
        |
        |  every io_conn_remove_*_op / io_conn_write_done
        |  calls conn_drain_if_idle_locked
        |
        +--[ all counters 0 && !write_active ]--> CONN_UNUSED  (healthy)
        |
        +--[ a counter leaked (Bug A) ]----------> stuck forever
                 |
                 |  force-drain pass in io_conn_check_timeouts
                 |  -- BUT only reached on the kqueue loop (Bug B);
                 |     never on Linux io_uring
                 v
            CONN_UNUSED  (only on macOS today; never on Linux)
```

Healthy drains complete in milliseconds (the CQEs for a closing
connection arrive promptly).  A slot still `CONN_CLOSING` seconds
later is wedged by definition.

## Fix plan -- two slices

### Slice 1 -- wire the force-drain backstop into the io_uring loop

Make the existing `CONN_CLOSING` force-drain run on Linux.  This
turns the BLOCKER from "permanent" into "self-heals in a few
seconds" and is low-risk -- it ships the *existing*, reviewed
force-drain logic, just from the loop that was missing it.

Design decisions:

1. **Single shared implementation.**  Both event loops must call
   the *same* `io_conn_check_timeouts` so they cannot drift again.
   The io_uring heartbeat's connection-check block calls
   `io_conn_check_timeouts(...)` instead of carrying its own
   parallel `CONN_CONNECTED`-only scan.  (The heartbeat's
   "resubmit a read if `ci_read_count == 0`" behaviour for live
   connections is retained -- only the timeout half is unified.)

2. **A short, CONN_CLOSING-specific timeout.**  The 60 s figure is
   right for *idle live connections*; it is far too long for a
   wedged drain.  A `CONN_CLOSING` slot that has made no progress
   for more than `CONN_CLOSING_FORCE_DRAIN_SECS` (proposed: 5 s)
   is wedged -- a healthy drain finishes in milliseconds.  A 5 s
   force-drain keeps a wedged slot's outage within one PS-startup
   stagger window, so Track 2 N=8 bringup tolerates it.
   `io_conn_check_timeouts` gains a separate, shorter deadline for
   the `CONN_CLOSING` pass than for the live-idle pass.

3. The force-drain already logs a `LOG()` warning per wedged slot
   -- keep it; each line is one Bug-A occurrence and is the
   field signal that Slice 2 is still needed.

Slice 1 alone makes the MDS production-safe (bounded self-heal)
and unblocks Track 2's 8-rank case.

### Slice 2 -- fix the counter leak (Bug A) at the source

Eliminate the leak so the force-drain never has to fire.

Investigation approach (the leaked counter is not yet pinned):

1. Add paired add/remove tracing: every `io_conn_add_*_op` and
   `io_conn_remove_*_op` logs `(fd, counter, value, caller)` under
   a build flag.
2. Reproduce with `run_chunk_collision_track2.sh --n 8` on a Linux
   bench host (dreamer); the wedge reproduces reliably at N=8.
3. From the force-drain warning line -- which already prints
   `r=%d w=%d a=%d c=%d write_active=%d` -- identify which
   counter is non-zero on the wedged slot.
4. Walk the CQE-completion path for that op type in `handler.c`
   and find the early-return that skips `io_conn_remove_*_op`
   (the "connection not tracked / closing, bail" branch).
5. Fix: that path must decrement its counter before returning --
   an SQE that was counted at submit time must be un-counted at
   completion time on **every** path, success or error, tracked
   or not.

The fix is delicate: it sits in the exact code the INV-6
Stage-3-Slice-4 hold was added to protect.  The decrement must
happen on the *correct* `conn_info` (matched by `ci_fd`, as the
other `*_remove_op` paths already do) so a stale CQE still cannot
touch a reused slot.

## Tests (first)

### Slice 1 -- unit

Extend `lib/io/tests/conn_info_test.c`:

| Test | Intent |
|------|--------|
| `test_closing_drain_completes_when_balanced` | register -> add 2 read ops -> remove 2 -> unregister: slot reaches `CONN_UNUSED` immediately (regression guard for the healthy path) |
| `test_closing_force_drain_on_leaked_counter` | register -> add 2 read ops -> remove 1 (simulate the Bug-A leak) -> unregister (slot is `CONN_CLOSING`, `ci_read_count == 1`) -> drive `io_conn_check_timeouts` with the slot's `ci_last_activity` aged past `CONN_CLOSING_FORCE_DRAIN_SECS` -> slot is `CONN_UNUSED`, counters zero, `ci_fd == -1` |
| `test_closing_not_drained_before_timeout` | same setup, but `ci_last_activity` recent -> `io_conn_check_timeouts` leaves the slot `CONN_CLOSING` (no premature reap of a drain still in progress) |
| `test_closing_force_drain_does_not_close_fd` | force-drained slot: assert no second `io_socket_close` on the fd (the fd was closed at the original unregister; re-closing risks a double-close on a descriptor the OS may have reissued) |

### Slice 1 -- functional / CI

- `run_chunk_collision_track2.sh --n 8` on a Linux bench host
  reaches IOR and passes (IOR write/read verify clean, sanitizers
  clean).  This is the acceptance test for the whole BLOCKER.
- The 4-PS basic and reorder cases still pass (no regression).

### Slice 2 -- unit

Once the leaked counter and its CQE path are identified, add a
test that drives that specific completion path (success and the
closing/not-tracked error variant) and asserts the add/remove
counter is balanced -- i.e. `conn_drain_if_idle_locked` completes
with no force-drain needed.

## Test impact analysis

| File | Impact |
|------|--------|
| `lib/io/tests/conn_info_test.c` | **EXTENDED** -- 4 new Slice-1 tests; existing cases unchanged |
| `lib/io/tests/conn_lifecycle_race_test.c` | **PASS, no change** -- the INV-5/INV-6 TSAN repro; Slice 1 adds a heartbeat caller but does not change `io_conn_*` semantics |
| `lib/io/tests/tls_write_count_test.c` | **PASS, no change** -- write-counter accounting is not touched by Slice 1 |
| All other `make check` tests | **PASS** -- Slice 1 changes only the io_uring heartbeat's timeout call; no protocol or fs surface |
| `run_chunk_collision_track2.sh` (Tracks 2 basic/reorder) | **PASS** -- already green; the N=8 case moves from FAIL to PASS |

Slice 2's test impact is assessed when the leaked counter is
identified; expected to be additive (one new targeted test) with
no existing-test changes.

## Risk

- **Slice 1: low.**  It ships the existing force-drain logic from
  the loop that lacked it.  The force-drain runs under
  `conn_mutex`; every CQE-completion path that decrements a
  counter also holds `conn_mutex`, so the zero-the-counters step
  cannot race a half-done decrement -- they serialise.  The one
  judgement call is the 5 s `CONN_CLOSING` timeout: too short
  risks reaping a drain that was merely slow.  Mitigation: a
  healthy drain completes in milliseconds; 5 s is three orders of
  magnitude of headroom, and the test
  `test_closing_not_drained_before_timeout` guards the lower
  bound.

- **Slice 2: medium.**  It edits the CQE-completion path inside
  the INV-6 anti-UAF region.  A decrement applied to the wrong
  `conn_info` (not matched by `ci_fd`) re-introduces exactly the
  UAF the `CONN_CLOSING` hold prevents.  The fix must match
  `ci_fd` like the existing `*_remove_op` paths.  TSAN
  (`conn_lifecycle_race_test`) is the regression gate.

## Deferred / NOT_NOW_BROWN_COW

- **INV-5 residue** -- concurrent mTLS PS->MDS session setup still
  flakes the bringup occasionally (`mds_session_create_tls failed`,
  recovered late by the renewal thread).  The 4 s PS-startup
  stagger papers over it.  Separate from this BLOCKER; tracked in
  experiments.md INV-5.
- **`MAX_CONNECTIONS` as `fd % N` keying** -- with `N == 65536`
  this is effectively a direct fd index and slot collisions
  between different fds cannot occur in practice; not revisited
  here.
- Unifying the kqueue and io_uring heartbeats wholesale (beyond
  the shared `io_conn_check_timeouts` call) is out of scope --
  Slice 1 only shares the timeout sweep.

## Key files

| File | Change |
|------|--------|
| `lib/io/conn_info.c` | Slice 1: separate `CONN_CLOSING` timeout in `io_conn_check_timeouts`.  Slice 2: the counter-leak fix lands here or in `handler.c`. |
| `lib/io/heartbeat.c` | Slice 1: connection-check block calls `io_conn_check_timeouts`; drop the parallel `CONN_CONNECTED`-only timeout scan |
| `lib/io/handler.c` | Slice 2: the leaking CQE-completion path |
| `lib/include/reffs/io.h` | Slice 1: `CONN_CLOSING_FORCE_DRAIN_SECS` constant |
| `lib/io/tests/conn_info_test.c` | Slice 1: 4 new tests |
| `deploy/benchmark/run_chunk_collision_track2.sh` | acceptance test (`--n 8`) |

## Acceptance

The BLOCKER is cleared when:

1. Slice 1 lands: `io_conn_check_timeouts` runs on the io_uring
   loop; a wedged `CONN_CLOSING` slot self-heals within
   `CONN_CLOSING_FORCE_DRAIN_SECS`; `make check` green; Track 2
   `--n 8` reaches IOR and passes on a Linux bench host.
2. Slice 2 lands: the counter leak is fixed at source; a Track 2
   `--n 8` run completes with **zero** "stuck in CLOSING ...
   force-draining" warnings in the MDS log.
