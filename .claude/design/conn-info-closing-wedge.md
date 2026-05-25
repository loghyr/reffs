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

### Slice 2 -- stop idle-reaping listener sockets

Slice 1 shipped; running `run_chunk_collision_track2.sh --n 8`
against it then PASSED -- the force-drain backstop cleared 4
wedged slots in ~6 s each and all 8 PSes registered.  The
force-drain warning lines, which print the per-counter state,
pinned the leaked counter:

```
Connection fd=18 stuck in CLOSING for 6 seconds
  (counts: r=0 w=0 a=1 c=0, write_active=0); force-draining
```

`a=1` -- the stuck counter is `ci_accept_count`, and the wedged
fds (18, 19, 20) are **listener sockets**: the MDS opens a TCP
listener per RPC service (NFSv4, NFSv3, MOUNT, NLM, NSM).  A
quiet listener sits in `CONN_ACCEPTING` with exactly one accept
SQE armed -- `ci_accept_count == 1` is its *healthy* idle state,
not a leak.

Root cause is in Slice 1 itself.  `io_conn_check_timeouts`'s
idle-close pass reaps any non-UNUSED, non-CLOSING slot idle past
`idle_timeout_seconds`.  The pre-Slice-1 io_uring heartbeat scan
acted only on `CONN_CONNECTED` slots, so it skipped listeners
implicitly; Slice 1 unified the two event loops onto the shared
`io_conn_check_timeouts`, which has no such exclusion.  A
listener gets no incoming connections during a Track 2 run, so
its `ci_last_activity` (refreshed only when an accept is
re-armed) ages past 60 s and the unified sweep `io_socket_close`s
it -- sending a perfectly healthy listener into `CONN_CLOSING`
with its armed accept op as the "stuck" `a=1`.

Fix: `io_conn_check_timeouts` skips `CONN_LISTENING` and
`CONN_ACCEPTING` slots in the idle-close pass.  A listener with
no incoming connections is idle by definition, not stale;
closing it silently stops the server accepting on that service.
The `CONN_CLOSING` force-drain pass is unchanged -- a listener
that genuinely closes still drains correctly.

This also corrects a latent defect in the kqueue path, which has
always called `io_conn_check_timeouts` and so has always been
able to idle-reap its own listeners; the shared fix covers both
loops.

NOT_NOW_BROWN_COW: the original pre-Slice-1 wedge (the first
N=8 run, `fd=23`, before Slice 1 existed and thus before the
listener-reaping regression) was not counter-instrumented -- the
counts-printing force-drain only runs with Slice 1.  If an N=8
acceptance run with Slice 1 + Slice 2 still logs any
"force-draining" warning, a genuine accept-CQE-completion leak
remains (a cancelled-accept CQE not pairing its
`io_conn_remove_accept_op`) and gets its own follow-up.  A clean
run closes the BLOCKER outright.

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

`lib/io/tests/conn_info_test.c` gains
`test_idle_sweep_spares_listeners`: register one `CONN_LISTENING`
slot and one `CONN_ACCEPTING` slot, run `io_conn_check_timeouts`
with an idle deadline of -1 (which would reap every non-listener
slot regardless of age), and assert both listener-state slots
survive in their original state.

## Test impact analysis

| File | Impact |
|------|--------|
| `lib/io/tests/conn_info_test.c` | **EXTENDED** -- 4 new Slice-1 tests + 1 Slice-2 test; existing cases unchanged |
| `lib/io/tests/conn_lifecycle_race_test.c` | **PASS, no change** -- the INV-5/INV-6 TSAN repro; the slices add a heartbeat caller but do not change `io_conn_*` semantics |
| `lib/io/tests/tls_write_count_test.c` | **PASS, no change** -- write-counter accounting is untouched |
| All other `make check` tests | **PASS** -- the slices change only the io_uring heartbeat's timeout call and the idle-sweep listener exclusion; no protocol or fs surface |
| `run_chunk_collision_track2.sh` (Tracks 2 basic/reorder) | **PASS** -- already green; the N=8 case moves from FAIL to PASS |

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

- **Slice 2: low.**  It adds two state values to a `continue`
  predicate in the idle-close pass -- `CONN_LISTENING` and
  `CONN_ACCEPTING` slots are skipped.  No counter accounting, no
  CQE path, no lock-ordering change; it restores an exclusion the
  pre-unification io_uring heartbeat already had.  The
  `CONN_CLOSING` force-drain pass is untouched.

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
| `lib/io/conn_info.c` | Slice 1: separate `CONN_CLOSING` timeout in `io_conn_check_timeouts`.  Slice 2: idle-close pass skips `CONN_LISTENING` / `CONN_ACCEPTING`. |
| `lib/io/heartbeat.c` | Slice 1: connection-check block calls `io_conn_check_timeouts`; drop the parallel `CONN_CONNECTED`-only timeout scan |
| `lib/include/reffs/io.h` | Slice 1: `CONN_CLOSING_FORCE_DRAIN_SECS` constant + 2-arg `io_conn_check_timeouts` |
| `lib/io/kqueue_socket.c` | Slice 1: caller passes the closing deadline |
| `lib/io/tests/conn_info_test.c` | Slice 1: 4 new tests; Slice 2: 1 new test |
| `deploy/benchmark/run_chunk_collision_track2.sh` | acceptance test (`--n 8`) |

## Acceptance

The BLOCKER is cleared when:

1. Slice 1 lands: `io_conn_check_timeouts` runs on the io_uring
   loop; a wedged `CONN_CLOSING` slot self-heals within
   `CONN_CLOSING_FORCE_DRAIN_SECS`; `make check` green; Track 2
   `--n 8` reaches IOR and passes on a Linux bench host.
   **Done -- landed 654ecb3754ea; N=8 run PASSED.**
2. Slice 2 lands: listener sockets are exempt from the idle-close
   sweep; a Track 2 `--n 8` run completes with **zero** "stuck in
   CLOSING ... force-draining" warnings in the MDS log.
   **Done -- landed 730d9787a2be; N=8 run PASSED with 0
   force-drain warnings.**

~~BLOCKER CLOSED.~~  Slice 1's force-drain backstop remains in
place as the safety net: should a genuine accept-CQE-completion
leak ever surface (the NOT_NOW_BROWN_COW above), a wedged slot
still self-heals within `CONN_CLOSING_FORCE_DRAIN_SECS` and logs
a warning -- that warning, not a stuck server, is the signal to
re-open the investigation.

**BLOCKER REOPENED 2026-05-25** by the conditional NOT_NOW_BROWN_COW
above.  Track 2 `--n 8` run with the new
[Criterion 4 in the harness](../../deploy/benchmark/run_chunk_collision_track2.sh)
(commit `969e3f9c514b`, "bench: track2 acceptance gates on zero
force-drain warnings") surfaced the predicted reopener -- but
with a sharper diagnosis than the original design anticipated.
See "Slice 3" below.

## Slice 3 -- the leak is on the read path, the symptom is mount EIO

### What the bench surfaced

Four runs of Track 2 `--n 8` against the new Criterion-4-gated
harness on garbo (2026-05-24/25) all surfaced the same pattern:

| Run | mount.nfs4 failed on | force-drain hits | tcpdump |
|-----|----------------------|------------------|---------|
| 1 | `/mnt/ps-6` | 17 containers (1 each: 7 PSes + 10 DSes; MDS clean, ps-7 never tried) | (not armed) |
| 2 | (SELinux blocked patched harness) | 8 (DSes only -- IOR never started) | not armed |
| 3 | `/mnt/ps-1` | 11 containers (1 each: 1 PS + 10 DSes) | armed but stale-file silent failure |
| 4 | `/mnt/ps-1` | 18 containers (1 each: 8 PSes + 10 DSes; mount.nfs4 -vvv + tcpdump captured) | 1294 pkts captured |

The IOR client reports the kernel mount call's errno:

    mount.nfs4: mount(2): Input/output error
    mount.nfs4: mount system call failed for /mnt/ps-1

The mount target shifted from `ps-6` (run 1) to `ps-1` (runs 3, 4)
-- different PS each time -- so the failure is NOT order-dependent
on a specific PS.  It's a per-connection race.

### tcpdump pinned the cause

Run 4's pcap (`/tmp/reffs-bench-pcap.pcap`, 182 KiB, 1294 packets)
captured the failing port-4099 (ps-1) handshake in full.  The
critical sequence on the FIRST connection from the kernel client
(port 940) to the PS (port 4099):

```
09:12:04.836611  TCP SYN     client -> PS
09:12:04.836645  TCP SYN/ACK PS -> client
09:12:04.836672  TCP ACK     client -> PS               (handshake done)
09:12:04.836731  client -> PS    44 bytes  (RPC NULL probe)
09:12:04.837448  PS -> client    28 bytes  (NULL reply)
09:12:04 .. 09:14:29             keepalive ACKs only    (~2 min 25 sec silence)
09:14:29.056149  client -> PS   252 bytes  (EXCHANGE_ID --
                                            payload contains
                                            "Linux NFSv4.2 garbo"
                                            in eia_client_owner.co_owner_id)
09:14:29.056307  PS -> client   FIN                     <-- PS closed
09:14:29.056402  client -> PS   FIN/ACK
                                                        (kernel then opens
                                                         a fresh TCP conn
                                                         from port 924 and
                                                         the EXCHANGE_ID
                                                         eventually completes
                                                         -- but mount.nfs4
                                                         had already given
                                                         up and returned
                                                         EIO to userspace)
```

PS-1's reffsd log shows the close was the unified
`io_conn_check_timeouts` idle reaper firing:

```
[2026-05-25 16:13:05.277916124] [1:1] (io_conn_check_timeouts:1059):
    Connection fd=14 timed out (61 seconds inactive)
[2026-05-25 16:13:11.284481655] [1:1] (io_conn_check_timeouts:1026):
    Connection fd=14 stuck in CLOSING for 6 seconds
    (counts: r=1 w=0 a=0 c=0, write_active=0); force-draining
```

The fingerprint is `r=1` on every container (not `a=1` like Slice
1 caught) -- so the leak is on the **read-CQE-completion path**,
not the accept path.  Different specific bug, same code family
(a CQE-completion path early-returning without
`io_conn_remove_*_op`).

### Root cause: idle reaper too aggressive for NFS-server connections

The Linux kernel NFS client's RPC engine treats a connection as
long-lived: an established TCP connection is reused across the
multi-RPC mount handshake, lease renewals, and op pipelining.
Multi-second gaps between RPCs are normal -- especially during
mount-time when userspace `mount.nfs4` does its own work between
the NULL probe and EXCHANGE_ID.

Pre-Slice-1, the Linux io_uring heartbeat only inspected
`CONN_CONNECTED` slots and did NOT apply an idle-close deadline.
Slice 1 unified the two event loops onto `io_conn_check_timeouts`
to gain the `CONN_CLOSING` force-drain backstop.  Slice 2 exempted
listener sockets from idle-close (they were getting reaped too).
But CONNECTED-with-NFS-traffic was left subject to a 60-second
idle reaper that the io_uring path never had before.  That makes
Slice 1's unification, in the absence of further exemption, a
regression on the deployment platform.

The two symptoms (mount EIO + r=1 leak) share one root.  When the
idle reaper closes a CONNECTED slot:

- The close path leaves the in-flight READ SQE's counter
  un-decremented (this is Bug A predicted in the original
  Slice 2 NOT_NOW_BROWN_COW -- but on the **read** sub-path of
  the same code family, not the **accept** sub-path the design
  anticipated).  Hence `r=1` in the force-drain warning.
- The kernel client's RPC engine sees the server close mid-mount-
  handshake, surfaces `EIO` to the mount(2) syscall.  Hence the
  IOR-visible `mount(2): Input/output error`.

### Fix plan -- one slice, two parts

#### Slice 3a -- exempt CONNECTED-with-NFS-traffic from the idle reaper

The minimum fix that unblocks Track 2 `--n 8`.  Options
(non-exclusive):

1. **Raise the CONNECTED idle deadline** from 60s to something
   NFS-appropriate.  NFSv4.1 default lease is 90s and clients
   expect the connection to outlive lease renewal cycles; a
   5--10 minute deadline (300--600s) is consistent with kernel
   NFS server behaviour, which typically does not idle-close at
   all.
2. **Track TCP-level keepalive activity** as "not idle" instead
   of only counting application-level reads/writes.  A quiet-but-
   ACK'd connection is healthy; closing it is wrong.
3. **Exempt CONNECTED slots from idle-close entirely** when they
   carry an NFSv4 session-bearing protocol -- match what kqueue
   was doing before Slice 1 unified the timeouts.  Cleanest, but
   reintroduces the per-loop divergence Slice 1 was trying to
   eliminate.

Option (1) is the smallest patch and matches the principle of
least surprise vs the kernel server.  Option (2) is more correct
but requires plumbing `tcp_info`-style keepalive observation
into `ci_last_activity`.

#### Slice 3b -- fix the read-counter leak at source

Independent of the timeout policy, the read-CQE-completion path
needs to pair `io_conn_add_read_op` with `io_conn_remove_read_op`
on every exit.  Audit every `io_conn_add_read_op` site for a
matching `remove` on its CQE-completion path (cancellation /
error / short-read branches).  This is the read-path twin of the
accept-path audit the original Slice-2 NOT_NOW_BROWN_COW scoped.

The force-drain backstop catches the leak after 5s either way, so
Slice 3b is a correctness-and-hygiene fix rather than an
unblocker.  Land it in the same slice as 3a since the leak is
how we see Slice 3a's symptom in the harness.

### Tests (first)

| Test | Where | Intent |
|------|-------|--------|
| `test_connected_idle_not_reaped_under_minute` | `lib/io/tests/conn_info_test.c` | Register a CONN_CONNECTED slot with a recent ci_last_activity; run `io_conn_check_timeouts` with the SAME idle deadline as production (whatever Slice 3a settles on); assert the slot is NOT closed.  This is the regression guard against Slice 1's unification re-introducing the aggressive reap. |
| `test_connected_idle_reaped_after_real_deadline` | same | Same as above but advance time past the (post-Slice-3a) production idle deadline; assert the slot IS closed.  Confirms the deadline still works, just at the right magnitude. |
| `test_read_op_remove_paired_on_cancel` | same | Submit a read op, cancel it; observe the CQE-cancel path; assert `ci_read_count` returns to zero.  Reproduces the r=1 leak the bench surfaced. |
| `test_read_op_remove_paired_on_close` | same | Submit a read op, close the connection synchronously (not via timeout); assert `ci_read_count` returns to zero. |

### Acceptance

The BLOCKER is re-cleared when:

3. Slice 3a + 3b land: `make check` green; the four new unit
   tests pass; Track 2 `--n 8` on a Linux bench host completes
   with **zero** mount.nfs4 EIO failures AND **zero**
   `stuck in CLOSING` force-drain warnings (Criterion 4 of the
   harness, commit `969e3f9c514b`).

The harness Criterion 4 (gating on zero force-drain warnings)
becomes the standing acceptance gate going forward.  If a future
slice somewhere else in the io_conn lifecycle introduces another
leak -- on the write path, the connect path, or back on the
accept path -- it will surface as a force-drain warning, the
Criterion 4 acceptance will fail, and the BLOCKER will reopen
again under whatever sub-path the data indicates.  The pattern
of "force-drain warning => reopen => add the missing
`io_conn_remove_*_op` audit for that sub-path" is the durable
shape.
