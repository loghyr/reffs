/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#include "reffs/super_block.h"
#include "reffs/log.h"
#include "reffs/inode.h"
#include <urcu.h>
#include <urcu/rculist.h>
#include <urcu/ref.h>
#include <urcu/rculfhash.h>
#include <errno.h>

CDS_LIST_HEAD(super_block_list);

static void super_block_remove_all_inodes(struct cds_lfht *ht)
{
	struct cds_lfht_iter iter;
	struct inode *inode;
	unsigned long count = 0;

	rcu_read_lock();
	cds_lfht_for_each_entry(ht, &iter, inode, i_node) {
		if (inode_unhash(inode))
			count++;
	}
	rcu_read_unlock();

	assert(!count);
}

static void super_block_free_rcu(struct rcu_head *rcu)
{
	int ret;

	struct super_block *sb =
		caa_container_of(rcu, struct super_block, sb_rcu);

	ret = cds_lfht_destroy(sb->sb_inodes, NULL);
	if (ret < 0) {
		ret = errno;
		LOG("Could not delete a hash table");
	}

	free(sb);
}

static void super_block_release(struct urcu_ref *ref)
{
	struct super_block *sb =
		caa_container_of(ref, struct super_block, sb_ref);

	super_block_remove_all_inodes(sb->sb_inodes);

	call_rcu(&sb->sb_rcu, super_block_free_rcu);
}

struct super_block *super_block_alloc(uint64_t id)
{
	struct super_block *sb;

	sb = calloc(1, sizeof(*sb));
	if (!sb) {
		LOG("Could not alloc a sb");
		return NULL;
	}

	CDS_INIT_LIST_HEAD(&sb->sb_link);

	sb->sb_inodes = cds_lfht_new(
		8, 8, 0, CDS_LFHT_AUTO_RESIZE | CDS_LFHT_ACCOUNTING, NULL);
	if (!sb->sb_inodes) {
		LOG("Could not create a new hash table");
		free(sb);
		return NULL;
	}

	sb->sb_id = id;
	cds_list_add_rcu(&sb->sb_link, &super_block_list);
	urcu_ref_init(&sb->sb_ref);

	return sb;
}

struct super_block *super_block_find(uint64_t id)
{
	struct super_block *sb = NULL;
	struct super_block *tmp;

	rcu_read_lock();
	cds_list_for_each_entry_rcu(tmp, &super_block_list, sb_link)
		if (id == tmp->sb_id) {
			sb = super_block_get(tmp);
			break;
		}
	rcu_read_unlock();

	return sb;
}

struct super_block *super_block_get(struct super_block *sb)
{
	if (!sb)
		return NULL;

	if (!urcu_ref_get_unless_zero(&sb->sb_ref))
		return NULL;

	return sb;
}

void super_block_put(struct super_block *sb)
{
	if (!sb)
		return;

	urcu_ref_put(&sb->sb_ref, super_block_release);
}
