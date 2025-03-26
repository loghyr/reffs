/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include "reffs/super_block.h"
#include "reffs/inode.h"
#include "reffs/test.h"

static uint64_t sb_id_next = 0;

static int add_sb_1(void)
{
	struct super_block *sb;
	uint64_t sb_id;

	sb_id = uatomic_add_return(&sb_id_next, 1);

	sb = super_block_alloc(sb_id);
	verify(sb);

	super_block_dirent_release(sb, reffs_life_action_death);
	super_block_put(sb);

	return 0;
}

static int add_sb_2(void)
{
	struct super_block *sb;
	uint64_t sb_id;

	sb_id = uatomic_add_return(&sb_id_next, 1);

	sb = super_block_alloc(sb_id);
	verify(sb);

	super_block_put(sb);
	super_block_dirent_release(sb, reffs_life_action_death);

	return 0;
}

static int find_sb_inode(void)
{
	struct inode *inode1;
	struct inode *inode2;
	struct super_block *sb;
	int ret;
	uint64_t sb_id;

	sb_id = uatomic_add_return(&sb_id_next, 1);

	sb = super_block_alloc(sb_id);
	verify(sb);

	ret = super_block_dirent_create(sb, reffs_life_action_birth);
	verify(ret == 0);

	inode1 = inode_alloc(sb, 1);
	verify(inode1 == sb->sb_dirent->d_inode);

	inode2 = inode_find(sb, 1);
	verify(inode2 == sb->sb_dirent->d_inode);

	inode_put(inode1);
	inode_put(inode2);

	super_block_put(sb);
	super_block_dirent_release(sb, reffs_life_action_death);

	return 0;
}

static int find_sb_inode_put(void)
{
	struct inode *inode1;
	struct inode *inode2;
	struct super_block *sb;
	int ret;
	uint64_t sb_id;

	sb_id = uatomic_add_return(&sb_id_next, 1);

	sb = super_block_alloc(sb_id);
	verify(sb);

	ret = super_block_dirent_create(sb, reffs_life_action_birth);
	verify(ret == 0);

	inode1 = inode_alloc(sb, 1);
	verify(inode1 == sb->sb_dirent->d_inode);

	inode2 = inode_find(sb, 1);
	verify(inode2 == sb->sb_dirent->d_inode);

	super_block_put(sb);
	super_block_dirent_release(sb, reffs_life_action_death);

	inode_put(inode1);
	inode_put(inode2);

	return 0;
}

static int find_sb_inode_unhash(void)
{
	struct inode *inode1;
	struct inode *inode2;
	struct super_block *sb;
	int ret;
	uint64_t sb_id;

	sb_id = uatomic_add_return(&sb_id_next, 1);

	sb = super_block_alloc(sb_id);
	verify(sb);

	ret = super_block_dirent_create(sb, reffs_life_action_birth);
	verify(ret == 0);

	inode1 = inode_alloc(sb, 1);
	verify(inode1 == sb->sb_dirent->d_inode);

	inode2 = inode_find(sb, 1);
	verify(inode2 == sb->sb_dirent->d_inode);

	inode_put(inode1);
	inode_put(inode2);

	super_block_put(sb);
	super_block_dirent_release(sb, reffs_life_action_death);

	return 0;
}

static int add_inode_1(void)
{
	struct inode *inode;
	struct super_block *sb;
	uint64_t sb_id;

	sb_id = uatomic_add_return(&sb_id_next, 1);

	sb = super_block_alloc(sb_id);
	verify(sb);

	inode = inode_alloc(sb, 2);
	verify(inode);

	inode_put(inode);
	inode = inode_find(sb, 2);
	verify(!inode);

	super_block_dirent_release(sb, reffs_life_action_death);
	super_block_put(sb);

	return 0;
}

static int add_inode_2(void)
{
	struct inode *inode1, *inode2;
	struct super_block *sb;
	uint64_t sb_id;

	sb_id = uatomic_add_return(&sb_id_next, 1);

	sb = super_block_alloc(sb_id);
	verify(sb);

	inode1 = inode_alloc(sb, 2);
	verify(inode1);

	inode2 = inode_alloc(sb, 3);
	verify(inode2);

	inode_put(inode1);
	inode1 = inode_find(sb, 2);
	verify(!inode1);

	inode_put(inode2);
	inode2 = inode_find(sb, 3);
	verify(!inode2);

	super_block_dirent_release(sb, reffs_life_action_death);
	super_block_put(sb);

	return 0;
}

static int put_inode_1(void)
{
	struct inode *inode1, *inode2, *inode3;
	struct super_block *sb;
	uint64_t sb_id;

	sb_id = uatomic_add_return(&sb_id_next, 1);

	sb = super_block_alloc(sb_id);
	verify(sb);
	inode1 = inode_alloc(sb, 2);
	verify(inode1);
	inode2 = inode_alloc(sb, 3);
	verify(inode2);

	inode3 = inode_find(sb, 2);
	verify(inode3);

	inode_put(inode1);
	inode1 = inode_find(sb, 2);
	verify(inode1 == inode3);
	inode_put(inode1);

	inode_put(inode2);
	inode2 = inode_find(sb, 3);
	verify(!inode2);

	super_block_dirent_release(sb, reffs_life_action_death);
	super_block_put(sb);

	inode_put(inode3);

	return 0;
}

static int get_inode_1(void)
{
	struct inode *inode1, *inode2, *inode3;
	struct super_block *sb;
	uint64_t sb_id;

	sb_id = uatomic_add_return(&sb_id_next, 1);

	sb = super_block_alloc(sb_id);
	verify(sb);

	inode1 = inode_alloc(sb, 2);
	verify(inode1);

	inode2 = inode_alloc(sb, 3);
	verify(inode2);

	inode3 = inode_get(inode1);
	verify(inode3);

	inode_unhash(inode1);
	inode_put(inode1);
	/* Better not be found even thought inode3 has a reference. */
	inode1 = inode_find(sb, 2);
	verify(!inode1);

	inode_put(inode2);
	inode2 = inode_find(sb, 3);
	verify(!inode2);

	super_block_dirent_release(sb, reffs_life_action_death);
	super_block_put(sb);

	inode_put(inode3);

	return 0;
}

static int sb_put_inode_1(void)
{
	struct inode *inode1, *inode2;
	struct super_block *sb;
	uint64_t sb_id;

	sb_id = uatomic_add_return(&sb_id_next, 1);

	sb = super_block_alloc(sb_id);
	verify(sb);

	inode1 = inode_alloc(sb, 2);
	verify(inode1);

	inode2 = inode_alloc(sb, 3);
	verify(inode2);

	inode_put(inode2);
	inode2 = inode_find(sb, 3);
	verify(!inode2);

	super_block_dirent_release(sb, reffs_life_action_death);
	super_block_put(sb);

	inode_put(inode1);
	inode1 = inode_find(sb, 3);
	verify(!inode1);

	return 0;
}

static int find_inode_1(void)
{
	struct inode *inode1, *inode2;
	struct super_block *sb;
	uint64_t sb_id;

	sb_id = uatomic_add_return(&sb_id_next, 1);

	sb = super_block_alloc(sb_id);
	verify(sb);
	inode1 = inode_alloc(sb, 2);
	verify(inode1);

	inode2 = inode_find(sb, 2);
	verify(inode2 == inode1);

	inode_put(inode2);
	inode_put(inode1);
	inode1 = inode_find(sb, 2);
	verify(!inode1);

	super_block_dirent_release(sb, reffs_life_action_death);
	super_block_put(sb);

	return 0;
}

static int find_inode_1_sb_NULL(void)
{
	struct inode *inode1, *inode2;
	struct super_block *sb;
	uint64_t sb_id;

	sb_id = uatomic_add_return(&sb_id_next, 1);

	sb = super_block_alloc(sb_id);
	verify(sb);

	inode1 = inode_alloc(sb, 2);
	verify(inode1);

	inode2 = inode_find(NULL, 2);
	verify(!inode2);

	inode_put(inode1);
	inode1 = inode_find(sb, 2);
	verify(!inode1);

	super_block_dirent_release(sb, reffs_life_action_death);
	super_block_put(sb);

	return 0;
}

static int find_inode_3(void)
{
	struct inode *inode1, *inode2, *inode3;
	struct super_block *sb;
	uint64_t sb_id;

	sb_id = uatomic_add_return(&sb_id_next, 1);

	sb = super_block_alloc(sb_id);
	verify(sb);

	inode1 = inode_alloc(sb, 2);
	verify(inode1);

	inode2 = inode_alloc(sb, 3);
	verify(inode2);

	inode3 = inode_find(sb, 4);
	verify(!inode3);

	inode_put(inode1);
	inode1 = inode_find(sb, 2);
	verify(!inode1);

	inode_put(inode2);
	inode2 = inode_find(sb, 3);
	verify(!inode2);

	super_block_dirent_release(sb, reffs_life_action_death);
	super_block_put(sb);

	sb = super_block_find(6);
	verify(!sb);

	return 0;
}

static int find_sb_1(void)
{
	struct super_block *sb1, *sb2;
	uint64_t sb_id;

	sb_id = uatomic_add_return(&sb_id_next, 1);

	sb1 = super_block_alloc(sb_id);
	verify(sb1);

	sb2 = super_block_find(sb_id);
	verify(sb1 == sb2);

	super_block_dirent_release(sb1, reffs_life_action_death);
	super_block_put(sb1);
	super_block_put(sb2);

	return 0;
}

int main(void)
{
	int number_failed = 0;

	rcu_register_thread();

	add_sb_1();
	add_inode_1();
	add_inode_2();
	find_inode_1();
	find_inode_1_sb_NULL();
	find_inode_3();
	find_sb_1();
	get_inode_1();
	put_inode_1();
	sb_put_inode_1();
	add_sb_2();
	find_sb_inode();
	find_sb_inode_put();
	find_sb_inode_unhash();

	synchronize_rcu();
	rcu_barrier();

	rcu_unregister_thread();

	return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
