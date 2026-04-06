/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_JEMALLOC
#include <jemalloc/jemalloc.h>
#endif

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <check.h>
#include "reffs/super_block.h"
#include "reffs/inode.h"
#include "reffs/test.h"
#include "fs_test_harness.h"
#include "reffs/trace/fs.h"

static uint64_t sb_id_next = 0;

START_TEST(test_add_sb_1)
{
	struct super_block *sb;
	uint64_t sb_id;

	sb_id = __atomic_add_fetch(&sb_id_next, 1, __ATOMIC_RELAXED);

	sb = super_block_alloc(sb_id, "/", REFFS_STORAGE_RAM, NULL);
	ck_assert_ptr_nonnull(sb);

	super_block_release_dirents(sb);
	super_block_put(sb);
}
END_TEST

START_TEST(test_add_sb_2)
{
	struct super_block *sb;
	uint64_t sb_id;

	sb_id = __atomic_add_fetch(&sb_id_next, 1, __ATOMIC_RELAXED);

	sb = super_block_alloc(sb_id, "/", REFFS_STORAGE_RAM, NULL);
	ck_assert_ptr_nonnull(sb);

	super_block_release_dirents(sb);
	super_block_put(sb);
}
END_TEST

START_TEST(test_find_sb_inode)
{
	struct inode *inode1;
	struct inode *inode2;
	struct super_block *sb;
	int ret;
	uint64_t sb_id;

	sb_id = __atomic_add_fetch(&sb_id_next, 1, __ATOMIC_RELAXED);

	sb = super_block_alloc(sb_id, "/", REFFS_STORAGE_RAM, NULL);
	ck_assert_ptr_nonnull(sb);

	ret = super_block_dirent_create(sb, NULL, reffs_life_action_birth);
	ck_assert_int_eq(ret, 0);

	inode1 = inode_alloc(sb, INODE_ROOT_ID);
	ck_assert_ptr_eq(inode1, sb->sb_dirent->rd_inode);

	inode2 = inode_find(sb, INODE_ROOT_ID);
	ck_assert_ptr_eq(inode2, sb->sb_dirent->rd_inode);

	inode_active_put(inode1);
	inode_active_put(inode2);

	super_block_release_dirents(sb);
	super_block_put(sb);
}
END_TEST

START_TEST(test_find_sb_inode_put)
{
	struct inode *inode1;
	struct inode *inode2;
	struct super_block *sb;
	int ret;
	uint64_t sb_id;

	sb_id = __atomic_add_fetch(&sb_id_next, 1, __ATOMIC_RELAXED);

	sb = super_block_alloc(sb_id, "/", REFFS_STORAGE_RAM, NULL);
	ck_assert_ptr_nonnull(sb);

	ret = super_block_dirent_create(sb, NULL, reffs_life_action_birth);
	ck_assert_int_eq(ret, 0);

	inode1 = inode_alloc(sb, 1);
	ck_assert_ptr_eq(inode1, sb->sb_dirent->rd_inode);

	inode2 = inode_find(sb, 1);
	ck_assert_ptr_eq(inode2, sb->sb_dirent->rd_inode);

	inode_active_put(inode1);
	inode_active_put(inode2);

	super_block_release_dirents(sb);
	super_block_put(sb);
}
END_TEST

START_TEST(test_find_sb_inode_unhash)
{
	struct inode *inode1;
	struct inode *inode2;
	struct super_block *sb;
	int ret;
	uint64_t sb_id;

	sb_id = __atomic_add_fetch(&sb_id_next, 1, __ATOMIC_RELAXED);

	sb = super_block_alloc(sb_id, "/", REFFS_STORAGE_RAM, NULL);
	ck_assert_ptr_nonnull(sb);

	ret = super_block_dirent_create(sb, NULL, reffs_life_action_birth);
	ck_assert_int_eq(ret, 0);

	inode1 = inode_alloc(sb, INODE_ROOT_ID);
	ck_assert_ptr_eq(inode1, sb->sb_dirent->rd_inode);

	inode2 = inode_find(sb, INODE_ROOT_ID);
	ck_assert_ptr_eq(inode2, sb->sb_dirent->rd_inode);

	inode_active_put(inode1);
	inode_active_put(inode2);

	super_block_release_dirents(sb);
	super_block_put(sb);
}
END_TEST

START_TEST(test_add_inode_1)
{
	struct inode *inode;
	struct super_block *sb;
	uint64_t sb_id;

	sb_id = __atomic_add_fetch(&sb_id_next, 1, __ATOMIC_RELAXED);

	sb = super_block_alloc(sb_id, "/", REFFS_STORAGE_RAM, NULL);
	ck_assert_ptr_nonnull(sb);

	inode = inode_alloc(sb, 2);
	ck_assert_ptr_nonnull(inode);
	inode_active_put(inode);

	super_block_release_dirents(sb);
	inode = inode_find(sb, 2);
	ck_assert_ptr_null(inode);

	super_block_put(sb);
}
END_TEST

START_TEST(test_add_inode_2)
{
	struct inode *inode1, *inode2;
	struct super_block *sb;
	uint64_t sb_id;

	sb_id = __atomic_add_fetch(&sb_id_next, 1, __ATOMIC_RELAXED);

	sb = super_block_alloc(sb_id, "/", REFFS_STORAGE_RAM, NULL);
	ck_assert_ptr_nonnull(sb);

	inode1 = inode_alloc(sb, 2);
	ck_assert_ptr_nonnull(inode1);

	inode2 = inode_alloc(sb, 3);
	ck_assert_ptr_nonnull(inode2);

	inode_active_put(inode1);
	inode_active_put(inode2);

	super_block_release_dirents(sb);
	inode1 = inode_find(sb, 2);
	ck_assert_ptr_null(inode1);
	inode2 = inode_find(sb, 3);
	ck_assert_ptr_null(inode2);

	super_block_put(sb);
}
END_TEST

START_TEST(test_put_inode_1)
{
	struct inode *inode1, *inode2, *inode3;
	struct super_block *sb;
	uint64_t sb_id;

	sb_id = __atomic_add_fetch(&sb_id_next, 1, __ATOMIC_RELAXED);

	sb = super_block_alloc(sb_id, "/", REFFS_STORAGE_RAM, NULL);
	ck_assert_ptr_nonnull(sb);

	inode1 = inode_alloc(sb, 2);
	ck_assert_ptr_nonnull(inode1);
	inode2 = inode_alloc(sb, 3);
	ck_assert_ptr_nonnull(inode2);

	inode3 = inode_find(sb, 2);
	ck_assert_ptr_nonnull(inode3);

	inode_active_put(inode1);
	inode1 = inode_find(sb, 2);
	ck_assert_ptr_eq(inode1, inode3);
	inode_active_put(inode1);

	inode_active_put(inode2);

	/* Release inode3 before teardown to avoid super_block_put double-put */
	inode_active_put(inode3);

	super_block_release_dirents(sb);
	super_block_put(sb);
}
END_TEST

START_TEST(test_get_inode_1)
{
	struct inode *inode1, *inode2, *inode3;
	struct super_block *sb;
	uint64_t sb_id;

	sb_id = __atomic_add_fetch(&sb_id_next, 1, __ATOMIC_RELAXED);

	sb = super_block_alloc(sb_id, "/", REFFS_STORAGE_RAM, NULL);
	ck_assert_ptr_nonnull(sb);

	inode1 = inode_alloc(sb, 2);
	ck_assert_ptr_nonnull(inode1);

	inode2 = inode_alloc(sb, 3);
	ck_assert_ptr_nonnull(inode2);

	/* inode_get bumps only i_ref -- release with inode_put */
	inode3 = inode_get(inode1);
	ck_assert_ptr_nonnull(inode3);

	inode_unhash(inode1);
	inode_active_put(inode1);
	/* Better not be found even though inode3 has a reference. */
	inode1 = inode_find(sb, 2);
	ck_assert_ptr_null(inode1);

	inode_active_put(inode2);

	/* Release plain i_ref ref from inode_get before teardown */
	inode_put(inode3);

	super_block_release_dirents(sb);
	super_block_put(sb);
}
END_TEST

START_TEST(test_sb_put_inode_1)
{
	struct inode *inode1, *inode2;
	struct super_block *sb;
	uint64_t sb_id;

	sb_id = __atomic_add_fetch(&sb_id_next, 1, __ATOMIC_RELAXED);

	sb = super_block_alloc(sb_id, "/", REFFS_STORAGE_RAM, NULL);
	ck_assert_ptr_nonnull(sb);

	inode1 = inode_alloc(sb, 2);
	ck_assert_ptr_nonnull(inode1);

	inode2 = inode_alloc(sb, 3);
	ck_assert_ptr_nonnull(inode2);

	inode_active_put(inode2);
	inode_active_put(inode1);

	super_block_release_dirents(sb);
	inode1 = inode_find(sb, 2);
	ck_assert_ptr_null(inode1);
	inode2 = inode_find(sb, 3);
	ck_assert_ptr_null(inode2);

	super_block_put(sb);
}
END_TEST

START_TEST(test_find_inode_1)
{
	struct inode *inode1, *inode2;
	struct super_block *sb;
	uint64_t sb_id;

	sb_id = __atomic_add_fetch(&sb_id_next, 1, __ATOMIC_RELAXED);

	sb = super_block_alloc(sb_id, "/", REFFS_STORAGE_RAM, NULL);
	ck_assert_ptr_nonnull(sb);

	inode1 = inode_alloc(sb, 2);
	ck_assert_ptr_nonnull(inode1);

	inode2 = inode_find(sb, 2);
	ck_assert_ptr_eq(inode2, inode1);

	inode_active_put(inode2);
	inode_active_put(inode1);

	super_block_release_dirents(sb);
	inode1 = inode_find(sb, 2);
	ck_assert_ptr_null(inode1);

	super_block_put(sb);
}
END_TEST

START_TEST(test_find_inode_1_sb_NULL)
{
	struct inode *inode1, *inode2;
	struct super_block *sb;
	uint64_t sb_id;

	sb_id = __atomic_add_fetch(&sb_id_next, 1, __ATOMIC_RELAXED);

	sb = super_block_alloc(sb_id, "/", REFFS_STORAGE_RAM, NULL);
	ck_assert_ptr_nonnull(sb);

	inode1 = inode_alloc(sb, 2);
	ck_assert_ptr_nonnull(inode1);

	inode2 = inode_find(NULL, 2);
	ck_assert_ptr_null(inode2);

	inode_active_put(inode1);

	super_block_release_dirents(sb);
	inode1 = inode_find(sb, 2);
	ck_assert_ptr_null(inode1);

	super_block_put(sb);
}
END_TEST

START_TEST(test_find_inode_3)
{
	struct inode *inode1, *inode2, *inode3;
	struct super_block *sb;
	uint64_t sb_id;

	sb_id = __atomic_add_fetch(&sb_id_next, 1, __ATOMIC_RELAXED);

	sb = super_block_alloc(sb_id, "/", REFFS_STORAGE_RAM, NULL);
	ck_assert_ptr_nonnull(sb);
	trace_fs_super_block(sb, __func__, __LINE__);

	inode1 = inode_alloc(sb, 2);
	ck_assert_ptr_nonnull(inode1);

	inode2 = inode_alloc(sb, 3);
	ck_assert_ptr_nonnull(inode2);

	inode3 = inode_find(sb, 4);
	ck_assert_ptr_null(inode3);

	inode_active_put(inode1);
	inode_active_put(inode2);

	super_block_release_dirents(sb);
	inode1 = inode_find(sb, 2);
	ck_assert_ptr_null(inode1);
	inode2 = inode_find(sb, 3);
	ck_assert_ptr_null(inode2);

	super_block_put(sb);

	rcu_barrier();

	sb = super_block_find(sb_id);
	trace_fs_super_block(sb, __func__, __LINE__);
	ck_assert_ptr_null(sb);
}
END_TEST

START_TEST(test_find_sb_1)
{
	struct super_block *sb1, *sb2;
	uint64_t sb_id;

	sb_id = __atomic_add_fetch(&sb_id_next, 1, __ATOMIC_RELAXED);

	sb1 = super_block_alloc(sb_id, "/", REFFS_STORAGE_RAM, NULL);
	ck_assert_ptr_nonnull(sb1);

	sb2 = super_block_find(sb_id);
	ck_assert_ptr_eq(sb1, sb2);

	super_block_release_dirents(sb1);
	super_block_put(sb1);
	super_block_put(sb2);
}
END_TEST

Suite *inode_hashing_no_check_suite(void)
{
	Suite *s = suite_create("fs: inode hashing (no check)");
	TCase *tc = tcase_create("Core");
	tcase_add_test(tc, test_add_sb_1);
	tcase_add_test(tc, test_add_inode_1);
	tcase_add_test(tc, test_add_inode_2);
	tcase_add_test(tc, test_find_inode_1);
	tcase_add_test(tc, test_find_inode_1_sb_NULL);
	tcase_add_test(tc, test_find_inode_3);
	tcase_add_test(tc, test_find_sb_1);
	tcase_add_test(tc, test_get_inode_1);
	tcase_add_test(tc, test_put_inode_1);
	tcase_add_test(tc, test_sb_put_inode_1);
	tcase_add_test(tc, test_add_sb_2);
	tcase_add_test(tc, test_find_sb_inode);
	tcase_add_test(tc, test_find_sb_inode_put);
	tcase_add_test(tc, test_find_sb_inode_unhash);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	return fs_test_run(inode_hashing_no_check_suite());
}
