/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * utf8string_compare.c — tests for all utf8string comparison functions.
 *
 * Tests:
 *   utf8string_cmp / utf8string_casecmp
 *   utf8string_eq  / utf8string_caseeq
 *   utf8string_cmp_cstr / utf8string_casecmp_cstr
 *   utf8string_eq_cstr  / utf8string_caseeq_cstr
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <string.h>
#include <check.h>

#include "reffs/utf8string.h"
#include "libreffs_test.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static utf8string make_str(const char *s)
{
	utf8string u = { 0 };
	cstr_to_utf8string(&u, s);
	return u;
}

/* ------------------------------------------------------------------ */
/* utf8string_cmp (utf8string vs utf8string)                          */
/* ------------------------------------------------------------------ */

START_TEST(test_cmp_equal)
{
	utf8string a = make_str("hello"), b = make_str("hello");
	ck_assert_int_eq(utf8string_cmp(&a, &b), 0);
	utf8string_free(&a);
	utf8string_free(&b);
}
END_TEST

START_TEST(test_cmp_less)
{
	utf8string a = make_str("abc"), b = make_str("abd");
	ck_assert_int_lt(utf8string_cmp(&a, &b), 0);
	utf8string_free(&a);
	utf8string_free(&b);
}
END_TEST

START_TEST(test_cmp_greater)
{
	utf8string a = make_str("abd"), b = make_str("abc");
	ck_assert_int_gt(utf8string_cmp(&a, &b), 0);
	utf8string_free(&a);
	utf8string_free(&b);
}
END_TEST

START_TEST(test_cmp_prefix_shorter_is_less)
{
	utf8string a = make_str("abc"), b = make_str("abcd");
	ck_assert_int_lt(utf8string_cmp(&a, &b), 0);
	ck_assert_int_gt(utf8string_cmp(&b, &a), 0);
	utf8string_free(&a);
	utf8string_free(&b);
}
END_TEST

START_TEST(test_cmp_null_a_less_than_nonnull)
{
	utf8string b = make_str("x");
	ck_assert_int_lt(utf8string_cmp(NULL, &b), 0);
	utf8string_free(&b);
}
END_TEST

START_TEST(test_cmp_null_b_greater)
{
	utf8string a = make_str("x");
	ck_assert_int_gt(utf8string_cmp(&a, NULL), 0);
	utf8string_free(&a);
}
END_TEST

START_TEST(test_cmp_same_pointer)
{
	utf8string a = make_str("same");
	ck_assert_int_eq(utf8string_cmp(&a, &a), 0);
	utf8string_free(&a);
}
END_TEST

START_TEST(test_cmp_case_sensitive)
{
	utf8string a = make_str("ABC"), b = make_str("abc");
	/* 'A' (0x41) < 'a' (0x61) in ASCII order */
	ck_assert_int_lt(utf8string_cmp(&a, &b), 0);
	utf8string_free(&a);
	utf8string_free(&b);
}
END_TEST

/* ------------------------------------------------------------------ */
/* utf8string_casecmp                                                  */
/* ------------------------------------------------------------------ */

START_TEST(test_casecmp_equal_different_case)
{
	utf8string a = make_str("Hello"), b = make_str("HELLO");
	ck_assert_int_eq(utf8string_casecmp(&a, &b), 0);
	utf8string_free(&a);
	utf8string_free(&b);
}
END_TEST

START_TEST(test_casecmp_orders_by_char_not_case)
{
	utf8string a = make_str("apple"), b = make_str("BANANA");
	ck_assert_int_lt(utf8string_casecmp(&a, &b), 0);
	utf8string_free(&a);
	utf8string_free(&b);
}
END_TEST

START_TEST(test_casecmp_prefix)
{
	utf8string a = make_str("abc"), b = make_str("ABCD");
	ck_assert_int_lt(utf8string_casecmp(&a, &b), 0);
	utf8string_free(&a);
	utf8string_free(&b);
}
END_TEST

/* ------------------------------------------------------------------ */
/* utf8string_eq / utf8string_caseeq                                  */
/* ------------------------------------------------------------------ */

START_TEST(test_eq_true)
{
	utf8string a = make_str("reffs"), b = make_str("reffs");
	ck_assert(utf8string_eq(&a, &b));
	utf8string_free(&a);
	utf8string_free(&b);
}
END_TEST

START_TEST(test_eq_false)
{
	utf8string a = make_str("reffs"), b = make_str("REFFS");
	ck_assert(!utf8string_eq(&a, &b));
	utf8string_free(&a);
	utf8string_free(&b);
}
END_TEST

START_TEST(test_caseeq_true)
{
	utf8string a = make_str("NFS4"), b = make_str("nfs4");
	ck_assert(utf8string_caseeq(&a, &b));
	utf8string_free(&a);
	utf8string_free(&b);
}
END_TEST

START_TEST(test_caseeq_false)
{
	utf8string a = make_str("nfs4"), b = make_str("nfs3");
	ck_assert(!utf8string_caseeq(&a, &b));
	utf8string_free(&a);
	utf8string_free(&b);
}
END_TEST

/* ------------------------------------------------------------------ */
/* utf8string_cmp_cstr                                                 */
/* ------------------------------------------------------------------ */

START_TEST(test_cmp_cstr_equal)
{
	utf8string u = make_str("hello");
	ck_assert_int_eq(utf8string_cmp_cstr(&u, "hello"), 0);
	utf8string_free(&u);
}
END_TEST

START_TEST(test_cmp_cstr_less)
{
	utf8string u = make_str("abc");
	ck_assert_int_lt(utf8string_cmp_cstr(&u, "abd"), 0);
	utf8string_free(&u);
}
END_TEST

START_TEST(test_cmp_cstr_greater)
{
	utf8string u = make_str("abd");
	ck_assert_int_gt(utf8string_cmp_cstr(&u, "abc"), 0);
	utf8string_free(&u);
}
END_TEST

START_TEST(test_cmp_cstr_utf8_shorter)
{
	utf8string u = make_str("abc");
	ck_assert_int_lt(utf8string_cmp_cstr(&u, "abcd"), 0);
	utf8string_free(&u);
}
END_TEST

START_TEST(test_cmp_cstr_utf8_longer)
{
	utf8string u = make_str("abcd");
	ck_assert_int_gt(utf8string_cmp_cstr(&u, "abc"), 0);
	utf8string_free(&u);
}
END_TEST

START_TEST(test_cmp_cstr_null_cstr_treated_as_empty)
{
	utf8string empty = { 0 };
	/* Both sides effectively empty → equal */
	ck_assert_int_eq(utf8string_cmp_cstr(&empty, NULL), 0);
}
END_TEST

START_TEST(test_cmp_cstr_null_utf8)
{
	/* NULL utf8string treated as empty; non-empty cstr is greater */
	ck_assert_int_lt(utf8string_cmp_cstr(NULL, "x"), 0);
}
END_TEST

START_TEST(test_cmp_cstr_case_sensitive)
{
	utf8string u = make_str("ABC");
	ck_assert_int_lt(utf8string_cmp_cstr(&u, "abc"), 0);
	utf8string_free(&u);
}
END_TEST

/* ------------------------------------------------------------------ */
/* utf8string_casecmp_cstr                                             */
/* ------------------------------------------------------------------ */

START_TEST(test_casecmp_cstr_equal_different_case)
{
	utf8string u = make_str("Hello");
	ck_assert_int_eq(utf8string_casecmp_cstr(&u, "HELLO"), 0);
	utf8string_free(&u);
}
END_TEST

START_TEST(test_casecmp_cstr_not_equal)
{
	utf8string u = make_str("nfs4");
	ck_assert_int_ne(utf8string_casecmp_cstr(&u, "nfs3"), 0);
	utf8string_free(&u);
}
END_TEST

/* ------------------------------------------------------------------ */
/* utf8string_eq_cstr / utf8string_caseeq_cstr                        */
/* ------------------------------------------------------------------ */

START_TEST(test_eq_cstr_true)
{
	utf8string u = make_str("match");
	ck_assert(utf8string_eq_cstr(&u, "match"));
	utf8string_free(&u);
}
END_TEST

START_TEST(test_eq_cstr_false_case)
{
	utf8string u = make_str("Match");
	ck_assert(!utf8string_eq_cstr(&u, "match"));
	utf8string_free(&u);
}
END_TEST

START_TEST(test_eq_cstr_false_content)
{
	utf8string u = make_str("foo");
	ck_assert(!utf8string_eq_cstr(&u, "bar"));
	utf8string_free(&u);
}
END_TEST

START_TEST(test_caseeq_cstr_true)
{
	utf8string u = make_str("NFS4");
	ck_assert(utf8string_caseeq_cstr(&u, "nfs4"));
	utf8string_free(&u);
}
END_TEST

START_TEST(test_caseeq_cstr_false)
{
	utf8string u = make_str("nfs4");
	ck_assert(!utf8string_caseeq_cstr(&u, "nfs3"));
	utf8string_free(&u);
}
END_TEST

/* ------------------------------------------------------------------ */

Suite *utf8_compare_suite(void)
{
	Suite *s = suite_create("utils: utf8string comparison");
	TCase *tc = tcase_create("Core");

	tcase_add_test(tc, test_cmp_equal);
	tcase_add_test(tc, test_cmp_less);
	tcase_add_test(tc, test_cmp_greater);
	tcase_add_test(tc, test_cmp_prefix_shorter_is_less);
	tcase_add_test(tc, test_cmp_null_a_less_than_nonnull);
	tcase_add_test(tc, test_cmp_null_b_greater);
	tcase_add_test(tc, test_cmp_same_pointer);
	tcase_add_test(tc, test_cmp_case_sensitive);

	tcase_add_test(tc, test_casecmp_equal_different_case);
	tcase_add_test(tc, test_casecmp_orders_by_char_not_case);
	tcase_add_test(tc, test_casecmp_prefix);

	tcase_add_test(tc, test_eq_true);
	tcase_add_test(tc, test_eq_false);
	tcase_add_test(tc, test_caseeq_true);
	tcase_add_test(tc, test_caseeq_false);

	tcase_add_test(tc, test_cmp_cstr_equal);
	tcase_add_test(tc, test_cmp_cstr_less);
	tcase_add_test(tc, test_cmp_cstr_greater);
	tcase_add_test(tc, test_cmp_cstr_utf8_shorter);
	tcase_add_test(tc, test_cmp_cstr_utf8_longer);
	tcase_add_test(tc, test_cmp_cstr_null_cstr_treated_as_empty);
	tcase_add_test(tc, test_cmp_cstr_null_utf8);
	tcase_add_test(tc, test_cmp_cstr_case_sensitive);

	tcase_add_test(tc, test_casecmp_cstr_equal_different_case);
	tcase_add_test(tc, test_casecmp_cstr_not_equal);

	tcase_add_test(tc, test_eq_cstr_true);
	tcase_add_test(tc, test_eq_cstr_false_case);
	tcase_add_test(tc, test_eq_cstr_false_content);
	tcase_add_test(tc, test_caseeq_cstr_true);
	tcase_add_test(tc, test_caseeq_cstr_false);

	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	return reffs_test_run_suite(utf8_compare_suite(), NULL, NULL);
}
