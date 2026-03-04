/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#define _GNU_SOURCE
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <stdint.h>
#include <check.h>
#include <stdbool.h>
#include <errno.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

#include "posix_recovery.h"
#include "reffs/fs.h"
#include "reffs/test.h"
#include "reffs/super_block.h"
#include "reffs/inode.h"

/* Helper to setup a private tmpfs mount for testing limits */
static int setup_private_tmpfs(const char *mountpoint, size_t size_mb)
{
	char options[64];

	/* Enter a new user+mount namespace - no privileges needed */
	if (unshare(CLONE_NEWUSER | CLONE_NEWNS) < 0)
		return -errno;

	if (mkdir(mountpoint, 0755) < 0 && errno != EEXIST)
		return -errno;

	snprintf(options, sizeof(options), "size=%zuM", size_mb);
	if (mount("tmpfs", mountpoint, "tmpfs", 0, options) < 0)
		return -errno;

	return 0;
}

START_TEST(test_fs_usage_posix_basic)
{
	struct test_context ctx;
	struct reffs_fs_usage_stats stats;
	const char *file1 = "/file1";
	char buf[1024];

	ck_assert_int_eq(test_setup(&ctx), 0);

	/* Check initial usage: root directory exists */
	ck_assert_int_eq(reffs_fs_usage(&stats), 0);
	ck_assert_uint_eq(stats.used_bytes, 0);
	ck_assert_uint_eq(stats.used_files, 1); /* Root directory */

	/* Create and write to file1 */
	ck_assert_int_eq(reffs_fs_create(file1, 0644), 0);
	memset(buf, 'A', 512);
	ck_assert_int_eq(reffs_fs_write(file1, buf, 512, 0), 512);

	/* Check usage after file1 */
	ck_assert_int_eq(reffs_fs_usage(&stats), 0);
	ck_assert_uint_eq(stats.used_bytes, 512);
	ck_assert_uint_eq(stats.used_files, 2); /* Root + file1 */

	/* Delete file1 */
	ck_assert_int_eq(reffs_fs_unlink(file1), 0);

	/* Check usage after unlink */
	ck_assert_int_eq(reffs_fs_usage(&stats), 0);
	ck_assert_uint_eq(stats.used_bytes, 0);
	ck_assert_uint_eq(stats.used_files, 1);

	test_teardown(&ctx);
}
END_TEST

START_TEST(test_fs_usage_posix_tmpfs)
{
	char tmp_mount[] = "/tmp/reffs_tmpfs_XXXXXX";
	struct reffs_fs_usage_stats stats;
	struct super_block *sb;
	struct statvfs sv;

	if (!mkdtemp(tmp_mount)) {
		ck_abort_msg("Failed to create temp mount point");
	}

	/* Setup private 10MB tmpfs */
	if (setup_private_tmpfs(tmp_mount, 10) < 0) {
		rmdir(tmp_mount);
		ck_abort_msg(
			"Failed to setup private tmpfs - maybe CLONE_NEWUSER not supported?");
	}

	/* Create a POSIX superblock on this tmpfs */
	sb = super_block_alloc(10, "/mnt/tmpfs", REFFS_STORAGE_POSIX,
			       tmp_mount);
	ck_assert(sb != NULL);
	super_block_dirent_create(sb, NULL, reffs_life_action_birth);

	/* Verify that stats reflect the 10MB tmpfs */
	ck_assert_int_eq(reffs_fs_usage(&stats), 0);

	/* 10MB is 10 * 1024 * 1024 = 10485760 bytes */
	ck_assert_uint_eq(stats.total_bytes, 10 * 1024 * 1024);

	/* Compare with statvfs directly from the mount */
	ck_assert_int_eq(statvfs(tmp_mount, &sv), 0);
	ck_assert_uint_eq(stats.total_bytes,
			  (uint64_t)sv.f_blocks * sv.f_frsize);
	ck_assert_uint_eq(stats.total_files, (uint64_t)sv.f_files);

	/* Initial state should be empty (except root) */
	ck_assert_uint_eq(stats.used_bytes, 0);
	ck_assert_uint_eq(stats.used_files, 1);

	/* Cleanup */
	super_block_dirent_release(sb, reffs_life_action_death);
	super_block_put(sb);

	umount(tmp_mount);
	rmdir(tmp_mount);
}
END_TEST

Suite *fs_usage_posix_suite(void)
{
	Suite *s = suite_create("fs: usage (POSIX)");
	TCase *tc = tcase_create("Core");
	tcase_add_test(tc, test_fs_usage_posix_basic);
	tcase_add_test(tc, test_fs_usage_posix_tmpfs);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	rcu_register_thread();
	SRunner *sr = srunner_create(fs_usage_posix_suite());
	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	rcu_unregister_thread();
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
