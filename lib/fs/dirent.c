/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#include <unistd.h>
#include <sys/stat.h>
#include <urcu.h>
#include <urcu/rculist.h>
#include <urcu/ref.h>

#include "reffs/dirent.h"
#include "reffs/log.h"
#include "reffs/test.h"
#include "reffs/types.h"

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

	// Basically trying to prep for when we are no longer a RAM disk!
	dirent_parent_release(de, reffs_life_action_unload);

	call_rcu(&de->d_rcu, dirent_free_rcu);
}

// name should be utf8
struct dirent *dirent_alloc(struct dirent *parent, char *name,
			    enum reffs_life_action rla)
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

	urcu_ref_init(&de->d_ref);

	pthread_mutex_init(&de->d_lock, NULL);

	CDS_INIT_LIST_HEAD(&de->d_children);
	CDS_INIT_LIST_HEAD(&de->d_siblings);
	if (parent) {
		de->d_parent = dirent_get(parent);
		verify(parent->d_inode->i_mode & S_IFDIR);
		uatomic_inc(&parent->d_inode->i_nlink);
		cds_list_add_rcu(&de->d_siblings, &parent->d_children);
		dirent_get(de); // One for the linked list

		if (rla == reffs_life_action_birth) {
			pthread_mutex_lock(&parent->d_inode->i_attr_lock);
			inode_update_times_now(
				parent->d_inode,
				REFFS_INODE_UPDATE_CTIME |
					REFFS_INODE_UPDATE_MTIME);
			pthread_mutex_unlock(&parent->d_inode->i_attr_lock);
		}
	}

	return de;
}

struct dirent *dirent_find(struct dirent *parent, enum reffs_text_case rtc,
			   char *name)
{
	struct dirent *de = NULL;
	struct dirent *tmp;
	reffs_strng_compare cmp;

	if (rtc == reffs_text_case_insensitive)
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

void dirent_children_release(struct dirent *parent, enum reffs_life_action rla)
{
	struct dirent *de;

	if (!parent)
		return;

	rcu_read_lock();
	while (!cds_list_empty(&parent->d_children)) {
		de = cds_list_first_entry(&parent->d_children, struct dirent,
					  d_siblings);
		dirent_parent_release(de, rla);
		dirent_children_release(de, rla);
		dirent_put(de);
	}
	rcu_read_unlock();
}

void dirent_parent_release(struct dirent *de, enum reffs_life_action rla)
{
	struct dirent *parent;

	rcu_read_lock();
	parent = rcu_xchg_pointer(&de->d_parent, NULL);
	if (parent) {
		uatomic_dec(&parent->d_inode->i_nlink);
		cds_list_del_init(&de->d_siblings);
		dirent_put(parent);
		dirent_put(de);

		if (rla == reffs_life_action_death)
			inode_update_times_now(
				parent->d_inode,
				REFFS_INODE_UPDATE_CTIME |
					REFFS_INODE_UPDATE_MTIME);
	}
	rcu_read_unlock();
}
