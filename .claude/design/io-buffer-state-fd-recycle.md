<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Buffer-state FD Recycle Race (BLOCKER)

## Revision history

- **2026-05-26 (initial draft)**: written against the bench symptoms.
- **2026-05-26 (revision 1)**: design-stage reviewer pass identified
  three BLOCKERs and six WARNINGs:
  - BLOCKER-1: eager `ci_bs` alloc at register breaks the
    TLS-ClientHello discriminant at `handlers.c:1471`. **Fixed**
    by switching to pointer + lazy alloc preserved.
  - BLOCKER-2: TDD step 1 "RED tests" wouldn't compile against
    the pre-fix tree. **Fixed** by reframing the slice as
    "single commit, build broken between steps 1 and 6".
  - BLOCKER-3: cited `lib/fs/tests/inode_alloc_test.c`
    malloc-fail pattern doesn't exist. **Fixed** by replacing
    `test_bs_freed_on_register_failure` with
    `test_bs_lifecycle_thread_race` modelled on
    `conn_lifecycle_race_test.c::test_no_race_churn_vs_readers`.
  - WARN-1..WARN-6 + NOTEs: incorporated inline (call-site
    table corrected, Hazard B TCP-shutdown limb acknowledged
    and deferred, free-ordering revised, inline-bs alternative
    added to alternatives-considered, verbatim LOG quote fixed).

## Context

Surfaced by chunk-collision Track 2 N=8 fio bench on 2026-05-26
(garbo). After the conn-info-closing-wedge BLOCKER closed
(7b17ec5b944f / e8e28b4f033c / 33e9c6ddfd02), the first bench run
that drove **real** client-visible I/O through the
kernel-NFS → PS → MDS/DS stack hit a different failure surface:

```
[18:38:46.219205] io_buffer_state_create:194:
  io: conn_buffers alias: fd=11 slot=11 already occupied
  -- increase MAX_CONNECTIONS or use a hash map
```

This is on `reffs-bench-ds9`. Five other DSes hit a similar pattern
(`reffs-bench-ds{0,3,5,7}` plus ds9), all clustered at 18:38:46-52
UTC during the bench's teardown / verify phase. 6 of the conn-info
slots ended up in `CONN_CLOSING` and were force-drained by the
heartbeat reaper 6 seconds later — Criterion 4 of Track 2 went red.

This document is for the second BLOCKER, not a regression of the
first. The conn-info wedge fix did its job:

- `shutdown(SHUT_RDWR)` propagated TCP close to pending io_uring
  read SQEs. Confirmed: every flagged fd shows `CQE error for
  op=READ ... Connection reset by peer` immediately after the close
  — the kernel CQE'd the pending SQE as designed.
- `io_socket_close()` is the close chokepoint. `io_client_fd_unregister(fd)`
  runs before `close(fd)`. No path bypasses this in the normal close
  flow.

The bug is one layer up: the `io_buffer_state` lifecycle is **not
synchronised** with the conn_info lifecycle, and the relevant
`conn_buffers[]` slot can be left stale through a window the
kernel can fill with a fresh accept.

## Why prior bench runs were "clean"

The two pre-fio Track 2 runs (the 5-hour dwell and the 50-minute
dwell that closed the conn-info-closing-wedge BLOCKER) used the
IOR-via-MPI harness, which wedged in `MPI_Init` / `MPI_Comm_split_type`
for the full bench window — they hit the **same** 601-second idle
close on the MDS-to-DS sessions, but because no new accepts came in
afterwards (the bench was wedged at the MPI layer, not yet pushing
I/O), there was nothing to collide with the leaked
`conn_buffers[]` slot. The slot leak was silent. fio's much faster
path to real I/O is what surfaced this.

The earlier closure of the conn-info wedge BLOCKER is still valid
on its own terms (it eliminated the structural `r=1` stranding
caused by `close(fd)` not propagating to io_uring SQEs); it just
didn't catch this second bug because the harness never reached the
code path that triggers it.

## Symptom Chain

1. **18:18 UTC**: bench bring-up; MDS-to-DS sessions establish on each
   DS at fd=11 (sometimes fd=12 for a second session).
2. **18:18 - 18:28**: bench setup (fio install via `dnf`, mount setup,
   first iter starts writing). Slow enough that the MDS-to-DS
   sessions sit idle without SEQUENCE heartbeats.
3. **18:28 (T+10min)**: `io_conn_check_timeouts` 601-second idle
   reaper fires on each DS: `Connection fd=11 timed out (601
   seconds inactive)` -> `io_socket_close(11)`.
4. **18:28 - 18:38 (T+10min to T+20min)**: bench writes happen.
   Each PS opens new MDS-to-DS connections, getting fresh fd
   assignments from the kernel (sometimes recycling fd=11/12).
5. **18:38:46**: bench teardown / verify phase begins; kernel
   NFS client closes its sessions; cascading `Connection reset by
   peer` CQEs across all DSes.
6. **Within ~20ms of 18:38:46**: an accept gets `client_fd=11`,
   `io_buffer_state_create(11)` finds `conn_buffers[11]` already
   non-NULL -> registration fails -> "Failed to register client
   connection fd=11" -> kernel NFS client sees `EREMOTEIO` -> fio
   verify-phase `fstat` returns `Remote I/O error`.
7. **6 seconds later**: the heartbeat reaper sees the wedged
   conn-info slot still in `CONN_CLOSING` and force-drains it;
   logged as the Criterion-4 failure we caught.

## Root Cause

`lib/io/net_state.c` keeps a parallel data structure
(`conn_buffers[MAX_CONNECTIONS]`, indexed by `fd % MAX_CONNECTIONS`)
that is not coordinated with `connections[]` (the `conn_info`
table in `lib/io/conn_info.c`). Specifically:

| Operation | conn_info side | buffer_state side |
|-----------|----------------|-------------------|
| Allocate | `io_conn_register()` under `conn_mutex` | `io_buffer_state_create()` outside any lock |
| Release | `io_conn_unregister()` sets `CONN_CLOSING`; slot reclaimed in `conn_drain_if_idle_locked` once counters drain | `io_client_fd_unregister()` frees and NULLs immediately, outside any lock |
| Reuse gate | INV-6: `io_conn_register` refuses to recycle a `CONN_CLOSING` slot | None: `io_buffer_state_create` LOG-and-fails on alias |
| Lock | `conn_mutex` | none |

The asymmetry creates two distinct hazards under a fd-recycle
sequence:

### Hazard A: Allocate-race against stale free

- Thread X closes old fd=11: enters `io_socket_close(11)`.
  Sequence inside: `io_conn_unregister(11)` (slot enters `CONN_CLOSING`
  under `conn_mutex`) -> `io_client_fd_unregister(11)` (frees
  `conn_buffers[11]` outside lock) -> `shutdown(11)` ->
  `close(11)` (fd released to kernel).
- Thread Y, between steps 2 and 4, takes a CQE-error-driven path
  for a *different* fd, or a heartbeat tick, that under load can
  delay X's `io_client_fd_unregister(11)`.
- Some other path's accept arrives, kernel reuses fd=11 (only
  possible after X's `close(11)` returns — but see Hazard B).
- New accept's `io_conn_register(11)` succeeds because by then the
  CLOSING-slot has drained (counters at 0). Slot is reset to
  `CONN_ACCEPTED`, fresh state.
- New accept's `io_client_fd_register(11)` -> `io_buffer_state_create(11)`
  sees the old `conn_buffers[11]` was NULL'd by X — so this path is
  safe in isolation.

**Hazard A precise wording**: if the bs free in
`io_client_fd_unregister` completes before the new accept's bs
create starts, the path is safe. Today there is **nothing that
enforces this ordering** — `io_client_fd_unregister` is outside
`conn_mutex`, and the new accept's `io_buffer_state_create` is
also outside `conn_mutex`. The window is small in the common
case but is fundamentally unbounded under load.

### Hazard B: Double-CQE during close

Looking at the ds9 log:

```
.199360  CQE error for op=READ,  fd=11: Connection reset by peer
.218798  CQE error for op=READ,  fd=11: Connection reset by peer
.219046  CQE error for op=WRITE, fd=11: Bad file descriptor
.219205  io_buffer_state_create: fd=11 slot=11 already occupied
```

Two read SQEs were in flight for fd=11 when the peer reset.
**Both** CQE-error handlers call `io_socket_close(11)`:

- 1st: `io_conn_get(11)` -> non-NULL (CONNECTED), proceeds full path,
  ends with `close(11)`. fd=11 returned to kernel pool.
- 2nd (~19ms later, same fd=11): `io_conn_get(11)` -> NULL (slot
  is now CLOSING and `io_conn_get` filters by `ci_fd == fd && state
  != CLOSING`). Skips `io_conn_unregister`. Calls
  `io_client_fd_unregister(11)` -> sees `conn_buffers[11]` is NULL
  (1st close NULL'd it), no-op. Calls `shutdown(11)` ->
  `close(11)`.

**Critical:** between 1st close's `close(11)` returning and the 2nd
close's `close(11)`, the kernel can re-issue fd=11 to a new accept
SQE that completes on the same fd number. If the new accept
completes between the 1st close and the 2nd close:

- New accept's `io_conn_register(11)` under `conn_mutex` checks
  INV-6. By this point the OLD conn_info slot was already
  transitioned out of CLOSING (counters drained via the .199 CQE),
  to `CONN_UNUSED` with `ci_fd = -1`. INV-6 passes; fresh slot
  allocated.
- New accept's `io_client_fd_register(11)` -> `io_buffer_state_create(11)`
  -> allocates fresh `bs`, sets `conn_buffers[11] = bs`. **Slot is
  now NEW.**
- 2nd close (still in flight for *old* fd=11) reaches
  `io_client_fd_unregister(11)`: `io_buffer_state_get(11)` returns
  the *new* `bs`, frees it, sets `conn_buffers[11] = NULL`. **The
  new connection's buffer state is now gone** — its next CQE will
  fail to read into a non-existent buffer.
- Or, in the opposite ordering: 2nd close's
  `io_client_fd_unregister(11)` runs first (frees old bs, NULLs
  slot), then the new accept's `io_buffer_state_create(11)`
  succeeds. No alias log line — but a kind of silent corruption
  the next time the 2nd close calls `shutdown(11)` / `close(11)`
  on a fd that's now bound to a different file in the kernel.

The "already occupied" log line is one window of this race; a
quieter "wrong buffer state for the wrong connection" is the
worse window.

**Hazard B has a TCP-shutdown limb this fix does NOT address.**
If the second `io_socket_close(11)` runs on a recycled fd
(option (b) in the race above — kernel handed fd=11 to a new
accept after the 1st close returned), the second close's
`shutdown(11, SHUT_RDWR)` tears down the **new** connection's
TCP and the `close(11)` releases its fd, from under whichever
thread was driving the new conn. The fold-in proposed here
fixes only the buffer_state slice of Hazard B; the
duplicate-close-of-recycled-fd slice requires an orthogonal
"close-once" gate per conn_info (e.g., a CAS-protected
`ci_close_done` flag set under `conn_mutex` inside
`io_socket_close`). Tracked as a NOT_NOW_BROWN_COW (see Deferred
section item 5).

### Hazard C: Bypass paths skipping `io_client_fd_unregister`

`io_handle_accept` (handlers.c:522) does:

```c
struct conn_info *client_conn =
    io_conn_register(client_fd, CONN_ACCEPTED, CONN_ROLE_SERVER);
if (!client_conn) {
    LOG("Failed to register client connection fd=%d", client_fd);
    io_socket_close(client_fd, ENOMEM);   /* OK: this path frees */
    io_context_destroy(ic);
    return ENOMEM;
}
/* ... */
io_client_fd_register(client_fd);         /* allocates bs */

io_request_read_op(client_fd, &ic->ic_ci, rc);  /* RV ignored */
```

`io_request_read_op` ignores its return; if it fails (rare but
possible: SQE submission OOM, ring full under burst), the bs
allocated at line 615 is never freed because no further close
fires until the next idle reaper sweep (601s later). This is a
slow leak rather than a race, but lives in the same code surface
and any restructure must close it.

## Proposed Fix: bs pointer in conn_info, lazy-alloc, freed at drain

The cleanest fix is to **add a `struct buffer_state *ci_bs`
field to `struct conn_info`** and migrate its lifecycle into the
conn-info side, which is already correctly gated by `conn_mutex`
+ INV-6.

The fix keeps the existing **lazy allocation** semantics
(allocate on first plain-NFS read, not at register time —
critical for the TLS-ClientHello detection at
`handlers.c:1471` — see note below). It changes only the
**free** side: instead of `io_client_fd_unregister` freeing
outside any lock at close time, the bs is freed at the
`CONN_CLOSING → CONN_UNUSED` transition inside
`conn_drain_if_idle_locked`, under `conn_mutex`.

### Why lazy allocation must be preserved

`lib/io/handlers.c:1471` reads:

```c
bs = io_buffer_state_get(client_fd);
if (!bs && is_tls_client_hello(ic->ic_buffer, bytes_read)) {
    /* TLS path: handshake first, no plain-NFS bs allocated */
    ret = handle_tls_handshake(...);
    ...
}

if (!bs)
    bs = io_buffer_state_create(client_fd);
```

The `!bs` test discriminates "first read on a fresh
connection" from "this connection is already carrying RPC
traffic" — only the first state is safe to run the
ClientHello probe on, because the probe matches against the
first 5 bytes of the kernel's TLS record header and would
false-positive on payload bytes that match. **Pre-allocating
`ci_bs` at `io_conn_register` time would make `!bs` always
false, silently regressing TLS-on-mixed-port detection.** The
fix preserves the `ci_bs == NULL` discriminant.

### State diagram (after fix)

```
io_conn_register(fd)             ci_bs = NULL (no alloc yet)
   |   under conn_mutex
   v
CONN_ACCEPTED / CONN_CONNECTED
   |
   v  (first plain-NFS read, via handlers.c:1471)
io_buffer_state_create(fd)       ci_bs = malloc(...); bs_data alloc'd
   |   under conn_mutex          (or lazy-init via a locked wrapper)
   v
[normal read/write traffic; ci_bs lives across CQEs]
   |
   v  (CQE error or idle timeout)
io_socket_close(fd)
   |
   |--> io_conn_unregister(fd) under conn_mutex
   |       |  (drain pending writes, detach SSL, set CONN_CLOSING)
   |       v
   |    CONN_CLOSING  (in-flight CQEs still find this slot;
   |                   ci_bs still readable for stale CQEs to
   |                   land in via the *_locked accessors)
   |
   |--> shutdown(fd, SHUT_RDWR)
   |--> close(fd)
   .
   . (CQE drain happens asynchronously, decrementing counters)
   .
io_conn_remove_read_op() / io_conn_remove_write_op()
   |   under conn_mutex
   v
conn_drain_if_idle_locked()
   |   counters all 0, write_active=false; under conn_mutex
   |   1. ci_state = CONN_UNUSED
   |   2. ci_fd = -1               <-- publish UNUSED first
   |   3. free(ci_bs->bs_data); free(ci_bs->bs_record.rs_data);
   |      free(ci_bs); ci_bs = NULL    (or NULL_after_lock-free)
   v
CONN_UNUSED  (slot reusable; bs gone)
   v
io_conn_register(new_fd_same_slot) succeeds  // INV-6 gate clears
   v
ci_bs = NULL (fresh conn, no alloc yet -- TLS probe path armed again)
```

### Free-ordering rationale

The `CONN_UNUSED / ci_fd = -1` writes happen **before** the bs
free, even though the entire transition is under
`conn_mutex`. The reason: a late `io_conn_remove_read_op(fd)`
arriving with a stale fd matches via `ci_fd == fd` and would
access `ci_bs` while decrementing its counter. If the free
happened first and the `ci_fd = -1` publish second, a racing
CQE handler on the same fd that took the lock between the free
and the publish would dereference freed memory. With the
ordering reversed — publish UNUSED and `ci_fd = -1` first, then
free `ci_bs` — once the lock is released, no fd-keyed lookup
matches this slot, so the bs is logically unreachable and the
free under the same lock-held window is race-free.

### Why this is correct

- **No parallel data structure.** `conn_buffers[]` array goes away.
  `io_buffer_state_get(fd)` becomes a thin wrapper that fetches
  `&connections[fd % MAX_CONNECTIONS]->ci_bs`, under `conn_mutex`.
  Or callers route through accessors that take the same lock.
- **No alias possible.** The slot table is now single-keyed; you
  cannot have a stale `ci_bs` "leak" past `CONN_UNUSED` because
  that transition is the one that frees it.
- **INV-6 already covers the gate.** A `CONN_CLOSING` slot cannot
  be reused for a new accept — `io_conn_register` refuses it.
  Anything that touches `ci_bs` after `CONN_CLOSING` is by
  definition a stale CQE for the old connection, and the existing
  `*_locked` accessors handle that correctly (they check `ci_fd ==
  fd` first).
- **Hazard C goes away.** Even if `io_request_read_op` fails
  silently after `io_handle_accept`, the next close (idle reaper
  or explicit) frees the bs as part of the conn-info teardown.

### What the call sites become

| Caller | Before | After |
|--------|--------|-------|
| `io_handle_accept` (`handlers.c:522`) | `io_conn_register` then `io_client_fd_register` (bs alloc'd eagerly) | `io_conn_register` only; bs stays NULL until first read |
| `io_handle_connect` (`handlers.c:635`) | `io_conn_register` only (bs was never alloc'd here — client-side connects don't pre-alloc bs; it's lazy-created on first read via the read path at `handlers.c:1471`) | unchanged — `io_conn_register` only; bs still lazy-created on first read |
| Read path (`handlers.c:1471`) | `io_buffer_state_get` then optional `io_buffer_state_create` (the latter outside `conn_mutex`) | `io_buffer_state_get_locked` (or equivalent) and `io_buffer_state_create_locked`, both taking `conn_mutex` and operating on `ci->ci_bs`. `!bs` discriminant for the TLS-ClientHello probe is preserved. |
| `io_socket_close` (`conn_info.c:819`) | `io_conn_unregister` then `io_client_fd_unregister` (frees bs outside lock) then `shutdown` + `close` | `io_conn_unregister` then `shutdown` + `close`; bs freed lazily by `conn_drain_if_idle_locked` once all CQE counters drain |
| `io_buffer_state_get(fd)` | direct read of `conn_buffers[fd % MAX_CONNECTIONS]` | takes `conn_mutex`; loads `ci->ci_bs` iff `ci->ci_fd == fd && ci->ci_state != CONN_CLOSING`; unlock |

`io_buffer_state_get()` callers will pay an extra `conn_mutex`
acquire — but they already pay one in surrounding `io_conn_*`
calls, and the receive-buffer path is not on the lock-contended
hot path (it is per-fd, per-record). Hot paths can take the lock
once and operate on a pointer captured under it (caveat: must
re-check before re-using, same rule as existing INV-6 accessors).

### Alternative considered: inline `struct buffer_state` directly into `struct conn_info`

Rather than `struct buffer_state *ci_bs` with lazy alloc, put
the entire `struct buffer_state` (head, ~50 bytes) inline. Only
`bs_data` (8 KiB capacity) and `bs_record.rs_data` (variable)
would still need heap allocation.

Rejected for two reasons:

1. **Memory cost**: `MAX_CONNECTIONS = 65536` × 50 bytes = 3.2 MiB
   of always-resident `struct buffer_state` headers, regardless
   of live-connection count. Embedded ports and small VMs care
   about this; the per-conn_info struct is already ~200 bytes.
2. **TLS detection**: the `!bs` discriminant at
   `handlers.c:1471` becomes `!bs_data` instead of `!bs`, which
   is a different flag (the head exists from process start;
   data is what's lazily allocated). Working but more
   error-prone — a developer maintaining the read path would
   need to remember that `bs_data == NULL` means "fresh, run
   TLS probe", rather than the more natural "bs == NULL" of the
   pointer variant. The pointer variant keeps the existing
   semantics intact.

The pointer variant adds one heap alloc per connection (which
happens today anyway — `io_buffer_state_create` already
mallocs both `buffer_state` and `bs_data` separately) and no
memory cost at zero live connections. Picking the pointer.

### Alternative considered: hash map (per the FIXME hint)

The error message says "increase MAX_CONNECTIONS or use a hash
map". A hash map fixes Hazard A's alias collision but **does not
fix Hazard B or C** — both are lifecycle bugs (when to free,
when to allocate) that exist independently of how slots are
keyed. Hazard B in particular would be silently worse with a
hash map, because the alias-on-create log line is the only
in-tree signal we currently have. Rejecting this alternative.

### Alternative considered: add a separate `buffer_state_mutex`

Add per-slot locking on `conn_buffers[]` independent of
`conn_mutex`. This works for Hazards A and B but doubles the
lock-acquire count on every read CQE (every fragment-reassembly
step does at least one `io_buffer_state_get`). Folding the bs
into `conn_info` keeps the lock count unchanged. Rejecting this
alternative.

## Persistence

None. `struct buffer_state` is per-connection RAM only, lost on
process exit, never written to disk. The fix does not change any
on-disk format and does not bump any version field.

## Security model

No external attack surface change. `conn_buffers[]` is internal
state never serialised; failing-closed when slot allocation fails
is preserved (the new path returns NULL from `io_conn_register`,
same as today, and `io_handle_accept` already closes the fd).

A side benefit: the silent-corruption window in Hazard B (2nd
close freeing the new connection's bs) goes away. That window has
no known exploit but it's a confused-deputy waiting to happen,
and folding bs into conn_info eliminates it structurally.

## Test impact analysis

### Existing tests affected

| File | Impact | Reason |
|------|--------|--------|
| `lib/io/tests/conn_info_test.c` | **EXTEND** | New conn-info struct field; existing 27 tests still pass because they don't directly poke `conn_buffers[]`. Add tests for the new fold-in (see below). |
| Any `lib/io/tests/buffer_state_test.c` if it exists | UPDATE accessor calls | `io_buffer_state_get` signature unchanged but now lock-aware. |
| `make check` (libcheck on macOS+ASAN) | ASAN HANG: pre-existing environmental issue documented in `feedback_check_stub_state_before_demos`; run lib/io/tests on non-ASAN build for verification. | |

No on-disk-format tests, no XDR tests, no RFC-compliance tests
are affected (this is internal to lib/io).

### New unit tests

**File**: `lib/io/tests/conn_info_test.c` (extend, +5 tests; brings total
from 27 to 32). These tests reference the **post-fold** API
(`ci_bs` on `conn_info`, lazy-created via the lock-aware accessor) —
they will not compile against the pre-fix tree (no
`io_buffer_state_create_locked` exists today). They land in the
same slice as the implementation, with the build broken between
step 1 (tests in) and step 5 (call sites converted). The reffs
codebase already uses this single-slice TDD pattern (see e.g.
`lib/backends/tests/Makefile.am`-conditional `rocksdb_test.c`).

| Test | Intent |
|------|--------|
| `test_bs_null_after_register` | After `io_conn_register(fd, CONN_ACCEPTED, ROLE_SERVER)`, `io_buffer_state_get(fd)` returns NULL — lazy allocation preserved (regression guard for the TLS-ClientHello discriminant at `handlers.c:1471`). |
| `test_bs_freed_on_drain_to_unused` | Register fd; lazy-create a bs via `io_buffer_state_create(fd)`; increment then decrement read_count via add/remove_read_op; verify slot transitions to CONN_UNUSED and `io_buffer_state_get(fd)` returns NULL post-drain. |
| `test_bs_survives_closing` | Register fd; create bs; add_read_op (count=1); io_conn_unregister (-> CONN_CLOSING); verify the **`_locked`-prefixed** accessor used by CQE handlers (the one that operates while holding `conn_mutex`) still returns the same bs while in CLOSING. |
| `test_bs_no_alias_on_recycle` | Register fd=N; create bs; transition to CONN_UNUSED via drain; re-register fd=N; create a new bs. Verify the second bs is freshly initialised (bs_filled==0, bs_capacity==2*BUFFER_SIZE) and `io_buffer_state_get` post-second-register returns the new bs, not the (freed) old one. Sequential single-thread test — exercises the post-fix invariant, not the race. |
| `test_bs_lifecycle_thread_race` | Multi-thread regression test using the pattern in `lib/io/tests/conn_lifecycle_race_test.c::test_no_race_churn_vs_readers`: spawn 2 threads. Thread A repeatedly drives `io_conn_register(N) → io_buffer_state_create(N) → io_conn_unregister(N) → drain (decrement counters) → repeat` on fd=N. Thread B repeatedly drives `io_conn_remove_read_op(N) → io_buffer_state_get_locked(N)` and asserts that whatever bs is returned (NULL or non-NULL) is consistent (no UAF, no torn pointer). Run for 10k iterations under ASAN/TSAN. This is the regression guard for Hazard A and Hazard B's buffer-state slice. |

### Functional test

**Reuse** the chunk-collision Track 2 fio harness (already shipped
in 0eefc3de589b). After the fix, re-run on garbo:

```
bash deploy/benchmark/run_chunk_collision_track2.sh --n 8
```

Pass criteria:
- Criterion 1 (fio verify): no `EREMOTEIO` on verify-phase fstat
- Criterion 3 (sanitizers): unchanged, must remain clean
- **Criterion 4 (force-drain): must be 0 across all DSes and PSes**
- Bonus: the `io_buffer_state_create: ... slot=N already occupied`
  LOG line must NOT appear in any container's docker logs.

### CI integration

The slow MDS-to-DS idle close (601s) is a precondition for the
race in the bench, but the unit tests above exercise the
lifecycle without needing a 10-minute wait. CI: `ci-full` is
sufficient. No new CI integration harness needed.

### Test infrastructure

`test_bs_lifecycle_thread_race` uses the same template as
`lib/io/tests/conn_lifecycle_race_test.c::test_no_race_churn_vs_readers`
— spawn worker threads via the existing test harness's
`pthread_create` pattern, churn the lifecycle, run under
ASAN/TSAN as part of the standard test build. No new test
infrastructure required.

**Earlier draft proposed a malloc-fail-injection counter**
(`io_test_malloc_fail_after_N`, gated by `#ifdef REFFS_TESTING`)
patterned after a hypothetical existing test in
`lib/fs/tests/inode_alloc_test.c`. **That file does not exist
in the tree** (verified by reviewer); no such injection
pattern is established elsewhere. The proposed
`test_bs_freed_on_register_failure` has been replaced by the
multi-thread `test_bs_lifecycle_thread_race` above, which
exercises the same lifecycle correctness without needing
malloc-fail injection. Any future malloc-fail injection work
is out of scope for this slice.

## Deferred / NOT_NOW_BROWN_COW items

1. **MDS-to-DS session keep-alive.** The bench's 601-second idle
   close was the *trigger* for surfacing this bug — NFSv4.2
   sessions should send SEQUENCE periodically to keep the
   underlying TCP connection alive. Fixing the session-level
   keep-alive would have masked the buffer_state race today.
   Track separately as a follow-up.
2. **Hash-map keying for `conn_info`/`conn_buffers`.** The
   FIXME-marked alternative; not needed once buffer_state is
   folded into conn_info, since the (fd → slot) collision
   surface is already addressed by INV-6.
3. **`io_request_read_op` return-value handling in
   `io_handle_accept`.** Hazard C goes away with this fix
   (drain-time free), but the bare `io_request_read_op(...)`
   call without RV check is still a code smell. Track as a
   follow-up cleanup, not a BLOCKER.
4. **Heartbeat-thread vs CQE-thread concurrency audit.** A
   sibling audit of every fd-keyed accessor outside `lib/io/`
   to confirm none assumes a NULL `conn_buffers[]` slot means
   "no conn" rather than "no bs". Defer until the fold-in
   lands and the call sites are reduced to one accessor.
5. **`io_socket_close` close-once gate (Hazard B's TCP slice).**
   This design fixes only the buffer_state slice of Hazard B.
   The duplicate-close-of-recycled-fd slice (where a second
   `io_socket_close(fd)` after fd recycling tears down the new
   connection's TCP via `shutdown(fd, SHUT_RDWR)` and `close(fd)`)
   requires a per-conn_info close-once gate. The natural shape
   is a CAS-protected `ci_close_done` flag under `conn_mutex`
   set inside `io_socket_close` after `io_conn_unregister`
   succeeds; a second call with `ci_close_done == true` skips
   the `shutdown` + `close`. Deferred as a follow-up BLOCKER
   slice in its own right.

## Admin interface

None — internal to `lib/io/`. No probe op added. No CLI added.

## Implementation order

The whole slice lands in a single commit — tests reference
post-fix API so they cannot land before the implementation
without breaking the build.

1. **TDD: write the 5 new unit tests** against the post-fold
   API (`ci_bs` on `struct conn_info`; `io_buffer_state_get`
   that reads `connections[idx]->ci_bs` under `conn_mutex`).
   The tests will not compile against the pre-fix tree (no
   `ci_bs` field exists yet); they go in alongside steps 2–6
   in the same TDD slice. The build stays broken between the
   beginning of step 1 and the end of step 6.
2. **Add `struct buffer_state *ci_bs` field** to
   `struct conn_info`. Initialise to NULL in
   `io_conn_register`. Do **not** allocate the bs head at
   register time — lazy allocation on first read is required
   to preserve the TLS-ClientHello discriminant at
   `handlers.c:1471` (see "Why lazy allocation must be
   preserved" above).
3. **Replace `io_buffer_state_get(fd)`** with a wrapper that
   takes `conn_mutex` and reads `connections[idx]->ci_bs`
   iff `ci->ci_fd == fd && ci->ci_state != CONN_CLOSING` (the
   CLOSING filter mirrors `io_conn_get`). Add a `_locked`
   variant for the read-handler / CQE-handler paths that
   already hold `conn_mutex`.
4. **Replace `io_buffer_state_create(fd)`** with a wrapper
   that takes `conn_mutex`, allocates the `struct buffer_state`
   and `bs_data`, stores into `connections[idx]->ci_bs`, and
   returns it. Refuse (return NULL) if `ci_state == CONN_CLOSING`.
5. **Wire free** into `conn_drain_if_idle_locked`,
   **after** the existing `ci_state = CONN_UNUSED;
   ci_fd = -1;` writes. The order matters — see "Free-ordering
   rationale" above. Under the same `conn_mutex` acquisition,
   free `ci_bs->bs_data`, free `ci_bs->bs_record.rs_data` if
   non-NULL, free `ci_bs`, NULL the pointer.
6. **Delete `conn_buffers[]`**, `io_client_fd_register`,
   `io_client_fd_unregister`. Move the per-bs free out of
   `io_net_state_fini` into `io_conn_cleanup` at
   `conn_info.c:794` (which already iterates `connections[]`
   at process shutdown and frees each — the bs free becomes
   a natural extension of that loop). Verify the call order in
   `io_handler_fini`: `io_conn_cleanup` must run before
   `io_net_state_fini`, since after the cleanup
   `io_net_state_fini` only has the pending-requests table to
   handle. If the order today is reversed, fix it in this
   slice.
7. **Run `make check`** on the non-ASAN build — must remain
   green. (libcheck+ASAN on macOS hangs; documented
   environmental issue, run lib/io tests on the non-ASAN build
   for verification.)
8. **Re-run Track 2 N=8 fio on garbo** via the existing harness
   (`deploy/benchmark/run_chunk_collision_track2.sh --n 8`).
   Criterion 4 must be 0. Criteria 1 and 3 must still pass.
   The `io: conn_buffers alias: ...` LOG line (verbatim from
   `net_state.c:192-194` — the actual text is `io: conn_buffers
   alias: fd=%d slot=%d already occupied`) must not appear in
   any container's docker logs.
9. **Reviewer gate.** This is a ref-counting / lifecycle change
   touching `lib/io/`; per `.claude/CLAUDE.md` it is an RCU /
   ref-counting lifecycle slice. Run the reviewer agent on the
   code diff before commit.

## RFC References

None — this is internal io_uring + TCP plumbing. The conn-info
state machine documented at the top of `lib/io/conn_info.c`
already covers the user-facing semantics this builds on. No
wire-format change.

## Key Files

| File | Change |
|------|--------|
| `lib/io/conn_info.c` | Add `ci_bs` field handling in register / drain / accessors; remove `conn_buffers[]` references in `io_socket_close`. |
| `lib/io/net_state.c` | Remove `conn_buffers[]`, `io_buffer_state_create`, `io_buffer_state_get` (or rewrite as wrapper), `io_client_fd_register`, `io_client_fd_unregister`, the `io_net_state_fini` per-fd bs free loop. |
| `lib/include/reffs/io.h` | `struct conn_info` gains a `struct buffer_state ci_bs` (or `struct buffer_state *ci_bs` with lazy alloc). |
| `lib/io/handler.c`, `lib/io/handlers.c` | Drop `io_client_fd_register` / `io_client_fd_unregister` call sites. |
| `lib/io/read.c`, `lib/io/handlers.c` | Update `io_buffer_state_get` call sites to use the lock-aware accessor (or accept that they already run under the appropriate lock window). |
| `lib/io/tests/conn_info_test.c` | +5 tests, total 32. |

## References

- Closing-wedge BLOCKER (the *first* bug, closed by 7b17ec5b944f
  / e8e28b4f033c / 33e9c6ddfd02):
  `.claude/design/conn-info-closing-wedge.md`
- Chunk-collision Track 2 harness (fio rewrite,
  0eefc3de589b): `deploy/benchmark/run_chunk_collision_track2.sh`
- Rule 6 hash-table-entry lifecycle:
  `.claude/patterns/ref-counting.md`
