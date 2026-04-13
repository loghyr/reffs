/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * Test 16: Zero-ino dir entry handling during recovery.
 *
 * Before the fix, posix_recover_directory_recursive() never set rd->rd_ino
 * from the on-disk dir entry.  rd_ino stayed zero (calloc'd).  The first
 * posix_dir_sync() on the parent directory then wrote ino=0 back to disk for
 * every recovered entry, permanently corrupting the .dir file.
 *
 * This test covers both parts of the fix:
 *
 * 1. test_zero_ino_entry_skipped: A .dir file with a corrupt ino=0 entry is
 *    written alongside a valid entry.  After recovery the valid entry is
 *    present and the corrupt entry is absent.
 *
 * 2. test_rd_ino_set_on_recovery: After recovery, rd->rd_ino equals the
 *    ino from the on-disk dir entry.  If it were left at 0 the next
 *    posix_dir_sync() would persist ino=0, re-introducing the corruption.
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

/*
 * A .dir file that contains one valid entry (ino=2, "good.txt") and one
 * corrupt entry (ino=0, "corrupt.txt") must result in only the valid entry
 * being recovered.
 */
START_TEST(test_zero_ino_entry_skipped)
{
	struct test_context ctx;
	ck_assert_int_eq(test_setup(&ctx), 0);

	struct inode_disk id_root = { .id_mode = S_IFDIR | 0755,
				      .id_nlink = 2 };
	struct inode_disk id_file = { .id_mode = S_IFREG | 0644,
				      .id_nlink = 1 };

	test_write_meta(&ctx, 1, &id_root);
	test_write_meta(&ctx, 2, &id_file);

	/* Root dir: one good entry and one corrupt (ino=0) entry. */
	int fd;
	ck_assert_int_eq(test_write_dir_header(&ctx, 1, 4, &fd), 0);
	test_write_dir_entry(fd, 2, 2, "good.txt");
	test_write_dir_entry(fd, 3, 0, "corrupt.txt"); /* ino=0: invalid */
	close(fd);

	reffs_fs_recover(ctx.sb);

	/* good.txt must be recoverable. */
	struct reffs_dirent *de = dirent_find(
		ctx.sb->sb_dirent, reffs_text_case_sensitive, "good.txt");
	ck_assert(de != NULL);
	ck_assert(de->rd_inode != NULL);
	ck_assert_uint_eq(de->rd_inode->i_ino, 2);
	dirent_put(de);

	/* corrupt.txt must not appear -- ino=0 entries are skipped. */
	struct reffs_dirent *de_bad = dirent_find(
		ctx.sb->sb_dirent, reffs_text_case_sensitive, "corrupt.txt");
	ck_assert(de_bad == NULL);

	test_teardown(&ctx);
}
END_TEST

/*
 * After recovery, rd->rd_ino must equal the ino from the on-disk dir entry.
 * Without the fix, rd_ino stayed at 0 after dirent_alloc (calloc'd zero),
 * so the next posix_dir_sync() would write ino=0 for all recovered entries.
 *
 * Verify the fix by inspecting rd_ino directly after recovery, then forcing
 * a second recovery from a re-synced .dir file.
 */
START_TEST(test_rd_ino_set_on_recovery)
{
	struct test_context ctx;
	ck_assert_int_eq(test_setup(&ctx), 0);

	struct inode_disk id_root = { .id_mode = S_IFDIR | 0755,
				      .id_nlink = 2 };
	struct inode_disk id_file = { .id_mode = S_IFREG | 0644,
				      .id_nlink = 1 };

	test_write_meta(&ctx, 1, &id_root);
	test_write_meta(&ctx, 2, &id_file);
	test_write_meta(&ctx, 3, &id_file);

	int fd;
	ck_assert_int_eq(test_write_dir_header(&ctx, 1, 4, &fd), 0);
	test_write_dir_entry(fd, 2, 2, "alpha.txt");
	test_write_dir_entry(fd, 3, 3, "beta.txt");
	close(fd);

	reffs_fs_recover(ctx.sb);

	/*
	 * rd_ino must match the on-disk ino immediately after recovery.
	 * If it is 0 the subsequent posix_dir_sync() will corrupt the file.
	 */
	struct reffs_dirent *de_a = dirent_find(
		ctx.sb->sb_dirent, reffs_text_case_sensitive, "alpha.txt");
	ck_assert(de_a != NULL);
	ck_assert_uint_eq(de_a->rd_ino, 2);
	ck_assert_uint_eq(de_a->rd_inode->i_ino, 2);
	dirent_put(de_a);

	struct reffs_dirent *de_b = dirent_find(
		ctx.sb->sb_dirent, reffs_text_case_sensitive, "beta.txt");
	ck_assert(de_b != NULL);
	ck_assert_uint_eq(de_b->rd_ino, 3);
	ck_assert_uint_eq(de_b->rd_inode->i_ino, 3);
	dirent_put(de_b);

	/*
	 * Force a dir sync by syncing the root inode (which also syncs the
	 * directory listing via posix_inode_sync -> posix_dir_sync).  The
	 * .dir file must be rewritten with the correct inos.
	 */
	inode_sync_to_disk(ctx.sb->sb_dirent->rd_inode);

	/*
	 * Second recovery from the rewritten .dir to confirm the entries
	 * were written with ino=2 and ino=3, not ino=0.
	 */
	struct super_block *sb2 = super_block_alloc(SUPER_BLOCK_ROOT_ID, "/",
						    REFFS_STORAGE_POSIX,
						    ctx.backend_path);
	ck_assert(sb2 != NULL);
	super_block_dirent_create(sb2, NULL, reffs_life_action_birth);
	reffs_fs_recover(sb2);

	struct reffs_dirent *de_a2 = dirent_find(
		sb2->sb_dirent, reffs_text_case_sensitive, "alpha.txt");
	ck_assert(de_a2 != NULL);
	ck_assert_uint_eq(de_a2->rd_ino, 2);
	ck_assert_uint_eq(de_a2->rd_inode->i_ino, 2);
	dirent_put(de_a2);

	struct reffs_dirent *de_b2 = dirent_find(
		sb2->sb_dirent, reffs_text_case_sensitive, "beta.txt");
	ck_assert(de_b2 != NULL);
	ck_assert_uint_eq(de_b2->rd_ino, 3);
	ck_assert_uint_eq(de_b2->rd_inode->i_ino, 3);
	dirent_put(de_b2);

	super_block_dirent_release(sb2, reffs_life_action_death);
	super_block_put(sb2);

	test_teardown(&ctx);
}
END_TEST

Suite *recovery_suite(void)
{
	Suite *s = suite_create("POSIX Recovery 16: Zero-ino dir entries");
	TCase *tc = tcase_create("Core");
	tcase_add_test(tc, test_zero_ino_entry_skipped);
	tcase_add_test(tc, test_rd_ino_set_on_recovery);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	return fs_test_run(recovery_suite());
}
