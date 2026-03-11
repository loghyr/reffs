/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_DIRENT_H
#define _REFFS_DIRENT_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <urcu.h>
#include <urcu/rculist.h>
#include <urcu/ref.h>

#include "reffs/types.h"
#include "reffs/inode.h"
#include "reffs/cmp.h"

#define REFFS_MAX_NAME (255)
#define REFFS_MAX_PATH (4095)

struct reffs_dirent {
	struct rcu_head rd_rcu;
	struct urcu_ref rd_ref; /* memory lifetime */

	/*
	 * rd_active counts callers preventing LRU eviction of this dirent.
	 * When zero and rd_ref would otherwise drop to the list-held count,
	 * the dirent is eligible for leaf eviction.
	 */
	int64_t rd_active;

	/* LRU eviction list -- sb->sb_dirent_lru, protected by sb_dirent_lru_lock */
	struct cds_list_head rd_lru;

#define DIRENT_IS_ON_LRU (1ULL << 0)
	uint64_t rd_state;

	/*
	 * This entry is in the children of either the inode
	 * above it or is the root of the superblock.
	 */
	struct cds_list_head rd_siblings;

	struct reffs_dirent *rd_parent;

	pthread_rwlock_t rd_rwlock;

	uint64_t rd_cookie;
	uint64_t rd_cookie_next;

	char *rd_name;

	/*
	 * rd_inode is a WEAK (non-owning) pointer.  It may be NULL if the
	 * inode has been evicted from the inode LRU.  Always use
	 * dirent_ensure_inode() to get a loaded, active-ref-held inode.
	 *
	 * rd_ino is authoritative and always valid once set.
	 */
	uint64_t rd_ino;
	struct inode *rd_inode; /* weak, nullable */
};

struct reffs_dirent *dirent_alloc(struct reffs_dirent *parent, char *name,
				  enum reffs_life_action rla, bool is_dir);

void dirent_children_release(struct reffs_dirent *de,
			     enum reffs_life_action rla);
struct reffs_dirent *dirent_find(struct reffs_dirent *parent,
				 enum reffs_text_case rtc, char *name);
struct reffs_dirent *dirent_get(struct reffs_dirent *de);
void dirent_parent_attach(struct reffs_dirent *de, struct reffs_dirent *parent,
			  enum reffs_life_action rla, bool is_dir);
void dirent_parent_release(struct reffs_dirent *de, enum reffs_life_action rla);
void dirent_put(struct reffs_dirent *de);

/*
 * Active-use reference: prevents LRU eviction while held.
 */
struct reffs_dirent *dirent_active_get(struct reffs_dirent *de);
void dirent_active_put(struct reffs_dirent *de);

/*
 * Ensure rd->rd_inode is populated and return it with an active ref held.
 * Caller must call inode_active_put() when done.
 * Returns NULL on OOM or if the inode cannot be loaded from disk.
 */
struct inode *dirent_ensure_inode(struct reffs_dirent *rd);

/*
 * dirent_load_child_by_name -- look up a named child in a directory whose
 * children may not all be resident in memory.
 *
 * First tries dirent_find() (in-memory fast path).  On a miss, calls the
 * storage backend's dir_find_entry_by_name op to read the on-disk directory,
 * then allocates and attaches a single dirent with reffs_life_action_load.
 *
 * Returns a ref-held dirent on success (caller must dirent_put()), or NULL
 * if the name does not exist in the directory or on I/O error.
 *
 * This is the correct function to call from LOOKUP instead of dirent_find().
 */
struct reffs_dirent *dirent_load_child_by_name(struct reffs_dirent *parent_de,
					       const char *name);

void dirent_sync_to_disk(struct reffs_dirent *parent);

#endif /* _REFFS_DIRENT_H */
