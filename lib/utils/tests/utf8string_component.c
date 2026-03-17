/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <check.h>
#include "reffs/errno.h"
#include "reffs/utf8string.h"
#include "libreffs_test.h"

/* Helper: build a utf8string from raw bytes without NUL assumption. */
static utf8string make(const char *bytes, unsigned int len)
{
	utf8string u = { 0 };
	utf8string_alloc(&u, len);
	memcpy(u.utf8string_val, bytes, len);
	return u;
}

START_TEST(test_valid_components)
{
	utf8string u = { 0 };

	/* plain ASCII filename */
	cstr_to_utf8string(&u, "hello.txt");
	ck_assert_int_eq(utf8string_validate_component(&u, 0), 0);
	utf8string_free(&u);

	/* filename with spaces and punctuation */
	cstr_to_utf8string(&u, "my file (2026).tar.gz");
	ck_assert_int_eq(utf8string_validate_component(&u, 0), 0);
	utf8string_free(&u);

	/* valid 2-byte UTF-8: é */
	u = make("\xC3\xA9", 2);
	ck_assert_int_eq(utf8string_validate_component(&u, 0), 0);
	utf8string_free(&u);

	/* valid 3-byte UTF-8: 世 */
	u = make("\xE4\xB8\x96", 3);
	ck_assert_int_eq(utf8string_validate_component(&u, 0), 0);
	utf8string_free(&u);

	/* valid 4-byte UTF-8: 😀 U+1F600 */
	u = make("\xF0\x9F\x98\x80", 4);
	ck_assert_int_eq(utf8string_validate_component(&u, 0), 0);
	utf8string_free(&u);

	/* exactly name_max bytes */
	utf8string_alloc(&u, 10);
	memset(u.utf8string_val, 'a', 10);
	ck_assert_int_eq(utf8string_validate_component(&u, 10), 0);
	utf8string_free(&u);
}
END_TEST

START_TEST(test_einval)
{
	utf8string u = { 0 };
	/* null string */
	ck_assert_int_eq(utf8string_validate_component(&u, 0), -EINVAL);
}
END_TEST

START_TEST(test_enametoolong)
{
	utf8string u = { 0 };
	/* one byte over name_max */
	utf8string_alloc(&u, 11);
	memset(u.utf8string_val, 'a', 11);
	ck_assert_int_eq(utf8string_validate_component(&u, 10), -ENAMETOOLONG);
	utf8string_free(&u);

	/* default limit: 256 bytes */
	utf8string_alloc(&u, 256);
	memset(u.utf8string_val, 'x', 256);
	ck_assert_int_eq(utf8string_validate_component_default(&u),
			 -ENAMETOOLONG);
	utf8string_free(&u);
}
END_TEST

START_TEST(test_ebadname_forbidden)
{
	utf8string u = { 0 };

	/* slash */
	cstr_to_utf8string(&u, "foo/bar");
	ck_assert_int_eq(utf8string_validate_component(&u, 0), -EBADNAME);
	utf8string_free(&u);

	/* NUL byte embedded */
	{
		char b[] = { 'f', 'o', 'o', '\0', 'b', 'a', 'r' };
		u = make(b, 7);
	}
	ck_assert_int_eq(utf8string_validate_component(&u, 0), -EBADNAME);
	utf8string_free(&u);

	/* C0 control: U+0001 */
	u = make("\x01", 1);
	ck_assert_int_eq(utf8string_validate_component(&u, 0), -EBADNAME);
	utf8string_free(&u);

	/* C0 control: newline U+000A */
	u = make("foo\nbar", 7);
	ck_assert_int_eq(utf8string_validate_component(&u, 0), -EBADNAME);
	utf8string_free(&u);

	/* DEL U+007F */
	u = make("\x7F", 1);
	ck_assert_int_eq(utf8string_validate_component(&u, 0), -EBADNAME);
	utf8string_free(&u);

	/* C1 control U+0085 (encoded as 0xC2 0x85) */
	u = make("\xC2\x85", 2);
	ck_assert_int_eq(utf8string_validate_component(&u, 0), -EBADNAME);
	utf8string_free(&u);

	/* C1 control U+009F (encoded as 0xC2 0x9F) */
	u = make("\xC2\x9F", 2);
	ck_assert_int_eq(utf8string_validate_component(&u, 0), -EBADNAME);
	utf8string_free(&u);
}
END_TEST

START_TEST(test_ebadname_reserved)
{
	utf8string u = { 0 };

	cstr_to_utf8string(&u, ".");
	ck_assert_int_eq(utf8string_validate_component(&u, 0), -EBADNAME);
	utf8string_free(&u);

	cstr_to_utf8string(&u, "..");
	ck_assert_int_eq(utf8string_validate_component(&u, 0), -EBADNAME);
	utf8string_free(&u);

	/* "..." is NOT reserved — three dots is a legal filename */
	cstr_to_utf8string(&u, "...");
	ck_assert_int_eq(utf8string_validate_component(&u, 0), 0);
	utf8string_free(&u);

	/* ".hidden" is NOT reserved */
	cstr_to_utf8string(&u, ".hidden");
	ck_assert_int_eq(utf8string_validate_component(&u, 0), 0);
	utf8string_free(&u);
}
END_TEST

START_TEST(test_eilseq)
{
	utf8string u = { 0 };

	/* stray continuation byte */
	u = make("\x80", 1);
	ck_assert_int_eq(utf8string_validate_component(&u, 0), -EILSEQ);
	utf8string_free(&u);

	/* overlong encoding of 'A' */
	u = make("\xC1\x81", 2);
	ck_assert_int_eq(utf8string_validate_component(&u, 0), -EILSEQ);
	utf8string_free(&u);

	/* truncated 3-byte sequence */
	u = make("\xE4\xB8", 2);
	ck_assert_int_eq(utf8string_validate_component(&u, 0), -EILSEQ);
	utf8string_free(&u);

	/* surrogate half U+D800 */
	u = make("\xED\xA0\x80", 3);
	ck_assert_int_eq(utf8string_validate_component(&u, 0), -EILSEQ);
	utf8string_free(&u);

	/* above U+10FFFF */
	u = make("\xF4\x90\x80\x80", 4);
	ck_assert_int_eq(utf8string_validate_component(&u, 0), -EILSEQ);
	utf8string_free(&u);
}
END_TEST

Suite *utf8_suite(void)
{
	Suite *s = suite_create("utils: utf8string component");
	TCase *tc = tcase_create("Core");
	tcase_add_test(tc, test_valid_components);
	tcase_add_test(tc, test_einval);
	tcase_add_test(tc, test_enametoolong);
	tcase_add_test(tc, test_ebadname_forbidden);
	tcase_add_test(tc, test_ebadname_reserved);
	tcase_add_test(tc, test_eilseq);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	return reffs_test_run_suite(utf8_suite(), NULL, NULL);
}
