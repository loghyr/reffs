/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * nfs4/attr.h -- NFSv4 bitmap4 macros and inline helpers.
 */

#ifndef ATTR_H
#define ATTR_H

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "nfsv42_xdr.h"
#include "nfsv42_names.h"

/* ------------------------------------------------------------------ */
/* Low-level word/bit arithmetic                                      */
/* ------------------------------------------------------------------ */

#define BITMAP4_WORD_BITS 32U
#define BITMAP4_WORD_INDEX(a) ((uint32_t)(a) / BITMAP4_WORD_BITS)
#define BITMAP4_BIT_INDEX(a) ((uint32_t)(a) % BITMAP4_WORD_BITS)
#define BITMAP4_WORDS_FOR_MAX(m) (BITMAP4_WORD_INDEX(m) + 1U)
#define BITMAP4_MASK(a) (UINT32_C(1) << BITMAP4_BIT_INDEX(a))

/*
 * Number of words required to represent all attributes up to
 * FATTR4_ATTRIBUTE_MAX.  Useful for stack-allocated storage arrays:
 *
 *   uint32_t storage[SUPPORTED_ATTR_WORDS];
 */
#define SUPPORTED_ATTR_WORDS BITMAP4_WORDS_FOR_MAX(FATTR4_ATTRIBUTE_MAX)

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

/*
 * bitmap4_wrap - use an existing uint32_t[] as a bitmap4 without allocating.
 *
 * The caller owns the storage and is responsible for its lifetime.
 * bitmap4_destroy() must NOT be called on a wrapped bitmap.
 */
static inline void bitmap4_wrap(bitmap4 *bm, uint32_t *storage, u_int words)
{
	bm->bitmap4_len = words;
	bm->bitmap4_val = storage;
}

/*
 * bitmap4_init - allocate and zero a bitmap4 sized to hold @max_attr.
 *
 * Returns  0       on success.
 *         -EINVAL  if @bm is NULL.
 *         -ENOMEM  if allocation fails.
 */
static inline int bitmap4_init(bitmap4 *bm, uint32_t max_attr)
{
	u_int words;

	if (bm == NULL)
		return -EINVAL;

	words = BITMAP4_WORDS_FOR_MAX(max_attr);
	bm->bitmap4_val = calloc(words, sizeof(*bm->bitmap4_val));
	if (bm->bitmap4_val == NULL)
		return -ENOMEM;

	bm->bitmap4_len = words;
	return 0;
}

/*
 * bitmap4_copy - allocate and copy a bitmap4 from src to dst
 *
 * Returns  0       on success.
 *         -EINVAL  if @bm is NULL.
 *         -ENOMEM  if allocation fails.
 */
static inline int bitmap4_copy(bitmap4 *src, bitmap4 *dst)
{
	if (src == NULL)
		return -EINVAL;

	if (src->bitmap4_len == 0)
		return 0;

	dst->bitmap4_val = calloc(src->bitmap4_len, sizeof(*dst->bitmap4_val));
	if (dst->bitmap4_val == NULL)
		return -ENOMEM;

	dst->bitmap4_len = src->bitmap4_len;

	for (u_int i = 0; i < dst->bitmap4_len; i++)
		dst->bitmap4_val[i] = src->bitmap4_val[i];

	return 0;
}

/*
 * bitmap4_equal - determine if a and b are the same.
 */
static inline bool bitmap4_equal(bitmap4 *a, bitmap4 *b)
{
	if (!a && !b)
		return true;

	if (!a || !b)
		return false;

	if (a->bitmap4_len != b->bitmap4_len)
		return false;

	for (u_int i = 0; i < a->bitmap4_len; i++)
		if (a->bitmap4_val[i] != b->bitmap4_val[i])
			return false;

	return true;
}

/*
 * bitmap4_destroy - free storage allocated by bitmap4_init().
 *
 * Safe to call on a NULL pointer or a zeroed struct.
 * Must NOT be called on bitmaps initialised with bitmap4_wrap().
 */
static inline void bitmap4_destroy(bitmap4 *bm)
{
	if (bm == NULL)
		return;

	free(bm->bitmap4_val);
	bm->bitmap4_val = NULL;
	bm->bitmap4_len = 0;
}

/*
 * bitmap4_zero - clear all bits without freeing storage.
 */
static inline void bitmap4_zero(bitmap4 *bm)
{
	if (bm == NULL || bm->bitmap4_val == NULL || bm->bitmap4_len == 0)
		return;

	memset(bm->bitmap4_val, 0, bm->bitmap4_len * sizeof(*bm->bitmap4_val));
}

/* ------------------------------------------------------------------ */
/* Internal bounds check (not part of the public API)                 */
/* ------------------------------------------------------------------ */

static inline bool bitmap4_attr_fits(const bitmap4 *bm, uint32_t attr)
{
	if (bm == NULL || bm->bitmap4_val == NULL)
		return false;

	return BITMAP4_WORD_INDEX(attr) < bm->bitmap4_len;
}

/* ------------------------------------------------------------------ */
/* Per-bit accessors                                                  */
/*                                                                    */
/* Out-of-range reads  --> false (safe).                                */
/* Out-of-range writes --> no-op, returns false (safe).                 */
/* ------------------------------------------------------------------ */

static inline bool bitmap4_attribute_is_set(const bitmap4 *bm, uint32_t attr)
{
	uint32_t word;

	if (!bitmap4_attr_fits(bm, attr))
		return false;

	word = BITMAP4_WORD_INDEX(attr);
	return (bm->bitmap4_val[word] & BITMAP4_MASK(attr)) != 0;
}

static inline bool bitmap4_attribute_set(bitmap4 *bm, uint32_t attr)
{
	uint32_t word;

	if (!bitmap4_attr_fits(bm, attr))
		return false;

	word = BITMAP4_WORD_INDEX(attr);
	bm->bitmap4_val[word] |= BITMAP4_MASK(attr);
	return true;
}

static inline bool bitmap4_attribute_clear(bitmap4 *bm, uint32_t attr)
{
	uint32_t word;

	if (!bitmap4_attr_fits(bm, attr))
		return false;

	word = BITMAP4_WORD_INDEX(attr);
	bm->bitmap4_val[word] &= ~BITMAP4_MASK(attr);
	return true;
}

/* ------------------------------------------------------------------ */
/* Composite predicates                                               */
/* ------------------------------------------------------------------ */

/*
 * bitmap4_attribute_is_supported_and_set - true when an attribute is both
 * present in the client-supplied @requested bitmap AND listed in the server's
 * @supported bitmap.
 *
 * Correctly handles mismatched bitmap lengths in both directions:
 *   - client sends fewer words than the server supports
 *   - client sends more words than the server supports
 */
static inline bool
bitmap4_attribute_is_supported_and_set(const bitmap4 *supported,
				       const bitmap4 *requested, uint32_t attr)
{
	return bitmap4_attribute_is_set(supported, attr) &&
	       bitmap4_attribute_is_set(requested, attr);
}

/*
 * bitmap4_has_unsupported_bits - true when @wire contains any bit that is not
 * set in @supported.
 *
 * Extra words beyond the server's supported range are treated as unsupported
 * if any of their bits are non-zero.
 */
static inline bool bitmap4_has_unsupported_bits(const bitmap4 *supported,
						const bitmap4 *wire)
{
	u_int i, common_words;

	if (supported == NULL || wire == NULL ||
	    supported->bitmap4_val == NULL || wire->bitmap4_val == NULL)
		return false;

	common_words = (supported->bitmap4_len < wire->bitmap4_len) ?
			       supported->bitmap4_len :
			       wire->bitmap4_len;

	for (i = 0; i < common_words; i++) {
		if (wire->bitmap4_val[i] & ~supported->bitmap4_val[i])
			return true;
	}

	/* Any extra non-zero words sent by the client are unsupported. */
	for (i = common_words; i < wire->bitmap4_len; i++) {
		if (wire->bitmap4_val[i] != 0)
			return true;
	}

	return false;
}

/* ------------------------------------------------------------------ */
/* Subsystem lifecycle                                                */
/* ------------------------------------------------------------------ */

int nfs4_attribute_init(void);
int nfs4_attribute_fini(void);
void nfs4_attr_enable_layouts(void);

/*
 * nfs4_wcc_fattr4_extract -- decode SIZE and TIME_MODIFY from an fattr4
 * blob received in LAYOUT_WCC ffdsw_attributes (RFC 9766 S3.7).
 *
 * Properly walks the bitmap in ascending attribute-number order,
 * advancing the XDR position through preceding attributes before
 * extracting the target values.
 */
nfsstat4 nfs4_wcc_fattr4_extract(const fattr4 *fa, uint64_t *size_out,
				 bool *has_size, nfstime4 *mtime_out,
				 bool *has_mtime);

#endif /* ATTR_H */
