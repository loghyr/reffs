/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#define _XOPEN_SOURCE 600
//#define _POSIX_C_SOURCE 200809L
#include <features.h>

#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <urcu.h>
#include <urcu/rculist.h>
#include <urcu/ref.h>

#include "reffs/dirent.h"
#include "reffs/log.h"
#include "reffs/test.h"
#include "reffs/types.h"
#include "reffs/cmp.h"
#include "reffs/trace/fs.h"

CDS_LIST_HEAD(dirent_list);

void dirent_parent_attach(struct reffs_dirent *rd, struct reffs_dirent *parent,
			  enum reffs_life_action rla)
{
	if (!parent || !parent->rd_inode)
		return;

	rcu_read_lock();
	rd->rd_parent = dirent_get(parent);
	verify(parent->rd_inode->i_mode & S_IFDIR);
	__atomic_fetch_add(&parent->rd_inode->i_nlink, 1, __ATOMIC_RELAXED);
	rd->rd_cookie = __atomic_add_fetch(&parent->rd_cookie_next, 1,
					   __ATOMIC_RELAXED);
	cds_list_add_tail_rcu(&rd->rd_siblings, &parent->rd_inode->i_children);
	dirent_get(rd); // One for the linked list

	if (rd->rd_inode) {
		if (rd->rd_inode->i_mode & S_IFDIR)
			rd->rd_inode->i_parent =
				parent; // Do not take a reference, manage carefully
		else
			__atomic_fetch_add(&rd->rd_inode->i_nlink, 1,
					   __ATOMIC_RELAXED);
	}

	if (rla == reffs_life_action_birth || rla == reffs_life_action_update) {
		inode_update_times_now(parent->rd_inode,
				       REFFS_INODE_UPDATE_CTIME |
					       REFFS_INODE_UPDATE_MTIME);
	}
	rcu_read_unlock();
}

static void dirent_free_rcu(struct rcu_head *rcu)
{
	struct reffs_dirent *rd =
		caa_container_of(rcu, struct reffs_dirent, rd_rcu);

	trace_fs_dirent(rd, __func__, __LINE__);

	pthread_rwlock_destroy(&rd->rd_rwlock);

	free(rd->rd_name);
	free(rd);
}

static void dirent_release(struct urcu_ref *ref)
{
	struct reffs_dirent *rd =
		caa_container_of(ref, struct reffs_dirent, rd_ref);

	trace_fs_dirent(rd, __func__, __LINE__);

	if (rd->rd_inode)
		inode_put(rd->rd_inode);

	// Basically trying to prep for when we are no longer a RAM disk!
	dirent_parent_release(rd, reffs_life_action_unload);

	call_rcu(&rd->rd_rcu, dirent_free_rcu);
}

// name should be utf8
struct reffs_dirent *dirent_alloc(struct reffs_dirent *parent, char *name,
				  enum reffs_life_action rla)
{
	struct reffs_dirent *rd;

	rd = calloc(1, sizeof(*rd));
	if (!rd) {
		LOG("Could not alloc a rd");
		return NULL;
	}

	rd->rd_name = strdup(name);
	if (!rd->rd_name) {
		LOG("Could not alloc a rd->rd_name");
		free(rd);
		return NULL;
	}

	urcu_ref_init(&rd->rd_ref);
	rd->rd_cookie_next = 2;

	trace_fs_dirent(rd, __func__, __LINE__);

	pthread_rwlock_init(&rd->rd_rwlock, NULL);

	CDS_INIT_LIST_HEAD(&rd->rd_siblings);
	if (parent)
		dirent_parent_attach(rd, parent, rla);

	return rd;
}

struct reffs_dirent *dirent_find(struct reffs_dirent *parent,
				 enum reffs_text_case rtc, char *name)
{
	struct reffs_dirent *rd = NULL;
	struct reffs_dirent *tmp;
	reffs_strng_compare cmp = reffs_text_case_cmp_of(rtc);

	assert(parent);
	assert(name);

	if (!name)
		return rd;

	rcu_read_lock();
	cds_list_for_each_entry_rcu(tmp, &parent->rd_inode->i_children,
				    rd_siblings)
		if (!cmp(tmp->rd_name, name)) {
			rd = dirent_get(tmp);
			break;
		}
	rcu_read_unlock();

	return rd;
}

struct reffs_dirent *dirent_get(struct reffs_dirent *rd)
{
	if (!rd)
		return NULL;

	if (!urcu_ref_get_unless_zero(&rd->rd_ref))
		return NULL;

	trace_fs_dirent(rd, __func__, __LINE__);

	return rd;
}

void dirent_put(struct reffs_dirent *rd)
{
	if (!rd)
		return;

	trace_fs_dirent(rd, __func__, __LINE__);
	urcu_ref_put(&rd->rd_ref, dirent_release);
}

void dirent_children_release(struct reffs_dirent *parent,
			     enum reffs_life_action rla)
{
	struct reffs_dirent *rd;

	if (!parent)
		return;

	rcu_read_lock();
	while (!cds_list_empty(&parent->rd_inode->i_children)) {
		rd = cds_list_first_entry(&parent->rd_inode->i_children,
					  struct reffs_dirent, rd_siblings);
		dirent_parent_release(rd, rla);
		dirent_children_release(rd, rla);
		dirent_put(rd);
	}
	rcu_read_unlock();
}

void dirent_parent_release(struct reffs_dirent *rd, enum reffs_life_action rla)
{
	struct reffs_dirent *parent;

	if (!rd)
		return;

	rcu_read_lock();
	parent = rcu_xchg_pointer(&rd->rd_parent, NULL);
	if (parent) {
		__atomic_fetch_sub(&parent->rd_inode->i_nlink, 1,
				   __ATOMIC_RELAXED);
		cds_list_del_init(&rd->rd_siblings);

		if (rd->rd_inode && rd->rd_inode->i_mode & S_IFDIR)
			rd->rd_inode->i_parent = NULL; // Prevent use-after-free

		if (rla == reffs_life_action_death ||
		    rla == reffs_life_action_update ||
		    rla == reffs_life_action_delayed_death) {
			inode_update_times_now(
				parent->rd_inode,
				REFFS_INODE_UPDATE_CTIME |
					REFFS_INODE_UPDATE_MTIME);
		}
		dirent_put(parent);

		if (rd->rd_inode) {
			__atomic_fetch_sub(&rd->rd_inode->i_nlink, 1,
					   __ATOMIC_RELAXED);

			if (rla == reffs_life_action_delayed_death)
				inode_schedule_delayed_release(
					rd->rd_inode, INODE_RELEASE_HARVEST);
		}

		dirent_put(rd);
	}
	rcu_read_unlock();
}
