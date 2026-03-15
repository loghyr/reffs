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
#include "reffs/errno.h"
#include "reffs/utf8string.h"

static int passed, failed;

#define CHECK(expr)                                                     \
	do {                                                            \
		if (!(expr)) {                                          \
			fprintf(stderr, "FAIL: %s  (line %d)\n", #expr, \
				__LINE__);                              \
			failed++;                                       \
		} else {                                                \
			printf("OK:   %s\n", #expr);                    \
			passed++;                                       \
		}                                                       \
	} while (0)

/* Helper: build a utf8string from raw bytes without NUL assumption. */
static utf8string make(const char *bytes, unsigned int len)
{
	utf8string u = { 0 };
	utf8string_alloc(&u, len);
	memcpy(u.utf8string_val, bytes, len);
	return u;
}

int main(void)
{
	utf8string u = { 0 };

	puts("--- valid components ---");

	/* plain ASCII filename */
	cstr_to_utf8string(&u, "hello.txt");
	CHECK(utf8string_validate_component(&u, 0) == 0);
	utf8string_free(&u);

	/* filename with spaces and punctuation */
	cstr_to_utf8string(&u, "my file (2026).tar.gz");
	CHECK(utf8string_validate_component(&u, 0) == 0);
	utf8string_free(&u);

	/* valid 2-byte UTF-8: é */
	u = make("\xC3\xA9", 2);
	CHECK(utf8string_validate_component(&u, 0) == 0);
	utf8string_free(&u);

	/* valid 3-byte UTF-8: 世 */
	u = make("\xE4\xB8\x96", 3);
	CHECK(utf8string_validate_component(&u, 0) == 0);
	utf8string_free(&u);

	/* valid 4-byte UTF-8: 😀 U+1F600 */
	u = make("\xF0\x9F\x98\x80", 4);
	CHECK(utf8string_validate_component(&u, 0) == 0);
	utf8string_free(&u);

	/* exactly name_max bytes */
	{
		utf8string_alloc(&u, 10);
		memset(u.utf8string_val, 'a', 10);
		CHECK(utf8string_validate_component(&u, 10) == 0);
		utf8string_free(&u);
	}

	puts("\n--- EINVAL ---");

	/* null string */
	CHECK(utf8string_validate_component(&u, 0) == -EINVAL);

	puts("\n--- ENAMETOOLONG ---");

	/* one byte over name_max */
	{
		utf8string_alloc(&u, 11);
		memset(u.utf8string_val, 'a', 11);
		CHECK(utf8string_validate_component(&u, 10) == -ENAMETOOLONG);
		utf8string_free(&u);
	}

	/* default limit: 256 bytes */
	{
		utf8string_alloc(&u, 256);
		memset(u.utf8string_val, 'x', 256);
		CHECK(utf8string_validate_component_default(&u) ==
		      -ENAMETOOLONG);
		utf8string_free(&u);
	}

	puts("\n--- EBADNAME: forbidden characters ---");

	/* slash */
	cstr_to_utf8string(&u, "foo/bar");
	CHECK(utf8string_validate_component(&u, 0) == -EBADNAME);
	utf8string_free(&u);

	/* NUL byte embedded — must use explicit array; C string literals
     * truncate at the first NUL so make("foo\x00bar",7) copies garbage. */
	{
		char b[] = { 'f', 'o', 'o', '\0', 'b', 'a', 'r' };
		u = make(b, 7);
	}
	CHECK(utf8string_validate_component(&u, 0) == -EBADNAME);
	utf8string_free(&u);

	/* C0 control: U+0001 */
	u = make("\x01", 1);
	CHECK(utf8string_validate_component(&u, 0) == -EBADNAME);
	utf8string_free(&u);

	/* C0 control: newline U+000A */
	u = make("foo\nbar", 7);
	CHECK(utf8string_validate_component(&u, 0) == -EBADNAME);
	utf8string_free(&u);

	/* DEL U+007F */
	u = make("\x7F", 1);
	CHECK(utf8string_validate_component(&u, 0) == -EBADNAME);
	utf8string_free(&u);

	/* C1 control U+0085 (encoded as 0xC2 0x85) */
	u = make("\xC2\x85", 2);
	CHECK(utf8string_validate_component(&u, 0) == -EBADNAME);
	utf8string_free(&u);

	/* C1 control U+009F (encoded as 0xC2 0x9F) */
	u = make("\xC2\x9F", 2);
	CHECK(utf8string_validate_component(&u, 0) == -EBADNAME);
	utf8string_free(&u);

	puts("\n--- EBADNAME: reserved names ---");

	cstr_to_utf8string(&u, ".");
	CHECK(utf8string_validate_component(&u, 0) == -EBADNAME);
	utf8string_free(&u);

	cstr_to_utf8string(&u, "..");
	CHECK(utf8string_validate_component(&u, 0) == -EBADNAME);
	utf8string_free(&u);

	/* "..." is NOT reserved — three dots is a legal filename */
	cstr_to_utf8string(&u, "...");
	CHECK(utf8string_validate_component(&u, 0) == 0);
	utf8string_free(&u);

	/* ".hidden" is NOT reserved */
	cstr_to_utf8string(&u, ".hidden");
	CHECK(utf8string_validate_component(&u, 0) == 0);
	utf8string_free(&u);

	puts("\n--- EILSEQ: bad UTF-8 ---");

	/* stray continuation byte */
	u = make("\x80", 1);
	CHECK(utf8string_validate_component(&u, 0) == -EILSEQ);
	utf8string_free(&u);

	/* overlong encoding of 'A' */
	u = make("\xC1\x81", 2);
	CHECK(utf8string_validate_component(&u, 0) == -EILSEQ);
	utf8string_free(&u);

	/* truncated 3-byte sequence */
	u = make("\xE4\xB8", 2);
	CHECK(utf8string_validate_component(&u, 0) == -EILSEQ);
	utf8string_free(&u);

	/* surrogate half U+D800 */
	u = make("\xED\xA0\x80", 3);
	CHECK(utf8string_validate_component(&u, 0) == -EILSEQ);
	utf8string_free(&u);

	/* above U+10FFFF */
	u = make("\xF4\x90\x80\x80", 4);
	CHECK(utf8string_validate_component(&u, 0) == -EILSEQ);
	utf8string_free(&u);

	printf("\n%d passed, %d failed\n", passed, failed);
	return failed ? 1 : 0;
}
