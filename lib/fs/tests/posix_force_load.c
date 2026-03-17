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

START_TEST(test_posix_force_load)
{
	struct test_context ctx;
	struct inode *inode;
	struct inode_disk id = { 0 };

	id.id_uid = 1234;
	id.id_gid = 5678;
	id.id_mode = S_IFREG | 0644;
	id.id_nlink = 1;
	id.id_size = 100;

	ck_assert_int_eq(test_setup(&ctx), 0);

	/*
	 * Scenario:
	 * - We have a meta file for ino 10.
	 * - We DON'T call reffs_fs_recover().
	 * - Calling inode_alloc(ctx.sb, 10) should load it.
	 */
	test_write_meta(&ctx, 10, &id);

	/* inode_find should NOT find it yet */
	inode = inode_find(ctx.sb, 10);
	ck_assert(inode == NULL);

	/* inode_alloc should LOAD it */
	inode = inode_alloc(ctx.sb, 10);
	ck_assert(inode != NULL);
	ck_assert_uint_eq(inode->i_ino, 10);
	ck_assert_uint_eq(inode->i_uid, 1234);
	ck_assert_uint_eq(inode->i_gid, 5678);
	ck_assert_uint_eq(inode->i_mode, S_IFREG | 0644);
	ck_assert_int_eq(inode->i_size, 100);

	inode_active_put(inode);
	test_teardown(&ctx);
}
END_TEST

Suite *force_load_suite(void)
{
	Suite *s = suite_create("POSIX Force Load");
	TCase *tc = tcase_create("Core");
	tcase_add_test(tc, test_posix_force_load);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	return fs_test_run(force_load_suite());
}
