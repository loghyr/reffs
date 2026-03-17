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

START_TEST(test_symlink_restoration)
{
	struct test_context ctx;
	struct inode_disk id_dir = { 0 };
	id_dir.id_mode = S_IFDIR | 0755;
	id_dir.id_nlink = 2;

	struct inode_disk id_link = { 0 };
	id_link.id_mode = S_IFLNK | 0777;
	id_link.id_nlink = 1;

	ck_assert_int_eq(test_setup(&ctx), 0);

	/* Write root metadata */
	test_write_meta(&ctx, 1, &id_dir);

	/* Root directory contains a symlink */
	int fd;
	test_write_dir_header(&ctx, 1, 4, &fd);
	test_write_dir_entry(fd, 3, 2, "mysymlink");
	close(fd);

	/* Write symlink metadata and target file */
	test_write_meta(&ctx, 2, &id_link);
	test_write_lnk(&ctx, 2, "/path/to/target");

	reffs_fs_recover(ctx.sb);

	struct inode *inode = inode_find(ctx.sb, 2);
	ck_assert(inode);

	/* Verification: symlink target must be restored */
	ck_assert(inode->i_symlink != NULL);
	ck_assert_str_eq(inode->i_symlink, "/path/to/target");

	inode_active_put(inode);

	test_teardown(&ctx);
}
END_TEST

Suite *recovery_suite(void)
{
	Suite *s = suite_create("POSIX Recovery 4: Symlinks");
	TCase *tc = tcase_create("Core");
	tcase_add_test(tc, test_symlink_restoration);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	return fs_test_run(recovery_suite());
}
