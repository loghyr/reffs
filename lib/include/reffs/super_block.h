/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_SB_H
#define _REFFS_SB_H

#include <stdint.h>
#include <stdbool.h>
#include <urcu.h>
#include <uuid/uuid.h>
#include <urcu/rculist.h>
#include <urcu/rculfhash.h>
#include <urcu/ref.h>

#include "reffs/dirent.h"
struct inode; /* forward decl for sb_root_inode */
#include "reffs/nfs4_stats.h"
#include "reffs/types.h"
#include "reffs/backend.h"

#define SUPER_BLOCK_ROOT_ID (1)

/*
 * LRU limits -- tunable, but these are reasonable defaults.
 * Eviction kicks in when the respective count exceeds the max.
 */
#define SB_INODE_LRU_MAX_DEFAULT (1024 * 64)
#define SB_DIRENT_LRU_MAX_DEFAULT (1024 * 256)

struct super_block {
	struct rcu_head sb_rcu;
	struct urcu_ref sb_ref;
	struct cds_list_head sb_link; /* List of sbs */
	struct cds_lfht *sb_inodes;

	struct reffs_dirent *sb_dirent;
	struct inode
		*sb_root_inode; /* permanent i_active pin; dropped at unmount */
	uint64_t sb_id;

	uint64_t sb_next_ino;

	char *sb_path;

	enum reffs_storage_type sb_storage_type;
	char *sb_backend_path;

	const struct reffs_storage_ops *sb_ops;
	void *sb_storage_private;

	uuid_t sb_uuid;

	size_t sb_bytes_max;
	size_t sb_bytes_used;

	size_t sb_inodes_max;
	size_t sb_inodes_used;

	uint64_t sb_delayed_count;

	size_t sb_block_size;

	/* Inode LRU -- inodes with i_active == 0 live here */
	struct cds_list_head sb_inode_lru;
	pthread_mutex_t sb_inode_lru_lock;
	size_t sb_inode_lru_count;
	size_t sb_inode_lru_max;

	/* Dirent LRU -- dirents with rd_active == 0 and no children live here */
	struct cds_list_head sb_dirent_lru;
	pthread_mutex_t sb_dirent_lru_lock;
	size_t sb_dirent_lru_count;
	size_t sb_dirent_lru_max;

#define SB_IN_LIST (1ULL << 0)
#define SB_IS_READ_ONLY (1ULL << 1)
#define SB_IS_MOUNTED (1ULL << 2)
	uint64_t sb_state;

	/* Per-op NFS4 statistics — superblock scope. */
	struct reffs_op_stats sb_nfs4_op_stats[REFFS_NFS4_OP_MAX];

	/* Backend I/O statistics for this superblock. */
	struct reffs_backend_stats sb_backend_stats;
};

struct super_block *super_block_alloc(uint64_t id, char *path,
				      enum reffs_storage_type type,
				      const char *backend_path);
struct super_block *super_block_find(uint64_t id);
struct super_block *super_block_get(struct super_block *sb);
void super_block_put(struct super_block *sb);

int super_block_dirent_create(struct super_block *sb, struct reffs_dirent *de,
			      enum reffs_life_action rla);
void super_block_dirent_release(struct super_block *sb,
				enum reffs_life_action rla);

struct cds_list_head *super_block_list_head(void);

void super_block_release_dirents(struct super_block *sb);

/* Evict up to 'count' idle inodes / dirents from the LRU. */
void super_block_evict_inodes(struct super_block *sb, size_t count);
void super_block_evict_dirents(struct super_block *sb, size_t count);
void super_block_drain(struct super_block *sb);

#endif /* _REFFS_SB_H */
