/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * utf8string_id.c — tests for utf8string uid/gid conversion.
 *
 * Tests:
 *   utf8string_from_uid / utf8string_from_gid
 *   utf8string_to_uid   / utf8string_to_gid
 *
 * Error paths exercised:
 *   - null / empty input
 *   - leading zero on multi-digit string
 *   - non-digit character
 *   - numeric overflow
 *   - round-trip: from_uid(n) → to_uid == n
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <check.h>

#include "reffs/errno.h"
#include "reffs/utf8string.h"
#include "libreffs_test.h"

/* ------------------------------------------------------------------ */
/* utf8string_from_uid                                                 */
/* ------------------------------------------------------------------ */

START_TEST(test_from_uid_zero)
{
	utf8string u = { 0 };
	ck_assert_int_eq(utf8string_from_uid(&u, 0), 0);
	ck_assert_str_eq(u.utf8string_val, "0");
	ck_assert_uint_eq(u.utf8string_len, 1);
	utf8string_free(&u);
}
END_TEST

START_TEST(test_from_uid_nonzero)
{
	utf8string u = { 0 };
	ck_assert_int_eq(utf8string_from_uid(&u, 1000), 0);
	ck_assert_str_eq(u.utf8string_val, "1000");
	ck_assert_uint_eq(u.utf8string_len, 4);
	utf8string_free(&u);
}
END_TEST

START_TEST(test_from_uid_max)
{
	utf8string u = { 0 };
	uid_t max_uid = (uid_t)-1;
	ck_assert_int_eq(utf8string_from_uid(&u, max_uid), 0);
	ck_assert_ptr_nonnull(u.utf8string_val);
	ck_assert_int_gt(u.utf8string_len, 0);
	utf8string_free(&u);
}
END_TEST

/* ------------------------------------------------------------------ */
/* utf8string_from_gid                                                 */
/* ------------------------------------------------------------------ */

START_TEST(test_from_gid_zero)
{
	utf8string u = { 0 };
	ck_assert_int_eq(utf8string_from_gid(&u, 0), 0);
	ck_assert_str_eq(u.utf8string_val, "0");
	utf8string_free(&u);
}
END_TEST

START_TEST(test_from_gid_nonzero)
{
	utf8string u = { 0 };
	ck_assert_int_eq(utf8string_from_gid(&u, 65534), 0);
	ck_assert_str_eq(u.utf8string_val, "65534");
	utf8string_free(&u);
}
END_TEST

/* ------------------------------------------------------------------ */
/* utf8string_to_uid — success cases                                  */
/* ------------------------------------------------------------------ */

START_TEST(test_to_uid_zero)
{
	utf8string u = { 0 };
	uid_t id = 99;
	cstr_to_utf8string(&u, "0");
	ck_assert_int_eq(utf8string_to_uid(&u, &id), 0);
	ck_assert_uint_eq(id, 0);
	utf8string_free(&u);
}
END_TEST

START_TEST(test_to_uid_valid)
{
	utf8string u = { 0 };
	uid_t id = 0;
	cstr_to_utf8string(&u, "1000");
	ck_assert_int_eq(utf8string_to_uid(&u, &id), 0);
	ck_assert_uint_eq(id, 1000);
	utf8string_free(&u);
}
END_TEST

START_TEST(test_to_uid_single_digit)
{
	utf8string u = { 0 };
	uid_t id = 0;
	cstr_to_utf8string(&u, "7");
	ck_assert_int_eq(utf8string_to_uid(&u, &id), 0);
	ck_assert_uint_eq(id, 7);
	utf8string_free(&u);
}
END_TEST

/* ------------------------------------------------------------------ */
/* utf8string_to_uid — error cases                                    */
/* ------------------------------------------------------------------ */

START_TEST(test_to_uid_null_string)
{
	uid_t id = 0;
	ck_assert_int_eq(utf8string_to_uid(NULL, &id), -EINVAL);
}
END_TEST

START_TEST(test_to_uid_empty_string)
{
	utf8string u = { 0 };
	uid_t id = 0;
	ck_assert_int_eq(utf8string_to_uid(&u, &id), -EINVAL);
}
END_TEST

START_TEST(test_to_uid_leading_zero)
{
	utf8string u = { 0 };
	uid_t id = 0;
	cstr_to_utf8string(&u, "01");
	ck_assert_int_eq(utf8string_to_uid(&u, &id), -EINVAL);
	utf8string_free(&u);
}
END_TEST

START_TEST(test_to_uid_non_digit)
{
	utf8string u = { 0 };
	uid_t id = 0;
	cstr_to_utf8string(&u, "12x4");
	ck_assert_int_eq(utf8string_to_uid(&u, &id), -EINVAL);
	utf8string_free(&u);
}
END_TEST

START_TEST(test_to_uid_all_alpha)
{
	utf8string u = { 0 };
	uid_t id = 0;
	cstr_to_utf8string(&u, "root");
	ck_assert_int_eq(utf8string_to_uid(&u, &id), -EINVAL);
	utf8string_free(&u);
}
END_TEST

START_TEST(test_to_uid_overflow)
{
	/* 20-digit number guaranteed to exceed any uid_t */
	utf8string u = { 0 };
	uid_t id = 0;
	cstr_to_utf8string(&u, "99999999999999999999");
	ck_assert_int_eq(utf8string_to_uid(&u, &id), -ERANGE);
	utf8string_free(&u);
}
END_TEST

/* ------------------------------------------------------------------ */
/* utf8string_to_gid — error cases (same implementation, spot-check)  */
/* ------------------------------------------------------------------ */

START_TEST(test_to_gid_leading_zero)
{
	utf8string u = { 0 };
	gid_t id = 0;
	cstr_to_utf8string(&u, "007");
	ck_assert_int_eq(utf8string_to_gid(&u, &id), -EINVAL);
	utf8string_free(&u);
}
END_TEST

START_TEST(test_to_gid_overflow)
{
	utf8string u = { 0 };
	gid_t id = 0;
	cstr_to_utf8string(&u, "99999999999999999999");
	ck_assert_int_eq(utf8string_to_gid(&u, &id), -ERANGE);
	utf8string_free(&u);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Round-trip: from_uid(n) → to_uid == n                              */
/* ------------------------------------------------------------------ */

START_TEST(test_roundtrip_uid_zero)
{
	utf8string u = { 0 };
	uid_t out = 99;
	ck_assert_int_eq(utf8string_from_uid(&u, 0), 0);
	ck_assert_int_eq(utf8string_to_uid(&u, &out), 0);
	ck_assert_uint_eq(out, 0);
	utf8string_free(&u);
}
END_TEST

START_TEST(test_roundtrip_uid_typical)
{
	utf8string u = { 0 };
	uid_t out = 0;
	ck_assert_int_eq(utf8string_from_uid(&u, 65534), 0);
	ck_assert_int_eq(utf8string_to_uid(&u, &out), 0);
	ck_assert_uint_eq(out, 65534);
	utf8string_free(&u);
}
END_TEST

START_TEST(test_roundtrip_gid)
{
	utf8string u = { 0 };
	gid_t out = 0;
	ck_assert_int_eq(utf8string_from_gid(&u, 1001), 0);
	ck_assert_int_eq(utf8string_to_gid(&u, &out), 0);
	ck_assert_uint_eq(out, 1001);
	utf8string_free(&u);
}
END_TEST

/* ------------------------------------------------------------------ */

Suite *utf8_id_suite(void)
{
	Suite *s = suite_create("utils: utf8string uid/gid conversion");
	TCase *tc = tcase_create("Core");

	tcase_add_test(tc, test_from_uid_zero);
	tcase_add_test(tc, test_from_uid_nonzero);
	tcase_add_test(tc, test_from_uid_max);
	tcase_add_test(tc, test_from_gid_zero);
	tcase_add_test(tc, test_from_gid_nonzero);

	tcase_add_test(tc, test_to_uid_zero);
	tcase_add_test(tc, test_to_uid_valid);
	tcase_add_test(tc, test_to_uid_single_digit);

	tcase_add_test(tc, test_to_uid_null_string);
	tcase_add_test(tc, test_to_uid_empty_string);
	tcase_add_test(tc, test_to_uid_leading_zero);
	tcase_add_test(tc, test_to_uid_non_digit);
	tcase_add_test(tc, test_to_uid_all_alpha);
	tcase_add_test(tc, test_to_uid_overflow);

	tcase_add_test(tc, test_to_gid_leading_zero);
	tcase_add_test(tc, test_to_gid_overflow);

	tcase_add_test(tc, test_roundtrip_uid_zero);
	tcase_add_test(tc, test_roundtrip_uid_typical);
	tcase_add_test(tc, test_roundtrip_gid);

	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	return reffs_test_run_suite(utf8_id_suite(), NULL, NULL);
}
