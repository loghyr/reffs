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
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include "posix_recovery.h"
#include "reffs/fs.h"
#include "reffs/inode.h"
#include "reffs/test.h"
#include "reffs/backend.h"

/* Helper to write a meta file with custom header */
static int write_custom_meta(struct test_context *ctx, uint64_t ino,
			     uint32_t magic, uint32_t version)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/sb_1/ino_%lu.meta", ctx->backend_path,
		 ino);
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return errno;

	struct reffs_disk_header hdr = {
		.rdh_magic = magic,
		.rdh_version = version,
	};
	struct inode_disk id = { 0 };
	id.id_mode = S_IFREG | 0644;
	id.id_nlink = 1;

	if (write(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
		close(fd);
		return EIO;
	}
	if (write(fd, &id, sizeof(id)) != sizeof(id)) {
		close(fd);
		return EIO;
	}
	close(fd);
	return 0;
}

START_TEST(test_magic_and_versioning)
{
	struct test_context ctx;
	ck_assert_int_eq(test_setup(&ctx), 0);

	/* 1. Test Valid Headered Format (Sync then reload) */
	struct inode *inode = ctx.sb->sb_dirent->rd_inode; /* Root (ino 1) */
	inode->i_uid = REFFS_ID_MAKE(REFFS_ID_UNIX, 0, 1001);
	inode->i_attr_flags = INODE_IS_OFFLINE;
	inode_sync_to_disk(inode);

	/* 2. Test Corruption (Wrong Magic) */
	ck_assert_int_eq(write_custom_meta(&ctx, 2, 0xBAD0BEEF,
					   REFFS_DISK_VERSION_1),
			 0);

	/* 3. Test Unsupported Version (Version 2) */
	ck_assert_int_eq(write_custom_meta(&ctx, 3, REFFS_DISK_MAGIC_META, 2),
			 0);

	/* Simulate recovery */
	struct super_block *sb2 = super_block_alloc(SUPER_BLOCK_ROOT_ID, "/",
						    REFFS_STORAGE_POSIX,
						    ctx.backend_path);
	ck_assert(sb2 != NULL);
	super_block_dirent_create(sb2, NULL, reffs_life_action_birth);

	/* recovery will skip ino 2 and 3 because of header/version errors */
	reffs_fs_recover(sb2);

	/* Verify Root (Valid Header) */
	ck_assert_uint_eq(sb2->sb_dirent->rd_inode->i_uid, 1001);
	ck_assert_uint_eq(sb2->sb_dirent->rd_inode->i_attr_flags,
			  INODE_IS_OFFLINE);

	/* Verify Ino 2 (Bad Magic) - should NOT be findable */
	struct inode *ino2 = inode_find(sb2, 2);
	ck_assert(ino2 == NULL);

	/* Verify Ino 3 (Unsupported Version) - should NOT be findable */
	struct inode *ino3 = inode_find(sb2, 3);
	ck_assert(ino3 == NULL);

	super_block_dirent_release(sb2, reffs_life_action_death);
	super_block_put(sb2);
	test_teardown(&ctx);
}
END_TEST

Suite *recovery_suite(void)
{
	Suite *s = suite_create("POSIX Recovery 15: Magic and Versioning");
	TCase *tc = tcase_create("Core");
	tcase_add_test(tc, test_magic_and_versioning);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	return fs_test_run(recovery_suite());
}
