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
	/* B = |p|(P-1) + |q|(Q-1) + 1 */

	/* p=0, q=1, P=64, Q=4: B = 0 + 3 + 1 = 4 */
	ck_assert_int_eq(moj_projection_size(0, 1, 64, 4), 4);

	/* p=1, q=1, P=64, Q=4: B = 63 + 3 + 1 = 67 */
	ck_assert_int_eq(moj_projection_size(1, 1, 64, 4), 67);

	/* p=-2, q=1, P=64, Q=4: B = 126 + 3 + 1 = 130 */
	ck_assert_int_eq(moj_projection_size(-2, 1, 64, 4), 130);

	/* p=0, q=1, P=128, Q=4: B = 0 + 3 + 1 = 4 */
	ck_assert_int_eq(moj_projection_size(0, 1, 128, 4), 4);

	/* p=1, q=1, P=128, Q=4: B = 127 + 3 + 1 = 131 */
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
	 * Use projections 0, 2, 3, 5 (skip 1 and 4) — an arbitrary
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
	 * Don't free projs_sub entries — they alias projs_full.
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

START_TEST(test_wrapping_arithmetic)
{
	/*
	 * Verify mod-2^64 arithmetic works: fill grid with large
	 * values that will overflow when summed.
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
/* Suite                                                               */
/* ------------------------------------------------------------------ */

static Suite *mojette_suite(void)
{
	Suite *s = suite_create("mojette");
	TCase *tc = tcase_create("core");

	tcase_add_test(tc, test_projection_size);
	tcase_add_test(tc, test_direction_generation);
	tcase_add_test(tc, test_katz_criterion);
	tcase_add_test(tc, test_forward_inverse_small);
	tcase_add_test(tc, test_forward_inverse_128x4);
	tcase_add_test(tc, test_inverse_subset);
	tcase_add_test(tc, test_inverse_too_few);
	tcase_add_test(tc, test_zero_grid);
	tcase_add_test(tc, test_wrapping_arithmetic);
	suite_add_tcase(s, tc);

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
