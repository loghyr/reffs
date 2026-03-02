/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
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

struct reffs_dirent {
	struct rcu_head rd_rcu;
	struct urcu_ref rd_ref;

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
	struct inode *rd_inode;
};

struct reffs_dirent *dirent_alloc(struct reffs_dirent *parent, char *name,
				  enum reffs_life_action rla);

void dirent_children_release(struct reffs_dirent *de,
			     enum reffs_life_action rla);
struct reffs_dirent *dirent_find(struct reffs_dirent *parent,
				 enum reffs_text_case rtc, char *name);
struct reffs_dirent *dirent_get(struct reffs_dirent *de);
void dirent_parent_attach(struct reffs_dirent *de, struct reffs_dirent *parent,
			  enum reffs_life_action rla);
void dirent_parent_release(struct reffs_dirent *de, enum reffs_life_action rla);
void dirent_put(struct reffs_dirent *de);

void dirent_sync_to_disk(struct reffs_dirent *parent);

#endif /* _REFFS_DIRENT_H */
