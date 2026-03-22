/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/*
 * Test 9: i_used / st_blocks accounting.
 *
 * The regression fixed in commit 37f4a90 set i_used = sb_block_size for new
 * directories, causing st_blocks to be inflated by a factor of block_size/512
 * squared.  This test pins the correct formula:
 *
 *   st_blocks = i_used * (sb_block_size / 512)
 *
 * For a new directory, i_used must be 1 (one allocated block), so:
 *
 *   st_blocks == sb_block_size / 512
 *
 * For a new regular file (size 0), i_used must be 0, so st_blocks == 0.
 * After the first write that fills less than one block, i_used stays 1.
 *
 * This test is intentionally independent of posix_recovery_11.c, which tests
 * the same field via the recovery path.
 */

#include "fuse_harness.h"
#include "reffs/super_block.h"

static void setup(void)
{
	fuse_test_setup();
}
static void teardown(void)
{
	fuse_test_teardown();
}

START_TEST(test_mkdir_st_blocks)
{
	struct super_block __attribute__((unused)) *sb;
	struct stat st;
	blksize_t blksize;
	blkcnt_t expected;

	ck_assert_int_eq(reffs_fuse_mkdir("/d", 0755), 0);
	ck_assert_int_eq(reffs_fuse_getattr("/d", &st), 0);

	blksize = st.st_blksize;
	ck_assert_int_gt(blksize, 0);

	expected = blksize / 512; /* i_used == 1 */
	ck_assert_int_eq(st.st_blocks, expected);

	ck_assert_int_eq(reffs_fuse_rmdir("/d"), 0);
}
END_TEST

START_TEST(test_new_file_st_blocks_zero)
{
	struct stat st;
	ck_assert_int_eq(reffs_fuse_create("/f", S_IFREG | 0644, NULL), 0);
	ck_assert_int_eq(reffs_fuse_getattr("/f", &st), 0);
	ck_assert_int_eq(st.st_blocks, 0);
	ck_assert_int_eq(st.st_size, 0);
	ck_assert_int_eq(reffs_fuse_unlink("/f"), 0);
}
END_TEST

Suite *fuse_suite(void)
{
	Suite *s = suite_create("fuse 9: i_used / st_blocks accounting");
	TCase *tc = tcase_create("Core");
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_mkdir_st_blocks);
	tcase_add_test(tc, test_new_file_st_blocks_zero);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	return fuse_test_run(fuse_suite());
}
