<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# STABLE_BAT — Stable Long-Running System for BAT Demo

## Goal

A stable, long-running (all-week) reffsd that recovers from
restarts/crashes, with multiple exports/sbs (root with all flavors,
per-flavor sbs), FFv1 + FFv2/CHUNK, Kerberos, TLS, and NFSv4.2
advanced operations.  Reinstall-from-scratch is acceptable if
things are too hosed; migration/upgrade is not required.

## Priorities (user-stated)

1. **HIGHEST**: TLS unit testing + CI parity (GitHub CI = ci-full)
2. **HIGH**: ci-full (renamed ci-sec, comprehensive, runs locally)
3. **HIGH**: Stability (crash recovery, restart survival, long-running)
4. **MEDIUM**: NFSv4.2 ops (COPY, CLONE, ALLOCATE, DEALLOCATE,
   READ_PLUS, OFFLOAD_*, EXCHANGE_RANGE)
5. **MEDIUM**: Directory delegations (needs Linux knfsd research)
6. **MEDIUM**: Multi-export with per-flavor sbs
7. **LOWER**: CHUNK demo at BAT
8. **LOWER**: External test suites (CTHON04, pynfs, pjdfstest)

## Progress (updated 2026-03-30)

| Phase | Status | Key deliverables |
|-------|--------|-----------------|
| 0: TLS + CI | **COMPLETE** | 7 TLS tests, ci-full, GitHub parity, build-on-NFS |
| 1: Stability | **COMPLETE** | Grace fix (_Atomic), 15 new tests, soak script |
| 2: Multi-export | **COMPLETE** | bat_export_setup.sh, bat_krb5_setup.sh |
| 3: NFSv4.2 ops | **IN PROGRESS** | ALLOCATE/DEALLOCATE next |
| 4: Dir delegations | Pending | Needs knfsd research first |
| 5: External tests | Pending | After Phase 1 stability |
| 6: BAT demo | Pending | After Phase 1+2 |

Total unit tests: 120.

## Outstanding Work Inventory

### From design docs and NOT_NOW_BROWN_COW markers

1. Grace lifecycle bug (server never leaves GRACE_STARTED)
2. Full client recovery (grace handling, state reclaim)
3. CB_GETATTR fattr4 decode/merge (TODO)
4. CB_LAYOUTRECALL fence+revoke on timeout (TODO)
5. RocksDB deferred: atomic WriteBatch, chunk_persist/load wiring,
   registry RocksDB persistence, parameterized tests
6. Identity management (entire Phase 2 unimplemented)
7. Export management: DRAINING state, child-mount check on unmount
8. Client persist: load-all/append/rewrite (NOT_NOW_BROWN_COW)
9. Server incarnations hash table (NOT_NOW_BROWN_COW)
10. idmap persistence (NOT_NOW_BROWN_COW)
11. Lock stateid validation (RFC 8881 S18.22.3, NOT_NOW_BROWN_COW)
12. CHUNK ops: 4 of 8 stubbed (HEADER_READ, LOCK, UNLOCK, ROLLBACK)
13. TIRPC connection sharing bug (workaround in place)

### Stubbed NFSv4.2 operations (all return NFS4ERR_NOTSUPP)

- COPY, COPY_NOTIFY, CLONE
- OFFLOAD_CANCEL, OFFLOAD_STATUS
- READ_PLUS
- ALLOCATE, DEALLOCATE
- GET_DIR_DELEGATION, WANT_DELEGATION
- xattr ops (partial)

## Test Impact Analysis

This section identifies which existing tests are affected by each
phase.  Per the planner responsibilities in `.claude/roles.md`,
every plan must analyse test impact before describing implementation.

### Phase 0 (TLS + CI) — test impact

| Existing test file | Impact | Reason |
|--------------------|--------|--------|
| All `make check` tests | PASS — no change | Phase 0 adds new test files only |
| `ci_integration_test.sh` | PASS — no change | ci-full extends, doesn't modify |

New test files added: `lib/io/tests/tls_test.c`.
No existing test is modified.

### Phase 1 (Stability) — test impact

| Existing test file | Impact | Reason |
|--------------------|--------|--------|
| `lib/nfs4/tests/nfs4_client_persist.c` | **MINOR UPDATE** | WI-1.2 may change `client_identity_load` serialization format; round-trip tests must be updated if the format changes.  The existing 5 test cases stay valid in intent. |
| `lib/nfs4/tests/nfs4_session.c` | PASS — no change | Session lifecycle is orthogonal to grace |
| `lib/nfs4/tests/delegation_lifecycle.c` | PASS — no change | Delegation tests don't exercise grace |
| `lib/fs/tests/sb_persistence_test.c` | PASS — no change | Registry format unchanged |
| `lib/fs/tests/sb_lifecycle_test.c` | PASS — no change | SB state machine unchanged |
| `lib/nfs4/tests/cb_pending.c` | PASS — no change | CB infrastructure unchanged |
| `lib/config/tests/config_test.c` | PASS — no change | Config parsing unchanged |
| `ci_integration_test.sh` | **BENEFITS** | Grace fix (WI-1.1) means the server correctly exits grace, which may unmask latent issues in the integration test flow |

New test files added: `lib/utils/tests/grace_test.c` (WI-1.1),
`lib/nfs4/tests/client_recovery_test.c` (WI-1.2),
`scripts/ci_soak_test.sh` (WI-1.6).

### Phase 2 (Multi-export) — test impact

| Existing test file | Impact | Reason |
|--------------------|--------|--------|
| `lib/fs/tests/sb_mount_crossing_test.c` | PASS — no change | Mount-crossing tests use direct API, not config |
| `lib/fs/tests/sb_security_test.c` | PASS — no change | Flavor setting API unchanged |
| `lib/fs/tests/sb_path_conflict_test.c` | PASS — no change | Path conflict API unchanged |

No existing test modified.  WI-2.4 (child mount check) adds new
assertions to `sb_mount_crossing_test.c` or a new test file.

### Phase 3 (NFSv4.2 ops) — test impact

| Existing test file | Impact | Reason |
|--------------------|--------|--------|
| All existing tests | PASS — no change | All Phase 3 work adds new op handlers; no existing handler modified |

New test files per op (WI-3.1 through WI-3.7).

### Phase 5 (External tests) — test impact

External test suites (pynfs, CTHON04, pjdfstest) are run against
the server as a black box.  No existing test files are modified.
Expected failures are documented in WI-5.2 (pynfs baseline).

**Lock stateid impact on pynfs**: The `NOT_NOW_BROWN_COW` in
`lock.c` (TEST_STATEID always returns OK, CLOSE doesn't check
LOCKS_HELD) means pynfs lock tests will fail.  See Risk #7 and
the mitigation documented there.

## Phase 0: TLS Unit Testing + CI Parity (HIGHEST)

### WI-0.1: TLS Unit Test Suite [M]

New `lib/io/tests/tls_test.c`:
- `test_tls_ctx_init_valid_certs` — SSL_CTX creation with valid cert/key
- `test_tls_ctx_init_missing_cert` — graceful failure
- `test_tls_ctx_init_missing_key` — graceful failure
- `test_tls_alpn_negotiation` — ALPN callback selects "sunrpc"
- `test_tls_alpn_no_sunrpc` — ALPN behavior when sunrpc not offered
- `test_tls_handshake_loopback` — server+client SSL on loopback
- `test_tls_data_roundtrip` — SSL_write/SSL_read RPC-sized buffer
- `test_tls_available_flag` — `tls_available()` tracks cert presence
- `test_tls_delay_on_unavailable` — NFS4ERR_DELAY when TLS unavail

Needs mini-CA fixture (ephemeral certs in setUp).
RFC reference: RFC 9289 (RPC-over-TLS), ALPN protocol "sunrpc".

#### Mini-CA Fixture Design

The mini-CA is a test setUp helper that generates ephemeral
certificates for each test run.  It lives in the test file, not
as a separate library.

**Implementation**:
- `mini_ca_init()` called from `setup()`:
  1. Create a temp directory (`mkdtemp`)
  2. Generate RSA 2048 CA key + self-signed CA cert (OpenSSL API)
  3. Generate RSA 2048 server key + CSR, sign with CA
  4. Generate RSA 2048 client key + CSR, sign with CA
  5. Write all to temp dir: `ca.pem`, `server.pem`, `server.key`,
     `client.pem`, `client.key`
- `mini_ca_fini()` called from `teardown()`:
  1. Unlink all files
  2. `rmdir` temp directory
- All OpenSSL calls use `libcrypto` API (`EVP_PKEY_*`, `X509_*`),
  not `libssl`.  No `openssl` CLI subprocess.
- Certs have 1-day validity (sufficient for test lifetime).
- Subject: `CN=reffs-test-{ca,server,client}`.
- SAN: `IP:127.0.0.1` on server cert (loopback tests).

**Dependency**: `libcrypto` is already linked (TLS support).
No new package dependency.

### WI-0.2: Rename ci-sec to ci-full [S]

- Rename `ci-sec` target in `Makefile.reffs` to `ci-full`
- ci-full runs: license, style, build, unit tests, integration
  (v3/v4 git clone, identity, TLS, krb5)
- Keep `ci-sec` as alias or remove

### WI-0.3: GitHub CI Parity with ci-full [M]

- Prereqs: WI-0.1, WI-0.2
- `.github/workflows/ci.yml` runs same test matrix as ci-full
- Add style check step to GitHub CI
- TLS unit tests automatically included via `make check`

### WI-0.4: v3 and v4 Git Build-on-reffs Tests [M]

- Prereqs: WI-0.2
- After git clone on NFS mount, run: `autoreconf -fi && mkdir build
  && cd build && ../configure && make -j$(nproc)`
- Exercises: hardlinks (autotools), large writes (object files),
  readdir at scale, renames, timestamps
- Risk: slow; may need timeout increase

## Phase 1: Stability — Crash Recovery and Long-Running (HIGH)

### WI-1.1: Fix Grace Lifecycle Bug [M]

- server never leaves `SERVER_GRACE_STARTED`
- Transition GRACE_STARTED -> IN_GRACE in `server_state_get()`
- grace_timer_thread should end grace after `2 * grace_time`
- Bug analysis: `server_state_get()` uses `__atomic_load_n` (GCC
  builtin) instead of `atomic_load_explicit` (C11 stdatomic).
  The `server_lifecycle_set` also uses GCC builtins.  Per coding
  standards, all atomics must use C11 `_Atomic` + `atomic_*_explicit`.
  The `ss_lifecycle` field is not declared `_Atomic` in the struct,
  so the GCC builtins may not generate correct fences on all
  platforms.  Fix: make `ss_lifecycle` `_Atomic enum server_lifecycle`
  and convert all accesses to C11.

#### Grace State Machine Unit Tests

New file: `lib/utils/tests/grace_test.c`

These tests exercise the grace state machine in isolation using
a short grace period (100ms) so each test completes well under
the 2-second budget.

| Test | Intent |
|------|--------|
| `test_grace_fresh_start_skips_grace` | `server_state_init` with no prior client state transitions BOOTING -> GRACE_ENDED directly.  Verify `server_in_grace()` returns false. |
| `test_grace_dirty_start_enters_grace` | `server_state_init` with prior clients (simulate dirty shutdown by persisting a client record) transitions BOOTING -> GRACE_STARTED.  First `server_state_get()` transitions to IN_GRACE.  Verify `server_in_grace()` returns true. |
| `test_grace_timer_ends_grace` | Start grace with `ss_grace_time = 1` (so 2s deadline).  Sleep 3s.  Verify `ss_lifecycle == SERVER_GRACE_ENDED`.  Verify `server_in_grace()` returns false. (Use condvar wakeup, not raw sleep, to stay under 2s budget — set grace_time to 0 with timer checking at 50ms intervals.) |
| `test_grace_reclaim_complete_ends_grace_early` | Start grace with 1 unreclaimed client.  Call `server_reclaim_complete()`.  Verify transitions to GRACE_ENDED before timer fires. |
| `test_grace_reclaim_partial_stays_in_grace` | Start grace with 2 unreclaimed clients.  Call `server_reclaim_complete()` once.  Verify still IN_GRACE (ss_unreclaimed == 1). |
| `test_grace_started_to_in_grace_on_get` | Start grace (GRACE_STARTED).  Call `server_state_get()`.  Verify `ss_lifecycle == SERVER_IN_GRACE`.  Verify `server_state_get()` returns non-NULL. |
| `test_grace_shutdown_during_grace` | Start grace.  Call `server_state_fini()`.  Verify transitions to SHUTTING_DOWN.  Verify `server_state_get()` returns NULL. |
| `test_grace_nfs4_check_grace_helper` | Verify `nfs4_check_grace()` returns true during grace, false after grace ends. |

RFC reference: RFC 8881 S8.4.2 (grace period semantics).

### WI-1.2: Client State Persistence and Recovery [L]

- Prereqs: WI-1.1, RocksDB namespace (done)
- Reload client records from namespace DB on restart
- `SEQ4_STATUS_RESTART_RECLAIM_NEEDED` path fully implemented
- Client reclaim during grace period
- RFC: RFC 8881 S8.4.2, S18.36.3

#### Unit Tests for Client Persistence

New tests in `lib/nfs4/tests/nfs4_client_persist.c` (extend
existing file, which already has 5 tests for the round-trip):

| Test | Intent |
|------|--------|
| `test_client_serialize_roundtrip` | Create client with known owner/verifier/slot/clientid.  Call `client_identity_append`.  Call `client_identity_load`.  Verify all fields match: owner string, verifier, slot, clientid4 (including boot_seq, incarnation, slot fields via accessor macros). |
| `test_client_serialize_multiple` | Append 3 clients.  Load all.  Verify count == 3 and all fields correct.  Order preserved. |
| `test_client_serialize_empty` | Load from empty/nonexistent file.  Verify count == 0, no error. |
| `test_client_serialize_corrupt` | Write garbage to the persistence file.  Verify load returns error, no crash. |
| `test_client_reload_sets_unreclaimed` | Persist 2 clients, restart (re-init server_state).  Verify `ss_unreclaimed == 2`.  Verify `server_in_grace()` is true. |
| `test_client_reclaim_decrements_unreclaimed` | Persist 1 client, restart.  Call `server_reclaim_complete()`.  Verify `ss_unreclaimed == 0` and grace ends. |

Note: if some of these overlap with the existing 5 tests in
`nfs4_client_persist.c`, the existing tests take precedence
and the new tests cover only the gaps.

#### Functional Tests

- `scripts/ci_client_recovery_test.sh`:
  Start reffsd, create sessions, SIGTERM, restart, verify client
  sees `SEQ4_STATUS_RESTART_RECLAIM_NEEDED`, reclaim completes,
  server exits grace.

### WI-1.3: Incarnation Hash Table [S]

- Replace linear scan with `cds_lfht` keyed by slot
- NOT_NOW_BROWN_COW in `server.h`

### WI-1.4: Server State Persistence Verification [M]

- Prereqs: RocksDB namespace (done)
- Tests: boot_seq increments on restart, UUID preserved,
  slot_next preserved, client sees RESTART_RECLAIM_NEEDED

### WI-1.5: Export Registry Recovery [M]

- Prereqs: WI-1.4
- Verify: all non-root exports restored from registry with correct
  mount paths, flavors, state
- Tests: create exports via probe, restart, verify sb-list matches

### WI-1.6: Long-Running Soak Test [M]

- Prereqs: WI-1.1 through WI-1.5
- New `scripts/ci_soak_test.sh`
- Run reffsd for extended period with concurrent workload
  (git clone/build loop, random file ops)
- Periodic restart cycles (SIGTERM + restart every N minutes)
- Verify: no ASAN/UBSAN errors, no memory growth, no leaked FDs

#### Soak Test Acceptance Criteria

| Parameter | Value | Rationale |
|-----------|-------|-----------|
| Duration | 30 minutes (CI), 8 hours (manual BAT soak) | CI budget vs. all-day stability proof |
| Restart interval | Every 5 minutes (CI), every 15 minutes (BAT) | Frequent enough to exercise recovery |
| Concurrent clients | 2 (CI), 4+ (BAT) | Enough to stress session/state tables |
| Workload | git clone + `make -j2` loop, plus random mkdir/rm/rename | Exercises OPEN, CLOSE, WRITE, READDIR, RENAME, REMOVE |
| Pass criteria — ASAN | Zero `ERROR: AddressSanitizer` in log | Any ASAN hit is a blocker |
| Pass criteria — UBSAN | Zero `runtime error:` in log | Any UBSAN hit is a blocker |
| Pass criteria — memory | RSS at end within 2x of RSS at 5-minute mark | Unbounded growth = leak |
| Pass criteria — FD | Open FD count at end within 10% of FD count at 5-minute mark | FD leak = eventual EMFILE |
| Pass criteria — recovery | After each restart, `mount -t nfs4` succeeds within 30s | Grace period must complete |
| Fail criteria | Any of the above violated | Script exits non-zero |

The CI soak (30 min) runs as part of `ci-full`.  The BAT soak
(8 hours) is manual, launched by `scripts/ci_soak_test.sh --bat`.

### WI-1.7: Lock Stateid Baseline [S]

Address the `NOT_NOW_BROWN_COW` in `lib/nfs4/server/lock.c` to
the minimum level needed for external test suites:

- **TEST_STATEID** (line ~465): Currently returns OK for all
  stateids without lookup.  Fix: look up the stateid in the
  per-session or per-inode table, return NFS4ERR_BAD_STATEID for
  unknown stateids.
- **CLOSE with LOCKS_HELD** (line ~441): Currently unconditionally
  succeeds.  Fix: check `lock_owner` list on the open stateid;
  if non-empty, return NFS4ERR_LOCKS_HELD per RFC 8881 S18.22.3.
- Scope: These are the two specific NOT_NOW_BROWN_COW markers.
  Full lock-owner lifecycle (per-inode lock table, contention
  detection, upgrade/downgrade) is deferred.

**Tests**: Add to `lib/nfs4/tests/` or extend existing stateid
tests:
- `test_test_stateid_valid` — known stateid returns OK
- `test_test_stateid_invalid` — unknown stateid returns BAD_STATEID
- `test_close_with_locks_held` — CLOSE while locks held returns
  NFS4ERR_LOCKS_HELD

**Impact on pynfs**: With these fixes, basic pynfs LOCK tests
(grant, release) should pass.  Advanced contention tests may
still fail.  The pynfs baseline (WI-5.2) will document which
tests pass/fail.

## Phase 2: Multi-Export with Per-Flavor Superblocks (MEDIUM)

### WI-2.1: Multi-Export Probe-Based Setup Script [S]

**Clarification**: Per sb-registry-v3 design, the probe protocol
is the sole authority for export creation.  `[[export]]` in TOML
is used ONLY for root sb flavor configuration (the loop that
created non-root sbs from `[[export]]` entries was removed in the
registry-v3 work).

This WI creates a setup script that uses the probe protocol to
create the BAT demo exports:

New `scripts/bat_export_setup.sh`:
```bash
#!/bin/bash
# Create BAT demo exports via probe protocol.
# The root sb flavors come from TOML config (first [[export]]).
# All other exports are created at runtime via probe.

PROBE="reffs-probe.py"
HOST="${1:-localhost}"

# Root sb: flavors set from TOML config at boot.
# [[export]]
# path = "/"
# flavors = ["sys", "krb5", "krb5i", "krb5p", "tls"]

# Per-flavor exports (probe-created, server-assigned IDs)
$PROBE --host $HOST sb-create --path /sys --storage ram
$PROBE --host $HOST sb-set-flavors --id $(get_id /sys) --flavors sys
$PROBE --host $HOST sb-mount --id $(get_id /sys) --path /sys

$PROBE --host $HOST sb-create --path /krb5 --storage ram
$PROBE --host $HOST sb-set-flavors --id $(get_id /krb5) --flavors krb5
$PROBE --host $HOST sb-mount --id $(get_id /krb5) --path /krb5

# ... similar for /krb5i, /krb5p, /tls, /ffv1, /ffv2
```

The TOML config for BAT is minimal — only the root export's
flavors:

```toml
[[export]]
path = "/"
flavors = ["sys", "krb5", "krb5i", "krb5p", "tls"]
```

This avoids the TOML-vs-probe authority conflict identified in
sb-registry-v3.

### WI-2.2: FFv1-to-FFv2 Cross-SB Layout [M]

- MDS issues v1 layouts pointing at ffv1 sb, v2 layouts at ffv2 sb
- ec_demo --layout v1 against ffv1, --layout v2 against ffv2
- v3 I/O from one sb to another on same node

### WI-2.3: Kerberos Setup Script [S]

- `scripts/bat_krb5_setup.sh` based on krb_setup.eml
- Realm: REFFS.BAT
- Create host + nfs principals, extract keytabs
- Verify with `klist -kt`

### WI-2.4: Child Mount Check on Unmount [S]

- Fix NOT_NOW_BROWN_COW in `super_block_unmount()`
- Return -EBUSY if child export is mounted within

## Phase 3: NFSv4.2 Operations (MEDIUM)

### DS vs MDS Operation Classification (RFC 7862 S3.3.1)

Operations that MAY be sent to NFSv4.2 data servers:
- **IO_ADVISE, READ_PLUS, WRITE_SAME, SEEK** — valid DS ops

Operations that are MDS-only (NOT listed in S3.3.1):
- **ALLOCATE, DEALLOCATE, COPY, CLONE, EXCHANGE_RANGE** — MDS must
  fan out to DSes (like SETATTR truncate).  For standalone/DS mode,
  operate directly on local data block.

Implementation approach: **standalone mode first** for all ops.
MDS fan-out via dstore vtable deferred to follow-up.

### EXCHANGE_RANGE Op Number

EXCHANGE_RANGE is NOT op 81.  It will be assigned after the CHUNK
ops in the XDR.  Same DS semantics as CLONE (MDS-only, fan-out).

### WI-3.1: ALLOCATE [S]

- RFC 7862 S15.1
- **Standalone mode**: validate stateid, `posix_fallocate()` or
  extend data_block.  MDS fan-out deferred.
- Tests: allocate on regular file, verify size
- Errors: NFS4ERR_WRONG_TYPE for non-regular, NFS4ERR_GRACE during
  grace, NFS4ERR_BAD_STATEID for invalid stateid

### WI-3.2: DEALLOCATE [S]

- Prereqs: WI-3.1
- RFC 7862 S15.4
- **Standalone mode**: `fallocate(FALLOC_FL_PUNCH_HOLE |
  FALLOC_FL_KEEP_SIZE)` or zero-fill for RAM backend.
  MDS fan-out deferred.
- Tests: allocate then deallocate, verify zeros
- Errors: same as ALLOCATE

### WI-3.3: READ_PLUS [M]

- RFC 7862 S15.10
- **Valid DS operation** (RFC 7862 S3.3.1)
- Return `read_plus_content4` with data/hole segments
- POSIX: `SEEK_HOLE`/`SEEK_DATA` for hole detection
- RAM: always return all-data content (no holes)
- Tests: file with holes, verify hole+data segments

### WI-3.4: COPY (synchronous) [L]

- **Prereqs**: None for basic implementation.  OPEN stateid
  validation already works (OPEN, SEQUENCE, CLOSE path is
  complete).  WI-1.2 (client persistence) is NOT a prerequisite
  — COPY validates the source and destination stateids via the
  existing stateid lookup infrastructure, which works for the
  current boot's stateids.  Client persistence only matters for
  stateids surviving a restart, which is a separate concern.
- RFC 7862 S15.2
- **Scope**: Synchronous COPY only (`ca_consecutive == true`,
  `ca_synchronous == true`).  The server performs the copy inline
  and returns the result in the COPY response.  Async COPY is
  WI-3.6.
- POSIX: `copy_file_range()` syscall
- RAM: memcpy between data blocks
- Tests: copy file, verify content, cross-sb returns NFS4ERR_XDEV

#### COPY Security Model

When source and destination are in different exports (different
superblocks), the security policy of BOTH exports must be satisfied:

1. **Same-export COPY**: Normal stateid validation.  The client
   already authenticated to the export's flavor when it opened
   the files.

2. **Cross-export COPY**: The compound contains PUTFH(src) +
   SAVEFH + PUTFH(dst) + COPY.  The PUTFH ops each validate
   the filehandle's export security (via `nfs4_check_wrongsec`).
   If the client's credential doesn't match BOTH exports' flavor
   lists, one of the PUTFHs returns NFS4ERR_WRONGSEC before COPY
   is reached.  **No additional security check is needed in the
   COPY handler itself** — the compound already enforced it.

3. **Cross-server COPY** (inter-server, RFC 7862 S4.4): Requires
   COPY_NOTIFY.  **Deferred** — see Deferred section.

4. **Flavor mismatch example**: Client authenticated with AUTH_SYS
   opens file in `/sys` (flavors: [sys]).  Client tries to COPY
   to `/krb5` (flavors: [krb5]).  PUTFH for the destination
   returns NFS4ERR_WRONGSEC.  COPY never executes.

### WI-3.5: CLONE [M]

- **Prereqs**: None (CLONE is independent of COPY).  CLONE is
  simpler than COPY — it is always synchronous, operates on a
  single file or between two files in the same filesystem, and
  has no async/offload machinery.
- RFC 7862 S15.13
- XFS: `ioctl(FICLONE_RANGE)`, else NFS4ERR_NOTSUPP
- Risk: requires reflink-capable FS.  RAM backend returns
  NFS4ERR_NOTSUPP.  POSIX backend returns NFS4ERR_NOTSUPP
  unless the underlying FS supports FICLONE_RANGE (XFS, Btrfs).
- Tests: clone on reflink-capable FS, verify content shared;
  clone on non-capable FS returns NFS4ERR_NOTSUPP

### WI-3.6: Async COPY + OFFLOAD_STATUS / OFFLOAD_CANCEL [L]

- Prereqs: WI-3.4 (synchronous COPY must work first)
- RFC 7862 S15.2 (async), S15.8, S15.9
- Async copy tracking table, offload_id, status polling, cancel

#### Async COPY State Machine

When `ca_synchronous == false` or the server decides to go async
(e.g., copy size > threshold), the server creates an async copy
task.

```
                    COPY request (async)
                          │
                          ▼
                      PENDING
                          │
                          │ worker thread picks up
                          ▼
                    IN_PROGRESS ────────► FAILED
                          │               (I/O error, ESTALE,
                          │                ENOSPC, etc.)
                          │
               ┌──────────┼──────────┐
               │          │          │
               ▼          ▼          ▼
          COMPLETE    CANCELLED    FAILED
          (normal)   (OFFLOAD_    (timeout,
                      CANCEL)     shutdown)
```

**States**:

| State | Description | OFFLOAD_STATUS returns |
|-------|-------------|----------------------|
| PENDING | Queued, not yet started | `osr_count = 0` |
| IN_PROGRESS | Worker thread copying | `osr_count = bytes_copied_so_far` |
| COMPLETE | All bytes copied | `osr_complete = (count, verifier, stable)` |
| FAILED | Error during copy | Error status from copy |
| CANCELLED | Client called OFFLOAD_CANCEL | NFS4ERR_NOXATTR (task gone) |

**Transitions**:

| From | To | Trigger | Precondition |
|------|----|---------|-------------|
| PENDING | IN_PROGRESS | Worker dequeues | Worker thread available |
| PENDING | CANCELLED | OFFLOAD_CANCEL | Client requests cancel |
| IN_PROGRESS | COMPLETE | Last byte copied | `bytes_copied == total` |
| IN_PROGRESS | FAILED | I/O error | `copy_file_range` returns error |
| IN_PROGRESS | CANCELLED | OFFLOAD_CANCEL | Sets cancel flag; worker checks between chunks |

**Data structures**:

```c
struct async_copy_task {
    struct cds_lfht_node act_node;  /* in ss_copy_ht */
    stateid4 act_stateid;           /* offload stateid */
    _Atomic enum copy_state act_state;
    _Atomic uint64_t act_bytes_copied;
    uint64_t act_total;
    nfsstat4 act_error;             /* if FAILED */
    /* source/dest FH, stateids, byte range */
};
```

**Cleanup**: Tasks in COMPLETE/FAILED/CANCELLED are reaped after
a timeout (1 lease period) or when the client calls OFFLOAD_STATUS
and retrieves the final result.

**OFFLOAD_CANCEL**: Sets a cancel flag.  The worker checks this
flag between copy chunks (e.g., every 1MB).  If set, the worker
transitions to CANCELLED and stops.  Partially copied data
remains (the client can use DEALLOCATE to clean up, or just
truncate + retry).

### WI-3.7: EXCHANGE_RANGE [L]

- Prereqs: WI-3.5
- draft-haynes-nfsv4-swap, op number 81
- XFS: `ioctl_xfs_exchange_range`, else atomic swap via temp storage
- Alignment check (clone_blksize), overlap check (same file)
- Tests: exchange ranges, verify contents swapped

### WANT_DELEGATION scope

WANT_DELEGATION (RFC 8881 S18.49) allows a client to request a
delegation without opening a file.  The existing delegation
infrastructure (OPEN-based grants, CB_RECALL) is in place.

**Scope for STABLE_BAT**: Implement the `WANT_DELEGATION` op
handler to process `OPEN_DELEGATE_NONE_EXT` (refuse gracefully)
and `OPEN_DELEGATE_READ`/`OPEN_DELEGATE_WRITE` requests.  The
server MAY grant or refuse at its discretion.  Initial
implementation: always refuse with `WD4_CONTENTION` or
`WD4_RESOURCE`.  This satisfies the wire protocol requirement
(no longer NFS4ERR_NOTSUPP) while deferring the delegation
grant policy to future work.

**Not in scope**: Proactive delegation grants via WANT_DELEGATION
(the server currently only grants delegations at OPEN time).

## Phase 4: Directory Delegations (MEDIUM)

### WI-4.1: Research Linux knfsd Implementation [M]

- Jeff Layton: RFC 8881 is broken for dir delegations
- Read Linux `fs/nfsd/` for GET_DIR_DELEGATION, CB_NOTIFY
- Document delta between RFC 8881 and Linux implementation
- Output: `.claude/design/dir-delegations.md`
- RFC: RFC 8881 S18.39, S20.4

### WI-4.2: GET_DIR_DELEGATION Implementation [XL]

- Prereqs: WI-4.1, CB infrastructure (done)
- Dir notification registration, CB_NOTIFY on changes
- Stateid management for dir delegations, recall mechanism
- Tests: dir deleg grant, create triggers CB_NOTIFY

## Phase 5: CI Workflows and External Test Suites (LOWER)

### WI-5.1: CTHON04 Integration [M]

- Prereqs: Phase 1
- Basic, general, special, lock tests for NFSv3/v4
- https://github.com/connectathon/cthon04

### WI-5.2: pynfs Integration [M]

- Prereqs: Phase 1
- Run as external process (no vendoring — GPL ply)
- NFSv4.0 and NFSv4.1 test suites
- Establish pass/fail baseline
- **Expected failures**: Lock tests that exercise TEST_STATEID
  edge cases or lock-owner contention beyond WI-1.7 scope.
  Document in a `pynfs-expected-failures.txt` file.

### WI-5.3: pjdfstest Integration [M]

- Prereqs: Phase 1
- POSIX filesystem compliance tests on NFS mount

### WI-5.4: Separate CI Workflow Files [S]

- `ci-check.yml` (existing), `ci-full.yml`, `ci-cthon04.yml`,
  `ci-pynfs.yml`, `ci-pjdfstest.yml`
- Local equivalents via Makefile.reffs targets

## Phase 6: BAT Demo Configuration (LOWER)

### WI-6.1: BAT Demo Setup [S]

- Prereqs: Phase 1 + Phase 2
- Docker Compose or deployment script
- 1 MDS with multi-export, KDC, ec_demo scenarios
- Soak test running in background

## Deferred / Out of Scope

Items from the Outstanding Work Inventory that are NOT addressed
by any WI in this plan.  Each has an explicit reason for deferral.

| # | Item | Reason for deferral |
|---|------|-------------------|
| 3 | CB_GETATTR fattr4 decode/merge | Works end-to-end (round-trip verified); decode/merge is a polish item, not stability-critical |
| 4 | CB_LAYOUTRECALL fence+revoke on timeout | Fire-and-forget CB_LAYOUTRECALL works.  Fence+revoke on timeout is a correctness enhancement for adversarial clients, not needed for BAT cooperative demo |
| 5 | RocksDB deferred items (WriteBatch, chunk wiring, parameterized tests) | RocksDB namespace DB works for persistence.  Advanced features are optimization, not stability |
| 6 | Identity management Phase 2 | Full identity schema (reffs_id, domain table, group cache) is a large feature.  BAT uses AUTH_SYS + krb5 with libnfsidmap, which already works |
| 10 | idmap persistence | Mappings regenerated on restart from libnfsidmap.  Visible as a brief delay after restart, not a correctness issue |
| 12 | CHUNK ops (HEADER_READ, LOCK, UNLOCK, ROLLBACK) | Happy-path CHUNK ops (WRITE, READ, FINALIZE, COMMIT) work.  Stubbed ops are for advanced chunk lifecycle not needed for BAT demo |
| 13 | TIRPC connection sharing bug | Workaround (DS connection dedup) is in place and stable |

**COPY_NOTIFY**: Inter-server COPY (RFC 7862 S4.4) requires
COPY_NOTIFY to authorize the destination server to read from the
source.  This is deferred because BAT demo uses intra-server COPY
only (source and destination on the same server).  The stub
returns NFS4ERR_NOTSUPP, which is the correct response for a
server that does not support inter-server copy.

**DRAINING state for exports**: Export lifecycle has CREATED,
MOUNTED, UNMOUNTED, DESTROYED.  A DRAINING state (graceful
teardown waiting for open files to close) is a nice-to-have but
not needed for BAT — exports are created at setup and persist
for the demo's lifetime.

## Dependency Graph

```
Phase 0 (TLS + CI):
  WI-0.1 ──┐
            ├── WI-0.3 (GitHub parity)
  WI-0.2 ──┘
  WI-0.4 ── depends on WI-0.2

Phase 1 (Stability):
  WI-1.1 ── WI-1.2 ── WI-1.6
  WI-1.3 ──┘
  WI-1.4 ── WI-1.5 ── WI-1.6
  WI-1.7 (independent, needed before Phase 5)

Phase 2 (Multi-export):
  WI-2.1 ── WI-2.2
  WI-2.3 (independent)
  WI-2.4 (independent)

Phase 3 (NFSv4.2 ops):
  WI-3.1 ── WI-3.2
  WI-3.3 (independent)
  WI-3.4 ── WI-3.6
  WI-3.5 (independent — no dependency on WI-3.4)
  WI-3.5 ── WI-3.7

Phase 4 (Dir deleg):
  WI-4.1 ── WI-4.2

Phase 5 (External tests):
  Phase 1 + WI-1.7 ── WI-5.1, WI-5.2, WI-5.3 ── WI-5.4

Phase 6 (BAT demo):
  Phase 1 + Phase 2 ── WI-6.1
```

## Summary Table

| WI | Description | Size | Phase | Prereqs |
|----|-------------|------|-------|---------|
| 0.1 | TLS unit test suite | M | 0 | — |
| 0.2 | Rename ci-sec -> ci-full | S | 0 | — |
| 0.3 | GitHub CI parity | M | 0 | 0.1, 0.2 |
| 0.4 | v3/v4 git build-on-reffs | M | 0 | 0.2 |
| 1.1 | Fix grace lifecycle bug | M | 1 | — |
| 1.2 | Client state persistence/recovery | L | 1 | 1.1 |
| 1.3 | Incarnation hash table | S | 1 | — |
| 1.4 | Server state persistence verify | M | 1 | RocksDB (done) |
| 1.5 | Export registry recovery | M | 1 | 1.4 |
| 1.6 | Long-running soak test | M | 1 | 1.1-1.5 |
| 1.7 | Lock stateid baseline | S | 1 | — |
| 2.1 | Multi-export probe-based setup | S | 2 | exports (done) |
| 2.2 | FFv1-to-FFv2 cross-sb layout | M | 2 | 2.1 |
| 2.3 | Kerberos setup script | S | 2 | — |
| 2.4 | Child mount check on unmount | S | 2 | — |
| 3.1 | ALLOCATE | S | 3 | — |
| 3.2 | DEALLOCATE | S | 3 | 3.1 |
| 3.3 | READ_PLUS | M | 3 | — |
| 3.4 | COPY (synchronous) | L | 3 | — |
| 3.5 | CLONE | M | 3 | — |
| 3.6 | Async COPY + OFFLOAD_STATUS/CANCEL | L | 3 | 3.4 |
| 3.7 | EXCHANGE_RANGE | L | 3 | 3.5 |
| 4.1 | Dir deleg research (knfsd) | M | 4 | — |
| 4.2 | GET_DIR_DELEGATION impl | XL | 4 | 4.1 |
| 5.1 | CTHON04 integration | M | 5 | Phase 1 |
| 5.2 | pynfs integration | M | 5 | Phase 1, 1.7 |
| 5.3 | pjdfstest integration | M | 5 | Phase 1 |
| 5.4 | Separate CI workflow files | S | 5 | 5.1-5.3 |
| 6.1 | BAT demo config | S | 6 | Phase 1+2 |

## Risks and Open Questions

1. **EXCHANGE_RANGE atomicity**: Without XFS, atomicity is
   best-effort.  May need NFS4ERR_NOTSUPP on non-capable backends.

2. **Directory delegations + RFC 8881 errors**: Research (WI-4.1)
   must precede implementation.  If delta is large, scope to Linux
   knfsd behavior only.

3. **CLONE without reflink**: On ext4/RAM, CLONE returns
   NFS4ERR_NOTSUPP.  REFFS_DATA_XFS deferred in RocksDB plan.

4. **Grace lifecycle bug blocks everything**: If server never exits
   grace, clients can't do normal ops after restart.  Must fix first.

5. **pynfs license**: Run as external process only (no vendoring).

6. **CI runtime**: Building on NFS mount (WI-0.4) is slow.  May
   need separate workflow or conditional step.

7. **Lock stateid validation**: NOT_NOW_BROWN_COW in lock.c.
   **Mitigation**: WI-1.7 addresses the two specific
   NOT_NOW_BROWN_COW markers (TEST_STATEID lookup, CLOSE
   LOCKS_HELD check).  This is sufficient for basic pynfs/CTHON04
   lock tests.  Advanced lock-owner lifecycle (contention
   detection, upgrade/downgrade, per-inode lock table iteration)
   remains deferred.  WI-5.2 establishes a pass/fail baseline
   that documents which lock tests still fail.

8. **idmap persistence**: Lost on restart.  Visible in krb5 env
   but not blocking.

9. **BAT timeline**: Need date to determine critical path.

10. **CB_RECALL + pynfs delegation tests**: pynfs delegation tests
    hang because our CB_RECALL doesn't populate the stateid in the
    format pynfs's callback Event expects.  The test receives the
    recall but `recall.stateid` is unset → AttributeError → fd
    close → select(-1) → hang.  Skipped in ci_pynfs.sh with
    `nodeleg`.  BAT demo will hit this if delegation recall is
    exercised by userspace clients.

11. **NFSv4.2 directory change_info**: CTHON04 basic/test7 fails
    because the Linux NFSv4 client doesn't invalidate its dcache
    after rename.  The changeid same-tick nsec bump helps but is
    not sufficient.  Needs a proper monotonic change counter on
    directory inodes instead of ctime-derived changeid.
   Phase 0 + Phase 1 are prerequisites for stable demo.
