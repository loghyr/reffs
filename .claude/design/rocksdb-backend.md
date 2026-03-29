# RocksDB Storage Backend

## Problem

The POSIX backend stores each inode's metadata, data, directory listing,
and layout segments as separate files (`ino_<N>.meta`, `.dat`, `.dir`,
`.layouts`) under `<backend_path>/sb_<id>/`.  This works but:

- Thousands of small files per superblock (one set per inode)
- No transactional writes across related objects
- `readdir` + `stat` overhead for recovery/enumeration
- Chunk metadata lives outside the backend (`<state_dir>/chunks/`)
  rather than co-located with the inode data it describes

RocksDB gives us ordered key-value storage with atomic WriteBatch,
column families for logical separation, and built-in compression —
all in a single directory per database.

## Design Principles

1. **One RocksDB database per superblock** — the sb is the unit of
   migration.  `tar` the sb's RocksDB directory, move it to another
   machine, load it there.

2. **Separate databases for cross-sb concerns** — server state
   (boot_seq, slot_next, uuid) and the sb registry (next_id, sb
   entries) live in a **namespace database** shared by all sbs in
   a connected namespace.  This keeps per-sb databases self-contained
   while centralizing the counters that span sbs.

3. **Chunk store co-located with inode data** — when the backend is
   RocksDB, chunk metadata lives in the same per-sb database as the
   inode it belongs to (column family `chunks`), not in a separate
   `<state_dir>/chunks/` directory.  This makes sb migration
   self-contained for data servers.

4. **Column families for logical separation** — each object type
   gets its own column family within the per-sb database.  This
   allows independent compaction tuning and clean prefix iteration.

5. **Metadata and data are independent backend axes** — the vtable
   struct (`reffs_storage_ops`) is unchanged, callers are unchanged,
   but function pointers are composed at runtime from two independent
   sources: a **metadata backend** (RAM, POSIX, RocksDB) and a
   **data backend** (RAM, POSIX, future XFS/extent).  Adding a new
   data backend never touches any metadata backend, and vice versa.

6. **Runtime composition, not vtable split** — `reffs_backend_compose()`
   allocates a `reffs_storage_ops` and fills md function pointers
   from one source and data function pointers from another.  The
   single flat vtable stays.  `data_block.c` still does
   `db->db_ops = sb->sb_ops` — zero caller changes.

7. **Constraint: md=RAM ↔ data=RAM** — if metadata is RAM, data
   must be RAM (no persistence for either).  If metadata is not
   RAM, data must not be RAM (partial persistence is incoherent).
   Enforced in `reffs_backend_compose()`.

8. **Host byte order** — like the POSIX backend, on-disk format is
   not portable across architectures.  The migration target must be
   the same endianness.  This matches the existing server_state and
   registry formats.

9. **Data layer is a future decision point** — POSIX files for data
   means we inherit the host filesystem's feature set.  When
   CLONE/COPY/SEEK_HOLE/punch/EXCHANGE_RANGE are needed, the
   options are: (a) require XFS (reflink, SEEK_HOLE native),
   (b) custom extent allocator on raw file/block device.  Adding
   either is a new `reffs_data_type` — compose with any md backend,
   no md code changes.

## Database Layout

### Per-Superblock Database

```
<backend_path>/sb_<id>/
├── md.rocksdb/               ← metadata (RocksDB md backend)
├── ino_1.dat                 ← bulk file data (POSIX data backend)
├── ino_2.dat
└── ...
```

With POSIX md + POSIX data (current behavior, unchanged):
```
<backend_path>/sb_<id>/
├── ino_1.meta                ← metadata (POSIX md backend)
├── ino_1.dat                 ← data (POSIX data backend)
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
files directly — same code as the POSIX backend's data path.
`db_get_fd()` returns a real fd, so io_uring async I/O works.

Key encoding: all integer keys are 8-byte big-endian for correct
lexicographic ordering in RocksDB.  This is critical — RocksDB
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

- `type = "rocksdb"` → namespace DB is RocksDB, default storage
  for new sbs is RocksDB (probe SB_CREATE can override per-sb)
- `type = "posix"` → namespace DB is flat files, default storage
  for new sbs is POSIX
- `type = "ram"` → no persistence (existing behavior)

Mixed backends are supported: a server with `type = "rocksdb"` can
have individual POSIX-backed sbs (created via probe with explicit
storage type).  The namespace DB is always RocksDB in this case.
The reverse (RocksDB sbs on a POSIX-namespace server) is NOT
supported — this avoids the ambiguity of two persistence authorities.

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

## Complete Persistence Inventory

Every piece of persistent state must be accounted for.  When
`[backend] type = "rocksdb"`, ALL persistence moves to RocksDB.

### Per-Superblock (→ per-sb RocksDB + POSIX data files)

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

### Server-Wide (→ namespace RocksDB database)

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
in a single RocksDB write.  Deferred — the current per-op
persistence is sufficient and matches the POSIX backend's
granularity.

## Backend Composition

### Two axes, one vtable

```c
/* Metadata backends — own inode/dir/symlink/layout persistence */
enum reffs_md_type {
    REFFS_MD_RAM     = 0,
    REFFS_MD_POSIX   = 1,
    REFFS_MD_ROCKSDB = 2,
};

/* Data backends — own bulk file I/O (db_* functions) */
enum reffs_data_type {
    REFFS_DATA_RAM   = 0,
    REFFS_DATA_POSIX = 1,
    /* future: REFFS_DATA_XFS, REFFS_DATA_EXTENT */
};
```

Each axis provides a **template** — a partial `reffs_storage_ops`
with only its half of the function pointers filled in:

```c
/* Metadata templates (inode/dir/symlink/layout/chunk + sb lifecycle) */
struct reffs_md_ops {
    int  (*sb_alloc)(struct super_block *sb, const char *backend_path);
    void (*sb_free)(struct super_block *sb);
    int  (*inode_alloc)(struct inode *inode);
    void (*inode_free)(struct inode *inode);  /* md-side cleanup only */
    void (*inode_sync)(struct inode *inode);
    void (*dir_sync)(struct inode *inode);
    int  (*dir_find_entry_by_ino)(...);
    int  (*dir_find_entry_by_name)(...);
    int  (*chunk_persist)(...);   /* NULL for RAM/POSIX */
    int  (*chunk_load)(...);      /* NULL for RAM/POSIX */
};

/* Data templates (bulk file I/O + data-side cleanup) */
struct reffs_data_ops {
    int     (*db_alloc)(...);
    void    (*db_free)(struct data_block *db);
    void    (*db_release_resources)(struct data_block *db);
    ssize_t (*db_read)(...);
    ssize_t (*db_write)(...);
    ssize_t (*db_resize)(...);
    size_t  (*db_get_size)(...);
    int     (*db_get_fd)(struct data_block *db);
    void    (*data_inode_cleanup)(struct inode *inode);  /* unlink .dat */
};
```

These are **not** exposed to callers.  They are internal to
`driver.c`, used only by the composer.

### The composer

```c
/*
 * Compose a reffs_storage_ops from independent md + data backends.
 * Returns a heap-allocated ops struct (freed in sb_release).
 * Returns NULL on invalid combination (logs error).
 *
 * Constraints:
 *   md=RAM   → data=RAM    (all-volatile)
 *   md!=RAM  → data!=RAM   (no partial persistence)
 */
const struct reffs_storage_ops *
reffs_backend_compose(enum reffs_md_type md, enum reffs_data_type data);
```

Implementation:
1. Validate constraints
2. `calloc` a `reffs_storage_ops`
3. Copy md function pointers from `md_templates[md]`
4. Copy data function pointers from `data_templates[data]`
5. Compose `inode_free` — generates a wrapper that calls
   `md_ops->inode_free(inode)` then `data_ops->data_inode_cleanup(inode)`
   (see "inode_free composition" below)
6. Set `.type` to the md backend's type (used as discriminant in
   recovery dispatch and wire/config serialization)
7. Set `.name` (e.g., `"rocksdb+posix"`)
8. Return

**Always heap-allocate**, even for RAM+RAM and POSIX+POSIX.  The
composer copies from static templates.  `super_block_free()` always
calls `free((void *)sb->sb_ops)`.  Consistent lifetime, no
conditional logic.

### inode_free composition

Today `posix_inode_free()` unlinks ALL files for an inode: `.meta`,
`.dat`, `.dir`, `.lnk`.  This straddles both axes — with RocksDB md
+ POSIX data, the RocksDB md backend deletes keys and the POSIX
data backend must unlink `.dat`.  Neither can do the other's job.

Solution: split `inode_free` into md cleanup + data cleanup, and
have the composer generate a wrapper:

```c
/* In reffs_storage_ops (public, unchanged): */
void (*inode_free)(struct inode *inode);

/* The composer generates: */
static void composed_inode_free(struct inode *inode)
{
    /* md cleanup (delete RocksDB keys, or unlink .meta/.dir/.lnk) */
    if (md_inode_free)
        md_inode_free(inode);
    /* data cleanup (unlink .dat) */
    if (data_inode_cleanup)
        data_inode_cleanup(inode);
}
```

The composed ops struct stores the two raw function pointers
alongside the wrapper.  Multiple approaches work (closure struct,
tagged union in sb_storage_private, or a small trampoline struct
embedded in the composed ops allocation).  Simplest: extend the
heap allocation to include the raw pointers:

```c
struct composed_ops {
    struct reffs_storage_ops ops;      /* the public vtable */
    void (*md_inode_free)(struct inode *);
    void (*data_inode_cleanup)(struct inode *);
};
```

`ops.inode_free` points to `composed_inode_free`, which casts
back to `struct composed_ops *` via `container_of` to find the
raw pointers.

For POSIX+POSIX, this degenerates to: `md_inode_free` = old
`posix_inode_free` (which now only unlinks `.meta`/`.dir`/`.lnk`),
`data_inode_cleanup` = `posix_data_inode_cleanup` (unlinks `.dat`).
Same net behavior as today.

### sb_storage_private and the data path coupling

Today POSIX data functions (`posix_db_alloc`, etc.) read
`sb->sb_storage_private` and cast to `posix_sb_private *` to get
`sb_priv->sb_dir`.  With RocksDB+POSIX composition,
`sb_storage_private` is `rocksdb_sb_private *` — the cast would
read garbage.

**Fix**: POSIX data functions compute paths directly from fields
on the public `struct super_block`:

```c
/* Before (posix_db_alloc — reads md-private struct): */
struct posix_sb_private *sb_priv = sb->sb_storage_private;
snprintf(path, ..., "%s/ino_%lu.dat", sb_priv->sb_dir, ino);

/* After (posix_data_db_alloc — reads public sb fields): */
snprintf(path, ..., "%s/sb_%lu/ino_%lu.dat",
         sb->sb_backend_path, sb->sb_id, ino);
```

`sb->sb_backend_path` and `sb->sb_id` are always set, regardless
of md backend.  The `snprintf` per call is cheap.  No need for a
second private-data pointer on the sb struct.

The POSIX md functions in `posix.c` continue to use
`posix_sb_private` (via `sb_storage_private`) for their own paths
(`.meta`, `.dir`, `.lnk`, `.layouts`).  No conflict — each axis
owns its own private data.

### Existing backends become compositions

| Current `reffs_storage_type` | md | data | Notes |
|------------------------------|-----|------|-------|
| `REFFS_STORAGE_RAM` | RAM | RAM | All in-memory, no persistence |
| `REFFS_STORAGE_POSIX` | POSIX | POSIX | Current behavior unchanged |
| `REFFS_STORAGE_ROCKSDB` | RocksDB | POSIX | New: metadata in RocksDB, data in POSIX files |

The existing `enum reffs_storage_type` stays for the wire/config
layer (probe protocol, TOML, registry).  `super_block_alloc()`
maps it to the (md, data) pair and calls the composer.

### What moves where

Extract from `posix.c`:

| Function | Stays in `posix.c` (md) | Moves to `posix_data.c` (data) |
|----------|:-----------------------:|:------------------------------:|
| `posix_sb_alloc` | x | |
| `posix_sb_free` | x | |
| `posix_inode_alloc` (load from .meta) | x | |
| `posix_inode_free` | **split** — unlink .meta/.dir/.lnk | **split** — `data_inode_cleanup` unlinks .dat |
| `posix_inode_sync` (write .meta/.lnk/.layouts) | x | |
| `posix_dir_sync` | x | |
| `posix_dir_find_entry_by_ino` | x | |
| `posix_dir_find_entry_by_name` | x | |
| `posix_db_alloc` | | x (paths from `sb->sb_backend_path`) |
| `posix_db_free` | | x |
| `posix_db_release_resources` | | x |
| `posix_db_read` | | x |
| `posix_db_write` | | x |
| `posix_db_resize` | | x |
| `posix_db_get_size` | | x |
| `posix_db_get_fd` | | x |
| (new) `posix_data_inode_cleanup` | | x (unlink .dat file) |

Similarly for `ram.c` → `ram.c` (md) + `ram_data.c` (data).
RAM `data_inode_cleanup` is a no-op (nothing on disk).

The `posix_data.c` functions compute paths from public sb fields
(`sb->sb_backend_path` + `sb->sb_id`), NOT from
`sb->sb_storage_private`.  This allows composition with any md
backend.

The md functions in `posix.c` continue to use `posix_sb_private`
(via `sb->sb_storage_private`) for metadata paths.  No coupling.

### sb_alloc integration

```c
/* In super_block_alloc(): */
enum reffs_md_type md;
enum reffs_data_type data;

switch (storage_type) {
case REFFS_STORAGE_RAM:     md = REFFS_MD_RAM;     data = REFFS_DATA_RAM;   break;
case REFFS_STORAGE_POSIX:   md = REFFS_MD_POSIX;   data = REFFS_DATA_POSIX; break;
case REFFS_STORAGE_ROCKSDB: md = REFFS_MD_ROCKSDB;  data = REFFS_DATA_POSIX; break;
default: return NULL;
}

sb->sb_ops = reffs_backend_compose(md, data);
```

The composed ops struct is heap-allocated per-sb.  `sb_release()`
frees it (today `sb_ops` points to a static const — need to handle
both cases, or always heap-allocate even for the static ones by
copying).

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
- `super_block_alloc()`: map `reffs_storage_type` → (md, data),
  call composer
- `super_block_release()`: free the composed ops struct

**Tests** (TDD — written as part of this step, before the split):

**File**: `lib/backends/tests/compose_test.c` (NEW)

| Test | Intent |
|------|--------|
| `test_compose_ram_ram` | Compose RAM+RAM, verify all function pointers non-NULL |
| `test_compose_posix_posix` | Compose POSIX+POSIX, verify all function pointers non-NULL |
| `test_compose_ram_posix_rejected` | RAM md + POSIX data → NULL (constraint violation) |
| `test_compose_posix_ram_rejected` | POSIX md + RAM data → NULL (constraint violation) |
| `test_compose_posix_inode_roundtrip` | POSIX+POSIX: sb_alloc → inode_alloc → inode_sync → inode_free, verify .meta and .dat created then cleaned up |
| `test_compose_posix_data_roundtrip` | POSIX+POSIX: db_alloc → db_write → db_read, verify content and fd valid |
| `test_compose_inode_free_cleans_both` | POSIX+POSIX: create inode with .meta + .dat, call inode_free, verify BOTH files unlinked |

Existing `make check` must also pass — RAM and POSIX compositions
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

### Step 4: RocksDB backend — sb_alloc / sb_free

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

### Step 5: RocksDB backend — inode ops

`rocksdb_inode_alloc()`:
- This is the vtable `inode_alloc` hook, called from `inode_alloc()`
  AFTER the inode is hashed.  Same pattern as `posix_inode_alloc()`
  which loads from disk inside the hook.
- `rocksdb_get()` from `inodes` CF with key `ino:<ino>`
- If found: deserialize `inode_disk` into inode fields
- Also load symlink from `symlinks` CF, layout from `layouts` CF
- If not found: new inode (return 0, caller initializes)
- Data file loading (`ino_<ino>.dat`) is handled by the composed
  data backend — `rocksdb_inode_alloc` only loads metadata from
  RocksDB.  The data backend's `db_alloc` opens the fd separately.

`rocksdb_inode_free()` (md-side only — data cleanup is composed):
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
- `rocksdb_write(db, wopts_sync, batch)` — one atomic write
- Directory sync is separate (called from `dirent_sync_to_disk`,
  not from `inode_sync`) — see Step 6

### Step 6: Data block ops — automatic via composition

**No new code needed.**  The composer (Step 1) wires
`REFFS_DATA_POSIX` functions into the RocksDB backend's ops
struct.  The POSIX data functions from `posix_data.c` are used
directly — same fd-backed pread/pwrite/ftruncate, same io_uring
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
    ├── md.rocksdb/      ← rocksdb_sb_alloc creates this
    └── ino_<N>.dat      ← posix_data uses sb_<id>/ directly
    ```
    The POSIX data functions already build paths from
    `sb->sb_backend_path` + `sb_id` — no change needed.

Option (b) is cleaner: the data backend owns the top-level sb
directory, the md backend creates a subdirectory inside it.
Both POSIX-md and RocksDB-md coexist with the same data layout.

### Step 7: RocksDB backend — directory ops

`rocksdb_dir_sync()`:
- Create iterator with prefix `dir:<parent_ino>:`, collect all
  existing keys (RocksDB has no prefix-delete)
- Build WriteBatch: delete each collected key, then put each
  current child entry
- Atomic `rocksdb_write()` — old entries removed and new entries
  written in one operation
- Each entry value: `{child_ino(8), name_len(2), name(variable)}`
- Thread safety: `dir_sync` is called from `dirent_sync_to_disk()`
  which is protected by the parent dirent's `rd_lock` — no
  concurrent modification of the same directory's keys

`rocksdb_dir_find_entry_by_ino()`:
- Create iterator with prefix `dir:<parent_ino>:`
- Scan values for matching child_ino

`rocksdb_dir_find_entry_by_name()`:
- Same prefix scan, match on name in value

### Step 8: RocksDB recovery — reffs_fs_recover() integration

**File**: `lib/fs/fs.c`

Today `reffs_fs_recover()` only handles `REFFS_STORAGE_POSIX` — it
scans `sb_<id>/` for `ino_*.meta` files, then walks `.dir` files
recursively to rebuild the in-memory inode/dirent tree.

For RocksDB, add a parallel recovery path:

`reffs_fs_recover_rocksdb(sb)`:
1. Iterate the `inodes` CF to find the max inode number → set
   `sb->sb_next_ino` (replaces the POSIX `scandir` of `.meta` files)
2. The root inode already exists in memory (created by
   `super_block_dirent_create()` before recovery).  Call
   `sb->sb_ops->inode_alloc(sb->sb_root_inode)` to load its
   fields from RocksDB — same pattern as `reffs_fs_recover()` for
   POSIX.  Do NOT hardcode inode number 1.
3. Call `recover_directory_recursive_rocksdb(sb->sb_dirent)`:
   - Iterate `dirs` CF with prefix `dir:<root_ino>:` to get
     all children of the root directory
   - For each child: `inode_alloc(sb, child_ino)` → loads from
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

### Step 9: RocksDB backend — chunk ops

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
- Replaces: the symlink-swap A/B file dance — RocksDB gives
  us atomic per-key writes natively, no double-buffering needed

#### 10e. Identity domains (stubs — identity phase 2 not started)
`rocksdb_ns_identity_domain_persist/load()`:
- Key: `idom:<index BE>` in `identity_domains` CF
- Value: domain record (name, type, flags)
- Replaces: `identity_domains` flat file
- CF created at open time; persist/load are stubs until phase 2

#### 10f. Identity mappings (stubs — identity phase 2 not started)
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
**File**: `lib/backends/tests/Makefile.am` (NEW — wire into build)

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
| `test_rocksdb_ns_incarnation_survives_reopen` | Add record, close, reopen, load — verify |
| `test_rocksdb_ns_identity_domains` | Persist + load domain table |
| `test_rocksdb_ns_identity_map` | Persist + load bidirectional mappings |
| `test_rocksdb_ns_nsm_state` | Save + load sm_state counter |
| `test_rocksdb_inode_survives_reopen` | Write inode, close DB, reopen DB, load inode, compare |
| `test_rocksdb_recovery` | Create sb with inodes+dirs, close, reopen, run `reffs_fs_recover_rocksdb()`, verify tree |
| `test_rocksdb_inode_delete_cleanup` | RocksDB+POSIX: create inode, write data, inode_free → verify RocksDB keys deleted AND .dat file unlinked |
| `test_rocksdb_open_corrupt` | Corrupt the MANIFEST file in md.rocksdb/, attempt sb_alloc → returns error, no crash |

All roundtrip tests MUST close and reopen the database between
write and read to verify WAL recovery, not just in-memory caching.

**Existing test impact**: NONE.  All new code, new backend, new tests.
Existing POSIX/RAM tests are unchanged.

### Step 15: Integration — existing tests with RocksDB

After unit tests pass, run the existing `sb_lifecycle_test`,
`sb_persistence_test`, and `sb_mount_crossing_test` with
`REFFS_STORAGE_ROCKSDB` instead of `REFFS_STORAGE_POSIX`.
This requires a test helper that parameterizes the storage type.

Minimal parameterization: duplicate `sb_persistence_test.c` as
`sb_persistence_rocksdb_test.c` with `REFFS_STORAGE_ROCKSDB` and
add `uuid_generate()` after alloc in helpers.  This exercises the
full superblock lifecycle (alloc → sync → free → reload) through
the RocksDB backend without building a full parameterized harness.

NOT_NOW_BROWN_COW: full parameterized test harness that runs ALL
existing tests against ALL backends.

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
Data writes use the same POSIX pwrite path as today — crash
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
  (`REFFS_DATA_EXTENT`) on a raw file or block device — full
  control over CLONE/CoW/punch semantics without host fs dependency

## Verification

1. `make -j$(nproc)` — zero errors, zero warnings
2. `make check` — all existing tests pass (POSIX/RAM unchanged)
3. New RocksDB unit tests pass (conditional on `HAVE_ROCKSDB`)
4. Start reffsd with `type = "rocksdb"`, create sb via probe,
   write files via NFS, restart, verify data survives
5. Chunk write/read via ec_demo with RocksDB-backed DS
6. `make -f Makefile.reffs fix-style` + `license` — clean
