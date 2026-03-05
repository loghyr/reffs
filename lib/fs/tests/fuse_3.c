/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/*
 * Test 3: mkdir timestamp and attribute stability.
 *
 * When a child directory is created under a parent, the parent's mtime and
 * ctime must advance but atime and size must be unchanged.  The child's
 * immutable attributes (ino, uid, gid, size) must be stable across a second
 * getattr call.
 *
 * usleep(1000) is used to guarantee a nanosecond-resolution clock advance
 * between the pre and post measurements, mirroring the strategy in
 * fuse_test.c.
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

START_TEST(test_mkdir_parent_timestamps)
{
	struct stat st_pre, st_post;

	ck_assert_int_eq(reffs_fuse_mkdir("/foo", 0755), 0);
	ck_assert_int_eq(reffs_fuse_getattr("/foo", &st_pre), 0);

	sleep_past(&st_pre.st_mtim);

	ck_assert_int_eq(reffs_fuse_mkdir("/foo/bar", 0640), 0);
	ck_assert_int_eq(reffs_fuse_getattr("/foo", &st_post), 0);

	/* stable */
	ck_assert_uint_eq(st_pre.st_ino, st_post.st_ino);
	ck_assert_uint_eq(st_pre.st_uid, st_post.st_uid);
	ck_assert_uint_eq(st_pre.st_gid, st_post.st_gid);
	ck_assert_uint_eq(st_pre.st_size, st_post.st_size);

	/* nlink incremented */
	ck_assert_uint_eq(st_pre.st_nlink, st_post.st_nlink - 1);

	/* atime unchanged */
	ck_assert_timespec_eq(st_pre.st_atim, st_post.st_atim);

	/* mtime and ctime advanced */
	ck_assert_timespec_lt(st_pre.st_mtim, st_post.st_mtim);
	ck_assert_timespec_lt(st_pre.st_ctim, st_post.st_ctim);

	ck_assert_int_eq(reffs_fuse_rmdir("/foo/bar"), 0);
	ck_assert_int_eq(reffs_fuse_rmdir("/foo"), 0);
}
END_TEST

START_TEST(test_mkdir_child_uid_gid)
{
	struct stat st;

	ck_assert_int_eq(reffs_fuse_mkdir("/foo", 0755), 0);
	ck_assert_int_eq(reffs_fuse_getattr("/foo", &st), 0);
	ck_assert_uint_eq(st.st_uid, fuse_test_uid);
	ck_assert_uint_eq(st.st_gid, fuse_test_gid);

	ck_assert_int_eq(reffs_fuse_rmdir("/foo"), 0);
}
END_TEST

START_TEST(test_mkdir_child_mode)
{
	struct stat st;

	ck_assert_int_eq(reffs_fuse_mkdir("/foo", 0640), 0);
	ck_assert_int_eq(reffs_fuse_getattr("/foo", &st), 0);
	ck_assert_uint_eq(st.st_mode, (mode_t)(S_IFDIR | 0640));

	ck_assert_int_eq(reffs_fuse_rmdir("/foo"), 0);
}
END_TEST

Suite *fuse_suite(void)
{
	Suite *s = suite_create("fuse 3: mkdir timestamps and attributes");
	TCase *tc = tcase_create("Core");
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_mkdir_parent_timestamps);
	tcase_add_test(tc, test_mkdir_child_uid_gid);
	tcase_add_test(tc, test_mkdir_child_mode);
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
