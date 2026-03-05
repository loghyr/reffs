/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/*
 * fs_test_write_read.c — reffs_fs_write() and reffs_fs_read() semantics
 *
 * Tests the data path and the timestamp contract:
 *   write: updates i_size, i_used, mtime, ctime; atime unchanged
 *   read:  returns correct bytes; updates atime; mtime/ctime unchanged
 *
 * Also tests the boundary cases visible in fs.c:
 *   - write to a directory returns -EISDIR
 *   - read from a directory returns -EISDIR
 *   - read past EOF returns -EOVERFLOW
 *   - append at non-zero offset extends i_size correctly
 *   - i_used rounds up: a write of 1 byte sets i_used to 1
 *
 * The write/read tests below correspond directly to the sequences in
 * fuse_test.c but call reffs_fs_{write,read}() without the fuse shim.
 */

#include "fs_test_harness.h"

#define BUF 1024

uid_t fs_test_uid;
gid_t fs_test_gid;

static void setup(void)
{
	fs_test_setup();
}
static void teardown(void)
{
	fs_test_teardown();
}

/* ------------------------------------------------------------------ */

START_TEST(test_write_returns_size)
{
	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_write("/f", "hello", 5, 0), 5);
	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
}
END_TEST

START_TEST(test_write_updates_size)
{
	struct stat st;

	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_write("/f", "hello", 5, 0), 5);
	ck_assert_int_eq(reffs_fs_getattr("/f", &st), 0);
	ck_assert_int_eq(st.st_size, 5);

	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
}
END_TEST

START_TEST(test_write_updates_mtime_ctime_not_atime)
{
	struct stat st_pre, st_post;

	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_getattr("/f", &st_pre), 0);

	usleep(1000);
	ck_assert_int_eq(reffs_fs_write("/f", "hello", 5, 0), 5);
	ck_assert_int_eq(reffs_fs_getattr("/f", &st_post), 0);

	ck_assert_timespec_eq(st_pre.st_atim, st_post.st_atim);
	ck_assert_timespec_lt(st_pre.st_mtim, st_post.st_mtim);
	ck_assert_timespec_lt(st_pre.st_ctim, st_post.st_ctim);

	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
}
END_TEST

START_TEST(test_write_to_dir_eisdir)
{
	ck_assert_int_eq(reffs_fs_mkdir("/d", 0755), 0);
	ck_assert_int_eq(reffs_fs_write("/d", "x", 1, 0), -EISDIR);
	ck_assert_int_eq(reffs_fs_rmdir("/d"), 0);
}
END_TEST

START_TEST(test_read_returns_data)
{
	char buf[BUF] = { 0 };

	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_write("/f", "hello", 5, 0), 5);
	ck_assert_int_eq(reffs_fs_read("/f", buf, 5, 0), 5);
	ck_assert_str_eq(buf, "hello");

	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
}
END_TEST

START_TEST(test_read_updates_atime_not_mtime_ctime)
{
	struct stat st_pre, st_post;
	char buf[BUF] = { 0 };

	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_write("/f", "hello", 5, 0), 5);
	ck_assert_int_eq(reffs_fs_getattr("/f", &st_pre), 0);

	usleep(1000);
	ck_assert_int_eq(reffs_fs_read("/f", buf, 5, 0), 5);
	ck_assert_int_eq(reffs_fs_getattr("/f", &st_post), 0);

	ck_assert_timespec_lt(st_pre.st_atim, st_post.st_atim);
	ck_assert_timespec_eq(st_pre.st_mtim, st_post.st_mtim);
	ck_assert_timespec_eq(st_pre.st_ctim, st_post.st_ctim);

	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
}
END_TEST

START_TEST(test_read_from_dir_eisdir)
{
	char buf[BUF] = { 0 };
	ck_assert_int_eq(reffs_fs_mkdir("/d", 0755), 0);
	ck_assert_int_eq(reffs_fs_read("/d", buf, 1, 0), -EISDIR);
	ck_assert_int_eq(reffs_fs_rmdir("/d"), 0);
}
END_TEST

START_TEST(test_read_past_eof_eoverflow)
{
	char buf[BUF] = { 0 };

	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_write("/f", "hello", 5, 0), 5);

	/* offset 10 is beyond the 5-byte file */
	ck_assert_int_eq(reffs_fs_read("/f", buf, 5, 10), -EOVERFLOW);

	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
}
END_TEST

/*
 * Append write: two sequential writes must concatenate correctly and the
 * final size must be the sum.
 */
START_TEST(test_append_extends_size)
{
	struct stat st;
	char buf[BUF] = { 0 };

	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_write("/f", "hello", 5, 0), 5);

	usleep(1000);
	ck_assert_int_eq(reffs_fs_write("/f", "world", 5, 5), 5);

	ck_assert_int_eq(reffs_fs_getattr("/f", &st), 0);
	ck_assert_int_eq(st.st_size, 10);

	ck_assert_int_eq(reffs_fs_read("/f", buf, 10, 0), 10);
	ck_assert_str_eq(buf, "helloworld");

	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
}
END_TEST

/*
 * i_used round-up: a 1-byte write must set i_used to 1 (not 0).
 * st_blocks must be st_blksize / 512.
 */
START_TEST(test_write_sets_i_used)
{
	struct stat st;

	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_write("/f", "x", 1, 0), 1);
	ck_assert_int_eq(reffs_fs_getattr("/f", &st), 0);

	ck_assert_int_gt(st.st_blksize, 0);
	/* i_used == 1 → st_blocks == st_blksize / 512 */
	ck_assert_int_eq(st.st_blocks, st.st_blksize / 512);

	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
}
END_TEST

/* ------------------------------------------------------------------ */

Suite *fs_write_read_suite(void)
{
	Suite *s = suite_create("fs: write/read");
	TCase *tc = tcase_create("Core");
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_write_returns_size);
	tcase_add_test(tc, test_write_updates_size);
	tcase_add_test(tc, test_write_updates_mtime_ctime_not_atime);
	tcase_add_test(tc, test_write_to_dir_eisdir);
	tcase_add_test(tc, test_read_returns_data);
	tcase_add_test(tc, test_read_updates_atime_not_mtime_ctime);
	tcase_add_test(tc, test_read_from_dir_eisdir);
	tcase_add_test(tc, test_read_past_eof_eoverflow);
	tcase_add_test(tc, test_append_extends_size);
	tcase_add_test(tc, test_write_sets_i_used);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	fs_test_global_init();
	SRunner *sr = srunner_create(fs_write_read_suite());
	srunner_set_fork_status(sr, CK_NOFORK);
	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	fs_test_global_fini();
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
