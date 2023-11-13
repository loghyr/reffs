/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#include <stdlib.h>
#include <check.h>
#include <stdbool.h>
#include <errno.h>
#include "reffs/super_block.h"
#include "reffs/inode.h"
#include "reffs/test.h"

/*
 * BIG WARNING HERE: The inode_unhash() puts the inode. So it
 * implicitly nukes the active reference.
 */

START_TEST(add_sb_1)
{
	struct super_block *sb;
	sb = super_block_alloc(1);
	ck_assert(sb);

	super_block_put(sb);
}

START_TEST(add_inode_1)
{
	struct inode *inode;
	struct super_block *sb;

	sb = super_block_alloc(2);
	ck_assert(sb);

	inode = inode_alloc(sb, 1);
	ck_assert(inode);

	inode_unhash(inode);
	inode = inode_find(sb, 1);
	ck_assert(!inode);

	super_block_put(sb);
}

START_TEST(add_inode_2)
{
	struct inode *inode1, *inode2;
	struct super_block *sb;

	sb = super_block_alloc(3);
	ck_assert(sb);

	inode1 = inode_alloc(sb, 1);
	ck_assert(inode1);

	inode2 = inode_alloc(sb, 2);
	ck_assert(inode2);

	inode_unhash(inode1);
	inode1 = inode_find(sb, 1);
	ck_assert(!inode1);

	inode_unhash(inode2);
	inode2 = inode_find(sb, 2);
	ck_assert(!inode2);

	super_block_put(sb);
}

START_TEST(put_inode_1)
{
	struct inode *inode1, *inode2, *inode3;
	struct super_block *sb;

	sb = super_block_alloc(3);
	ck_assert(sb);
	inode1 = inode_alloc(sb, 1);
	ck_assert(inode1);
	inode2 = inode_alloc(sb, 2);
	ck_assert(inode2);

	inode3 = inode_find(sb, 1);
	ck_assert(inode3);

	inode_unhash(inode1);
	inode1 = inode_find(sb, 1);
	ck_assert(!inode1);

	inode_unhash(inode2);
	inode2 = inode_find(sb, 2);
	ck_assert(!inode2);

	super_block_put(sb);

	inode_put(inode3);
}

START_TEST(get_inode_1)
{
	struct inode *inode1, *inode2, *inode3;
	struct super_block *sb;

	sb = super_block_alloc(3);
	ck_assert(sb);

	inode1 = inode_alloc(sb, 1);
	ck_assert(inode1);

	inode2 = inode_alloc(sb, 2);
	ck_assert(inode2);

	inode3 = inode_get(inode1);
	ck_assert(inode3);

	inode_unhash(inode1);
	inode1 = inode_find(sb, 1);
	ck_assert(!inode1);

	inode_unhash(inode2);
	inode2 = inode_find(sb, 2);
	ck_assert(!inode2);

	super_block_put(sb);

	inode_put(inode3);
}

START_TEST(sb_put_inode_1)
{
	struct inode *inode1, *inode2;
	struct super_block *sb;

	sb = super_block_alloc(3);
	ck_assert(sb);

	inode1 = inode_alloc(sb, 1);
	ck_assert(inode1);

	inode2 = inode_alloc(sb, 2);
	ck_assert(inode2);

	inode_unhash(inode2);
	inode2 = inode_find(sb, 2);
	ck_assert(!inode2);

	super_block_put(sb);
}

START_TEST(find_inode_1)
{
	struct inode *inode1, *inode2;
	struct super_block *sb;

	sb = super_block_alloc(4);
	ck_assert(sb);
	inode1 = inode_alloc(sb, 1);
	ck_assert(inode1);

	inode2 = inode_find(sb, 1);
	ck_assert(inode2 == inode1);

	inode_unhash(inode1);
	inode1 = inode_find(sb, 1);
	ck_assert(!inode1);

	super_block_put(sb);
}

START_TEST(find_inode_1_sb_NULL)
{
	struct inode *inode1, *inode2;
	struct super_block *sb;

	sb = super_block_alloc(5);
	ck_assert(sb);

	inode1 = inode_alloc(sb, 1);
	ck_assert(inode1);

	inode2 = inode_find(NULL, 1);
	ck_assert(!inode2);

	inode_unhash(inode1);
	inode1 = inode_find(sb, 1);
	ck_assert(!inode1);

	super_block_put(sb);
}

START_TEST(find_inode_3)
{
	struct inode *inode1, *inode2, *inode3;
	struct super_block *sb;

	sb = super_block_alloc(6);
	ck_assert(sb);

	inode1 = inode_alloc(sb, 1);
	ck_assert(inode1);

	inode2 = inode_alloc(sb, 2);
	ck_assert(inode2);

	inode3 = inode_find(sb, 3);
	ck_assert(!inode3);

	inode_unhash(inode1);
	inode1 = inode_find(sb, 1);
	ck_assert(!inode1);

	inode_unhash(inode2);
	inode2 = inode_find(sb, 2);
	ck_assert(!inode2);

	super_block_put(sb);
	sb = super_block_find(1);
	ck_assert(!sb);
}

START_TEST(find_sb_1)
{
	struct super_block *sb1, *sb2;
	sb1 = super_block_alloc(7);
	ck_assert(sb1);

	sb2 = super_block_find(7);
	ck_assert(sb1 == sb2);

	super_block_put(sb1);
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
