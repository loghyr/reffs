<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# PS Phase 6 bypass-side tests (α + β)

## Context

Phase 6b-ii landed the `nc_is_registered_ps` bypass in
`nfs4_check_wrongsec()` at `lib/nfs4/server/security.c:237-247`.
The bypass:

- fires only when `op_is_namespace_discovery(curr_opnum)` is true
  (the discovery set is PUTFH / PUTPUBFH / PUTROOTFH / RESTOREFH /
  LOOKUP / LOOKUPP);
- emits a TRACE-level audit line ("PS-bypass: op=... ...");
- returns NFS4_OK before any rule-match or `rpc_cred_squash` runs.

`lib/nfs4/tests/wrongsec_test.c` already pins three positive /
negative cases:

- `test_registered_ps_bypasses_wrongsec_on_lookup` (positive)
- `test_registered_ps_bypasses_wrongsec_on_putfh` (positive)
- `test_registered_ps_does_not_bypass_open` (negative: OPEN is
  data-access, must NOT bypass)

Two contract gaps remain from proxy-server.md Phase 6 test list:

- `test_registered_ps_does_not_bypass_root_squash` (α)
- `test_registered_ps_lookup_audit_logged` (β)

This slice ships both as additions to `wrongsec_test.c`.  No
production code moves.

## α: root_squash bypass-negative

### Intent

The bypass is namespace-discovery-only.  For a non-discovery op
(e.g. OP_OPEN) on a registered-PS session, the rule-match path
runs and applies `rpc_cred_squash(&compound->c_ap, rule)` at
`security.c:266` -- BEFORE the flavor check.  This means a
registered PS forwarding uid=0 on a non-discovery op gets squashed
to 65534, same as any other client.

### Test mechanism

The existing `make_ctx_registered_ps` allocates a context with
`c_ap.aup_uid = 0` (calloc'd default).  The test:

1. Builds a 2-op compound: PUTFH(child_sb) + OPEN(CLAIM_NULL).
2. Calls `dispatch_compound()`.
3. Asserts:
   - PUTFH succeeds (NFS4_OK) -- bypass on the discovery op.
   - OPEN fails NFS4ERR_WRONGSEC -- the rule-match path runs,
     squash applies, then the flavor check rejects AUTH_SYS on
     the KRB5-only `child_sb`.
   - **`compound->c_ap.aup_uid == 65534` and `c_ap.aup_gid ==
     65534`** -- squash ran before the WRONGSEC return.

The third assertion is the contract this slice exists to pin: a
future refactor that hoists the bypass above the rule-match
branch, or that re-orders the squash to after the flavor check,
would break the third assertion and the test fires.

### Test impact analysis

| File | Impact |
|------|--------|
| `wrongsec_test.c` (all 21 existing tests) | PASS -- new test is additive |
| All other `make check` tests | PASS -- no production code change |

## β: audit-log capture

### Intent

The bypass emits a TRACE line at `security.c:241`:

```
TRACE("PS-bypass: op=%u client_flavor=%u tls=%d sb_id=%lu",
      curr_opnum, client_flavor, client_tls, ...)
```

A future refactor that drops the audit log (intentionally or
accidentally) silently disables the namespace-shape disclosure
audit trail.  The test pins that the line fires on a bypassed
LOOKUP.

### Test mechanism

The reffs test harness (`lib/tests/libreffs_test.c`) initializes
the trace subsystem with `reffs_trace_init(NULL)`, which falls
back to `trace_fp = stderr` (per `common.c:238`).  All TRACE
events during tests write to stderr.

The test captures stderr via the stdlib mechanism, runs the
bypass-triggering dispatch, and greps the captured output:

```c
fflush(stderr);
int saved_fd = dup(STDERR_FILENO);
FILE *tf = tmpfile();           /* unlinked stream */
dup2(fileno(tf), STDERR_FILENO); /* TRACE -> tf */

dispatch_compound(ctx->compound);

fflush(stderr);                  /* drain pending writes */
fseek(tf, 0, SEEK_SET);
read_full(tf, buf, bufsz);

dup2(saved_fd, STDERR_FILENO);   /* restore */
close(saved_fd);
fclose(tf);

ck_assert(strstr(buf, "PS-bypass:") != NULL);
```

No new trace API surface.  The mechanism relies on:

- `reffs_test_global_init` having already called
  `reffs_trace_init(NULL)` so `trace_fp == stderr`.  This is the
  case for every test using the `libreffs_test` harness.
- `reffs_trace_event` calling `fflush(trace_fp)` after each TRACE
  event (`common.c:343`) so the test sees its writes after the
  test body returns.
- The FS trace category being enabled (the harness calls
  `reffs_trace_enable_all_categories()`).

### Buffer sizing

`dispatch_compound` can emit many TRACE events.  The capture
buffer is sized at 64 KiB on the stack -- plenty for a 2-op
compound (PUTROOTFH + LOOKUP) and well within libcheck's per-test
stack budget.  If a future compound produces more output, the
read truncates and the substring search still finds the
"PS-bypass:" line as long as it lands in the first 64 KiB.

### Concurrency

`dispatch_compound` in the test runs synchronously on the test
thread.  No other thread emits TRACE during the test body
(the test harness's compress thread reads `trace_fp` indirectly
via rotation, but does not emit TRACE itself).  The `dup2` is
race-free because no other thread is writing to stderr at the
moment of the swap.

### Test impact analysis

| File | Impact |
|------|--------|
| `wrongsec_test.c` (all 21 existing tests) | PASS -- new test is additive |
| All other `make check` tests | PASS -- no production code change, no global state leakage (saved_fd / tmpfile restored at test end) |

## Implementation order

1. Write both tests (TDD; both should pass at HEAD since the
   production behavior they pin is already in place).
2. Register them in the `wrongsec_suite()` test list.
3. Build, run locally on macOS (without sanitizers per the
   macOS-26 ASAN init deadlock).
4. Push branch, dreamer ASAN+UBSAN verification.
5. FF main if green.

## Files touched

| File | Change |
|------|--------|
| `lib/nfs4/tests/wrongsec_test.c` | +2 tests (~90 LOC), +2 suite registrations |
| `.claude/design/ps-phase6-bypass-tests.md` | NEW -- this doc |

No production code moves.  Reviewer guidance per `.claude/CLAUDE.md`:
"test-only additions where the production code did not move" -- inline
review is sufficient; no reviewer-agent pass needed.

## Deferred

- CB_PROXY_STATUS receive path (`test_cb_proxy_status_idle_response`
  and TSAN-clean concurrent variant) -- the larger Phase 7/8
  cross-layer work.  Not in scope here.
- Trace-capture library helper.  If a second test needs trace
  capture, extract the tmpfile+dup2 pattern into
  `lib/tests/libreffs_test.{c,h}` as `reffs_test_trace_capture_*`.
  Today's single consumer (β) keeps the mechanism inline.
