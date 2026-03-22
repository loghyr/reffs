/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Unit tests for GF(2^8) finite field arithmetic.
 */

#include <check.h>
#include <stdlib.h>

#include "gf.h"

static void setup(void)
{
	gf_init();
}

START_TEST(test_add_identity)
{
	for (int x = 0; x < 256; x++)
		ck_assert_uint_eq(gf_add((uint8_t)x, 0), (uint8_t)x);
}
END_TEST

START_TEST(test_add_self_inverse)
{
	for (int x = 0; x < 256; x++)
		ck_assert_uint_eq(gf_add((uint8_t)x, (uint8_t)x), 0);
}
END_TEST

START_TEST(test_mul_identity)
{
	for (int x = 0; x < 256; x++)
		ck_assert_uint_eq(gf_mul((uint8_t)x, 1), (uint8_t)x);
}
END_TEST

START_TEST(test_mul_zero)
{
	for (int x = 0; x < 256; x++)
		ck_assert_uint_eq(gf_mul((uint8_t)x, 0), 0);
}
END_TEST

START_TEST(test_mul_inverse)
{
	for (int x = 1; x < 256; x++)
		ck_assert_uint_eq(gf_mul((uint8_t)x, gf_inv((uint8_t)x)), 1);
}
END_TEST

START_TEST(test_mul_commutative)
{
	for (int a = 0; a < 256; a += 17)
		for (int b = 0; b < 256; b += 13)
			ck_assert_uint_eq(gf_mul((uint8_t)a, (uint8_t)b),
					  gf_mul((uint8_t)b, (uint8_t)a));
}
END_TEST

START_TEST(test_mul_associative)
{
	for (int a = 1; a < 256; a += 31)
		for (int b = 1; b < 256; b += 37)
			for (int c = 1; c < 256; c += 41)
				ck_assert_uint_eq(
					gf_mul((uint8_t)a,
					       gf_mul((uint8_t)b, (uint8_t)c)),
					gf_mul(gf_mul((uint8_t)a, (uint8_t)b),
					       (uint8_t)c));
}
END_TEST

START_TEST(test_mul_distributive)
{
	for (int a = 0; a < 256; a += 19)
		for (int b = 0; b < 256; b += 23)
			for (int c = 0; c < 256; c += 29)
				ck_assert_uint_eq(
					gf_mul((uint8_t)a,
					       gf_add((uint8_t)b, (uint8_t)c)),
					gf_add(gf_mul((uint8_t)a, (uint8_t)b),
					       gf_mul((uint8_t)a,
						      (uint8_t)c)));
}
END_TEST

START_TEST(test_generator_order)
{
	/* g=2 is primitive: 2^255 == 1, 2^i != 1 for 0 < i < 255 */
	ck_assert_uint_eq(gf_pow(2, 255), 1);
	for (int i = 1; i < 255; i++)
		ck_assert_uint_ne(gf_pow(2, (uint8_t)i), 1);
}
END_TEST

START_TEST(test_pow_zero_exponent)
{
	for (int x = 0; x < 256; x++)
		ck_assert_uint_eq(gf_pow((uint8_t)x, 0), 1);
}
END_TEST

Suite *gf_suite(void)
{
	Suite *s = suite_create("GF(2^8) Arithmetic");

	TCase *tc = tcase_create("field");
	tcase_add_unchecked_fixture(tc, setup, NULL);
	tcase_add_test(tc, test_add_identity);
	tcase_add_test(tc, test_add_self_inverse);
	tcase_add_test(tc, test_mul_identity);
	tcase_add_test(tc, test_mul_zero);
	tcase_add_test(tc, test_mul_inverse);
	tcase_add_test(tc, test_mul_commutative);
	tcase_add_test(tc, test_mul_associative);
	tcase_add_test(tc, test_mul_distributive);
	tcase_add_test(tc, test_generator_order);
	tcase_add_test(tc, test_pow_zero_exponent);
	suite_add_tcase(s, tc);

	return s;
}

int main(void)
{
	SRunner *sr = srunner_create(gf_suite());

	srunner_run_all(sr, CK_NORMAL);
	int nf = srunner_ntests_failed(sr);

	srunner_free(sr);
	return nf ? EXIT_FAILURE : EXIT_SUCCESS;
}
