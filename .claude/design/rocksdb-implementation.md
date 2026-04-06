<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

## Implementation Steps

### Step 1: Backend composition scaffolding

**File**: `lib/include/reffs/backend.h`
- Add `enum reffs_md_type`, `enum reffs_data_type`
- Add `reffs_backend_compose()` declaration
- `struct reffs_storage_ops` unchanged

**File**: `lib/backends/posix_data.c` (NEW)
- Move `posix_db_private`, `posix_db_alloc`, `posix_db_free`,
  `posix_db_release_resources`, `posix_db_read`, `posix_db_write`,
  `posix_db_resize`, `posix_db_get_size`, `posix_db_get_fd`
  from `posix.c`
- Export as `posix_data_ops` template

**File**: `lib/backends/ram_data.c` (NEW)
- Move `ram_db_private`, `ram_db_alloc`, `ram_db_free`,
  `ram_db_read`, `ram_db_write`, `ram_db_resize`,
  `ram_db_get_size`, `ram_db_get_fd` from `ram.c`
- Export as `ram_data_ops` template

**File**: `lib/backends/driver.c`
- `md_templates[]` and `data_templates[]` arrays
- `reffs_backend_compose()` implementation
- Constraint validation

**File**: `lib/fs/super_block.c`
- `super_block_alloc()`: map `reffs_storage_type` --> (md, data),
  call composer
- `super_block_release()`: free the composed ops struct

**Tests** (TDD -- written as part of this step, before the split):

**File**: `lib/backends/tests/compose_test.c` (NEW)

| Test | Intent |
|------|--------|
| `test_compose_ram_ram` | Compose RAM+RAM, verify all function pointers non-NULL |
| `test_compose_posix_posix` | Compose POSIX+POSIX, verify all function pointers non-NULL |
| `test_compose_ram_posix_rejected` | RAM md + POSIX data --> NULL (constraint violation) |
| `test_compose_posix_ram_rejected` | POSIX md + RAM data --> NULL (constraint violation) |
| `test_compose_posix_inode_roundtrip` | POSIX+POSIX: sb_alloc --> inode_alloc --> inode_sync --> inode_free, verify .meta and .dat created then cleaned up |
| `test_compose_posix_data_roundtrip` | POSIX+POSIX: db_alloc --> db_write --> db_read, verify content and fd valid |
| `test_compose_inode_free_cleans_both` | POSIX+POSIX: create inode with .meta + .dat, call inode_free, verify BOTH files unlinked |

Existing `make check` must also pass -- RAM and POSIX compositions
produce identical behavior to today's static ops.

### Step 2: configure.ac + Makefile.am (conditional RocksDB)

**File**: `configure.ac`
- Add `PKG_CHECK_MODULES([ROCKSDB], [rocksdb], [have_rocksdb=yes],
  [have_rocksdb=no])`
- `AM_CONDITIONAL([HAVE_ROCKSDB], [test "x$have_rocksdb" = xyes])`
- Report in summary

**File**: `lib/backends/Makefile.am`
- Add `posix_data.c` and `ram_data.c` unconditionally
- `if HAVE_ROCKSDB` block: add `rocksdb.c` to sources, add
  `$(ROCKSDB_CFLAGS)` to CFLAGS, `$(ROCKSDB_LIBS)` to LIBADD

**File**: `lib/backends/driver.c`
- `#ifdef HAVE_ROCKSDB` guard around rocksdb md template entry

**Docker packages**: Already present in both Dockerfiles (checked).

### Step 3: Key encoding utilities

**File**: `lib/backends/rocksdb_keys.h` (NEW, internal header)

```c
static inline void encode_be64(uint8_t *buf, uint64_t val);
static inline uint64_t decode_be64(const uint8_t *buf);
size_t rocksdb_key_ino(uint8_t *buf, size_t bufsz, uint64_t ino);
size_t rocksdb_key_dir(uint8_t *buf, size_t bufsz,
                       uint64_t parent_ino, uint64_t cookie);
size_t rocksdb_key_lnk(uint8_t *buf, size_t bufsz, uint64_t ino);
size_t rocksdb_key_lay(uint8_t *buf, size_t bufsz, uint64_t ino);
size_t rocksdb_key_chk(uint8_t *buf, size_t bufsz,
                       uint64_t ino, uint64_t block_offset);
```

### Step 4: RocksDB backend -- sb_alloc / sb_free

**File**: `lib/backends/rocksdb.c` (NEW)

```c
struct rocksdb_sb_private {
    rocksdb_t *rsp_db;
    rocksdb_column_family_handle_t *rsp_cf[ROCKSDB_CF_COUNT];
    rocksdb_options_t *rsp_opts;
    rocksdb_writeoptions_t *rsp_wopts;    /* sync=true */
    rocksdb_writeoptions_t *rsp_wopts_nosync; /* for data writes */
    rocksdb_readoptions_t *rsp_ropts;
    char *rsp_path;
};
```

`rocksdb_sb_alloc()`:
1. Build path: `<backend_path>/sb_<id>/md.rocksdb/`
   (the `sb_<id>/` parent is created by the data backend's
   `posix_db_alloc` or by `super_block_alloc` itself)
2. `mkdir -p` the md.rocksdb directory
3. Create RocksDB options (create_if_missing=true)
4. Open with column families (create missing CFs)
5. Store handles in `sb->sb_storage_private`
6. Read `sb_meta` key for `sb_next_ino` if exists

`rocksdb_sb_free()`:
1. Close all CF handles
2. Close DB
3. Free options and private struct

### Step 5: RocksDB backend -- inode ops

`rocksdb_inode_alloc()`:
- This is the vtable `inode_alloc` hook, called from `inode_alloc()`
  AFTER the inode is hashed.  Same pattern as `posix_inode_alloc()`
  which loads from disk inside the hook.
- `rocksdb_get()` from `inodes` CF with key `ino:<ino>`
- If found: deserialize `inode_disk` into inode fields
- Also load symlink from `symlinks` CF, layout from `layouts` CF
- If not found: new inode (return 0, caller initializes)
- Data file loading (`ino_<ino>.dat`) is handled by the composed
  data backend -- `rocksdb_inode_alloc` only loads metadata from
  RocksDB.  The data backend's `db_alloc` opens the fd separately.

`rocksdb_inode_free()` (md-side only -- data cleanup is composed):
- Delete keys from `inodes`, `symlinks`, `layouts` CFs
- WriteBatch for atomicity
- The composed `inode_free` wrapper calls this, then calls
  `posix_data_inode_cleanup()` to unlink the `.dat` file

`rocksdb_inode_sync()`:
- Build a single WriteBatch for atomicity across CFs:
  - Serialize inode to `inode_disk`, put to `inodes` CF
  - If symlink: put to `symlinks` CF
  - If layout segments: put to `layouts` CF
  - Update `sb_meta` key with current `sb_next_ino`
- `rocksdb_write(db, wopts_sync, batch)` -- one atomic write
- Directory sync is separate (called from `dirent_sync_to_disk`,
  not from `inode_sync`) -- see Step 6

### Step 6: Data block ops -- automatic via composition

**No new code needed.**  The composer (Step 1) wires
`REFFS_DATA_POSIX` functions into the RocksDB backend's ops
struct.  The POSIX data functions from `posix_data.c` are used
directly -- same fd-backed pread/pwrite/ftruncate, same io_uring
support.

The only concern is the data file path.  The POSIX data backend
needs to know the sb's data directory.  Two options:

(a) The md backend's `sb_alloc` stores the data directory path
    in `sb->sb_storage_private` at a known offset, and the data
    backend reads it.  Couples the two.

(b) `sb->sb_backend_path` is the data directory root.  The md
    backend manages its own subdirectory within it.  Layout:
    ```
    <backend_path>/sb_<id>/
    ├── md.rocksdb/      <-- rocksdb_sb_alloc creates this
    └── ino_<N>.dat      <-- posix_data uses sb_<id>/ directly
    ```
    The POSIX data functions already build paths from
    `sb->sb_backend_path` + `sb_id` -- no change needed.

Option (b) is cleaner: the data backend owns the top-level sb
directory, the md backend creates a subdirectory inside it.
Both POSIX-md and RocksDB-md coexist with the same data layout.

### Step 7: RocksDB backend -- directory ops

`rocksdb_dir_sync()`:
- Create iterator with prefix `dir:<parent_ino>:`, collect all
  existing keys (RocksDB has no prefix-delete)
- Build WriteBatch: delete each collected key, then put each
  current child entry
- Atomic `rocksdb_write()` -- old entries removed and new entries
  written in one operation
- Each entry value: `{child_ino(8), name_len(2), name(variable)}`
- Thread safety: `dir_sync` is called from `dirent_sync_to_disk()`
  which is protected by the parent dirent's `rd_lock` -- no
  concurrent modification of the same directory's keys

`rocksdb_dir_find_entry_by_ino()`:
- Create iterator with prefix `dir:<parent_ino>:`
- Scan values for matching child_ino

`rocksdb_dir_find_entry_by_name()`:
- Same prefix scan, match on name in value

### Step 8: RocksDB recovery -- reffs_fs_recover() integration

**File**: `lib/fs/fs.c`

Today `reffs_fs_recover()` only handles `REFFS_STORAGE_POSIX` -- it
scans `sb_<id>/` for `ino_*.meta` files, then walks `.dir` files
recursively to rebuild the in-memory inode/dirent tree.

For RocksDB, add a parallel recovery path:

`reffs_fs_recover_rocksdb(sb)`:
1. Iterate the `inodes` CF to find the max inode number --> set
   `sb->sb_next_ino` (replaces the POSIX `scandir` of `.meta` files)
2. The root inode already exists in memory (created by
   `super_block_dirent_create()` before recovery).  Call
   `sb->sb_ops->inode_alloc(sb->sb_root_inode)` to load its
   fields from RocksDB -- same pattern as `reffs_fs_recover()` for
   POSIX.  Do NOT hardcode inode number 1.
3. Call `recover_directory_recursive_rocksdb(sb->sb_dirent)`:
   - Iterate `dirs` CF with prefix `dir:<root_ino>:` to get
     all children of the root directory
   - For each child: `inode_alloc(sb, child_ino)` --> loads from
     `inodes` CF via the hook
   - Attach inode to dirent via `dirent_attach_inode()`
   - Recurse into subdirectories

The recursive recovery function mirrors `recover_directory_recursive()`
but reads from RocksDB instead of `.dir` files.

Ordering: the namespace database (server_state, registry) MUST be
loaded before per-sb databases, since registry load creates the
sb structs that per-sb recovery operates on.

Dispatch in `reffs_fs_recover()`:
```c
if (sb->sb_ops->type == REFFS_STORAGE_POSIX)
    reffs_fs_recover_posix(sb);
else if (sb->sb_ops->type == REFFS_STORAGE_ROCKSDB)
    reffs_fs_recover_rocksdb(sb);
```

### Step 9: RocksDB backend -- chunk ops

`rocksdb_chunk_persist()`:
- WriteBatch: for each non-EMPTY block, put `chk:<ino>:<offset>`
- Delete keys for blocks that became EMPTY (if any)

`rocksdb_chunk_load()`:
- Iterator with prefix `chk:<ino>:`
- Build block array from values

### Step 10: Namespace database for ALL server-wide state

**File**: `lib/backends/rocksdb_namespace.c` (NEW)

This replaces ALL server-wide flat-file persistence when RocksDB
is the backend.  The existing flat-file code remains for POSIX/RAM.

```c
struct rocksdb_namespace {
    rocksdb_t *rn_db;
    rocksdb_column_family_handle_t *rn_cf_default;
    rocksdb_column_family_handle_t *rn_cf_registry;
    rocksdb_column_family_handle_t *rn_cf_clients;
    rocksdb_column_family_handle_t *rn_cf_incarnations;
    rocksdb_column_family_handle_t *rn_cf_identity_domains;
    rocksdb_column_family_handle_t *rn_cf_identity_map;
    rocksdb_column_family_handle_t *rn_cf_nsm;
    /* ... options ... */
};
```

#### 10a. Server state
`rocksdb_ns_server_state_save/load()`:
- Key `"server_state"` in default CF
- Value: `struct server_persistent_state` (same binary format)

#### 10b. SB registry
`rocksdb_ns_registry_save/load()`:
- Header: key `"sb_registry_header"` in default CF
- Entries: key `sbreg:<id>` in `registry` CF

#### 10c. Client identity log
`rocksdb_ns_client_identity_append/load()`:
- Key: `clid:<slot BE>` in `clients` CF
- Value: `struct client_identity_record` (write-once per slot)
- Replaces: append-only `clients` flat file
- Load: iterate `clients` CF, call callback per record

#### 10d. Client incarnations
`rocksdb_ns_client_incarnation_add/remove/load()`:
- Key: `cinc:<slot BE>` in `incarnations` CF
- Value: `struct client_incarnation_record`
- Add: single `rocksdb_put`
- Remove: single `rocksdb_delete`
- Load: iterate `incarnations` CF into caller's array
- Replaces: the symlink-swap A/B file dance -- RocksDB gives
  us atomic per-key writes natively, no double-buffering needed

#### 10e. Identity domains (stubs -- identity phase 2 not started)
`rocksdb_ns_identity_domain_persist/load()`:
- Key: `idom:<index BE>` in `identity_domains` CF
- Value: domain record (name, type, flags)
- Replaces: `identity_domains` flat file
- CF created at open time; persist/load are stubs until phase 2

#### 10f. Identity mappings (stubs -- identity phase 2 not started)
`rocksdb_ns_identity_map_persist/load()`:
- Key: `imap:<reffs_id BE>` in `identity_map` CF
- Value: mapping record (aliases, name, flags)
- Replaces: `identity_map` flat file
- CF created at open time; persist/load are stubs until phase 2

#### 10g. NSM state
`rocksdb_ns_nsm_save/load()`:
- Key: `"sm_state"` in `nsm` CF
- Value: `uint32_t` sm_state counter
- Replaces: `./statd.state` flat file

### Step 11: Persistence dispatch layer

Today, callers invoke persistence functions directly:
```c
server_persist_save(ss->ss_state_dir, &sps);
client_identity_append(ss->ss_state_dir, &cir);
client_incarnation_add(ss->ss_state_dir, &crc);
```

For RocksDB, these need to dispatch to the namespace database
instead of flat files.

**Approach**: a `struct persist_ops` vtable on `server_state`:

```c
struct persist_ops {
    /* Server state */
    int (*server_state_save)(void *ctx,
                             const struct server_persistent_state *sps);
    int (*server_state_load)(void *ctx,
                             struct server_persistent_state *sps);

    /* SB registry */
    int (*registry_save)(void *ctx);
    int (*registry_load)(void *ctx);
    uint64_t (*registry_alloc_id)(void *ctx);

    /* Client identity */
    int (*client_identity_append)(void *ctx,
                                  const struct client_identity_record *cir);
    int (*client_identity_load)(void *ctx,
                                int (*cb)(...), void *arg);

    /* Client incarnations */
    int (*client_incarnation_add)(void *ctx,
                                  const struct client_incarnation_record *crc);
    int (*client_incarnation_remove)(void *ctx, uint32_t slot);
    int (*client_incarnation_load)(void *ctx,
                                   struct client_incarnation_record *recs,
                                   size_t max, size_t *count);

    /* Identity domains (stubs until identity phase 2) */
    int (*identity_domain_persist)(void *ctx, ...);
    int (*identity_domain_load)(void *ctx, ...);

    /* Identity mappings (stubs until identity phase 2) */
    int (*identity_map_persist)(void *ctx, ...);
    int (*identity_map_load)(void *ctx, ...);

    /* NSM state */
    int (*nsm_state_save)(void *ctx, uint32_t sm_state);
    int (*nsm_state_load)(void *ctx, uint32_t *sm_state);
};
```

- `ss->ss_persist_ops` set at startup based on `[backend] type`
- `ss->ss_persist_ctx` is either `char *state_dir` (flat-file) or
  `struct rocksdb_namespace *` (RocksDB)
- Existing flat-file functions become the `flatfile_persist_ops`
  implementation (thin wrappers)
- RocksDB functions become the `rocksdb_persist_ops` implementation

Callers change from:
```c
client_identity_append(ss->ss_state_dir, &cir);
```
to:
```c
ss->ss_persist_ops->client_identity_append(ss->ss_persist_ctx, &cir);
```

This is a mechanical refactor of existing call sites, not new logic.

### Step 12: Extend chunk_store.c with backend dispatch

**File**: `lib/include/reffs/backend.h`
- Add `chunk_persist` and `chunk_load` to `reffs_storage_ops`

**File**: `lib/nfs4/server/chunk_store.c`
- In `chunk_store_persist()`: check `sb->sb_ops->chunk_persist`,
  call it if non-NULL, else existing flat-file code
- In `chunk_store_load()`: same dispatch

### Step 13: Wire up driver.c + config

**File**: `lib/backends/driver.c`
- `#ifdef HAVE_ROCKSDB`: add `extern rocksdb_storage_ops`,
  set `backends[REFFS_STORAGE_ROCKSDB]`

**File**: `src/reffsd.c` (or config parser)
- Accept `storage = "rocksdb"` in TOML config
- Probe SB_CREATE already accepts storage type

### Step 14: Unit tests

**File**: `lib/backends/tests/rocksdb_test.c` (NEW, conditional)
**File**: `lib/backends/tests/Makefile.am` (NEW -- wire into build)

Both guarded by `if HAVE_ROCKSDB` in the Makefile.

| Test | Intent |
|------|--------|
| `test_compose_rocksdb_posix` | Compose RocksDB+POSIX, verify md=rocksdb and data=posix fn ptrs |
| `test_rocksdb_sb_alloc_free` | Open/close database, verify md.rocksdb/ created |
| `test_rocksdb_inode_roundtrip` | Write inode_disk, read back, compare all fields |
| `test_rocksdb_data_posix_roundtrip` | Write data via db_write, read via db_read, verify fd is valid |
| `test_rocksdb_data_resize` | Write, truncate shorter, verify size; extend, verify zeros (POSIX path) |
| `test_rocksdb_dir_roundtrip` | Sync directory with 3 entries, find by name, find by ino |
| `test_rocksdb_dir_overwrite` | Sync, add entry, re-sync, verify old entries + new entry |
| `test_rocksdb_symlink_roundtrip` | Write symlink target, read back, compare |
| `test_rocksdb_layout_roundtrip` | Write layout segments, read back, compare |
| `test_rocksdb_chunk_roundtrip` | Persist chunks, load, compare per-block state |
| `test_rocksdb_chunk_sparse` | Only non-EMPTY blocks persisted; EMPTY gaps don't appear |
| `test_rocksdb_key_ordering` | Verify BE64 keys sort correctly (ino 1 < ino 256 < ino 65536) |
| `test_rocksdb_ns_server_state` | Save/load server_persistent_state via namespace DB |
| `test_rocksdb_ns_registry` | Save/load registry entries via namespace DB |
| `test_rocksdb_ns_client_identity` | Append + load client identity records |
| `test_rocksdb_ns_incarnation_add_remove` | Add, remove, load incarnation records |
| `test_rocksdb_ns_incarnation_survives_reopen` | Add record, close, reopen, load -- verify |
| `test_rocksdb_ns_identity_domains` | Persist + load domain table |
| `test_rocksdb_ns_identity_map` | Persist + load bidirectional mappings |
| `test_rocksdb_ns_nsm_state` | Save + load sm_state counter |
| `test_rocksdb_inode_survives_reopen` | Write inode, close DB, reopen DB, load inode, compare |
| `test_rocksdb_recovery` | Create sb with inodes+dirs, close, reopen, run `reffs_fs_recover_rocksdb()`, verify tree |
| `test_rocksdb_inode_delete_cleanup` | RocksDB+POSIX: create inode, write data, inode_free --> verify RocksDB keys deleted AND .dat file unlinked |
| `test_rocksdb_open_corrupt` | Corrupt the MANIFEST file in md.rocksdb/, attempt sb_alloc --> returns error, no crash |

All roundtrip tests MUST close and reopen the database between
write and read to verify WAL recovery, not just in-memory caching.

**Existing test impact**: NONE.  All new code, new backend, new tests.
Existing POSIX/RAM tests are unchanged.

### Step 15: Integration -- existing tests with RocksDB

After unit tests pass, run the existing `sb_lifecycle_test`,
`sb_persistence_test`, and `sb_mount_crossing_test` with
`REFFS_STORAGE_ROCKSDB` instead of `REFFS_STORAGE_POSIX`.
This requires a test helper that parameterizes the storage type.

Minimal parameterization: duplicate `sb_persistence_test.c` as
`sb_persistence_rocksdb_test.c` with `REFFS_STORAGE_ROCKSDB` and
add `uuid_generate()` after alloc in helpers.  This exercises the
full superblock lifecycle (alloc --> sync --> free --> reload) through
the RocksDB backend without building a full parameterized harness.

NOT_NOW_BROWN_COW: full parameterized test harness that runs ALL
existing tests against ALL backends.

