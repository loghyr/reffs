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

#include "reffs/inode.h"

#define REFFS_MAX_NAME (255)

struct dirent {
	struct rcu_head d_rcu;
	struct urcu_ref d_ref;
	struct cds_list_head d_siblings;
	struct cds_list_head d_children;

	char *d_name;
	struct inode *d_inode;
};

#define REFFS_STRING_CASE_SENSITIVE (true)
#define REFFS_STRING_CASE_INSENSITIVE (false)

struct dirent *dirent_alloc(struct dirent *parent, char *name);
struct dirent *dirent_find(struct dirent *parent, bool case_sensitive,
			   char *name);
struct dirent *dirent_get(struct dirent *de);
void dirent_put(struct dirent *de);

#endif /* _REFFS_DIRENT_H */
