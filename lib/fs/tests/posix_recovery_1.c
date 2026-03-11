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

START_TEST(test_inode_gaps)
{
	struct test_context ctx;

	/*
	 * The root inode (ino 1) must be a directory — load_inode_attributes()
	 * overwrites the in-memory mode with whatever is in the .meta file, so
	 * using S_IFREG here would leave the root with the wrong type after
	 * recovery, putting the filesystem in an inconsistent state even though
	 * the sb_next_ino assertion would still pass.
	 */
	struct inode_disk id_root = { 0 };
	id_root.id_mode = S_IFDIR | 0755;
	id_root.id_nlink = 2;

	struct inode_disk id_file = { 0 };
	id_file.id_mode = S_IFREG | 0644;
	id_file.id_nlink = 1;

	ck_assert_int_eq(test_setup(&ctx), 0);

	/* 
	 * Scenario: 
	 * - Root inode is 1.
	 * - We have meta files for ino 2, 5, and 10.
	 * - Even if not linked in any directory, sb_next_ino should become 11.
	 */
	test_write_meta(&ctx, 1, &id_root);
	test_write_meta(&ctx, 2, &id_file);
	test_write_meta(&ctx, 5, &id_file);
	test_write_meta(&ctx, 10, &id_file);

	reffs_fs_recover(ctx.sb);

	ck_assert_uint_eq(ctx.sb->sb_next_ino, 11);

	/* Root inode must still be a directory after recovery */
	ck_assert((ctx.sb->sb_dirent->rd_inode->i_mode & S_IFMT) == S_IFDIR);

	test_teardown(&ctx);
}
END_TEST

Suite *recovery_suite(void)
{
	Suite *s = suite_create("POSIX Recovery 1: Inode Gaps");
	TCase *tc = tcase_create("Core");
	tcase_add_test(tc, test_inode_gaps);
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
