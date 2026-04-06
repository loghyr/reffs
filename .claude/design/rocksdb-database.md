<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

## Database Layout

### Per-Superblock Database

```
<backend_path>/sb_<id>/
├── md.rocksdb/               <-- metadata (RocksDB md backend)
├── ino_1.dat                 <-- bulk file data (POSIX data backend)
├── ino_2.dat
└── ...
```

With POSIX md + POSIX data (current behavior, unchanged):
```
<backend_path>/sb_<id>/
├── ino_1.meta                <-- metadata (POSIX md backend)
├── ino_1.dat                 <-- data (POSIX data backend)
├── ino_1.dir
└── ...
```

The `.dat` files are in the same place regardless of md backend.
Only the metadata storage changes.

RocksDB column families (metadata only):

| CF | Key | Value | Notes |
|----|-----|-------|-------|
| `default` | `"sb_meta"` | sb metadata (next_ino, etc.) | Single key |
| `inodes` | `ino:<uint64 BE>` | `struct inode_disk` | Big-endian key for ordered iteration |
| `dirs` | `dir:<parent_ino BE>:<cookie BE>` | `{child_ino, name_len, name}` | Ordered by cookie within parent |
| `symlinks` | `lnk:<uint64 BE>` | symlink target string | |
| `layouts` | `lay:<uint64 BE>` | layout segment header + data files | Same format as `.layouts` file body |
| `chunks` | `chk:<uint64 BE>:<block_offset BE>` | `struct chunk_block_disk` | Per-block chunk metadata |

Bulk file data lives in POSIX files under `data/`, NOT in RocksDB.
The `db_*` functions (read, write, resize, get_fd) operate on these
files directly -- same code as the POSIX backend's data path.
`db_get_fd()` returns a real fd, so io_uring async I/O works.

Key encoding: all integer keys are 8-byte big-endian for correct
lexicographic ordering in RocksDB.  This is critical -- RocksDB
sorts keys as byte strings.

### RocksDB Error Handling Convention

RocksDB's C API sets `char *err` on failure (caller must
`rocksdb_free(err)`).  Use a helper macro throughout:

```c
#define ROCKSDB_CHECK_ERR(err, ret_val, label) do {          \
    if (err) {                                                \
        LOG("rocksdb: %s", err);                              \
        rocksdb_free(err);                                    \
        err = NULL;                                           \
        ret = (ret_val);                                      \
        goto label;                                           \
    }                                                         \
} while (0)
```

Every `rocksdb_put`, `rocksdb_get`, `rocksdb_write`, `rocksdb_open`
call must be followed by `ROCKSDB_CHECK_ERR`.  Leaking an error
string is an ASAN finding.

### Namespace Database

Location: `<state_dir>/namespace.rocksdb/`

Column families:

| CF | Key | Value | Notes |
|----|-----|-------|-------|
| `default` | `"server_state"` | `struct server_persistent_state` | Single key |
| `default` | `"sb_registry_header"` | `struct sb_registry_header` | next_id counter |
| `registry` | `sbreg:<uint64 BE>` | `struct sb_registry_entry` | Per-sb entry |
| `clients` | `clid:<slot BE>` | `struct client_identity_record` | Stable identity, write-once |
| `incarnations` | `cinc:<slot BE>` | `struct client_incarnation_record` | Current active clients |
| `identity_domains` | `idom:<index BE>` | domain name + type + flags | Domain table |
| `identity_map` | `imap:<reffs_id BE>` | mapping record (aliases, name) | Bidirectional identity mappings |
| `nsm` | `"sm_state"` | `uint32_t` | NSM sm_state counter |

The namespace database replaces ALL server-wide flat files:
- `<state_dir>/server_state`
- `<state_dir>/superblocks.registry`
- `<state_dir>/clients` (append-only identity log)
- `<state_dir>/client_incarnations{,.A,.B}` (active client state)
- `<state_dir>/identity_domains`
- `<state_dir>/identity_map`
- `./statd.state` (NSM counter)

### Namespace DB selection rule

The namespace database is selected by `[backend] type` in the TOML
config, NOT by individual sb storage types.  This is a server-wide
setting:

- `type = "rocksdb"` --> namespace DB is RocksDB, default storage
  for new sbs is RocksDB (probe SB_CREATE can override per-sb)
- `type = "posix"` --> namespace DB is flat files, default storage
  for new sbs is POSIX
- `type = "ram"` --> no persistence (existing behavior)

Mixed backends are supported: a server with `type = "rocksdb"` can
have individual POSIX-backed sbs (created via probe with explicit
storage type).  The namespace DB is always RocksDB in this case.
The reverse (RocksDB sbs on a POSIX-namespace server) is NOT
supported -- this avoids the ambiguity of two persistence authorities.

### Why Not One Big Database?

A single database per namespace would be simpler but defeats the
migration goal.  With per-sb databases:

```bash
# Migrate sb 42 from machine A to machine B:
# The entire sb_42/ directory (RocksDB metadata + POSIX data files)
rsync -a /var/lib/reffs/data/sb_42/ machineB:/var/lib/reffs/data/sb_42/
# Then on machine B: probe sb-create --import /var/lib/reffs/data/sb_42/
```

The namespace database stays on the original machine.  Only the
self-contained per-sb directory (metadata + data) moves.

