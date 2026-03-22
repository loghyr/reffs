/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Unit tests for GF(2^8) matrix operations.
 */

#include <check.h>
#include <errno.h>
#include <stdlib.h>

#include "gf.h"
#include "matrix.h"

static void setup(void)
{
	gf_init();
}

START_TEST(test_identity_mul)
{
	struct gf_matrix *a = gf_matrix_create(3, 3);
	struct gf_matrix *id = gf_matrix_create(3, 3);
	struct gf_matrix *out = gf_matrix_create(3, 3);

	/* Fill A with known values. */
	uint8_t vals[] = { 5, 7, 3, 2, 9, 1, 4, 6, 8 };

	for (int i = 0; i < 9; i++)
		a->data[i] = vals[i];

	/* Identity matrix. */
	for (int i = 0; i < 3; i++)
		gf_matrix_set(id, i, i, 1);

	ck_assert_int_eq(gf_matrix_mul(a, id, out), 0);

	for (int i = 0; i < 9; i++)
		ck_assert_uint_eq(out->data[i], a->data[i]);

	gf_matrix_destroy(a);
	gf_matrix_destroy(id);
	gf_matrix_destroy(out);
}
END_TEST

START_TEST(test_invert_identity)
{
	struct gf_matrix *id = gf_matrix_create(3, 3);
	struct gf_matrix *out = gf_matrix_create(3, 3);

	for (int i = 0; i < 3; i++)
		gf_matrix_set(id, i, i, 1);

	ck_assert_int_eq(gf_matrix_invert(id, out), 0);

	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
			ck_assert_uint_eq(gf_matrix_get(out, r, c),
					  r == c ? 1 : 0);

	gf_matrix_destroy(id);
	gf_matrix_destroy(out);
}
END_TEST

START_TEST(test_invert_roundtrip)
{
	struct gf_matrix *a = gf_matrix_create(3, 3);
	struct gf_matrix *a_inv = gf_matrix_create(3, 3);
	struct gf_matrix *product = gf_matrix_create(3, 3);

	/* Non-singular matrix. */
	uint8_t vals[] = { 1, 2, 3, 4, 5, 6, 7, 8, 10 };

	for (int i = 0; i < 9; i++)
		a->data[i] = vals[i];

	ck_assert_int_eq(gf_matrix_invert(a, a_inv), 0);
	ck_assert_int_eq(gf_matrix_mul(a, a_inv, product), 0);

	/* Product should be identity. */
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
			ck_assert_uint_eq(gf_matrix_get(product, r, c),
					  r == c ? 1 : 0);

	gf_matrix_destroy(a);
	gf_matrix_destroy(a_inv);
	gf_matrix_destroy(product);
}
END_TEST

START_TEST(test_invert_singular)
{
	struct gf_matrix *a = gf_matrix_create(2, 2);
	struct gf_matrix *out = gf_matrix_create(2, 2);

	/* Singular: row 1 == row 0 in GF(2^8). */
	gf_matrix_set(a, 0, 0, 5);
	gf_matrix_set(a, 0, 1, 10);
	gf_matrix_set(a, 1, 0, 5);
	gf_matrix_set(a, 1, 1, 10);

	ck_assert_int_eq(gf_matrix_invert(a, out), -EINVAL);

	gf_matrix_destroy(a);
	gf_matrix_destroy(out);
}
END_TEST

START_TEST(test_vandermonde_shape)
{
	struct gf_matrix *v = gf_matrix_vandermonde(6, 4);

	ck_assert_ptr_nonnull(v);
	ck_assert_int_eq(v->rows, 6);
	ck_assert_int_eq(v->cols, 4);

	/* First column: i^0 = 1 for all rows. */
	for (int r = 0; r < 6; r++)
		ck_assert_uint_eq(gf_matrix_get(v, r, 0), 1);

	/* Row 0: 0^j = 1 for j=0, 0 for j>0. */
	ck_assert_uint_eq(gf_matrix_get(v, 0, 0), 1);
	for (int c = 1; c < 4; c++)
		ck_assert_uint_eq(gf_matrix_get(v, 0, c), 0);

	/* Row 1: 1^j = 1 for all j. */
	for (int c = 0; c < 4; c++)
		ck_assert_uint_eq(gf_matrix_get(v, 1, c), 1);

	gf_matrix_destroy(v);
}
END_TEST

Suite *matrix_suite(void)
{
	Suite *s = suite_create("GF(2^8) Matrix Ops");

	TCase *tc = tcase_create("matrix");
	tcase_add_unchecked_fixture(tc, setup, NULL);
	tcase_add_test(tc, test_identity_mul);
	tcase_add_test(tc, test_invert_identity);
	tcase_add_test(tc, test_invert_roundtrip);
	tcase_add_test(tc, test_invert_singular);
	tcase_add_test(tc, test_vandermonde_shape);
	suite_add_tcase(s, tc);

	return s;
}

int main(void)
{
	SRunner *sr = srunner_create(matrix_suite());

	srunner_run_all(sr, CK_NORMAL);
	int nf = srunner_ntests_failed(sr);

	srunner_free(sr);
	return nf ? EXIT_FAILURE : EXIT_SUCCESS;
}
