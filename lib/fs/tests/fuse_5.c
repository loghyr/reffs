/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/*
 * Test 5: unlink nlink and parent timestamp effects.
 *
 * Unlinking a regular file must:
 *   - decrement the parent directory's st_nlink
 *   - update the parent's mtime and ctime
 *   - leave the parent's atime unchanged
 *   - leave the parent's ino, uid, gid, size unchanged
 *
 * This mirrors the inverse of the mkdir nlink test and catches regressions
 * where unlink forgets to walk up to the parent.
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

START_TEST(test_unlink_does_not_decrement_parent_nlink)
{
	struct stat st_before, st_after;

	ck_assert_int_eq(reffs_fuse_mkdir("/d", 0755), 0);
	ck_assert_int_eq(reffs_fuse_create("/d/f", S_IFREG | 0644, NULL), 0);
	ck_assert_int_eq(reffs_fuse_getattr("/d", &st_before), 0);

	ck_assert_int_eq(reffs_fuse_unlink("/d/f"), 0);
	ck_assert_int_eq(reffs_fuse_getattr("/d", &st_after), 0);

	ck_assert_uint_eq(st_before.st_nlink, st_after.st_nlink);

	ck_assert_int_eq(reffs_fuse_rmdir("/d"), 0);
}
END_TEST

START_TEST(test_unlink_parent_timestamps)
{
	struct stat st_pre, st_post;

	ck_assert_int_eq(reffs_fuse_mkdir("/d", 0755), 0);
	ck_assert_int_eq(reffs_fuse_create("/d/f", S_IFREG | 0644, NULL), 0);

	sleep_past(&st_pre.st_mtim);
	ck_assert_int_eq(reffs_fuse_getattr("/d", &st_pre), 0);

	/* Need another clock tick before unlink so times actually advance */
	sleep_past(&st_pre.st_mtim);
	ck_assert_int_eq(reffs_fuse_unlink("/d/f"), 0);
	ck_assert_int_eq(reffs_fuse_getattr("/d", &st_post), 0);

	/* stable */
	ck_assert_uint_eq(st_pre.st_ino, st_post.st_ino);
	ck_assert_uint_eq(st_pre.st_uid, st_post.st_uid);
	ck_assert_uint_eq(st_pre.st_gid, st_post.st_gid);
	ck_assert_uint_eq(st_pre.st_size, st_post.st_size);

	/* atime unchanged */
	ck_assert_timespec_eq(st_pre.st_atim, st_post.st_atim);

	/* mtime and ctime advanced */
	ck_assert_timespec_lt(st_pre.st_mtim, st_post.st_mtim);
	ck_assert_timespec_lt(st_pre.st_ctim, st_post.st_ctim);

	ck_assert_int_eq(reffs_fuse_rmdir("/d"), 0);
}
END_TEST

Suite *fuse_suite(void)
{
	Suite *s = suite_create("fuse 5: unlink nlink and parent timestamps");
	TCase *tc = tcase_create("Core");
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_unlink_does_not_decrement_parent_nlink);
	tcase_add_test(tc, test_unlink_parent_timestamps);
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
