/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

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

START_TEST(test_setattr_utime_now_permission)
{
	struct stat st_pre, st_post;
	uid_t nobody = 65534;
	gid_t nobody_g = 65534;

	/* Create a file owned by root (default), but writable by everyone */
	ck_assert_int_eq(reffs_fs_create("/f", 0666), 0);
	ck_assert_int_eq(reffs_fs_getattr("/f", &st_pre), 0);

	usleep(100000);

	/* Update timestamps to NOW as 'nobody' (who is not the owner) */
	struct reffs_context ctx = { .uid = nobody, .gid = nobody_g };
	reffs_set_context(&ctx);

	struct timespec times[2];
	times[0].tv_nsec = UTIME_NOW;
	times[1].tv_nsec = UTIME_NOW;

	/* POSIX: UTIME_NOW is allowed if user has write permission */
	ck_assert_int_eq(reffs_fs_utimensat("/f", times), 0);
	ck_assert_int_eq(reffs_fs_getattr("/f", &st_post), 0);

	ck_assert_timespec_lt(st_pre.st_atim, st_post.st_atim);
	ck_assert_timespec_lt(st_pre.st_mtim, st_post.st_mtim);

	/* Clean up */
	reffs_set_context(NULL);
	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
}
END_TEST

START_TEST(test_setattr_utime_explicit_permission_denied)
{
	struct stat st_pre;
	uid_t nobody = 65534;
	gid_t nobody_g = 65534;

	ck_assert_int_eq(reffs_fs_create("/f", 0666), 0);
	ck_assert_int_eq(reffs_fs_getattr("/f", &st_pre), 0);

	struct reffs_context ctx = { .uid = nobody, .gid = nobody_g };
	reffs_set_context(&ctx);

	struct timespec times[2];
	times[0].tv_sec = st_pre.st_atim.tv_sec + 100;
	times[0].tv_nsec = 0;
	times[1].tv_sec = st_pre.st_mtim.tv_sec + 100;
	times[1].tv_nsec = 0;

	/* POSIX: Explicit timestamp updates REQUIRE ownership */
	ck_assert_int_eq(reffs_fs_utimensat("/f", times), -EPERM);

	/* Clean up */
	reffs_set_context(NULL);
	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
}
END_TEST

Suite *fs_setattr_perm_suite(void)
{
	Suite *s = suite_create("fs: setattr permissions");
	TCase *tc = tcase_create("Core");
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_setattr_utime_now_permission);
	tcase_add_test(tc, test_setattr_utime_explicit_permission_denied);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	fs_test_global_init();
	SRunner *sr = srunner_create(fs_setattr_perm_suite());
	srunner_set_fork_status(sr, CK_NOFORK);
	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	fs_test_global_fini();
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
