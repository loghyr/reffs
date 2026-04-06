/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * utf8string_lifecycle.c -- tests for utf8string lifecycle, predicate,
 * copy/move, wire copy, standalone validation, and cstr conversion.
 *
 * Tests:
 *   utf8string_alloc / utf8string_free
 *   utf8string_is_null
 *   cstr_to_utf8string / utf8string_to_cstr
 *   utf8string_copy / utf8string_move
 *   utf8string_from_wire / utf8string_from_wire_validated
 *   utf8string_validate
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <string.h>
#include <check.h>

#include "reffs/errno.h"
#include "reffs/utf8string.h"
#include "libreffs_test.h"

/* ------------------------------------------------------------------ */
/* utf8string_alloc / utf8string_free                                  */
/* ------------------------------------------------------------------ */

START_TEST(test_alloc_sets_len_and_val)
{
	utf8string u = { 0 };
	ck_assert_int_eq(utf8string_alloc(&u, 5), 0);
	ck_assert_uint_eq(u.utf8string_len, 5);
	ck_assert_ptr_nonnull(u.utf8string_val);
	/* calloc guarantees NUL pad at [len] */
	ck_assert_int_eq(u.utf8string_val[5], '\0');
	utf8string_free(&u);
}
END_TEST

START_TEST(test_alloc_zero_len)
{
	utf8string u = { 0 };
	ck_assert_int_eq(utf8string_alloc(&u, 0), 0);
	ck_assert_uint_eq(u.utf8string_len, 0);
	ck_assert_ptr_nonnull(u.utf8string_val);
	ck_assert_int_eq(u.utf8string_val[0], '\0');
	utf8string_free(&u);
}
END_TEST

START_TEST(test_free_clears_fields)
{
	utf8string u = { 0 };
	ck_assert_int_eq(utf8string_alloc(&u, 8), 0);
	utf8string_free(&u);
	ck_assert_ptr_null(u.utf8string_val);
	ck_assert_uint_eq(u.utf8string_len, 0);
}
END_TEST

START_TEST(test_free_null_pointer)
{
	/* Must not crash. */
	utf8string_free(NULL);
}
END_TEST

START_TEST(test_free_already_free)
{
	utf8string u = { 0 };
	utf8string_free(&u); /* val is NULL -- must not crash */
}
END_TEST

/* ------------------------------------------------------------------ */
/* utf8string_is_null                                                  */
/* ------------------------------------------------------------------ */

START_TEST(test_is_null_null_ptr)
{
	ck_assert(utf8string_is_null(NULL));
}
END_TEST

START_TEST(test_is_null_zero_len)
{
	utf8string u = { 0 };
	ck_assert(utf8string_is_null(&u));
}
END_TEST

START_TEST(test_is_null_nonempty)
{
	utf8string u = { 0 };
	cstr_to_utf8string(&u, "x");
	ck_assert(!utf8string_is_null(&u));
	utf8string_free(&u);
}
END_TEST

/* ------------------------------------------------------------------ */
/* cstr_to_utf8string / utf8string_to_cstr                            */
/* ------------------------------------------------------------------ */

START_TEST(test_cstr_to_utf8string_basic)
{
	utf8string u = { 0 };
	ck_assert_int_eq(cstr_to_utf8string(&u, "hello"), 0);
	ck_assert_uint_eq(u.utf8string_len, 5);
	ck_assert_int_eq(memcmp(u.utf8string_val, "hello", 5), 0);
	utf8string_free(&u);
}
END_TEST

START_TEST(test_cstr_to_utf8string_null)
{
	utf8string u = { 0 };
	ck_assert_int_eq(cstr_to_utf8string(&u, NULL), 0);
	ck_assert(utf8string_is_null(&u));
}
END_TEST

START_TEST(test_cstr_to_utf8string_empty)
{
	utf8string u = { 0 };
	ck_assert_int_eq(cstr_to_utf8string(&u, ""), 0);
	ck_assert(utf8string_is_null(&u));
}
END_TEST

START_TEST(test_cstr_to_utf8string_replaces_existing)
{
	utf8string u = { 0 };
	ck_assert_int_eq(cstr_to_utf8string(&u, "first"), 0);
	ck_assert_int_eq(cstr_to_utf8string(&u, "second"), 0);
	ck_assert_uint_eq(u.utf8string_len, 6);
	ck_assert_int_eq(memcmp(u.utf8string_val, "second", 6), 0);
	utf8string_free(&u);
}
END_TEST

START_TEST(test_utf8string_to_cstr_null_ptr)
{
	ck_assert_ptr_null(utf8string_to_cstr(NULL));
}
END_TEST

START_TEST(test_utf8string_to_cstr_empty)
{
	utf8string u = { 0 };
	/* val is NULL for an uninitialized string */
	ck_assert_ptr_null(utf8string_to_cstr(&u));
}
END_TEST

START_TEST(test_utf8string_to_cstr_content)
{
	utf8string u = { 0 };
	cstr_to_utf8string(&u, "reffs");
	ck_assert_str_eq(utf8string_to_cstr(&u), "reffs");
	utf8string_free(&u);
}
END_TEST

/* ------------------------------------------------------------------ */
/* utf8string_copy / utf8string_move                                  */
/* ------------------------------------------------------------------ */

START_TEST(test_copy_basic)
{
	utf8string src = { 0 }, dst = { 0 };
	cstr_to_utf8string(&src, "hello");
	ck_assert_int_eq(utf8string_copy(&dst, &src), 0);
	ck_assert_uint_eq(dst.utf8string_len, src.utf8string_len);
	ck_assert_int_eq(memcmp(dst.utf8string_val, src.utf8string_val, 5), 0);
	/* independent allocations */
	ck_assert_ptr_ne(dst.utf8string_val, src.utf8string_val);
	utf8string_free(&src);
	utf8string_free(&dst);
}
END_TEST

START_TEST(test_copy_null_src)
{
	utf8string dst = { 0 };
	cstr_to_utf8string(&dst, "old");
	ck_assert_int_eq(utf8string_copy(&dst, NULL), 0);
	ck_assert(utf8string_is_null(&dst));
}
END_TEST

START_TEST(test_copy_empty_src)
{
	utf8string src = { 0 }, dst = { 0 };
	cstr_to_utf8string(&dst, "old");
	ck_assert_int_eq(utf8string_copy(&dst, &src), 0);
	ck_assert(utf8string_is_null(&dst));
}
END_TEST

START_TEST(test_move_basic)
{
	utf8string src = { 0 }, dst = { 0 };
	cstr_to_utf8string(&src, "transfer");
	char *orig_val = src.utf8string_val;

	utf8string_move(&dst, &src);

	/* dst owns the original buffer */
	ck_assert_ptr_eq(dst.utf8string_val, orig_val);
	ck_assert_uint_eq(dst.utf8string_len, 8);
	/* src is zeroed */
	ck_assert_ptr_null(src.utf8string_val);
	ck_assert_uint_eq(src.utf8string_len, 0);

	utf8string_free(&dst);
}
END_TEST

START_TEST(test_move_self)
{
	utf8string u = { 0 };
	cstr_to_utf8string(&u, "self");
	char *orig_val = u.utf8string_val;

	utf8string_move(&u, &u);

	/* no-op: content unchanged */
	ck_assert_ptr_eq(u.utf8string_val, orig_val);
	ck_assert_uint_eq(u.utf8string_len, 4);
	utf8string_free(&u);
}
END_TEST

/* ------------------------------------------------------------------ */
/* utf8string_from_wire / utf8string_from_wire_validated              */
/* ------------------------------------------------------------------ */

START_TEST(test_from_wire_basic)
{
	utf8string wire = { 0 }, dst = { 0 };
	cstr_to_utf8string(&wire, "wire");

	ck_assert_int_eq(utf8string_from_wire(&dst, &wire), 0);
	ck_assert_uint_eq(dst.utf8string_len, 4);
	ck_assert_int_eq(memcmp(dst.utf8string_val, "wire", 4), 0);
	/* independent copy */
	ck_assert_ptr_ne(dst.utf8string_val, wire.utf8string_val);

	utf8string_free(&wire);
	utf8string_free(&dst);
}
END_TEST

START_TEST(test_from_wire_null_wire)
{
	utf8string dst = { 0 };
	cstr_to_utf8string(&dst, "old");
	ck_assert_int_eq(utf8string_from_wire(&dst, NULL), 0);
	ck_assert(utf8string_is_null(&dst));
}
END_TEST

START_TEST(test_from_wire_zero_len)
{
	utf8string wire = { 0 }, dst = { 0 };
	cstr_to_utf8string(&dst, "old");
	/* zero-len wire clears dst */
	ck_assert_int_eq(utf8string_from_wire(&dst, &wire), 0);
	ck_assert(utf8string_is_null(&dst));
}
END_TEST

START_TEST(test_from_wire_null_val_einval)
{
	utf8string wire = { .utf8string_len = 3, .utf8string_val = NULL };
	utf8string dst = { 0 };
	ck_assert_int_eq(utf8string_from_wire(&dst, &wire), -EINVAL);
}
END_TEST

START_TEST(test_from_wire_validated_valid)
{
	utf8string wire = { 0 }, dst = { 0 };
	cstr_to_utf8string(&wire, "valid");
	ck_assert_int_eq(utf8string_from_wire_validated(&dst, &wire), 0);
	ck_assert_uint_eq(dst.utf8string_len, 5);
	utf8string_free(&wire);
	utf8string_free(&dst);
}
END_TEST

START_TEST(test_from_wire_validated_rejects_invalid_utf8)
{
	/* stray continuation byte */
	utf8string wire = { .utf8string_len = 1, .utf8string_val = "\x80" };
	utf8string dst = { 0 };
	ck_assert_int_eq(utf8string_from_wire_validated(&dst, &wire), -EILSEQ);
	/* dst must be freed / zeroed on failure */
	ck_assert(utf8string_is_null(&dst));
}
END_TEST

/* ------------------------------------------------------------------ */
/* utf8string_validate (standalone)                                   */
/* ------------------------------------------------------------------ */

START_TEST(test_validate_null_ptr)
{
	ck_assert_int_eq(utf8string_validate(NULL), -EINVAL);
}
END_TEST

START_TEST(test_validate_empty)
{
	utf8string u = { 0 };
	ck_assert_int_eq(utf8string_validate(&u), 0);
}
END_TEST

START_TEST(test_validate_ascii)
{
	utf8string u = { 0 };
	cstr_to_utf8string(&u, "hello");
	ck_assert_int_eq(utf8string_validate(&u), 0);
	utf8string_free(&u);
}
END_TEST

START_TEST(test_validate_multibyte)
{
	utf8string u = { 0 };
	/* e (U+00E9) = 0xC3 0xA9, 世 (U+4E16) = 0xE4 0xB8 0x96 */
	u.utf8string_len = 5;
	u.utf8string_val = "\xC3\xA9\xE4\xB8\x96";
	ck_assert_int_eq(utf8string_validate(&u), 0);
}
END_TEST

START_TEST(test_validate_stray_continuation)
{
	utf8string u = { .utf8string_len = 1, .utf8string_val = "\x80" };
	ck_assert_int_eq(utf8string_validate(&u), -EILSEQ);
}
END_TEST

START_TEST(test_validate_overlong_2byte)
{
	/* Overlong encoding of U+0041 'A' as 0xC1 0x81 */
	utf8string u = { .utf8string_len = 2, .utf8string_val = "\xC1\x81" };
	ck_assert_int_eq(utf8string_validate(&u), -EILSEQ);
}
END_TEST

START_TEST(test_validate_truncated_sequence)
{
	/* 3-byte leader followed by only one continuation byte */
	utf8string u = { .utf8string_len = 2, .utf8string_val = "\xE4\xB8" };
	ck_assert_int_eq(utf8string_validate(&u), -EILSEQ);
}
END_TEST

START_TEST(test_validate_surrogate_half)
{
	/* U+D800 = 0xED 0xA0 0x80 */
	utf8string u = { .utf8string_len = 3,
			 .utf8string_val = "\xED\xA0\x80" };
	ck_assert_int_eq(utf8string_validate(&u), -EILSEQ);
}
END_TEST

START_TEST(test_validate_above_unicode_range)
{
	/* U+110000 encoded as 0xF4 0x90 0x80 0x80 */
	utf8string u = { .utf8string_len = 4,
			 .utf8string_val = "\xF4\x90\x80\x80" };
	ck_assert_int_eq(utf8string_validate(&u), -EILSEQ);
}
END_TEST

START_TEST(test_validate_noncharacter_fffe)
{
	/* U+FFFE = 0xEF 0xBF 0xBE */
	utf8string u = { .utf8string_len = 3,
			 .utf8string_val = "\xEF\xBF\xBE" };
	ck_assert_int_eq(utf8string_validate(&u), -EILSEQ);
}
END_TEST

/* ------------------------------------------------------------------ */

Suite *utf8_lifecycle_suite(void)
{
	Suite *s = suite_create("utils: utf8string lifecycle");
	TCase *tc = tcase_create("Core");

	tcase_add_test(tc, test_alloc_sets_len_and_val);
	tcase_add_test(tc, test_alloc_zero_len);
	tcase_add_test(tc, test_free_clears_fields);
	tcase_add_test(tc, test_free_null_pointer);
	tcase_add_test(tc, test_free_already_free);

	tcase_add_test(tc, test_is_null_null_ptr);
	tcase_add_test(tc, test_is_null_zero_len);
	tcase_add_test(tc, test_is_null_nonempty);

	tcase_add_test(tc, test_cstr_to_utf8string_basic);
	tcase_add_test(tc, test_cstr_to_utf8string_null);
	tcase_add_test(tc, test_cstr_to_utf8string_empty);
	tcase_add_test(tc, test_cstr_to_utf8string_replaces_existing);
	tcase_add_test(tc, test_utf8string_to_cstr_null_ptr);
	tcase_add_test(tc, test_utf8string_to_cstr_empty);
	tcase_add_test(tc, test_utf8string_to_cstr_content);

	tcase_add_test(tc, test_copy_basic);
	tcase_add_test(tc, test_copy_null_src);
	tcase_add_test(tc, test_copy_empty_src);
	tcase_add_test(tc, test_move_basic);
	tcase_add_test(tc, test_move_self);

	tcase_add_test(tc, test_from_wire_basic);
	tcase_add_test(tc, test_from_wire_null_wire);
	tcase_add_test(tc, test_from_wire_zero_len);
	tcase_add_test(tc, test_from_wire_null_val_einval);
	tcase_add_test(tc, test_from_wire_validated_valid);
	tcase_add_test(tc, test_from_wire_validated_rejects_invalid_utf8);

	tcase_add_test(tc, test_validate_null_ptr);
	tcase_add_test(tc, test_validate_empty);
	tcase_add_test(tc, test_validate_ascii);
	tcase_add_test(tc, test_validate_multibyte);
	tcase_add_test(tc, test_validate_stray_continuation);
	tcase_add_test(tc, test_validate_overlong_2byte);
	tcase_add_test(tc, test_validate_truncated_sequence);
	tcase_add_test(tc, test_validate_surrogate_half);
	tcase_add_test(tc, test_validate_above_unicode_range);
	tcase_add_test(tc, test_validate_noncharacter_fffe);

	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	return reffs_test_run_suite(utf8_lifecycle_suite(), NULL, NULL);
}
