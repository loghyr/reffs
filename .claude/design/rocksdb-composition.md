<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

## Backend Composition

### Two axes, one vtable

```c
/* Metadata backends -- own inode/dir/symlink/layout persistence */
enum reffs_md_type {
    REFFS_MD_RAM     = 0,
    REFFS_MD_POSIX   = 1,
    REFFS_MD_ROCKSDB = 2,
};

/* Data backends -- own bulk file I/O (db_* functions) */
enum reffs_data_type {
    REFFS_DATA_RAM   = 0,
    REFFS_DATA_POSIX = 1,
    /* future: REFFS_DATA_XFS, REFFS_DATA_EXTENT */
};
```

Each axis provides a **template** -- a partial `reffs_storage_ops`
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
 *   md=RAM   --> data=RAM    (all-volatile)
 *   md!=RAM  --> data!=RAM   (no partial persistence)
 */
const struct reffs_storage_ops *
reffs_backend_compose(enum reffs_md_type md, enum reffs_data_type data);
```

Implementation:
1. Validate constraints
2. `calloc` a `reffs_storage_ops`
3. Copy md function pointers from `md_templates[md]`
4. Copy data function pointers from `data_templates[data]`
5. Compose `inode_free` -- generates a wrapper that calls
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
`.dat`, `.dir`, `.lnk`.  This straddles both axes -- with RocksDB md
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
`sb_storage_private` is `rocksdb_sb_private *` -- the cast would
read garbage.

**Fix**: POSIX data functions compute paths directly from fields
on the public `struct super_block`:

```c
/* Before (posix_db_alloc -- reads md-private struct): */
struct posix_sb_private *sb_priv = sb->sb_storage_private;
snprintf(path, ..., "%s/ino_%lu.dat", sb_priv->sb_dir, ino);

/* After (posix_data_db_alloc -- reads public sb fields): */
snprintf(path, ..., "%s/sb_%lu/ino_%lu.dat",
         sb->sb_backend_path, sb->sb_id, ino);
```

`sb->sb_backend_path` and `sb->sb_id` are always set, regardless
of md backend.  The `snprintf` per call is cheap.  No need for a
second private-data pointer on the sb struct.

The POSIX md functions in `posix.c` continue to use
`posix_sb_private` (via `sb_storage_private`) for their own paths
(`.meta`, `.dir`, `.lnk`, `.layouts`).  No conflict -- each axis
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
| `posix_inode_free` | **split** -- unlink .meta/.dir/.lnk | **split** -- `data_inode_cleanup` unlinks .dat |
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

Similarly for `ram.c` --> `ram.c` (md) + `ram_data.c` (data).
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
frees it (today `sb_ops` points to a static const -- need to handle
both cases, or always heap-allocate even for the static ones by
copying).

