/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * layout_shard_size_test.c -- unit tests for the shard-length to
 * file-length conversion used by the reflected-GETATTR paths.
 *
 * The metadata server learns a file's length from what the data
 * servers report for their own shard files.  For a mirrored layout
 * that number IS the file length; for an erasure-coded one it is the
 * file length divided by the number of data shards, and taking it at
 * face value under-reports the file by a factor of k.  The conversion
 * is a pure function of the layout geometry, so it is tested here
 * rather than through the two async fan-out resume paths that call
 * it (nfs4_op_layoutreturn_resume and nfs4_op_layout_wcc).
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdint.h>

#include <check.h>

#include "reffs/layout_segment.h"
#include "libreffs_test.h"

/*
 * Forward decl of the helper under test.  Defined non-static in
 * lib/nfs4/server/layout.c; declared here rather than in a header
 * for one symbol, matching default_coding_dispatch_test.c.
 */
int64_t layout_shard_to_file_size(const struct layout_segment *seg,
				  int64_t max_shard);

/* ------------------------------------------------------------------ */
/* Mirrored geometries -- the shard is the whole file                  */
/* ------------------------------------------------------------------ */

/*
 * ls_m == 0 means no parity shards, which covers both a replicated
 * layout and the legacy PASSTHROUGH geometry.  Neither multiplies.
 */
START_TEST(test_replicated_shard_is_file_size)
{
	struct layout_segment seg = {
		.ls_k = 3,
		.ls_m = 0,
		.ls_nfiles = 3,
		.ls_stripe_unit = 4096,
	};

	ck_assert_int_eq(layout_shard_to_file_size(&seg, 1000), 1000);
}
END_TEST

/*
 * A single-shard layout has nothing to spread across, so even with
 * parity declared the shard length is the file length.
 */
START_TEST(test_single_data_shard_not_scaled)
{
	struct layout_segment seg = {
		.ls_k = 1,
		.ls_m = 1,
		.ls_nfiles = 2,
		.ls_stripe_unit = 4096,
	};

	ck_assert_int_eq(layout_shard_to_file_size(&seg, 1000), 1000);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Erasure-coded geometries -- scale by the data-shard count           */
/* ------------------------------------------------------------------ */

/*
 * The measured regression case, 2026-08-09: a 262144-byte write over
 * rs 4+2 with a 4096 striping unit leaves every shard at 65536, and
 * the metadata server used to publish 65536 as the file size.
 */
START_TEST(test_rs_4_2_scales_by_k)
{
	struct layout_segment seg = {
		.ls_k = 4,
		.ls_m = 2,
		.ls_nfiles = 6,
		.ls_stripe_unit = 4096,
	};

	ck_assert_int_eq(layout_shard_to_file_size(&seg, 65536), 262144);
}
END_TEST

/* A wider geometry scales by its own k, not a hard-coded one. */
START_TEST(test_rs_8_2_scales_by_k)
{
	struct layout_segment seg = {
		.ls_k = 8,
		.ls_m = 2,
		.ls_nfiles = 10,
		.ls_stripe_unit = 4096,
	};

	ck_assert_int_eq(layout_shard_to_file_size(&seg, 4096), 32768);
}
END_TEST

/*
 * A file whose last stripe is partial is padded out before encoding
 * and nothing on the wire records how much of it was real, so the
 * answer is a whole number of stripes -- an upper bound, never short.
 * 100000 bytes over rs 4+2 with a 4096 unit is 7 stripes (114688),
 * so each shard holds 7 * 4096 = 28672.
 */
START_TEST(test_partial_stripe_rounds_up_not_down)
{
	struct layout_segment seg = {
		.ls_k = 4,
		.ls_m = 2,
		.ls_nfiles = 6,
		.ls_stripe_unit = 4096,
	};
	int64_t got = layout_shard_to_file_size(&seg, 28672);

	ck_assert_int_eq(got, 114688);
	ck_assert(got >= 100000);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Degenerate inputs                                                   */
/* ------------------------------------------------------------------ */

/* An empty file stays empty rather than becoming k * 0 by accident. */
START_TEST(test_zero_shard_stays_zero)
{
	struct layout_segment seg = {
		.ls_k = 4,
		.ls_m = 2,
		.ls_nfiles = 6,
	};

	ck_assert_int_eq(layout_shard_to_file_size(&seg, 0), 0);
}
END_TEST

/*
 * No data server reported (every shard stale) leaves max_shard at its
 * caller-side initial value.  Pass it through untouched so the caller's
 * "only ever grow" guard sees the same value it started with.
 */
START_TEST(test_negative_shard_passes_through)
{
	struct layout_segment seg = {
		.ls_k = 4,
		.ls_m = 2,
		.ls_nfiles = 6,
	};

	ck_assert_int_eq(layout_shard_to_file_size(&seg, -1), -1);
}
END_TEST

/* A missing segment must not be dereferenced. */
START_TEST(test_null_segment_passes_through)
{
	ck_assert_int_eq(layout_shard_to_file_size(NULL, 8192), 8192);
}
END_TEST

/*
 * A data server reporting an absurd shard length must not wrap the
 * multiply into a negative size.  Saturate instead.
 */
START_TEST(test_overflow_saturates)
{
	struct layout_segment seg = {
		.ls_k = 4,
		.ls_m = 2,
		.ls_nfiles = 6,
	};

	ck_assert_int_eq(layout_shard_to_file_size(&seg, INT64_MAX), INT64_MAX);
}
END_TEST

/* ------------------------------------------------------------------ */

static Suite *layout_shard_size_suite(void)
{
	Suite *s = suite_create("layout_shard_size");

	TCase *tc_mirror = tcase_create("mirrored");
	tcase_add_test(tc_mirror, test_replicated_shard_is_file_size);
	tcase_add_test(tc_mirror, test_single_data_shard_not_scaled);
	suite_add_tcase(s, tc_mirror);

	TCase *tc_ec = tcase_create("erasure_coded");
	tcase_add_test(tc_ec, test_rs_4_2_scales_by_k);
	tcase_add_test(tc_ec, test_rs_8_2_scales_by_k);
	tcase_add_test(tc_ec, test_partial_stripe_rounds_up_not_down);
	suite_add_tcase(s, tc_ec);

	TCase *tc_edge = tcase_create("degenerate");
	tcase_add_test(tc_edge, test_zero_shard_stays_zero);
	tcase_add_test(tc_edge, test_negative_shard_passes_through);
	tcase_add_test(tc_edge, test_null_segment_passes_through);
	tcase_add_test(tc_edge, test_overflow_saturates);
	suite_add_tcase(s, tc_edge);

	return s;
}

int main(void)
{
	return reffs_test_run_suite(layout_shard_size_suite(), NULL, NULL);
}
