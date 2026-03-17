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
#include "reffs/fs.h"
#include "reffs/context.h"
#include "fs_test_harness.h"

START_TEST(test_rename_src_parent_no_write)
{
	struct reffs_context ctx = { .uid = 1000, .gid = 1000 };

	/* Create /p as root (UID 0), with 0755 (no write for others) */
	reffs_set_context(NULL);
	ck_assert_int_eq(reffs_fs_mkdir("/p", 0755), 0);
	ck_assert_int_eq(reffs_fs_create("/p/f", S_IFREG | 0644), 0);

	/* Try rename as user 1000 */
	reffs_set_context(&ctx);
	ck_assert_int_eq(reffs_fs_rename("/p/f", "/p/f2"), -EACCES);

	/* Reset to root and verify we can still rename */
	reffs_set_context(NULL);
	ck_assert_int_eq(reffs_fs_rename("/p/f", "/p/f2"), 0);

	ck_assert_int_eq(reffs_fs_unlink("/p/f2"), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/p"), 0);
}
END_TEST

START_TEST(test_mkdir_no_write_permission)
{
	struct reffs_context ctx = { .uid = 1000, .gid = 1000 };

	/* Create /root_dir as root (UID 0), with 0755 */
	reffs_set_context(NULL);
	ck_assert_int_eq(reffs_fs_mkdir("/root_dir", 0755), 0);

	/* Try mkdir as user 1000 */
	reffs_set_context(&ctx);
	ck_assert_int_eq(reffs_fs_mkdir("/root_dir/sub", 0755), -EACCES);

	/* Reset to root and verify success */
	reffs_set_context(NULL);
	ck_assert_int_eq(reffs_fs_mkdir("/root_dir/sub", 0755), 0);

	ck_assert_int_eq(reffs_fs_rmdir("/root_dir/sub"), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/root_dir"), 0);
}
END_TEST

START_TEST(test_rename_search_permission_failure)
{
	struct reffs_context ctx = { .uid = 1000, .gid = 1000 };

	/* Create /top/p/f as root */
	reffs_set_context(NULL);
	ck_assert_int_eq(reffs_fs_mkdir("/top", 0755), 0);
	ck_assert_int_eq(reffs_fs_mkdir("/top/p", 0700),
			 0); /* No perms for others */
	ck_assert_int_eq(reffs_fs_create("/top/p/f", S_IFREG | 0644), 0);

	/* Try rename /top/p/f as user 1000 - should fail on search of /top/p */
	reffs_set_context(&ctx);
	ck_assert_int_eq(reffs_fs_rename("/top/p/f", "/top/f2"), -EACCES);

	/* Cleanup as root */
	reffs_set_context(NULL);
	ck_assert_int_eq(reffs_fs_unlink("/top/p/f"), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/top/p"), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/top"), 0);
}
END_TEST

START_TEST(test_sticky_bit_semantics)
{
	struct reffs_context ctx_a = { .uid = 1001, .gid = 1001 };
	struct reffs_context ctx_b = { .uid = 1002, .gid = 1002 };
	struct reffs_context ctx_c = { .uid = 1003, .gid = 1003 };

	/* 1. Setup: Create a sticky directory with 1777 perms as root */
	reffs_set_context(NULL);
	ck_assert_int_eq(reffs_fs_mkdir("/sticky", S_ISVTX | 0777), 0);
	ck_assert_int_eq(reffs_fs_chown("/sticky", 1001, 1001), 0);
	ck_assert_int_eq(reffs_fs_chmod("/sticky", S_ISVTX | 0777), 0);

	/* 2. User B creates a file in the sticky directory */
	reffs_set_context(&ctx_b);
	ck_assert_int_eq(reffs_fs_create("/sticky/file_b", S_IFREG | 0644), 0);

	/* 3. Unauthorized: User C tries to unlink User B's file -> EACCES */
	reffs_set_context(&ctx_c);
	ck_assert_int_eq(reffs_fs_unlink("/sticky/file_b"), -EACCES);

	/* 4. Unauthorized: User C tries to rename User B's file -> EACCES */
	ck_assert_int_eq(reffs_fs_rename("/sticky/file_b", "/sticky/file_c"),
			 -EACCES);

	/* 5. Authorized: User B (owner) can rename their own file */
	reffs_set_context(&ctx_b);
	ck_assert_int_eq(reffs_fs_rename("/sticky/file_b", "/sticky/file_b2"),
			 0);

	/* 6. Authorized: User A (dir owner) can unlink User B's file */
	reffs_set_context(&ctx_a);
	ck_assert_int_eq(reffs_fs_unlink("/sticky/file_b2"), 0);

	/* 7. Replacement: User B creates f1, User C creates f2. User B can't overwrite f2. */
	reffs_set_context(&ctx_b);
	ck_assert_int_eq(reffs_fs_create("/sticky/f1", S_IFREG | 0644), 0);
	reffs_set_context(&ctx_c);
	ck_assert_int_eq(reffs_fs_create("/sticky/f2", S_IFREG | 0644), 0);

	reffs_set_context(&ctx_b);
	/* Rename f1 -> f2 should fail because User B doesn't own f2 (the entry being replaced) */
	ck_assert_int_eq(reffs_fs_rename("/sticky/f1", "/sticky/f2"), -EACCES);

	/* Cleanup as root */
	reffs_set_context(NULL);
	ck_assert_int_eq(reffs_fs_unlink("/sticky/f1"), 0);
	ck_assert_int_eq(reffs_fs_unlink("/sticky/f2"), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/sticky"), 0);
}
END_TEST

static void fs_test_perm_setup(void)
{
	fs_test_setup();
	fs_test_uid = getuid();
	fs_test_gid = getgid();
}

static void fs_test_perm_teardown(void)
{
	reffs_set_context(NULL);
	fs_test_teardown();
}

Suite *fs_permission_suite(void)
{
	Suite *s = suite_create("fs: permissions");
	TCase *tc = tcase_create("Core");

	tcase_add_checked_fixture(tc, fs_test_perm_setup,
				  fs_test_perm_teardown);
	tcase_add_test(tc, test_rename_src_parent_no_write);
	tcase_add_test(tc, test_mkdir_no_write_permission);
	tcase_add_test(tc, test_rename_search_permission_failure);
	tcase_add_test(tc, test_sticky_bit_semantics);

	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	return fs_test_run(fs_permission_suite());
}
