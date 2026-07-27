/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * trace_archive_test.c -- unit tests for trace archive retention
 * (reffs_trace_archive_prune() in lib/trace/common.c).
 *
 * Rotation used to keep every compressed archive forever.  On
 * 2026-07-27 that produced 1442 archives (43 GB) in 13.7 hours on
 * reffs.ci, filled the root filesystem, and wedged the nightly for
 * 13 hours.  These tests pin the retention behaviour that bounds it.
 *
 * Each test builds a scratch directory of fake archives with known
 * sizes and mtimes, then asserts which survive a prune.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <utime.h>

#include <check.h>

#include "reffs/trace/common.h"

static char g_dir[256];

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void make_file(const char *name, size_t bytes, time_t mtime)
{
	char path[512];
	struct utimbuf tb;
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", g_dir, name);
	f = fopen(path, "wb");
	ck_assert_ptr_nonnull(f);
	for (size_t i = 0; i < bytes; i++)
		fputc('x', f);
	fclose(f);

	tb.actime = mtime;
	tb.modtime = mtime;
	ck_assert_int_eq(utime(path, &tb), 0);
}

static bool file_exists(const char *name)
{
	char path[512];
	struct stat st;

	snprintf(path, sizeof(path), "%s/%s", g_dir, name);
	return stat(path, &st) == 0;
}

static char *trace_path(void)
{
	static char buf[512];

	snprintf(buf, sizeof(buf), "%s/reffsd.log", g_dir);
	return buf;
}

static void setup(void)
{
	snprintf(g_dir, sizeof(g_dir), "/tmp/reffs_trace_test.XXXXXX");
	ck_assert_ptr_nonnull(mkdtemp(g_dir));
}

static void teardown(void)
{
	char cmd[512];

	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
	if (system(cmd) != 0)
		fprintf(stderr, "warning: failed to clean %s\n", g_dir);
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

/*
 * Under the cap, nothing is dropped.  Guards against a prune that is
 * over-eager and throws away history it did not need to.
 */
START_TEST(test_prune_under_cap_keeps_all)
{
	make_file("reffsd-20260727-010000.log.zst", 100, 1000);
	make_file("reffsd-20260727-020000.log.zst", 100, 2000);

	reffs_trace_archive_prune(trace_path(), 1000);

	ck_assert(file_exists("reffsd-20260727-010000.log.zst"));
	ck_assert(file_exists("reffsd-20260727-020000.log.zst"));
}
END_TEST

/*
 * Over the cap, the OLDEST archives go first and the newest survive.
 * Cap of 250 with three 100-byte archives keeps two.
 */
START_TEST(test_prune_drops_oldest_first)
{
	make_file("reffsd-20260727-010000.log.zst", 100, 1000); /* oldest */
	make_file("reffsd-20260727-020000.log.zst", 100, 2000);
	make_file("reffsd-20260727-030000.log.zst", 100, 3000); /* newest */

	reffs_trace_archive_prune(trace_path(), 250);

	ck_assert(!file_exists("reffsd-20260727-010000.log.zst"));
	ck_assert(file_exists("reffsd-20260727-020000.log.zst"));
	ck_assert(file_exists("reffsd-20260727-030000.log.zst"));
}
END_TEST

/*
 * The live trace file must never be pruned -- deleting the log reffsd
 * is actively writing would lose the very trace being captured.  It is
 * "reffsd.log" with no "-TIMESTAMP", so it must not match.
 */
START_TEST(test_prune_never_touches_live_log)
{
	make_file("reffsd.log", 5000, 9000);
	make_file("reffsd-20260727-010000.log.zst", 100, 1000);

	/* Cap of 1 byte: prune everything it is allowed to prune. */
	reffs_trace_archive_prune(trace_path(), 1);

	ck_assert(file_exists("reffsd.log"));
	ck_assert(!file_exists("reffsd-20260727-010000.log.zst"));
}
END_TEST

/*
 * Unrelated files in the same directory are not ours to delete.  reffsd
 * runs with its trace log in the repo root, which is full of files that
 * matter.
 */
START_TEST(test_prune_ignores_foreign_files)
{
	make_file("reffsd-20260727-010000.log.zst", 100, 1000);
	make_file("other-20260727-010000.log.zst", 100, 1000);
	make_file("reffsd-20260727-010000.txt", 100, 1000);
	make_file("configure.ac", 100, 1000);

	reffs_trace_archive_prune(trace_path(), 1);

	ck_assert(!file_exists("reffsd-20260727-010000.log.zst"));
	ck_assert(file_exists("other-20260727-010000.log.zst"));
	ck_assert(file_exists("reffsd-20260727-010000.txt"));
	ck_assert(file_exists("configure.ac"));
}
END_TEST

/*
 * Uncompressed ".log" rotations count too.  When the compress queue is
 * full, rotate falls back to inline compression, and a killed reffsd
 * can leave raw .log rotations behind -- both consume disk.
 */
START_TEST(test_prune_covers_uncompressed_rotations)
{
	make_file("reffsd-20260727-010000.log", 100, 1000);
	make_file("reffsd-20260727-020000.log.zst", 100, 2000);

	reffs_trace_archive_prune(trace_path(), 150);

	ck_assert(!file_exists("reffsd-20260727-010000.log"));
	ck_assert(file_exists("reffsd-20260727-020000.log.zst"));
}
END_TEST

/*
 * max_bytes == 0 is the documented escape hatch: pruning disabled.
 */
START_TEST(test_prune_zero_cap_disables)
{
	make_file("reffsd-20260727-010000.log.zst", 100, 1000);

	reffs_trace_archive_prune(trace_path(), 0);

	ck_assert(file_exists("reffsd-20260727-010000.log.zst"));
}
END_TEST

/*
 * A NULL trace path (reffs_trace_init(NULL) -- the unit-test harness
 * does exactly this) must be a no-op, not a crash.
 */
START_TEST(test_prune_null_path_is_noop)
{
	reffs_trace_archive_prune(NULL, 1000);
}
END_TEST

/*
 * A bare relative trace name has no '/' to split on; the prune must
 * fall back to "." rather than mis-parsing.  reffsd is invoked this way
 * on reffs.ci, where the archives landed as "./reffsd-*.log.zst".
 */
START_TEST(test_prune_relative_path_uses_cwd)
{
	char cwd[512];

	ck_assert_ptr_nonnull(getcwd(cwd, sizeof(cwd)));
	ck_assert_int_eq(chdir(g_dir), 0);

	make_file("reffsd-20260727-010000.log.zst", 100, 1000);
	make_file("reffsd-20260727-020000.log.zst", 100, 2000);

	reffs_trace_archive_prune("reffsd.log", 150);

	ck_assert(!file_exists("reffsd-20260727-010000.log.zst"));
	ck_assert(file_exists("reffsd-20260727-020000.log.zst"));

	ck_assert_int_eq(chdir(cwd), 0);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

static Suite *trace_archive_suite(void)
{
	Suite *s = suite_create("trace_archive");
	TCase *tc = tcase_create("prune");

	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_prune_under_cap_keeps_all);
	tcase_add_test(tc, test_prune_drops_oldest_first);
	tcase_add_test(tc, test_prune_never_touches_live_log);
	tcase_add_test(tc, test_prune_ignores_foreign_files);
	tcase_add_test(tc, test_prune_covers_uncompressed_rotations);
	tcase_add_test(tc, test_prune_zero_cap_disables);
	tcase_add_test(tc, test_prune_null_path_is_noop);
	tcase_add_test(tc, test_prune_relative_path_uses_cwd);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	SRunner *sr = srunner_create(trace_archive_suite());

	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
