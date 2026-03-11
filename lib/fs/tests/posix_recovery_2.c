/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <stdint.h>
#include <check.h>
#include <stdbool.h>
#include <errno.h>
#include "posix_recovery.h"
#include "reffs/fs.h"
#include "reffs/test.h"

START_TEST(test_cookie_persistence)
{
	struct test_context ctx;
	struct inode_disk id_dir = { 0 };
	id_dir.id_mode = S_IFDIR | 0755;
	id_dir.id_nlink = 2;

	struct inode_disk id_file = { 0 };
	id_file.id_mode = S_IFREG | 0644;
	id_file.id_nlink = 1;

	ck_assert_int_eq(test_setup(&ctx), 0);

	/* Write root metadata */
	test_write_meta(&ctx, 1, &id_dir);

	/* Write a .dir file for root with custom cookies */
	int fd;
	ck_assert_int_eq(test_write_dir_header(&ctx, 1, 100, &fd), 0);
	test_write_dir_entry(fd, 10, 2, "file1");
	test_write_dir_entry(fd, 20, 3, "file2");
	close(fd);

	/* Write child metadata */
	test_write_meta(&ctx, 2, &id_file);
	test_write_meta(&ctx, 3, &id_file);

	reffs_fs_recover(ctx.sb);

	struct reffs_dirent *root_de = ctx.sb->sb_dirent;
	ck_assert_uint_eq(root_de->rd_cookie_next, 100);

	struct reffs_dirent *de1 =
		dirent_find(root_de, reffs_text_case_sensitive, "file1");
	ck_assert(de1);
	ck_assert_uint_eq(de1->rd_cookie, 10);
	dirent_put(de1);

	struct reffs_dirent *de2 =
		dirent_find(root_de, reffs_text_case_sensitive, "file2");
	ck_assert(de2);
	ck_assert_uint_eq(de2->rd_cookie, 20);
	dirent_put(de2);

	test_teardown(&ctx);
}
END_TEST

Suite *recovery_suite(void)
{
	Suite *s = suite_create("POSIX Recovery 2: Cookie Persistence");
	TCase *tc = tcase_create("Core");
	tcase_add_test(tc, test_cookie_persistence);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int number_failed;
	Suite *s;
	SRunner *sr;

	rcu_register_thread();
	s = recovery_suite();
	sr = srunner_create(s);
	srunner_set_fork_status(sr, CK_NOFORK);
	srunner_run_all(sr, CK_NORMAL);
	number_failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	rcu_unregister_thread();

	return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
