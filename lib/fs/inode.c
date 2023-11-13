/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#include "reffs/inode.h"
#include "reffs/super_block.h"
#include "reffs/log.h"
#include <urcu.h>
#include <urcu/rculist.h>
#include <urcu/ref.h>

static void inode_free_rcu(struct rcu_head *rcu)
{
	struct inode *inode = caa_container_of(rcu, struct inode, i_rcu);

	free(inode);
}

static void inode_release(struct urcu_ref *ref)
{
	struct inode *inode = caa_container_of(ref, struct inode, i_ref);

	call_rcu(&inode->i_rcu, inode_free_rcu);
}

struct inode *inode_alloc(struct super_block *sb, uint64_t ino)
{
	struct inode *inode;

	inode = calloc(1, sizeof(*inode));
	if (!inode) {
		LOG("Could not alloc a inode");
		return NULL;
	}

	inode->i_ino = ino;
	cds_list_add_rcu(&inode->i_link, &sb->sb_inodes);
	urcu_ref_init(&inode->i_ref);

	return inode;
}

struct inode *inode_find(struct super_block *sb, uint64_t ino)
{
	struct inode *inode = NULL;
	struct inode *tmp;

	rcu_read_lock();
	cds_list_for_each_entry_rcu(tmp, &sb->sb_inodes, i_link)
		if (ino == tmp->i_ino) {
			inode = inode_get(tmp);
			break;
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
