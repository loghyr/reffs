/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * Test 9: sb_next_ino from independent .meta scan, not directory traversal.
 *
 * If a high-numbered inode's .meta file exists on disk but the inode is not
 * reachable from the root (e.g. it was orphaned after a crash between
 * inode_sync_to_disk and dirent_sync_to_disk), the directory traversal in
 * recover_directory_recursive() will never visit it.  However, the initial
 * full-directory scan in reffs_fs_recover() must still find it and set
 * sb_next_ino correctly.
 *
 * This is a stricter version of test 1 (inode gaps): here the high-numbered
 * inode is not linked into any directory, so traversal alone would miss it.
 *
 * Related issue (reffs_issues.md):
 *   POSIX Backend — "sb_next_ino recovery depends on complete traversal":
 *   fix is to independently scan all ino_*.meta files in sb_N/ at startup.
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

START_TEST(test_sb_next_ino_orphaned_inode)
{
	struct test_context ctx;

	struct inode_disk id_dir = { 0 };
	id_dir.id_mode = S_IFDIR | 0755;
	id_dir.id_nlink = 2;

	struct inode_disk id_file = { 0 };
	id_file.id_mode = S_IFREG | 0644;
	id_file.id_nlink = 1;

	ck_assert_int_eq(test_setup(&ctx), 0);

	/* Root inode */
	test_write_meta(&ctx, 1, &id_dir);

	/* Normal child linked into the directory */
	int fd;
	ck_assert_int_eq(test_write_dir_header(&ctx, 1, 4, &fd), 0);
	test_write_dir_entry(fd, 3, 2, "linked_file");
	close(fd);
	test_write_meta(&ctx, 2, &id_file);

	/*
	 * Orphaned inode: .meta exists on disk (inode_sync_to_disk ran) but
	 * the parent .dir was never updated (dirent_sync_to_disk crashed).
	 * It has a much higher inode number.
	 */
	test_write_meta(&ctx, 99, &id_file);

	reffs_fs_recover(ctx.sb);

	/*
	 * sb_next_ino must reflect the orphaned inode, not just the highest
	 * inode reached via directory traversal (which would be 2).
	 */
	ck_assert_uint_eq(ctx.sb->sb_next_ino, 100);

	/*
	 * The linked file must still be accessible normally.
	 */
	struct reffs_dirent *de = dirent_find(
		ctx.sb->sb_dirent, reffs_text_case_sensitive, "linked_file");
	ck_assert(de != NULL);
	dirent_put(de);

	test_teardown(&ctx);
}
END_TEST

Suite *recovery_suite(void)
{
	Suite *s = suite_create(
		"POSIX Recovery 9: sb_next_ino from Orphaned Inode");
	TCase *tc = tcase_create("Core");
	tcase_add_test(tc, test_sb_next_ino_orphaned_inode);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	return fs_test_run(recovery_suite());
}
