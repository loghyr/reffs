/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <stdint.h>
#include <check.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include "posix_recovery.h"
#include "reffs/fs.h"
#include "reffs/inode.h"
#include "reffs/test.h"

START_TEST(test_attr_flags_persistence)
{
	struct test_context ctx;
	ck_assert_int_eq(test_setup(&ctx), 0);

	struct inode *inode = ctx.sb->sb_dirent->rd_inode;
	ck_assert(inode != NULL);

	inode->i_mode = S_IFREG | 0644;
	inode->i_uid = 1000;
	inode->i_gid = 1000;
	inode->i_nlink = 1;
	inode->i_size = 0;
	inode->i_attr_flags = INODE_IS_OFFLINE | INODE_IS_HIDDEN;

	/* Sync to disk */
	inode_sync_to_disk(inode);

	/* 
	 * Now simulate recovery. We'll create a new superblock and recover it.
	 * This tests both the sync (save) and load paths.
	 */
	struct super_block *sb2 = super_block_alloc(SUPER_BLOCK_ROOT_ID, "/",
						    REFFS_STORAGE_POSIX,
						    ctx.backend_path);
	ck_assert(sb2 != NULL);

	super_block_dirent_create(sb2, NULL, reffs_life_action_birth);
	reffs_fs_recover(sb2);

	struct inode *recovered_inode = sb2->sb_dirent->rd_inode;
	ck_assert(recovered_inode != NULL);
	ck_assert_uint_eq(recovered_inode->i_attr_flags,
			  INODE_IS_OFFLINE | INODE_IS_HIDDEN);
	ck_assert_uint_eq(recovered_inode->i_uid, 1000);
	ck_assert_uint_eq(recovered_inode->i_mode, S_IFREG | 0644);

	super_block_dirent_release(sb2, reffs_life_action_death);
	super_block_put(sb2);

	test_teardown(&ctx);
}
END_TEST

Suite *recovery_suite(void)
{
	Suite *s = suite_create("POSIX Recovery 14: Attribute Flags");
	TCase *tc = tcase_create("Core");
	tcase_add_test(tc, test_attr_flags_persistence);
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
