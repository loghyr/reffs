<!--
SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

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
column families for logical separation, and built-in compression --
all in a single directory per database.

## Design Principles

1. **One RocksDB database per superblock** -- the sb is the unit of
   migration.  `tar` the sb's RocksDB directory, move it to another
   machine, load it there.

2. **Separate databases for cross-sb concerns** -- server state
   (boot_seq, slot_next, uuid) and the sb registry (next_id, sb
   entries) live in a **namespace database** shared by all sbs in
   a connected namespace.

3. **Chunk store co-located with inode data** -- when the backend is
   RocksDB, chunk metadata lives in the same per-sb database as the
   inode it belongs to (column family `chunks`).

4. **Column families for logical separation** -- each object type
   gets its own column family within the per-sb database.

5. **Metadata and data are independent backend axes** -- the vtable
   struct is unchanged, function pointers are composed at runtime
   from a metadata backend and a data backend.

6. **Runtime composition, not vtable split** -- `reffs_backend_compose()`
   fills a single flat vtable from two templates.

7. **Constraint: md=RAM <--> data=RAM** -- partial persistence is
   incoherent.

8. **Host byte order** -- not portable across architectures.

9. **Data layer is a future decision point** -- POSIX files for data
   means we inherit the host filesystem's feature set.

## Sub-documents

Detailed design is split across focused documents:

- @.claude/design/rocksdb-database.md -- Per-sb and namespace database
  layout, column families, key encoding
- @.claude/design/rocksdb-persistence.md -- Complete persistence
  inventory, persistence guarantees, migration path
- @.claude/design/rocksdb-composition.md -- Backend composition
  (md/data axis split), inode_free composition, sb_alloc integration
- @.claude/design/rocksdb-implementation.md -- Implementation steps,
  chunk store integration, unit tests, verification
- @.claude/design/rocksdb-config-and-notes.md -- RocksDB C API notes,
  configuration, security/licensing, deferred items
