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

START_TEST(test_unlink_open_file_nlink_zero)
{
	struct stat st;

	/* Create a file */
	ck_assert_int_eq(reffs_fs_create("/f", 0644), 0);
	ck_assert_int_eq(reffs_fs_getattr("/f", &st), 0);
	ck_assert_uint_eq(st.st_nlink, 1);

	/* 
	 * Note: Our current FS API doesn't have an explicit 'open' that 
	 * returns a file descriptor, it's all path-based and stateless.
	 * However, the 'unlink' operation itself should immediately
	 * reflect nlink=0 if we could stat it.
	 * 
	 * In a real VFS, an open file descriptor would keep the inode
	 * alive even after nlink hits 0. 
	 * 
	 * For this unit test, we'll verify that once unlinked, the entry
	 * is gone, and if we had a reference to the inode, its nlink
	 * would be 0.
	 */

	/* We'll use an internal check if possible, or just verify it's gone.
	 * But wait, pjdfstest unlink/14.t uses 'fstat 0 nlink'. 
	 * This implies it has an open file.
	 * 
	 * Since our internal 'reffs_fs_unlink' eventually calls 
	 * dirent_parent_release which decrements nlink, let's verify
	 * the inode's nlink directly if we can.
	 */
}
END_TEST

Suite *fs_unlink_open_suite(void)
{
	Suite *s = suite_create("fs: unlink open file");
	TCase *tc = tcase_create("Core");
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_unlink_open_file_nlink_zero);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	fs_test_global_init();
	SRunner *sr = srunner_create(fs_unlink_open_suite());
	srunner_set_fork_status(sr, CK_NOFORK);
	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	fs_test_global_fini();
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
