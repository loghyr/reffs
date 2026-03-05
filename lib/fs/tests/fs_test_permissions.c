/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/stat.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include "reffs/fs.h"
#include "reffs/context.h"
#include "fs_test_harness.h"

uid_t fs_test_uid;
gid_t fs_test_gid;

START_TEST(test_rename_src_parent_no_write)
{
	struct reffs_context ctx = {.uid = 1000, .gid = 1000};

	/* Create /p as root (UID 0), with 0755 (no write for others) */
	reffs_set_context(NULL); 
	ck_assert_int_eq(reffs_fs_mkdir("/p", 0755), 0);
	ck_assert_int_eq(reffs_fs_create("/p/f", S_IFREG | 0644), 0);

	/* Try rename as user 1000 */
	reffs_set_context(&ctx);
	ck_assert_int_eq(reffs_fs_rename("/p/f", "/p/f2"), -EACCES);

	/* Reset to root and verify we can still rename */
	reffs_set_context(NULL);
	ck_assert_int_eq(reffs_fs_rename("/p/f", "/p/f2"), 0);
	
	ck_assert_int_eq(reffs_fs_unlink("/p/f2"), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/p"), 0);
}
END_TEST

START_TEST(test_mkdir_no_write_permission)
{
	struct reffs_context ctx = {.uid = 1000, .gid = 1000};

	/* Create /root_dir as root (UID 0), with 0755 */
	reffs_set_context(NULL);
	ck_assert_int_eq(reffs_fs_mkdir("/root_dir", 0755), 0);
	
	/* Try mkdir as user 1000 */
	reffs_set_context(&ctx);
	ck_assert_int_eq(reffs_fs_mkdir("/root_dir/sub", 0755), -EACCES);

	/* Reset to root and verify success */
	reffs_set_context(NULL);
	ck_assert_int_eq(reffs_fs_mkdir("/root_dir/sub", 0755), 0);

	ck_assert_int_eq(reffs_fs_rmdir("/root_dir/sub"), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/root_dir"), 0);
}
END_TEST

START_TEST(test_rename_search_permission_failure)
{
	struct reffs_context ctx = {.uid = 1000, .gid = 1000};

	/* Create /top/p/f as root */
	reffs_set_context(NULL);
	ck_assert_int_eq(reffs_fs_mkdir("/top", 0755), 0);
	ck_assert_int_eq(reffs_fs_mkdir("/top/p", 0700), 0); /* No perms for others */
	ck_assert_int_eq(reffs_fs_create("/top/p/f", S_IFREG | 0644), 0);

	/* Try rename /top/p/f as user 1000 - should fail on search of /top/p */
	reffs_set_context(&ctx);
	ck_assert_int_eq(reffs_fs_rename("/top/p/f", "/top/f2"), -EACCES);

	/* Cleanup as root */
	reffs_set_context(NULL);
	ck_assert_int_eq(reffs_fs_unlink("/top/p/f"), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/top/p"), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/top"), 0);
}
END_TEST

Suite *fs_permission_suite(void)
{
	Suite *s = suite_create("fs: permissions");
	TCase *tc = tcase_create("Core");

	tcase_add_checked_fixture(tc, fs_test_setup, fs_test_teardown);
	tcase_add_test(tc, test_rename_src_parent_no_write);
	tcase_add_test(tc, test_mkdir_no_write_permission);
	tcase_add_test(tc, test_rename_search_permission_failure);

	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int number_failed;
	SRunner *sr = srunner_create(fs_permission_suite());

	srunner_set_fork_status(sr, CK_NOFORK);
	srunner_run_all(sr, CK_NORMAL);
	number_failed = srunner_ntests_failed(sr);
	srunner_free(sr);

	return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
