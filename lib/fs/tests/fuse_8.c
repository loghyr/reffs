/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/*
 * Test 8: utimensat.
 *
 * Also absent from fuse_test.c.  reffs_fuse_utimensat() sets both atime and
 * mtime to caller-supplied values.
 *
 *   - After utimensat, getattr must return exactly the supplied times.
 *   - ino, uid, gid, size, mode, nlink must be unchanged.
 *   - Works on both regular files and directories.
 *   - Applying utimensat on a non-existent path returns -ENOENT.
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

START_TEST(test_utimensat_file)
{
	struct stat st_pre, st_post;
	struct timespec times[2];

	ck_assert_int_eq(reffs_fuse_create("/f", S_IFREG | 0644, NULL), 0);
	ck_assert_int_eq(reffs_fuse_getattr("/f", &st_pre), 0);

	times[0].tv_sec = 1000000000; /* atime */
	times[0].tv_nsec = 111;
	times[1].tv_sec = 2000000000; /* mtime */
	times[1].tv_nsec = 222;

	ck_assert_int_eq(reffs_fuse_utimensat("/f", times), 0);
	ck_assert_int_eq(reffs_fuse_getattr("/f", &st_post), 0);

	ck_assert_int_eq(st_post.st_atim.tv_sec, 1000000000);
	ck_assert_int_eq(st_post.st_atim.tv_nsec, 111);
	ck_assert_int_eq(st_post.st_mtim.tv_sec, 2000000000);
	ck_assert_int_eq(st_post.st_mtim.tv_nsec, 222);

	/* everything else stable */
	ck_assert_uint_eq(st_pre.st_ino, st_post.st_ino);
	ck_assert_uint_eq(st_pre.st_uid, st_post.st_uid);
	ck_assert_uint_eq(st_pre.st_gid, st_post.st_gid);
	ck_assert_uint_eq(st_pre.st_mode, st_post.st_mode);
	ck_assert_uint_eq(st_pre.st_nlink, st_post.st_nlink);
	ck_assert_uint_eq((uint64_t)st_pre.st_size, (uint64_t)st_post.st_size);

	ck_assert_int_eq(reffs_fuse_unlink("/f"), 0);
}
END_TEST

START_TEST(test_utimensat_dir)
{
	struct stat st_post;
	struct timespec times[2];

	ck_assert_int_eq(reffs_fuse_mkdir("/d", 0755), 0);

	times[0].tv_sec = 500;
	times[0].tv_nsec = 1;
	times[1].tv_sec = 600;
	times[1].tv_nsec = 2;

	ck_assert_int_eq(reffs_fuse_utimensat("/d", times), 0);
	ck_assert_int_eq(reffs_fuse_getattr("/d", &st_post), 0);
	ck_assert_int_eq(st_post.st_atim.tv_sec, 500);
	ck_assert_int_eq(st_post.st_mtim.tv_sec, 600);

	ck_assert_int_eq(reffs_fuse_rmdir("/d"), 0);
}
END_TEST

START_TEST(test_utimensat_enoent)
{
	struct timespec times[2] = { { 1, 0 }, { 2, 0 } };
	ck_assert_int_eq(reffs_fuse_utimensat("/no_such", times), -ENOENT);
}
END_TEST

Suite *fuse_suite(void)
{
	Suite *s = suite_create("fuse 8: utimensat");
	TCase *tc = tcase_create("Core");
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_utimensat_file);
	tcase_add_test(tc, test_utimensat_dir);
	tcase_add_test(tc, test_utimensat_enoent);
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
