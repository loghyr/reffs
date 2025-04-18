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

	pthread_mutex_init(&inode->i_db_lock, NULL);
	pthread_mutex_init(&inode->i_attr_lock, NULL);

	inode->i_sb = super_block_get(sb);

	CDS_INIT_LIST_HEAD(&inode->i_children);

	/* Make sure no one else beat us to it */
	rcu_read_lock();
	node = cds_lfht_add_unique(inode->i_sb->sb_inodes, hash, inode_match,
				   &ino, &inode->i_node);
	if (node != &inode->i_node) {
		tmp = caa_container_of(node, struct inode, i_node);
		inode_put(inode);
		inode = tmp;
	} else {
		__atomic_fetch_or(&inode->i_state, INODE_IS_HASHED,
				  __ATOMIC_ACQUIRE);
	}
	rcu_read_unlock();

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

enum reffs_text_case reffs_rtc = reffs_text_case_sensitive;

bool inode_name_is_child(struct inode *inode, char *name)
{
	bool exists = false;
	struct dirent *de;

	reffs_strng_compare cmp;

	// In case we refactor
	if (reffs_rtc == reffs_text_case_insensitive)
		cmp = strcasecmp;
	else
		cmp = strcmp;

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

	reffs_strng_compare cmp;

	// In case we refactor
	if (reffs_rtc == reffs_text_case_insensitive)
		cmp = strcasecmp;
	else
		cmp = strcmp;

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

