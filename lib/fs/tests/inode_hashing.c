/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_JEMALLOC
#include <jemalloc/jemalloc.h>
#endif

#include <stdlib.h>
#include <stdint.h>
#include <check.h>
#include <stdbool.h>
#include <errno.h>
#include "reffs/super_block.h"
#include "reffs/inode.h"
#include "reffs/test.h"

static uint64_t sb_id_next = 0;

START_TEST(add_sb_1)
{
	struct super_block *sb;
	uint64_t sb_id;

	sb_id = __atomic_add_fetch(&sb_id_next, 1, __ATOMIC_RELAXED);

	sb = super_block_alloc(sb_id, "/", REFFS_STORAGE_RAM, NULL);
	ck_assert(sb);

	super_block_dirent_release(sb, reffs_life_action_death);
	super_block_put(sb);
}

START_TEST(add_sb_2)
{
	struct super_block *sb;
	uint64_t sb_id;

	sb_id = __atomic_add_fetch(&sb_id_next, 1, __ATOMIC_RELAXED);

	sb = super_block_alloc(sb_id, "/", REFFS_STORAGE_RAM, NULL);
	ck_assert(sb);

	super_block_put(sb);
	super_block_dirent_release(sb, reffs_life_action_death);
}

START_TEST(find_sb_inode)
{
	struct inode *inode1;
	struct inode *inode2;
	struct super_block *sb;
	int ret;
	uint64_t sb_id;

	sb_id = __atomic_add_fetch(&sb_id_next, 1, __ATOMIC_RELAXED);

	sb = super_block_alloc(sb_id, "/", REFFS_STORAGE_RAM, NULL);
	ck_assert(sb);

	ret = super_block_dirent_create(sb, NULL, reffs_life_action_birth);
	ck_assert_int_eq(ret, 0);

	inode1 = inode_alloc(sb, 1);
	ck_assert(inode1 == sb->sb_dirent->d_inode);

	inode2 = inode_find(sb, 1);
	ck_assert(inode2 == sb->sb_dirent->d_inode);

	inode_put(inode1);
	inode_put(inode2);

	super_block_put(sb);
	super_block_dirent_release(sb, reffs_life_action_death);
}

START_TEST(find_sb_inode_put)
{
	struct inode *inode1;
	struct inode *inode2;
	struct super_block *sb;
	int ret;
	uint64_t sb_id;

	sb_id = __atomic_add_fetch(&sb_id_next, 1, __ATOMIC_RELAXED);

	sb = super_block_alloc(sb_id, "/", REFFS_STORAGE_RAM, NULL);
	ck_assert(sb);

	ret = super_block_dirent_create(sb, NULL, reffs_life_action_birth);
	ck_assert_int_eq(ret, 0);

	inode1 = inode_alloc(sb, 1);
	ck_assert(inode1 == sb->sb_dirent->d_inode);

	inode2 = inode_find(sb, 1);
	ck_assert(inode2 == sb->sb_dirent->d_inode);

	super_block_put(sb);
	super_block_dirent_release(sb, reffs_life_action_death);

	inode_put(inode1);
	inode_put(inode2);
}

START_TEST(find_sb_inode_unhash)
{
	struct inode *inode1;
	struct inode *inode2;
	struct super_block *sb;
	int ret;
	uint64_t sb_id;

	sb_id = __atomic_add_fetch(&sb_id_next, 1, __ATOMIC_RELAXED);

	sb = super_block_alloc(sb_id, "/", REFFS_STORAGE_RAM, NULL);
	ck_assert(sb);

	ret = super_block_dirent_create(sb, NULL, reffs_life_action_birth);
	ck_assert_int_eq(ret, 0);

	inode1 = inode_alloc(sb, 1);
	ck_assert(inode1 == sb->sb_dirent->d_inode);

	inode2 = inode_find(sb, 1);
	ck_assert(inode2 == sb->sb_dirent->d_inode);

	inode_put(inode1);
	inode_put(inode2);

	super_block_put(sb);
	super_block_dirent_release(sb, reffs_life_action_death);
}

START_TEST(add_inode_1)
{
	struct inode *inode;
	struct super_block *sb;
	uint64_t sb_id;

	sb_id = __atomic_add_fetch(&sb_id_next, 1, __ATOMIC_RELAXED);

	sb = super_block_alloc(sb_id, "/", REFFS_STORAGE_RAM, NULL);
	ck_assert(sb);

	inode = inode_alloc(sb, 2);
	ck_assert(inode);

	inode_put(inode);
	inode = inode_find(sb, 2);
	ck_assert(!inode);

	super_block_dirent_release(sb, reffs_life_action_death);
	super_block_put(sb);
}

START_TEST(add_inode_2)
{
	struct inode *inode1, *inode2;
	struct super_block *sb;
	uint64_t sb_id;

	sb_id = __atomic_add_fetch(&sb_id_next, 1, __ATOMIC_RELAXED);

	sb = super_block_alloc(sb_id, "/", REFFS_STORAGE_RAM, NULL);
	ck_assert(sb);

	inode1 = inode_alloc(sb, 2);
	ck_assert(inode1);

	inode2 = inode_alloc(sb, 3);
	ck_assert(inode2);

	inode_put(inode1);
	inode1 = inode_find(sb, 2);
	ck_assert(!inode1);

	inode_put(inode2);
	inode2 = inode_find(sb, 3);
	ck_assert(!inode2);

	super_block_dirent_release(sb, reffs_life_action_death);
	super_block_put(sb);
}

START_TEST(put_inode_1)
{
	struct inode *inode1, *inode2, *inode3;
	struct super_block *sb;
	uint64_t sb_id;

	sb_id = __atomic_add_fetch(&sb_id_next, 1, __ATOMIC_RELAXED);

	sb = super_block_alloc(sb_id, "/", REFFS_STORAGE_RAM, NULL);
	ck_assert(sb);
	inode1 = inode_alloc(sb, 2);
	ck_assert(inode1);
	inode2 = inode_alloc(sb, 3);
	ck_assert(inode2);

	inode3 = inode_find(sb, 2);
	ck_assert(inode3);

	inode_put(inode1);
	inode1 = inode_find(sb, 2);
	ck_assert(inode1 == inode3);
	inode_put(inode1);

	inode_put(inode2);
	inode2 = inode_find(sb, 3);
	ck_assert(!inode2);

	super_block_dirent_release(sb, reffs_life_action_death);
	super_block_put(sb);

	inode_put(inode3);
}

START_TEST(get_inode_1)
{
	struct inode *inode1, *inode2, *inode3;
	struct super_block *sb;
	uint64_t sb_id;

	sb_id = __atomic_add_fetch(&sb_id_next, 1, __ATOMIC_RELAXED);

	sb = super_block_alloc(sb_id, "/", REFFS_STORAGE_RAM, NULL);
	ck_assert(sb);

	inode1 = inode_alloc(sb, 2);
	ck_assert(inode1);

	inode2 = inode_alloc(sb, 3);
	ck_assert(inode2);

	inode3 = inode_get(inode1);
	ck_assert(inode3);

	inode_unhash(inode1);
	inode_put(inode1);
	/* Better not be found even thought inode3 has a reference. */
	inode1 = inode_find(sb, 2);
	ck_assert(!inode1);

	inode_put(inode2);
	inode2 = inode_find(sb, 3);
	ck_assert(!inode2);

	super_block_dirent_release(sb, reffs_life_action_death);
	super_block_put(sb);

	inode_put(inode3);
}

START_TEST(sb_put_inode_1)
{
	struct inode *inode1, *inode2;
	struct super_block *sb;
	uint64_t sb_id;

	sb_id = __atomic_add_fetch(&sb_id_next, 1, __ATOMIC_RELAXED);

	sb = super_block_alloc(sb_id, "/", REFFS_STORAGE_RAM, NULL);
	ck_assert(sb);

	inode1 = inode_alloc(sb, 2);
	ck_assert(inode1);

	inode2 = inode_alloc(sb, 3);
	ck_assert(inode2);

	inode_put(inode2);
	inode2 = inode_find(sb, 3);
	ck_assert(!inode2);

	super_block_dirent_release(sb, reffs_life_action_death);
	super_block_put(sb);

	inode_put(inode1);
	inode1 = inode_find(sb, 3);
	ck_assert(!inode1);
}

START_TEST(find_inode_1)
{
	struct inode *inode1, *inode2;
	struct super_block *sb;
	uint64_t sb_id;

	sb_id = __atomic_add_fetch(&sb_id_next, 1, __ATOMIC_RELAXED);

	sb = super_block_alloc(sb_id, "/", REFFS_STORAGE_RAM, NULL);
	ck_assert(sb);
	inode1 = inode_alloc(sb, 2);
	ck_assert(inode1);

	inode2 = inode_find(sb, 2);
	ck_assert(inode2 == inode1);

	inode_put(inode2);
	inode_put(inode1);
	inode1 = inode_find(sb, 2);
	ck_assert(!inode1);

	super_block_dirent_release(sb, reffs_life_action_death);
	super_block_put(sb);
}

START_TEST(find_inode_1_sb_NULL)
{
	struct inode *inode1, *inode2;
	struct super_block *sb;
	uint64_t sb_id;

	sb_id = __atomic_add_fetch(&sb_id_next, 1, __ATOMIC_RELAXED);

	sb = super_block_alloc(sb_id, "/", REFFS_STORAGE_RAM, NULL);
	ck_assert(sb);

	inode1 = inode_alloc(sb, 2);
	ck_assert(inode1);

	inode2 = inode_find(NULL, 2);
	ck_assert(!inode2);

	inode_put(inode1);
	inode1 = inode_find(sb, 2);
	ck_assert(!inode1);

	super_block_dirent_release(sb, reffs_life_action_death);
	super_block_put(sb);
}

START_TEST(find_inode_3)
{
	struct inode *inode1, *inode2, *inode3;
	struct super_block *sb;
	uint64_t sb_id;

	sb_id = __atomic_add_fetch(&sb_id_next, 1, __ATOMIC_RELAXED);

	sb = super_block_alloc(sb_id, "/", REFFS_STORAGE_RAM, NULL);
	ck_assert(sb);

	inode1 = inode_alloc(sb, 2);
	ck_assert(inode1);

	inode2 = inode_alloc(sb, 3);
	ck_assert(inode2);

	inode3 = inode_find(sb, 4);
	ck_assert(!inode3);

	inode_put(inode1);
	inode1 = inode_find(sb, 2);
	ck_assert(!inode1);

	inode_put(inode2);
	inode2 = inode_find(sb, 3);
	ck_assert(!inode2);

	super_block_dirent_release(sb, reffs_life_action_death);
	super_block_put(sb);

	sb = super_block_find(6);
	ck_assert(!sb);
}

START_TEST(find_sb_1)
{
	struct super_block *sb1, *sb2;
	uint64_t sb_id;

	sb_id = __atomic_add_fetch(&sb_id_next, 1, __ATOMIC_RELAXED);

	sb1 = super_block_alloc(sb_id, "/", REFFS_STORAGE_RAM, NULL);
	ck_assert(sb1);

	sb2 = super_block_find(sb_id);
	ck_assert(sb1 == sb2);

	super_block_dirent_release(sb1, reffs_life_action_death);
	super_block_put(sb1);
	super_block_put(sb2);
}

Suite *error_suite(void)
{
	Suite *s;
	TCase *tc_core;

	s = suite_create("Inode hashing");

	/* Core test case */
	tc_core = tcase_create("Core");

	tcase_add_test(tc_core, add_sb_1);
	tcase_add_test(tc_core, add_inode_1);
	tcase_add_test(tc_core, add_inode_2);
	tcase_add_test(tc_core, find_inode_1);
	tcase_add_test(tc_core, find_inode_1_sb_NULL);
	tcase_add_test(tc_core, find_inode_3);
	tcase_add_test(tc_core, find_sb_1);
	tcase_add_test(tc_core, get_inode_1);
	tcase_add_test(tc_core, put_inode_1);
	tcase_add_test(tc_core, sb_put_inode_1);
	tcase_add_test(tc_core, add_sb_2);
	tcase_add_test(tc_core, find_sb_inode);
	tcase_add_test(tc_core, find_sb_inode_put);
	tcase_add_test(tc_core, find_sb_inode_unhash);

	suite_add_tcase(s, tc_core);

	return s;
}

int main(void)
{
	int number_failed;
	Suite *s;
	SRunner *sr;

	rcu_register_thread();

	s = error_suite();
	sr = srunner_create(s);

	srunner_run_all(sr, CK_NORMAL);
	number_failed = srunner_ntests_failed(sr);
	srunner_free(sr);

	synchronize_rcu();
	rcu_barrier();

	rcu_unregister_thread();

	return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
