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

	/* attributes */
	pthread_mutex_t i_attrs_lock;
	uint32_t i_uid;
	uint32_t i_gid;
	uint32_t i_nlink;
	uint16_t i_mode;
	uint16_t i_unused;
	uint64_t i_size;
	uint64_t i_used;
	struct timespec i_atime;
	struct timespec i_btime;
	struct timespec i_ctime;
	struct timespec i_mtime;
};

struct inode *inode_alloc(struct super_block *sb, uint64_t ino);
struct inode *inode_find(struct super_block *sb, uint64_t ino);
struct inode *inode_get(struct inode *inode);
void inode_put(struct inode *inode);
bool inode_unhash(struct inode *inode);

#endif /* _REFFS_INODE_H */
