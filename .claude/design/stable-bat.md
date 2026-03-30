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
- Transition GRACE_STARTED → IN_GRACE in `server_state_get()`
- grace_timer_thread should end grace after `2 * grace_time`
- Tests: unit test for grace state machine with short grace period

### WI-1.2: Client State Persistence and Recovery [L]

- Prereqs: WI-1.1, RocksDB namespace (done)
- Reload client records from namespace DB on restart
- `SEQ4_STATUS_RESTART_RECLAIM_NEEDED` path fully implemented
- Client reclaim during grace period
- Tests: start, create sessions, SIGTERM, restart, verify reclaim
- RFC: RFC 8881 S8.4.2, S18.36.3

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

## Phase 2: Multi-Export with Per-Flavor Superblocks (MEDIUM)

### WI-2.1: Multi-Export TOML Configuration [S]

```toml
[[export]]
path = "/"
flavors = ["sys", "krb5", "krb5i", "krb5p", "tls"]

[[export]]
path = "/sys"
flavors = ["sys"]

[[export]]
path = "/krb5"
flavors = ["krb5"]

[[export]]
path = "/krb5i"
flavors = ["krb5i"]

[[export]]
path = "/krb5p"
flavors = ["krb5p"]

[[export]]
path = "/tls"
flavors = ["tls"]

[[export]]
path = "/ffv1"
flavors = ["sys"]

[[export]]
path = "/ffv2"
flavors = ["sys"]
```

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

### WI-3.1: ALLOCATE [S]

- RFC 7862 S15.1
- Validate stateid, `posix_fallocate()` or extend data_block
- Tests: allocate on regular file, verify size

### WI-3.2: DEALLOCATE [S]

- Prereqs: WI-3.1
- RFC 7862 S15.4
- `fallocate(FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE)` or
  zero-fill for RAM backend
- Tests: allocate then deallocate, verify zeros

### WI-3.3: READ_PLUS [M]

- RFC 7862 S15.10
- Return `read_plus_content4` with data/hole segments
- POSIX: `SEEK_HOLE`/`SEEK_DATA` for hole detection
- Tests: file with holes, verify hole+data segments

### WI-3.4: COPY (synchronous) [L]

- Prereqs: WI-1.2 (stateid validation)
- RFC 7862 S15.2
- POSIX: `copy_file_range()` syscall
- RAM: memcpy between data blocks
- Tests: copy file, verify content, cross-sb returns NFS4ERR_XDEV

### WI-3.5: CLONE [M]

- Prereqs: WI-3.4
- RFC 7862 S15.13
- XFS: `ioctl(FICLONE_RANGE)`, else NFS4ERR_NOTSUPP
- Risk: requires reflink-capable FS

### WI-3.6: OFFLOAD_STATUS / OFFLOAD_CANCEL [M]

- Prereqs: WI-3.4
- RFC 7862 S15.8, S15.9
- Async copy tracking table, offload_id, status polling, cancel

### WI-3.7: EXCHANGE_RANGE [L]

- Prereqs: WI-3.5
- draft-haynes-nfsv4-swap, op number 81
- XFS: `ioctl_xfs_exchange_range`, else atomic swap via temp storage
- Alignment check (clone_blksize), overlap check (same file)
- Tests: exchange ranges, verify contents swapped

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

Phase 2 (Multi-export):
  WI-2.1 ── WI-2.2
  WI-2.3 (independent)
  WI-2.4 (independent)

Phase 3 (NFSv4.2 ops):
  WI-3.1 ── WI-3.2
  WI-3.3 (independent)
  WI-3.4 ── WI-3.5 ── WI-3.7
       └──── WI-3.6

Phase 4 (Dir deleg):
  WI-4.1 ── WI-4.2

Phase 5 (External tests):
  Phase 1 ── WI-5.1, WI-5.2, WI-5.3 ── WI-5.4

Phase 6 (BAT demo):
  Phase 1 + Phase 2 ── WI-6.1
```

## Summary Table

| WI | Description | Size | Phase | Prereqs |
|----|-------------|------|-------|---------|
| 0.1 | TLS unit test suite | M | 0 | — |
| 0.2 | Rename ci-sec → ci-full | S | 0 | — |
| 0.3 | GitHub CI parity | M | 0 | 0.1, 0.2 |
| 0.4 | v3/v4 git build-on-reffs | M | 0 | 0.2 |
| 1.1 | Fix grace lifecycle bug | M | 1 | — |
| 1.2 | Client state persistence/recovery | L | 1 | 1.1 |
| 1.3 | Incarnation hash table | S | 1 | — |
| 1.4 | Server state persistence verify | M | 1 | RocksDB (done) |
| 1.5 | Export registry recovery | M | 1 | 1.4 |
| 1.6 | Long-running soak test | M | 1 | 1.1–1.5 |
| 2.1 | Multi-export TOML config | S | 2 | exports (done) |
| 2.2 | FFv1-to-FFv2 cross-sb layout | M | 2 | 2.1 |
| 2.3 | Kerberos setup script | S | 2 | — |
| 2.4 | Child mount check on unmount | S | 2 | — |
| 3.1 | ALLOCATE | S | 3 | — |
| 3.2 | DEALLOCATE | S | 3 | 3.1 |
| 3.3 | READ_PLUS | M | 3 | — |
| 3.4 | COPY (synchronous) | L | 3 | 1.2 |
| 3.5 | CLONE | M | 3 | 3.4 |
| 3.6 | OFFLOAD_STATUS/CANCEL | M | 3 | 3.4 |
| 3.7 | EXCHANGE_RANGE | L | 3 | 3.5 |
| 4.1 | Dir deleg research (knfsd) | M | 4 | — |
| 4.2 | GET_DIR_DELEGATION impl | XL | 4 | 4.1 |
| 5.1 | CTHON04 integration | M | 5 | Phase 1 |
| 5.2 | pynfs integration | M | 5 | Phase 1 |
| 5.3 | pjdfstest integration | M | 5 | Phase 1 |
| 5.4 | Separate CI workflow files | S | 5 | 5.1–5.3 |
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

7. **Lock stateid validation**: NOT_NOW_BROWN_COW in lock.c will
   surface in pynfs/CTHON04 testing.  Address in Phase 1.

8. **idmap persistence**: Lost on restart.  Visible in krb5 env
   but not blocking.

9. **BAT timeline**: Need date to determine critical path.
   Phase 0 + Phase 1 are prerequisites for stable demo.
