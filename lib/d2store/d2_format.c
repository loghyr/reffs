/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <stdlib.h>
#include <string.h>

#include <openssl/sha.h>
#include <zlib.h>

#include "d1_codec.h"
#include "d1_digest.h"
#include "d2_format.h"

#define D2_SB_DOMAIN "FFV2-D2-SB-v1"
#define D2_WAL_DOMAIN "FFV2-D2-WAL-v1"
#define D2_PAY_DOMAIN "FFV2-D2-PAY-v1"

static bool d2_zero(const uint8_t *p, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++)
		if (p[i])
			return false;
	return true;
}

static bool d2_equal_or_unspecified(const uint8_t got[16],
				    const uint8_t expected[16])
{
	return !expected || memcmp(got, expected, 16) == 0;
}

static void d2_domain_join(const char *domain, const void *buf, size_t len,
			   uint8_t **joined, size_t *joined_len)
{
	size_t dlen = strlen(domain);

	*joined = NULL;
	*joined_len = 0;
	if (len > SIZE_MAX - dlen)
		return;
	*joined = malloc(dlen + len);
	if (!*joined)
		return;
	memcpy(*joined, domain, dlen);
	if (len)
		memcpy(*joined + dlen, buf, len);
	*joined_len = dlen + len;
}

uint32_t d2_crc32c_domain(const char *domain, const void *buf, size_t len)
{
	uint8_t *joined;
	size_t joined_len;
	uint32_t crc;

	d2_domain_join(domain, buf, len, &joined, &joined_len);
	if (!joined)
		return 0;
	crc = d1_crc32c(joined, joined_len);
	free(joined);
	return crc;
}

void d2_hash_domain(const char *domain, const void *buf, size_t len,
		    uint8_t out[32])
{
	uint8_t *joined;
	size_t joined_len;

	d2_domain_join(domain, buf, len, &joined, &joined_len);
	if (!joined) {
		memset(out, 0, 32);
		return;
	}
	d1_sha256(joined, joined_len, out);
	free(joined);
}

void d2_file_key(const void *handle, uint32_t len, uint8_t out[32])
{
	struct d1_cursor c;
	uint8_t *buf;

	if (len > D2_MAX_HANDLE_BYTES) {
		memset(out, 0, 32);
		return;
	}
	buf = malloc(4u + len);
	if (!buf) {
		memset(out, 0, 32);
		return;
	}
	d1_enc_init(&c, buf, 4u + len);
	d1_enc_u32(&c, len);
	d1_enc_raw(&c, handle, len);
	d2_hash_domain("FFV2-D2-FILEKEY-v1", buf, c.len, out);
	free(buf);
}

void d2_operation_key(const uint8_t session[16], uint32_t slot,
		      uint32_t sequence, uint32_t ordinal, uint8_t out[32])
{
	struct d1_cursor c;
	uint8_t buf[28];

	d1_enc_init(&c, buf, sizeof(buf));
	d1_enc_raw(&c, session, 16);
	d1_enc_u32(&c, slot);
	d1_enc_u32(&c, sequence);
	d1_enc_u32(&c, ordinal);
	d2_hash_domain("FFV2-D2-OPKEY-v1", buf, sizeof(buf), out);
}

static void d2_enc_common(struct d1_cursor *c, uint32_t magic, uint16_t type,
			  uint32_t header_bytes, uint32_t total_bytes,
			  const uint8_t store_uuid[16])
{
	d1_enc_u32(c, magic);
	d1_enc_u16(c, D2_FORMAT_VERSION);
	d1_enc_u16(c, type);
	d1_enc_u32(c, header_bytes);
	d1_enc_u32(c, total_bytes);
	d1_enc_u32(c, ~total_bytes);
	d1_enc_raw(c, store_uuid, 16);
}

static bool d2_dec_common(struct d1_cursor *c, uint32_t magic, uint16_t type,
			  uint32_t header_bytes, uint32_t total_bytes,
			  const uint8_t expected_store[16],
			  uint8_t store_uuid[16])
{
	uint32_t got_magic, got_header, got_total, complement;
	uint16_t version, got_type;

	if (!d1_dec_u32(c, &got_magic) || !d1_dec_u16(c, &version) ||
	    !d1_dec_u16(c, &got_type) || !d1_dec_u32(c, &got_header) ||
	    !d1_dec_u32(c, &got_total) || !d1_dec_u32(c, &complement) ||
	    !d1_dec_raw(c, store_uuid, 16))
		return false;
	return got_magic == magic && version == D2_FORMAT_VERSION &&
	       got_type == type && got_header == header_bytes &&
	       got_total == total_bytes &&
	       (got_total ^ complement) == UINT32_MAX &&
	       d2_equal_or_unspecified(store_uuid, expected_store);
}

static void d2_enc_crc(struct d1_cursor *c, const char *domain,
		       const uint8_t *buf, size_t covered)
{
	d1_enc_u32(c, d2_crc32c_domain(domain, buf, covered));
}

static bool d2_check_crc(const char *domain, const uint8_t *buf, size_t total)
{
	struct d1_cursor c;
	uint32_t stored;

	if (total < 4)
		return false;
	d1_dec_init(&c, buf + total - 4, 4);
	return d1_dec_u32(&c, &stored) &&
	       stored == d2_crc32c_domain(domain, buf, total - 4);
}

bool d2_super_encode(const struct d2_superblock *sb,
		     uint8_t out[D2_SB_SLOT_BYTES])
{
	struct d1_cursor c;

	if (!sb || sb->root_handle_len > D2_MAX_HANDLE_BYTES ||
	    sb->state < D2_SB_CLEAN || sb->state > D2_SB_RETIRED ||
	    sb->format_floor > D2_FORMAT_VERSION)
		return false;
	memset(out, 0, D2_SB_SLOT_BYTES);
	d1_enc_init(&c, out, D2_SB_RECORD_BYTES);
	d2_enc_common(&c, D2_SB_MAGIC, D2_SB_SLOT, D2_PROLOGUE_BYTES,
		      D2_SB_RECORD_BYTES, sb->store_uuid);
	d1_enc_u64(&c, sb->generation);
	d1_enc_raw(&c, sb->fs_uuid, 16);
	d1_enc_u64(&c, sb->fs_dev_hint);
	d1_enc_u16(&c, sb->root_handle_type);
	d1_enc_u16(&c, sb->root_handle_len);
	d1_enc_raw(&c, sb->root_handle, D2_MAX_HANDLE_BYTES);
	d1_enc_raw(&c, sb->export_uuid, 16);
	d1_enc_u64(&c, sb->ds_incarnation);
	d1_enc_u64(&c, sb->verifier_epoch);
	d1_enc_raw(&c, sb->write_verifier, 8);
	d1_enc_raw(&c, sb->wal_uuid, 16);
	d1_enc_raw(&c, sb->payload_uuid, 16);
	d1_enc_raw(&c, sb->binding_token_digest, 32);
	d1_enc_u64(&c, sb->wal_durable_lsn);
	d1_enc_u64(&c, sb->wal_durable_bytes);
	d1_enc_u64(&c, sb->payload_durable_bytes);
	d1_enc_u64(&c, sb->capacity_wal_bytes);
	d1_enc_u64(&c, sb->capacity_payload_bytes);
	d1_enc_u32(&c, sb->state);
	d1_enc_u32(&c, sb->format_floor);
	d1_enc_u32(&c, 0);
	if (!d1_cursor_ok(&c) || c.len != D2_SB_RECORD_BYTES - 4)
		return false;
	d2_enc_crc(&c, D2_SB_DOMAIN, out, c.len);
	return d1_cursor_ok(&c) && c.len == D2_SB_RECORD_BYTES;
}

bool d2_super_decode(const uint8_t in[D2_SB_SLOT_BYTES],
		     const uint8_t expected_store[16], struct d2_superblock *sb)
{
	struct d1_cursor c;
	uint8_t store[16];
	uint32_t reserved;

	if (!in || !sb || !d2_check_crc(D2_SB_DOMAIN, in, D2_SB_RECORD_BYTES) ||
	    !d2_zero(in + D2_SB_RECORD_BYTES,
		     D2_SB_SLOT_BYTES - D2_SB_RECORD_BYTES))
		return false;
	memset(sb, 0, sizeof(*sb));
	d1_dec_init(&c, in, D2_SB_RECORD_BYTES - 4);
	if (!d2_dec_common(&c, D2_SB_MAGIC, D2_SB_SLOT, D2_PROLOGUE_BYTES,
			   D2_SB_RECORD_BYTES, expected_store, store) ||
	    !d1_dec_u64(&c, &sb->generation) ||
	    !d1_dec_raw(&c, sb->fs_uuid, 16) ||
	    !d1_dec_u64(&c, &sb->fs_dev_hint) ||
	    !d1_dec_u16(&c, &sb->root_handle_type) ||
	    !d1_dec_u16(&c, &sb->root_handle_len) ||
	    !d1_dec_raw(&c, sb->root_handle, D2_MAX_HANDLE_BYTES) ||
	    !d1_dec_raw(&c, sb->export_uuid, 16) ||
	    !d1_dec_u64(&c, &sb->ds_incarnation) ||
	    !d1_dec_u64(&c, &sb->verifier_epoch) ||
	    !d1_dec_raw(&c, sb->write_verifier, 8) ||
	    !d1_dec_raw(&c, sb->wal_uuid, 16) ||
	    !d1_dec_raw(&c, sb->payload_uuid, 16) ||
	    !d1_dec_raw(&c, sb->binding_token_digest, 32) ||
	    !d1_dec_u64(&c, &sb->wal_durable_lsn) ||
	    !d1_dec_u64(&c, &sb->wal_durable_bytes) ||
	    !d1_dec_u64(&c, &sb->payload_durable_bytes) ||
	    !d1_dec_u64(&c, &sb->capacity_wal_bytes) ||
	    !d1_dec_u64(&c, &sb->capacity_payload_bytes) ||
	    !d1_dec_u32(&c, &sb->state) || !d1_dec_u32(&c, &sb->format_floor) ||
	    !d1_dec_u32(&c, &reserved) || !d1_dec_finished(&c))
		return false;
	memcpy(sb->store_uuid, store, 16);
	return sb->root_handle_len <= D2_MAX_HANDLE_BYTES &&
	       d2_zero(sb->root_handle + sb->root_handle_len,
		       D2_MAX_HANDLE_BYTES - sb->root_handle_len) &&
	       sb->state >= D2_SB_CLEAN && sb->state <= D2_SB_RETIRED &&
	       sb->format_floor == D2_FORMAT_VERSION && sb->generation != 0 &&
	       sb->ds_incarnation <= D2_MAX_INCARNATION &&
	       sb->wal_durable_bytes <= sb->capacity_wal_bytes &&
	       sb->payload_durable_bytes <= sb->capacity_payload_bytes &&
	       sb->capacity_wal_bytes >= D2_MIN_WAL_BYTES &&
	       sb->capacity_payload_bytes >= D2_PAYLOAD_ALIGN * 2u &&
	       sb->capacity_payload_bytes % D2_PAYLOAD_ALIGN == 0 &&
	       reserved == 0;
}

bool d2_payload_header_encode(const struct d2_payload_header *h,
			      uint8_t out[D2_PAYLOAD_ALIGN])
{
	struct d1_cursor c;

	if (!h || !out)
		return false;
	memset(out, 0, D2_PAYLOAD_ALIGN);
	d1_enc_init(&c, out, D2_PAYLOAD_FILE_HEADER_BYTES + 4);
	d2_enc_common(&c, D2_PAYLOAD_MAGIC, D2_PAYLOAD_FILE_HEADER,
		      D2_PAYLOAD_FILE_HEADER_BYTES,
		      D2_PAYLOAD_FILE_HEADER_BYTES + 4, h->store_uuid);
	d1_enc_raw(&c, h->payload_uuid, 16);
	d1_enc_raw(&c, h->wal_uuid, 16);
	d1_enc_u64(&c, D2_PAYLOAD_ALIGN);
	d1_enc_u32(&c, 0);
	if (!d1_cursor_ok(&c) || c.len != D2_PAYLOAD_FILE_HEADER_BYTES)
		return false;
	d2_enc_crc(&c, D2_PAY_DOMAIN, out, c.len);
	return d1_cursor_ok(&c);
}

bool d2_payload_header_decode(const uint8_t in[D2_PAYLOAD_ALIGN],
			      const uint8_t expected_store[16],
			      struct d2_payload_header *h)
{
	struct d1_cursor c;
	uint8_t store[16];
	uint64_t object_bytes;
	uint32_t reserved;

	if (!in || !h ||
	    !d2_check_crc(D2_PAY_DOMAIN, in,
			  D2_PAYLOAD_FILE_HEADER_BYTES + 4) ||
	    !d2_zero(in + D2_PAYLOAD_FILE_HEADER_BYTES + 4,
		     D2_PAYLOAD_ALIGN - D2_PAYLOAD_FILE_HEADER_BYTES - 4))
		return false;
	d1_dec_init(&c, in, D2_PAYLOAD_FILE_HEADER_BYTES);
	if (!d2_dec_common(&c, D2_PAYLOAD_MAGIC, D2_PAYLOAD_FILE_HEADER,
			   D2_PAYLOAD_FILE_HEADER_BYTES,
			   D2_PAYLOAD_FILE_HEADER_BYTES + 4, expected_store,
			   store) ||
	    !d1_dec_raw(&c, h->payload_uuid, 16) ||
	    !d1_dec_raw(&c, h->wal_uuid, 16) ||
	    !d1_dec_u64(&c, &object_bytes) || !d1_dec_u32(&c, &reserved) ||
	    !d1_dec_finished(&c))
		return false;
	memcpy(h->store_uuid, store, 16);
	return object_bytes == D2_PAYLOAD_ALIGN && reserved == 0;
}

uint64_t d2_payload_object_bytes(uint32_t content_len)
{
	uint64_t total = D2_PAYLOAD_HEADER_BYTES + (uint64_t)content_len + 4u;

	return (total + D2_PAYLOAD_ALIGN - 1u) &
	       ~(uint64_t)(D2_PAYLOAD_ALIGN - 1u);
}

static void d2_put_be32(uint8_t out[4], uint32_t value)
{
	out[0] = (uint8_t)(value >> 24);
	out[1] = (uint8_t)(value >> 16);
	out[2] = (uint8_t)(value >> 8);
	out[3] = (uint8_t)value;
}

static uint32_t d2_get_be32(const uint8_t in[4])
{
	return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
	       ((uint32_t)in[2] << 8) | in[3];
}

static void d2_put_be64(uint8_t out[8], uint64_t value)
{
	unsigned int i;

	for (i = 0; i < 8; i++)
		out[i] = (uint8_t)(value >> (56 - 8 * i));
}

static uint32_t d2_get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static bool d2_content_checksum(uint32_t alg, const uint8_t *content,
				uint32_t len, uint8_t out[64],
				uint32_t *out_len)
{
	uint64_t a = 0, b = 0, c = 0, d = 0;
	uLong crc;
	uint32_t i;

	memset(out, 0, 64);
	switch (alg) {
	case 0:
		*out_len = 0;
		return true;
	case 1:
		crc = crc32(0L, Z_NULL, 0);
		crc = crc32(crc, content, len);
		d2_put_be32(out, (uint32_t)crc);
		*out_len = 4;
		return true;
	case 2:
		d2_put_be32(out, d1_crc32c(content, len));
		*out_len = 4;
		return true;
	case 3:
		if (len % 4)
			return false;
		for (i = 0; i < len; i += 4) {
			a += d2_get_le32(content + i);
			b += a;
			c += b;
			d += c;
		}
		d2_put_be64(out, a);
		d2_put_be64(out + 8, b);
		d2_put_be64(out + 16, c);
		d2_put_be64(out + 24, d);
		*out_len = 32;
		return true;
	case 4:
		if (!SHA256(content, len, out))
			return false;
		*out_len = SHA256_DIGEST_LENGTH;
		return true;
	default:
		return false;
	}
}

static bool d2_content_checksum_ok(const struct d2_payload_object *o)
{
	uint8_t computed[64];
	uint32_t len;

	return d2_content_checksum(o->content_alg, o->content, o->content_len,
				   computed, &len) &&
	       len == o->content_ck_len &&
	       memcmp(computed, o->content_ck, len) == 0 &&
	       d2_zero(o->content_ck + len, D2_MAX_CHECKSUM_BYTES - len);
}

bool d2_payload_encode(const struct d2_payload_object *o, uint8_t *out,
		       size_t cap, size_t *written)
{
	struct d1_cursor c;
	uint64_t object_bytes;
	uint32_t total;

	if (!o || !out || !written ||
	    o->content_ck_len > D2_MAX_CHECKSUM_BYTES ||
	    (o->content_len && !o->content) ||
	    o->content_len > UINT32_MAX - D2_PAYLOAD_HEADER_BYTES - 4u ||
	    !d2_content_checksum_ok(o))
		return false;
	object_bytes = d2_payload_object_bytes(o->content_len);
	if (object_bytes > cap || object_bytes > SIZE_MAX ||
	    (o->object_bytes && o->object_bytes != object_bytes))
		return false;
	total = D2_PAYLOAD_HEADER_BYTES + o->content_len + 4u;
	memset(out, 0, (size_t)object_bytes);
	d1_enc_init(&c, out, total);
	d2_enc_common(&c, D2_PAYLOAD_MAGIC, D2_PAYLOAD_DATA,
		      D2_PAYLOAD_HEADER_BYTES, total, o->store_uuid);
	d1_enc_u64(&c, o->payload_object_id);
	d1_enc_u32(&c, d1_crc32c(o->content, o->content_len));
	d1_enc_u32(&c, o->content_alg);
	d1_enc_u32(&c, o->content_ck_len);
	d1_enc_raw(&c, o->content_ck, D2_MAX_CHECKSUM_BYTES);
	d1_enc_u32(&c, o->content_len);
	d1_enc_u64(&c, object_bytes);
	d1_enc_u32(&c, 0);
	if (!d1_cursor_ok(&c) || c.len != D2_PAYLOAD_HEADER_BYTES)
		return false;
	d1_enc_raw(&c, o->content, o->content_len);
	d2_enc_crc(&c, D2_PAY_DOMAIN, out, D2_PAYLOAD_HEADER_BYTES);
	if (!d1_cursor_ok(&c) || c.len != total)
		return false;
	*written = (size_t)object_bytes;
	return true;
}

bool d2_payload_decode_content(const uint8_t *in, size_t len,
			       const uint8_t expected_store[16],
			       struct d2_payload_object *o, bool *content_ok)
{
	struct d1_cursor c;
	uint8_t store[16];
	uint32_t total, reserved, stored_crc;
	uint64_t object_bytes;

	if (!in || !o || !content_ok || len < D2_PAYLOAD_HEADER_BYTES + 4)
		return false;
	*content_ok = false;
	total = ((uint32_t)in[12] << 24) | ((uint32_t)in[13] << 16) |
		((uint32_t)in[14] << 8) | in[15];
	if (total < D2_PAYLOAD_HEADER_BYTES + 4 || total > len)
		return false;
	memset(o, 0, sizeof(*o));
	d1_dec_init(&c, in, D2_PAYLOAD_HEADER_BYTES);
	if (!d2_dec_common(&c, D2_PAYLOAD_MAGIC, D2_PAYLOAD_DATA,
			   D2_PAYLOAD_HEADER_BYTES, total, expected_store,
			   store) ||
	    !d1_dec_u64(&c, &o->payload_object_id) ||
	    !d1_dec_u32(&c, &stored_crc) || !d1_dec_u32(&c, &o->content_alg) ||
	    !d1_dec_u32(&c, &o->content_ck_len) ||
	    !d1_dec_raw(&c, o->content_ck, D2_MAX_CHECKSUM_BYTES) ||
	    !d1_dec_u32(&c, &o->content_len) ||
	    !d1_dec_u64(&c, &object_bytes) || !d1_dec_u32(&c, &reserved) ||
	    !d1_dec_finished(&c))
		return false;
	if (o->content_ck_len > D2_MAX_CHECKSUM_BYTES ||
	    !d2_zero(o->content_ck + o->content_ck_len,
		     D2_MAX_CHECKSUM_BYTES - o->content_ck_len) ||
	    total != D2_PAYLOAD_HEADER_BYTES + o->content_len + 4u ||
	    object_bytes != d2_payload_object_bytes(o->content_len) ||
	    object_bytes > len || reserved != 0 ||
	    (((uint32_t)in[total - 4] << 24) | ((uint32_t)in[total - 3] << 16) |
	     ((uint32_t)in[total - 2] << 8) | in[total - 1]) !=
		    d2_crc32c_domain(D2_PAY_DOMAIN, in,
				     D2_PAYLOAD_HEADER_BYTES) ||
	    !d2_zero(in + total, (size_t)object_bytes - total))
		return false;
	o->content = in + D2_PAYLOAD_HEADER_BYTES;
	o->content_crc32c = stored_crc;
	o->object_bytes = object_bytes;
	memcpy(o->store_uuid, store, 16);
	*content_ok = stored_crc == d1_crc32c(o->content, o->content_len) &&
		      d2_content_checksum_ok(o);
	return true;
}

bool d2_payload_decode(const uint8_t *in, size_t len,
		       const uint8_t expected_store[16],
		       struct d2_payload_object *o)
{
	bool content_ok;

	return d2_payload_decode_content(in, len, expected_store, o,
					 &content_ok) &&
	       content_ok;
}

static void d2_enc_wal_header(struct d1_cursor *c,
			      const struct d2_wal_header *h, uint32_t total)
{
	d2_enc_common(c, D2_WAL_MAGIC, h->family, D2_WAL_HEADER_BYTES, total,
		      h->store_uuid);
	d1_enc_raw(c, h->wal_uuid, 16);
	d1_enc_u64(c, h->lsn);
	d1_enc_u64(c, h->ds_incarnation);
	d1_enc_u32(c, 0);
}

bool d2_wal_header_decode(const uint8_t *in, size_t len,
			  const uint8_t expected_store[16],
			  const uint8_t expected_wal[16],
			  struct d2_wal_header *h)
{
	struct d1_cursor c;
	uint8_t store[16];
	uint32_t magic, header, total, complement, reserved;
	uint16_t version, family;

	if (!in || !h || len < D2_WAL_HEADER_BYTES)
		return false;
	d1_dec_init(&c, in, D2_WAL_HEADER_BYTES);
	if (!d1_dec_u32(&c, &magic) || !d1_dec_u16(&c, &version) ||
	    !d1_dec_u16(&c, &family) || !d1_dec_u32(&c, &header) ||
	    !d1_dec_u32(&c, &total) || !d1_dec_u32(&c, &complement) ||
	    !d1_dec_raw(&c, store, 16) || !d1_dec_raw(&c, h->wal_uuid, 16) ||
	    !d1_dec_u64(&c, &h->lsn) || !d1_dec_u64(&c, &h->ds_incarnation) ||
	    !d1_dec_u32(&c, &reserved) || !d1_dec_finished(&c))
		return false;
	if (magic != D2_WAL_MAGIC || version != D2_FORMAT_VERSION ||
	    family < D2_REC_START || family > D2_REC_COHORT ||
	    header != D2_WAL_HEADER_BYTES || total < D2_WAL_HEADER_BYTES + 4 ||
	    total > D2_MAX_RECORD_BYTES || total > len ||
	    (total ^ complement) != UINT32_MAX || reserved != 0 ||
	    !d2_equal_or_unspecified(store, expected_store) ||
	    !d2_equal_or_unspecified(h->wal_uuid, expected_wal) ||
	    h->lsn == 0 || h->ds_incarnation == 0 ||
	    h->ds_incarnation > D2_MAX_INCARNATION ||
	    !d2_check_crc(D2_WAL_DOMAIN, in, total))
		return false;
	h->family = family;
	h->total_bytes = total;
	memcpy(h->store_uuid, store, 16);
	return true;
}

static bool d2_start_decision_ok(const struct d2_start *s)
{
	uint32_t d = s->recovery_decision;
	uint32_t scan = d & (D2_RD_CLEAN_SCAN | D2_RD_TRUNCATED_TAIL |
			     D2_RD_DISCARDED_UNSTABLE);

	if (d & ~(D2_RD_FIRST_PROVISION | D2_RD_CLEAN_SCAN |
		  D2_RD_TRUNCATED_TAIL | D2_RD_DISCARDED_UNSTABLE |
		  D2_RD_ADMIN_CLEARED_FENCE | D2_RD_TRUNCATED_NONZERO_TAIL))
		return false;
	if (d & D2_RD_FIRST_PROVISION)
		return d == D2_RD_FIRST_PROVISION && s->prev_incarnation == 0 &&
		       s->last_valid_lsn_before == 0 && s->truncated_bytes == 0;
	if (!scan || (d & D2_RD_CLEAN_SCAN &&
		      d & (D2_RD_TRUNCATED_TAIL | D2_RD_DISCARDED_UNSTABLE)))
		return false;
	if (d & D2_RD_ADMIN_CLEARED_FENCE && !scan)
		return false;
	if (d & D2_RD_TRUNCATED_NONZERO_TAIL && !(d & D2_RD_TRUNCATED_TAIL))
		return false;
	if (!!s->truncated_bytes != !!(d & D2_RD_TRUNCATED_TAIL))
		return false;
	return s->verifier_changed == !!(d & (D2_RD_DISCARDED_UNSTABLE |
					      D2_RD_TRUNCATED_NONZERO_TAIL));
}

bool d2_start_encode(const struct d2_wal_header *h, const struct d2_start *s,
		     uint8_t out[D2_START_RECORD_BYTES])
{
	struct d1_cursor c;

	if (!h || !s || !out || h->family != D2_REC_START ||
	    h->ds_incarnation != s->ds_incarnation || !d2_start_decision_ok(s))
		return false;
	d1_enc_init(&c, out, D2_START_RECORD_BYTES);
	d2_enc_wal_header(&c, h, D2_START_RECORD_BYTES);
	d1_enc_u32(&c, D2_FORMAT_VERSION);
	d1_enc_u64(&c, s->ds_incarnation);
	d1_enc_u64(&c, s->prev_incarnation);
	d1_enc_u64(&c, s->verifier_epoch);
	d1_enc_bool(&c, s->verifier_changed);
	d1_enc_u32(&c, s->recovery_decision);
	d1_enc_u64(&c, s->truncated_bytes);
	d1_enc_u64(&c, s->last_valid_lsn_before);
	d1_enc_u64(&c, s->wal_append_cursor);
	d1_enc_u64(&c, s->payload_append_cursor);
	d1_enc_u64(&c, s->capacity_wal_bytes);
	d1_enc_u64(&c, s->capacity_payload_bytes);
	d1_enc_raw(&c, s->export_uuid, 16);
	d1_enc_u64(&c, s->live_txn_wal_reserved);
	d1_enc_u64(&c, s->live_txn_payload_outstanding);
	d1_enc_u64(&c, s->live_txn_payload_staged);
	if (!d1_cursor_ok(&c) || c.len != D2_START_RECORD_BYTES - 4)
		return false;
	d2_enc_crc(&c, D2_WAL_DOMAIN, out, c.len);
	return d1_cursor_ok(&c);
}

bool d2_start_decode(const uint8_t *record, size_t len,
		     const struct d2_wal_header *h, struct d2_start *s)
{
	struct d1_cursor c;
	uint32_t format;

	if (!record || !h || !s || h->family != D2_REC_START ||
	    h->total_bytes != D2_START_RECORD_BYTES || len < h->total_bytes)
		return false;
	memset(s, 0, sizeof(*s));
	d1_dec_init(&c, record + D2_WAL_HEADER_BYTES, D2_START_BODY_BYTES);
	if (!d1_dec_u32(&c, &format) || !d1_dec_u64(&c, &s->ds_incarnation) ||
	    !d1_dec_u64(&c, &s->prev_incarnation) ||
	    !d1_dec_u64(&c, &s->verifier_epoch) ||
	    !d1_dec_bool(&c, &s->verifier_changed) ||
	    !d1_dec_u32(&c, &s->recovery_decision) ||
	    !d1_dec_u64(&c, &s->truncated_bytes) ||
	    !d1_dec_u64(&c, &s->last_valid_lsn_before) ||
	    !d1_dec_u64(&c, &s->wal_append_cursor) ||
	    !d1_dec_u64(&c, &s->payload_append_cursor) ||
	    !d1_dec_u64(&c, &s->capacity_wal_bytes) ||
	    !d1_dec_u64(&c, &s->capacity_payload_bytes) ||
	    !d1_dec_raw(&c, s->export_uuid, 16) ||
	    !d1_dec_u64(&c, &s->live_txn_wal_reserved) ||
	    !d1_dec_u64(&c, &s->live_txn_payload_outstanding) ||
	    !d1_dec_u64(&c, &s->live_txn_payload_staged) ||
	    !d1_dec_finished(&c))
		return false;
	return format == D2_FORMAT_VERSION &&
	       s->ds_incarnation == h->ds_incarnation &&
	       d2_start_decision_ok(s);
}

static void d2_enc_key(struct d1_cursor *c, const struct d2_key_block *k)
{
	d1_enc_raw(c, k->session, 16);
	d1_enc_u32(c, k->slot);
	d1_enc_u32(c, k->sequence);
	d1_enc_u32(c, k->compound_ordinal);
	d1_enc_raw(c, k->operation_key, 32);
	d1_enc_raw(c, k->request_digest, 32);
}

static bool d2_dec_key(struct d1_cursor *c, struct d2_key_block *k)
{
	uint8_t computed[32];

	if (!d1_dec_raw(c, k->session, 16) || !d1_dec_u32(c, &k->slot) ||
	    !d1_dec_u32(c, &k->sequence) ||
	    !d1_dec_u32(c, &k->compound_ordinal) ||
	    !d1_dec_raw(c, k->operation_key, 32) ||
	    !d1_dec_raw(c, k->request_digest, 32))
		return false;
	if (d2_zero(k->session, 16))
		return false;
	d2_operation_key(k->session, k->slot, k->sequence, k->compound_ordinal,
			 computed);
	return memcmp(computed, k->operation_key, 32) == 0;
}

static void d2_enc_admission(struct d1_cursor *c,
			     const struct d2_admission_block *a)
{
	d1_enc_raw(c, a->session, 16);
	d1_enc_raw(c, a->principal, 16);
	d1_enc_raw(c, a->issuer, 16);
	d1_enc_u64(c, a->authority_epoch);
	d1_enc_u64(c, a->fence_sequence);
	d1_enc_u64(c, a->lease_epoch);
	d1_enc_u64(c, a->client_id);
	d1_enc_u32(c, a->stateid_seqid);
	d1_enc_raw(c, a->stateid_other, 12);
	d1_enc_u32(c, a->writer);
	d1_enc_u32(c, a->rights);
}

static bool d2_dec_admission(struct d1_cursor *c, struct d2_admission_block *a)
{
	return d1_dec_raw(c, a->session, 16) &&
	       d1_dec_raw(c, a->principal, 16) &&
	       d1_dec_raw(c, a->issuer, 16) &&
	       d1_dec_u64(c, &a->authority_epoch) &&
	       d1_dec_u64(c, &a->fence_sequence) &&
	       d1_dec_u64(c, &a->lease_epoch) && d1_dec_u64(c, &a->client_id) &&
	       d1_dec_u32(c, &a->stateid_seqid) &&
	       d1_dec_raw(c, a->stateid_other, 12) &&
	       d1_dec_u32(c, &a->writer) && d1_dec_u32(c, &a->rights);
}

static bool d2_control_body_size(uint32_t subtype, const uint8_t *body,
				 uint32_t len)
{
	struct d1_cursor c;
	const uint8_t *entry;
	uint32_t count, fixed, each, i;
	uint8_t last[48];
	size_t key_off, key_len;

	if (subtype < D2_CTL_FILE_REGISTER ||
	    subtype > D2_CTL_CERTIFICATE_INSTALL)
		return false;
	switch (subtype) {
	case D2_CTL_FILE_REGISTER: {
		uint8_t computed[32];
		uint16_t handle_type, handle_len;
		uint32_t chunk_bytes;
		uint64_t max_chunks;
		uint64_t logical_eof;

		if (len != 184)
			return false;
		handle_type = ((uint16_t)body[32] << 8) | body[33];
		handle_len = ((uint16_t)body[34] << 8) | body[35];
		chunk_bytes = ((uint32_t)body[164] << 24) |
			      ((uint32_t)body[165] << 16) |
			      ((uint32_t)body[166] << 8) | body[167];
		max_chunks = ((uint64_t)body[168] << 56) |
			     ((uint64_t)body[169] << 48) |
			     ((uint64_t)body[170] << 40) |
			     ((uint64_t)body[171] << 32) |
			     ((uint64_t)body[172] << 24) |
			     ((uint64_t)body[173] << 16) |
			     ((uint64_t)body[174] << 8) | body[175];
		logical_eof = ((uint64_t)body[176] << 56) |
			      ((uint64_t)body[177] << 48) |
			      ((uint64_t)body[178] << 40) |
			      ((uint64_t)body[179] << 32) |
			      ((uint64_t)body[180] << 24) |
			      ((uint64_t)body[181] << 16) |
			      ((uint64_t)body[182] << 8) | body[183];
		if (handle_type != 0 || handle_len != 32 ||
		    !d2_zero(body + 36 + handle_len,
			     D2_MAX_HANDLE_BYTES - handle_len) ||
		    chunk_bytes < 4096 || chunk_bytes > 1024u * 1024u ||
		    chunk_bytes % 4096 || max_chunks == 0 || logical_eof != 0 ||
		    (uint64_t)chunk_bytes * max_chunks / chunk_bytes !=
			    max_chunks)
			return false;
		d2_file_key(body + 36, handle_len, computed);
		return memcmp(computed, body, 32) == 0;
	}
	case D2_CTL_FILE_TOMBSTONE:
		fixed = 36;
		break;
	case D2_CTL_AUTHORITY_REVOKE:
		fixed = 28;
		break;
	case D2_CTL_TRUST_STATEID:
		fixed = 84;
		break;
	case D2_CTL_REVOKE_STATEID:
		fixed = 60;
		break;
	case D2_CTL_CUSTODY:
		fixed = 36;
		if (len == fixed &&
		    (d2_get_be32(body + 16) < 1 || d2_get_be32(body + 16) > 3))
			return false;
		break;
	case D2_CTL_COORDINATOR_COMPLETION:
		fixed = 100;
		break;
	case D2_CTL_EXPORT_TOMBSTONE:
		fixed = 12;
		break;
	case D2_CTL_POSTCOND:
		fixed = 68;
		if (len == fixed &&
		    (d2_get_be32(body + 64) < 1 || d2_get_be32(body + 64) > 3))
			return false;
		break;
	case D2_CTL_LEASE_EXPIRE:
		fixed = 52;
		break;
	case D2_CTL_CERTIFICATE_INSTALL:
		fixed = 56;
		if (len == fixed &&
		    (d2_zero(body, 16) || d2_zero(body + 16, 8) ||
		     d2_zero(body + 24, 32)))
			return false;
		break;
	case D2_CTL_AUTHORITY_ADMIT:
		fixed = 28;
		each = 16;
		key_off = 0;
		key_len = 16;
		goto counted_at_end;
	case D2_CTL_LEASE_REAP:
		fixed = 4;
		each = 48;
		key_off = 0;
		key_len = 8;
		goto counted_first;
	case D2_CTL_RECOVERY_ADMIT:
		fixed = 4;
		each = 40;
		key_off = 0;
		key_len = 8;
		goto counted_first;
	case D2_CTL_EPISODE_MARK:
		fixed = 68;
		each = 24;
		key_len = 0;
		goto counted_at_64;
	default:
		fixed = 64;
		each = 40;
		key_len = 0;
		goto counted_at_28;
	}
	return len == fixed;

counted_first:
	d1_dec_init(&c, body, len);
	if (!d1_dec_u32(&c, &count))
		return false;
	goto counted;
counted_at_end:
	if (len < fixed)
		return false;
	d1_dec_init(&c, body + 24, len - 24);
	if (!d1_dec_u32(&c, &count))
		return false;
	entry = body + fixed;
	goto counted_check;
counted_at_64:
	if (len < fixed)
		return false;
	d1_dec_init(&c, body + 64, len - 64);
	if (!d1_dec_u32(&c, &count))
		return false;
	goto counted;
counted_at_28:
	if (len < fixed)
		return false;
	d1_dec_init(&c, body + 28, len - 28);
	if (!d1_dec_u32(&c, &count))
		return false;
counted:
	entry = body + fixed;
counted_check:
	if (count > D2_MAX_COUNTED_ENTRIES ||
	    (subtype != D2_CTL_AUTHORITY_ADMIT && count == 0) ||
	    len != fixed + count * each)
		return false;
	if (!key_len)
		return count <= D2_MAX_BATCH_ENTRIES;
	memset(last, 0, sizeof(last));
	for (i = 0; i < count; i++) {
		const uint8_t *key = entry + i * each + key_off;

		if (i && memcmp(last, key, key_len) >= 0)
			return false;
		memcpy(last, key, key_len);
	}
	return true;
}

static bool d2_control_key_ok(const struct d2_control *c, uint64_t incarnation)
{
	uint8_t computed[32] = { 0 };
	bool store_generated = c->subtype == D2_CTL_FILE_REGISTER ||
			       c->subtype == D2_CTL_FILE_TOMBSTONE ||
			       c->subtype == D2_CTL_LEASE_REAP ||
			       c->subtype == D2_CTL_EXPORT_TOMBSTONE ||
			       c->subtype == D2_CTL_POSTCOND;

	if (!store_generated) {
		if (d2_zero(c->key.session, 16))
			return false;
		d2_operation_key(c->key.session, c->key.slot, c->key.sequence,
				 c->key.compound_ordinal, computed);
		return memcmp(computed, c->key.operation_key, 32) == 0;
	}
	if (!d2_zero(c->key.session, 16) || c->key.slot ||
	    c->key.compound_ordinal || !d2_zero(c->key.request_digest, 32) ||
	    c->key.sequence == 0)
		return false;
	computed[0] = (uint8_t)(incarnation >> 56);
	computed[1] = (uint8_t)(incarnation >> 48);
	computed[2] = (uint8_t)(incarnation >> 40);
	computed[3] = (uint8_t)(incarnation >> 32);
	computed[4] = (uint8_t)(incarnation >> 24);
	computed[5] = (uint8_t)(incarnation >> 16);
	computed[6] = (uint8_t)(incarnation >> 8);
	computed[7] = (uint8_t)incarnation;
	computed[12] = (uint8_t)(c->key.sequence >> 24);
	computed[13] = (uint8_t)(c->key.sequence >> 16);
	computed[14] = (uint8_t)(c->key.sequence >> 8);
	computed[15] = (uint8_t)c->key.sequence;
	return memcmp(computed, c->key.operation_key, 32) == 0;
}

bool d2_control_encode(const struct d2_wal_header *h,
		       const struct d2_control *c, uint8_t *out, size_t cap,
		       size_t *written)
{
	struct d1_cursor cur;
	uint32_t total;

	if (!h || !c || !out || !written || h->family != D2_REC_CONTROL ||
	    !d2_control_body_size(c->subtype, c->body, c->body_len) ||
	    !d2_control_key_ok(c, h->ds_incarnation))
		return false;
	total = D2_WAL_HEADER_BYTES + D2_CONTROL_PREFIX_BYTES + c->body_len + 4;
	if (total > cap)
		return false;
	d1_enc_init(&cur, out, total);
	d2_enc_wal_header(&cur, h, total);
	d1_enc_u32(&cur, c->subtype);
	d1_enc_u32(&cur, c->transition);
	d1_enc_u32(&cur, c->status);
	d1_enc_raw(&cur, c->key.operation_key, 32);
	d1_enc_raw(&cur, c->key.request_digest, 32);
	d1_enc_raw(&cur, c->key.session, 16);
	d1_enc_u32(&cur, c->key.slot);
	d1_enc_u32(&cur, c->key.sequence);
	d1_enc_u32(&cur, c->key.compound_ordinal);
	d1_enc_raw(&cur, c->admission_issuer, 16);
	d1_enc_u64(&cur, c->admission_authority_epoch);
	d1_enc_u64(&cur, c->admission_client_id);
	d1_enc_raw(&cur, c->body, c->body_len);
	if (!d1_cursor_ok(&cur) || cur.len != total - 4)
		return false;
	d2_enc_crc(&cur, D2_WAL_DOMAIN, out, cur.len);
	*written = total;
	return d1_cursor_ok(&cur);
}

bool d2_control_decode(const uint8_t *record, size_t len,
		       const struct d2_wal_header *h, struct d2_control *c)
{
	struct d1_cursor cur;
	uint32_t body_len;

	if (!record || !h || !c || h->family != D2_REC_CONTROL ||
	    h->total_bytes > len ||
	    h->total_bytes < D2_WAL_HEADER_BYTES + D2_CONTROL_PREFIX_BYTES + 4)
		return false;
	memset(c, 0, sizeof(*c));
	body_len = h->total_bytes - D2_WAL_HEADER_BYTES -
		   D2_CONTROL_PREFIX_BYTES - 4;
	if (body_len > sizeof(c->body))
		return false;
	d1_dec_init(&cur, record + D2_WAL_HEADER_BYTES,
		    D2_CONTROL_PREFIX_BYTES + body_len);
	if (!d1_dec_u32(&cur, &c->subtype) ||
	    !d1_dec_u32(&cur, &c->transition) ||
	    !d1_dec_u32(&cur, &c->status) ||
	    !d1_dec_raw(&cur, c->key.operation_key, 32) ||
	    !d1_dec_raw(&cur, c->key.request_digest, 32) ||
	    !d1_dec_raw(&cur, c->key.session, 16) ||
	    !d1_dec_u32(&cur, &c->key.slot) ||
	    !d1_dec_u32(&cur, &c->key.sequence) ||
	    !d1_dec_u32(&cur, &c->key.compound_ordinal) ||
	    !d1_dec_raw(&cur, c->admission_issuer, 16) ||
	    !d1_dec_u64(&cur, &c->admission_authority_epoch) ||
	    !d1_dec_u64(&cur, &c->admission_client_id) ||
	    !d1_dec_raw(&cur, c->body, body_len) || !d1_dec_finished(&cur))
		return false;
	c->body_len = body_len;
	return d2_control_body_size(c->subtype, c->body, body_len) &&
	       d2_control_key_ok(c, h->ds_incarnation) &&
	       (c->transition == D2_COMMITTED || c->transition == D2_REFUSED) &&
	       c->status >= D1_OK && c->status <= D1_REPLAY_CONFLICT &&
	       ((c->transition == D2_COMMITTED && c->status == D1_OK) ||
		(c->transition == D2_REFUSED && c->status != D1_OK &&
		 c->status != D1_NOSPC && c->status != D1_IO));
}

static bool d2_result_ck_ok(uint32_t len, const uint8_t ck[64])
{
	return len <= D2_MAX_CHECKSUM_BYTES &&
	       d2_zero(ck + len, D2_MAX_CHECKSUM_BYTES - len);
}

static bool d2_payload_ref_ok(uint64_t id, uint64_t offset,
			      uint64_t record_incarnation)
{
	uint64_t allocating_incarnation;

	if (!id)
		return offset == 0;
	allocating_incarnation = id >> 40;
	return allocating_incarnation != 0 &&
	       allocating_incarnation <= record_incarnation &&
	       offset >= D2_PAYLOAD_ALIGN && offset % D2_PAYLOAD_ALIGN == 0;
}

static bool d2_recorded_refusal(uint32_t status)
{
	return status == D1_INVALID || status == D1_GUARDED ||
	       status == D1_OWNER_CONFLICT || status == D1_STALE_AUTH ||
	       status == D1_BAD_PHASE || status == D1_NO_PREDECESSOR ||
	       status == D1_QUARANTINED || status == D1_CHECKSUM;
}

static bool d2_entry_pair(uint32_t transition, uint32_t status)
{
	if (transition == D2_PREPARED || transition == D2_FINALIZED ||
	    transition == D2_COMMITTED || transition == D2_ROLLED_BACK)
		return status == D1_OK;
	if (transition == D2_ABORTED)
		return status == D1_GUARDED || status == D1_OWNER_CONFLICT ||
		       status == D1_STALE_AUTH || status == D1_NO_PREDECESSOR ||
		       status == D1_QUARANTINED || status == D1_CHECKSUM;
	return transition == D2_REFUSED && d2_recorded_refusal(status);
}

static bool d2_cohort_triple(uint32_t transition, uint32_t status, bool keep)
{
	if (transition == D2_ADMITTED || transition == D2_PREPARED ||
	    transition == D2_FINALIZED || transition == D2_COMMITTED ||
	    transition == D2_UNLOCKED)
		return status == D1_OK && !keep;
	if (transition == D2_ABORTED)
		return (!keep && status == D1_OK) ||
		       (keep &&
			(status == D1_OWNER_CONFLICT || status == D1_CHECKSUM));
	return transition == D2_REFUSED && !keep && d2_recorded_refusal(status);
}

bool d2_entry_encode(const struct d2_wal_header *h, const struct d2_entry *e,
		     uint8_t out[D2_ENTRY_RECORD_BYTES])
{
	struct d1_cursor c;

	if (!h || !e || !out || h->family != D2_REC_ENTRY ||
	    !d2_entry_pair(e->transition, e->status) ||
	    (!e->predecessor_present &&
	     (e->predecessor_object_id || e->predecessor_generation)) ||
	    (!e->postcond_present && e->postcond_id) ||
	    !d2_result_ck_ok(e->result_ck_len, e->result_ck))
		return false;
	d1_enc_init(&c, out, D2_ENTRY_RECORD_BYTES);
	d2_enc_wal_header(&c, h, D2_ENTRY_RECORD_BYTES);
	d1_enc_u32(&c, e->transition);
	d1_enc_u32(&c, e->status);
	d1_enc_u32(&c, e->disposition);
	d1_enc_u32(&c, e->stability);
	d1_enc_u32(&c, e->batch_ordinal);
	d1_enc_u32(&c, e->batch_count);
	d1_enc_raw(&c, e->file_key, 32);
	d1_enc_u64(&c, e->chunk_index);
	d1_enc_u64(&c, e->txn_id);
	d2_enc_key(&c, &e->key);
	d2_enc_admission(&c, &e->admission);
	d1_enc_u64(&c, e->owner_cohort);
	d1_enc_u32(&c, e->owner_client_id);
	d1_enc_u32(&c, e->owner_co_id);
	d1_enc_u32(&c, e->generation);
	d1_enc_bool(&c, e->predecessor_present);
	d1_enc_u64(&c, e->predecessor_object_id);
	d1_enc_u32(&c, e->predecessor_generation);
	d1_enc_bool(&c, e->postcond_present);
	d1_enc_u64(&c, e->postcond_id);
	d1_enc_u64(&c, e->payload_object_id);
	d1_enc_u64(&c, e->payload_object_offset);
	d1_enc_u32(&c, e->payload_content_len);
	d1_enc_u32(&c, e->extent_kind);
	d1_enc_u64(&c, e->extent_high_water);
	d1_enc_u64(&c, e->extent_highest_index);
	d1_enc_u64(&c, e->index_generation);
	d1_enc_raw(&c, e->result_verifier, 8);
	d1_enc_bool(&c, e->result_activated);
	d1_enc_u64(&c, e->result_visible_object_id);
	d1_enc_u32(&c, e->result_effective_len);
	d1_enc_u32(&c, e->result_ck_alg);
	d1_enc_u32(&c, e->result_ck_len);
	d1_enc_raw(&c, e->result_ck, 64);
	d1_enc_bool(&c, e->result_guard_never_written);
	d1_enc_u32(&c, e->result_guard_generation);
	d1_enc_u32(&c, e->result_guard_writer);
	if (!d1_cursor_ok(&c) || c.len != D2_ENTRY_RECORD_BYTES - 4)
		return false;
	d2_enc_crc(&c, D2_WAL_DOMAIN, out, c.len);
	return d1_cursor_ok(&c);
}

bool d2_entry_decode(const uint8_t *record, size_t len,
		     const struct d2_wal_header *h, struct d2_entry *e)
{
	struct d1_cursor c;

	if (!record || !h || !e || h->family != D2_REC_ENTRY ||
	    h->total_bytes != D2_ENTRY_RECORD_BYTES || len < h->total_bytes)
		return false;
	memset(e, 0, sizeof(*e));
	d1_dec_init(&c, record + D2_WAL_HEADER_BYTES, D2_ENTRY_BODY_BYTES);
	if (!d1_dec_u32(&c, &e->transition) || !d1_dec_u32(&c, &e->status) ||
	    !d1_dec_u32(&c, &e->disposition) ||
	    !d1_dec_u32(&c, &e->stability) ||
	    !d1_dec_u32(&c, &e->batch_ordinal) ||
	    !d1_dec_u32(&c, &e->batch_count) ||
	    !d1_dec_raw(&c, e->file_key, 32) ||
	    !d1_dec_u64(&c, &e->chunk_index) || !d1_dec_u64(&c, &e->txn_id) ||
	    !d2_dec_key(&c, &e->key) || !d2_dec_admission(&c, &e->admission) ||
	    !d1_dec_u64(&c, &e->owner_cohort) ||
	    !d1_dec_u32(&c, &e->owner_client_id) ||
	    !d1_dec_u32(&c, &e->owner_co_id) ||
	    !d1_dec_u32(&c, &e->generation) ||
	    !d1_dec_bool(&c, &e->predecessor_present) ||
	    !d1_dec_u64(&c, &e->predecessor_object_id) ||
	    !d1_dec_u32(&c, &e->predecessor_generation) ||
	    !d1_dec_bool(&c, &e->postcond_present) ||
	    !d1_dec_u64(&c, &e->postcond_id) ||
	    !d1_dec_u64(&c, &e->payload_object_id) ||
	    !d1_dec_u64(&c, &e->payload_object_offset) ||
	    !d1_dec_u32(&c, &e->payload_content_len) ||
	    !d1_dec_u32(&c, &e->extent_kind) ||
	    !d1_dec_u64(&c, &e->extent_high_water) ||
	    !d1_dec_u64(&c, &e->extent_highest_index) ||
	    !d1_dec_u64(&c, &e->index_generation) ||
	    !d1_dec_raw(&c, e->result_verifier, 8) ||
	    !d1_dec_bool(&c, &e->result_activated) ||
	    !d1_dec_u64(&c, &e->result_visible_object_id) ||
	    !d1_dec_u32(&c, &e->result_effective_len) ||
	    !d1_dec_u32(&c, &e->result_ck_alg) ||
	    !d1_dec_u32(&c, &e->result_ck_len) ||
	    !d1_dec_raw(&c, e->result_ck, 64) ||
	    !d1_dec_bool(&c, &e->result_guard_never_written) ||
	    !d1_dec_u32(&c, &e->result_guard_generation) ||
	    !d1_dec_u32(&c, &e->result_guard_writer) || !d1_dec_finished(&c))
		return false;
	return e->batch_count >= 1 && e->batch_count <= D2_MAX_BATCH_ENTRIES &&
	       e->batch_ordinal < e->batch_count &&
	       d2_entry_pair(e->transition, e->status) && e->status >= D1_OK &&
	       e->status <= D1_REPLAY_CONFLICT &&
	       e->disposition == D1_COMPLETED && e->stability >= 1 &&
	       e->stability <= 3 && e->extent_kind >= D2_EXTENT_UNCHANGED &&
	       e->extent_kind <= D2_EXTENT_SHRINK &&
	       (e->predecessor_present ||
		(!e->predecessor_object_id && !e->predecessor_generation)) &&
	       (e->postcond_present || !e->postcond_id) &&
	       d2_payload_ref_ok(e->payload_object_id, e->payload_object_offset,
				 h->ds_incarnation) &&
	       d2_result_ck_ok(e->result_ck_len, e->result_ck);
}

static void d2_enc_member(struct d1_cursor *c, const struct d2_cohort_member *m)
{
	d1_enc_u32(c, m->repair_mode);
	d1_enc_raw(c, m->file_key, 32);
	d1_enc_u64(c, m->chunk_index);
	d1_enc_u64(c, m->member_txn_id);
	d1_enc_u64(c, m->owner_cohort);
	d1_enc_u32(c, m->owner_client_id);
	d1_enc_u32(c, m->owner_co_id);
	d1_enc_u64(c, m->custody_id);
	d1_enc_bool(c, m->postcond_present);
	d1_enc_u64(c, m->postcond_id);
	d1_enc_u64(c, m->successor_object_id);
	d1_enc_bool(c, m->predecessor_present);
	d1_enc_u64(c, m->predecessor_object_id);
	d1_enc_u32(c, m->predecessor_generation);
	d1_enc_u64(c, m->payload_object_id);
	d1_enc_u64(c, m->payload_object_offset);
	d1_enc_u32(c, m->payload_content_len);
	d1_enc_bool(c, m->staged);
	d1_enc_u32(c, m->member_status);
	d1_enc_u32(c, m->extent_kind);
	d1_enc_u64(c, m->extent_high_water);
	d1_enc_u64(c, m->extent_highest_index);
	d1_enc_u64(c, m->result_visible_object_id);
	d1_enc_u32(c, m->result_effective_len);
	d1_enc_u32(c, m->result_ck_alg);
	d1_enc_u32(c, m->result_ck_len);
	d1_enc_raw(c, m->result_ck, 64);
	d1_enc_bool(c, m->result_guard_never_written);
	d1_enc_u32(c, m->result_guard_generation);
	d1_enc_u32(c, m->result_guard_writer);
}

static bool d2_dec_member(struct d1_cursor *c, struct d2_cohort_member *m)
{
	return d1_dec_u32(c, &m->repair_mode) &&
	       d1_dec_raw(c, m->file_key, 32) &&
	       d1_dec_u64(c, &m->chunk_index) &&
	       d1_dec_u64(c, &m->member_txn_id) &&
	       d1_dec_u64(c, &m->owner_cohort) &&
	       d1_dec_u32(c, &m->owner_client_id) &&
	       d1_dec_u32(c, &m->owner_co_id) &&
	       d1_dec_u64(c, &m->custody_id) &&
	       d1_dec_bool(c, &m->postcond_present) &&
	       d1_dec_u64(c, &m->postcond_id) &&
	       d1_dec_u64(c, &m->successor_object_id) &&
	       d1_dec_bool(c, &m->predecessor_present) &&
	       d1_dec_u64(c, &m->predecessor_object_id) &&
	       d1_dec_u32(c, &m->predecessor_generation) &&
	       d1_dec_u64(c, &m->payload_object_id) &&
	       d1_dec_u64(c, &m->payload_object_offset) &&
	       d1_dec_u32(c, &m->payload_content_len) &&
	       d1_dec_bool(c, &m->staged) && d1_dec_u32(c, &m->member_status) &&
	       d1_dec_u32(c, &m->extent_kind) &&
	       d1_dec_u64(c, &m->extent_high_water) &&
	       d1_dec_u64(c, &m->extent_highest_index) &&
	       d1_dec_u64(c, &m->result_visible_object_id) &&
	       d1_dec_u32(c, &m->result_effective_len) &&
	       d1_dec_u32(c, &m->result_ck_alg) &&
	       d1_dec_u32(c, &m->result_ck_len) &&
	       d1_dec_raw(c, m->result_ck, 64) &&
	       d1_dec_bool(c, &m->result_guard_never_written) &&
	       d1_dec_u32(c, &m->result_guard_generation) &&
	       d1_dec_u32(c, &m->result_guard_writer) &&
	       d2_result_ck_ok(m->result_ck_len, m->result_ck);
}

bool d2_cohort_encode(const struct d2_wal_header *h, const struct d2_cohort *co,
		      uint8_t *out, size_t cap, size_t *written)
{
	struct d1_cursor c;
	uint32_t total, i;

	if (!h || !co || !out || !written || h->family != D2_REC_COHORT ||
	    co->member_count < 1 || co->member_count > D2_MAX_BATCH_ENTRIES ||
	    co->flags & ~3u ||
	    !d2_cohort_triple(co->transition, co->status, !!(co->flags & 2u)))
		return false;
	for (i = 0; i < co->member_count; i++)
		if (!d2_result_ck_ok(co->members[i].result_ck_len,
				     co->members[i].result_ck))
			return false;
	total = D2_WAL_HEADER_BYTES + D2_COHORT_PREFIX_BYTES +
		co->member_count * D2_COHORT_MEMBER_BYTES + 4;
	if (total > cap)
		return false;
	d1_enc_init(&c, out, total);
	d2_enc_wal_header(&c, h, total);
	d1_enc_u32(&c, co->transition);
	d1_enc_u32(&c, co->status);
	d1_enc_u32(&c, co->disposition);
	d1_enc_u64(&c, co->cohort_id);
	d1_enc_raw(&c, co->episode_uuid, 16);
	d1_enc_u32(&c, co->flags);
	d2_enc_key(&c, &co->key);
	d2_enc_admission(&c, &co->admission);
	d1_enc_u64(&c, co->index_generation);
	d1_enc_raw(&c, co->result_verifier, 8);
	d1_enc_u32(&c, co->member_count);
	for (i = 0; i < co->member_count; i++)
		d2_enc_member(&c, &co->members[i]);
	if (!d1_cursor_ok(&c) || c.len != total - 4)
		return false;
	d2_enc_crc(&c, D2_WAL_DOMAIN, out, c.len);
	*written = total;
	return d1_cursor_ok(&c);
}

bool d2_cohort_decode(const uint8_t *record, size_t len,
		      const struct d2_wal_header *h, struct d2_cohort *co)
{
	struct d1_cursor c;
	uint32_t i, count_at;

	if (!record || !h || !co || h->family != D2_REC_COHORT ||
	    h->total_bytes < D2_WAL_HEADER_BYTES + D2_COHORT_PREFIX_BYTES +
				     D2_COHORT_MEMBER_BYTES + 4 ||
	    h->total_bytes > len)
		return false;
	memset(co, 0, sizeof(*co));
	d1_dec_init(&c, record + D2_WAL_HEADER_BYTES,
		    h->total_bytes - D2_WAL_HEADER_BYTES - 4);
	if (!d1_dec_u32(&c, &co->transition) || !d1_dec_u32(&c, &co->status) ||
	    !d1_dec_u32(&c, &co->disposition) ||
	    !d1_dec_u64(&c, &co->cohort_id) ||
	    !d1_dec_raw(&c, co->episode_uuid, 16) ||
	    !d1_dec_u32(&c, &co->flags) || !d2_dec_key(&c, &co->key) ||
	    !d2_dec_admission(&c, &co->admission) ||
	    !d1_dec_u64(&c, &co->index_generation) ||
	    !d1_dec_raw(&c, co->result_verifier, 8) ||
	    !d1_dec_u32(&c, &co->member_count))
		return false;
	count_at = D2_WAL_HEADER_BYTES + D2_COHORT_PREFIX_BYTES +
		   co->member_count * D2_COHORT_MEMBER_BYTES + 4;
	if (co->member_count < 1 || co->member_count > D2_MAX_BATCH_ENTRIES ||
	    co->flags & ~3u || count_at != h->total_bytes ||
	    !d2_cohort_triple(co->transition, co->status, !!(co->flags & 2u)) ||
	    co->disposition != D1_COMPLETED ||
	    (!!(co->flags & 1u) != !d2_zero(co->episode_uuid, 16)))
		return false;
	for (i = 0; i < co->member_count; i++)
		if (!d2_dec_member(&c, &co->members[i]) ||
		    co->members[i].extent_kind < D2_EXTENT_UNCHANGED ||
		    co->members[i].extent_kind > D2_EXTENT_SHRINK ||
		    !d2_payload_ref_ok(co->members[i].payload_object_id,
				       co->members[i].payload_object_offset,
				       h->ds_incarnation))
			return false;
	return d1_dec_finished(&c);
}
