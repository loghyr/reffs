/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_TYPES_H
#define _REFFS_TYPES_H

#include <stdint.h>
#include <stdbool.h>

/*
 * reffs_life_action -- describes the semantic context of a dirent or inode
 * lifecycle operation.  Passed to dirent_parent_attach(),
 * dirent_parent_release(), dirent_children_release(), and related helpers to
 * control which side-effects (nlink accounting, cookie assignment, disk sync,
 * inode scheduling) are performed.
 *
 * birth
 *   A new name is being created in the namespace (CREATE, MKDIR, SYMLINK,
 *   MKNOD, LINK).  nlink is incremented, a cookie is assigned, and both
 *   the inode and parent directory are synced to disk.
 *
 * load
 *   A dirent is being faulted back into memory from disk after LRU eviction
 *   (LOOKUP miss, PUTFH fault-in, READDIR population).  The on-disk state is
 *   already authoritative: nlink is NOT touched, cookies are NOT reassigned,
 *   and nothing is written to disk.
 *
 * unload
 *   A dirent is being evicted from memory to make room in the LRU.  The
 *   in-memory structure is torn down but the on-disk state is left intact.
 *   Like load, nlink is NOT touched and nothing is written to disk.
 *
 * death
 *   A name is being permanently removed (REMOVE, RMDIR, the final CLOSE of
 *   an unlinked file).  nlink is decremented on both the entry and its parent
 *   directory; sb_inodes_used and sb_bytes_used are updated; the inode and
 *   parent directory are synced to disk.
 *
 * delayed_death
 *   Like death but the inode is not immediately released; instead
 *   inode_schedule_delayed_release() is called so that it lingers briefly for
 *   any in-flight operations to drain (NFS "silly-rename" / open-unlinked
 *   semantics).  All accounting side-effects are identical to death.
 *
 * update
 *   An in-place metadata change (RENAME within the same directory, chmod,
 *   etc.) that does not alter the parent/child relationship.  Currently a
 *   placeholder; specific update semantics are handled at the VFS layer.
 *
 * move
 *   A name is being relocated to a different parent directory (RENAME across
 *   directories).  The child's own nlink is NOT changed; the source parent
 *   loses a subdirectory link and the destination parent gains one (handled
 *   by the VFS rename path outside of dirent_parent_release).  No disk sync
 *   is performed by the release/attach helpers; the VFS caller owns that.
 *
 * shutdown
 *   The filesystem is being torn down (reffs_ns_fini / super_block_drain).
 *   All on-disk state is already consistent; no accounting, no nlink changes,
 *   no disk writes are needed.  The only goal is to detach the in-memory
 *   dirent tree and release memory safely.  Weak rd_inode pointers are NOT
 *   dereferenced; inode lifecycle is handled separately by super_block_drain.
 */
enum reffs_life_action {
	reffs_life_action_birth = 0,
	reffs_life_action_load = 1,
	reffs_life_action_unload = 2,
	reffs_life_action_death = 3,
	reffs_life_action_delayed_death = 4,
	reffs_life_action_update = 5,
	reffs_life_action_move = 6,
	reffs_life_action_shutdown = 7,
};

enum reffs_storage_type {
	REFFS_STORAGE_RAM = 0,
	REFFS_STORAGE_POSIX = 1,
	REFFS_STORAGE_ROCKSDB = 2,
	REFFS_STORAGE_FUSE = 3
};

#endif /* _REFFS_TYPES_H */
