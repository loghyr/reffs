/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/*
 * Test 6: rename regular file.
 *
 * rename(src, dst) on a regular file must:
 *   - make the file visible at dst and invisible at src
 *   - preserve the file's ino, uid, gid, size, mtime, ctime, and atime
 *     (the file itself is not modified)
 *   - decrement src directory's st_nlink and update its mtime/ctime
 *   - increment dst directory's st_nlink and update its mtime/ctime
 *   - leave both directories' atimes unchanged
 *
 * Tested in both directions: root -> subdir and subdir -> root.
 */

#include "fuse_harness.h"

uid_t fuse_test_uid;
gid_t fuse_test_gid;

static void setup(void)
{
	fuse_test_setup();
}
static void teardown(void)
{
	fuse_test_teardown();
}

START_TEST(test_rename_file_to_subdir)
{
	struct stat st_file_before, st_file_after;
	struct stat st_src_before, st_src_after;
	struct stat st_dst_before, st_dst_after;

	ck_assert_int_eq(reffs_fuse_mkdir("/d", 0755), 0);
	ck_assert_int_eq(reffs_fuse_create("/f", S_IFREG | 0644, NULL), 0);
	ck_assert_int_eq(reffs_fuse_write("/f", "hello", 5, 0, NULL), 5);
	ck_assert_int_eq(reffs_fuse_getattr("/f", &st_file_before), 0);
	ck_assert_int_eq(reffs_fuse_getattr("/", &st_src_before), 0);
	ck_assert_int_eq(reffs_fuse_getattr("/d", &st_dst_before), 0);

	sleep_past(&st_file_before.st_mtim);
	ck_assert_int_eq(reffs_fuse_rename("/f", "/d/f"), 0);

	/* old path gone */
	ck_assert_int_eq(reffs_fuse_getattr("/f", &st_file_after), -ENOENT);

	/* new path: file identity preserved */
	ck_assert_int_eq(reffs_fuse_getattr("/d/f", &st_file_after), 0);
	ck_assert_uint_eq(st_file_before.st_ino, st_file_after.st_ino);
	ck_assert_uint_eq(st_file_before.st_uid, st_file_after.st_uid);
	ck_assert_uint_eq(st_file_before.st_gid, st_file_after.st_gid);
	ck_assert_int_eq(st_file_after.st_size, 5);
	ck_assert_timespec_eq(st_file_before.st_atim, st_file_after.st_atim);
	ck_assert_timespec_eq(st_file_before.st_mtim, st_file_after.st_mtim);
	ck_assert_timespec_eq(st_file_before.st_ctim, st_file_after.st_ctim);

	/* src directory: nlink unchanged, mtime/ctime advanced */
	ck_assert_int_eq(reffs_fuse_getattr("/", &st_src_after), 0);
	ck_assert_uint_eq(st_src_before.st_nlink, st_src_after.st_nlink);
	ck_assert_timespec_eq(st_src_before.st_atim, st_src_after.st_atim);
	ck_assert_timespec_lt(st_src_before.st_mtim, st_src_after.st_mtim);
	ck_assert_timespec_lt(st_src_before.st_ctim, st_src_after.st_ctim);

	/* dst directory: nlink unchanged, mtime/ctime advanced */
	ck_assert_int_eq(reffs_fuse_getattr("/d", &st_dst_after), 0);
	ck_assert_uint_eq(st_dst_before.st_nlink, st_dst_after.st_nlink);
	ck_assert_timespec_eq(st_dst_before.st_atim, st_dst_after.st_atim);
	ck_assert_timespec_lt(st_dst_before.st_mtim, st_dst_after.st_mtim);
	ck_assert_timespec_lt(st_dst_before.st_ctim, st_dst_after.st_ctim);

	ck_assert_int_eq(reffs_fuse_unlink("/d/f"), 0);
	ck_assert_int_eq(reffs_fuse_rmdir("/d"), 0);
}
END_TEST

START_TEST(test_rename_file_back_to_root)
{
	struct stat st_file_before, st_file_after;
	struct stat st_src_before, st_src_after;
	struct stat st_dst_before, st_dst_after;

	ck_assert_int_eq(reffs_fuse_mkdir("/d", 0755), 0);
	ck_assert_int_eq(reffs_fuse_create("/d/f", S_IFREG | 0644, NULL), 0);
	ck_assert_int_eq(reffs_fuse_write("/d/f", "hello", 5, 0, NULL), 5);
	ck_assert_int_eq(reffs_fuse_getattr("/d/f", &st_file_before), 0);
	ck_assert_int_eq(reffs_fuse_getattr("/d", &st_src_before), 0);
	ck_assert_int_eq(reffs_fuse_getattr("/", &st_dst_before), 0);

	sleep_past(&st_file_before.st_mtim);
	ck_assert_int_eq(reffs_fuse_rename("/d/f", "/f"), 0);

	ck_assert_int_eq(reffs_fuse_getattr("/d/f", &st_file_after), -ENOENT);

	ck_assert_int_eq(reffs_fuse_getattr("/f", &st_file_after), 0);
	ck_assert_uint_eq(st_file_before.st_ino, st_file_after.st_ino);
	ck_assert_int_eq(st_file_after.st_size, 5);
	ck_assert_timespec_eq(st_file_before.st_mtim, st_file_after.st_mtim);
	ck_assert_timespec_eq(st_file_before.st_ctim, st_file_after.st_ctim);

	ck_assert_int_eq(reffs_fuse_getattr("/d", &st_src_after), 0);
	ck_assert_uint_eq(st_src_before.st_nlink, st_src_after.st_nlink);

	ck_assert_int_eq(reffs_fuse_getattr("/", &st_dst_after), 0);
	ck_assert_uint_eq(st_dst_before.st_nlink, st_dst_after.st_nlink);

	ck_assert_int_eq(reffs_fuse_unlink("/f"), 0);
	ck_assert_int_eq(reffs_fuse_rmdir("/d"), 0);
}
END_TEST

Suite *fuse_suite(void)
{
	Suite *s = suite_create("fuse 6: rename regular file");
	TCase *tc = tcase_create("Core");
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_rename_file_to_subdir);
	tcase_add_test(tc, test_rename_file_back_to_root);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int number_failed;
	fuse_test_global_init();
	SRunner *sr = srunner_create(fuse_suite());
	srunner_set_fork_status(sr, CK_NOFORK);
	srunner_run_all(sr, CK_NORMAL);
	number_failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	fuse_test_global_fini();
	return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
