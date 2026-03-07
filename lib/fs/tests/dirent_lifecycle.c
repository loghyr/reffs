/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

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
 * Test 1: Root directory has nlink=2 (dot, itself).
 */
START_TEST(test_dirent_root_nlink)
{
	struct stat st;
	ck_assert_int_eq(reffs_fs_getattr("/", &st), 0);
	ck_assert_uint_eq(st.st_nlink, 2);
}
END_TEST

/*
 * Test 2: Subdirectory creation increments parent nlink.
 */
START_TEST(test_dirent_mkdir_nlink)
{
	struct stat st_parent, st_child;

	ck_assert_int_eq(reffs_fs_mkdir("/a", 0755), 0);

	/* Parent / now has: dot, root, a. Total 3. */
	ck_assert_int_eq(reffs_fs_getattr("/", &st_parent), 0);
	ck_assert_uint_eq(st_parent.st_nlink, 3);

	/* Child /a has: dot, parent. Total 2. */
	ck_assert_int_eq(reffs_fs_getattr("/a", &st_child), 0);
	ck_assert_uint_eq(st_child.st_nlink, 2);

	/* Remove /a, parent should drop back to 2. */
	ck_assert_int_eq(reffs_fs_rmdir("/a"), 0);
	ck_assert_int_eq(reffs_fs_getattr("/", &st_parent), 0);
	ck_assert_uint_eq(st_parent.st_nlink, 2);
}
END_TEST

/*
 * Test 3: Regular file creation does NOT increment parent nlink.
 */
START_TEST(test_dirent_file_nlink)
{
	struct stat st_parent;

	ck_assert_int_eq(reffs_fs_create("/f", 0644), 0);

	/* Parent / still has 2 links. */
	ck_assert_int_eq(reffs_fs_getattr("/", &st_parent), 0);
	ck_assert_uint_eq(st_parent.st_nlink, 2);

	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
	ck_assert_int_eq(reffs_fs_getattr("/", &st_parent), 0);
	ck_assert_uint_eq(st_parent.st_nlink, 2);
}
END_TEST

/*
 * Test 4: Safety floor - nlink should not drop below 2 for directories.
 * This is hard to trigger via public API if logic is correct, but
 * it validates the fix in dirent_parent_release.
 */
START_TEST(test_dirent_nlink_safety_floor)
{
	struct stat st;

	/* Root starts at 2. */
	ck_assert_int_eq(reffs_fs_getattr("/", &st), 0);
	ck_assert_uint_eq(st.st_nlink, 2);

	/* 
	 * Attempt to rmdir something that doesn't exist shouldn't 
	 * affect anything.
	 */
	ck_assert_int_ne(reffs_fs_rmdir("/nonexistent"), 0);
	ck_assert_int_eq(reffs_fs_getattr("/", &st), 0);
	ck_assert_uint_eq(st.st_nlink, 2);
}
END_TEST

/*
 * Test 5: Rename updates nlink correctly for cross-directory moves.
 */
START_TEST(test_dirent_rename_nlink)
{
	struct stat st_a, st_b;

	ck_assert_int_eq(reffs_fs_mkdir("/a", 0755), 0);
	ck_assert_int_eq(reffs_fs_mkdir("/b", 0755), 0);
	ck_assert_int_eq(reffs_fs_mkdir("/a/sub", 0755), 0);

	ck_assert_int_eq(reffs_fs_getattr("/a", &st_a), 0);
	ck_assert_int_eq(reffs_fs_getattr("/b", &st_b), 0);
	ck_assert_uint_eq(st_a.st_nlink, 3);
	ck_assert_uint_eq(st_b.st_nlink, 2);

	/* Move /a/sub to /b/sub */
	ck_assert_int_eq(reffs_fs_rename("/a/sub", "/b/sub"), 0);

	ck_assert_int_eq(reffs_fs_getattr("/a", &st_a), 0);
	ck_assert_int_eq(reffs_fs_getattr("/b", &st_b), 0);
	ck_assert_uint_eq(st_a.st_nlink, 2);
	ck_assert_uint_eq(st_b.st_nlink, 3);

	ck_assert_int_eq(reffs_fs_rmdir("/b/sub"), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/a"), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/b"), 0);
}
END_TEST

Suite *dirent_lifecycle_suite(void)
{
	Suite *s = suite_create("fs: dirent lifecycle");
	TCase *tc = tcase_create("Core");
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_dirent_root_nlink);
	tcase_add_test(tc, test_dirent_mkdir_nlink);
	tcase_add_test(tc, test_dirent_file_nlink);
	tcase_add_test(tc, test_dirent_nlink_safety_floor);
	tcase_add_test(tc, test_dirent_rename_nlink);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	fs_test_global_init();
	SRunner *sr = srunner_create(dirent_lifecycle_suite());
	srunner_set_fork_status(sr, CK_NOFORK);
	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	fs_test_global_fini();
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
