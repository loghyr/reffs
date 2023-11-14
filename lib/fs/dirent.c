/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#include "reffs/dirent.h"
#include "reffs/log.h"
#include <urcu.h>
#include <urcu/rculist.h>
#include <urcu/ref.h>

CDS_LIST_HEAD(dirent_list);

static void dirent_free_rcu(struct rcu_head *rcu)
{
	struct dirent *de = caa_container_of(rcu, struct dirent, d_rcu);

	free(de->d_name);
	free(de);
}

static void dirent_release(struct urcu_ref *ref)
{
	struct dirent *de = caa_container_of(ref, struct dirent, d_ref);

	if (de->d_inode)
		inode_put(de->d_inode);

	call_rcu(&de->d_rcu, dirent_free_rcu);
}

struct dirent *dirent_alloc(struct dirent *parent, char *name)
{
	struct dirent *de;

	de = calloc(1, sizeof(*de));
	if (!de) {
		LOG("Could not alloc a de");
		return NULL;
	}

	de->d_name = strdup(name);
	if (!de->d_name) {
		LOG("Could not alloc a de->d_name");
		free(de);
		return NULL;
	}

	CDS_INIT_LIST_HEAD(&de->d_children);
	CDS_INIT_LIST_HEAD(&de->d_siblings);
	if (parent)
		cds_list_add_rcu(&de->d_siblings, &parent->d_children);
	urcu_ref_init(&de->d_ref);

	return de;
}

typedef int (*reffs_strng_compare)(const char *s1, const char *s2);

struct dirent *dirent_find(struct dirent *parent, bool case_sensitive,
			   char *name)
{
	struct dirent *de = NULL;
	struct dirent *tmp;
	reffs_strng_compare cmp;

	if (case_sensitive)
		cmp = strcasecmp;
	else
		cmp = strcmp;

	rcu_read_lock();
	cds_list_for_each_entry_rcu(tmp, &parent->d_children, d_siblings)
		if (!cmp(de->d_name, name)) {
			de = dirent_get(tmp);
			break;
		}
	rcu_read_unlock();

	return de;
}

struct dirent *dirent_get(struct dirent *de)
{
	if (!de)
		return NULL;

	if (!urcu_ref_get_unless_zero(&de->d_ref))
		return NULL;

	return de;
}

void dirent_put(struct dirent *de)
{
	if (!de)
		return;

	urcu_ref_put(&de->d_ref, dirent_release);
}
