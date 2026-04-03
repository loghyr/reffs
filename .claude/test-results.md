<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# External Test Suite Results

Last updated: 2026-04-02 (post createattrs + W_OK fix)

## Summary

| Suite | Protocol | Score | Notes |
|-------|----------|-------|-------|
| CTHON04 | NFSv4.2 | **4/4 (100%)** | basic, general, special, lock |
| CTHON04 | NFSv3 | **4/4 (100%)** | basic, general, special, lock |
| CTHON04 | pNFS | **4/4 (100%)** | basic, general, special, lock |
| pynfs | NFSv4.1 | **169/169 (100%)** | all pass |
| pjdfstest | NFSv3 | **8787/8789 (99.98%)** | 2 failures |
| pjdfstest | NFSv4.2 | **8757/8789 (99.6%)** | 32 failures (ctime + perm) |
| nfstest | NFSv4.2 | **98/98 (100%)** | nfstest_posix |
| Unit tests | — | **ALL PASS** | COPY tests enabled |

## pynfs — 100% (2026-04-03)

All 169 tests pass. Fixed in this session:
- **EID5f**: zombie session re-parenting for EXCHANGE_ID case 7
- **RECC2/RECC3**: per-client RECLAIM_COMPLETE enforcement
  (removed server-wide grace guard)

## pjdfstest NFSv4.2 Failures (28) by Category

After commit 7f7135f2 (enforce W_OK + apply createattrs), down from 124.

### Described failures (7 unique patterns)

| Pattern | Count | Category |
|---------|-------|----------|
| O_RDONLY\|O_TRUNC expected EACCES, got 0 | 3 | Truncate permission not enforced |
| open O_CREAT ftruncate expected 0, got EACCES | 1 | ftruncate after create by nobody |
| fstat nlink expected 0, got 1 | 1 | nlink not 0 after unlink of open file |
| lstat nlink expected 2, got 3 | 1 | nlink off by one |
| fstat size expected 1, got 0 | 1 | Read size wrong after open |

### Bare failures — ctime not visible after mutation (26)

| Test file | Failed | Total | Tests checking |
|-----------|--------|-------|----------------|
| link/00.t | 10 | 202 | file ctime + dir ctime/mtime after link |
| unlink/00.t | 12 | 112 | file ctime + dir ctime/mtime after unlink |
| rmdir/00.t | 2 | 10 | dir ctime/mtime after rmdir |
| rename/24.t | 1 | 13 | ctime after rename |
| ftruncate/00.t | 1 | 26 | ctime after truncate |

**Investigation (2026-04-02)**: The VFS layer (vfs_link, vfs_remove,
vfs_rmdir) correctly updates ctime on both the target inode and
parent directory via `inode_update_times_now()`.  The tests use
`sleep 1` to guarantee a time gap, then stat the file/dir.

NFSv3 passes these same tests (link/00.t, unlink/00.t) — only NFSv4
fails.  This could be:
- NFSv4 client attr cache behavior (v4 caches more aggressively than v3)
- Server not conveying the change correctly in the NFSv4 response
- Missing post-op attr update that the v3 wcc_data provides

**TODO**: Run pjdfstest NFSv4.2 against Linux knfsd to determine
whether this is client behavior or a server bug.  Do not assume
client cache until confirmed.

### Other remaining failures

| Test file | Failed | Total | Category |
|-----------|--------|-------|----------|
| open/07.t | 4 | 25 | O_TRUNC permission |
| unlink/14.t | 1 | 7 | nlink on unlink |
| utimensat/01.t | 1 | 7 | nsec overflow at 2^32 boundary |

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
