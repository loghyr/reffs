/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Unit tests for Mojette transform core (forward + inverse).
 */

#include <check.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "mojette.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Fill a grid with a deterministic pattern. */
static void fill_grid(uint64_t *grid, int P, int Q)
{
	for (int i = 0; i < P * Q; i++)
		grid[i] = (uint64_t)(i * 37 + 13);
}

/* Allocate projection array for n directions on a PxQ grid. */
static struct moj_projection **alloc_projs(const struct moj_direction *dirs,
					   int n, int P, int Q)
{
	struct moj_projection **projs;

	projs = calloc((size_t)n, sizeof(*projs));
	if (!projs)
		return NULL;

	for (int i = 0; i < n; i++) {
		int nbins =
			moj_projection_size(dirs[i].md_p, dirs[i].md_q, P, Q);

		projs[i] = moj_projection_create(nbins);
		if (!projs[i]) {
			for (int j = 0; j < i; j++)
				moj_projection_destroy(projs[j]);
			free(projs);
			return NULL;
		}
	}
	return projs;
}

static void free_projs(struct moj_projection **projs, int n)
{
	if (!projs)
		return;
	for (int i = 0; i < n; i++)
		moj_projection_destroy(projs[i]);
	free(projs);
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

START_TEST(test_projection_size)
{
	/* Paper convention: B = |p|(Q-1) + |q|(P-1) + 1 */

	/* p=0, q=1, P=64, Q=4: B = 0 + 63 + 1 = 64 */
	ck_assert_int_eq(moj_projection_size(0, 1, 64, 4), 64);

	/* p=1, q=1, P=64, Q=4: B = 3 + 63 + 1 = 67 */
	ck_assert_int_eq(moj_projection_size(1, 1, 64, 4), 67);

	/* p=-2, q=1, P=64, Q=4: B = 6 + 63 + 1 = 70 */
	ck_assert_int_eq(moj_projection_size(-2, 1, 64, 4), 70);

	/* p=0, q=1, P=128, Q=4: B = 0 + 127 + 1 = 128 */
	ck_assert_int_eq(moj_projection_size(0, 1, 128, 4), 128);

	/* p=1, q=1, P=128, Q=4: B = 3 + 127 + 1 = 131 */
	ck_assert_int_eq(moj_projection_size(1, 1, 128, 4), 131);
}
END_TEST

START_TEST(test_direction_generation)
{
	struct moj_direction *dirs = NULL;
	int ret;

	/* n=4: expect p = {-2, -1, 1, 2}, q = 1 (no zero) */
	ret = moj_directions_generate(4, &dirs);
	ck_assert_int_eq(ret, 0);
	ck_assert_ptr_nonnull(dirs);
	ck_assert_int_eq(dirs[0].md_p, -2);
	ck_assert_int_eq(dirs[1].md_p, -1);
	ck_assert_int_eq(dirs[2].md_p, 1);
	ck_assert_int_eq(dirs[3].md_p, 2);
	for (int i = 0; i < 4; i++) {
		ck_assert_int_eq(dirs[i].md_q, 1);
		ck_assert(dirs[i].md_p != 0);
	}
	free(dirs);

	/* n=6: expect p = {-3, -2, -1, 1, 2, 3}, q = 1 (no zero) */
	ret = moj_directions_generate(6, &dirs);
	ck_assert_int_eq(ret, 0);
	ck_assert_int_eq(dirs[0].md_p, -3);
	ck_assert_int_eq(dirs[5].md_p, 3);
	for (int i = 0; i < 6; i++)
		ck_assert(dirs[i].md_p != 0);
	free(dirs);
}
END_TEST

START_TEST(test_katz_criterion)
{
	/* 4 directions with q=1: sum(|q_i|) = 4 >= Q=4 -> true */
	struct moj_direction dirs4[] = {
		{ -2, 1 }, { -1, 1 }, { 1, 1 }, { 2, 1 }
	};

	ck_assert(moj_katz_check(dirs4, 4, 128, 4));

	/* 3 directions with q=1: sum(|q_i|) = 3 < Q=4 -> false */
	ck_assert(!moj_katz_check(dirs4, 3, 128, 4));

	/* 3 directions with q=1: sum(|q_i|) = 3 >= Q=3 -> true */
	ck_assert(moj_katz_check(dirs4, 3, 128, 3));
}
END_TEST

START_TEST(test_forward_inverse_small)
{
	/* Small 4x3 grid, 4 projections (one extra for redundancy). */
	int P = 4, Q = 3, n = 4;
	struct moj_direction dirs[] = {
		{ -2, 1 }, { -1, 1 }, { 1, 1 }, { 2, 1 }
	};
	uint64_t grid[12], recovered[12];

	fill_grid(grid, P, Q);

	struct moj_projection **projs = alloc_projs(dirs, n, P, Q);

	ck_assert_ptr_nonnull(projs);

	/* Forward: grid -> projections */
	moj_forward(grid, P, Q, dirs, n, projs);

	/* Inverse using all 4 projections: should recover exactly. */
	int ret = moj_inverse(recovered, P, Q, dirs, n, projs);

	ck_assert_int_eq(ret, 0);
	ck_assert_int_eq(memcmp(grid, recovered, sizeof(grid)), 0);

	free_projs(projs, n);
}
END_TEST

START_TEST(test_forward_inverse_128x4)
{
	/* Realistic size: 128 columns x 4 rows = 4KB with 8-byte elements. */
	int P = 128, Q = 4, n = 6;
	struct moj_direction *dirs = NULL;

	ck_assert_int_eq(moj_directions_generate(n, &dirs), 0);

	uint64_t *grid = calloc((size_t)(P * Q), sizeof(uint64_t));
	uint64_t *recovered = calloc((size_t)(P * Q), sizeof(uint64_t));

	ck_assert_ptr_nonnull(grid);
	ck_assert_ptr_nonnull(recovered);
	fill_grid(grid, P, Q);

	struct moj_projection **projs = alloc_projs(dirs, n, P, Q);

	ck_assert_ptr_nonnull(projs);

	moj_forward(grid, P, Q, dirs, n, projs);

	int ret = moj_inverse(recovered, P, Q, dirs, n, projs);

	ck_assert_int_eq(ret, 0);
	ck_assert_int_eq(memcmp(grid, recovered, P * Q * (int)sizeof(uint64_t)),
			 0);

	free(grid);
	free(recovered);
	free_projs(projs, n);
	free(dirs);
}
END_TEST

START_TEST(test_inverse_subset)
{
	/*
	 * Encode with 6 projections, decode with only 4 (= Q).
	 * Any 4 projections should suffice (Katz: sum(q_i) = 4 >= Q).
	 */
	int P = 16, Q = 4, n_full = 6, n_decode = 4;
	struct moj_direction *dirs_full = NULL;

	ck_assert_int_eq(moj_directions_generate(n_full, &dirs_full), 0);

	uint64_t *grid = calloc((size_t)(P * Q), sizeof(uint64_t));
	uint64_t *recovered = calloc((size_t)(P * Q), sizeof(uint64_t));

	fill_grid(grid, P, Q);

	struct moj_projection **projs_full =
		alloc_projs(dirs_full, n_full, P, Q);

	moj_forward(grid, P, Q, dirs_full, n_full, projs_full);

	/*
	 * Use projections 0, 2, 3, 5 (skip 1 and 4) -- an arbitrary
	 * subset of 4 out of 6.
	 */
	int subset[] = { 0, 2, 3, 5 };
	struct moj_direction dirs_sub[4];
	struct moj_projection *projs_sub[4];

	for (int i = 0; i < n_decode; i++) {
		dirs_sub[i] = dirs_full[subset[i]];
		projs_sub[i] = projs_full[subset[i]];
	}

	ck_assert(moj_katz_check(dirs_sub, n_decode, P, Q));

	int ret = moj_inverse(recovered, P, Q, dirs_sub, n_decode, projs_sub);

	ck_assert_int_eq(ret, 0);
	ck_assert_int_eq(memcmp(grid, recovered, P * Q * (int)sizeof(uint64_t)),
			 0);

	/*
	 * Don't free projs_sub entries -- they alias projs_full.
	 * Only free the full set.
	 */
	free(grid);
	free(recovered);
	free_projs(projs_full, n_full);
	free(dirs_full);
}
END_TEST

START_TEST(test_inverse_too_few)
{
	/*
	 * Only 2 projections for a Q=4 grid: clearly insufficient.
	 * sum(|q_i|) = 2 < 4, sum(|p_i|) = 2 < P=8.
	 */
	int P = 8, Q = 4, n = 2;
	struct moj_direction dirs[] = { { -1, 1 }, { 1, 1 } };
	uint64_t grid[32], recovered[32];

	fill_grid(grid, P, Q);

	struct moj_projection **projs = alloc_projs(dirs, n, P, Q);

	moj_forward(grid, P, Q, dirs, n, projs);

	ck_assert(!moj_katz_check(dirs, n, P, Q));

	int ret = moj_inverse(recovered, P, Q, dirs, n, projs);

	ck_assert_int_eq(ret, -EIO);

	free_projs(projs, n);
}
END_TEST

START_TEST(test_zero_grid)
{
	/* All-zero grid should round-trip correctly. */
	int P = 8, Q = 4, n = 4;
	struct moj_direction dirs[] = {
		{ -2, 1 }, { -1, 1 }, { 1, 1 }, { 2, 1 }
	};
	uint64_t grid[32] = { 0 };
	uint64_t recovered[32];

	struct moj_projection **projs = alloc_projs(dirs, n, P, Q);

	moj_forward(grid, P, Q, dirs, n, projs);

	int ret = moj_inverse(recovered, P, Q, dirs, n, projs);

	ck_assert_int_eq(ret, 0);
	ck_assert_int_eq(memcmp(grid, recovered, sizeof(grid)), 0);

	free_projs(projs, n);
}
END_TEST

START_TEST(test_xor_identity)
{
	/*
	 * Verify XOR algebra works across the full uint64_t range:
	 * fill grid with high-bit values that exercise every byte of
	 * the bin word.  XOR is bit-parallel with no overflow concerns,
	 * so the forward+inverse round-trip must reproduce the input
	 * exactly regardless of bit pattern.
	 */
	int P = 4, Q = 4, n = 4;
	struct moj_direction dirs[] = {
		{ -2, 1 }, { -1, 1 }, { 1, 1 }, { 2, 1 }
	};
	uint64_t grid[16], recovered[16];

	for (int i = 0; i < 16; i++)
		grid[i] = UINT64_MAX - (uint64_t)i;

	struct moj_projection **projs = alloc_projs(dirs, n, P, Q);

	moj_forward(grid, P, Q, dirs, n, projs);

	int ret = moj_inverse(recovered, P, Q, dirs, n, projs);

	ck_assert_int_eq(ret, 0);
	ck_assert_int_eq(memcmp(grid, recovered, sizeof(grid)), 0);

	free_projs(projs, n);
}
END_TEST

/* ------------------------------------------------------------------ */
/* SIMD correctness tests                                              */
/* ------------------------------------------------------------------ */

/*
 * test_simd_p1_roundtrip -- isolate the p=+1 SIMD fast path.
 *
 * Only dirs[0] (p=+1, q=1) dispatches to the NEON/SSE2 sequential-
 * ascending-bin helper; the other three directions (|p|>1) use the
 * scalar path.  An incorrect SIMD accumulation would corrupt the p=+1
 * projection and cause moj_inverse() to return -EIO or produce a grid
 * that differs from the original.
 *
 * Katz: sum|p_i| = 1+2+3+4 = 10 >= P=4.
 */
START_TEST(test_simd_p1_roundtrip)
{
	int P = 4, Q = 4, n = 4;
	struct moj_direction dirs[] = { { 1, 1 }, { 2, 1 }, { 3, 1 }, { 4, 1 } };
	uint64_t grid[16], recovered[16];

	fill_grid(grid, P, Q);

	struct moj_projection **projs = alloc_projs(dirs, n, P, Q);

	ck_assert_ptr_nonnull(projs);

	moj_forward(grid, P, Q, dirs, n, projs);

	int ret = moj_inverse(recovered, P, Q, dirs, n, projs);

	ck_assert_int_eq(ret, 0);
	ck_assert_int_eq(memcmp(grid, recovered, sizeof(grid)), 0);

	free_projs(projs, n);
}
END_TEST

/*
 * test_simd_pm1_roundtrip -- isolate the p=-1 SIMD fast path.
 *
 * Only dirs[0] (p=-1, q=1) dispatches to the reversed-bin
 * NEON/SSE2 helper (vextq_u64 / shuffle swap); the other three
 * use scalar.  A swap bug would produce wrong bins for p=-1 only.
 *
 * Katz: sum|p_i| = 1+2+3+4 = 10 >= P=4.
 */
START_TEST(test_simd_pm1_roundtrip)
{
	int P = 4, Q = 4, n = 4;
	struct moj_direction dirs[] = {
		{ -1, 1 }, { -2, 1 }, { -3, 1 }, { -4, 1 }
	};
	uint64_t grid[16], recovered[16];

	fill_grid(grid, P, Q);

	struct moj_projection **projs = alloc_projs(dirs, n, P, Q);

	ck_assert_ptr_nonnull(projs);

	moj_forward(grid, P, Q, dirs, n, projs);

	int ret = moj_inverse(recovered, P, Q, dirs, n, projs);

	ck_assert_int_eq(ret, 0);
	ck_assert_int_eq(memcmp(grid, recovered, sizeof(grid)), 0);

	free_projs(projs, n);
}
END_TEST

/*
 * test_simd_p1_tail -- P=7 exercises the scalar tail in the p=+1 SIMD loop.
 *
 * The 4-wide SIMD loop handles columns 0--3; columns 4--6 fall through
 * to the scalar tail.  An off-by-one in the loop bound or a wrong
 * starting column in the tail produces corrupt bins.
 *
 * Katz: sum|p_i| = 10 >= P=7.
 */
START_TEST(test_simd_p1_tail)
{
	int P = 7, Q = 4, n = 4;
	struct moj_direction dirs[] = { { 1, 1 }, { 2, 1 }, { 3, 1 }, { 4, 1 } };
	uint64_t grid[28], recovered[28];

	fill_grid(grid, P, Q);

	struct moj_projection **projs = alloc_projs(dirs, n, P, Q);

	ck_assert_ptr_nonnull(projs);

	moj_forward(grid, P, Q, dirs, n, projs);

	int ret = moj_inverse(recovered, P, Q, dirs, n, projs);

	ck_assert_int_eq(ret, 0);
	ck_assert_int_eq(memcmp(grid, recovered, sizeof(grid)), 0);

	free_projs(projs, n);
}
END_TEST

/*
 * test_simd_pm1_tail -- P=7, exercises the p=-1 SIMD tail.
 *
 * For NEON: 4-wide loop takes cols 0--3, 2-wide cleanup takes cols 4--5,
 * scalar handles col 6.  For SSE2 the 2-wide cleanup is identical.
 * The bin addresses for the tail are negative offsets from dst; an
 * off-by-one here is particularly easy to miss.
 *
 * Katz: sum|p_i| = 10 >= P=7.
 */
START_TEST(test_simd_pm1_tail)
{
	int P = 7, Q = 4, n = 4;
	struct moj_direction dirs[] = {
		{ -1, 1 }, { -2, 1 }, { -3, 1 }, { -4, 1 }
	};
	uint64_t grid[28], recovered[28];

	fill_grid(grid, P, Q);

	struct moj_projection **projs = alloc_projs(dirs, n, P, Q);

	ck_assert_ptr_nonnull(projs);

	moj_forward(grid, P, Q, dirs, n, projs);

	int ret = moj_inverse(recovered, P, Q, dirs, n, projs);

	ck_assert_int_eq(ret, 0);
	ck_assert_int_eq(memcmp(grid, recovered, sizeof(grid)), 0);

	free_projs(projs, n);
}
END_TEST

/*
 * test_simd_bins_p1 -- exact bin values for p=+1, q=1 on a 3x2 grid.
 *
 * Paper convention: b = row*p + col*q - off.
 *
 * fill_grid values (i*37+13, row-major):
 *   row 0: [13, 50, 87]     row 1: [124, 161, 198]
 *
 * For p=1, q=1: off = 0 (min of row+col is 0).  b = row + col.
 *   b=0: (r=0,c=0) --> 13
 *   b=1: (r=0,c=1) ^ (r=1,c=0) --> 50 ^ 124 = 78
 *   b=2: (r=0,c=2) ^ (r=1,c=1) --> 87 ^ 161 = 246
 *   b=3: (r=1,c=2) --> 198
 *
 * P=3 < 4: 4-wide SIMD loop does not fire; scalar tail path only.
 * Anchors the bin addressing formula numerically.
 */
START_TEST(test_simd_bins_p1)
{
	int P = 3, Q = 2, n = 1;
	struct moj_direction dirs[] = { { 1, 1 } };
	uint64_t grid[6];
	uint64_t expected[4] = { 13, 78, 246, 198 };

	fill_grid(grid, P, Q);

	struct moj_projection **projs = alloc_projs(dirs, n, P, Q);

	ck_assert_ptr_nonnull(projs);

	moj_forward(grid, P, Q, dirs, n, projs);

	ck_assert_int_eq(projs[0]->mp_nbins, 4);
	ck_assert_int_eq(memcmp(projs[0]->mp_bins, expected, sizeof(expected)),
			 0);

	free_projs(projs, n);
}
END_TEST

/*
 * test_simd_bins_pm1 -- exact bin values for p=-1, q=1 on a 3x2 grid.
 *
 * Same grid as test_simd_bins_p1.
 * Paper convention: b = row*p + col*q - off.
 * For p=-1, q=1: off = -(Q-1) = -1.  b = -row + col - (-1) = -row + col + 1.
 *   b=0: (r=1,c=0) --> 124
 *   b=1: (r=0,c=0) ^ (r=1,c=1) --> 13 ^ 161 = 172
 *   b=2: (r=0,c=1) ^ (r=1,c=2) --> 50 ^ 198 = 244
 *   b=3: (r=0,c=2) --> 87
 *
 * In paper convention with q=1, bins are sequential in col for any p
 * (bin pointer = bins + row*p - off shifts per row; cols XOR in
 * sequentially).  Same SIMD helper handles every direction.
 */
START_TEST(test_simd_bins_pm1)
{
	int P = 3, Q = 2, n = 1;
	struct moj_direction dirs[] = { { -1, 1 } };
	uint64_t grid[6];
	uint64_t expected[4] = { 124, 172, 244, 87 };

	fill_grid(grid, P, Q);

	struct moj_projection **projs = alloc_projs(dirs, n, P, Q);

	ck_assert_ptr_nonnull(projs);

	moj_forward(grid, P, Q, dirs, n, projs);

	ck_assert_int_eq(projs[0]->mp_nbins, 4);
	ck_assert_int_eq(memcmp(projs[0]->mp_bins, expected, sizeof(expected)),
			 0);

	free_projs(projs, n);
}
END_TEST

/*
 * test_simd_vs_scalar_large -- 64x64 grid, SIMD reproducibility.
 *
 * Run moj_forward() twice with separate zero-initialised projection
 * arrays on the same grid.  The bin arrays must be bit-identical,
 * confirming the NEON/SSE2 paths are deterministic and do not depend
 * on initial register state or stale bin contents from a prior call.
 * Direction set includes both p=+1 and p=-1 to exercise both SIMD helpers.
 */
START_TEST(test_simd_vs_scalar_large)
{
	int P = 64, Q = 64, n = 4;
	struct moj_direction dirs[] = {
		{ -2, 1 }, { -1, 1 }, { 1, 1 }, { 2, 1 }
	};

	uint64_t *grid = calloc((size_t)(P * Q), sizeof(uint64_t));

	ck_assert_ptr_nonnull(grid);
	fill_grid(grid, P, Q);

	struct moj_projection **projs_a = alloc_projs(dirs, n, P, Q);
	struct moj_projection **projs_b = alloc_projs(dirs, n, P, Q);

	ck_assert_ptr_nonnull(projs_a);
	ck_assert_ptr_nonnull(projs_b);

	moj_forward(grid, P, Q, dirs, n, projs_a);
	moj_forward(grid, P, Q, dirs, n, projs_b);

	for (int i = 0; i < n; i++) {
		ck_assert_int_eq(projs_a[i]->mp_nbins, projs_b[i]->mp_nbins);
		ck_assert_int_eq(
			memcmp(projs_a[i]->mp_bins, projs_b[i]->mp_bins,
			       (size_t)projs_a[i]->mp_nbins * sizeof(uint64_t)),
			0);
	}

	free(grid);
	free_projs(projs_a, n);
	free_projs(projs_b, n);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Geometry-driven (gd) tests                                          */
/*                                                                     */
/* Setup toggles moj_force_gd(true); teardown restores false.  Every   */
/* test in this tcase exercises moj_inverse_gd directly to keep the    */
/* assertions grounded — not via the dispatcher.                       */
/* ------------------------------------------------------------------ */

static void gd_setup(void)
{
	moj_force_gd(true);
}

static void gd_teardown(void)
{
	moj_force_gd(false);
}

START_TEST(test_gd_4x3)
{
	int P = 4, Q = 3, n = 3;
	struct moj_direction dirs[] = { { -1, 1 }, { 1, 1 }, { 2, 1 } };
	uint64_t grid[12], recovered[12];

	fill_grid(grid, P, Q);

	struct moj_projection **projs = alloc_projs(dirs, n, P, Q);

	ck_assert_ptr_nonnull(projs);

	moj_forward(grid, P, Q, dirs, n, projs);

	int ret = moj_inverse_gd(recovered, P, Q, dirs, n, projs);

	ck_assert_int_eq(ret, 0);
	ck_assert_int_eq(memcmp(grid, recovered, sizeof(grid)), 0);

	free_projs(projs, n);
}
END_TEST

START_TEST(test_gd_6x4)
{
	int P = 6, Q = 4, n = 4;
	struct moj_direction dirs[] = {
		{ -2, 1 }, { -1, 1 }, { 1, 1 }, { 2, 1 }
	};
	uint64_t grid[24], recovered[24];

	fill_grid(grid, P, Q);

	struct moj_projection **projs = alloc_projs(dirs, n, P, Q);

	ck_assert_ptr_nonnull(projs);

	moj_forward(grid, P, Q, dirs, n, projs);

	int ret = moj_inverse_gd(recovered, P, Q, dirs, n, projs);

	ck_assert_int_eq(ret, 0);
	ck_assert_int_eq(memcmp(grid, recovered, sizeof(grid)), 0);

	free_projs(projs, n);
}
END_TEST

START_TEST(test_gd_q1)
{
	/*
	 * Q=1 degenerate case.  np=1, sweep collapses to copying the
	 * sole bin into the single row.  s_minus = s_plus = 0 (empty
	 * interior range).
	 */
	int P = 4, Q = 1, n = 1;
	struct moj_direction dirs[] = { { 1, 1 } };
	uint64_t grid[4], recovered[4];

	fill_grid(grid, P, Q);

	struct moj_projection **projs = alloc_projs(dirs, n, P, Q);

	ck_assert_ptr_nonnull(projs);

	moj_forward(grid, P, Q, dirs, n, projs);

	int ret = moj_inverse_gd(recovered, P, Q, dirs, n, projs);

	ck_assert_int_eq(ret, 0);
	ck_assert_int_eq(memcmp(grid, recovered, sizeof(grid)), 0);

	free_projs(projs, n);
}
END_TEST

START_TEST(test_gd_q2)
{
	/* Q=2: smallest non-trivial case where rdv != 0. */
	int P = 4, Q = 2, n = 2;
	struct moj_direction dirs[] = { { -1, 1 }, { 1, 1 } };
	uint64_t grid[8], recovered[8];

	fill_grid(grid, P, Q);

	struct moj_projection **projs = alloc_projs(dirs, n, P, Q);

	ck_assert_ptr_nonnull(projs);

	moj_forward(grid, P, Q, dirs, n, projs);

	int ret = moj_inverse_gd(recovered, P, Q, dirs, n, projs);

	ck_assert_int_eq(ret, 0);
	ck_assert_int_eq(memcmp(grid, recovered, sizeof(grid)), 0);

	free_projs(projs, n);
}
END_TEST

START_TEST(test_gd_n_neq_q_rejection)
{
	/*
	 * gd requires n == Q.  Passing n != Q must return -EINVAL.
	 * (The dispatcher would normally fall back to peel; here we
	 * call gd directly.)
	 */
	int P = 4, Q = 3, n = 4;
	struct moj_direction dirs[] = {
		{ -2, 1 }, { -1, 1 }, { 1, 1 }, { 2, 1 }
	};
	uint64_t recovered[12];

	struct moj_projection **projs = alloc_projs(dirs, n, P, Q);

	ck_assert_ptr_nonnull(projs);

	int ret = moj_inverse_gd(recovered, P, Q, dirs, n, projs);

	ck_assert_int_eq(ret, -EINVAL);

	free_projs(projs, n);
}
END_TEST

START_TEST(test_gd_zero_grid)
{
	int P = 6, Q = 4, n = 4;
	struct moj_direction dirs[] = {
		{ -2, 1 }, { -1, 1 }, { 1, 1 }, { 2, 1 }
	};
	uint64_t grid[24] = { 0 }, recovered[24];

	struct moj_projection **projs = alloc_projs(dirs, n, P, Q);

	ck_assert_ptr_nonnull(projs);

	moj_forward(grid, P, Q, dirs, n, projs);

	int ret = moj_inverse_gd(recovered, P, Q, dirs, n, projs);

	ck_assert_int_eq(ret, 0);
	for (int i = 0; i < 24; i++)
		ck_assert_uint_eq(recovered[i], 0);

	free_projs(projs, n);
}
END_TEST

START_TEST(test_gd_p0_row_parity)
{
	/*
	 * Exercise p==0 alongside non-zero slopes.  In paper
	 * convention with q=1, p==0 is the column-XOR parity (bin
	 * b = col - off); the gd sweep ordering must arrange other
	 * column-x pixels to be recovered before p==0 is reached for
	 * pixel (y, x).  Test name is historical (p==0 was row-parity
	 * in reffs's earlier convention).
	 *
	 * P=4, Q=3, n=3, dirs={-1, 0, 1}.
	 * Katz: sum|q|=3 >= Q=3.
	 */
	int P = 4, Q = 3, n = 3;
	struct moj_direction dirs[] = { { -1, 1 }, { 0, 1 }, { 1, 1 } };
	uint64_t grid[12], recovered[12];

	fill_grid(grid, P, Q);

	struct moj_projection **projs = alloc_projs(dirs, n, P, Q);

	ck_assert_ptr_nonnull(projs);

	moj_forward(grid, P, Q, dirs, n, projs);

	int ret = moj_inverse_gd(recovered, P, Q, dirs, n, projs);

	ck_assert_int_eq(ret, 0);
	ck_assert_int_eq(memcmp(grid, recovered, sizeof(grid)), 0);

	free_projs(projs, n);
}
END_TEST

START_TEST(test_gd_24k_geometry)
{
	/*
	 * Production geometry: P=3072, Q=2, n=2.  ~6144 cells, well
	 * within the 2-second per-test budget.  Mirrors the existing
	 * test_sys_24k codec scenarios.
	 */
	int P = 3072, Q = 2, n = 2;
	struct moj_direction dirs[] = { { -1, 1 }, { 1, 1 } };
	uint64_t *grid = calloc((size_t)P * Q, sizeof(uint64_t));
	uint64_t *recovered = calloc((size_t)P * Q, sizeof(uint64_t));

	ck_assert_ptr_nonnull(grid);
	ck_assert_ptr_nonnull(recovered);

	for (int i = 0; i < P * Q; i++)
		grid[i] = (uint64_t)(i * 37 + 13);

	struct moj_projection **projs = alloc_projs(dirs, n, P, Q);

	ck_assert_ptr_nonnull(projs);

	moj_forward(grid, P, Q, dirs, n, projs);

	int ret = moj_inverse_gd(recovered, P, Q, dirs, n, projs);

	ck_assert_int_eq(ret, 0);
	ck_assert_int_eq(memcmp(grid, recovered, P * Q * sizeof(uint64_t)),
			 0);

	free_projs(projs, n);
	free(recovered);
	free(grid);
}
END_TEST

START_TEST(test_gd_vs_peel_parity)
{
	/*
	 * gd and peel must produce byte-identical recovered grids on
	 * the same input.  The forward bins are the same (algebra is
	 * shared), so any divergence in the recovered output is a gd
	 * bug.
	 */
	int P = 6, Q = 4, n = 4;
	struct moj_direction dirs[] = {
		{ -2, 1 }, { -1, 1 }, { 1, 1 }, { 2, 1 }
	};
	uint64_t grid[24];
	uint64_t recovered_gd[24];
	uint64_t recovered_peel[24];

	fill_grid(grid, P, Q);

	struct moj_projection **projs_gd = alloc_projs(dirs, n, P, Q);
	struct moj_projection **projs_peel = alloc_projs(dirs, n, P, Q);

	ck_assert_ptr_nonnull(projs_gd);
	ck_assert_ptr_nonnull(projs_peel);

	moj_forward(grid, P, Q, dirs, n, projs_gd);
	moj_forward(grid, P, Q, dirs, n, projs_peel);

	int rgd = moj_inverse_gd(recovered_gd, P, Q, dirs, n, projs_gd);
	int rpe = moj_inverse_peel(recovered_peel, P, Q, dirs, n, projs_peel);

	ck_assert_int_eq(rgd, 0);
	ck_assert_int_eq(rpe, 0);
	ck_assert_int_eq(memcmp(recovered_gd, recovered_peel, sizeof(grid)),
			 0);
	ck_assert_int_eq(memcmp(grid, recovered_gd, sizeof(grid)), 0);

	free_projs(projs_gd, n);
	free_projs(projs_peel, n);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

static Suite *mojette_suite(void)
{
	Suite *s = suite_create("mojette");
	TCase *tc = tcase_create("core");
	TCase *tc_simd = tcase_create("simd");
	TCase *tc_gd = tcase_create("gd");

	tcase_add_test(tc, test_projection_size);
	tcase_add_test(tc, test_direction_generation);
	tcase_add_test(tc, test_katz_criterion);
	tcase_add_test(tc, test_forward_inverse_small);
	tcase_add_test(tc, test_forward_inverse_128x4);
	tcase_add_test(tc, test_inverse_subset);
	tcase_add_test(tc, test_inverse_too_few);
	tcase_add_test(tc, test_zero_grid);
	tcase_add_test(tc, test_xor_identity);
	suite_add_tcase(s, tc);

	tcase_add_test(tc_simd, test_simd_p1_roundtrip);
	tcase_add_test(tc_simd, test_simd_pm1_roundtrip);
	tcase_add_test(tc_simd, test_simd_p1_tail);
	tcase_add_test(tc_simd, test_simd_pm1_tail);
	tcase_add_test(tc_simd, test_simd_bins_p1);
	tcase_add_test(tc_simd, test_simd_bins_pm1);
	tcase_add_test(tc_simd, test_simd_vs_scalar_large);
	suite_add_tcase(s, tc_simd);

	tcase_add_checked_fixture(tc_gd, gd_setup, gd_teardown);
	tcase_add_test(tc_gd, test_gd_4x3);
	tcase_add_test(tc_gd, test_gd_6x4);
	tcase_add_test(tc_gd, test_gd_q1);
	tcase_add_test(tc_gd, test_gd_q2);
	tcase_add_test(tc_gd, test_gd_n_neq_q_rejection);
	tcase_add_test(tc_gd, test_gd_zero_grid);
	tcase_add_test(tc_gd, test_gd_p0_row_parity);
	tcase_add_test(tc_gd, test_gd_24k_geometry);
	tcase_add_test(tc_gd, test_gd_vs_peel_parity);
	suite_add_tcase(s, tc_gd);

	return s;
}

int main(void)
{
	Suite *s = mojette_suite();
	SRunner *sr = srunner_create(s);

	srunner_run_all(sr, CK_NORMAL);
	int nf = srunner_ntests_failed(sr);

	srunner_free(sr);
	return nf == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
