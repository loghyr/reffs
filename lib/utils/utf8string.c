/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/*
 * utf8string.c - NFSv4.2 utf8string utility functions
 */

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strncasecmp */

#include "reffs/utf8string.h"
#include "reffs/errno.h"

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

static inline const char *_utf8_val_or_empty(const utf8string *u)
{
	return (u->utf8string_val != NULL) ? u->utf8string_val : "";
}

static inline const char *_cstr_safe(const char *s)
{
	return (s != NULL) ? s : "";
}

/* =========================================================================
 * Lifecycle
 * ========================================================================= */

int utf8string_alloc(utf8string *u, unsigned int len)
{
	char *buf;

	buf = calloc(1, (size_t)len + 1);
	if (buf == NULL)
		return -ENOMEM;

	u->utf8string_len = len;
	u->utf8string_val = buf;
	return 0;
}

void utf8string_free(utf8string *u)
{
	if (u == NULL)
		return;
	free(u->utf8string_val);
	u->utf8string_val = NULL;
	u->utf8string_len = 0;
}

int utf8string_from_wire(utf8string *dst, const utf8string *wire)
{
	int rc;

	utf8string_free(dst);

	if (wire == NULL || wire->utf8string_len == 0)
		return 0;

	if (wire->utf8string_val == NULL)
		return -EINVAL;

	rc = utf8string_alloc(dst, wire->utf8string_len);
	if (rc != 0)
		return rc;

	/*
	 * Copy exactly len bytes.  No strlen, no NUL-termination assumption.
	 * The NUL pad at dst->utf8string_val[len] is already in place from
	 * calloc inside utf8string_alloc.
	 */
	memcpy(dst->utf8string_val, wire->utf8string_val, wire->utf8string_len);
	return 0;
}

/* =========================================================================
 * Validation
 * ========================================================================= */

/*
 * _utf8_sequence_len - return the expected total byte count for a sequence
 * starting with leading byte 'b', or 0 if 'b' is not a valid leader.
 *
 * Does NOT check for the overlong/surrogate/range problems — those are
 * handled in utf8string_validate after the continuation bytes are collected.
 */
static inline int _utf8_sequence_len(unsigned char b)
{
	if (b < 0x80)
		return 1; /* 0xxxxxxx — ASCII */
	if (b < 0xC0)
		return 0; /* 10xxxxxx — continuation byte, not a leader */
	if (b < 0xE0)
		return 2; /* 110xxxxx */
	if (b < 0xF0)
		return 3; /* 1110xxxx */
	if (b < 0xF8)
		return 4; /* 11110xxx */
	return 0; /* 0xF8-0xFF — never valid */
}

int utf8string_validate(const utf8string *u)
{
	const unsigned char *p;
	const unsigned char *end;
	uint32_t cp;
	int seqlen;
	int i;

	if (u == NULL)
		return -EINVAL;
	if (u->utf8string_len == 0)
		return 0;
	if (u->utf8string_val == NULL)
		return -EINVAL;

	p = (const unsigned char *)u->utf8string_val;
	end = p + u->utf8string_len;

	while (p < end) {
		seqlen = _utf8_sequence_len(*p);
		if (seqlen == 0)
			return -EILSEQ; /* bad leading byte */

		/* Check continuation bytes are present and well-formed. */
		if (p + seqlen > end)
			return -EILSEQ; /* truncated sequence */

		for (i = 1; i < seqlen; i++) {
			if ((p[i] & 0xC0) != 0x80)
				return -EILSEQ; /* missing continuation byte */
		}

		/* Decode the code point. */
		switch (seqlen) {
		case 1:
			cp = p[0] & 0x7F;
			break;
		case 2:
			cp = ((uint32_t)(p[0] & 0x1F) << 6) |
			     (uint32_t)(p[1] & 0x3F);
			/* Overlong: must be >= U+0080 */
			if (cp < 0x0080)
				return -EILSEQ;
			break;
		case 3:
			cp = ((uint32_t)(p[0] & 0x0F) << 12) |
			     ((uint32_t)(p[1] & 0x3F) << 6) |
			     (uint32_t)(p[2] & 0x3F);
			/* Overlong: must be >= U+0800 */
			if (cp < 0x0800)
				return -EILSEQ;
			/* UTF-16 surrogate halves: U+D800-U+DFFF */
			if (cp >= 0xD800 && cp <= 0xDFFF)
				return -EILSEQ;
			/* Noncharacters U+FFFE and U+FFFF */
			if (cp == 0xFFFE || cp == 0xFFFF)
				return -EILSEQ;
			break;
		case 4:
			cp = ((uint32_t)(p[0] & 0x07) << 18) |
			     ((uint32_t)(p[1] & 0x3F) << 12) |
			     ((uint32_t)(p[2] & 0x3F) << 6) |
			     (uint32_t)(p[3] & 0x3F);
			/* Overlong: must be >= U+10000 */
			if (cp < 0x10000)
				return -EILSEQ;
			/* Above Unicode range */
			if (cp > 0x10FFFF)
				return -EILSEQ;
			break;
		}

		p += seqlen;
	}

	return 0;
}

int utf8string_from_wire_validated(utf8string *dst, const utf8string *wire)
{
	int rc;

	rc = utf8string_from_wire(dst, wire);
	if (rc != 0)
		return rc;

	rc = utf8string_validate(dst);
	if (rc != 0) {
		utf8string_free(dst);
		return rc;
	}

	return 0;
}

/* =========================================================================
 * Predicates
 * ========================================================================= */

bool utf8string_is_null(const utf8string *u)
{
	return (u == NULL || u->utf8string_len == 0);
}

/* =========================================================================
 * Copy and move
 * ========================================================================= */

int utf8string_copy(utf8string *dst, const utf8string *src)
{
	int rc;

	utf8string_free(dst);

	if (utf8string_is_null(src))
		return 0;

	rc = utf8string_alloc(dst, src->utf8string_len);
	if (rc != 0)
		return rc;

	memcpy(dst->utf8string_val, src->utf8string_val, src->utf8string_len);
	return 0;
}

void utf8string_move(utf8string *dst, utf8string *src)
{
	if (dst == src)
		return;

	utf8string_free(dst);

	dst->utf8string_len = src->utf8string_len;
	dst->utf8string_val = src->utf8string_val;

	src->utf8string_len = 0;
	src->utf8string_val = NULL;
}

/* =========================================================================
 * Comparison: utf8string vs utf8string
 * ========================================================================= */

static int _utf8_cmp(const utf8string *a, const utf8string *b,
		     int (*cmpfn)(const char *, const char *, size_t))
{
	unsigned int minlen;
	int rc;

	if (a == b)
		return 0;
	if (a == NULL)
		return -1;
	if (b == NULL)
		return 1;

	minlen = (a->utf8string_len < b->utf8string_len) ? a->utf8string_len :
							   b->utf8string_len;

	rc = cmpfn(_utf8_val_or_empty(a), _utf8_val_or_empty(b), minlen);
	if (rc != 0)
		return rc;

	if (a->utf8string_len < b->utf8string_len)
		return -1;
	if (a->utf8string_len > b->utf8string_len)
		return 1;
	return 0;
}

int utf8string_cmp(const utf8string *a, const utf8string *b)
{
	return _utf8_cmp(a, b, strncmp);
}

int utf8string_casecmp(const utf8string *a, const utf8string *b)
{
	return _utf8_cmp(a, b, strncasecmp);
}

bool utf8string_eq(const utf8string *a, const utf8string *b)
{
	return utf8string_cmp(a, b) == 0;
}

bool utf8string_caseeq(const utf8string *a, const utf8string *b)
{
	return utf8string_casecmp(a, b) == 0;
}

/* =========================================================================
 * Conversion: C string <-> utf8string
 * ========================================================================= */

int cstr_to_utf8string(utf8string *dst, const char *cstr)
{
	size_t len;
	int rc;

	utf8string_free(dst);

	if (cstr == NULL || *cstr == '\0')
		return 0;

	len = strlen(cstr);
	if (len > UINT_MAX)
		return -EINVAL;

	rc = utf8string_alloc(dst, (unsigned int)len);
	if (rc != 0)
		return rc;

	memcpy(dst->utf8string_val, cstr, len);
	return 0;
}

const char *utf8string_to_cstr(const utf8string *u)
{
	if (u == NULL)
		return NULL;
	return u->utf8string_val;
}

/* =========================================================================
 * Comparison: utf8string vs C string
 * ========================================================================= */

static int _utf8_cmp_cstr(const utf8string *u, const char *cstr,
			  int (*cmpfn)(const char *, const char *, size_t))
{
	const char *uval;
	size_t clen;
	unsigned int ulen;
	int rc;

	cstr = _cstr_safe(cstr);
	clen = strlen(cstr);
	ulen = (u != NULL) ? u->utf8string_len : 0;
	uval = (u != NULL) ? _utf8_val_or_empty(u) : "";

	rc = cmpfn(uval, cstr, (ulen < clen) ? ulen : clen);
	if (rc != 0)
		return rc;

	if (ulen < (unsigned int)clen)
		return -1;
	if (ulen > (unsigned int)clen)
		return 1;
	return 0;
}

int utf8string_cmp_cstr(const utf8string *u, const char *cstr)
{
	return _utf8_cmp_cstr(u, cstr, strncmp);
}

int utf8string_casecmp_cstr(const utf8string *u, const char *cstr)
{
	return _utf8_cmp_cstr(u, cstr, strncasecmp);
}

bool utf8string_eq_cstr(const utf8string *u, const char *cstr)
{
	return utf8string_cmp_cstr(u, cstr) == 0;
}

bool utf8string_caseeq_cstr(const utf8string *u, const char *cstr)
{
	return utf8string_casecmp_cstr(u, cstr) == 0;
}

/* =========================================================================
 * UID / GID conversion
 * ========================================================================= */

/*
 * _id_to_utf8string - shared formatter for uid_t and gid_t.
 *
 * Both types are unsigned; we promote to uintmax_t to handle any width.
 */
static int _id_to_utf8string(utf8string *dst, uintmax_t id)
{
	/*
	 * uintmax_t is at most 64 bits → 20 decimal digits.
	 * A 21-byte stack buffer is always sufficient.
	 */
	char buf[21];
	int len;

	len = snprintf(buf, sizeof(buf), "%ju", id);
	if (len < 0 || (size_t)len >= sizeof(buf))
		return -EINVAL; /* should never happen */

	return cstr_to_utf8string(dst, buf);
}

int utf8string_from_uid(utf8string *dst, uid_t uid)
{
	return _id_to_utf8string(dst, (uintmax_t)uid);
}

int utf8string_from_gid(utf8string *dst, gid_t gid)
{
	return _id_to_utf8string(dst, (uintmax_t)gid);
}

/*
 * _utf8string_to_id - shared parser for uid_t and gid_t.
 *
 * We parse into a uintmax_t, then range-check against 'max_val' (the largest
 * value representable by the target type, e.g. UINT32_MAX for a 32-bit id).
 */
static int _utf8string_to_id(const utf8string *u, uintmax_t max_val,
			     uintmax_t *out)
{
	const char *p;
	uintmax_t val;
	uintmax_t digit;
	unsigned int i;

	if (utf8string_is_null(u))
		return -EINVAL;

	p = u->utf8string_val;

	/* Reject leading zeros on multi-digit strings. */
	if (u->utf8string_len > 1 && p[0] == '0')
		return -EINVAL;

	val = 0;
	for (i = 0; i < u->utf8string_len; i++) {
		if (!isdigit((unsigned char)p[i]))
			return -EINVAL;

		digit = (uintmax_t)(p[i] - '0');

		/* Overflow check: val*10 + digit > max_val */
		if (val > (max_val - digit) / 10)
			return -ERANGE;

		val = val * 10 + digit;
	}

	*out = val;
	return 0;
}

int utf8string_to_uid(const utf8string *u, uid_t *uid_out)
{
	uintmax_t val;
	int rc;

	/*
	 * uid_t is POSIX-defined as an unsigned integer type; its maximum
	 * value is not directly exposed, so we use (uid_t)-1 which, for any
	 * unsigned type, gives the all-ones bit pattern == the maximum value.
	 */
	rc = _utf8string_to_id(u, (uintmax_t)(uid_t)-1, &val);
	if (rc != 0)
		return rc;

	*uid_out = (uid_t)val;
	return 0;
}

int utf8string_to_gid(const utf8string *u, gid_t *gid_out)
{
	uintmax_t val;
	int rc;

	rc = _utf8string_to_id(u, (uintmax_t)(gid_t)-1, &val);
	if (rc != 0)
		return rc;

	*gid_out = (gid_t)val;
	return 0;
}

/* =========================================================================
 * Printing
 * ========================================================================= */

void utf8string_print(FILE *f, const utf8string *u)
{
	if (utf8string_is_null(u)) {
		fputs("<null>", f);
		return;
	}
	/*
	 * %.*s with the explicit byte length: correct without NUL termination,
	 * so safe on wire buffers too.
	 */
	fprintf(f, "%.*s", (int)u->utf8string_len, u->utf8string_val);
}

void utf8string_print_repr(FILE *f, const utf8string *u)
{
	const unsigned char *p;
	unsigned int i;

	if (utf8string_is_null(u)) {
		fputs("<null>", f);
		return;
	}

	p = (const unsigned char *)u->utf8string_val;
	for (i = 0; i < u->utf8string_len; i++) {
		if (p[i] >= 0x20 && p[i] <= 0x7E)
			fputc((int)p[i], f);
		else
			fprintf(f, "\\x%02X", (unsigned int)p[i]);
	}
}

/* =========================================================================
 * Path component validation
 * ========================================================================= */

/*
 * _is_forbidden_codepoint - true for code points that are never legal in an
 * NFSv4 path component regardless of filesystem:
 *
 *   U+0000           NUL  — C string terminator
 *   U+0001-U+001F    C0 controls
 *   U+002F           '/'  — path separator
 *   U+007F           DEL
 *   U+0080-U+009F    C1 controls
 */
static inline bool _is_forbidden_codepoint(uint32_t cp)
{
	if (cp == 0x0000)
		return true; /* NUL */
	if (cp <= 0x001F)
		return true; /* C0 controls */
	if (cp == 0x002F)
		return true; /* '/' */
	if (cp == 0x007F)
		return true; /* DEL */
	if (cp >= 0x0080 && cp <= 0x009F)
		return true; /* C1 controls */
	return false;
}

int utf8string_validate_component(const utf8string *u, unsigned int name_max)
{
	const unsigned char *p;
	const unsigned char *end;
	uint32_t cp;
	int seqlen;
	int i;

	/* Rule 1: non-empty */
	if (utf8string_is_null(u))
		return -EINVAL;
	if (u->utf8string_val == NULL)
		return -EINVAL;

	/* Rule 3: length limit */
	if (name_max == 0)
		name_max = 255;
	if (u->utf8string_len > name_max)
		return -ENAMETOOLONG;

	/* Rule 5: reserved names "." and ".." */
	if (u->utf8string_len == 1 && u->utf8string_val[0] == '.')
		return -EBADNAME;
	if (u->utf8string_len == 2 && u->utf8string_val[0] == '.' &&
	    u->utf8string_val[1] == '.')
		return -EBADNAME;

	/*
	 * Rules 2 and 4: walk the byte sequence decoding each UTF-8 sequence,
	 * validating structure (-EILSEQ) and checking each decoded code point
	 * for forbidden characters (-EBADNAME).
	 *
	 * We do this in a single pass rather than calling utf8string_validate()
	 * followed by a separate character scan so we can (a) return distinct
	 * error codes per failure class and (b) avoid iterating twice.
	 */
	p = (const unsigned char *)u->utf8string_val;
	end = p + u->utf8string_len;

	while (p < end) {
		seqlen = _utf8_sequence_len(*p);
		if (seqlen == 0)
			return -EILSEQ; /* bad leading byte */

		if (p + seqlen > end)
			return -EILSEQ; /* truncated sequence */

		for (i = 1; i < seqlen; i++) {
			if ((p[i] & 0xC0) != 0x80)
				return -EILSEQ; /* missing continuation byte */
		}

		/* Decode and range-check the code point. */
		switch (seqlen) {
		case 1:
			cp = p[0] & 0x7F;
			break;
		case 2:
			cp = ((uint32_t)(p[0] & 0x1F) << 6) |
			     (uint32_t)(p[1] & 0x3F);
			if (cp < 0x0080)
				return -EILSEQ; /* overlong */
			break;
		case 3:
			cp = ((uint32_t)(p[0] & 0x0F) << 12) |
			     ((uint32_t)(p[1] & 0x3F) << 6) |
			     (uint32_t)(p[2] & 0x3F);
			if (cp < 0x0800)
				return -EILSEQ; /* overlong */
			if (cp >= 0xD800 && cp <= 0xDFFF)
				return -EILSEQ; /* surrogate half */
			if (cp == 0xFFFE || cp == 0xFFFF)
				return -EILSEQ; /* noncharacter */
			break;
		case 4:
			cp = ((uint32_t)(p[0] & 0x07) << 18) |
			     ((uint32_t)(p[1] & 0x3F) << 12) |
			     ((uint32_t)(p[2] & 0x3F) << 6) |
			     (uint32_t)(p[3] & 0x3F);
			if (cp < 0x10000)
				return -EILSEQ; /* overlong */
			if (cp > 0x10FFFF)
				return -EILSEQ; /* above Unicode range */
			break;
		}

		/* Forbidden character check on the decoded code point. */
		if (_is_forbidden_codepoint(cp))
			return -EBADNAME;

		p += seqlen;
	}

	return 0;
}
