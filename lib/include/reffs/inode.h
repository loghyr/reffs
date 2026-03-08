/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_INODE_H
#define _REFFS_INODE_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <pthread.h>
#include <urcu.h>
#include <urcu/rculist.h>
#include <urcu/rculfhash.h>
#include <urcu/ref.h>
#include "reffs/backend.h"

struct super_block;
struct data_block;
struct reffs_dirent;

struct reffs_file_handle {
	uint64_t rfh_ino;
	uint64_t rfh_sb;
};

#define INODE_ROOT_ID (1)

struct inode {
	struct rcu_head i_rcu;
	struct urcu_ref i_ref; /* memory lifetime */
	struct cds_lfht_node i_node;

	/*
	 * i_active counts callers that are actively using this inode
	 * (NFS operations in flight, open state, etc).  When it reaches
	 * zero the inode is eligible for LRU eviction.  It is separate
	 * from i_ref so that the eviction path can drop i_ref without
	 * racing with a live caller.
	 */
	int64_t i_active;

	/* LRU eviction list -- sb->sb_inode_lru, protected by sb_inode_lru_lock */
	struct cds_list_head i_lru;

#define INODE_IS_HASHED (1ULL << 0)
#define INODE_IS_ON_LRU (1ULL << 1)
	uint64_t i_state;

	uint64_t i_ino;
	uint64_t i_parent_ino; /* authoritative even when i_parent is NULL */

	struct reffs_file_handle i_on;
	struct super_block *i_sb;
	struct data_block *i_db;

	void *i_storage_private;

	/* Do this as a linked list for now */
	struct cds_list_head i_children;

	/*
	 * Weak back-pointer to the dirent that names this directory.
	 * May be NULL if the dirent has been evicted.  Use
	 * inode_ensure_parent_dirent() to get a loaded, ref-held dirent.
	 * Only valid for directories.
	 */
	struct reffs_dirent *i_parent;

	pthread_rwlock_t i_db_rwlock;
	pthread_mutex_t i_attr_mutex;

	/* locking */
	pthread_mutex_t i_lock_mutex;
	struct cds_list_head i_locks; /* List of struct reffs_lock */
	struct cds_list_head i_shares; /* List of struct reffs_share */

	/* attributes */
	pthread_mutex_t i_attrs_lock;
	uint32_t i_uid;
	uint32_t i_gid;
	uint32_t i_nlink;
	uint16_t i_mode;
	uint16_t i_unused;
	int64_t i_size;
	int64_t i_used;
	struct timespec i_atime;
	struct timespec i_btime;
	struct timespec i_ctime;
	struct timespec i_mtime;

	uint32_t i_dev_major;
	uint32_t i_dev_minor;

#define INODE_IS_OFFLINE (1ULL << 0)
#define INODE_IS_HIDDEN (1ULL << 1)
	uint64_t i_attr_flags;

	char *i_symlink;
};

struct inode_disk {
	uint32_t id_uid;
	uint32_t id_gid;
	uint32_t id_nlink;
	uint16_t id_mode;
	int64_t id_size;
	struct timespec id_atime;
	struct timespec id_ctime;
	struct timespec id_mtime;
	uint64_t id_attr_flags;
	uint64_t id_parent_ino; /* 0 = root/unknown */
};

/* Alloc (new) or find-and-load (existing) an inode.  Returns with
 * i_active already incremented -- caller must call inode_active_put(). */
struct inode *inode_alloc(struct super_block *sb, uint64_t ino);
struct inode *inode_find(struct super_block *sb, uint64_t ino);

/* Raw memory-lifetime ref; rarely needed directly. */
struct inode *inode_get(struct inode *inode);
void inode_put(struct inode *inode);

/*
 * Active-use reference: prevents LRU eviction while held.
 * inode_active_get() returns NULL if the inode is already being torn down.
 */
struct inode *inode_active_get(struct inode *inode);
void inode_active_put(struct inode *inode);

bool inode_unhash(struct inode *inode);

#define REFFS_INODE_UPDATE_ATIME (1ULL << 0)
#define REFFS_INODE_UPDATE_CTIME (1ULL << 1)
#define REFFS_INODE_UPDATE_MTIME (1ULL << 2)
void inode_update_times_now(struct inode *inode, uint64_t flags);

bool inode_name_is_child(struct inode *inode, char *name);
struct inode *inode_name_get_inode(struct inode *inode, char *name);

/*
 * Schedule this inode for eviction from the LRU after delay_seconds,
 * provided i_active has reached zero by then.
 */
#define INODE_RELEASE_HARVEST (2)
void inode_schedule_delayed_release(struct inode *inode, int delay_seconds);

void inode_sync_to_disk(struct inode *inode);

/*
 * Ensure inode->i_parent is loaded and return a ref-held dirent.
 * The dirent ref must be released with dirent_put().
 * Returns NULL only on OOM / corrupt fs.
 */
struct reffs_dirent *inode_ensure_parent_dirent(struct inode *inode);

/*
 * inode_reconstruct_path_to_root -- rebuild the dirent branch from this
 * inode up to either an already-resident ancestor or the superblock root.
 *
 * After a PUTFH the inode is loaded but its dirent may have been evicted.
 * This function walks upward via i_parent_ino, reads each parent's .dir
 * file to recover the child's name and cookie, then replays the chain
 * downward attaching dirents with reffs_life_action_load.
 *
 * Returns 0 on success, ENOENT if any link in the chain cannot be found
 * on disk (stale handle), or another errno on I/O failure.
 *
 * Callers (e.g. the NFSv4 PUTFH / LOOKUPP dispatcher) should treat a
 * non-zero return as NFS4ERR_STALE / NFS3ERR_STALE.
 */
int inode_reconstruct_path_to_root(struct inode *inode);

#endif /* _REFFS_INODE_H */
