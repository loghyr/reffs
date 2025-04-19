/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_DIRENT_H
#define _REFFS_DIRENT_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <urcu.h>
#include <urcu/rculist.h>
#include <urcu/ref.h>

#include "reffs/types.h"
#include "reffs/inode.h"
#include "reffs/cmp.h"

#define REFFS_MAX_NAME (255)

struct dirent {
	struct rcu_head d_rcu;
	struct urcu_ref d_ref;

	/*
	 * This entry is in the children of either the inode
	 * above it or is the root of the superblock.
	 */
	struct cds_list_head d_siblings;

	struct dirent *d_parent;

	pthread_mutex_t d_lock;

	char *d_name;
	struct inode *d_inode;
};

struct dirent *dirent_alloc(struct dirent *parent, char *name,
			    enum reffs_life_action rla);

void dirent_children_release(struct dirent *de, enum reffs_life_action rla);
struct dirent *dirent_find(struct dirent *parent, enum reffs_text_case rtc,
			   char *name);
struct dirent *dirent_get(struct dirent *de);
void dirent_parent_attach(struct dirent *de, struct dirent *parent,
			  enum reffs_life_action rla);
void dirent_parent_release(struct dirent *de, enum reffs_life_action rla);
void dirent_put(struct dirent *de);

#endif /* _REFFS_DIRENT_H */
