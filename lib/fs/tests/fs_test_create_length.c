/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "fs_test_harness.h"
#include "reffs/dirent.h" // for REFFS_MAX_NAME

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

START_TEST(test_create_name_too_long)
{
	char long_name[REFFS_MAX_NAME + 2];
	memset(long_name, 'a', REFFS_MAX_NAME + 1);
	long_name[REFFS_MAX_NAME + 1] = '\0';

	char path[REFFS_MAX_NAME + 3];
	snprintf(path, sizeof(path), "/%s", long_name);

	ck_assert_int_eq(reffs_fs_create(path, 0644), -ENAMETOOLONG);
	ck_assert_int_eq(reffs_fs_mkdir(path, 0755), -ENAMETOOLONG);
	ck_assert_int_eq(reffs_fs_symlink("target", path), -ENAMETOOLONG);

	/* Target too long */
	char long_target[REFFS_MAX_PATH + 2];
	memset(long_target, 'c', REFFS_MAX_PATH + 1);
	long_target[REFFS_MAX_PATH + 1] = '\0';
	ck_assert_int_eq(reffs_fs_symlink(long_target, "/short"),
			 -ENAMETOOLONG);
}
END_TEST

START_TEST(test_create_name_max_length)
{
	char max_name[REFFS_MAX_NAME + 1];
	memset(max_name, 'b', REFFS_MAX_NAME);
	max_name[REFFS_MAX_NAME] = '\0';

	char path[REFFS_MAX_NAME + 2];
	snprintf(path, sizeof(path), "/%s", max_name);

	ck_assert_int_eq(reffs_fs_create(path, 0644), 0);
	ck_assert_int_eq(reffs_fs_unlink(path), 0);

	ck_assert_int_eq(reffs_fs_mkdir(path, 0755), 0);
	ck_assert_int_eq(reffs_fs_rmdir(path), 0);

	ck_assert_int_eq(reffs_fs_symlink("target", path), 0);
	ck_assert_int_eq(reffs_fs_unlink(path), 0);
}
END_TEST

Suite *fs_create_length_suite(void)
{
	Suite *s = suite_create("fs: create name length");
	TCase *tc = tcase_create("Core");
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_create_name_too_long);
	tcase_add_test(tc, test_create_name_max_length);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	fs_test_global_init();
	SRunner *sr = srunner_create(fs_create_length_suite());
	srunner_set_fork_status(sr, CK_NOFORK);
	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	fs_test_global_fini();
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
