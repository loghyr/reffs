/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

/*
 * fs_test_getattr.c — reffs_fs_getattr() correctness
 *
 * This is the first-line canary test.  The regression fixed in commit 37f4a90
 * (S_IFLNK typo, i_nlink=1 for directories) would have been caught
 * immediately by test_root_initial_state.
 *
 * Tests:
 *  - Root inode has exactly the attributes set by ns.c after init
 *  - getattr on a non-existent path returns -ENOENT
 *  - getattr propagates the correct mode for a directory
 *  - getattr propagates the correct mode for a regular file
 *  - st_blocks = i_used * (sb_block_size / 512); for a new dir i_used == 1
 *  - st_blocks == 0 for a new empty file (i_used == 0)
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

START_TEST(test_root_initial_state)
{
	struct stat st;

	ck_assert_int_eq(reffs_fs_getattr("/", &st), 0);

	/* The regression: i_nlink must be 2, mode must have S_IFDIR */
	ck_assert_uint_eq(st.st_nlink, 2);
	ck_assert_uint_eq(st.st_mode & S_IFMT, S_IFDIR);
	ck_assert_uint_eq(st.st_mode & 0777, 0755);

	/* uid/gid set by fs_test_setup() */
	ck_assert_uint_eq(st.st_uid, fs_test_uid);
	ck_assert_uint_eq(st.st_gid, fs_test_gid);
}
END_TEST

START_TEST(test_getattr_enoent)
{
	struct stat st;
	ck_assert_int_eq(reffs_fs_getattr("/no_such_entry", &st), -ENOENT);
}
END_TEST

START_TEST(test_getattr_dir_mode)
{
	struct stat st;

	ck_assert_int_eq(reffs_fs_mkdir("/d", 0750), 0);
	ck_assert_int_eq(reffs_fs_getattr("/d", &st), 0);

	ck_assert_uint_eq(st.st_mode & S_IFMT, S_IFDIR);
	ck_assert_uint_eq(st.st_mode & 0777, 0750);
	ck_assert_uint_eq(st.st_nlink, 2);

	ck_assert_int_eq(reffs_fs_rmdir("/d"), 0);
}
END_TEST

START_TEST(test_getattr_file_mode)
{
	struct stat st;

	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0640), 0);
	ck_assert_int_eq(reffs_fs_getattr("/f", &st), 0);

	ck_assert_uint_eq(st.st_mode, (mode_t)(S_IFREG | 0640));
	ck_assert_uint_eq(st.st_nlink, 1);
	ck_assert_int_eq(st.st_size, 0);

	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
}
END_TEST

/*
 * Pin the st_blocks formula.  For a new directory i_used == 1, so:
 *   st_blocks == st_blksize / 512
 * If i_used were ever set to sb_block_size (the old bug) this would
 * catch a value many orders of magnitude too large.
 */
START_TEST(test_getattr_dir_st_blocks)
{
	struct stat st;

	ck_assert_int_eq(reffs_fs_mkdir("/d", 0755), 0);
	ck_assert_int_eq(reffs_fs_getattr("/d", &st), 0);

	ck_assert_int_gt(st.st_blksize, 0);
	ck_assert_int_eq(st.st_blocks, st.st_blksize / 512);

	ck_assert_int_eq(reffs_fs_rmdir("/d"), 0);
}
END_TEST

START_TEST(test_getattr_file_st_blocks_zero)
{
	struct stat st;

	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_getattr("/f", &st), 0);
	ck_assert_int_eq(st.st_blocks, 0);

	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
}
END_TEST

/* ------------------------------------------------------------------ */

Suite *fs_getattr_suite(void)
{
	Suite *s = suite_create("fs: getattr");
	TCase *tc = tcase_create("Core");
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_root_initial_state);
	tcase_add_test(tc, test_getattr_enoent);
	tcase_add_test(tc, test_getattr_dir_mode);
	tcase_add_test(tc, test_getattr_file_mode);
	tcase_add_test(tc, test_getattr_dir_st_blocks);
	tcase_add_test(tc, test_getattr_file_st_blocks_zero);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	fs_test_global_init();
	SRunner *sr = srunner_create(fs_getattr_suite());
	srunner_set_fork_status(sr, CK_NOFORK);
	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	fs_test_global_fini();
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
