/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * cmp.c -- tests for reffs_case_set/get and reffs_text_case_cmp/_of.
 *
 * Tests:
 *   reffs_case_get / reffs_case_set
 *   reffs_text_case_cmp  (respects global reffs_rtc)
 *   reffs_text_case_cmp_of  (ignores global reffs_rtc)
 *
 * Each test that mutates the global resets it in a teardown fixture so
 * tests remain independent of execution order.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <check.h>

#include "reffs/cmp.h"
#include "libreffs_test.h"

static void teardown(void)
{
	reffs_case_set(reffs_text_case_sensitive);
}

/* ------------------------------------------------------------------ */
/* reffs_case_get / reffs_case_set                                     */
/* ------------------------------------------------------------------ */

START_TEST(test_default_is_sensitive)
{
	ck_assert_int_eq(reffs_case_get(), reffs_text_case_sensitive);
}
END_TEST

START_TEST(test_set_insensitive)
{
	reffs_case_set(reffs_text_case_insensitive);
	ck_assert_int_eq(reffs_case_get(), reffs_text_case_insensitive);
}
END_TEST

START_TEST(test_set_sensitive_after_insensitive)
{
	reffs_case_set(reffs_text_case_insensitive);
	reffs_case_set(reffs_text_case_sensitive);
	ck_assert_int_eq(reffs_case_get(), reffs_text_case_sensitive);
}
END_TEST

/* ------------------------------------------------------------------ */
/* reffs_text_case_cmp -- returns function that honours the global      */
/* ------------------------------------------------------------------ */

START_TEST(test_cmp_sensitive_equal)
{
	reffs_strng_compare cmp = reffs_text_case_cmp();
	ck_assert_int_eq(cmp("abc", "abc"), 0);
}
END_TEST

START_TEST(test_cmp_sensitive_different_case)
{
	/* "ABC" < "abc" in ASCII order when case-sensitive */
	reffs_strng_compare cmp = reffs_text_case_cmp();
	ck_assert_int_lt(cmp("ABC", "abc"), 0);
}
END_TEST

START_TEST(test_cmp_sensitive_unequal)
{
	reffs_strng_compare cmp = reffs_text_case_cmp();
	ck_assert_int_ne(cmp("abc", "def"), 0);
}
END_TEST

START_TEST(test_cmp_insensitive_equal_mixed_case)
{
	reffs_case_set(reffs_text_case_insensitive);
	reffs_strng_compare cmp = reffs_text_case_cmp();
	ck_assert_int_eq(cmp("ABC", "abc"), 0);
}
END_TEST

START_TEST(test_cmp_insensitive_equal_upper)
{
	reffs_case_set(reffs_text_case_insensitive);
	reffs_strng_compare cmp = reffs_text_case_cmp();
	ck_assert_int_eq(cmp("HELLO", "hello"), 0);
}
END_TEST

START_TEST(test_cmp_insensitive_unequal)
{
	reffs_case_set(reffs_text_case_insensitive);
	reffs_strng_compare cmp = reffs_text_case_cmp();
	ck_assert_int_ne(cmp("abc", "def"), 0);
}
END_TEST

START_TEST(test_cmp_switch_updates_function)
{
	reffs_strng_compare cmp;

	/* Sensitive: "ABC" != "abc" */
	cmp = reffs_text_case_cmp();
	ck_assert_int_ne(cmp("ABC", "abc"), 0);

	/* Switch to insensitive: fresh call returns the other fn */
	reffs_case_set(reffs_text_case_insensitive);
	cmp = reffs_text_case_cmp();
	ck_assert_int_eq(cmp("ABC", "abc"), 0);
}
END_TEST

/* ------------------------------------------------------------------ */
/* reffs_text_case_cmp_of -- ignores global, uses explicit argument     */
/* ------------------------------------------------------------------ */

START_TEST(test_cmp_of_sensitive_equal)
{
	reffs_strng_compare cmp =
		reffs_text_case_cmp_of(reffs_text_case_sensitive);
	ck_assert_int_eq(cmp("abc", "abc"), 0);
}
END_TEST

START_TEST(test_cmp_of_sensitive_different_case)
{
	reffs_strng_compare cmp =
		reffs_text_case_cmp_of(reffs_text_case_sensitive);
	ck_assert_int_lt(cmp("ABC", "abc"), 0);
}
END_TEST

START_TEST(test_cmp_of_insensitive_equal)
{
	reffs_strng_compare cmp =
		reffs_text_case_cmp_of(reffs_text_case_insensitive);
	ck_assert_int_eq(cmp("NFS4", "nfs4"), 0);
}
END_TEST

START_TEST(test_cmp_of_insensitive_unequal)
{
	reffs_strng_compare cmp =
		reffs_text_case_cmp_of(reffs_text_case_insensitive);
	ck_assert_int_ne(cmp("nfs4", "nfs3"), 0);
}
END_TEST

START_TEST(test_cmp_of_sensitive_ignores_global_insensitive)
{
	/* Global is insensitive -- cmp_of(sensitive) must still be strict. */
	reffs_case_set(reffs_text_case_insensitive);
	reffs_strng_compare cmp =
		reffs_text_case_cmp_of(reffs_text_case_sensitive);
	ck_assert_int_ne(cmp("ABC", "abc"), 0);
}
END_TEST

START_TEST(test_cmp_of_insensitive_ignores_global_sensitive)
{
	/* Global is sensitive (default) -- cmp_of(insensitive) folds case. */
	reffs_strng_compare cmp =
		reffs_text_case_cmp_of(reffs_text_case_insensitive);
	ck_assert_int_eq(cmp("ABC", "abc"), 0);
}
END_TEST

/* ------------------------------------------------------------------ */

Suite *cmp_suite(void)
{
	Suite *s = suite_create("utils: case sensitivity (cmp)");

	TCase *tc_getset = tcase_create("get/set");
	tcase_add_checked_fixture(tc_getset, NULL, teardown);
	tcase_add_test(tc_getset, test_default_is_sensitive);
	tcase_add_test(tc_getset, test_set_insensitive);
	tcase_add_test(tc_getset, test_set_sensitive_after_insensitive);
	suite_add_tcase(s, tc_getset);

	TCase *tc_cmp = tcase_create("reffs_text_case_cmp");
	tcase_add_checked_fixture(tc_cmp, NULL, teardown);
	tcase_add_test(tc_cmp, test_cmp_sensitive_equal);
	tcase_add_test(tc_cmp, test_cmp_sensitive_different_case);
	tcase_add_test(tc_cmp, test_cmp_sensitive_unequal);
	tcase_add_test(tc_cmp, test_cmp_insensitive_equal_mixed_case);
	tcase_add_test(tc_cmp, test_cmp_insensitive_equal_upper);
	tcase_add_test(tc_cmp, test_cmp_insensitive_unequal);
	tcase_add_test(tc_cmp, test_cmp_switch_updates_function);
	suite_add_tcase(s, tc_cmp);

	TCase *tc_of = tcase_create("reffs_text_case_cmp_of");
	tcase_add_checked_fixture(tc_of, NULL, teardown);
	tcase_add_test(tc_of, test_cmp_of_sensitive_equal);
	tcase_add_test(tc_of, test_cmp_of_sensitive_different_case);
	tcase_add_test(tc_of, test_cmp_of_insensitive_equal);
	tcase_add_test(tc_of, test_cmp_of_insensitive_unequal);
	tcase_add_test(tc_of, test_cmp_of_sensitive_ignores_global_insensitive);
	tcase_add_test(tc_of, test_cmp_of_insensitive_ignores_global_sensitive);
	suite_add_tcase(s, tc_of);

	return s;
}

int main(void)
{
	return reffs_test_run_suite(cmp_suite(), NULL, NULL);
}
