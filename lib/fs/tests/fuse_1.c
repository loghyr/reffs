/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

/*
 * Test 1: Root inode initial state.
 *
 * After reffs_ns_init() the root directory must be visible via getattr and
 * have exactly the attributes set by ns.c:
 *   - st_nlink == 2  (self + ".")
 *   - st_mode  == S_IFDIR | 0755
 *   - getattr on a non-existent path returns -ENOENT
 *
 * This is the canary test.  If the initialization of i_nlink, i_mode, or
 * i_used is ever broken (as happened with the S_IFLNK/i_nlink=1 regression),
 * this test will be the first to fire.
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

START_TEST(test_root_nlink)
{
	struct stat st;
	ck_assert_int_eq(reffs_fuse_getattr("/", &st), 0);
	ck_assert_uint_eq(st.st_nlink, 2);
}
END_TEST

START_TEST(test_root_mode)
{
	struct stat st;
	ck_assert_int_eq(reffs_fuse_getattr("/", &st), 0);
	ck_assert_uint_eq(st.st_mode, (mode_t)(S_IFDIR | 0755));
}
END_TEST

START_TEST(test_root_uid_gid)
{
	struct stat st;
	ck_assert_int_eq(reffs_fuse_getattr("/", &st), 0);
	ck_assert_uint_eq(st.st_uid, fuse_test_uid);
	ck_assert_uint_eq(st.st_gid, fuse_test_gid);
}
END_TEST

START_TEST(test_getattr_enoent)
{
	struct stat st;
	ck_assert_int_eq(reffs_fuse_getattr("/nonexistent", &st), -ENOENT);
}
END_TEST

Suite *fuse_suite(void)
{
	Suite *s = suite_create("fuse 1: root inode initial state");
	TCase *tc = tcase_create("Core");
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_root_nlink);
	tcase_add_test(tc, test_root_mode);
	tcase_add_test(tc, test_root_uid_gid);
	tcase_add_test(tc, test_getattr_enoent);
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
