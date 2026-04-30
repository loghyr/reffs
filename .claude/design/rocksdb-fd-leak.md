<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# RocksDB Backend FD Leak (soak_rocksdb nightly failure)

## Symptom

Garbo nightly `soak_rocksdb` (RocksDB backend, 30 min, 5 restarts)
fails the FD-growth check:

```
SOAK FAIL: FD growth: 62 > 57 (baseline 35 + 50%)
```

`soak_posix` (same script, POSIX backend) passes.  Reproduces on
nightlies 2026-04-29 and 2026-04-30; FD count crept from 55 ->
62 in 24h.

The downstream effects are observed during the failure window:
many "FAILURE: N D-state processes OUTSIDE grace period" events
in the soak log, and the post-soak `dstate_check` catches a
leftover `ld` userspace process wedged in
`rpc_wait_bit_killable / _nfs4_proc_getattr`.  These are
symptoms -- the kernel client is wedging because reffsd is
degraded by FD pressure (io_uring submit failures / open() ENFILE
on backend ops) -- not the bug itself.

## Triage data (2026-04-30 13:00 nightly)

`lsof`-style listing in soak_rocksdb.log captures FD types open
in the reffsd process at "FDs before stop":

| Category | Count (aggregated across snapshots) |
|----------|-------------------------------------|
| anon_inode:[io_uring] | 8 |
| anon_inode:[eventfd] | 3 |
| RocksDB `*.sst` | 23 |
| RocksDB `*.log` (WAL/db_log) | 22 |
| RocksDB MANIFEST | 5 |
| RocksDB LOCK | 4 |
| Other (sockets, /dev/, /tmp/, etc.) | 72 |

Per "Final: ... FD=62" at end of 30 min run.  Baseline at
startup is 35.  The 27-FD growth is the budget that exceeds the
threshold.

The "Restart" cadence (every 5 min) does fork a fresh reffsd
process, so the leak is **within a single reffsd's lifetime**
under the soak workload, not across processes.  Each "Restart"
gets a clean baseline and the growth is per-workload.

## Suspect surface (priority order)

### 1. io_uring instance leak in connection lifecycle
8 io_uring anon_inodes is high for a server doing a single sb's
worth of I/O.  reffsd creates io_uring per-connection in
`io_handler.c`; if `io_conn_unregister()` does not call
`io_uring_queue_exit()` on the per-connection ring, the anon
inode stays alive.  Probably the highest-yield investigation
because io_uring leaks compound under restart cycles AND under
client churn within a single process.

Files: `lib/io/io_handler.c`, `lib/io/io_conn.c`,
`io_uring_queue_init` / `io_uring_queue_exit` matched pairs.

### 2. RocksDB column family handle leak
`rocksdb_open_column_families` returns one
`rocksdb_column_family_handle_t *` per CF; each must be
`rocksdb_column_family_handle_destroy`'d before
`rocksdb_close`.  Per-sb DB has 6 CFs (default, inodes, dirs,
symlinks, layouts, chunks); namespace DB has 7 (default,
registry, clients, incarnations, identity_domains,
identity_map, nsm).

Files: `lib/backends/rocksdb.c` (`rocksdb_sb_free`),
`lib/backends/rocksdb_namespace.c` (`rocksdb_namespace_close`).

### 3. RocksDB iterator / snapshot leak
Long iterators (`cds_lfht_*` analogs in RocksDB:
`rocksdb_create_iterator_cf`, `rocksdb_create_snapshot`) hold
SST refs.  If any code path early-exits without
`rocksdb_iter_destroy` / `rocksdb_release_snapshot`, the
underlying SST FDs stay pinned.

Files: `lib/backends/rocksdb_namespace.c` (incarnation/identity
load paths), `lib/nfs4/server/chunk_store.c` (chunk_load via
the rocksdb hook).

### 4. WAL file leak (`*.log` count)
22 db_log FDs is suspicious.  RocksDB rotates WAL files; old
logs should close after flush.  Either WAL retention is
configured too high, or a snapshot is pinning old logs.

## Reproduction

The 30-min nightly is too coarse for fast iteration.  A focused
repro is:

```sh
# 1.  Build with --enable-asan (Asan tracks FD leaks indirectly
#     via fd-table inspection but more importantly catches
#     missing rocksdb_close in the call graph).
mkdir build && cd build
../configure --enable-asan
make -j$(nproc)

# 2.  Start reffsd with rocksdb backend, capture initial lsof.
mkdir -p /tmp/reffs_fdtest_state /tmp/reffs_fdtest_data
./src/reffsd --config=examples/reffsd-rocksdb.toml &
sleep 3
RFD=$!
ls -l /proc/$RFD/fd > /tmp/fd-baseline.txt
wc -l /tmp/fd-baseline.txt

# 3.  Drive a workload (mount + git clone + make).
sudo mount -t nfs4 -o vers=4.2 localhost:/ /mnt/reffs
cd /mnt/reffs && git clone https://github.com/loghyr/reffs.git src
make -C src/build -j2 2>&1 | tail -1
cd / && sudo umount /mnt/reffs

# 4.  Capture post-workload lsof, diff.
ls -l /proc/$RFD/fd > /tmp/fd-post.txt
diff /tmp/fd-baseline.txt /tmp/fd-post.txt | grep "^>" | wc -l
diff /tmp/fd-baseline.txt /tmp/fd-post.txt | grep "^>" \
    | awk '{print $NF}' | sort | uniq -c | sort -rn | head -20
```

The last command outputs the FD-types that grew, with counts.
This is the diff that should drive the suspect-priority list.

## Unit test (deliverable)

New: `lib/backends/tests/rocksdb_fd_test.c`

| Test | Intent |
|------|--------|
| `test_rocksdb_sb_alloc_free_no_fd_leak` | Open/close per-sb RocksDB 100x in a loop; assert FD count via `count_open_fds()` does not grow more than +2 (slop for syslog/random). |
| `test_rocksdb_namespace_open_close_no_fd_leak` | Same for the namespace DB. |
| `test_rocksdb_iterator_no_leak` | Iterate `inodes` CF 100x, destroy each iterator; assert no FD growth. |
| `test_rocksdb_snapshot_no_leak` | Create + release snapshot 100x; assert no FD growth. |

`count_open_fds()` helper: read `/proc/self/fd/`, return entry
count.  Linux-only; macos `make check` skips these tests via
`HAVE_PROC_SELF_FD` autoconf check.

These tests catch the rocksdb-specific leaks but NOT the
io_uring leak (suspect 1).  For that, an io_uring-specific
test in `lib/io/tests/` that bringing up + tearing down N
connections asserts io_uring anon_inode count stays bounded.

## Acceptance

Soak_rocksdb FD growth across 30 min + 5 restarts <= soak_posix
FD growth (currently posix passes well within budget).  Stretch
goal: FD count at end == FD count after first restart (steady
state, no per-cycle accumulation).

## Out of scope

- Tuning RocksDB `max_open_files` (would mask the bug rather
  than fix it).
- Switching the soak threshold from "baseline + 50%" to a fixed
  number (the threshold catches real leaks; relaxing it is the
  wrong response).
- Investigating the kernel-side rpc_wait_bit_killable wedge --
  separate work, partially mitigated by the 30->180s mount
  timeout in `664fd5324209`.

## Related work

- Mount-timeout fix: `664fd5324209` (mitigates symptoms, not
  cause).
- `local_soak.sh` D-state grace tolerance: `eca4396a145c`
  (avoids false failures during the legitimate restart window).
- POSIX backend FD profile (passes under same soak): see
  `lib/backends/posix.c` -- `posix_sb_free` closes per-inode
  fd; reuse for the rocksdb cleanup symmetry check.
