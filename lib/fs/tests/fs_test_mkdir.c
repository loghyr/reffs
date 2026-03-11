/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/*
 * fs_test_mkdir.c — reffs_fs_mkdir() and reffs_fs_rmdir() nlink accounting
 *
 * POSIX: creating a subdirectory increments the parent's st_nlink by 1
 * (the new entry's ".." hard-link back to the parent).  The new directory
 * itself starts at st_nlink == 2.  rmdir reverses all of this.
 *
 * These are the tests most directly targeted at the S_IFLNK/nlink=1
 * regression — any change that touches how mkdir initialises i_nlink or
 * i_mode must pass this file cleanly.
 *
 * Tests:
 *  - New directory nlink is exactly 2
 *  - mkdir increments parent nlink by 1
 *  - Multiple siblings accumulate correctly
 *  - Nested mkdir only affects direct parent, not grandparent
 *  - rmdir decrements parent nlink
 *  - rmdir "/" returns -EBUSY
 *  - rmdir on a non-empty directory returns -ENOTEMPTY
 *  - rmdir on a regular file returns -ENOTDIR
 *  - mkdir on an existing name returns -EEXIST
 *  - After full teardown root returns to nlink 2
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

START_TEST(test_new_dir_nlink_is_2)
{
	struct stat st;

	ck_assert_int_eq(reffs_fs_mkdir("/d", 0755), 0);
	ck_assert_int_eq(reffs_fs_getattr("/d", &st), 0);
	ck_assert_uint_eq(st.st_nlink, 2);

	ck_assert_int_eq(reffs_fs_rmdir("/d"), 0);
}
END_TEST

START_TEST(test_mkdir_increments_parent_nlink)
{
	struct stat st_before, st_after;

	ck_assert_int_eq(reffs_fs_getattr("/", &st_before), 0);
	ck_assert_int_eq(reffs_fs_mkdir("/d", 0755), 0);
	ck_assert_int_eq(reffs_fs_getattr("/", &st_after), 0);

	ck_assert_uint_eq(st_after.st_nlink, st_before.st_nlink + 1);

	ck_assert_int_eq(reffs_fs_rmdir("/d"), 0);
}
END_TEST

START_TEST(test_mkdir_multiple_siblings)
{
	struct stat st;

	ck_assert_int_eq(reffs_fs_mkdir("/p", 0755), 0);
	ck_assert_int_eq(reffs_fs_mkdir("/p/a", 0755), 0);
	ck_assert_int_eq(reffs_fs_mkdir("/p/b", 0755), 0);
	ck_assert_int_eq(reffs_fs_mkdir("/p/c", 0755), 0);

	ck_assert_int_eq(reffs_fs_getattr("/p", &st), 0);
	ck_assert_uint_eq(st.st_nlink, 5); /* 2 + 3 subdirs */

	ck_assert_int_eq(reffs_fs_rmdir("/p/a"), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/p/b"), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/p/c"), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/p"), 0);
}
END_TEST

/*
 * Creating a grandchild must increment the direct parent's nlink,
 * but must leave the grandparent's nlink unchanged.
 */
START_TEST(test_mkdir_only_affects_direct_parent)
{
	struct stat st_root_before, st_root_after, st_child;

	ck_assert_int_eq(reffs_fs_mkdir("/p", 0755), 0);
	ck_assert_int_eq(reffs_fs_getattr("/", &st_root_before), 0);

	ck_assert_int_eq(reffs_fs_mkdir("/p/c", 0755), 0);

	ck_assert_int_eq(reffs_fs_getattr("/", &st_root_after), 0);
	ck_assert_uint_eq(st_root_before.st_nlink, st_root_after.st_nlink);

	ck_assert_int_eq(reffs_fs_getattr("/p", &st_child), 0);
	ck_assert_uint_eq(st_child.st_nlink, 3);

	ck_assert_int_eq(reffs_fs_rmdir("/p/c"), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/p"), 0);
}
END_TEST

START_TEST(test_rmdir_decrements_parent_nlink)
{
	struct stat st_before, st_after;

	ck_assert_int_eq(reffs_fs_mkdir("/d", 0755), 0);
	ck_assert_int_eq(reffs_fs_getattr("/", &st_before), 0);

	ck_assert_int_eq(reffs_fs_rmdir("/d"), 0);
	ck_assert_int_eq(reffs_fs_getattr("/", &st_after), 0);

	ck_assert_uint_eq(st_after.st_nlink, st_before.st_nlink - 1);
}
END_TEST

START_TEST(test_rmdir_root_ebusy)
{
	ck_assert_int_eq(reffs_fs_rmdir("/"), -EBUSY);
}
END_TEST

START_TEST(test_rmdir_nonempty_enotempty)
{
	ck_assert_int_eq(reffs_fs_mkdir("/p", 0755), 0);
	ck_assert_int_eq(reffs_fs_mkdir("/p/c", 0755), 0);

	ck_assert_int_eq(reffs_fs_rmdir("/p"), -ENOTEMPTY);

	ck_assert_int_eq(reffs_fs_rmdir("/p/c"), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/p"), 0);
}
END_TEST

START_TEST(test_rmdir_on_file_enotdir)
{
	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/f"), -ENOTDIR);
	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
}
END_TEST

START_TEST(test_mkdir_eexist)
{
	ck_assert_int_eq(reffs_fs_mkdir("/d", 0755), 0);
	ck_assert_int_eq(reffs_fs_mkdir("/d", 0755), -EEXIST);
	ck_assert_int_eq(reffs_fs_rmdir("/d"), 0);
}
END_TEST

START_TEST(test_root_nlink_returns_to_2)
{
	struct stat st;

	ck_assert_int_eq(reffs_fs_mkdir("/a", 0755), 0);
	ck_assert_int_eq(reffs_fs_mkdir("/b", 0755), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/a"), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/b"), 0);

	ck_assert_int_eq(reffs_fs_getattr("/", &st), 0);
	ck_assert_uint_eq(st.st_nlink, 2);
}
END_TEST

/* ------------------------------------------------------------------ */

Suite *fs_mkdir_suite(void)
{
	Suite *s = suite_create("fs: mkdir/rmdir nlink");
	TCase *tc = tcase_create("Core");
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_new_dir_nlink_is_2);
	tcase_add_test(tc, test_mkdir_increments_parent_nlink);
	tcase_add_test(tc, test_mkdir_multiple_siblings);
	tcase_add_test(tc, test_mkdir_only_affects_direct_parent);
	tcase_add_test(tc, test_rmdir_decrements_parent_nlink);
	tcase_add_test(tc, test_rmdir_root_ebusy);
	tcase_add_test(tc, test_rmdir_nonempty_enotempty);
	tcase_add_test(tc, test_rmdir_on_file_enotdir);
	tcase_add_test(tc, test_mkdir_eexist);
	tcase_add_test(tc, test_root_nlink_returns_to_2);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	fs_test_global_init();
	SRunner *sr = srunner_create(fs_mkdir_suite());
	srunner_set_fork_status(sr, CK_NOFORK);
	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	fs_test_global_fini();
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
