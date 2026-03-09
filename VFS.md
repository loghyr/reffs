<!--
SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: GPL-2.0+
-->

# Reffs VFS (Virtual File System)
 Layer

## Rationale
The VFS layer was introduced to establish a protocol-agnostic, inode-based internal API. The primary goals are:
1.  **Centralize POSIX Enforcement:** Ensure consistent behavior (permissions, link counts, timestamps) across all protocol frontends (NFSv3, NFSv4.2, FUSE).
2.  **Simplify Protocol Handlers:** Thin out the protocol-specific code by moving complex filesystem logic into a shared layer.
3.  **Inode-Based Architecture:** Move away from path-based operations to avoid redundant lookups and simplify internal synchronization.

## Architecture
The VFS layer (`lib/fs/vfs.c`) provides a set of core operations (`vfs_create`, `vfs_mkdir`, `vfs_rename`, etc.) that operate on `struct inode` and `struct authunix_parms`.

### Locking Invariants
To prevent deadlocks, the VFS enforces a strict locking order:
1.  **Attribute Mutexes:** Lock `i_attr_mutex` for involved inodes in ascending order of Inode ID.
2.  **Directory RWLocks:** Lock `rd_rwlock` for involved parent dirents in ascending order of Inode ID.

## Findings & Gotchas

### 1. The Shared Dirent Deadlock
**Finding:** Multiple inodes can share the same parent `reffs_dirent` (common for top-level entries or during moves).
**Gotcha:** Attempting to lock the same `rd_rwlock` twice in a single thread results in an immediate self-deadlock.
**Resolution:** `vfs_lock_dirs` must explicitly check if `de1 == de2` even if the inodes `d1` and `d2` are different.

### 2. Identity Resolution (`i_parent`)
**Finding:** For accurate path resolution and directory stability, a directory inode's `i_parent` must point to its own `reffs_dirent`.
**Gotcha:** Early implementations incorrectly pointed it to the parent directory's dirent, causing child lookups to fail after a directory was moved.

### 3. Link Accounting Nuances
**Finding:** Directory `nlink` management varies by operation.
**Gotcha:** 
- `rmdir` and `move` (source) must decrement the parent's `nlink`.
- `mkdir` and `move` (destination) must increment the parent's `nlink`.
- `dirent_parent_release` semantics must be carefully matched to the life action (`birth`, `death`, `move`, `load`) to ensure POSIX invariants (e.g., directory floor of 2 links) are maintained.

### 4. RCU Consistency
**Finding:** Sibling iterations must be stable during concurrent modifications.
**Gotcha:** Using standard list deletions can corrupt concurrent lookups.
**Resolution:** Always use `cds_list_del_rcu` for dirent sibling removal.

## Status (March 2026)
- [x] Core creation and removal operations migrated to VFS.
- [x] NFSv3 handlers for `REMOVE`, `RMDIR`, `RENAME`, and `LINK` refactored.
- [ ] NFSv3 handlers for `CREATE`, `MKDIR`, `SYMLINK`, and `MKNOD` pending VFS conversion.
- [x] Locking deadlocks resolved (via `de1 == de2` check).
- [ ] Remaining operations (READ, WRITE, SETATTR) pending VFS conversion.
