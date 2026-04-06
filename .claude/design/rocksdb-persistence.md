<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

## Complete Persistence Inventory

Every piece of persistent state must be accounted for.  When
`[backend] type = "rocksdb"`, ALL persistence moves to RocksDB.

### Per-Superblock (--> per-sb RocksDB + POSIX data files)

| State | POSIX backend path | RocksDB backend | Source |
|-------|-------------------|-----------------|--------|
| Inode metadata | `sb_<id>/ino_<N>.meta` | RocksDB `inodes:ino:<N>` | `posix.c` |
| File data | `sb_<id>/ino_<N>.dat` | **POSIX** `sb_<id>/ino_<N>.dat` (same) | `posix_data.c` |
| Directory entries | `sb_<id>/ino_<N>.dir` | RocksDB `dirs:dir:<N>:<cookie>` | `posix.c` |
| Symlink targets | `sb_<id>/ino_<N>.lnk` | RocksDB `symlinks:lnk:<N>` | `posix.c` |
| Layout segments | `sb_<id>/ino_<N>.layouts` | RocksDB `layouts:lay:<N>` | `posix.c` |
| Chunk metadata | `chunks/<N>.meta` | RocksDB `chunks:chk:<N>:<off>` | `chunk_store.c` |

Note: file data stays in POSIX `.dat` files at the same path
regardless of md backend.  RocksDB handles metadata; POSIX data
backend handles bulk data.  The entire `sb_<id>/` directory
(md.rocksdb/ + .dat files) is the migration unit.

### Server-Wide (--> namespace RocksDB database)

| State | Flat-file path | RocksDB CF:key | Source |
|-------|---------------|----------------|--------|
| Server state | `server_state` | `default:"server_state"` | `server_persist.c` |
| SB registry header | `superblocks.registry` | `default:"sb_registry_header"` | `sb_registry.c` |
| SB registry entries | `superblocks.registry` | `registry:sbreg:<id>` | `sb_registry.c` |
| Client identities | `clients` | `clients:clid:<slot>` | `client_persist.c` |
| Client incarnations | `client_incarnations{,.A,.B}` | `incarnations:cinc:<slot>` | `client_persist.c` |
| Identity domains | `identity_domains` | `identity_domains:idom:<idx>` | `identity_domain.c` |
| Identity mappings | `identity_map` | `identity_map:imap:<id>` | `identity_map.c` |
| NSM state | `./statd.state` | `nsm:"sm_state"` | `nsm_server.c` |

## Chunk Store Integration

Today, `chunk_store.c` writes to `<state_dir>/chunks/<ino>.meta`
using its own persistence code (write-temp/fdatasync/rename).  This
is independent of the storage backend.

For RocksDB superblocks, chunk metadata should live in the per-sb
database (CF `chunks`) so it migrates with the sb.

### Approach: Backend-Aware Chunk Persistence

Add two optional function pointers to `reffs_storage_ops`:

```c
/* Persist chunk block metadata for an inode.  If NULL, chunk_store
 * falls back to its existing flat-file persistence. */
int (*chunk_persist)(struct super_block *sb, uint64_t ino,
                     const struct chunk_block *blocks,
                     uint32_t nblocks, uint32_t chunk_size);

/* Load chunk block metadata for an inode.  Returns 0 and sets
 * *blocks_out/*nblocks_out on success, -ENOENT if no chunks
 * stored, other -errno on error. */
int (*chunk_load)(struct super_block *sb, uint64_t ino,
                  struct chunk_block **blocks_out,
                  uint32_t *nblocks_out, uint32_t *chunk_size_out);
```

RAM and POSIX backends set these to NULL (existing behavior).
RocksDB implements them using the `chunks` column family.

`chunk_store_persist()` and `chunk_store_load()` check
`inode->i_sb->sb_ops->chunk_persist` first; if NULL, use the
existing flat-file code path.

### NOT_NOW_BROWN_COW: Atomic Inode + Chunk Writes

WriteBatch could atomically update inode metadata and chunk state
in a single RocksDB write.  Deferred -- the current per-op
persistence is sufficient and matches the POSIX backend's
granularity.

