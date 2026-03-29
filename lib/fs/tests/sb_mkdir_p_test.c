/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Unit tests for reffs_fs_mkdir_p (recursive mkdir).
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <sys/stat.h>

#include <check.h>

#include "reffs/fs.h"
#include "fs_test_harness.h"

/*
 * Intent: create a single-level directory.
 */
START_TEST(test_mkdir_p_single)
{
	ck_assert_int_eq(reffs_fs_mkdir_p("/one", 0755), 0);

	struct stat st;

	ck_assert_int_eq(reffs_fs_getattr("/one", &st), 0);
	ck_assert(S_ISDIR(st.st_mode));

	ck_assert_int_eq(reffs_fs_rmdir("/one"), 0);
}
END_TEST

/*
 * Intent: create a multi-level path in one call.
 */
START_TEST(test_mkdir_p_multi)
{
	ck_assert_int_eq(reffs_fs_mkdir_p("/a/b/c", 0755), 0);

	struct stat st;

	ck_assert_int_eq(reffs_fs_getattr("/a", &st), 0);
	ck_assert(S_ISDIR(st.st_mode));
	ck_assert_int_eq(reffs_fs_getattr("/a/b", &st), 0);
	ck_assert_int_eq(reffs_fs_getattr("/a/b/c", &st), 0);

	ck_assert_int_eq(reffs_fs_rmdir("/a/b/c"), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/a/b"), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/a"), 0);
}
END_TEST

/*
 * Intent: calling mkdir_p on an existing path succeeds (idempotent).
 */
START_TEST(test_mkdir_p_exists)
{
	ck_assert_int_eq(reffs_fs_mkdir("/exists", 0755), 0);
	ck_assert_int_eq(reffs_fs_mkdir_p("/exists", 0755), 0);

	ck_assert_int_eq(reffs_fs_rmdir("/exists"), 0);
}
END_TEST

/*
 * Intent: partial path exists, create the rest.
 */
START_TEST(test_mkdir_p_partial)
{
	ck_assert_int_eq(reffs_fs_mkdir("/x", 0755), 0);
	ck_assert_int_eq(reffs_fs_mkdir_p("/x/y/z", 0755), 0);

	struct stat st;

	ck_assert_int_eq(reffs_fs_getattr("/x/y/z", &st), 0);

	ck_assert_int_eq(reffs_fs_rmdir("/x/y/z"), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/x/y"), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/x"), 0);
}
END_TEST

/*
 * Intent: root path is a no-op.
 */
START_TEST(test_mkdir_p_root)
{
	ck_assert_int_eq(reffs_fs_mkdir_p("/", 0755), 0);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

static Suite *mkdir_p_suite(void)
{
	Suite *s = suite_create("mkdir_p");
	TCase *tc;

	tc = tcase_create("core");
	tcase_add_checked_fixture(tc, fs_test_setup, fs_test_teardown);
	tcase_add_test(tc, test_mkdir_p_single);
	tcase_add_test(tc, test_mkdir_p_multi);
	tcase_add_test(tc, test_mkdir_p_exists);
	tcase_add_test(tc, test_mkdir_p_partial);
	tcase_add_test(tc, test_mkdir_p_root);
	suite_add_tcase(s, tc);

	return s;
}

int main(void)
{
	int nfailed;
	Suite *s = mkdir_p_suite();
	SRunner *sr = srunner_create(s);

	srunner_run_all(sr, CK_NORMAL);
	nfailed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return nfailed ? 1 : 0;
}
