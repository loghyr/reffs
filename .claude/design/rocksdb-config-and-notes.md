<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

## RocksDB C API Notes

RocksDB ships a C API (`rocksdb/c.h`).  Key functions:

```c
rocksdb_t *rocksdb_open(options, path, &err);
rocksdb_t *rocksdb_open_column_families(options, path, num_cf,
    cf_names, cf_options, cf_handles, &err);
void rocksdb_put_cf(db, wopts, cf, key, klen, val, vlen, &err);
char *rocksdb_get_cf(db, ropts, cf, key, klen, &vlen, &err);
void rocksdb_delete_cf(db, wopts, cf, key, klen, &err);
rocksdb_iterator_t *rocksdb_create_iterator_cf(db, ropts, cf);
rocksdb_writebatch_t *rocksdb_writebatch_create(void);
void rocksdb_writebatch_put_cf(wb, cf, key, klen, val, vlen);
void rocksdb_write(db, wopts, wb, &err);
```

Error handling: `err` is a `char *` set on failure (caller must
`rocksdb_free(err)`).  NULL = success.

## Configuration

```toml
[backend]
type = "rocksdb"              # or "posix" or "ram"
path = "/var/lib/reffs/data"  # backend_path for per-sb databases
state_file = "/var/lib/reffs/mds"  # state_dir for namespace DB

[rocksdb]
# Optional tuning (all have sane defaults)
write_buffer_size = 67108864      # 64MB
max_write_buffer_number = 3
target_file_size_base = 67108864  # 64MB
compression = "lz4"              # lz4, snappy, zstd, none
sync_on_metadata = true           # fsync on inode/dir writes
sync_on_data = false              # async for file data writes
```

## Migration Path

No deployed persistent storage exists (CLAUDE.md deployment status).
All on-disk formats are version 1.  The RocksDB backend is a new
storage type selection, not a migration from POSIX.  An admin creates
a RocksDB-backed superblock via:

```bash
reffs-probe.py sb-create --path /data --storage rocksdb
```

Existing POSIX superblocks continue to work.  Mixed backends in the
same namespace are supported (different sbs can use different
backends).

NOT_NOW_BROWN_COW: POSIX-to-RocksDB migration tool (offline,
reads POSIX files, writes RocksDB keys).

## Persistence Guarantees

| Operation | POSIX Backend | RocksDB Backend |
|-----------|--------------|-----------------|
| inode_sync | write-temp/fdatasync/rename | RocksDB WriteBatch (WAL + sync) |
| dir_sync | write-temp/fdatasync/rename | RocksDB WriteBatch (atomic) |
| chunk_persist | write-temp/fdatasync/rename | RocksDB WriteBatch (atomic) |
| data write | pwrite to .dat file | pwrite to .dat file (same) |
| server_state | write-temp/fdatasync/rename | RocksDB put to namespace DB |
| sb_registry | write-temp/fdatasync/rename | RocksDB WriteBatch to namespace DB |
| client state | append / symlink-swap | RocksDB put/delete to namespace DB |

RocksDB's WAL provides crash safety for metadata.
`sync_on_metadata = true` ensures
`rocksdb_writeoptions_set_sync(wopts, 1)` for metadata writes.
Data writes use the same POSIX pwrite path as today -- crash
safety depends on the underlying filesystem (same as current
POSIX backend).

## Security / Licensing

RocksDB is Apache-2.0 licensed.  Apache-2.0 is compatible with
AGPL-3.0-or-later (one-way: AGPL can incorporate Apache-2.0 code,
the combined work is AGPL).  No licensing concern.

## Deferred Items

- **NOT_NOW_BROWN_COW**: Atomic WriteBatch for inode + chunk + dir
  in a single transaction
- **NOT_NOW_BROWN_COW**: `REFFS_DATA_XFS` data backend for
  CLONE/reflink/SEEK_HOLE (new data axis, compose with any md)
- **NOT_NOW_BROWN_COW**: Parameterized test harness to run existing
  tests against all backends
- **NOT_NOW_BROWN_COW**: POSIX-to-RocksDB offline migration tool
- **NOT_NOW_BROWN_COW**: RocksDB statistics/metrics export via probe
- **NOT_NOW_BROWN_COW**: Column family tuning per workload profile
- **NOT_NOW_BROWN_COW**: Backup/checkpoint API for online sb snapshot
- **NOT_NOW_BROWN_COW**: Custom extent allocator data backend
  (`REFFS_DATA_EXTENT`) on a raw file or block device -- full
  control over CLONE/CoW/punch semantics without host fs dependency

## Verification

1. `make -j$(nproc)` -- zero errors, zero warnings
2. `make check` -- all existing tests pass (POSIX/RAM unchanged)
3. New RocksDB unit tests pass (conditional on `HAVE_ROCKSDB`)
4. Start reffsd with `type = "rocksdb"`, create sb via probe,
   write files via NFS, restart, verify data survives
5. Chunk write/read via ec_demo with RocksDB-backed DS
6. `make -f Makefile.reffs fix-style` + `license` -- clean
