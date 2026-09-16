/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * D1 storage model: the canonical encoding, as described by its header.
 */

#include <string.h>

#include "d1_codec.h"

void d1_enc_init(struct d1_cursor *c, void *buf, size_t cap)
{
	c->buf = buf;
	c->cap = cap;
	c->len = 0;
	c->bad = false;
}

void d1_dec_init(struct d1_cursor *c, const void *buf, size_t len)
{
	/*
	 * The decoder never writes through this pointer; the cast keeps one
	 * cursor type for both directions.
	 */
	c->buf = (uint8_t *)(uintptr_t)buf;
	c->cap = len;
	c->len = 0;
	c->bad = false;
}

static uint8_t *d1_enc_room(struct d1_cursor *c, size_t n)
{
	uint8_t *at;

	if (c->bad)
		return NULL;
	if (n > c->cap - c->len) {
		c->bad = true;
		return NULL;
	}
	at = c->buf + c->len;
	c->len += n;
	return at;
}

static const uint8_t *d1_dec_take(struct d1_cursor *c, size_t n)
{
	const uint8_t *at;

	if (c->bad)
		return NULL;
	if (n > c->cap - c->len) {
		c->bad = true;
		return NULL;
	}
	at = c->buf + c->len;
	c->len += n;
	return at;
}

void d1_enc_u8(struct d1_cursor *c, uint8_t v)
{
	uint8_t *at = d1_enc_room(c, 1);

	if (at)
		at[0] = v;
}

void d1_enc_u16(struct d1_cursor *c, uint16_t v)
{
	uint8_t *at = d1_enc_room(c, 2);

	if (!at)
		return;
	at[0] = (uint8_t)(v >> 8);
	at[1] = (uint8_t)v;
}

void d1_enc_u32(struct d1_cursor *c, uint32_t v)
{
	uint8_t *at = d1_enc_room(c, 4);

	if (!at)
		return;
	at[0] = (uint8_t)(v >> 24);
	at[1] = (uint8_t)(v >> 16);
	at[2] = (uint8_t)(v >> 8);
	at[3] = (uint8_t)v;
}

void d1_enc_u64(struct d1_cursor *c, uint64_t v)
{
	uint8_t *at = d1_enc_room(c, 8);
	unsigned int i;

	if (!at)
		return;
	for (i = 0; i < 8; i++)
		at[i] = (uint8_t)(v >> (56 - 8 * i));
}

void d1_enc_bool(struct d1_cursor *c, bool v)
{
	d1_enc_u8(c, v ? 1u : 0u);
}

void d1_enc_raw(struct d1_cursor *c, const void *data, size_t len)
{
	uint8_t *at = d1_enc_room(c, len);

	if (at && len)
		memcpy(at, data, len);
}

void d1_enc_bytes(struct d1_cursor *c, const void *data, size_t len)
{
	if (len > UINT32_MAX) {
		c->bad = true;
		return;
	}
	d1_enc_u32(c, (uint32_t)len);
	d1_enc_raw(c, data, len);
}

void d1_enc_uuid(struct d1_cursor *c, const struct d1_uuid *u)
{
	d1_enc_raw(c, u->bytes, sizeof(u->bytes));
}

void d1_enc_opt_u64(struct d1_cursor *c, bool present, uint64_t v)
{
	d1_enc_u8(c, present ? 1u : 0u);
	if (present)
		d1_enc_u64(c, v);
}

void d1_enc_guard(struct d1_cursor *c, const struct d1_guard *g)
{
	d1_enc_u32(c, g->generation);
	d1_enc_u32(c, g->writer);
	d1_enc_bool(c, g->never_written);
}

void d1_enc_owner(struct d1_cursor *c, const struct d1_owner *o)
{
	d1_enc_u64(c, o->cohort.raw);
	d1_enc_u32(c, o->writer);
	d1_enc_u32(c, o->co_id);
}

void d1_enc_opkey(struct d1_cursor *c, const struct d1_opkey *k)
{
	d1_enc_uuid(c, &k->origin);
	d1_enc_u64(c, k->sequence);
	d1_enc_u32(c, k->ordinal);
}

void d1_enc_checksum(struct d1_cursor *c, const struct d1_checksum *s)
{
	/*
	 * The digest array is fixed, so a caller-supplied length past it is
	 * refused here rather than read.  A typed struct owning its own
	 * input does not authorise reading past its own fields.
	 */
	if (s->len > sizeof(s->digest)) {
		c->bad = true;
		return;
	}
	d1_enc_u32(c, s->alg);
	d1_enc_bytes(c, s->digest, s->len);
}

void d1_enc_objkey(struct d1_cursor *c, const struct d1_objkey *k)
{
	d1_enc_uuid(c, &k->export_uuid);
	d1_enc_uuid(c, &k->object_uuid);
}

bool d1_dec_u8(struct d1_cursor *c, uint8_t *v)
{
	const uint8_t *at = d1_dec_take(c, 1);

	if (!at)
		return false;
	*v = at[0];
	return true;
}

bool d1_dec_u16(struct d1_cursor *c, uint16_t *v)
{
	const uint8_t *at = d1_dec_take(c, 2);

	if (!at)
		return false;
	*v = (uint16_t)(((uint16_t)at[0] << 8) | at[1]);
	return true;
}

bool d1_dec_u32(struct d1_cursor *c, uint32_t *v)
{
	const uint8_t *at = d1_dec_take(c, 4);

	if (!at)
		return false;
	*v = ((uint32_t)at[0] << 24) | ((uint32_t)at[1] << 16) |
	     ((uint32_t)at[2] << 8) | (uint32_t)at[3];
	return true;
}

bool d1_dec_u64(struct d1_cursor *c, uint64_t *v)
{
	const uint8_t *at = d1_dec_take(c, 8);
	uint64_t out = 0;
	unsigned int i;

	if (!at)
		return false;
	for (i = 0; i < 8; i++)
		out = (out << 8) | at[i];
	*v = out;
	return true;
}

bool d1_dec_bool(struct d1_cursor *c, bool *v)
{
	uint8_t raw;

	if (!d1_dec_u8(c, &raw))
		return false;
	/* Only the two canonical spellings decode. */
	if (raw > 1u) {
		c->bad = true;
		return false;
	}
	*v = raw == 1u;
	return true;
}

bool d1_dec_raw(struct d1_cursor *c, void *out, size_t len)
{
	const uint8_t *at = d1_dec_take(c, len);

	if (!at)
		return false;
	if (len)
		memcpy(out, at, len);
	return true;
}

bool d1_dec_bytes_ref(struct d1_cursor *c, const uint8_t **out, uint32_t *len,
		      uint32_t max)
{
	uint32_t n;
	const uint8_t *at;

	if (!d1_dec_u32(c, &n))
		return false;
	if (n > max) {
		c->bad = true;
		return false;
	}
	at = d1_dec_take(c, n);
	if (!at)
		return false;
	*out = at;
	*len = n;
	return true;
}

bool d1_dec_uuid(struct d1_cursor *c, struct d1_uuid *u)
{
	return d1_dec_raw(c, u->bytes, sizeof(u->bytes));
}

bool d1_dec_opt_u64(struct d1_cursor *c, bool *present, uint64_t *v)
{
	uint8_t tag;

	if (!d1_dec_u8(c, &tag))
		return false;
	if (tag > 1u) {
		c->bad = true;
		return false;
	}
	*present = tag == 1u;
	if (!*present) {
		*v = 0;
		return true;
	}
	return d1_dec_u64(c, v);
}

bool d1_dec_guard(struct d1_cursor *c, struct d1_guard *g)
{
	return d1_dec_u32(c, &g->generation) && d1_dec_u32(c, &g->writer) &&
	       d1_dec_bool(c, &g->never_written);
}

bool d1_dec_owner(struct d1_cursor *c, struct d1_owner *o)
{
	return d1_dec_u64(c, &o->cohort.raw) && d1_dec_u32(c, &o->writer) &&
	       d1_dec_u32(c, &o->co_id);
}

bool d1_dec_opkey(struct d1_cursor *c, struct d1_opkey *k)
{
	return d1_dec_uuid(c, &k->origin) && d1_dec_u64(c, &k->sequence) &&
	       d1_dec_u32(c, &k->ordinal);
}

bool d1_dec_checksum(struct d1_cursor *c, struct d1_checksum *s)
{
	const uint8_t *digest;
	uint32_t len;

	if (!d1_dec_u32(c, &s->alg))
		return false;
	if (!d1_dec_bytes_ref(c, &digest, &len, sizeof(s->digest)))
		return false;
	s->len = len;
	memset(s->digest, 0, sizeof(s->digest));
	memcpy(s->digest, digest, len);
	return true;
}

bool d1_dec_objkey(struct d1_cursor *c, struct d1_objkey *k)
{
	return d1_dec_uuid(c, &k->export_uuid) &&
	       d1_dec_uuid(c, &k->object_uuid);
}

bool d1_dec_finished(const struct d1_cursor *c)
{
	return !c->bad && c->len == c->cap;
}

bool d1_add_u64(uint64_t a, uint64_t b, uint64_t *out)
{
	if (b > UINT64_MAX - a)
		return false;
	*out = a + b;
	return true;
}

bool d1_mul_u64(uint64_t a, uint64_t b, uint64_t *out)
{
	if (a != 0 && b > UINT64_MAX / a)
		return false;
	*out = a * b;
	return true;
}

bool d1_add_size(size_t a, size_t b, size_t *out)
{
	if (b > SIZE_MAX - a)
		return false;
	*out = a + b;
	return true;
}
