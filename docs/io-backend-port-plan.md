<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# I/O Backend Port Plan — FreeBSD + macOS

**Date:** 2026-04-18
**Goal:** Abstract reffs' I/O layer behind a narrow backend interface so it
runs natively on Linux (liburing, unchanged), FreeBSD (aio + kqueue),
and macOS (thread pool). Shake out io_uring-specific bugs via
triangulation across three backends with different completion
semantics.

---

## Motivation

1. **Cross-platform reach.** Run reffs on witchie (FreeBSD 15) and mana
   (macOS) in addition to shadow/adept (Linux). Useful for NFS protocol
   testing against a second server stack and for local dev on the
   MacBook.
2. **Bug triangulation.** A second (and third) I/O implementation with
   different completion semantics is a better regression test than any
   amount of Linux-only unit coverage. Bugs that repro on all backends
   are app-level; bugs on one backend are backend-specific.
3. **Header hygiene.** Today `<liburing.h>` leaks into four public
   headers (`io.h`, `ring.h`, `task.h`, `rpc.h`). Even if the port is
   never shipped, the abstraction work has standalone value.

---

## Current State Analysis

### What's already good

- `lib/io/` is a dedicated layer with a clean op-level API in
  `lib/include/reffs/io.h`. Callers (NFS op handlers) hit high-level
  `io_request_*` functions, not raw liburing.
- Protocol is completion-oriented: ops submit, task pauses,
  completion resumes the task. Same shape as FreeBSD aio+kqueue and as
  a thread-pool with condvar.
- **Op surface is narrow.** The only `io_uring_prep_*` calls in the
  codebase are: `accept`, `connect`, `read`, `write`, `poll_add`,
  `timeout`, `cancel`. Seven ops. All have direct FreeBSD / macOS
  equivalents.
- **No unportable features in use.** No `IOSQE_IO_LINK` chains, no
  `IORING_REGISTER_BUFFERS` / registered fds, no
  `IORING_OP_OPENAT`, no `IORING_SETUP_SQPOLL`, no zero-copy
  (`sendzc`). Only two feature-gates: `IORING_FEAT_NODROP` and
  `IORING_FEAT_EXT_ARG`, both perf/ergonomic not semantic.
- `tsan_uring.h` (32 lines) is pure `__tsan_release` / `__tsan_acquire`
  barriers. Implementation is io_uring-agnostic; only the filename
  references io_uring. Trivial rename to `tsan_io.h`.

### What leaks

- `#include <liburing.h>` appears in `lib/include/reffs/io.h`,
  `ring.h`, `task.h`, and `rpc.h`. Every module that includes any of
  those transitively pulls in liburing.
- `struct ring_context` (in `ring.h`) has a
  `struct io_uring rc_ring` field directly. Hard ABI leak.
- Two separate rings at runtime: `io_handler_main_loop` (network) and
  `io_backend_main_loop` (file I/O). Both need to be ported, not just
  one.
- `handler.c:174` runtime-checks `IORING_FEAT_NODROP`;
  `handler.c:422` runtime-checks `IORING_FEAT_EXT_ARG`. Both need
  `#ifdef` or a backend-method equivalent.

---

## Goals

1. Three I/O backends, compile-time selected:
   - `backend_liburing.c` — Linux, current behavior, zero performance
     regression.
   - `backend_kqueue.c` — FreeBSD, POSIX `aio(4)` + `kqueue(2)` with
     `EVFILT_AIO` completions.
   - `backend_threadpool.c` — macOS primarily, also a portable fallback
     on any POSIX. Worker threads doing blocking `pread`/`pwrite`,
     completions via internal condvar/eventfd-equivalent.
2. `<liburing.h>` not present in any public header. Types in headers
   are either POSIX or reffs-native.
3. `struct ring_context` is opaque in headers; full definition lives
   in the active backend's `.c` file (or a shared private header).
4. All existing Linux tests pass unchanged (the liburing backend keeps
   semantic parity).
5. A new backend-level smoke test runs on all three backends from one
   harness.

## Non-goals

- Not replacing liburing with libuv/libev/libevent. They're either too
  readiness-oriented (libev/libevent) or force a larger refactor
  (libuv) without the cross-backend-triangulation benefit we want.
- Not porting to Windows. No one asked for it.
- Not supporting pre-FreeBSD 13 (which lacks `accept4(2)`).
- Not supporting pre-macOS 11 on the thread-pool backend (no specific
  blocker found yet; revisit if needed).
- **Not exposing io_uring-only features in the backend API.**
  Registered buffers, IOSQE_IO_LINK, etc. are explicitly out of scope.
  If future reffs features need them, that's a new plan.

---

## Architecture

### Backend interface

Defined in `lib/include/reffs/io_backend.h` (new). Opaque handles,
POSIX types, backend-neutral op semantics. Something like:

```c
struct io_backend;             /* opaque */
struct io_op;                  /* opaque; owned by caller, embedded
                                  in io_context today */

int  io_backend_init(struct io_backend **out, size_t sq_size,
                     size_t cq_size);
void io_backend_fini(struct io_backend *be);

int  io_submit_read (struct io_backend *, struct io_op *,
                     int fd, void *buf, size_t len, off_t offset);
int  io_submit_write(struct io_backend *, struct io_op *,
                     int fd, const void *buf, size_t len, off_t offset);
int  io_submit_accept (struct io_backend *, struct io_op *, int fd);
int  io_submit_connect(struct io_backend *, struct io_op *,
                       int fd, const struct sockaddr *, socklen_t);
int  io_submit_poll   (struct io_backend *, struct io_op *,
                       int fd, short events);
int  io_submit_timeout(struct io_backend *, struct io_op *,
                       const struct timespec *);
int  io_submit_cancel (struct io_backend *, struct io_op *target);

int  io_backend_submit(struct io_backend *);  /* kick submissions */
int  io_backend_reap(struct io_backend *,
                     const struct timespec *timeout,
                     struct io_op **completed, int *result,
                     int max);                /* drain completions */
```

Each backend implements the same interface, selected at compile time:

```c
#if defined(__linux__)
  #include "backend_liburing.c"  /* or linked as .o */
#elif defined(__FreeBSD__)
  #include "backend_kqueue.c"
#elif defined(__APPLE__)
  #include "backend_threadpool.c"
#else
  #error "unsupported platform"
#endif
```

Builds pick exactly one via Makefile.am conditional compilation.

### Two rings → two backends

reffs runs two rings today (network + backend-file-I/O) for latency
isolation. The pattern carries over: `io_backend_init()` is called
twice (once per main loop), each gets its own backend instance,
each has its own main loop draining completions. On FreeBSD that's
two kqueue fds. On threadpool, two worker pools (or one pool with two
completion queues). No shared-backend-state simplification.

### TSAN annotations

`lib/io/tsan_uring.h` → rename `lib/io/tsan_io.h`. Content unchanged
(macros are io_uring-agnostic). Every backend calls `TSAN_RELEASE`
before handing an op to the kernel (or to a worker thread) and
`TSAN_ACQUIRE` on the completion side.

---

## Phased PR Sequence

### PR 1 — Header hygiene + backend interface definition

**Goal:** No behavior change. The existing liburing implementation
survives, just moves behind the new interface. Tests pass unchanged.

- Define `lib/include/reffs/io_backend.h` with the opaque interface
  above.
- Make `struct ring_context` opaque in `lib/include/reffs/ring.h`;
  move the full definition (including `struct io_uring rc_ring`) to a
  new private header `lib/io/ring_internal.h` or inline into
  `backend_liburing.c`.
- Drop `#include <liburing.h>` from `io.h`, `ring.h`, `task.h`,
  `rpc.h`. Any remaining uses of io_uring types in those headers
  (audit with `grep -E "io_uring_(sqe|cqe)" lib/include/reffs/`) move
  to private headers or become opaque via the new interface.
- Rename `tsan_uring.h` → `tsan_io.h`, update includes.
- Rename functions/types where "ring" terminology is misleading (see
  W-6 below) — **OR** leave names as historical artifacts and document.
  Default: leave names, add comment in `io_backend.h`.
- Refactor `lib/io/backend.c`, `handler.c`, `read.c`, `write.c`,
  `accept.c`, `connect.c`, `worker.c` to call through the new
  `io_backend.h` interface. Internals unchanged — still liburing.
- Add stub `lib/io/backend_kqueue.c.in` and
  `backend_threadpool.c.in`: function prototypes matching the
  interface, `#error "not implemented"` bodies. Not compiled. Purely
  to prove the interface is drawable and to make PR 2/3 reviewers'
  jobs easier.
- `make check` passes on shadow; no binary or behavior diff vs. main.

**Effort:** 1–2 days focused.

### PR 2 — FreeBSD `aio(4)` + `kqueue(2)` backend

**Goal:** Runs on witchie. Linux unchanged.

- Implement `backend_kqueue.c`: one kqueue per backend instance,
  `aio_read` / `aio_write` / `aio_fsync` issue + `EVFILT_AIO`
  completion; `kevent` `EVFILT_READ`/`EVFILT_WRITE` for
  accept/connect/poll; `EVFILT_TIMER` for timeout; `aio_cancel` +
  `EV_DELETE` for cancel.
- Autotools detection: `AC_CHECK_HEADERS([sys/event.h sys/aio.h])`
  and `AC_CHECK_FUNCS([aio_read aio_write aio_cancel kqueue kevent])`.
- `#ifdef __FreeBSD__` the `IORING_FEAT_NODROP` and
  `IORING_FEAT_EXT_ARG` gates in `handler.c` (which is now
  `backend_liburing.c` internal).
- Raise `kern.maxaio*` at startup if below expected outstanding-op
  count (via `sysctl` — log warning if we can't).
- Port `io_backend_main_loop` to a kqueue-driven loop. Port
  `io_handler_main_loop` likewise.
- Add FreeBSD to CI (a witchie-side smoke run).

**Effort:** 2–3 days focused.

### PR 3 — Thread-pool backend (macOS + portable fallback)

**Goal:** Runs on mana. Also a second no-io_uring data point on
Linux/FreeBSD for bug triangulation.

- Implement `backend_threadpool.c`: fixed pool of worker threads
  (size tunable, default `nproc` or `sysconf(_SC_NPROCESSORS_ONLN)`);
  per-backend submission queue (mutex + condvar); completion queue
  returned via `io_backend_reap`; blocking `pread` / `pwrite` /
  `accept` / `connect` / `poll` (via `poll(2)` with short timeout)
  inside worker threads.
- Cancel semantics: set a cancelled flag on the op; best-effort — in
  flight ops complete normally with a cancelled result if still
  pending, otherwise complete as usual.
- Compile-time selection on `__APPLE__`. Also selectable via a
  `--with-io-backend=threadpool` configure flag for forcing it on
  Linux/FreeBSD during triangulation runs.
- Add macOS to CI (mana-side smoke run if feasible; otherwise docs-
  only "works on macOS").

**Effort:** 1 day focused.

### PR 4 — Smoke test harness

**Goal:** Catches backend bugs before the reffs-integration tests.

- New test in `lib/io/tests/` that runs a known op pattern (e.g.,
  1 k × 4 KB `pread` + 1 k × 4 KB `pwrite` + 64 × fsync) through the
  backend interface and verifies correctness. Not a perf bench — just
  correctness. Runs against whichever backend is compiled in. In
  `make check`.
- Optional follow-up: run the same harness with `--with-io-backend=`
  forced to each backend on Linux, as a CI matrix job. Divergence =
  backend bug.

**Effort:** 0.5 day.

---

## Reviewer Findings (c-protocol-review-prompts)

### BLOCKER
None.

### WARNING

- **W-1. Two main loops.** `io_handler_main_loop` (network) and
  `io_backend_main_loop` (file I/O) are structurally distinct. Port
  both, not just one. Each backend instance is independent.
- **W-2. `IORING_FEAT_NODROP` gate in `handler.c:174`.** CQE overflow
  handling. kqueue has no equivalent gating (events either enqueue or
  return `EV_OVERFLOW` explicitly). `#ifdef __linux__` the branch, or
  add a `be->supports_no_drop` backend method.
- **W-3. `IORING_FEAT_EXT_ARG` gate in `handler.c:422`.** Per-submit
  timeout. `kevent()` takes `struct timespec *timeout` natively.
  `#ifdef` or backend-method "submit_with_timeout".
- **W-4. Header leakage scope audit.** Before PR 1, run
  `grep -E "io_uring_(sqe|cqe)" lib/include/reffs/` to enumerate
  every io_uring type reference in public headers. If hits are
  confined to `struct ring_context`'s field, PR 1 is small. If there
  are user-facing structs with `struct io_uring_cqe *cqe` fields, PR 1
  is wider.
- **W-5. FreeBSD aio kernel tunables.** `kern.maxaiops`,
  `kern.maxaio`, `kern.aio_max_aio_per_proc`. Defaults differ by
  FreeBSD version (low on < 14, 1024 on 14+). Probe at startup; log a
  warning if limits are below expected outstanding-op count. Consider
  raising via `sysctl` at startup if running as root.
- **W-6. Ring terminology in public API.** `io_backend_set_global()` /
  `io_backend_get_global()` return `struct ring_context *`. On
  FreeBSD/macOS there is no ring. Options: (a) rename to
  `io_context` — mechanical churn across all callers; (b) leave as
  historical artifact with a comment. **Decision:** leave, document.

### NOTE

- **N-1. macOS aio is not worth pursuing.** Older macOS doesn't
  deliver aio completions via kqueue at all (signal-only); newer
  versions have `EVFILT_MACHPORT` workarounds but libdispatch (GCD)
  is the "native" completion model. The thread-pool backend is both
  simpler and more portable than any macOS-specific aio path.
- **N-2. Three backends >> two for bug triangulation.** Bug on all
  three = app logic. Bug on one = backend bug. Bug on two that
  exclude liburing = portable bug that io_uring happens to hide.
  Strong argument for PR 3 even if macOS support weren't a goal.
- **N-3. Smoke test harness (PR 4) is high leverage.** 200 LOC,
  catches backend bugs without running the full reffs stack. Include
  even if the rest of the plan slips.
- **N-4. `IORING_FEAT_NODROP` assumption already requires kernel
  5.11+.** If this has never bitten us, all production kernels are
  5.11+. Worth a CHANGELOG note when PR 2 lands clarifying kernel
  requirements for the Linux backend.
- **N-5. macOS `accept4` equivalent.** macOS lacks `accept4(2)`. Use
  `accept(2)` + `fcntl(F_SETFL, O_NONBLOCK)` + `fcntl(F_SETFD,
  FD_CLOEXEC)` in `backend_threadpool.c`'s accept handler. Two extra
  syscalls, not a correctness issue.

### PLAN-WARNING

PR 1 reviewers cannot tell whether the opaque types are drawn at the
right boundary unless they see at least an outline of PR 2. Include
stub `backend_kqueue.c.in` / `backend_threadpool.c.in` files with
function prototypes and `#error "not implemented"` bodies in PR 1.
Proves the boundary is right without shipping working backends.

---

## Revised Effort Summary

| Phase | Effort |
|-------|--------|
| PR 1: header hygiene + opaque interface | 1–2 days |
| PR 2: FreeBSD kqueue backend | 2–3 days |
| PR 3: thread-pool backend (macOS) | 1 day |
| PR 4: backend smoke test harness | 0.5 day |
| **Total** | **4.5–6.5 days** |

---

## Open Questions / Decisions to Make Before Coding

1. **Where do FreeBSD aio limits get raised?** Runtime `sysctl` at
   startup if root, or document as operator responsibility?
   *Proposed:* log current limits, warn if below expected outstanding,
   don't silently raise.
2. **Thread-pool worker count**: default to `nproc`, or expose as
   `[iouring] workers = N` config (which would need renaming since
   "iouring" is misleading)?
   *Proposed:* add new `[io_backend] workers = N`, fall back to
   `[iouring]` as deprecated alias for one release.
3. **Rename the `[iouring]` TOML section?** It configures ring sizes
   which only apply to the liburing backend; the field names
   (`network_sq_size` etc.) are io_uring-specific.
   *Proposed:* rename to `[io_backend]`, keep `[iouring]` as
   deprecated-alias for one release, generic field names
   (`submit_queue_depth`, `completion_queue_depth`).
4. **CI matrix**: run which backends on which CI host?
   *Proposed:* shadow runs `backend_liburing` (current); witchie runs
   `backend_kqueue`; add `--with-io-backend=threadpool` nightly on
   shadow as a triangulation signal.

---

## Relevant Code Pointers

- Public headers with liburing leaks:
  `lib/include/reffs/io.h`,
  `lib/include/reffs/ring.h`,
  `lib/include/reffs/task.h`,
  `lib/include/reffs/rpc.h`
- Current I/O implementation: `lib/io/*.c` (accept, backend, connect,
  context, handler, heartbeat, lsnr, read, tls, worker, write)
- TSAN shim: `lib/io/tsan_uring.h`
- Two runtime feature gates: `lib/io/handler.c:174` and `:422`
- Ops actually used (search reproducibility):
  `grep -roE "io_uring_prep_[a-z_]+" lib/ src/ | awk -F: '{print $2}' | sort -u`
  → `accept connect cancel poll_add read timeout write`

---

## How to Resume After an Outage

1. Re-read this document, §Current State Analysis and §Architecture.
2. Run the audit grep commands at end of §Relevant Code Pointers and
   check that `grep -c "io_uring_prep_" lib/ src/ -r` hasn't grown
   (indicating new ops added since the plan was written).
3. Start with PR 1 (header hygiene). It's standalone; no FreeBSD
   knowledge needed. Reviewers can approve it on Linux-only merits.
4. PR 2 (FreeBSD) and PR 3 (thread-pool) are independent of each
   other after PR 1 lands. Whichever is easier to write first is fine.
5. PR 4 (smoke test) can land anytime after PR 1.
