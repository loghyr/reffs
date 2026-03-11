/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/stat.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <rpc/auth.h>
#include <rpc/auth_unix.h>
#include "reffs/fs.h"
#include "reffs/inode.h"
#include "reffs/super_block.h"
#include "reffs/identity.h"
#include "reffs/context.h"
#include "fs_test_harness.h"

uid_t fs_test_uid;
gid_t fs_test_gid;

START_TEST(test_owner_write_no_perm_bit)
{
	struct super_block *sb;
	struct inode *inode;
	struct authunix_parms ap;
	int ret;

	sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	ck_assert_ptr_nonnull(sb);

	inode = inode_alloc(sb, 100);
	ck_assert_ptr_nonnull(inode);

	inode->i_uid = 1000;
	inode->i_gid = 1000;
	inode->i_mode = S_IFREG | 0444; /* Read only for owner too */

	ap.aup_uid = 1000;
	ap.aup_gid = 1000;
	ap.aup_len = 0;
	ap.aup_gids = NULL;

	/*
	 * Without override: Should fail with -EACCES.
	 */
	ret = inode_access_check(inode, &ap, W_OK);
	ck_assert_int_eq(ret, -EACCES);

	/*
	 * With override: Should succeed ONLY if NOT in strict POSIX mode.
	 */
	ret = inode_access_check_flags(inode, &ap, W_OK,
				       REFFS_ACCESS_OWNER_OVERRIDE);
#ifdef HAVE_STRICT_POSIX
	ck_assert_int_eq(ret, -EACCES);
#else
	ck_assert_int_eq(ret, 0);
#endif

	inode_active_put(inode);
	super_block_put(sb);
}
END_TEST

START_TEST(test_owner_read_no_perm_bit)
{
	struct super_block *sb;
	struct inode *inode;
	struct authunix_parms ap;
	int ret;

	sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	ck_assert_ptr_nonnull(sb);

	inode = inode_alloc(sb, 101);
	ck_assert_ptr_nonnull(inode);

	inode->i_uid = 1000;
	inode->i_gid = 1000;
	inode->i_mode = S_IFREG | 0222; /* Write only for owner too */

	ap.aup_uid = 1000;
	ap.aup_gid = 1000;
	ap.aup_len = 0;
	ap.aup_gids = NULL;

	/*
	 * Without override: Should fail.
	 */
	ret = inode_access_check(inode, &ap, R_OK);
	ck_assert_int_eq(ret, -EACCES);

	/*
	 * With override: Should succeed ONLY if NOT in strict POSIX mode.
	 */
	ret = inode_access_check_flags(inode, &ap, R_OK,
				       REFFS_ACCESS_OWNER_OVERRIDE);
#ifdef HAVE_STRICT_POSIX
	ck_assert_int_eq(ret, -EACCES);
#else
	ck_assert_int_eq(ret, 0);
#endif

	inode_active_put(inode);
	super_block_put(sb);
}
END_TEST

START_TEST(test_owner_exec_no_perm_bit)
{
	struct super_block *sb;
	struct inode *inode;
	struct authunix_parms ap;
	int ret;

	sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	ck_assert_ptr_nonnull(sb);

	inode = inode_alloc(sb, 102);
	ck_assert_ptr_nonnull(inode);

	inode->i_uid = 1000;
	inode->i_gid = 1000;
	inode->i_mode = S_IFREG | 0666; /* No exec bits */

	ap.aup_uid = 1000;
	ap.aup_gid = 1000;
	ap.aup_len = 0;
	ap.aup_gids = NULL;

	/*
	 * Owner should NOT be allowed to exec even with override if X bit is missing.
	 */
	ret = inode_access_check_flags(inode, &ap, X_OK,
				       REFFS_ACCESS_OWNER_OVERRIDE);
	ck_assert_int_eq(ret, -EACCES);

	inode_active_put(inode);
	super_block_put(sb);
}
END_TEST

static void fs_test_owner_setup(void)
{
	fs_test_setup();
}

static void fs_test_owner_teardown(void)
{
	fs_test_teardown();
}

Suite *fs_owner_write_suite(void)
{
	Suite *s = suite_create("fs: owner_write");
	TCase *tc = tcase_create("Core");

	tcase_add_checked_fixture(tc, fs_test_owner_setup,
				  fs_test_owner_teardown);
	tcase_add_test(tc, test_owner_write_no_perm_bit);
	tcase_add_test(tc, test_owner_read_no_perm_bit);
	tcase_add_test(tc, test_owner_exec_no_perm_bit);

	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int number_failed;

	fs_test_global_init();

	SRunner *sr = srunner_create(fs_owner_write_suite());

	srunner_set_fork_status(sr, CK_NOFORK);
	srunner_run_all(sr, CK_NORMAL);
	number_failed = srunner_ntests_failed(sr);
	srunner_free(sr);

	fs_test_global_fini();

	return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
