/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Pure-math tests for the partial-range stripe walk used by
 * ec_write_encoding_range / ec_read_encoding_range (Track 1b, see
 * .claude/design/chunk-collision-t1b.md).  The per-stripe
 * primitives ec_write_stripe_with_file / ec_read_stripe_with_file
 * are exercised against a live MDS by the existing PS pipeline
 * tests; what's new here is the byte-range -> stripe-list
 * arithmetic.  Replicating the formula in the test (the pattern
 * already used by ec_pipeline_stripe_test.c) avoids dragging an
 * MDS into a 2-second unit test.
 *
 * Test matrix: aligned vs unaligned start / end, single-stripe
 * vs multi-stripe, prefix-only, suffix-only, prefix-and-suffix
 * sandwich.  Each test asserts the per-stripe RMW flag matches
 * the "this stripe is not fully covered by the dirty range" rule.
 */

#include <check.h>
#include <stdint.h>
#include <stdlib.h>

#define K 4
#define SHARD_SIZE (4 * 1024)
#define STRIPE_DATA ((size_t)K * SHARD_SIZE) /* 16 KiB */

struct stripe_op {
	uint64_t stripe_no;
	size_t sub_off_in_stripe;
	size_t sub_off_in_data;
	size_t sub_len;
	int is_rmw; /* 1 if partial, 0 if fully-dirty */
};

/*
 * Mirror of the per-stripe overlap helper in ec_pipeline.c
 * (range_stripe_overlap + the is_rmw branch).  Walks the range
 * [offset, offset+length), one entry per stripe touched.
 * Returns the number of entries written into `out`; bounded by
 * `cap`.  Tests pass cap >= last_stripe - first_stripe + 1.
 */
static size_t range_walk(uint64_t offset, size_t length, struct stripe_op *out,
			 size_t cap)
{
	uint64_t end;
	uint64_t first_stripe;
	uint64_t last_stripe;
	size_t n = 0;

	if (length == 0)
		return 0;
	end = offset + (uint64_t)length;
	first_stripe = offset / (uint64_t)STRIPE_DATA;
	last_stripe = (end - 1) / (uint64_t)STRIPE_DATA;

	for (uint64_t s = first_stripe; s <= last_stripe && n < cap; s++) {
		uint64_t base = s * (uint64_t)STRIPE_DATA;
		uint64_t send = base + (uint64_t)STRIPE_DATA;
		uint64_t sub_start = base > offset ? base : offset;
		uint64_t sub_endx = send < end ? send : end;
		size_t sub_off_stripe = (size_t)(sub_start - base);
		size_t sub_len = (size_t)(sub_endx - sub_start);

		out[n].stripe_no = s;
		out[n].sub_off_in_stripe = sub_off_stripe;
		out[n].sub_off_in_data = (size_t)(sub_start - offset);
		out[n].sub_len = sub_len;
		out[n].is_rmw =
			!(sub_off_stripe == 0 && sub_len == STRIPE_DATA);
		n++;
	}
	return n;
}

/* ------------------------------------------------------------------ */
/* Aligned cases -- no RMW                                              */
/* ------------------------------------------------------------------ */

START_TEST(test_range_aligned_single_stripe)
{
	struct stripe_op ops[4];
	size_t n = range_walk(0, STRIPE_DATA, ops, 4);

	ck_assert_uint_eq(n, 1);
	ck_assert_uint_eq(ops[0].stripe_no, 0);
	ck_assert_uint_eq(ops[0].sub_off_in_stripe, 0);
	ck_assert_uint_eq(ops[0].sub_off_in_data, 0);
	ck_assert_uint_eq(ops[0].sub_len, STRIPE_DATA);
	ck_assert_int_eq(ops[0].is_rmw, 0);
}
END_TEST

START_TEST(test_range_aligned_multi_stripe)
{
	struct stripe_op ops[8];
	size_t n = range_walk(0, 4 * STRIPE_DATA, ops, 8);

	ck_assert_uint_eq(n, 4);
	for (size_t i = 0; i < n; i++) {
		ck_assert_uint_eq(ops[i].stripe_no, i);
		ck_assert_uint_eq(ops[i].sub_off_in_stripe, 0);
		ck_assert_uint_eq(ops[i].sub_off_in_data, i * STRIPE_DATA);
		ck_assert_uint_eq(ops[i].sub_len, STRIPE_DATA);
		ck_assert_int_eq(ops[i].is_rmw, 0);
	}
}
END_TEST

START_TEST(test_range_aligned_offset_nonzero)
{
	/*
	 * Range starts at stripe 2 exactly, length spans 2 full
	 * stripes -- still no RMW.
	 */
	struct stripe_op ops[4];
	size_t n = range_walk(2 * STRIPE_DATA, 2 * STRIPE_DATA, ops, 4);

	ck_assert_uint_eq(n, 2);
	ck_assert_uint_eq(ops[0].stripe_no, 2);
	ck_assert_uint_eq(ops[1].stripe_no, 3);
	ck_assert_int_eq(ops[0].is_rmw, 0);
	ck_assert_int_eq(ops[1].is_rmw, 0);
	ck_assert_uint_eq(ops[1].sub_off_in_data, STRIPE_DATA);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Prefix RMW                                                          */
/* ------------------------------------------------------------------ */

START_TEST(test_range_prefix_only_partial)
{
	/*
	 * Range starts mid-stripe at 4 KiB, ends at the stripe
	 * boundary at 16 KiB -- prefix is partial, no suffix or
	 * interior stripes.  One RMW, one entry.
	 */
	struct stripe_op ops[4];
	size_t n = range_walk(4096, STRIPE_DATA - 4096, ops, 4);

	ck_assert_uint_eq(n, 1);
	ck_assert_uint_eq(ops[0].stripe_no, 0);
	ck_assert_uint_eq(ops[0].sub_off_in_stripe, 4096);
	ck_assert_uint_eq(ops[0].sub_off_in_data, 0);
	ck_assert_uint_eq(ops[0].sub_len, STRIPE_DATA - 4096);
	ck_assert_int_eq(ops[0].is_rmw, 1);
}
END_TEST

START_TEST(test_range_prefix_then_full_interior)
{
	/*
	 * Range starts mid-stripe, extends through a full second
	 * stripe.  Prefix is RMW, interior is fully-dirty.
	 */
	struct stripe_op ops[4];
	size_t length = (STRIPE_DATA - 4096) + STRIPE_DATA;
	size_t n = range_walk(4096, length, ops, 4);

	ck_assert_uint_eq(n, 2);
	ck_assert_int_eq(ops[0].is_rmw, 1);
	ck_assert_uint_eq(ops[0].sub_off_in_stripe, 4096);
	ck_assert_uint_eq(ops[0].sub_len, STRIPE_DATA - 4096);
	ck_assert_int_eq(ops[1].is_rmw, 0);
	ck_assert_uint_eq(ops[1].stripe_no, 1);
	ck_assert_uint_eq(ops[1].sub_off_in_data, STRIPE_DATA - 4096);
	ck_assert_uint_eq(ops[1].sub_len, STRIPE_DATA);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suffix RMW                                                          */
/* ------------------------------------------------------------------ */

START_TEST(test_range_suffix_only_partial)
{
	/*
	 * Range starts at stripe 1, ends partway through stripe 1
	 * -- suffix is partial, no prefix.
	 */
	struct stripe_op ops[4];
	size_t n = range_walk(STRIPE_DATA, STRIPE_DATA / 2, ops, 4);

	ck_assert_uint_eq(n, 1);
	ck_assert_uint_eq(ops[0].stripe_no, 1);
	ck_assert_uint_eq(ops[0].sub_off_in_stripe, 0);
	ck_assert_uint_eq(ops[0].sub_len, STRIPE_DATA / 2);
	ck_assert_int_eq(ops[0].is_rmw, 1);
}
END_TEST

START_TEST(test_range_full_then_suffix)
{
	/*
	 * Range covers a full first stripe and the first quarter
	 * of the second -- suffix RMW, no prefix.
	 */
	struct stripe_op ops[4];
	size_t length = STRIPE_DATA + STRIPE_DATA / 4;
	size_t n = range_walk(0, length, ops, 4);

	ck_assert_uint_eq(n, 2);
	ck_assert_int_eq(ops[0].is_rmw, 0);
	ck_assert_uint_eq(ops[0].sub_len, STRIPE_DATA);
	ck_assert_int_eq(ops[1].is_rmw, 1);
	ck_assert_uint_eq(ops[1].sub_off_in_stripe, 0);
	ck_assert_uint_eq(ops[1].sub_len, STRIPE_DATA / 4);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Sandwich: prefix + interior + suffix                                */
/* ------------------------------------------------------------------ */

START_TEST(test_range_prefix_interior_suffix)
{
	/*
	 * Range from 4 KiB into stripe 0 to 8 KiB into stripe 2
	 * -- prefix and suffix are RMW, stripe 1 is fully dirty.
	 */
	struct stripe_op ops[8];
	uint64_t offset = 4096;
	size_t length = (STRIPE_DATA - 4096) + STRIPE_DATA + 8192;
	size_t n = range_walk(offset, length, ops, 8);

	ck_assert_uint_eq(n, 3);
	ck_assert_int_eq(ops[0].is_rmw, 1);
	ck_assert_uint_eq(ops[0].sub_off_in_stripe, 4096);
	ck_assert_int_eq(ops[1].is_rmw, 0);
	ck_assert_uint_eq(ops[1].stripe_no, 1);
	ck_assert_uint_eq(ops[1].sub_len, STRIPE_DATA);
	ck_assert_int_eq(ops[2].is_rmw, 1);
	ck_assert_uint_eq(ops[2].stripe_no, 2);
	ck_assert_uint_eq(ops[2].sub_off_in_stripe, 0);
	ck_assert_uint_eq(ops[2].sub_len, 8192);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Sub-stripe range (prefix == suffix)                                 */
/* ------------------------------------------------------------------ */

START_TEST(test_range_within_single_stripe)
{
	/*
	 * coll_t1b_subchunk_interleave: writer A writes bytes
	 * [0, 1024) of a 4 KiB block (sub-shard within stripe 0).
	 * Single RMW round-trip, one entry.
	 */
	struct stripe_op ops[4];
	size_t n = range_walk(0, 1024, ops, 4);

	ck_assert_uint_eq(n, 1);
	ck_assert_uint_eq(ops[0].stripe_no, 0);
	ck_assert_uint_eq(ops[0].sub_off_in_stripe, 0);
	ck_assert_uint_eq(ops[0].sub_len, 1024);
	ck_assert_int_eq(ops[0].is_rmw, 1);
}
END_TEST

START_TEST(test_range_subchunk_interleave_writer_b)
{
	/*
	 * coll_t1b_subchunk_interleave: writer B writes bytes
	 * [1024, 2048) of the same 4 KiB block.  RMW, offset/len
	 * land in the middle of the stripe.
	 */
	struct stripe_op ops[4];
	size_t n = range_walk(1024, 1024, ops, 4);

	ck_assert_uint_eq(n, 1);
	ck_assert_uint_eq(ops[0].stripe_no, 0);
	ck_assert_uint_eq(ops[0].sub_off_in_stripe, 1024);
	ck_assert_uint_eq(ops[0].sub_off_in_data, 0);
	ck_assert_uint_eq(ops[0].sub_len, 1024);
	ck_assert_int_eq(ops[0].is_rmw, 1);
}
END_TEST

/* ------------------------------------------------------------------ */
/* coll_t1b_disjoint -- 4 writers, non-overlapping ranges              */
/* ------------------------------------------------------------------ */

START_TEST(test_range_disjoint_writers)
{
	/*
	 * 4 writers, each writes one full stripe at a distinct
	 * stripe number.  No two writers touch the same stripe;
	 * every stripe is fully dirty (no RMW).
	 */
	for (uint64_t rank = 0; rank < 4; rank++) {
		struct stripe_op ops[2];
		uint64_t offset = rank * STRIPE_DATA;
		size_t n = range_walk(offset, STRIPE_DATA, ops, 2);

		ck_assert_uint_eq(n, 1);
		ck_assert_uint_eq(ops[0].stripe_no, rank);
		ck_assert_int_eq(ops[0].is_rmw, 0);
	}
}
END_TEST

/* ------------------------------------------------------------------ */
/* coll_t1b_full_chunk_split -- 2 writers, halves of one stripe        */
/* ------------------------------------------------------------------ */

START_TEST(test_range_full_chunk_split)
{
	/*
	 * Two writers each take half a stripe.  Both are RMW
	 * because neither covers all k*shard_size bytes alone.
	 */
	struct stripe_op a_ops[2];
	struct stripe_op b_ops[2];
	size_t na = range_walk(0, STRIPE_DATA / 2, a_ops, 2);
	size_t nb = range_walk(STRIPE_DATA / 2, STRIPE_DATA / 2, b_ops, 2);

	ck_assert_uint_eq(na, 1);
	ck_assert_uint_eq(nb, 1);
	ck_assert_uint_eq(a_ops[0].stripe_no, 0);
	ck_assert_uint_eq(b_ops[0].stripe_no, 0);
	ck_assert_int_eq(a_ops[0].is_rmw, 1);
	ck_assert_int_eq(b_ops[0].is_rmw, 1);
	ck_assert_uint_eq(a_ops[0].sub_off_in_stripe, 0);
	ck_assert_uint_eq(b_ops[0].sub_off_in_stripe, STRIPE_DATA / 2);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Zero-length is a no-op                                              */
/* ------------------------------------------------------------------ */

START_TEST(test_range_zero_length)
{
	struct stripe_op ops[4];
	size_t n = range_walk(0, 0, ops, 4);

	ck_assert_uint_eq(n, 0);

	/* And not at byte 0 either. */
	n = range_walk(12345, 0, ops, 4);
	ck_assert_uint_eq(n, 0);
}
END_TEST

/* ------------------------------------------------------------------ */

static Suite *ec_range_suite(void)
{
	Suite *s = suite_create("ec_pipeline_range");
	TCase *tc = tcase_create("walk");

	tcase_add_test(tc, test_range_aligned_single_stripe);
	tcase_add_test(tc, test_range_aligned_multi_stripe);
	tcase_add_test(tc, test_range_aligned_offset_nonzero);
	tcase_add_test(tc, test_range_prefix_only_partial);
	tcase_add_test(tc, test_range_prefix_then_full_interior);
	tcase_add_test(tc, test_range_suffix_only_partial);
	tcase_add_test(tc, test_range_full_then_suffix);
	tcase_add_test(tc, test_range_prefix_interior_suffix);
	tcase_add_test(tc, test_range_within_single_stripe);
	tcase_add_test(tc, test_range_subchunk_interleave_writer_b);
	tcase_add_test(tc, test_range_disjoint_writers);
	tcase_add_test(tc, test_range_full_chunk_split);
	tcase_add_test(tc, test_range_zero_length);

	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	SRunner *sr = srunner_create(ec_range_suite());

	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);

	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
