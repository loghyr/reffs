/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "fs_test_harness.h"

static void setup(void)
{
	fs_test_setup();
}

static void teardown(void)
{
	fs_test_teardown();
}

START_TEST(test_fs_usage_ram_basic)
{
	struct reffs_fs_usage_stats stats;
	const char *file1 = "/file1";
	char buf[1024];

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
	ck_assert_uint_eq(stats.used_files, 1); /* Only root remains */
}
END_TEST

START_TEST(test_fs_usage_ram_multiple_sb)
{
	struct reffs_fs_usage_stats stats;
	struct super_block *sb2;

	/* Initial: one RAM sb (id=1) with root */
	ck_assert_int_eq(reffs_fs_usage(&stats), 0);
	ck_assert_uint_eq(stats.used_files, 1);
	uint64_t total1 = stats.total_bytes;

	/* Add a second RAM-based superblock (id=2) */
	sb2 = super_block_alloc(2, "/ram2", REFFS_STORAGE_RAM, NULL);
	ck_assert(sb2 != NULL);
	super_block_dirent_create(sb2, NULL, reffs_life_action_birth);

	/* Check aggregate usage: 2 sbs, each with root */
	ck_assert_int_eq(reffs_fs_usage(&stats), 0);
	ck_assert_uint_eq(stats.used_files, 2);
	/* Total bytes should be total1 + SIZE_MAX (with overflow) */
	ck_assert_uint_eq(stats.total_bytes, total1 + (uint64_t)SIZE_MAX);

	/* Cleanup sb2 */
	super_block_dirent_release(sb2, reffs_life_action_death);
	super_block_drain(sb2);
	super_block_put(sb2);
}
END_TEST

Suite *fs_usage_ram_suite(void)
{
	Suite *s = suite_create("fs: usage (RAM)");
	TCase *tc = tcase_create("Core");
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_fs_usage_ram_basic);
	tcase_add_test(tc, test_fs_usage_ram_multiple_sb);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	return fs_test_run(fs_usage_ram_suite());
}
