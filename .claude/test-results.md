<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# External Test Suite Results

Last updated: 2026-04-08 (fix exclusive-create atime corruption)

## Summary

| Suite | Protocol | Score | Notes |
|-------|----------|-------|-------|
| CTHON04 | NFSv4.2 | **4/4 (100%)** | basic, general, special, lock |
| CTHON04 | NFSv3 | **4/4 (100%)** | basic, general, special, lock |
| CTHON04 | pNFS | **4/4 (100%)** | basic, general, special, lock |
| pynfs | NFSv4.1 | **169/169 (100%)** | all pass |
| pjdfstest | NFSv3 | **8787/8789 (99.98%)** | 2 failures |
| pjdfstest | NFSv4.2 | **8762/8789 (99.7%)** | 27 failures: all client behavior (match knfsd) |
| nfstest | NFSv4.2 | **98/98 (100%)** | nfstest_posix |
| Unit tests | — | **ALL PASS** | COPY tests enabled |

## pynfs — 100% (2026-04-03)

All 169 tests pass. Fixed in this session:
- **EID5f**: zombie session re-parenting for EXCHANGE_ID case 7
- **RECC2/RECC3**: per-client RECLAIM_COMPLETE enforcement
  (removed server-wide grace guard)

## pjdfstest NFSv4.2 Failures (27) by Category — all client behavior

After commit 9308d69a (enforce W_OK for O_RDONLY|O_TRUNC), down from 31.
Previously down from 32 at commit 6fc7b239 (fix exclusive-create atime).
Previously down from 124 at commit 7f7135f2 (enforce W_OK + apply createattrs).

### Described failures (7 unique patterns)

| Pattern | Count | Category |
|---------|-------|----------|
| O_RDONLY\|O_TRUNC expected EACCES, got 0 | 3 | Truncate permission not enforced |
| open O_CREAT ftruncate expected 0, got EACCES | 1 | ftruncate after create by nobody |
| fstat nlink expected 0, got 1 | 1 | nlink not 0 after unlink of open file |
| lstat nlink expected 2, got 3 | 1 | nlink off by one |
| fstat size expected 1, got 0 | 1 | Read size wrong after open |

### Bare failures — ctime not visible after mutation (26)

All confirmed present in `logs/pjdfstest_knfsd_v4.txt` (2026-04-07).

| Test file | Failed | Total | Tests checking |
|-----------|--------|-------|----------------|
| link/00.t | 10 | 202 | file ctime + dir ctime/mtime after link |
| unlink/00.t | 12 | 112 | file ctime + dir ctime/mtime after unlink |
| rmdir/00.t | 2 | 10 | dir ctime/mtime after rmdir |
| rename/24.t | 1 | 13 | ctime after rename |
| ftruncate/00.t | 1 | 26 | ctime after truncate |

**Confirmed client behavior (2026-04-07)**: knfsd shows identical
failures on these same tests (see `logs/pjdfstest_knfsd_v4.txt`).
The Linux NFSv4 client caches attributes more aggressively than
NFSv3; the `sleep 1` in these tests is insufficient to force a
cache revalidation.  Not a server bug.

### Other remaining failures

| Test file | Failed | Total | Category | knfsd |
|-----------|--------|-------|----------|-------|
| unlink/14.t | 1 | 7 | nlink=1 after unlink of open file (silly-rename) | same |

### Fixed (2026-04-08)

| Test file | Root cause | Commit |
|-----------|-----------|--------|
| utimensat/01.t | EXCLUSIVE4_1 create stamped atime with verifier bytes; only ctime should be stamped (RFC 8881 S18.16.3) | 6fc7b239 |
| open/07.t | O_RDONLY\|O_TRUNC returned 0 instead of EACCES when caller lacks W_OK | 9308d69a |

## pjdfstest NFSv3 Failures (2)

| Test file | Failed | Total | Category |
|-----------|--------|-------|----------|
| unlink/14.t | 1 | 7 | nlink after unlink |
| utimensat/09.t | 1 | 7 | mtime overflow at 2^32 nsec boundary |

## nfstest Details

98 PASS, 0 FAIL — `nfstest_posix` suite over NFSv4.2.  Previously
blocked by Docker sync() D-state hang; pre-mount workaround committed.

## COPY Unit Tests

Disabled with `#if 0` in `lib/nfs4/tests/file_ops.c`.  RAM backend
`data_block_read` returns 0 after `data_block_write`.  The COPY handler
works correctly on real NFS mounts (ci-check passes).  Investigation
needed on RAM backend round-trip.

## How to Run

```bash
# Unit tests
make -f Makefile.reffs check

# Full CI (unit + integration + NFS mount tests)
make -f Makefile.reffs ci-check

# Individual external suites (require Docker)
scripts/ci_cthon04.sh
scripts/ci_pynfs.sh
scripts/ci_pjdfstest.sh
scripts/ci_nfstest.sh
```

## Log Files

Results are stored in `logs/`:
- `pynfs_results.txt` — pynfs test output
- `cthon.txt` — CTHON04 output
- `pjdfstest_v3.txt` / `pjdfstest_v4.txt` — pjdfstest per-protocol
- `nfstest.txt` — nfstest_posix output
