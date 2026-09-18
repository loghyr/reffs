/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * D1 storage model: the canonical encoding.
 *
 * One form, used for the request digest, for journal bodies and for the
 * golden fixtures: big-endian integers at their declared widths, a
 * boolean as one byte 0 or 1, an option as one byte absent or present
 * followed by the value when present, a vector as a u32 count followed
 * by its elements, and bytes as a u32 length followed by exactly that
 * many.  Nothing is padded and nothing is implicit.
 *
 * The decoder is deliberately strict.  It refuses an option tag that is
 * neither 0 nor 1, a boolean that is neither, a count past the model's
 * declared limit, a length that would run past the end, and any trailing
 * byte after the value it was asked for.  A form that decodes must be
 * the only encoding of what it decoded to.
 */

#ifndef REFFS_D1_CODEC_H
#define REFFS_D1_CODEC_H

#include "d1_types.h"

/* A bounded cursor over a buffer being written or read. */
struct d1_cursor {
	uint8_t *buf;
	size_t cap;
	size_t len;
	/*
	 * Sticky: once an encode has run out of room or a decode has met
	 * something it cannot accept, every later call is a no-op and the
	 * result is discarded whole.
	 */
	bool bad;
};

void d1_enc_init(struct d1_cursor *c, void *buf, size_t cap);
void d1_dec_init(struct d1_cursor *c, const void *buf, size_t len);

/* Whether the cursor is still usable. */
static inline bool d1_cursor_ok(const struct d1_cursor *c)
{
	return !c->bad;
}

void d1_enc_u8(struct d1_cursor *c, uint8_t v);
void d1_enc_u16(struct d1_cursor *c, uint16_t v);
void d1_enc_u32(struct d1_cursor *c, uint32_t v);
void d1_enc_u64(struct d1_cursor *c, uint64_t v);
void d1_enc_bool(struct d1_cursor *c, bool v);
void d1_enc_bytes(struct d1_cursor *c, const void *data, size_t len);
void d1_enc_raw(struct d1_cursor *c, const void *data, size_t len);
void d1_enc_uuid(struct d1_cursor *c, const struct d1_uuid *u);
void d1_enc_opt_u64(struct d1_cursor *c, bool present, uint64_t v);
void d1_enc_guard(struct d1_cursor *c, const struct d1_guard *g);
void d1_enc_owner(struct d1_cursor *c, const struct d1_owner *o);
void d1_enc_opkey(struct d1_cursor *c, const struct d1_opkey *k);
void d1_enc_checksum(struct d1_cursor *c, const struct d1_checksum *s);
void d1_enc_objkey(struct d1_cursor *c, const struct d1_objkey *k);

bool d1_dec_u8(struct d1_cursor *c, uint8_t *v);
bool d1_dec_u16(struct d1_cursor *c, uint16_t *v);
bool d1_dec_u32(struct d1_cursor *c, uint32_t *v);
bool d1_dec_u64(struct d1_cursor *c, uint64_t *v);
bool d1_dec_bool(struct d1_cursor *c, bool *v);
bool d1_dec_raw(struct d1_cursor *c, void *out, size_t len);
bool d1_dec_bytes_ref(struct d1_cursor *c, const uint8_t **out, uint32_t *len,
		      uint32_t max);
bool d1_dec_uuid(struct d1_cursor *c, struct d1_uuid *u);
bool d1_dec_opt_u64(struct d1_cursor *c, bool *present, uint64_t *v);
bool d1_dec_opt_u32(struct d1_cursor *c, bool *present, uint32_t *v);
bool d1_dec_guard(struct d1_cursor *c, struct d1_guard *g);
bool d1_dec_owner(struct d1_cursor *c, struct d1_owner *o);
bool d1_dec_opkey(struct d1_cursor *c, struct d1_opkey *k);
bool d1_dec_checksum(struct d1_cursor *c, struct d1_checksum *s);
bool d1_dec_objkey(struct d1_cursor *c, struct d1_objkey *k);

/* Whether the decode consumed the whole buffer and met nothing bad. */
bool d1_dec_finished(const struct d1_cursor *c);

/*
 * Checked arithmetic.  Every sum and product that can reach a declared
 * limit goes through these, so an overflow is a refusal rather than a
 * wrapped value that passes a later bound.
 */
bool d1_add_u64(uint64_t a, uint64_t b, uint64_t *out);
bool d1_mul_u64(uint64_t a, uint64_t b, uint64_t *out);
bool d1_add_size(size_t a, size_t b, size_t *out);

#endif /* REFFS_D1_CODEC_H */
