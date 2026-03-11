/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/*
 * Test 7: chmod and chown.
 *
 * These operations exist in fs.c but are untested in fuse_test.c.
 *
 * chmod must:
 *   - update i_mode (only the permission bits; the file-type bits are
 *     preserved)
 *   - advance ctime and mtime
 *   - not change atime, ino, uid, gid, size, nlink
 *
 * chown must:
 *   - update uid and gid
 *   - advance ctime and mtime
 *   - not change atime, ino, mode, size, nlink
 *
 * Both are tested on regular files and on directories.
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

START_TEST(test_chmod_file)
{
	struct stat st_pre, st_post;

	ck_assert_int_eq(reffs_fuse_create("/f", S_IFREG | 0644, NULL), 0);
	ck_assert_int_eq(reffs_fuse_getattr("/f", &st_pre), 0);

	sleep_past(&st_pre.st_mtim);
	ck_assert_int_eq(reffs_fuse_chmod("/f", 0600), 0);
	ck_assert_int_eq(reffs_fuse_getattr("/f", &st_post), 0);

	ck_assert_uint_eq(st_post.st_mode, (mode_t)(S_IFREG | 0600));
	ck_assert_uint_eq(st_pre.st_ino, st_post.st_ino);
	ck_assert_uint_eq(st_pre.st_uid, st_post.st_uid);
	ck_assert_uint_eq(st_pre.st_gid, st_post.st_gid);
	ck_assert_uint_eq(st_pre.st_nlink, st_post.st_nlink);
	ck_assert_timespec_eq(st_pre.st_atim, st_post.st_atim);
	ck_assert_timespec_lt(st_pre.st_mtim, st_post.st_mtim);
	ck_assert_timespec_lt(st_pre.st_ctim, st_post.st_ctim);

	ck_assert_int_eq(reffs_fuse_unlink("/f"), 0);
}
END_TEST

START_TEST(test_chmod_dir)
{
	struct stat st_post;

	ck_assert_int_eq(reffs_fuse_mkdir("/d", 0755), 0);
	ck_assert_int_eq(reffs_fuse_chmod("/d", 0700), 0);
	ck_assert_int_eq(reffs_fuse_getattr("/d", &st_post), 0);

	ck_assert_uint_eq(st_post.st_mode, (mode_t)(S_IFDIR | 0700));

	ck_assert_int_eq(reffs_fuse_rmdir("/d"), 0);
}
END_TEST

START_TEST(test_chown_file)
{
	struct stat st_pre, st_post;

	ck_assert_int_eq(reffs_fuse_create("/f", S_IFREG | 0644, NULL), 0);
	ck_assert_int_eq(reffs_fuse_getattr("/f", &st_pre), 0);

	sleep_past(&st_pre.st_mtim);
	ck_assert_int_eq(reffs_fuse_chown("/f", 1234, 5678), 0);
	ck_assert_int_eq(reffs_fuse_getattr("/f", &st_post), 0);

	ck_assert_uint_eq(st_post.st_uid, 1234);
	ck_assert_uint_eq(st_post.st_gid, 5678);
	ck_assert_uint_eq(st_pre.st_ino, st_post.st_ino);
	ck_assert_uint_eq(st_pre.st_mode, st_post.st_mode);
	ck_assert_timespec_eq(st_pre.st_atim, st_post.st_atim);
	ck_assert_timespec_lt(st_pre.st_mtim, st_post.st_mtim);
	ck_assert_timespec_lt(st_pre.st_ctim, st_post.st_ctim);

	ck_assert_int_eq(reffs_fuse_unlink("/f"), 0);
}
END_TEST

START_TEST(test_chown_nonexistent_enoent)
{
	ck_assert_int_eq(reffs_fuse_chown("/no_such", 1, 1), -ENOENT);
}
END_TEST

Suite *fuse_suite(void)
{
	Suite *s = suite_create("fuse 7: chmod and chown");
	TCase *tc = tcase_create("Core");
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_chmod_file);
	tcase_add_test(tc, test_chmod_dir);
	tcase_add_test(tc, test_chown_file);
	tcase_add_test(tc, test_chown_nonexistent_enoent);
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
