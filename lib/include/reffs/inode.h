/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_INODE_H
#define _REFFS_INODE_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <urcu.h>
#include <urcu/rculist.h>
#include <urcu/rculfhash.h>
#include <urcu/ref.h>

struct super_block;
struct data_block;
struct reffs_dirent;

struct reffs_file_handle {
	uint64_t rfh_ino;
	uint64_t rfh_sb;
};

struct inode {
	struct rcu_head i_rcu;
	struct urcu_ref i_ref;
	struct cds_lfht_node i_node;

#define INODE_IS_HASHED (1ULL << 0)
	uint64_t i_state;

	uint64_t i_ino;

	struct reffs_file_handle i_on;
	struct super_block *i_sb;
	struct data_block *i_db;

	/* Do this as a linked list for now */
	struct cds_list_head i_children;

	/* Only valid for directories */
	struct reffs_dirent *i_parent;

	pthread_rwlock_t i_db_rwlock;
	pthread_mutex_t i_attr_mutex;

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
};

struct inode *inode_alloc(struct super_block *sb, uint64_t ino);
struct inode *inode_find(struct super_block *sb, uint64_t ino);
struct inode *inode_get(struct inode *inode);
void inode_put(struct inode *inode);
bool inode_unhash(struct inode *inode);

#define REFFS_INODE_UPDATE_ATIME (1ULL << 0)
#define REFFS_INODE_UPDATE_CTIME (1ULL << 1)
#define REFFS_INODE_UPDATE_MTIME (1ULL << 2)
void inode_update_times_now(struct inode *inode, uint64_t flags);

bool inode_name_is_child(struct inode *inode, char *name);
struct inode *inode_name_get_inode(struct inode *inode, char *name);

#define INODE_RELEASE_HARVEST (2)
void inode_schedule_delayed_release(struct inode *inode, int delay_seconds);

void inode_sync_to_disk(struct inode *inode);

#endif /* _REFFS_INODE_H */
