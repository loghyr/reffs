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
#include "fs_test_harness.h"

static uint64_t sb_id_next = 0;
uid_t fs_test_uid;
gid_t fs_test_gid;

START_TEST(add_sb_1)
{
	struct super_block *sb;
	uint64_t sb_id;

	sb_id = __atomic_add_fetch(&sb_id_next, 1, __ATOMIC_RELAXED);

	sb = super_block_alloc(sb_id, "/", REFFS_STORAGE_RAM, NULL);
	ck_assert(sb);

	super_block_release_dirents(sb);
	super_block_put(sb);
}

START_TEST(add_sb_2)
{
	struct super_block *sb;
	uint64_t sb_id;

	sb_id = __atomic_add_fetch(&sb_id_next, 1, __ATOMIC_RELAXED);

	sb = super_block_alloc(sb_id, "/", REFFS_STORAGE_RAM, NULL);
	ck_assert(sb);

	super_block_release_dirents(sb);
	super_block_put(sb);
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

	/*
	 * inode_alloc returns the existing root inode (already in hash from
	 * super_block_dirent_create) with i_active=1, i_ref bumped.
	 * inode_find also returns an active ref.
	 * Both must be released with inode_active_put.
	 */
	inode1 = inode_alloc(sb, INODE_ROOT_ID);
	ck_assert(inode1 == sb->sb_dirent->rd_inode);

	inode2 = inode_find(sb, INODE_ROOT_ID);
	ck_assert(inode2 == sb->sb_dirent->rd_inode);

	inode_active_put(inode1);
	inode_active_put(inode2);

	super_block_release_dirents(sb);
	super_block_put(sb);
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
	ck_assert(inode1 == sb->sb_dirent->rd_inode);

	inode2 = inode_find(sb, 1);
	ck_assert(inode2 == sb->sb_dirent->rd_inode);

	/*
	 * Release active refs before teardown so that super_block_release_dirents
	 * can drain the hash cleanly without inode_release calling super_block_put
	 * after we've already put the sb.
	 */
	inode_active_put(inode1);
	inode_active_put(inode2);

	super_block_release_dirents(sb);
	super_block_put(sb);
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
	ck_assert(inode1 == sb->sb_dirent->rd_inode);

	inode2 = inode_find(sb, 1);
	ck_assert(inode2 == sb->sb_dirent->rd_inode);

	inode_active_put(inode1);
	inode_active_put(inode2);

	super_block_release_dirents(sb);
	super_block_put(sb);
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
	inode_active_put(inode);

	/*
	 * Inode stays in hash until drained — drain first, then verify gone.
	 */
	super_block_release_dirents(sb);
	inode = inode_find(sb, 2);
	ck_assert(!inode);

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

	inode_active_put(inode1);
	inode_active_put(inode2);

	super_block_release_dirents(sb);
	inode1 = inode_find(sb, 2);
	ck_assert(!inode1);
	inode2 = inode_find(sb, 3);
	ck_assert(!inode2);

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

	/*
	 * Drop inode1's active ref (from alloc). Inode still in hash (inode3
	 * holds an active ref). Re-find and verify it's still the same object.
	 */
	inode_active_put(inode1);
	inode1 = inode_find(sb, 2);
	ck_assert(inode1 == inode3);
	inode_active_put(inode1);

	inode_active_put(inode2);

	/*
	 * inode3 must be released before super_block_release_dirents so that
	 * drain can drop the hash ref cleanly without inode_release calling
	 * super_block_put on an already-released sb.
	 */
	inode_active_put(inode3);

	super_block_release_dirents(sb);
	super_block_put(sb);
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

	/* inode_get bumps only i_ref, not i_active — use inode_put to release */
	inode3 = inode_get(inode1);
	ck_assert(inode3);

	inode_unhash(inode1);
	inode_active_put(inode1);
	/* Better not be found even though inode3 has a reference. */
	inode1 = inode_find(sb, 2);
	ck_assert(!inode1);

	inode_active_put(inode2);

	/*
	 * inode3 holds a plain i_ref (from inode_get). Must be released before
	 * super_block_release_dirents. inode2's hash ref will be dropped by drain;
	 * inode1 was manually unhasked so drain won't find it — its i_ref drops
	 * to 0 when inode_put(inode3) fires inode_release.
	 */
	inode_put(inode3);

	super_block_release_dirents(sb);
	super_block_put(sb);
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

	inode_active_put(inode2);

	/*
	 * inode1 must be released before teardown to avoid inode_release
	 * calling super_block_put after the sb has already been released.
	 */
	inode_active_put(inode1);

	super_block_release_dirents(sb);
	inode1 = inode_find(sb, 2);
	ck_assert(!inode1);
	inode2 = inode_find(sb, 3);
	ck_assert(!inode2);

	super_block_put(sb);
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

	inode_active_put(inode2);
	inode_active_put(inode1);

	super_block_release_dirents(sb);
	inode1 = inode_find(sb, 2);
	ck_assert(!inode1);

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

	inode_active_put(inode1);

	super_block_release_dirents(sb);
	inode1 = inode_find(sb, 2);
	ck_assert(!inode1);

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

	inode_active_put(inode1);
	inode_active_put(inode2);

	super_block_release_dirents(sb);
	inode1 = inode_find(sb, 2);
	ck_assert(!inode1);
	inode2 = inode_find(sb, 3);
	ck_assert(!inode2);

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

	super_block_release_dirents(sb1);
	super_block_put(sb1);
	super_block_put(sb2);
}

static void fs_test_perm_setup(void)
{
}

static void fs_test_perm_teardown(void)
{
}

Suite *error_suite(void)
{
	Suite *s;
	TCase *tc_core;

	s = suite_create("Inode hashing");

	/* Core test case */
	tc_core = tcase_create("Core");

	tcase_add_checked_fixture(tc_core, fs_test_perm_setup,
				  fs_test_perm_teardown);

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

	fs_test_global_init();

	s = error_suite();
	sr = srunner_create(s);

	srunner_set_fork_status(sr, CK_NOFORK);
	srunner_run_all(sr, CK_NORMAL);
	number_failed = srunner_ntests_failed(sr);
	srunner_free(sr);

	fs_test_global_fini();

	return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
