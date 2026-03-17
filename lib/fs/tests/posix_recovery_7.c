/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * Test 7: Graceful recovery when a symlink inode's .lnk file is absent.
 *
 * This exercises the crash scenario where a symlink's .meta file was flushed
 * but the server crashed before the .lnk file was written.  Recovery must
 * not crash and must leave i_symlink as NULL so that a subsequent READLINK
 * returns an error rather than dereferencing a garbage pointer.
 *
 * Related issue (reffs_issues.md):
 *   POSIX Backend Persistence & Recovery — "Symlink target (i_symlink) not
 *   persisted": after a restart, all symlinks have i_symlink == NULL.
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

START_TEST(test_symlink_missing_lnk_file)
{
	struct test_context ctx;
	struct inode_disk id_dir = { 0 };
	id_dir.id_mode = S_IFDIR | 0755;
	id_dir.id_nlink = 2;

	struct inode_disk id_link = { 0 };
	id_link.id_mode = S_IFLNK | 0777;
	id_link.id_nlink = 1;

	ck_assert_int_eq(test_setup(&ctx), 0);

	test_write_meta(&ctx, 1, &id_dir);

	int fd;
	ck_assert_int_eq(test_write_dir_header(&ctx, 1, 4, &fd), 0);
	test_write_dir_entry(fd, 3, 2, "dangling_link");
	close(fd);

	/* Write symlink inode .meta but deliberately omit the .lnk file */
	test_write_meta(&ctx, 2, &id_link);

	/* Recovery must not crash */
	reffs_fs_recover(ctx.sb);

	struct inode *inode = inode_find(ctx.sb, 2);
	ck_assert(inode != NULL);

	/*
	 * i_symlink must be NULL — callers must check before dereferencing.
	 * A non-NULL garbage pointer here would indicate the bug is present.
	 */
	ck_assert(inode->i_symlink == NULL);

	inode_active_put(inode);
	test_teardown(&ctx);
}
END_TEST

Suite *recovery_suite(void)
{
	Suite *s = suite_create("POSIX Recovery 7: Missing .lnk File");
	TCase *tc = tcase_create("Core");
	tcase_add_test(tc, test_symlink_missing_lnk_file);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	return fs_test_run(recovery_suite());
}
