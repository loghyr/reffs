/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * Test 13: inode_sync_to_disk must not be called before load_inode_attributes
 *          during recovery (metadata preservation check).
 *
 * The bug: dirent_parent_attach() --> inode_sync_to_disk() is called on a
 * freshly-zeroed inode *before* load_inode_attributes() restores the
 * attributes from disk.  This overwrites the .meta file with zeroes.
 *
 * After the fix (gating inode_sync_to_disk on
 * rla != reffs_life_action_load), inode_sync_to_disk must NOT fire during
 * dirent_parent_attach for load-path dirents, so the .meta file on disk
 * must remain unchanged after reffs_fs_recover().
 *
 * Verification: read the .meta file after recover and compare with what was
 * written before it.  If the file has been zeroed, the bug is present.
 *
 * Related issue (reffs_issues.md):
 *   POSIX Backend -- "inode_sync_to_disk called before load_inode_attributes
 *   during recovery": fix is to gate on rla != reffs_life_action_load.
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
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include "posix_recovery.h"
#include "reffs/fs.h"
#include "reffs/test.h"

START_TEST(test_meta_not_overwritten_during_recovery)
{
	struct test_context ctx;
	ck_assert_int_eq(test_setup(&ctx), 0);

	struct inode_disk id_dir = { .id_mode = S_IFDIR | 0755, .id_nlink = 2 };
	struct inode_disk id_file = {
		.id_mode = S_IFREG | 0640,
		.id_nlink = 1,
		.id_uid = 1001,
		.id_gid = 2002,
		.id_size = 512,
	};

	test_write_meta(&ctx, 1, &id_dir);
	test_write_meta(&ctx, 2, &id_file);

	int fd;
	ck_assert_int_eq(test_write_dir_header(&ctx, 1, 4, &fd), 0);
	test_write_dir_entry(fd, 3, 2, "preserved");
	close(fd);

	/*
	 * Snapshot the .meta content BEFORE recovery so we can compare after.
	 */
	char meta_path[PATH_MAX];
	snprintf(meta_path, sizeof(meta_path), "%s/sb_1/ino_2.meta",
		 ctx.backend_path);

	struct inode_disk before = { 0 };
	int mfd = open(meta_path, O_RDONLY);
	ck_assert(mfd >= 0);
	ck_assert_int_eq(read(mfd, &before, sizeof(before)), sizeof(before));
	close(mfd);

	reffs_fs_recover(ctx.sb);

	/*
	 * Read the .meta file again.  If inode_sync_to_disk fired on a
	 * zeroed inode during load, uid/gid/mode will all be 0.
	 */
	struct inode_disk after = { 0 };
	mfd = open(meta_path, O_RDONLY);
	ck_assert(mfd >= 0);
	ck_assert_int_eq(read(mfd, &after, sizeof(after)), sizeof(after));
	close(mfd);

	ck_assert_uint_eq(after.id_uid, before.id_uid);
	ck_assert_uint_eq(after.id_gid, before.id_gid);
	ck_assert_uint_eq(after.id_mode, before.id_mode);
	ck_assert_int_eq(after.id_size, before.id_size);

	/* Also confirm the in-memory inode was populated correctly */
	struct inode *inode = inode_find(ctx.sb, 2);
	ck_assert(inode != NULL);
	ck_assert_uint_eq(inode->i_uid, 1001);
	ck_assert_uint_eq(inode->i_gid, 2002);
	inode_active_put(inode);

	test_teardown(&ctx);
}
END_TEST

Suite *recovery_suite(void)
{
	Suite *s = suite_create(
		"POSIX Recovery 13: .meta Not Overwritten During Recovery");
	TCase *tc = tcase_create("Core");
	tcase_add_test(tc, test_meta_not_overwritten_during_recovery);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	return fs_test_run(recovery_suite());
}
