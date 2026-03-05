/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#include "fs_test_harness.h"

uid_t fs_test_uid;
gid_t fs_test_gid;

static void setup(void)
{
	fs_test_setup();
}
static void teardown(void)
{
	fs_test_teardown();
}

/*
 * Test that renaming a directory across parents correctly updates both.
 */
START_TEST(test_nlink_move_dir_cross_parent)
{
	struct stat st_src, st_dst;

	ck_assert_int_eq(reffs_fs_mkdir("/src", 0755), 0);
	ck_assert_int_eq(reffs_fs_mkdir("/dst", 0755), 0);
	ck_assert_int_eq(reffs_fs_mkdir("/src/sub", 0755), 0);

	ck_assert_int_eq(reffs_fs_getattr("/src", &st_src), 0);
	ck_assert_int_eq(reffs_fs_getattr("/dst", &st_dst), 0);
	ck_assert_uint_eq(st_src.st_nlink, 3);
	ck_assert_uint_eq(st_dst.st_nlink, 2);

	ck_assert_int_eq(reffs_fs_rename("/src/sub", "/dst/sub"), 0);

	ck_assert_int_eq(reffs_fs_getattr("/src", &st_src), 0);
	ck_assert_int_eq(reffs_fs_getattr("/dst", &st_dst), 0);
	ck_assert_uint_eq(st_src.st_nlink, 2);
	ck_assert_uint_eq(st_dst.st_nlink, 3);

	ck_assert_int_eq(reffs_fs_rmdir("/dst/sub"), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/src"), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/dst"), 0);
}
END_TEST

/*
 * Test that renaming a directory onto an existing empty directory
 * correctly decrements the parent's nlink (as one subdir is replaced).
 */
START_TEST(test_nlink_rename_replace_dir)
{
	struct stat st;

	ck_assert_int_eq(reffs_fs_mkdir("/p", 0755), 0);
	ck_assert_int_eq(reffs_fs_mkdir("/p/a", 0755), 0);
	ck_assert_int_eq(reffs_fs_mkdir("/p/b", 0755), 0);

	ck_assert_int_eq(reffs_fs_getattr("/p", &st), 0);
	ck_assert_uint_eq(st.st_nlink, 4); /* dot, root, a, b */

	/* Replace b with a */
	ck_assert_int_eq(reffs_fs_rename("/p/a", "/p/b"), 0);

	ck_assert_int_eq(reffs_fs_getattr("/p", &st), 0);
	/* a is gone, b is now the old a. b (old) was removed. Net -1. */
	ck_assert_uint_eq(st.st_nlink, 3);

	ck_assert_int_eq(reffs_fs_rmdir("/p/b"), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/p"), 0);
}
END_TEST

Suite *nlink_semantics_suite(void)
{
	Suite *s = suite_create("fs: nlink semantics");
	TCase *tc = tcase_create("Core");
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_nlink_move_dir_cross_parent);
	tcase_add_test(tc, test_nlink_rename_replace_dir);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	fs_test_global_init();
	SRunner *sr = srunner_create(nlink_semantics_suite());
	srunner_set_fork_status(sr, CK_NOFORK);
	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	fs_test_global_fini();
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
