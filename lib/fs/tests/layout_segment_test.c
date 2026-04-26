/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Unit tests for layout segment persistence.
 *
 * Tests:
 *   1. alloc/free: basic container lifecycle
 *   2. add segment: add a segment with data files, verify fields
 *   3. persist/load: sync to POSIX disk, reload, verify round-trip
 *   4. multiple segments: persist 2 segments (continuations), verify
 *   5. dstore_id persisted: verify dstore ID (not name) round-trips
 */

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <limits.h>

#include <check.h>

#include "reffs/inode.h"
#include "reffs/layout_segment.h"
#include "reffs/super_block.h"
#include "reffs/fs.h"
#include "reffs/log.h"
#include "nfsv42_xdr.h"
#include "fs_test_harness.h"

/* Persistence tests need a POSIX-backed superblock. */
static struct super_block *g_posix_sb;
static char g_tmpdir[PATH_MAX];

static void setup_mem(void)
{
	reffs_test_setup_server();
	fs_test_setup();
}

static void teardown_mem(void)
{
	fs_test_teardown();
	reffs_test_teardown_server();
}

static void setup_posix(void)
{
	char tmpl[] = "/tmp/reffs_layout_test_XXXXXX";

	reffs_test_setup_server();

	ck_assert_ptr_nonnull(mkdtemp(tmpl));
	strncpy(g_tmpdir, tmpl, sizeof(g_tmpdir));

	char sb_path[PATH_MAX];

	snprintf(sb_path, sizeof(sb_path), "%s/sb_1", g_tmpdir);
	mkdir(sb_path, 0755);

	g_posix_sb = super_block_alloc(SUPER_BLOCK_ROOT_ID, "/",
				       REFFS_STORAGE_POSIX, g_tmpdir);
	ck_assert_ptr_nonnull(g_posix_sb);
	super_block_dirent_create(g_posix_sb, NULL, reffs_life_action_birth);
}

static void teardown_posix(void)
{
	if (g_posix_sb) {
		super_block_release_dirents(g_posix_sb);
		super_block_put(g_posix_sb);
		g_posix_sb = NULL;
	}

	char cmd[PATH_MAX];

	snprintf(cmd, sizeof(cmd), "rm -rf %s", g_tmpdir);
	system(cmd);

	reffs_test_teardown_server();
}

/* Helper: create a data file entry with test values. */
static struct layout_data_file make_data_file(uint32_t dstore_id,
					      uint8_t fh_val, int64_t size)
{
	struct layout_data_file ldf = { 0 };

	ldf.ldf_dstore_id = dstore_id;
	ldf.ldf_fh_len = 8;
	memset(ldf.ldf_fh, fh_val, 8);
	ldf.ldf_size = size;
	ldf.ldf_uid = 1000;
	ldf.ldf_gid = 1000;
	ldf.ldf_mode = 0644;
	clock_gettime(CLOCK_REALTIME, &ldf.ldf_mtime);
	ldf.ldf_atime = ldf.ldf_mtime;
	ldf.ldf_ctime = ldf.ldf_mtime;
	return ldf;
}

/* ------------------------------------------------------------------ */
/* In-memory lifecycle tests                                           */
/* ------------------------------------------------------------------ */

START_TEST(test_alloc_free)
{
	struct layout_segments *lss = layout_segments_alloc();

	ck_assert_ptr_nonnull(lss);
	ck_assert_uint_eq(lss->lss_count, 0);
	ck_assert_ptr_null(lss->lss_segs);

	layout_segments_free(lss);
	layout_segments_free(NULL); /* NULL-safe */
}
END_TEST

START_TEST(test_add_segment)
{
	struct layout_segments *lss = layout_segments_alloc();

	ck_assert_ptr_nonnull(lss);

	struct layout_data_file *files =
		calloc(2, sizeof(struct layout_data_file));

	files[0] = make_data_file(10, 0xAA, 4096);
	files[1] = make_data_file(20, 0xBB, 4096);

	struct layout_segment seg = {
		.ls_offset = 0,
		.ls_length = 0,
		.ls_stripe_unit = 0,
		.ls_k = 2,
		.ls_m = 1,
		.ls_nfiles = 2,
		.ls_layout_type = LAYOUT4_FLEX_FILES_V2,
		.ls_files = files,
	};

	ck_assert_int_eq(layout_segments_add(lss, &seg), 0);
	ck_assert_uint_eq(lss->lss_count, 1);
	ck_assert_uint_eq(lss->lss_segs[0].ls_k, 2);
	ck_assert_uint_eq(lss->lss_segs[0].ls_m, 1);
	ck_assert_uint_eq(lss->lss_segs[0].ls_nfiles, 2);
	ck_assert_uint_eq(lss->lss_segs[0].ls_layout_type,
			  LAYOUT4_FLEX_FILES_V2);
	ck_assert_uint_eq(lss->lss_segs[0].ls_files[0].ldf_dstore_id, 10);
	ck_assert_uint_eq(lss->lss_segs[0].ls_files[1].ldf_dstore_id, 20);

	layout_segments_free(lss);
}
END_TEST

/* ------------------------------------------------------------------ */
/* POSIX persistence tests                                             */
/* ------------------------------------------------------------------ */

START_TEST(test_persist_load)
{
	struct inode *inode = inode_alloc(g_posix_sb, 500);

	ck_assert_ptr_nonnull(inode);

	struct layout_segments *lss = layout_segments_alloc();
	struct layout_data_file *files =
		calloc(3, sizeof(struct layout_data_file));

	files[0] = make_data_file(100, 0x11, 8192);
	files[1] = make_data_file(200, 0x22, 8192);
	files[2] = make_data_file(300, 0x33, 8192);

	struct layout_segment seg = {
		.ls_offset = 0,
		.ls_length = 0,
		.ls_stripe_unit = 65536,
		.ls_k = 2,
		.ls_m = 1,
		.ls_nfiles = 3,
		.ls_layout_type = LAYOUT4_FLEX_FILES_V2,
		.ls_files = files,
	};

	layout_segments_add(lss, &seg);
	inode->i_layout_segments = lss;

	/* Persist to disk. */
	inode_sync_to_disk(inode);

	/* Clear in-memory, re-load via backend hook. */
	layout_segments_free(inode->i_layout_segments);
	inode->i_layout_segments = NULL;

	ck_assert_int_eq(g_posix_sb->sb_ops->inode_alloc(inode), 0);

	ck_assert_ptr_nonnull(inode->i_layout_segments);
	ck_assert_uint_eq(inode->i_layout_segments->lss_count, 1);

	struct layout_segment *ls = &inode->i_layout_segments->lss_segs[0];

	ck_assert_uint_eq(ls->ls_stripe_unit, 65536);
	ck_assert_uint_eq(ls->ls_k, 2);
	ck_assert_uint_eq(ls->ls_m, 1);
	ck_assert_uint_eq(ls->ls_nfiles, 3);
	ck_assert_uint_eq(ls->ls_layout_type, LAYOUT4_FLEX_FILES_V2);

	/* Verify dstore IDs survived the round-trip. */
	ck_assert_uint_eq(ls->ls_files[0].ldf_dstore_id, 100);
	ck_assert_uint_eq(ls->ls_files[1].ldf_dstore_id, 200);
	ck_assert_uint_eq(ls->ls_files[2].ldf_dstore_id, 300);

	/* Verify FHs. */
	for (int i = 0; i < 8; i++) {
		ck_assert_uint_eq(ls->ls_files[0].ldf_fh[i], 0x11);
		ck_assert_uint_eq(ls->ls_files[1].ldf_fh[i], 0x22);
		ck_assert_uint_eq(ls->ls_files[2].ldf_fh[i], 0x33);
	}

	/* Verify cached attrs. */
	ck_assert_int_eq(ls->ls_files[0].ldf_size, 8192);
	ck_assert_uint_eq(ls->ls_files[0].ldf_uid, 1000);
	ck_assert_uint_eq(ls->ls_files[0].ldf_mode, 0644);

	inode_active_put(inode);
}
END_TEST

START_TEST(test_multiple_segments)
{
	struct inode *inode = inode_alloc(g_posix_sb, 501);

	ck_assert_ptr_nonnull(inode);

	struct layout_segments *lss = layout_segments_alloc();

	/* Segment 0: bytes 0-1M */
	struct layout_data_file *f1 =
		calloc(2, sizeof(struct layout_data_file));

	f1[0] = make_data_file(10, 0xAA, 524288);
	f1[1] = make_data_file(20, 0xBB, 524288);

	struct layout_segment seg1 = {
		.ls_offset = 0,
		.ls_length = 1048576,
		.ls_stripe_unit = 65536,
		.ls_k = 1,
		.ls_m = 1,
		.ls_nfiles = 2,
		.ls_layout_type = LAYOUT4_FLEX_FILES,
		.ls_files = f1,
	};

	layout_segments_add(lss, &seg1);

	/* Segment 1: bytes 1M+ (continuation). */
	struct layout_data_file *f2 =
		calloc(2, sizeof(struct layout_data_file));

	f2[0] = make_data_file(30, 0xCC, 0);
	f2[1] = make_data_file(40, 0xDD, 0);

	struct layout_segment seg2 = {
		.ls_offset = 1048576,
		.ls_length = 0,
		.ls_stripe_unit = 131072,
		.ls_k = 1,
		.ls_m = 1,
		.ls_nfiles = 2,
		.ls_layout_type = LAYOUT4_FLEX_FILES_V2,
		.ls_files = f2,
	};

	layout_segments_add(lss, &seg2);
	inode->i_layout_segments = lss;

	inode_sync_to_disk(inode);

	layout_segments_free(inode->i_layout_segments);
	inode->i_layout_segments = NULL;

	ck_assert_int_eq(g_posix_sb->sb_ops->inode_alloc(inode), 0);

	ck_assert_ptr_nonnull(inode->i_layout_segments);
	ck_assert_uint_eq(inode->i_layout_segments->lss_count, 2);

	struct layout_segment *s0 = &inode->i_layout_segments->lss_segs[0];

	ck_assert_uint_eq(s0->ls_offset, 0);
	ck_assert_uint_eq(s0->ls_length, 1048576);
	ck_assert_uint_eq(s0->ls_layout_type, LAYOUT4_FLEX_FILES);
	ck_assert_uint_eq(s0->ls_files[0].ldf_dstore_id, 10);
	ck_assert_uint_eq(s0->ls_files[1].ldf_dstore_id, 20);

	struct layout_segment *s1 = &inode->i_layout_segments->lss_segs[1];

	ck_assert_uint_eq(s1->ls_offset, 1048576);
	ck_assert_uint_eq(s1->ls_length, 0);
	ck_assert_uint_eq(s1->ls_stripe_unit, 131072);
	ck_assert_uint_eq(s1->ls_layout_type, LAYOUT4_FLEX_FILES_V2);
	ck_assert_uint_eq(s1->ls_files[0].ldf_dstore_id, 30);
	ck_assert_uint_eq(s1->ls_files[1].ldf_dstore_id, 40);

	inode_active_put(inode);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Slice B' tests: lss_gen counter                                      */
/* ------------------------------------------------------------------ */

START_TEST(test_lss_gen_starts_at_zero)
{
	struct layout_segments *lss = layout_segments_alloc();

	ck_assert_ptr_nonnull(lss);
	ck_assert_uint_eq(
		atomic_load_explicit(&lss->lss_gen, memory_order_relaxed), 0);

	layout_segments_free(lss);
}
END_TEST

START_TEST(test_lss_gen_bumps_on_add)
{
	struct layout_segments *lss = layout_segments_alloc();

	ck_assert_ptr_nonnull(lss);

	uint64_t gen_pre =
		atomic_load_explicit(&lss->lss_gen, memory_order_relaxed);

	struct layout_data_file *files =
		calloc(1, sizeof(struct layout_data_file));

	files[0] = make_data_file(7, 0xAB, 1024);

	struct layout_segment seg = {
		.ls_offset = 0,
		.ls_length = 0,
		.ls_stripe_unit = 0,
		.ls_k = 1,
		.ls_m = 0,
		.ls_nfiles = 1,
		.ls_layout_type = LAYOUT4_FLEX_FILES,
		.ls_files = files,
	};

	ck_assert_int_eq(layout_segments_add(lss, &seg), 0);

	uint64_t gen_post =
		atomic_load_explicit(&lss->lss_gen, memory_order_relaxed);

	ck_assert_uint_gt(gen_post, gen_pre);

	/* Second add bumps again. */
	struct layout_data_file *files2 =
		calloc(1, sizeof(struct layout_data_file));

	files2[0] = make_data_file(8, 0xCD, 2048);
	seg.ls_files = files2;

	ck_assert_int_eq(layout_segments_add(lss, &seg), 0);

	uint64_t gen_post2 =
		atomic_load_explicit(&lss->lss_gen, memory_order_relaxed);

	ck_assert_uint_gt(gen_post2, gen_post);

	layout_segments_free(lss);
}
END_TEST

START_TEST(test_lss_gen_persists_across_inode_sync)
{
	struct inode *inode = inode_alloc(g_posix_sb, 600);

	ck_assert_ptr_nonnull(inode);

	struct layout_segments *lss = layout_segments_alloc();
	struct layout_data_file *files =
		calloc(2, sizeof(struct layout_data_file));

	files[0] = make_data_file(50, 0xEE, 4096);
	files[1] = make_data_file(60, 0xFF, 4096);

	struct layout_segment seg = {
		.ls_offset = 0,
		.ls_length = 0,
		.ls_stripe_unit = 0,
		.ls_k = 2,
		.ls_m = 0,
		.ls_nfiles = 2,
		.ls_layout_type = LAYOUT4_FLEX_FILES,
		.ls_files = files,
	};

	layout_segments_add(lss, &seg);
	inode->i_layout_segments = lss;

	uint64_t gen_pre =
		atomic_load_explicit(&lss->lss_gen, memory_order_relaxed);

	ck_assert_uint_gt(gen_pre, 0);

	/* Persist + reload via backend. */
	inode_sync_to_disk(inode);
	layout_segments_free(inode->i_layout_segments);
	inode->i_layout_segments = NULL;

	ck_assert_int_eq(g_posix_sb->sb_ops->inode_alloc(inode), 0);
	ck_assert_ptr_nonnull(inode->i_layout_segments);

	uint64_t gen_loaded = atomic_load_explicit(
		&inode->i_layout_segments->lss_gen, memory_order_relaxed);

	ck_assert_uint_eq(gen_loaded, gen_pre);

	inode_active_put(inode);
}
END_TEST

/* ------------------------------------------------------------------ */

Suite *layout_segment_suite(void)
{
	Suite *s = suite_create("Layout Segment Persistence");

	TCase *tc_mem = tcase_create("lifecycle");

	tcase_add_checked_fixture(tc_mem, setup_mem, teardown_mem);
	tcase_add_test(tc_mem, test_alloc_free);
	tcase_add_test(tc_mem, test_add_segment);
	tcase_add_test(tc_mem, test_lss_gen_starts_at_zero);
	tcase_add_test(tc_mem, test_lss_gen_bumps_on_add);
	suite_add_tcase(s, tc_mem);

	TCase *tc_posix = tcase_create("persistence");

	tcase_add_checked_fixture(tc_posix, setup_posix, teardown_posix);
	tcase_add_test(tc_posix, test_persist_load);
	tcase_add_test(tc_posix, test_multiple_segments);
	tcase_add_test(tc_posix, test_lss_gen_persists_across_inode_sync);
	suite_add_tcase(s, tc_posix);

	return s;
}

int main(void)
{
	return fs_test_run(layout_segment_suite());
}
