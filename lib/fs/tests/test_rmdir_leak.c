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
 * Reproducer for the ENOTEMPTY leak seen in pjdfstest/chmod/00.t and cthon special.
 * The test creates various types of files and removes them, then tries to rmdir.
 */
START_TEST(test_rmdir_leak_various_types)
{
	ck_assert_int_eq(reffs_fs_mkdir("/testdir", 0755), 0);

	/* Create and remove a regular file */
	ck_assert_int_eq(reffs_fs_create("/testdir/reg", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_unlink("/testdir/reg"), 0);

	/* Create and remove a subdirectory */
	ck_assert_int_eq(reffs_fs_mkdir("/testdir/subdir", 0755), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/testdir/subdir"), 0);

	/* Create and remove a FIFO */
	ck_assert_int_eq(reffs_fs_mknod("/testdir/fifo", S_IFIFO | 0644, 0), 0);
	ck_assert_int_eq(reffs_fs_unlink("/testdir/fifo"), 0);

	/* Create and remove a block device */
	ck_assert_int_eq(reffs_fs_mknod("/testdir/block", S_IFBLK | 0644, 0),
			 0);
	ck_assert_int_eq(reffs_fs_unlink("/testdir/block"), 0);

	/* Create and remove a character device */
	ck_assert_int_eq(reffs_fs_mknod("/testdir/char", S_IFCHR | 0644, 0), 0);
	ck_assert_int_eq(reffs_fs_unlink("/testdir/char"), 0);

	/* Create and remove a socket */
	ck_assert_int_eq(reffs_fs_mknod("/testdir/sock", S_IFSOCK | 0644, 0),
			 0);
	ck_assert_int_eq(reffs_fs_unlink("/testdir/sock"), 0);

	/* Now the directory MUST be empty */
	ck_assert_int_eq(reffs_fs_rmdir("/testdir"), 0);
}
END_TEST

/*
 * Test case specifically for the nlink safety floor logic.
 */
START_TEST(test_nlink_safety_floor)
{
	struct stat st;
	ck_assert_int_eq(reffs_fs_mkdir("/floor_test", 0755), 0);

	ck_assert_int_eq(reffs_fs_getattr("/floor_test", &st), 0);
	ck_assert_uint_eq(st.st_nlink, 2);

	/* 
	 * Rename a directory to itself.
	 * If our logic is BUGGY and does dec then inc, this might hit the floor.
	 */
	ck_assert_int_eq(reffs_fs_rename("/floor_test", "/floor_test"), 0);

	ck_assert_int_eq(reffs_fs_getattr("/floor_test", &st), 0);
	ck_assert_uint_eq(st.st_nlink, 2);

	ck_assert_int_eq(reffs_fs_rmdir("/floor_test"), 0);
}
END_TEST

Suite *rmdir_leak_suite(void)
{
	Suite *s = suite_create("fs: rmdir leak and nlink floor");
	TCase *tc = tcase_create("Core");
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_rmdir_leak_various_types);
	tcase_add_test(tc, test_nlink_safety_floor);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	fs_test_global_init();
	SRunner *sr = srunner_create(rmdir_leak_suite());
	srunner_set_fork_status(sr, CK_NOFORK);
	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	fs_test_global_fini();
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
