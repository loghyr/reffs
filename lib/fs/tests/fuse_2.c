/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/*
 * Test 2: mkdir nlink accounting.
 *
 * POSIX requires that creating a subdirectory increments the parent's
 * st_nlink by 1 (the new directory's ".." hard-link back to the parent).
 * The new directory itself must start with st_nlink == 2.
 *
 * Each test creates what it needs and removes it before returning so that
 * ASAN sees a clean shutdown and the namespace is pristine for the next test.
 *
 * Scenarios:
 *   - new dir at root: root nlink goes from 2 to 3; new dir has nlink 2
 *   - multiple siblings: each mkdir increments parent nlink by 1
 *   - nested mkdir: grandparent nlink is unaffected; direct parent nlink += 1
 *   - rmdir decrements parent nlink
 *   - rmdir on root returns -EBUSY
 *   - rmdir on non-empty dir returns -ENOTEMPTY
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

START_TEST(test_mkdir_new_dir_nlink)
{
	struct stat st;
	ck_assert_int_eq(reffs_fuse_mkdir("/foo", 0755), 0);
	ck_assert_int_eq(reffs_fuse_getattr("/foo", &st), 0);
	ck_assert_uint_eq(st.st_nlink, 2);

	ck_assert_int_eq(reffs_fuse_rmdir("/foo"), 0);
}
END_TEST

START_TEST(test_mkdir_increments_parent_nlink)
{
	struct stat st_before, st_after;
	ck_assert_int_eq(reffs_fuse_getattr("/", &st_before), 0);

	ck_assert_int_eq(reffs_fuse_mkdir("/foo", 0755), 0);
	ck_assert_int_eq(reffs_fuse_getattr("/", &st_after), 0);
	ck_assert_uint_eq(st_after.st_nlink, st_before.st_nlink + 1);

	ck_assert_int_eq(reffs_fuse_rmdir("/foo"), 0);
}
END_TEST

START_TEST(test_mkdir_multiple_siblings)
{
	struct stat st;

	ck_assert_int_eq(reffs_fuse_mkdir("/foo", 0755), 0);
	ck_assert_int_eq(reffs_fuse_mkdir("/foo/bar", 0640), 0);
	ck_assert_int_eq(reffs_fuse_mkdir("/foo/garbo", 0640), 0);
	ck_assert_int_eq(reffs_fuse_mkdir("/foo/nurse", 0640), 0);

	ck_assert_int_eq(reffs_fuse_getattr("/foo", &st), 0);
	ck_assert_uint_eq(st.st_nlink, 5); /* 2 + 3 children */

	ck_assert_int_eq(reffs_fuse_rmdir("/foo/bar"), 0);
	ck_assert_int_eq(reffs_fuse_rmdir("/foo/garbo"), 0);
	ck_assert_int_eq(reffs_fuse_rmdir("/foo/nurse"), 0);
	ck_assert_int_eq(reffs_fuse_rmdir("/foo"), 0);
}
END_TEST

START_TEST(test_mkdir_nested_only_direct_parent)
{
	struct stat st_root_before, st_root_after, st_foo;

	ck_assert_int_eq(reffs_fuse_mkdir("/foo", 0755), 0);
	ck_assert_int_eq(reffs_fuse_getattr("/", &st_root_before), 0);

	ck_assert_int_eq(reffs_fuse_mkdir("/foo/bar", 0640), 0);

	/* root nlink must not change when grandchild is created */
	ck_assert_int_eq(reffs_fuse_getattr("/", &st_root_after), 0);
	ck_assert_uint_eq(st_root_before.st_nlink, st_root_after.st_nlink);

	/* direct parent /foo gains one */
	ck_assert_int_eq(reffs_fuse_getattr("/foo", &st_foo), 0);
	ck_assert_uint_eq(st_foo.st_nlink, 3);

	ck_assert_int_eq(reffs_fuse_rmdir("/foo/bar"), 0);
	ck_assert_int_eq(reffs_fuse_rmdir("/foo"), 0);
}
END_TEST

START_TEST(test_rmdir_decrements_parent_nlink)
{
	struct stat st_before, st_after;

	ck_assert_int_eq(reffs_fuse_mkdir("/foo", 0755), 0);
	ck_assert_int_eq(reffs_fuse_getattr("/", &st_before), 0);

	ck_assert_int_eq(reffs_fuse_rmdir("/foo"), 0);
	ck_assert_int_eq(reffs_fuse_getattr("/", &st_after), 0);
	ck_assert_uint_eq(st_after.st_nlink, st_before.st_nlink - 1);
}
END_TEST

START_TEST(test_rmdir_root_ebusy)
{
	ck_assert_int_eq(reffs_fuse_rmdir("/"), -EBUSY);
}
END_TEST

START_TEST(test_rmdir_nonempty_enotempty)
{
	ck_assert_int_eq(reffs_fuse_mkdir("/foo", 0755), 0);
	ck_assert_int_eq(reffs_fuse_mkdir("/foo/bar", 0755), 0);

	ck_assert_int_eq(reffs_fuse_rmdir("/foo"), -ENOTEMPTY);

	ck_assert_int_eq(reffs_fuse_rmdir("/foo/bar"), 0);
	ck_assert_int_eq(reffs_fuse_rmdir("/foo"), 0);
}
END_TEST

START_TEST(test_root_returns_to_nlink2_after_cleanup)
{
	struct stat st;

	ck_assert_int_eq(reffs_fuse_mkdir("/foo", 0755), 0);
	ck_assert_int_eq(reffs_fuse_mkdir("/bar", 0755), 0);
	ck_assert_int_eq(reffs_fuse_rmdir("/foo"), 0);
	ck_assert_int_eq(reffs_fuse_rmdir("/bar"), 0);

	ck_assert_int_eq(reffs_fuse_getattr("/", &st), 0);
	ck_assert_uint_eq(st.st_nlink, 2);
}
END_TEST

Suite *fuse_suite(void)
{
	Suite *s = suite_create("fuse 2: mkdir/rmdir nlink accounting");
	TCase *tc = tcase_create("Core");
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_mkdir_new_dir_nlink);
	tcase_add_test(tc, test_mkdir_increments_parent_nlink);
	tcase_add_test(tc, test_mkdir_multiple_siblings);
	tcase_add_test(tc, test_mkdir_nested_only_direct_parent);
	tcase_add_test(tc, test_rmdir_decrements_parent_nlink);
	tcase_add_test(tc, test_rmdir_root_ebusy);
	tcase_add_test(tc, test_rmdir_nonempty_enotempty);
	tcase_add_test(tc, test_root_returns_to_nlink2_after_cleanup);
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
