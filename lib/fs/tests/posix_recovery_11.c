/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * Test 11: sb_bytes_used after recovery reflects actual on-disk file sizes.
 *
 * During recovery, data_block_alloc() is called with size=0 (the file is
 * opened for access but not resized).  db_size must be populated from fstat()
 * rather than left at 0.  The superblock's sb_bytes_used accounting that
 * happens when the data block is opened must also reflect the real size.
 *
 * Two files with known sizes are recovered; sb_bytes_used after recovery must
 * equal the sum of their sizes rounded up to block boundaries.
 *
 * Related issue (reffs_issues.md):
 *   POSIX Backend -- "data_block_alloc with size=0 leaves db_size=0": during
 *   recovery, data files are opened with size=0; db_size remains 0 until the
 *   first write, causing sb_bytes_used accounting to be incorrect until then.
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
#include "reffs/data_block.h"

START_TEST(test_sb_bytes_used_accounting)
{
	struct test_context ctx;
	ck_assert_int_eq(test_setup(&ctx), 0);

	/*
	 * block_size is used for i_used assertions below.  sb_bytes_used uses
	 * raw bytes so does not need it.
	 */
	const size_t block_size = ctx.sb->sb_block_size;

	struct inode_disk id_dir = { .id_mode = S_IFDIR | 0755, .id_nlink = 3 };
	struct inode_disk id_f1 = { .id_mode = S_IFREG | 0644,
				    .id_nlink = 1,
				    .id_size = 100 };
	struct inode_disk id_f2 = { .id_mode = S_IFREG | 0644,
				    .id_nlink = 1,
				    .id_size = 5000 };

	test_write_meta(&ctx, 1, &id_dir);
	test_write_meta(&ctx, 2, &id_f1);
	test_write_meta(&ctx, 3, &id_f2);

	/* Create actual .dat files of the declared sizes */
	char buf[5000];
	memset(buf, 0xAB, sizeof(buf));
	test_write_dat(&ctx, 2, buf, 100);
	test_write_dat(&ctx, 3, buf, 5000);

	int fd;
	ck_assert_int_eq(test_write_dir_header(&ctx, 1, 5, &fd), 0);
	test_write_dir_entry(fd, 3, 2, "small");
	test_write_dir_entry(fd, 4, 3, "large");
	close(fd);

	reffs_fs_recover(ctx.sb);

	/* Verify individual data block sizes via fstat path */
	struct inode *i2 = inode_find(ctx.sb, 2);
	ck_assert(i2 != NULL);
	ck_assert(i2->i_db != NULL);
	ck_assert_uint_eq(data_block_get_size(i2->i_db), 100);
	ck_assert_int_eq(i2->i_size, 100);
	ck_assert_uint_eq(i2->i_used, (100 + block_size - 1) / block_size);
	inode_active_put(i2);

	struct inode *i3 = inode_find(ctx.sb, 3);
	ck_assert(i3 != NULL);
	ck_assert(i3->i_db != NULL);
	ck_assert_uint_eq(data_block_get_size(i3->i_db), 5000);
	ck_assert_int_eq(i3->i_size, 5000);
	ck_assert_uint_eq(i3->i_used, (5000 + block_size - 1) / block_size);
	inode_active_put(i3);

	/*
	 * sb_bytes_used is in raw bytes (not block-rounded) -- it tracks the
	 * sum of i_size values, mirroring what nfs3_server.c does on the write
	 * path.  i_used is the block count (block-rounded), which is a
	 * separate field.
	 */
	ck_assert_uint_eq(ctx.sb->sb_bytes_used, 100 + 5000);

	test_teardown(&ctx);
}
END_TEST

Suite *recovery_suite(void)
{
	Suite *s = suite_create("POSIX Recovery 11: sb_bytes_used Accounting");
	TCase *tc = tcase_create("Core");
	tcase_add_test(tc, test_sb_bytes_used_accounting);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	return fs_test_run(recovery_suite());
}
