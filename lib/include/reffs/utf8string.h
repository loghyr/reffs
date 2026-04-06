/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
/*
 * utf8string.h - NFSv4.2 utf8string utility functions
 *
 * The wire type is:
 *   typedef struct {
 *       u_int  utf8string_len;   // does NOT include trailing NUL
 *       char  *utf8string_val;   // allocated with one extra byte for NUL pad
 *   } utf8string;
 *
 * WIRE vs. LOCAL STORAGE
 * ----------------------
 * utf8strings that arrive from the network are decoded by the XDR layer into
 * a struct whose val pointer points directly into the XDR receive buffer.
 * That buffer is NOT NUL-terminated.  Such a struct must NEVER be passed to
 * any function here without first being sanitised with utf8string_from_wire(),
 * which copies the bytes into a fresh, NUL-padded allocation and takes
 * ownership of the result.
 *
 * UTF-8 VALIDATION
 * ----------------
 * Strings produced locally (utf8string_alloc, cstr_to_utf8string, the uid/gid
 * converters) are always valid UTF-8 by construction.  Strings that arrive
 * from the wire may not be; call utf8string_validate() after
 * utf8string_from_wire() when validity must be enforced.
 */

#ifndef UTF8STRING_H
#define UTF8STRING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h> /* uid_t, gid_t */

#ifdef STAND_ALONE
/* -------------------------------------------------------------------------
 * Wire type (matches XDR / kernel definition)
 * ------------------------------------------------------------------------- */
typedef struct {
	unsigned int utf8string_len; /* byte count, excluding NUL pad */
	char *utf8string_val; /* always NUL-terminated in memory */
} utf8string;
#else
#include "nfsv42_xdr.h"
#endif

/* =========================================================================
 * Lifecycle
 * ========================================================================= */

/*
 * utf8string_alloc - allocate a utf8string of 'len' bytes.
 *
 * Allocates len+1 bytes for utf8string_val, zeroes the entire buffer, and
 * sets utf8string_len = len.  The caller fills in utf8string_val[0..len-1].
 *
 * Returns 0 on success, -ENOMEM on allocation failure.
 * On failure *u is left unchanged.
 */
int utf8string_alloc(utf8string *u, unsigned int len);

/*
 * utf8string_free - release storage owned by a utf8string.
 *
 * Sets utf8string_val to NULL and utf8string_len to 0 after freeing.
 * Safe to call on a zeroed or already-freed utf8string.
 */
void utf8string_free(utf8string *u);

/*
 * utf8string_from_wire - safely ingest a raw XDR-decoded utf8string.
 *
 * The XDR receive buffer is NOT NUL-terminated.  This function copies
 * exactly 'wire->utf8string_len' bytes from 'wire->utf8string_val' into a
 * fresh allocation that has the mandatory NUL pad, then writes the result
 * into *dst.
 *
 * The wire buffer is left untouched; the caller retains responsibility for
 * releasing it via whatever mechanism the XDR layer requires (typically
 * xdr_free / clnt_freeres).
 *
 * Any existing storage in *dst is freed before the copy.
 *
 * Returns 0 on success, -ENOMEM on allocation failure, -EINVAL if
 * wire->utf8string_val is NULL but wire->utf8string_len is non-zero.
 * On error *dst is left in a valid, freed state (len=0, val=NULL).
 */
int utf8string_from_wire(utf8string *dst, const utf8string *wire);

/* =========================================================================
 * Validation
 * ========================================================================= */

/*
 * utf8string_validate - verify that a utf8string contains well-formed UTF-8.
 *
 * Checks for:
 *   - Illegal leading bytes (0xC0, 0xC1, 0xF5-0xFF)
 *   - Overlong encodings (e.g. U+0041 encoded as 0xC1 0x81)
 *   - Truncated multi-byte sequences
 *   - Unexpected continuation bytes
 *   - Code points above U+10FFFF
 *   - UTF-16 surrogate halves (U+D800-U+DFFF)
 *   - Noncharacters U+FFFE and U+FFFF
 *
 * A null utf8string (len == 0) is considered valid.
 *
 * Returns  0        if the string is valid UTF-8.
 *          -EILSEQ  if an invalid byte sequence is found.
 *          -EINVAL  if u is NULL or val is NULL with non-zero len.
 */
int utf8string_validate(const utf8string *u);

/*
 * utf8string_from_wire_validated - utf8string_from_wire + utf8string_validate
 *                                  in one call.
 *
 * On a validation failure the destination is freed and left null.
 * Returns 0, -ENOMEM, -EINVAL, or -EILSEQ.
 */
int utf8string_from_wire_validated(utf8string *dst, const utf8string *wire);

/* =========================================================================
 * Predicates
 * ========================================================================= */

/*
 * utf8string_is_null - true when the string carries zero bytes (empty string).
 *
 * A NULL val with len == 0 is also considered null.
 */
bool utf8string_is_null(const utf8string *u);

/* =========================================================================
 * Copy and move
 * ========================================================================= */

/*
 * utf8string_copy - deep-copy 'src' into 'dst'.
 *
 * Any existing storage in 'dst' is freed first.
 * Returns 0 on success, -ENOMEM on allocation failure.
 * On failure *dst is left in a valid, freed state (len=0, val=NULL).
 */
int utf8string_copy(utf8string *dst, const utf8string *src);

/*
 * utf8string_move - transfer ownership of 'src' storage to 'dst'.
 *
 * No allocation or copy is performed.  Any existing storage in *dst is freed
 * first.  On return *src is a valid null utf8string (len=0, val=NULL).
 *
 * Use this instead of utf8string_copy when the source is no longer needed,
 * e.g. when building a string in a temporary and storing it into a struct.
 */
void utf8string_move(utf8string *dst, utf8string *src);

/* =========================================================================
 * Comparison: utf8string vs utf8string
 * ========================================================================= */

/*
 * utf8string_cmp - case-sensitive comparison.
 *
 * Returns <0, 0, or >0 (strcmp semantics).
 * Two null strings compare equal.  A null string sorts before a non-null one.
 */
int utf8string_cmp(const utf8string *a, const utf8string *b);

/*
 * utf8string_casecmp - case-insensitive comparison (ASCII folding only).
 *
 * Same return semantics as utf8string_cmp.
 */
int utf8string_casecmp(const utf8string *a, const utf8string *b);

/* utf8string_eq  - true when utf8string_cmp(a,b) == 0. */
bool utf8string_eq(const utf8string *a, const utf8string *b);

/* utf8string_caseeq - true when utf8string_casecmp(a,b) == 0. */
bool utf8string_caseeq(const utf8string *a, const utf8string *b);

/* =========================================================================
 * Conversion: C string <-> utf8string
 * ========================================================================= */

/*
 * cstr_to_utf8string - build a utf8string from a NUL-terminated C string.
 *
 * Any existing storage in 'dst' is freed first.
 * Passing NULL or "" for 'cstr' produces a null utf8string (len=0).
 * Returns 0 on success, -ENOMEM on allocation failure.
 */
int cstr_to_utf8string(utf8string *dst, const char *cstr);

/*
 * utf8string_to_cstr - return a pointer to the NUL-terminated C string
 *                      inside 'u'.
 *
 * The returned pointer is valid for the lifetime of 'u' and must NOT be
 * freed by the caller.  Returns NULL if u->utf8string_val is NULL.
 */
const char *utf8string_to_cstr(const utf8string *u);

/* =========================================================================
 * Comparison: utf8string vs C string
 * ========================================================================= */

/* utf8string_cmp_cstr - case-sensitive comparison with a C string.
 * 'cstr' may be NULL (treated as the empty string).
 * Returns <0, 0, or >0 (strcmp semantics). */
int utf8string_cmp_cstr(const utf8string *u, const char *cstr);

/* utf8string_casecmp_cstr - case-insensitive comparison with a C string. */
int utf8string_casecmp_cstr(const utf8string *u, const char *cstr);

/* utf8string_eq_cstr  - true when utf8string_cmp_cstr(u,cstr) == 0. */
bool utf8string_eq_cstr(const utf8string *u, const char *cstr);

/* utf8string_caseeq_cstr - true when utf8string_casecmp_cstr(u,cstr) == 0. */
bool utf8string_caseeq_cstr(const utf8string *u, const char *cstr);

/* =========================================================================
 * UID / GID conversion
 *
 * NFSv4 transmits owner and owner_group as UTF-8 strings.  In the absence of
 * a name-mapping service the numeric form is used: a plain decimal string
 * with no leading zeros (except for id 0 itself), e.g. "0", "1000", "65534".
 * ========================================================================= */

/* utf8string_from_uid - format a uid_t as a decimal UTF-8 string. */
int utf8string_from_uid(utf8string *dst, uid_t uid);

/* utf8string_from_gid - format a gid_t as a decimal UTF-8 string. */
int utf8string_from_gid(utf8string *dst, gid_t gid);

/*
 * utf8string_to_uid - parse a decimal UTF-8 string into a uid_t.
 *
 * Rejects empty strings, leading zeros on multi-digit strings, non-digit
 * characters, and values that overflow uid_t.
 *
 * Returns 0 on success, -EINVAL on bad input, -ERANGE on overflow.
 */
int utf8string_to_uid(const utf8string *u, uid_t *uid_out);

/* utf8string_to_gid - same rules as utf8string_to_uid. */
int utf8string_to_gid(const utf8string *u, gid_t *gid_out);

/* =========================================================================
 * Printing
 * ========================================================================= */

/*
 * utf8string_print - write a utf8string to a FILE stream.
 *
 * A null string prints as "<null>".  Safe on wire buffers (uses explicit
 * length, not NUL termination).
 */
void utf8string_print(FILE *f, const utf8string *u);

/*
 * utf8string_print_repr - print with non-ASCII bytes escaped as \xNN.
 *
 * ASCII printable characters (0x20-0x7E) are written as-is; everything else
 * becomes \xNN.  Safe for any log sink regardless of locale.
 */
void utf8string_print_repr(FILE *f, const utf8string *u);

/* =========================================================================
 * Path component validation
 *
 * A valid NFSv4 path component must satisfy all of:
 *
 *   1. Non-empty                              (-EINVAL)
 *   2. Valid UTF-8 byte sequences             (-EILSEQ,  RFC 8881 S14.5)
 *   3. Does not exceed name_max bytes         (-ENAMETOOLONG)
 *   4. Contains no forbidden characters:
 *        NUL (U+0000), C0 controls (U+0001-U+001F),
 *        '/' (U+002F), DEL (U+007F), C1 controls (U+0080-U+009F)
 *                                             (-EBADNAME)
 *   5. Is not the reserved name "." or ".."   (-EBADNAME)
 *
 * Unicode normalization and plane-0-only restrictions are filesystem-layer
 * concerns and are not enforced here.
 * ========================================================================= */

/*
 * utf8string_validate_component - validate a utf8string as an NFSv4 path
 *                                 component.
 *
 * 'name_max' is the maximum byte length the underlying filesystem permits
 * (typically 255; pass 0 to use that default).
 *
 * Returns  0               on success.
 *          -EINVAL          string is NULL or empty.
 *          -EILSEQ          invalid UTF-8 byte sequence.
 *          -ENAMETOOLONG    utf8string_len > name_max.
 *          -EBADNAME        forbidden character or reserved name (".", "..").
 */
int utf8string_validate_component(const utf8string *u, unsigned int name_max);

/*
 * utf8string_validate_component_default - utf8string_validate_component
 *                                         with name_max = 255.
 */
static inline int utf8string_validate_component_default(const utf8string *u)
{
	return utf8string_validate_component(u, 255);
}

#endif /* UTF8STRING_H */
