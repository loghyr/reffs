/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <xxhash.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <assert.h>
#include <time.h>
#include <urcu.h>
#include <urcu/rculist.h>
#include <urcu/ref.h>
#include <urcu/rculfhash.h>

#include "reffs/data_block.h"
#include "reffs/inode.h"
#include "reffs/super_block.h"
#include "reffs/log.h"
#include "reffs/cmp.h"

static int inode_match(struct cds_lfht_node *ht_node, const void *vkey)
{
	struct inode *inode = caa_container_of(ht_node, struct inode, i_node);
	const uint64_t *key = vkey;

	return *key == inode->i_ino;
}

static void inode_free_rcu(struct rcu_head *rcu)
{
	struct inode *inode = caa_container_of(rcu, struct inode, i_rcu);

	// Eventually we will have to abstract this layer, perhaps
	// as a property of the sb?
	if (inode->i_db)
		data_block_put(inode->i_db);

	pthread_rwlock_destroy(&inode->i_db_rwlock);
	pthread_mutex_destroy(&inode->i_attr_mutex);

	free(inode->i_symlink);
	free(inode);
}

bool inode_unhash(struct inode *inode)
{
	int ret;
	bool b;
	uint64_t state;

	// Do we want to hide the memory model with an inline function?
	state = __atomic_fetch_and(&inode->i_state, ~INODE_IS_HASHED,
				   __ATOMIC_ACQUIRE);
	b = state & INODE_IS_HASHED;
	if (b) {
		ret = cds_lfht_del(inode->i_sb->sb_inodes, &inode->i_node);
		if (ret)
			LOG("ret = %d", ret);
		assert(!ret);
		return true;
	}

	return false;
}

static void inode_release(struct urcu_ref *ref)
{
	struct inode *inode = caa_container_of(ref, struct inode, i_ref);

	inode_unhash(inode);
	if (inode->i_sb)
		uatomic_inc(&inode->i_sb->sb_inodes_used, __ATOMIC_RELAXED);
	super_block_put(inode->i_sb);

	call_rcu(&inode->i_rcu, inode_free_rcu);
}

struct inode *inode_alloc(struct super_block *sb, uint64_t ino)
{
	struct inode *inode;
	struct inode *tmp;
	struct cds_lfht_node *node;
	unsigned long hash = XXH3_64bits(&ino, sizeof(ino));

	/* If it already exists, use it */
	inode = inode_find(sb, ino);
	if (inode)
		return inode;

	inode = calloc(1, sizeof(*inode));
	if (!inode) {
		LOG("Could not alloc a inode");
		return NULL;
	}

	cds_lfht_node_init(&inode->i_node);
	urcu_ref_init(&inode->i_ref);

	pthread_rwlock_init(&inode->i_db_rwlock, NULL);
	pthread_mutex_init(&inode->i_attr_mutex, NULL);

	CDS_INIT_LIST_HEAD(&inode->i_children);

	if (sb) {
		inode->i_sb = super_block_get(sb);
		assert(inode->i_sb);

		if (inode->i_sb) {
			uatomic_inc(&inode->i_sb->sb_inodes_used,
				    __ATOMIC_RELAXED);

			/* Make sure no one else beat us to it */
			rcu_read_lock();
			node = cds_lfht_add_unique(inode->i_sb->sb_inodes, hash,
						   inode_match, &ino,
						   &inode->i_node);
			if (node != &inode->i_node) {
				tmp = caa_container_of(node, struct inode,
						       i_node);
				inode_put(inode);
				inode = tmp;
			} else {
				__atomic_fetch_or(&inode->i_state,
						  INODE_IS_HASHED,
						  __ATOMIC_ACQUIRE);
			}
			rcu_read_unlock();
		}
	}

	inode->i_ino = ino;

	return inode;
}

struct inode *inode_find(struct super_block *sb, uint64_t ino)
{
	struct inode *inode = NULL;
	struct inode *tmp;
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;
	unsigned long hash = XXH3_64bits(&ino, sizeof(ino));

	// Is this just for unit testing?
	if (!sb)
		return NULL;

	rcu_read_lock();
	cds_lfht_lookup(sb->sb_inodes, hash, inode_match, &ino, &iter);
	node = cds_lfht_iter_get_node(&iter);
	if (node) {
		tmp = caa_container_of(node, struct inode, i_node);
		inode = inode_get(tmp);
	}
	rcu_read_unlock();

	return inode;
}

struct inode *inode_get(struct inode *inode)
{
	if (!inode)
		return NULL;

	if (!urcu_ref_get_unless_zero(&inode->i_ref))
		return NULL;

	return inode;
}

void inode_put(struct inode *inode)
{
	if (!inode)
		return;

	urcu_ref_put(&inode->i_ref, inode_release);
}

void inode_update_times_now(struct inode *inode, uint64_t flags)
{
	struct timespec now;

	clock_gettime(CLOCK_REALTIME, &now);

	if (flags & REFFS_INODE_UPDATE_ATIME)
		inode->i_atime = now;

	if (flags & REFFS_INODE_UPDATE_CTIME)
		inode->i_ctime = now;

	if (flags & REFFS_INODE_UPDATE_MTIME)
		inode->i_mtime = now;
}

bool inode_name_is_child(struct inode *inode, char *name)
{
	bool exists = false;
	struct dirent *de;

	reffs_strng_compare cmp = reffs_text_case_cmp();

	rcu_read_lock();
	cds_list_for_each_entry_rcu(de, &inode->i_children, d_siblings) {
		if (!cmp(de->d_name, name)) {
			exists = true;
			break;
		}
	}
	rcu_read_unlock();

	return exists;
}

struct inode *inode_name_get_inode(struct inode *inode, char *name)
{
	struct inode *exists = NULL;
	struct dirent *de;

	reffs_strng_compare cmp = reffs_text_case_cmp();

	rcu_read_lock();
	cds_list_for_each_entry_rcu(de, &inode->i_children, d_siblings) {
		if (!cmp(de->d_name, name)) {
			exists = inode_get(de->d_inode);
			break;
		}
	}
	rcu_read_unlock();

	return exists;
}

struct inode_delayed_release {
	struct inode *idr_inode;
	time_t idr_release_time;
	struct cds_list_head idr_list;
};

static CDS_LIST_HEAD(delayed_release_list);
static pthread_mutex_t delayed_release_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_t reaper_thread;
static bool reaper_running = false;
static pthread_mutex_t reaper_lock = PTHREAD_MUTEX_INITIALIZER;

static void *reaper_thread_func(void *__attribute__((unused)) arg)
{
	while (1) {
		time_t now = time(NULL);
		struct inode_delayed_release *idr, *tmp;
		bool list_empty = true;

		pthread_mutex_lock(&delayed_release_lock);
		cds_list_for_each_entry_safe(idr, tmp, &delayed_release_list,
					     idr_list) {
			if (idr->idr_release_time <= now) {
				cds_list_del(&idr->idr_list);
				inode_put(idr->idr_inode);
				free(idr);
			} else {
				list_empty = false;
			}
		}

		if (list_empty && cds_list_empty(&delayed_release_list)) {
			reaper_running = false;
			pthread_mutex_unlock(&delayed_release_lock);
			break;
		}

		pthread_mutex_unlock(&delayed_release_lock);

		// Sleep for a short time before next check
		sleep(1);
	}

	return NULL;
}

static void ensure_reaper_thread(void)
{
	pthread_mutex_lock(&reaper_lock);
	if (!reaper_running) {
		reaper_running = true;
		pthread_create(&reaper_thread, NULL, reaper_thread_func, NULL);
		pthread_detach(reaper_thread);
	}
	pthread_mutex_unlock(&reaper_lock);
}

void inode_schedule_delayed_release(struct inode *inode, int delay_seconds)
{
	struct inode_delayed_release *idr =
		malloc(sizeof(struct inode_delayed_release));
	if (!idr) {
		inode_put(inode);
		return;
	}

	idr->idr_inode = inode;
	idr->idr_release_time = time(NULL) + delay_seconds;

	pthread_mutex_lock(&delayed_release_lock);
	cds_list_add(&idr->idr_list, &delayed_release_list);
	pthread_mutex_unlock(&delayed_release_lock);

	ensure_reaper_thread();
}
