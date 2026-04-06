/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * Unit tests for reffs_lock_range_overlap().
 *
 * This function is the foundation of the entire locking system -- every
 * conflict check ultimately calls it.  It is a pure function (no inode,
 * no owner, no side effects) so these tests require no setup or teardown.
 *
 * Semantics (from lock.c):
 *   len == 0  means "from offset to end of file" (UINT64_MAX).
 *   Two ranges overlap iff [off1, end1] intersection [off2, end2] is non-empty.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <stdint.h>
#include <check.h>
#include "libreffs_test.h"
#include "fs_test_harness.h"
#include <urcu.h>
#include <urcu/ref.h>

#include "reffs/lock.h"
#include "reffs/test.h"

/* -- non-overlapping -------------------------------------------------------- */

START_TEST(test_overlap_no_before)
{
	/*
	 * [0, 9] vs [10, 19]: strictly before, no touch.
	 * off1=0 len1=10 --> end1=9; off2=10 len2=10 --> end2=19.
	 */
	ck_assert(!reffs_lock_range_overlap(0, 10, 10, 10));
	ck_assert(!reffs_lock_range_overlap(10, 10, 0, 10)); /* symmetric */
}
END_TEST

START_TEST(test_overlap_no_after)
{
	/* [100, 199] vs [200, 299] */
	ck_assert(!reffs_lock_range_overlap(100, 100, 200, 100));
	ck_assert(!reffs_lock_range_overlap(200, 100, 100, 100));
}
END_TEST

/* -- adjacent (touching at one byte boundary, must NOT overlap) ------------- */

START_TEST(test_overlap_adjacent)
{
	/*
	 * [0, 4] (off=0, len=5, end=4) vs [5, 9] (off=5, len=5, end=9).
	 * Adjacent ranges share no byte -- must not overlap.
	 */
	ck_assert(!reffs_lock_range_overlap(0, 5, 5, 5));
	ck_assert(!reffs_lock_range_overlap(5, 5, 0, 5));
}
END_TEST

/* -- overlapping ----------------------------------------------------------- */

START_TEST(test_overlap_partial)
{
	/* [0, 9] vs [5, 14]: overlap at [5, 9] */
	ck_assert(reffs_lock_range_overlap(0, 10, 5, 10));
	ck_assert(reffs_lock_range_overlap(5, 10, 0, 10));
}
END_TEST

START_TEST(test_overlap_one_contains_other)
{
	/* [0, 99] contains [10, 19] */
	ck_assert(reffs_lock_range_overlap(0, 100, 10, 10));
	ck_assert(reffs_lock_range_overlap(10, 10, 0, 100));
}
END_TEST

START_TEST(test_overlap_identical)
{
	/* Same range always overlaps itself */
	ck_assert(reffs_lock_range_overlap(50, 50, 50, 50));
}
END_TEST

START_TEST(test_overlap_single_byte)
{
	/* len=1: single-byte ranges */
	ck_assert(reffs_lock_range_overlap(5, 1, 5, 1));
	ck_assert(!reffs_lock_range_overlap(5, 1, 6, 1));
	ck_assert(!reffs_lock_range_overlap(5, 1, 4, 1));
}
END_TEST

/* -- len=0 (to end-of-file) semantics ------------------------------------- */

START_TEST(test_overlap_len0_vs_finite_overlapping)
{
	/*
	 * len=0 extends to UINT64_MAX.
	 * [1000, EOF) must overlap any range that starts at >= 1000
	 * and any range that ends at >= 1000.
	 */
	ck_assert(reffs_lock_range_overlap(1000, 0, 1000, 100));
	ck_assert(reffs_lock_range_overlap(1000, 100, 1000, 0));

	/* Finite range starting inside the to-EOF range */
	ck_assert(reffs_lock_range_overlap(1000, 0, 2000, 100));
	ck_assert(reffs_lock_range_overlap(2000, 100, 1000, 0));
}
END_TEST

START_TEST(test_overlap_len0_vs_finite_before)
{
	/*
	 * [1000, EOF) vs [0, 999]: the finite range ends at byte 999,
	 * which is before offset 1000.  Must NOT overlap.
	 *
	 * off1=1000, len1=0, end1=UINT64_MAX
	 * off2=0,    len2=1000, end2=999
	 * Condition: off1(1000) <= end2(999) is FALSE --> no overlap. OK
	 */
	ck_assert(!reffs_lock_range_overlap(1000, 0, 0, 1000));
	ck_assert(!reffs_lock_range_overlap(0, 1000, 1000, 0));
}
END_TEST

START_TEST(test_overlap_both_len0)
{
	/*
	 * Both len=0: both extend to UINT64_MAX.
	 * [0, EOF) and [1000, EOF) must overlap.
	 */
	ck_assert(reffs_lock_range_overlap(0, 0, 1000, 0));
	ck_assert(reffs_lock_range_overlap(1000, 0, 0, 0));
	ck_assert(reffs_lock_range_overlap(0, 0, 0, 0));
}
END_TEST

START_TEST(test_overlap_len0_adjacent_boundary)
{
	/*
	 * [0, 999] (off=0, len=1000, end=999) vs [1000, EOF) (off=1000, len=0).
	 * The finite range ends at 999, the to-EOF range starts at 1000.
	 * Adjacent, must NOT overlap.
	 */
	ck_assert(!reffs_lock_range_overlap(0, 1000, 1000, 0));
	ck_assert(!reffs_lock_range_overlap(1000, 0, 0, 1000));
}
END_TEST

/* -- suite ----------------------------------------------------------------- */

Suite *lock_range_suite(void)
{
	Suite *s = suite_create("Lock: range_overlap");
	TCase *tc = tcase_create("Core");

	tcase_add_test(tc, test_overlap_no_before);
	tcase_add_test(tc, test_overlap_no_after);
	tcase_add_test(tc, test_overlap_adjacent);
	tcase_add_test(tc, test_overlap_partial);
	tcase_add_test(tc, test_overlap_one_contains_other);
	tcase_add_test(tc, test_overlap_identical);
	tcase_add_test(tc, test_overlap_single_byte);
	tcase_add_test(tc, test_overlap_len0_vs_finite_overlapping);
	tcase_add_test(tc, test_overlap_len0_vs_finite_before);
	tcase_add_test(tc, test_overlap_both_len0);
	tcase_add_test(tc, test_overlap_len0_adjacent_boundary);

	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	return fs_test_run(lock_range_suite());
}
