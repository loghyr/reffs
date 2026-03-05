/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/*
 * Test 4: File create, write, read.
 *
 * Basic regular-file lifecycle:
 *   - create returns 0; initial size is 0; mode is preserved
 *   - create with S_IFDIR mode returns -EISDIR
 *   - write to a directory returns -EISDIR
 *   - write updates size, mtime, ctime but not atime
 *   - read returns the exact bytes written; updates atime but not mtime/ctime
 *   - read past EOF returns -EOVERFLOW
 *   - append write (non-zero offset) extends size correctly
 *   - unlink removes the file (getattr returns -ENOENT)
 *   - unlink on a directory returns -EISDIR
 */

#include "fuse_harness.h"

#define BUF_LEN 1024

uid_t fuse_test_uid;
gid_t fuse_test_gid;

static void setup(void)
{
	fuse_test_setup();
}
static void teardown(void)
{
	fuse_test_teardown();
}

START_TEST(test_create_initial_attrs)
{
	struct stat st;
	ck_assert_int_eq(reffs_fuse_create("/f", S_IFREG | 0755, NULL), 0);
	ck_assert_int_eq(reffs_fuse_getattr("/f", &st), 0);
	ck_assert_uint_eq(st.st_size, 0);
	ck_assert_uint_eq(st.st_mode, (mode_t)(S_IFREG | 0755));
	ck_assert_uint_eq(st.st_uid, fuse_test_uid);
	ck_assert_uint_eq(st.st_gid, fuse_test_gid);

	ck_assert_int_eq(reffs_fuse_unlink("/f"), 0);
}
END_TEST

START_TEST(test_create_eisdir_on_dir_mode)
{
	ck_assert_int_eq(reffs_fuse_create("/f", S_IFDIR | 0755, NULL),
			 -EISDIR);
}
END_TEST

START_TEST(test_write_updates_size_and_times)
{
	struct stat st_pre, st_post;
	ck_assert_int_eq(reffs_fuse_mkdir("/d", 0755), 0);
	ck_assert_int_eq(reffs_fuse_create("/d/f", S_IFREG | 0644, NULL), 0);
	ck_assert_int_eq(reffs_fuse_getattr("/d/f", &st_pre), 0);

	sleep_past(&st_pre.st_mtim);

	ck_assert_int_eq(reffs_fuse_write("/d/f", "hello", 5, 0, NULL), 5);
	ck_assert_int_eq(reffs_fuse_getattr("/d/f", &st_post), 0);

	ck_assert_uint_eq(st_pre.st_ino, st_post.st_ino);
	ck_assert_int_eq(st_post.st_size, 5);
	ck_assert_timespec_eq(st_pre.st_atim, st_post.st_atim);
	ck_assert_timespec_lt(st_pre.st_mtim, st_post.st_mtim);
	ck_assert_timespec_lt(st_pre.st_ctim, st_post.st_ctim);

	ck_assert_int_eq(reffs_fuse_unlink("/d/f"), 0);
	ck_assert_int_eq(reffs_fuse_rmdir("/d"), 0);
}
END_TEST

START_TEST(test_write_to_dir_eisdir)
{
	ck_assert_int_eq(reffs_fuse_mkdir("/d", 0755), 0);
	ck_assert_int_eq(reffs_fuse_write("/d", "hello", 5, 0, NULL), -EISDIR);
	ck_assert_int_eq(reffs_fuse_rmdir("/d"), 0);
}
END_TEST

START_TEST(test_read_returns_data_and_updates_atime)
{
	char buf[BUF_LEN];
	struct stat st_pre, st_post;

	ck_assert_int_eq(reffs_fuse_create("/f", S_IFREG | 0644, NULL), 0);
	ck_assert_int_eq(reffs_fuse_write("/f", "hello", 5, 0, NULL), 5);
	ck_assert_int_eq(reffs_fuse_getattr("/f", &st_pre), 0);

	usleep(1000);
	memset(buf, 0, sizeof(buf));
	ck_assert_int_eq(reffs_fuse_read("/f", buf, 5, 0, NULL), 5);
	ck_assert_str_eq(buf, "hello");

	ck_assert_int_eq(reffs_fuse_getattr("/f", &st_post), 0);
	ck_assert_timespec_eq(st_pre.st_mtim, st_post.st_mtim);
	ck_assert_timespec_eq(st_pre.st_ctim, st_post.st_ctim);
	ck_assert_timespec_lt(st_pre.st_atim, st_post.st_atim);

	ck_assert_int_eq(reffs_fuse_unlink("/f"), 0);
}
END_TEST

START_TEST(test_read_past_eof_eoverflow)
{
	char buf[BUF_LEN];
	ck_assert_int_eq(reffs_fuse_create("/f", S_IFREG | 0644, NULL), 0);
	ck_assert_int_eq(reffs_fuse_write("/f", "hello", 5, 0, NULL), 5);
	ck_assert_int_eq(reffs_fuse_read("/f", buf, 5, 10, NULL), -EOVERFLOW);
	ck_assert_int_eq(reffs_fuse_unlink("/f"), 0);
}
END_TEST

START_TEST(test_append_write_extends_size)
{
	char buf[BUF_LEN];
	struct stat st;

	ck_assert_int_eq(reffs_fuse_create("/f", S_IFREG | 0644, NULL), 0);
	ck_assert_int_eq(reffs_fuse_write("/f", "hello", 5, 0, NULL), 5);

	usleep(1000);
	ck_assert_int_eq(reffs_fuse_write("/f", "hello", 5, 5, NULL), 5);
	ck_assert_int_eq(reffs_fuse_getattr("/f", &st), 0);
	ck_assert_int_eq(st.st_size, 10);

	memset(buf, 0, sizeof(buf));
	ck_assert_int_eq(reffs_fuse_read("/f", buf, 10, 0, NULL), 10);
	ck_assert_str_eq(buf, "hellohello");

	ck_assert_int_eq(reffs_fuse_unlink("/f"), 0);
}
END_TEST

START_TEST(test_unlink_removes_file)
{
	struct stat st;
	ck_assert_int_eq(reffs_fuse_create("/f", S_IFREG | 0644, NULL), 0);
	ck_assert_int_eq(reffs_fuse_unlink("/f"), 0);
	ck_assert_int_eq(reffs_fuse_getattr("/f", &st), -ENOENT);
}
END_TEST

START_TEST(test_unlink_dir_eisdir)
{
	ck_assert_int_eq(reffs_fuse_mkdir("/d", 0755), 0);
	ck_assert_int_eq(reffs_fuse_unlink("/d"), -EISDIR);
	ck_assert_int_eq(reffs_fuse_rmdir("/d"), 0);
}
END_TEST

Suite *fuse_suite(void)
{
	Suite *s = suite_create("fuse 4: file create/write/read");
	TCase *tc = tcase_create("Core");
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_create_initial_attrs);
	tcase_add_test(tc, test_create_eisdir_on_dir_mode);
	tcase_add_test(tc, test_write_updates_size_and_times);
	tcase_add_test(tc, test_write_to_dir_eisdir);
	tcase_add_test(tc, test_read_returns_data_and_updates_atime);
	tcase_add_test(tc, test_read_past_eof_eoverflow);
	tcase_add_test(tc, test_append_write_extends_size);
	tcase_add_test(tc, test_unlink_removes_file);
	tcase_add_test(tc, test_unlink_dir_eisdir);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int number_failed;
	fuse_test_global_init();
	SRunner *sr = srunner_create(fuse_suite());
	srunner_set_fork_status(sr, CK_NOFORK);
	srunner_run_all(sr, CK_NORMAL);
	number_failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	fuse_test_global_fini();
	return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
