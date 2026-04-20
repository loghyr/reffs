<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Backend Extraction Pattern (reffs)

Load this file when CHANGE categories involve:

- `lib/io/backend_kqueue.c` and its `lib/io/backend.c` sibling.
- Moving code between `lib/io/handlers.c` / `lib/io/conn_info.c` /
  `lib/io/net_state.c` (shared) and the backend-specific files.
- Adding or removing stubs on the inactive backend.
- Changes to `lib/io/Makefile.am` conditional compilation
  (`if IO_BACKEND_LIBURING` / `if IO_BACKEND_KQUEUE`).

This pattern codifies the extraction methodology used in PRs #4-#7
of the FreeBSD port (conn_info extraction, handlers extraction,
net_state extraction, read/write handler + resubmit-primitive
split).  Future backend work (macOS thread-pool backend, PR #8 TLS
on kqueue) follows the same pattern.

---

## 1. The reffs Backend Architecture

The I/O layer has three compilation strata:

1. **Always-compiled shared code** (`Makefile.am` `libreffs_io_la_SOURCES` unconditional):
   - `conn_info.c` -- per-fd hash, connection state machine,
     write-serialization gate
   - `handlers.c` -- completion handlers (`io_handle_read/write/accept/connect`),
     TLS state machine, RPC record marker reassembly,
     `rpc_trans_writer` orchestration, `io_rpc_trans_cb`,
     `io_do_tls`
   - `net_state.c` -- pending request table, listener registry,
     per-fd buffer state
   - `context.c` -- `io_context` lifecycle, hash registry
   - `lsnr.c` -- IPv4/IPv6 listen socket setup
   - `tls.c` -- OpenSSL server context init (shared but guarded
     by tls_available)
   - `worker.c` -- worker thread pool

2. **liburing-only** (`Makefile.am` `if IO_BACKEND_LIBURING`):
   - `accept.c`, `backend.c`, `connect.c`, `handler.c`,
     `heartbeat.c`, `read.c`, `write.c`

3. **kqueue-only** (`Makefile.am` `if IO_BACKEND_KQUEUE`):
   - `backend_kqueue.c` (single file; ~1260 lines)

The backend interface is **compile-time-selected same-named
functions**, not a runtime vtable.  Each backend provides
implementations of:

- `io_request_{read,write,accept,connect}_op` -- allocate ic,
  submit fresh I/O
- `io_resubmit_{read,write}` -- reuse existing ic, submit next chunk
- `io_schedule_heartbeat`, `io_heartbeat_*` -- timer-driven watchdog
- `io_handler_init`, `io_handler_main_loop`, `io_handler_fini`,
  `io_handler_stop`, `io_handler_signal_shutdown`
- `io_backend_init` / `io_backend_main_loop` / `io_backend_fini`
  (file I/O, separate ring from network)
- `io_send_request` (outbound RPC; `-ENOSYS` stub on kqueue today)

Shared code in `handlers.c` / `conn_info.c` calls these via normal
function calls; the linker resolves to whichever backend's `.o`
is in the build.

---

## 2. The Extraction Workflow

When a function is backend-agnostic but currently lives in a
backend-specific file, extraction follows this sequence:

### Step A: Identify the backend dependencies

Grep for `io_uring_`, `liburing`, `<linux/`, `<sys/eventfd.h>`,
`<sys/signalfd.h>` in the function body.  If any exist, the function
is not directly extractable; see §3 below.

### Step B: Determine whether siblings need a primitive

If the function calls a `static` helper that IS backend-specific
(e.g., `rpc_trans_writer`'s inline `io_uring_prep_write`), the
helper must become a public primitive before the function can be
extracted.  See §4 for the primitive-introduction pattern.

### Step C: Move the function

Cut from backend-specific file, paste into `handlers.c` (or
`conn_info.c` / `net_state.c` as appropriate).  Include set in
destination file must include every header the moved code needs.
Delete the definition from the source file; don't leave behind an
extern declaration unless the source file still calls the function.

### Step D: Remove matching stubs

The inactive backend (at the time, typically kqueue) often had a
`-ENOSYS` or no-op stub for the now-shared function.  Delete that
stub in the **same commit** as the extraction.  Otherwise the build
breaks with a duplicate-symbol linker error.

### Step E: Audit for orphan callers

`git grep` for the moved function's name.  Every remaining caller
either passes through the public API (fine) or still references a
file-scope static from the source file (broken -- fix).

### Step F: Commit as one atomic change

The extraction + stub removal is a single commit.  No bisect traps:
before this commit, inactive backend has stub; after this commit,
both backends have the shared implementation.  Commit messages cite
"pure code move" only if the move truly is byte-identical; otherwise
call out the BEHAVIOR DELTA explicitly.

---

## 3. Function Depends on Backend Primitive

If the function you want to extract uses `io_uring_prep_write` or
`kevent(...)` directly, introduce a backend primitive first.  Then
extract.  This is the pattern `rpc_trans_writer` followed in PR #7:

### Before extraction

```c
/* lib/io/write.c -- io_uring-only */
static int rpc_trans_writer(struct io_context *ic, struct ring_context *rc)
{
    /* ... gate claim, TLS dispatch, chunk math ... */

    /* direct io_uring calls: */
    sqe = io_uring_get_sqe(&rc->rc_ring);
    io_uring_prep_write(sqe, fd, buf, chunk_size, 0);
    io_uring_submit(&rc->rc_ring);
    /* ... */
}
```

### Step 1: Extract the backend primitive

```c
/* lib/io/write.c -- io_uring-only, new public function */
int io_resubmit_write(struct io_context *ic, struct ring_context *rc)
{
    /* extracted from rpc_trans_writer: get_sqe / prep_write / submit */
}

/* lib/io/backend_kqueue.c -- stub, later real */
int io_resubmit_write(struct io_context *ic, struct ring_context *rc)
{
    (void)ic; (void)rc;
    return -ENOSYS;
}
```

Commit 1: refactor rpc_trans_writer to use the primitive.  Pure
refactor on the active backend; inactive backend's stub satisfies
the linker but the path is still unreachable (no caller yet).

### Step 2: Extract rpc_trans_writer

Now that `rpc_trans_writer` calls `io_resubmit_write` instead of
inline io_uring, it's backend-agnostic.  Move it to `handlers.c`.

### Step 3: Implement primitive on inactive backend

Commit 3: replace the `-ENOSYS` stub with a real implementation
using the inactive backend's primitives.  For kqueue, that's
`kevent(EV_ADD | EV_ONESHOT, EVFILT_WRITE, ...)`.

**Why three commits instead of one:** each commit builds cleanly on
both backends and leaves the active backend's behavior unchanged.
If a regression slips in, `git bisect` can locate which of the three
steps introduced it.

---

## 4. Stub Contracts

Stubs exist only to satisfy the linker until the real implementation
lands.  Their contract:

- **Return `-ENOSYS`** for operations that return an int.  Never
  return `0` (success) or `1` -- silent success hides the missing
  implementation.
- **Return `NULL`** for pointer-returning operations, matching
  allocation-failure semantics.
- **Log once at `LOG` level** when the stub is called.  The log
  line should say the backend, the function name, and what PR
  will provide the real implementation.
- **Do not call `io_context_destroy`** on ics passed in.  The
  caller owns lifecycle; stubs should not destroy on the
  caller's behalf without documentation.

**Exception:** stubs for completion handlers (`io_handle_*`)
typically DO destroy the ic, because the handler-chain contract is
"handler takes ownership."  A stub `io_handle_read` must
`io_context_destroy(ic)` + return `-ENOSYS` to match the real
handler's contract.

---

## 5. Precondition Documentation

When a shared function has behavior that's safe on one backend due
to a structural precondition that doesn't hold on the other,
document the precondition in the function's header comment and in
the commit message.

**Concrete reffs example.**  `io_do_tls` in `handlers.c` submits via
`io_request_write_op` outside the per-fd write gate.  On io_uring,
this is safe because the kernel linearizes concurrent writes.  On
kqueue, `EV_ADD | EV_ONESHOT` replaces the knote udata, orphaning a
prior writer.  So on FreeBSD PR #7, `io_do_tls` is unreachable
because `reffs_server_ssl_ctx` is never initialized -- the TLS
branch in `rpc_trans_writer` is dead.

The precondition is documented at `handlers.c:io_do_tls` header:

```c
/*
 * Unreachable on FreeBSD PR #7 because io_tls_init_server_context is
 * not called from the kqueue io_handler_init -- ci->ci_ssl is always
 * NULL.  When PR #8 ports TLS, the io_request_write_op call at the
 * bottom must be adjusted to honor the per-fd write gate (otherwise
 * the subsequent writer's EVFILT_WRITE registration silently replaces
 * this one's on kqueue).  See PR #7 addendum B2 for the analysis.
 */
```

and in the commit message body:

> FreeBSD PR #7 precondition: the TLS branch in rpc_trans_writer
> (ci->ci_ssl && ci->ci_tls_enabled) is unreachable because
> io_tls_init_server_context is not called from the kqueue
> io_handler_init.  PR #8 will port TLS and must address the
> EVFILT_WRITE collision that arises when io_do_tls submits outside
> the write gate.

**Review technique.**  When the commit message says "unreachable on
X because Y," verify Y in the code.  If Y is also the preconditions
for some *other* unreachability, you have a single point of failure
that the PR removing Y must address.  Track it as a blocker for that
future PR.

---

## 6. Smoke Evidence for Extraction Commits

For every extraction that activates code on a backend, attach smoke
evidence to the review:

1. **Build on both backends.**  `make -j4` on dreamer (Linux) and
   `gmake -j4` on witchie (FreeBSD).  Zero errors, zero new warnings.
2. **Tests pass on both.**  `make check` on dreamer (all io tests
   pass); `gmake check` on witchie (io tests pass, liburing-only tests
   like `tls_write_count_test` correctly gated with `IO_BACKEND_LIBURING`).
3. **reffsd starts on the activated backend.**  `./src/reffsd -b
   ram -S /var/lib/reffs -c 1` on witchie enters main loop, listeners
   on port 2049 appear in `sockstat`.
4. **Smoke I/O on the activated backend.**  For backend ports:
   `mount -t nfs -o nfsv4 localhost:/ /mnt/test && dd if=/dev/urandom
   of=/mnt/test/x bs=1M count=16 conv=sync && sha256sum /mnt/test/x`
   matches read-back.

The smoke evidence goes in the commit message body or the PR
description.  A reviewer who approves an extraction commit without
smoke evidence on the activated backend is signing off an idea, not
an implementation.

---

## 7. Checklist

Before approving an extraction / backend-port commit:

1. **Forward reachability:** new code runs for the described workload.
2. **Backward reachability:** every function this newly reaches is
   re-reviewed (NULL guards, edge inputs, error paths).  List them
   explicitly in the review output.
3. **Cross-backend invariants verified:** the shared code's
   assumptions hold on both backends' primitives.  Called out any
   invariant that only holds due to a structural precondition.
4. **Calling conventions audited:** backend dispatchers translate
   `-1 + errno` / `SO_ERROR` / `cqe->res` uniformly before calling
   shared handlers.
5. **Stub removal atomic with extraction:** no bisect trap; both
   backends build after every commit in the series.
6. **Move is byte-identical or BEHAVIOR DELTA called out:**
   drive-by changes split or documented.
7. **Preconditions documented:** any "safe because X is never
   true" note is verified and recorded as a blocker for the PR that
   makes X true.
8. **Smoke evidence attached:** log + sha256 + clean shutdown on
   the activated backend.
