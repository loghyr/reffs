/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_SB_H
#define _REFFS_SB_H

#include <stdint.h>
#include <stdbool.h>
#include <urcu.h>
#include <urcu/rculist.h>
#include <urcu/rculfhash.h>
#include <urcu/ref.h>

#include "reffs/dirent.h"

#define REFFS_UUID_SIZE (16)

struct super_block {
	struct rcu_head sb_rcu;
	struct urcu_ref sb_ref;
	struct cds_list_head sb_link; /* List of sbs */
	struct cds_lfht *sb_inodes;

	struct dirent *sb_dirent;
	uint64_t sb_id;

	uint64_t sb_next_ino;

	uint8_t sb_uuid[REFFS_UUID_SIZE];

	size_t sb_bytes_max;
	size_t sb_bytes_used;

	size_t sb_inodes_max;
	size_t sb_inodes_used;

#define SB_IS_READ_ONLY (1ULL << 0)
#define SB_IS_MOUNTED (1ULL << 1)
	uint64_t sb_state;
};

struct super_block *super_block_alloc(uint64_t id);
struct super_block *super_block_find(uint64_t id);
struct super_block *super_block_get(struct super_block *sb);
void super_block_put(struct super_block *sb);

int super_block_dirent_create(struct super_block *sb,
			      enum reffs_life_action rla);
void super_block_dirent_release(struct super_block *sb,
				enum reffs_life_action rla);

#endif /* _REFFS_SB_H */
