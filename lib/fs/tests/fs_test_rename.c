/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

/*
 * fs_test_rename.c — reffs_fs_rename() correctness
 *
 * rename() changes a file's name and/or parent directory.  The critical
 * invariants are:
 *
 *   File identity: ino, uid, gid, size, mtime, ctime, atime are unchanged.
 *   Source parent: nlink decrements; mtime/ctime advance; atime unchanged.
 *   Dest parent:   nlink increments; mtime/ctime advance; atime unchanged.
 *   Old path:      getattr returns -ENOENT.
 *   New path:      file is visible with same identity.
 *
 * Tests cover:
 *  - rename file from root into a subdirectory
 *  - rename file back from subdirectory to root (reverse direction)
 *  - rename "/" returns -EFAULT
 *  - rename to "/" returns -EFAULT
 *  - rename within the same directory (rename in place)
 *  - rename replaces an existing regular file at the destination
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

/*
 * Helper: assert that stat fields that must survive a rename are equal.
 * Timestamps are NOT checked here because callers provide separate
 * before/after specs for what changed vs what did not.
 */
static void assert_identity_preserved(const struct stat *before,
				      const struct stat *after)
{
	ck_assert_uint_eq(before->st_ino, after->st_ino);
	ck_assert_uint_eq(before->st_uid, after->st_uid);
	ck_assert_uint_eq(before->st_gid, after->st_gid);
	ck_assert_int_eq((int)before->st_size, (int)after->st_size);
}

/* ------------------------------------------------------------------ */

START_TEST(test_rename_file_into_subdir)
{
	struct stat st_file_pre, st_file_post;
	struct stat st_src_pre, st_src_post;
	struct stat st_dst_pre, st_dst_post;

	ck_assert_int_eq(reffs_fs_mkdir("/d", 0755), 0);
	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_write("/f", "hello", 5, 0), 5);

	ck_assert_int_eq(reffs_fs_getattr("/f", &st_file_pre), 0);
	ck_assert_int_eq(reffs_fs_getattr("/", &st_src_pre), 0);
	ck_assert_int_eq(reffs_fs_getattr("/d", &st_dst_pre), 0);

	usleep(1000);
	ck_assert_int_eq(reffs_fs_rename("/f", "/d/f"), 0);

	/* old path gone */
	ck_assert_int_eq(reffs_fs_getattr("/f", &st_file_post), -ENOENT);

	/* new path: file identity preserved */
	ck_assert_int_eq(reffs_fs_getattr("/d/f", &st_file_post), 0);
	assert_identity_preserved(&st_file_pre, &st_file_post);
	ck_assert_timespec_eq(st_file_pre.st_atim, st_file_post.st_atim);
	ck_assert_timespec_eq(st_file_pre.st_mtim, st_file_post.st_mtim);
	ck_assert_timespec_eq(st_file_pre.st_ctim, st_file_post.st_ctim);

	/* source parent: nlink down, mtime/ctime advanced */
	ck_assert_int_eq(reffs_fs_getattr("/", &st_src_post), 0);
	ck_assert_uint_eq(st_src_pre.st_nlink, st_src_post.st_nlink + 1);
	ck_assert_timespec_eq(st_src_pre.st_atim, st_src_post.st_atim);
	ck_assert_timespec_lt(st_src_pre.st_mtim, st_src_post.st_mtim);
	ck_assert_timespec_lt(st_src_pre.st_ctim, st_src_post.st_ctim);

	/* dest parent: nlink up, mtime/ctime advanced */
	ck_assert_int_eq(reffs_fs_getattr("/d", &st_dst_post), 0);
	ck_assert_uint_eq(st_dst_pre.st_nlink, st_dst_post.st_nlink - 1);
	ck_assert_timespec_eq(st_dst_pre.st_atim, st_dst_post.st_atim);
	ck_assert_timespec_lt(st_dst_pre.st_mtim, st_dst_post.st_mtim);
	ck_assert_timespec_lt(st_dst_pre.st_ctim, st_dst_post.st_ctim);

	ck_assert_int_eq(reffs_fs_unlink("/d/f"), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/d"), 0);
}
END_TEST

START_TEST(test_rename_file_out_of_subdir)
{
	struct stat st_file_pre, st_file_post;
	struct stat st_src_pre, st_src_post;
	struct stat st_dst_pre, st_dst_post;

	ck_assert_int_eq(reffs_fs_mkdir("/d", 0755), 0);
	ck_assert_int_eq(reffs_fs_create("/d/f", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_write("/d/f", "hello", 5, 0), 5);

	ck_assert_int_eq(reffs_fs_getattr("/d/f", &st_file_pre), 0);
	ck_assert_int_eq(reffs_fs_getattr("/d", &st_src_pre), 0);
	ck_assert_int_eq(reffs_fs_getattr("/", &st_dst_pre), 0);

	usleep(1000);
	ck_assert_int_eq(reffs_fs_rename("/d/f", "/f"), 0);

	ck_assert_int_eq(reffs_fs_getattr("/d/f", &st_file_post), -ENOENT);

	ck_assert_int_eq(reffs_fs_getattr("/f", &st_file_post), 0);
	assert_identity_preserved(&st_file_pre, &st_file_post);
	ck_assert_timespec_eq(st_file_pre.st_mtim, st_file_post.st_mtim);
	ck_assert_timespec_eq(st_file_pre.st_ctim, st_file_post.st_ctim);

	ck_assert_int_eq(reffs_fs_getattr("/d", &st_src_post), 0);
	ck_assert_uint_eq(st_src_pre.st_nlink, st_src_post.st_nlink + 1);

	ck_assert_int_eq(reffs_fs_getattr("/", &st_dst_post), 0);
	ck_assert_uint_eq(st_dst_pre.st_nlink, st_dst_post.st_nlink - 1);

	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/d"), 0);
}
END_TEST

START_TEST(test_rename_root_src_efault)
{
	ck_assert_int_eq(reffs_fs_rename("/", "/d"), -EFAULT);
}
END_TEST

START_TEST(test_rename_root_dst_efault)
{
	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_rename("/f", "/"), -EFAULT);
	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
}
END_TEST

/*
 * Rename within the same directory: the file's name changes but its
 * parent does not change and nlink is unaffected.
 */
START_TEST(test_rename_within_same_dir)
{
	struct stat st_pre, st_post, st_parent_before, st_parent_after;

	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_write("/f", "data", 4, 0), 4);
	ck_assert_int_eq(reffs_fs_getattr("/f", &st_pre), 0);
	ck_assert_int_eq(reffs_fs_getattr("/", &st_parent_before), 0);

	ck_assert_int_eq(reffs_fs_rename("/f", "/g"), 0);

	ck_assert_int_eq(reffs_fs_getattr("/f", &st_post), -ENOENT);
	ck_assert_int_eq(reffs_fs_getattr("/g", &st_post), 0);
	assert_identity_preserved(&st_pre, &st_post);

	/* parent nlink unchanged (same directory, file counts don't change) */
	ck_assert_int_eq(reffs_fs_getattr("/", &st_parent_after), 0);
	ck_assert_uint_eq(st_parent_before.st_nlink, st_parent_after.st_nlink);

	ck_assert_int_eq(reffs_fs_unlink("/g"), 0);
}
END_TEST

/*
 * rename() onto an existing regular file must atomically replace it.
 * After the rename the destination file must have the source's identity
 * and the old destination must be gone.
 */
START_TEST(test_rename_replaces_existing_file)
{
	struct stat st_src_pre, st_post;

	ck_assert_int_eq(reffs_fs_create("/src", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_write("/src", "source", 6, 0), 6);
	ck_assert_int_eq(reffs_fs_getattr("/src", &st_src_pre), 0);

	ck_assert_int_eq(reffs_fs_create("/dst", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_write("/dst", "old", 3, 0), 3);

	ck_assert_int_eq(reffs_fs_rename("/src", "/dst"), 0);

	/* /src is gone */
	ck_assert_int_eq(reffs_fs_getattr("/src", &st_post), -ENOENT);

	/* /dst now has the source file's identity */
	ck_assert_int_eq(reffs_fs_getattr("/dst", &st_post), 0);
	ck_assert_uint_eq(st_src_pre.st_ino, st_post.st_ino);
	ck_assert_int_eq(st_post.st_size, 6);

	ck_assert_int_eq(reffs_fs_unlink("/dst"), 0);
}
END_TEST

/* ------------------------------------------------------------------ */

Suite *fs_rename_suite(void)
{
	Suite *s = suite_create("fs: rename");
	TCase *tc = tcase_create("Core");
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_rename_file_into_subdir);
	tcase_add_test(tc, test_rename_file_out_of_subdir);
	tcase_add_test(tc, test_rename_root_src_efault);
	tcase_add_test(tc, test_rename_root_dst_efault);
	tcase_add_test(tc, test_rename_within_same_dir);
	tcase_add_test(tc, test_rename_replaces_existing_file);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	fs_test_global_init();
	SRunner *sr = srunner_create(fs_rename_suite());
	srunner_set_fork_status(sr, CK_NOFORK);
	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	fs_test_global_fini();
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
