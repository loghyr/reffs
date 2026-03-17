/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/*
 * fs_test_utimensat.c — reffs_fs_utimensat() correctness
 *
 * utimensat sets both atime (times[0]) and mtime (times[1]) to the exact
 * caller-supplied values, bypassing clock_gettime().  This is used by NFS
 * clients to replay server-assigned timestamps, so precision matters.
 *
 * Tests:
 *  - After utimensat, getattr returns exactly the supplied atime and mtime
 *  - ino, uid, gid, mode, size, nlink are unchanged
 *  - Works on both regular files and directories
 *  - Applying utimensat to a non-existent path returns -ENOENT
 *  - Applying utimensat with nanosecond-resolution values round-trips cleanly
 */

#include "fs_test_harness.h"

static void setup(void)
{
	fs_test_setup();
}
static void teardown(void)
{
	fs_test_teardown();
}

/* ------------------------------------------------------------------ */

START_TEST(test_utimensat_file_times_stored)
{
	struct timespec times[2];
	struct stat st_pre, st_post;

	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_getattr("/f", &st_pre), 0);

	times[0].tv_sec = 1000000000; /* atime */
	times[0].tv_nsec = 123456789;
	times[1].tv_sec = 2000000000; /* mtime */
	times[1].tv_nsec = 987654321;

	ck_assert_int_eq(reffs_fs_utimensat("/f", times), 0);
	ck_assert_int_eq(reffs_fs_getattr("/f", &st_post), 0);

	ck_assert_int_eq((long)st_post.st_atim.tv_sec, 1000000000);
	ck_assert_int_eq((long)st_post.st_atim.tv_nsec, 123456789);
	ck_assert_int_eq((long)st_post.st_mtim.tv_sec, 2000000000);
	ck_assert_int_eq((long)st_post.st_mtim.tv_nsec, 987654321);

	/* identity unchanged */
	ck_assert_uint_eq(st_pre.st_ino, st_post.st_ino);
	ck_assert_uint_eq(st_pre.st_uid, st_post.st_uid);
	ck_assert_uint_eq(st_pre.st_gid, st_post.st_gid);
	ck_assert_uint_eq(st_pre.st_mode, st_post.st_mode);
	ck_assert_uint_eq(st_pre.st_nlink, st_post.st_nlink);
	ck_assert_int_eq((long)st_pre.st_size, (long)st_post.st_size);

	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
}
END_TEST

START_TEST(test_utimensat_dir)
{
	struct timespec times[2];
	struct stat st;

	ck_assert_int_eq(reffs_fs_mkdir("/d", 0755), 0);

	times[0].tv_sec = 111;
	times[0].tv_nsec = 1;
	times[1].tv_sec = 222;
	times[1].tv_nsec = 2;

	ck_assert_int_eq(reffs_fs_utimensat("/d", times), 0);
	ck_assert_int_eq(reffs_fs_getattr("/d", &st), 0);

	ck_assert_int_eq((long)st.st_atim.tv_sec, 111);
	ck_assert_int_eq((long)st.st_atim.tv_nsec, 1);
	ck_assert_int_eq((long)st.st_mtim.tv_sec, 222);
	ck_assert_int_eq((long)st.st_mtim.tv_nsec, 2);

	ck_assert_int_eq(reffs_fs_rmdir("/d"), 0);
}
END_TEST

START_TEST(test_utimensat_enoent)
{
	struct timespec times[2] = { { 1, 0 }, { 2, 0 } };
	ck_assert_int_eq(reffs_fs_utimensat("/no_such", times), -ENOENT);
}
END_TEST

/*
 * utimensat with tv_nsec == 0 must store zero, not some leftover value.
 */
START_TEST(test_utimensat_nsec_zero)
{
	struct timespec times[2];
	struct stat st;

	ck_assert_int_eq(reffs_fs_create("/f", S_IFREG | 0644), 0);

	times[0].tv_sec = 500;
	times[0].tv_nsec = 0;
	times[1].tv_sec = 600;
	times[1].tv_nsec = 0;

	ck_assert_int_eq(reffs_fs_utimensat("/f", times), 0);
	ck_assert_int_eq(reffs_fs_getattr("/f", &st), 0);

	ck_assert_int_eq((long)st.st_atim.tv_nsec, 0);
	ck_assert_int_eq((long)st.st_mtim.tv_nsec, 0);

	ck_assert_int_eq(reffs_fs_unlink("/f"), 0);
}
END_TEST

/* ------------------------------------------------------------------ */

Suite *fs_utimensat_suite(void)
{
	Suite *s = suite_create("fs: utimensat");
	TCase *tc = tcase_create("Core");
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_utimensat_file_times_stored);
	tcase_add_test(tc, test_utimensat_dir);
	tcase_add_test(tc, test_utimensat_enoent);
	tcase_add_test(tc, test_utimensat_nsec_zero);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	return fs_test_run(fs_utimensat_suite());
}
