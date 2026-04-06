/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/*
 * fs_test_create.c -- reffs_fs_create() initial attribute correctness
 *
 * create() is the "touch" operation.  Tests focus on the state of the
 * newly created inode, not the write/read path (that lives in
 * fs_test_write_read.c).
 *
 * Tests:
 *  - Initial size is 0
 *  - Mode is stored exactly as supplied
 *  - uid/gid come from the calling process
 *  - nlink is 1 (regular files are not directories)
 *  - st_blocks is 0 (no data block allocated yet)
 *  - Requesting S_IFDIR mode returns -EISDIR
 *  - Creating in a non-directory parent returns -ENOTDIR
 *  - Duplicate create returns -EEXIST
 *  - create() in a deep path works if intermediate dirs exist
 */

#include "fs_test_harness.h"

static void setup(void)
{
	fs_test_setup();
}
static void teardown(void)
{
	fs_test_teardown();
}

/* ------------------------------------------------------------------ */

START_TEST(test_create_initial_size_zero)
{
	struct stat st;

	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_getattr("/f", &st), 0);
	ck_assert_int_eq(st.st_size, 0);

	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
}
END_TEST

START_TEST(test_create_mode_stored)
{
	struct stat st;

	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0755), 0);
	ck_assert_int_eq(reffs_fs_getattr("/f", &st), 0);
	ck_assert_uint_eq(st.st_mode, (mode_t)(S_IFREG | 0755));

	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
}
END_TEST

START_TEST(test_create_uid_gid)
{
	struct stat st;

	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_getattr("/f", &st), 0);
	ck_assert_uint_eq(st.st_uid, fs_test_uid);
	ck_assert_uint_eq(st.st_gid, fs_test_gid);

	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
}
END_TEST

START_TEST(test_create_nlink_is_1)
{
	struct stat st;

	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_getattr("/f", &st), 0);
	ck_assert_uint_eq(st.st_nlink, 1);

	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
}
END_TEST

START_TEST(test_create_st_blocks_zero)
{
	struct stat st;

	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_getattr("/f", &st), 0);
	ck_assert_int_eq(st.st_blocks, 0);

	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
}
END_TEST

START_TEST(test_create_eisdir_on_dir_mode)
{
	/* S_IFDIR in the mode argument must be rejected */
	ck_assert_int_eq(reffs_fs_create("/f", S_IFDIR | 0755), -EISDIR);
}
END_TEST

START_TEST(test_create_enotdir_in_file_parent)
{
	/* Trying to create inside a regular file as if it were a dir */
	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_create("/f/child", S_IFREG | 0644), -ENOTDIR);
	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
}
END_TEST

START_TEST(test_create_eexist)
{
	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0644), -EEXIST);
	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
}
END_TEST

START_TEST(test_create_deep_path)
{
	struct stat st;

	ck_assert_int_eq(reffs_fs_mkdir("/a", 0755), 0);
	ck_assert_int_eq(reffs_fs_mkdir("/a/b", 0755), 0);
	ck_assert_int_eq(reffs_fs_create("/a/b/f", S_IFREG | 0600), 0);

	ck_assert_int_eq(reffs_fs_getattr("/a/b/f", &st), 0);
	ck_assert_int_eq(st.st_size, 0);
	ck_assert_uint_eq(st.st_mode, (mode_t)(S_IFREG | 0600));

	ck_assert_int_eq(reffs_fs_unlink("/a/b/f"), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/a/b"), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/a"), 0);
}
END_TEST

/* ------------------------------------------------------------------ */

Suite *fs_create_suite(void)
{
	Suite *s = suite_create("fs: create");
	TCase *tc = tcase_create("Core");
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_create_initial_size_zero);
	tcase_add_test(tc, test_create_mode_stored);
	tcase_add_test(tc, test_create_uid_gid);
	tcase_add_test(tc, test_create_nlink_is_1);
	tcase_add_test(tc, test_create_st_blocks_zero);
	tcase_add_test(tc, test_create_eisdir_on_dir_mode);
	tcase_add_test(tc, test_create_enotdir_in_file_parent);
	tcase_add_test(tc, test_create_eexist);
	tcase_add_test(tc, test_create_deep_path);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	return fs_test_run(fs_create_suite());
}
