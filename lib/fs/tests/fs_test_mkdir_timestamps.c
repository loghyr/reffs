/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/*
 * fs_test_mkdir_timestamps.c — mkdir/rmdir mtime/ctime semantics
 *
 * Separated from the nlink tests because timestamp assertions require
 * usleep() to guarantee clock advancement, which would slow the nlink
 * tests unnecessarily.
 *
 * POSIX: mkdir/rmdir must advance the parent's mtime and ctime.
 * The parent's atime, ino, uid, gid, and size must not change.
 *
 * Tests:
 *  - mkdir advances parent mtime and ctime, leaves atime unchanged
 *  - rmdir advances parent mtime and ctime, leaves atime unchanged
 *  - New child directory inherits uid/gid from process (not parent inode)
 *  - New child directory mode bits match the mode argument
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

START_TEST(test_mkdir_advances_parent_mtime_ctime)
{
	struct stat st_pre, st_post;

	ck_assert_int_eq(reffs_fs_mkdir("/p", 0755), 0);
	ck_assert_int_eq(reffs_fs_getattr("/p", &st_pre), 0);

	usleep(1000);

	ck_assert_int_eq(reffs_fs_mkdir("/p/c", 0755), 0);
	ck_assert_int_eq(reffs_fs_getattr("/p", &st_post), 0);

	/* identity stable */
	ck_assert_uint_eq(st_pre.st_ino, st_post.st_ino);
	ck_assert_uint_eq(st_pre.st_uid, st_post.st_uid);
	ck_assert_uint_eq(st_pre.st_gid, st_post.st_gid);
	ck_assert_uint_eq(st_pre.st_size, st_post.st_size);
	ck_assert_uint_eq(st_pre.st_nlink, st_post.st_nlink - 1);

	/* timestamps */
	ck_assert_timespec_eq(st_pre.st_atim, st_post.st_atim);
	ck_assert_timespec_lt(st_pre.st_mtim, st_post.st_mtim);
	ck_assert_timespec_lt(st_pre.st_ctim, st_post.st_ctim);

	ck_assert_int_eq(reffs_fs_rmdir("/p/c"), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/p"), 0);
}
END_TEST

START_TEST(test_rmdir_advances_parent_mtime_ctime)
{
	struct stat st_pre, st_post;

	ck_assert_int_eq(reffs_fs_mkdir("/p", 0755), 0);
	ck_assert_int_eq(reffs_fs_mkdir("/p/c", 0755), 0);

	usleep(1000);
	ck_assert_int_eq(reffs_fs_getattr("/p", &st_pre), 0);

	usleep(1000);
	ck_assert_int_eq(reffs_fs_rmdir("/p/c"), 0);
	ck_assert_int_eq(reffs_fs_getattr("/p", &st_post), 0);

	ck_assert_uint_eq(st_pre.st_ino, st_post.st_ino);
	ck_assert_uint_eq(st_pre.st_nlink, st_post.st_nlink + 1);
	ck_assert_timespec_eq(st_pre.st_atim, st_post.st_atim);
	ck_assert_timespec_lt(st_pre.st_mtim, st_post.st_mtim);
	ck_assert_timespec_lt(st_pre.st_ctim, st_post.st_ctim);

	ck_assert_int_eq(reffs_fs_rmdir("/p"), 0);
}
END_TEST

START_TEST(test_new_dir_inherits_process_uid_gid)
{
	struct stat st;

	ck_assert_int_eq(reffs_fs_mkdir("/d", 0755), 0);
	ck_assert_int_eq(reffs_fs_getattr("/d", &st), 0);

	ck_assert_uint_eq(st.st_uid, fs_test_uid);
	ck_assert_uint_eq(st.st_gid, fs_test_gid);

	ck_assert_int_eq(reffs_fs_rmdir("/d"), 0);
}
END_TEST

START_TEST(test_new_dir_mode_matches_argument)
{
	struct stat st;

	ck_assert_int_eq(reffs_fs_mkdir("/d", 0640), 0);
	ck_assert_int_eq(reffs_fs_getattr("/d", &st), 0);

	ck_assert_uint_eq(st.st_mode, (mode_t)(S_IFDIR | 0640));

	ck_assert_int_eq(reffs_fs_rmdir("/d"), 0);
}
END_TEST

/* ------------------------------------------------------------------ */

Suite *fs_mkdir_ts_suite(void)
{
	Suite *s = suite_create("fs: mkdir/rmdir timestamps");
	TCase *tc = tcase_create("Core");
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_mkdir_advances_parent_mtime_ctime);
	tcase_add_test(tc, test_rmdir_advances_parent_mtime_ctime);
	tcase_add_test(tc, test_new_dir_inherits_process_uid_gid);
	tcase_add_test(tc, test_new_dir_mode_matches_argument);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	fs_test_global_init();
	SRunner *sr = srunner_create(fs_mkdir_ts_suite());
	srunner_set_fork_status(sr, CK_NOFORK);
	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	fs_test_global_fini();
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
