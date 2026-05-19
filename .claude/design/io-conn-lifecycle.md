<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# lib/io Connection-Lifecycle Fix (INV-5 / INV-6)

## Context

Track 2 (chunk-collision, shared-file IOR through N Proxy Servers)
cannot produce data until the PS<->MDS RPC-over-TLS path is solid.
Track 2 surfaced two failures, characterised in
`.claude/design/experiments.md`:

- **INV-5**: concurrent PS->MDS mTLS session establishment races;
  the first RPC over a freshly-established TLS session fails EIO.
- **INV-6**: an established PS<->MDS TLS connection breaks ~4s into
  IOR write load; all PSes reconnect at once, hit INV-5, ~90s
  outage, IOR aborts.

The Stage 2 root-cause read and the Stage 1 reproducer
(`lib/io/tests/conn_lifecycle_race_test.c`, commit `8cd8149b50e1`)
proved INV-5 and INV-6 are **one defect class**: `struct conn_info`
has no lifecycle discipline.  TSAN on dreamer flagged two data
races on the `io_conn_register()`'d `conn_info` struct -- a
write/read race on `ci_ssl` and on `ci_tls_enabled`.

This document is the Stage 3 plan: the `lib/io/` fix that makes
`conn_info` SSL access memory-safe and race-free, so Track 2 can
re-run and finally produce chunk-collision data.

## Threading model (as read, 2026-05-19)

`lib/io/conn_info.c` is a flat `connections[fd % MAX_CONNECTIONS]`
array under one process-wide `conn_mutex`.  Three thread classes
touch it:

| Thread | Role | conn_info / ci_ssl interaction |
|--------|------|-------------------------------|
| Event loop | CQE/kevent dispatch -> `io_handle_read` / `io_handle_write` / `io_handle_accept` | Runs the TLS handshake, `SSL_read`, installs and (on error) frees `ci_ssl` |
| Worker pool | dequeues tasks, builds replies -> `io_rpc_trans_cb` -> `rpc_trans_writer` -> `io_do_tls` | `SSL_write` on `ci_ssl` (under the per-fd write gate) |
| Heartbeat | `io_conn_check_timeouts` -> `io_socket_close` -> `io_conn_unregister` | Frees `ci_ssl` on an idle/timed-out fd |

The per-fd write gate (`ci_write_active`, `ic_write_gen`) exists
*because* reply submission runs on worker threads -- confirmation
that `conn_info` is genuinely multi-threaded.

`conn_mutex` is process-wide: it must NOT be held across `SSL_read`
/ `SSL_write` / BIO work, or one connection's crypto serialises
every other connection's bookkeeping.

## The defect

`io_conn_get(fd)` returns a raw `struct conn_info *` and drops
`conn_mutex` before returning.  Every consumer then reads
lock-protected fields -- `ci_ssl`, `ci_tls_enabled`,
`ci_tls_handshaking` -- with no lock.  Meanwhile:

- `io_conn_unregister()` (heartbeat thread) does `SSL_shutdown` +
  `SSL_free` + `ci_ssl = NULL` under `conn_mutex`.
- `io_handle_read()` error paths (event loop) do
  `SSL_free(ci->ci_ssl); ci->ci_ssl = NULL;` with **no lock**
  (`handlers.c:1300, 1352, 1399`).
- `io_conn_destroy()` / `io_conn_cleanup()` also `SSL_free`.

A worker in `io_do_tls()` caches `ssl = ci->ci_ssl` (`handlers.c:102`)
and calls `SSL_write` on it; concurrently the event loop or the
heartbeat thread `SSL_free`s that exact object.  Result: a
use-after-free on the `SSL` -- the INV-6 mid-load disconnect -- and
a data race on the `ci_ssl` / `ci_tls_enabled` fields themselves
(what TSAN flagged).

The `conn_info` **struct** is stable: it is `malloc`'d once per fd
slot and never freed until `io_conn_cleanup()`.  Only the
**`ci_ssl` pointer and the TLS flag fields** need lifecycle
discipline.

`io_conn_get_peer_cert_fingerprint()` (conn_info.c:908) already
documents and dodges this hazard the right way -- it extracts the
X509 *under* `conn_mutex*.  That is the precedent the fix
generalises.

## Inventory: every `ci_ssl` / TLS-field site to convert

Frees (become `io_conn_ssl_clear`):
- `conn_info.c` `io_conn_unregister` :531-537
- `conn_info.c` `io_conn_destroy` :507-511
- `conn_info.c` `io_conn_cleanup` :608-612
- `handlers.c` `io_handle_read` error paths :1300-1301, :1352-1353,
  :1399-1400

Install (becomes `io_conn_ssl_install`):
- `handlers.c` `handle_tls_handshake` :1011

Uses (become `acquire` / `release`, or a locked snapshot):
- `handlers.c` `io_do_tls` :102 (already saves to a local -- but
  the save is unlocked)
- `handlers.c` `rpc_trans_writer` :281
- `handlers.c` `io_handle_write` :383, :388-390, :405-412, :422
- `handlers.c` `io_handle_read` :1311-1376 (SSL_read drain loop)
- `handlers.c` `handle_tls_handshake` :948-949 (`process_ssl_accept`)

TLS-flag reads to make lock-safe (TSAN flagged `ci_tls_enabled`):
- every `ci->ci_tls_enabled` / `ci->ci_tls_handshaking` read in
  `handlers.c` (:281, :422, :948, :1296, :1311, ...).

## Design

### Mechanism A -- refcounted `ci_ssl` handoff (the core fix)

`SSL` objects carry their own atomic refcount: `SSL_up_ref()`
bumps it, `SSL_free()` drops it, the object frees at zero.  Use
that instead of a bespoke `urcu_ref`.  New `conn_info.c` API
(declared in `reffs/io.h`):

```
/* Install a freshly SSL_new()'d object.  The +1 it arrives with
 * IS the conn_info slot's ref.  Under conn_mutex. */
void io_conn_ssl_install(int fd, SSL *ssl);

/* Take a use-ref: under conn_mutex, SSL_up_ref(ci_ssl) and return
 * it, or NULL if fd is not registered / has no SSL.  Caller MUST
 * pair with io_conn_ssl_release(). */
SSL *io_conn_ssl_acquire(int fd);

/* Drop a use-ref taken by io_conn_ssl_acquire(). */
void io_conn_ssl_release(SSL *ssl);

/* Drop the slot's ref: under conn_mutex detach ci_ssl (NULL it,
 * clear ci_tls_enabled / ci_tls_handshaking), then SSL_shutdown +
 * SSL_free outside the lock.  Idempotent. */
void io_conn_ssl_clear(int fd);
```

Lifecycle (Rule-6 shaped, OpenSSL refcount as the counter):

- **Install**: `handle_tls_handshake` -> `io_conn_ssl_install`.
  The slot owns one ref.
- **Use**: every consumer does
  `ssl = io_conn_ssl_acquire(fd); if (!ssl) { gone; } ... use ...;
  io_conn_ssl_release(ssl);`.  The use-ref keeps the `SSL` memory
  alive for the duration even if the slot's ref is dropped
  concurrently.
- **Clear**: `io_conn_unregister` / `io_conn_destroy` /
  `io_conn_cleanup` and the `handlers.c` error paths all route
  through `io_conn_ssl_clear`, which drops the slot's ref under
  the lock.  If a use-ref is outstanding the memory survives until
  the holder releases.

This eliminates the **use-after-free** (memory safety) and the
**data race on the `ci_ssl` field** -- the field is now read and
written only under `conn_mutex`.

Known limitation, documented not fixed here: `SSL_up_ref` keeps
the *memory* alive but OpenSSL `SSL` objects are not safe for two
threads issuing `SSL_*` calls concurrently.  The event loop
(`SSL_read`) and a worker (`SSL_write`) can still both be inside
one `SSL`.  That is a pre-existing concurrency hazard *separate*
from the INV-5/INV-6 free; the refcount makes it memory-safe
(no UAF), and serialising reader vs. writer is tracked as a
follow-up (see Deferred).  The reader/writer split already mostly
holds: `SSL_write` runs under the per-fd write gate.

### Mechanism B -- lock-safe TLS-state snapshot

The `if (ci && ci->ci_ssl && ci->ci_tls_enabled)` test pattern
becomes a single locked accessor so the flags are never read
torn:

```
/* Snapshot the TLS flags under conn_mutex.  Returns false if fd
 * is not registered. */
bool io_conn_tls_snapshot(int fd, bool *tls_enabled,
                          bool *handshaking);

/* Write the TLS flags as a pair under conn_mutex. */
void io_conn_tls_set_state(int fd, bool tls_enabled,
                           bool handshaking);
```

`io_conn_is_tls_enabled()` already exists and is lock-safe; this
adds the handshaking flag and a combined read so a consumer sees a
consistent pair.

`io_conn_tls_set_state` is the write half of Mechanism B (added
during Slice 1 -- the original four-function sketch named only the
snapshot reader).  The handshake-progress writes in
`process_ssl_accept` / `io_handle_write` set `ci_tls_enabled` +
`ci_tls_handshaking` together, so a paired locked setter keeps a
concurrent `io_conn_tls_snapshot` from observing a torn
(enabled && handshaking) state.  The pre-existing single-flag
`io_conn_set_tls_handshaking` is reused where only the handshaking
bit changes.

### Mechanism C -- CONN_CLOSING fd-reuse hardening (Slice 3, lower priority)

`io_conn_check_timeouts()` (conn_info.c:778-783) already names the
gap: a stale CQE for a closed fd, or an fd reused between
unregister and the next register, resolves `io_conn_get()` to the
wrong connection.  The fix is a `CONN_CLOSING` state that blocks
`io_conn_register()` slot reuse until in-flight ops
(`ci_read_count` / `ci_write_count` / write gate) drain, then
transitions `CONN_CLOSING -> CONN_UNUSED`.

Mechanism A already removes the *dangerous* face of this window
(the SSL UAF).  Mechanism C closes the narrower "wrong connection"
face.  It is staged **after** the Track 2 re-run: if the re-run
with A+B is clean, C is hardening that can land on its own
schedule; if the re-run still shows reuse symptoms, C is promoted.

## Tests first

### Reproducer promotion

`conn_lifecycle_race_test.c` is the TDD anchor.  After Slice 1 it
is rewritten to use `io_conn_ssl_acquire` / `io_conn_ssl_release`
in the reader and `io_conn_ssl_clear` in the churn thread, must go
**GREEN** under TSAN and ASAN on dreamer, and moves from
`check_PROGRAMS`-only into `TESTS`.

### New unit tests -- `lib/io/tests/conn_info_test.c`

A test `SSL_CTX` (created in a fixture, as `tls_test.c` does) backs
real `SSL` objects:

| Test | Intent |
|------|--------|
| `test_ssl_install_acquire_release` | install -> acquire returns the SSL, ref held -> release; clear frees it; ASAN/LSAN clean |
| `test_ssl_acquire_unregistered` | acquire on an unknown fd -> NULL |
| `test_ssl_acquire_no_ssl` | registered fd, no SSL installed -> NULL |
| `test_ssl_clear_idempotent` | clear twice -> no double-free |
| `test_ssl_clear_with_outstanding_ref` | acquire, then clear; the acquired SSL stays valid until release (no UAF) -- the core safety property |
| `test_ssl_unregister_clears_ssl` | install, `io_conn_unregister` -> `io_conn_ssl_acquire` returns NULL, SSL freed |
| `test_tls_snapshot` | install + set flags -> snapshot returns the pair; unregistered fd -> false |

### Test impact analysis

| File | Impact |
|------|--------|
| `lib/io/tests/conn_info_test.c` | **EXTEND** -- new SSL-lifecycle tests; needs an `SSL_CTX` fixture.  Existing register/get/unregister/write-gate tests unchanged in intent and must still pass. |
| `lib/io/tests/conn_lifecycle_race_test.c` | **REWRITE** to the safe API; moves into `TESTS` (Slice 2). |
| `lib/io/tests/tls_test.c` | PASS -- exercises `io_tls_init_server_context`, untouched. |
| `lib/io/tests/tls_write_count_test.c` | **AUDIT** -- it does `io_conn_get(TEST_FD)` and may poke `ci_ssl` directly; if so, convert to the new accessors.  Confirm during Slice 1. |
| `lib/io/tests/backend_io_test.c` | PASS -- no TLS. |
| `make check` (whole tree) | PASS -- the new `conn_info.c` API is additive; `io_conn_unregister` semantics are unchanged for non-TLS fds. |

### CI / sanitizer note

Sanitizers do not run on the macOS dev box (TSAN segfaults / ASAN
deadlocks in their own runtime init on Darwin 25.5).  All
TSAN/ASAN verification for this slice runs on **dreamer**
(`reffs-cc-t2`, Fedora aarch64).  macOS is build-only.

## Implementation order

**Slice 1 -- refcounted `ci_ssl` + TLS snapshot (the fix).**
1. `reffs/io.h`: declare the four `io_conn_ssl_*` functions and
   `io_conn_tls_snapshot`.
2. `conn_info.c`: implement them; route `io_conn_unregister` /
   `io_conn_destroy` / `io_conn_cleanup` SSL teardown through
   `io_conn_ssl_clear`.
3. `handlers.c`: convert every site in the inventory above --
   install via `io_conn_ssl_install`, frees via
   `io_conn_ssl_clear`, uses via `acquire` / `release`, flag tests
   via `io_conn_tls_snapshot`.
4. `conn_info_test.c`: add the SSL-lifecycle unit tests (write
   them first; they fail until step 2 lands).
5. Build + `make check` on dreamer; reviewer agent (lock-ordering
   / lifecycle -> reviewer-gated per `.claude/CLAUDE.md`).

**Slice 1 status: DONE 2026-05-19** (commits `1c7aae9a6276` design
amendment, `50572a93024d` code).  macOS build clean + full
`make check` 100% pass; reviewer agent found no BLOCKERs.  dreamer
(`reffs-cc-t2`, Fedora aarch64) ASAN build + full `make check`
clean (every suite FAIL:0/ERROR:0, no sanitizer reports); dreamer
TSAN: `conn_info_test` (incl. the 7 new SSL-lifecycle tests),
`tls_test`, `tls_write_count_test` all GREEN, no TSAN warning
references `conn_info.c` / `handlers.c`.

Pre-existing issue surfaced (NOT this slice): `backend_io_test`
fails under TSAN with data races in `lib/io/backend.c`
`io_handle_backend_pread` / `io_handle_backend_pwrite` and the
test's own `teardown` -- the async file-I/O ring, a separate
subsystem from the network/TLS conn_info path.  `backend.c` was
last touched 2026-04-20 and is untouched by this slice.  Tracked
for a separate fix; does not block the Track 2 re-run.

**Slice 2 -- promote the reproducer.**
6. Rewrite `conn_lifecycle_race_test.c` to the safe API; verify
   GREEN under TSAN and ASAN on dreamer; move it into `TESTS`.

**Slice 2 status: DONE 2026-05-19.**  Test rewritten to drive the
churn / reader threads through `io_conn_ssl_install`,
`io_conn_ssl_acquire` / `_release`, `io_conn_tls_snapshot`, and
`io_conn_tls_set_state` -- no direct `ci_ssl` / `ci_tls_enabled`
access remains; `#include "io_internal.h"` dropped.  Wrapped in a
libcheck `Suite` so the LOG_COMPILER picks it up uniformly with the
rest of the io tests, and moved from `check_PROGRAMS`-only into
`TESTS`.  Sized to fit the standards.md two-second budget under
TSAN: CHURN_ITERS=1000, NR_FDS=4, READERS_PER_FD=2 -- runs in 107 ms
under TSAN on dreamer.  macOS clean build + full `make check` 100%
pass; dreamer ASAN: 5/5 io tests PASS, no sanitizer reports; dreamer
TSAN: `conn_lifecycle_race_test` PASS plus `conn_info_test` /
`tls_test` / `tls_write_count_test` PASS, zero TSAN warnings on
`lib/io/conn_info.c` / `handlers.c` / `tests/conn_lifecycle_race_test.c`.
The `backend_io_test` TSAN failure carries the same three warnings
already recorded in the Slice 1 status section (still
`backend.c:122` / `backend.c:146` / `backend_io_test.c:69`) -- not
regressed by Slice 2, still tracked separately.

**Slice 3 -- CONN_CLOSING fd-reuse hardening (after Track 2 re-run).**
7. Add `CONN_CLOSING`; `io_conn_unregister` -> `CONN_CLOSING`;
   drain-driven `CONN_CLOSING -> CONN_UNUSED`; block
   `io_conn_register` reuse of a `CONN_CLOSING` slot.
8. Extend the generation check beyond the write gate.

State machine for Slice 3 (`CONN_CLOSING`):

```
   live state ---- io_conn_unregister ----> CONN_CLOSING
        ^                                        |
        |                              in-flight ops drain
        |                              (read/write counts 0,
   io_conn_register                     write gate idle)
   (fresh slot) <---- CONN_UNUSED <----------+
```

`io_conn_register` on a `CONN_CLOSING` slot returns NULL (caller
retries) rather than reusing it; on `CONN_UNUSED` it reuses as
today.

## Deferred / NOT_NOW_BROWN_COW

- Serialising event-loop `SSL_read` against worker `SSL_write` on
  one `SSL` (the pre-existing reader/writer concurrency hazard).
  Mechanism A makes it memory-safe; full serialisation is a
  separate slice and is not required to unblock Track 2.
- Replacing the flat `connections[]` array + single `conn_mutex`
  with a sharded or RCU-backed table.  Out of scope.

## RFC references

- RFC 9289 (RPC-over-TLS) -- the transport under repair.
