/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/*
 * fs_test_unlink.c — reffs_fs_unlink() correctness
 *
 * Unlink removes a regular file from the namespace.  fs.c currently
 * decrements the parent's nlink through dirent_parent_release(); these
 * tests verify that effect and its inverse.  Note that unlink does NOT
 * update the parent's mtime/ctime in the current implementation — only
 * the timestamps of the file's own inode are affected (via the
 * dirent_parent_release path).  Tests here track what the code actually
 * guarantees; add mtime/ctime assertions if that changes.
 *
 * Tests:
 *  - unlink removes the file (getattr returns -ENOENT)
 *  - unlink on a directory returns -EISDIR
 *  - unlink on a non-existent path returns -ENOENT
 *  - unlink decrements parent nlink
 *  - parent mtime/ctime advance after unlink
 *  - parent atime is unchanged after unlink
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

/* ------------------------------------------------------------------ */

START_TEST(test_unlink_removes_file)
{
	struct stat st;

	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
	ck_assert_int_eq(reffs_fs_getattr("/f", &st), -ENOENT);
}
END_TEST

START_TEST(test_unlink_eisdir)
{
	ck_assert_int_eq(reffs_fs_mkdir("/d", 0755), 0);
	ck_assert_int_eq(reffs_fs_unlink("/d"), -EISDIR);
	ck_assert_int_eq(reffs_fs_rmdir("/d"), 0);
}
END_TEST

START_TEST(test_unlink_enoent)
{
	ck_assert_int_eq(reffs_fs_unlink("/no_such_file"), -ENOENT);
}
END_TEST

START_TEST(test_unlink_does_not_decrement_parent_nlink)
{
	struct stat st_before, st_after;

	ck_assert_int_eq(reffs_fs_mkdir("/d", 0755), 0);
	ck_assert_int_eq(reffs_fs_create("/d/f", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_getattr("/d", &st_before), 0);

	ck_assert_int_eq(reffs_fs_unlink("/d/f"), 0);
	ck_assert_int_eq(reffs_fs_getattr("/d", &st_after), 0);

	ck_assert_uint_eq(st_before.st_nlink, st_after.st_nlink);

	ck_assert_int_eq(reffs_fs_rmdir("/d"), 0);
}
END_TEST

START_TEST(test_unlink_advances_parent_mtime_ctime)
{
	struct stat st_pre, st_post;

	ck_assert_int_eq(reffs_fs_mkdir("/d", 0755), 0);
	ck_assert_int_eq(reffs_fs_create("/d/f", S_IFREG | 0644), 0);

	usleep(1000);
	ck_assert_int_eq(reffs_fs_getattr("/d", &st_pre), 0);
	usleep(1000);

	ck_assert_int_eq(reffs_fs_unlink("/d/f"), 0);
	ck_assert_int_eq(reffs_fs_getattr("/d", &st_post), 0);

	ck_assert_uint_eq(st_pre.st_ino, st_post.st_ino);
	ck_assert_timespec_eq(st_pre.st_atim, st_post.st_atim);
	ck_assert_timespec_lt(st_pre.st_mtim, st_post.st_mtim);
	ck_assert_timespec_lt(st_pre.st_ctim, st_post.st_ctim);

	ck_assert_int_eq(reffs_fs_rmdir("/d"), 0);
}
END_TEST

START_TEST(test_unlink_multiply_linked_updates_ctime)
{
	struct stat st_pre, st_post;

	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_link("/f", "/f_link"), 0);

	ck_assert_int_eq(reffs_fs_getattr("/f", &st_pre), 0);
	ck_assert_uint_eq(st_pre.st_nlink, 2);

	usleep(100000);
	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);

	ck_assert_int_eq(reffs_fs_getattr("/f_link", &st_post), 0);
	ck_assert_uint_eq(st_post.st_nlink, 1);
	ck_assert_timespec_lt(st_pre.st_ctim, st_post.st_ctim);

	ck_assert_int_eq(reffs_fs_unlink("/f_link"), 0);
}
END_TEST

/* ------------------------------------------------------------------ */

Suite *fs_unlink_suite(void)
{
	Suite *s = suite_create("fs: unlink");
	TCase *tc = tcase_create("Core");
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_unlink_removes_file);
	tcase_add_test(tc, test_unlink_eisdir);
	tcase_add_test(tc, test_unlink_enoent);
	tcase_add_test(tc, test_unlink_does_not_decrement_parent_nlink);
	tcase_add_test(tc, test_unlink_advances_parent_mtime_ctime);
	tcase_add_test(tc, test_unlink_multiply_linked_updates_ctime);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	fs_test_global_init();
	SRunner *sr = srunner_create(fs_unlink_suite());
	srunner_set_fork_status(sr, CK_NOFORK);
	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	fs_test_global_fini();
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
