/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#include <stdlib.h>
#include <string.h>

#include "d1_codec.h"
#include "d1_digest.h"
#include "d1_journal.h"

bool d1_journal_init(struct d1_journal *j, const struct d1_uuid *store_uuid)
{
	memset(j, 0, sizeof(*j));
	j->cap = 4096;
	j->buf = calloc(1, j->cap);
	if (!j->buf)
		return false;
	/* LSNs begin at one; zero means no record was ever written. */
	j->next_lsn = 1;
	j->store_uuid = *store_uuid;
	return true;
}

void d1_journal_fini(struct d1_journal *j)
{
	free(j->buf);
	memset(j, 0, sizeof(*j));
}

void d1_journal_set_incarnation(struct d1_journal *j, uint64_t incarnation)
{
	j->incarnation = incarnation;
}

static bool d1_journal_reserve(struct d1_journal *j, size_t need)
{
	size_t want = j->cap;
	uint8_t *grown;

	if (j->len + need <= j->cap)
		return true;
	while (want < j->len + need) {
		if (want > SIZE_MAX / 2)
			return false;
		want *= 2;
	}
	grown = realloc(j->buf, want);
	if (!grown)
		return false;
	j->buf = grown;
	j->cap = want;
	return true;
}

bool d1_journal_append(struct d1_journal *j, uint32_t type, const uint8_t *body,
		       uint32_t len)
{
	struct d1_cursor cur;
	uint64_t total;
	uint32_t crc;

	/*
	 * An armed fault is consumed whether or not the append would have
	 * succeeded, so one arm cannot be observed as two failures.
	 */
	if (j->fail_append_in) {
		j->fail_append_in--;
		if (!j->fail_append_in)
			return false;
	}
	if (type < D1_REC_START || type > D1_REC_COHORT)
		return false;
	total = (uint64_t)D1_JOURNAL_HEADER_BYTES + len +
		D1_JOURNAL_TRAILER_BYTES;
	if (total > D1_JOURNAL_RECORD_MAX || total > UINT32_MAX)
		return false;
	/* Overflow in the LSN fails closed rather than wrapping. */
	if (j->next_lsn == UINT64_MAX)
		return false;
	if (!d1_journal_reserve(j, (size_t)total))
		return false;

	d1_enc_init(&cur, j->buf + j->len, (size_t)total);
	d1_enc_u32(&cur, D1_JOURNAL_MAGIC);
	d1_enc_u16(&cur, (uint16_t)D1_JOURNAL_FORMAT);
	d1_enc_u16(&cur, (uint16_t)type);
	d1_enc_u32(&cur, D1_JOURNAL_HEADER_BYTES);
	d1_enc_u32(&cur, (uint32_t)total);
	/* The complement is what makes a corrupted length detectable. */
	d1_enc_u32(&cur, ~(uint32_t)total);
	d1_enc_uuid(&cur, &j->store_uuid);
	d1_enc_u64(&cur, j->next_lsn);
	d1_enc_u64(&cur, j->incarnation);
	d1_enc_u32(&cur, 0u);
	d1_enc_raw(&cur, body, len);
	if (cur.bad || cur.len != total - D1_JOURNAL_TRAILER_BYTES)
		return false;
	crc = d1_crc32c(j->buf + j->len, cur.len);
	d1_enc_u32(&cur, crc);
	if (cur.bad)
		return false;

	j->len += (size_t)total;
	j->next_lsn++;
	return true;
}

bool d1_journal_adopt(struct d1_journal *j, const uint8_t *log, size_t durable,
		      uint64_t next_lsn)
{
	if (j->len)
		return false;
	if (!d1_journal_reserve(j, durable))
		return false;
	memcpy(j->buf, log, durable);
	j->len = durable;
	j->durable = durable;
	j->next_lsn = next_lsn;
	return true;
}

bool d1_journal_flush(struct d1_journal *j)
{
	if (j->fail_next_flush) {
		j->fail_next_flush = false;
		return false;
	}
	/* Appends only ever leave whole records, so this is a boundary. */
	j->durable = j->len;
	return true;
}

void d1_journal_cursor_init(struct d1_journal_cursor *c, const uint8_t *buf,
			    size_t durable, const struct d1_uuid *expect_uuid)
{
	c->buf = buf;
	c->durable = durable;
	c->at = 0;
	c->expect_uuid = expect_uuid;
	c->last_lsn = 0;
}

enum d1_journal_read d1_journal_next(struct d1_journal_cursor *c,
				     uint32_t *type, uint64_t *lsn,
				     uint64_t *incarnation,
				     const uint8_t **body, uint32_t *len)
{
	struct d1_cursor cur;
	struct d1_uuid uuid;
	const uint8_t *trailer;
	size_t remaining = c->durable - c->at;
	uint32_t magic, header_bytes, total, complement, reserved, stored;
	uint16_t format, rtype;
	uint64_t rlsn, rinc;

	if (c->at == c->durable)
		return D1_JOURNAL_CLEAN_END;
	/*
	 * From here every failure is corruption of bytes the writer
	 * claimed durable.  None of them may be reported as an end.
	 */
	if (remaining < D1_JOURNAL_HEADER_BYTES + D1_JOURNAL_TRAILER_BYTES)
		return D1_JOURNAL_CORRUPT;

	d1_dec_init(&cur, c->buf + c->at, remaining);
	if (!d1_dec_u32(&cur, &magic) || magic != D1_JOURNAL_MAGIC)
		return D1_JOURNAL_CORRUPT;
	if (!d1_dec_u16(&cur, &format) || format != D1_JOURNAL_FORMAT)
		return D1_JOURNAL_CORRUPT;
	if (!d1_dec_u16(&cur, &rtype) || rtype < D1_REC_START ||
	    rtype > D1_REC_COHORT)
		return D1_JOURNAL_CORRUPT;
	if (!d1_dec_u32(&cur, &header_bytes) ||
	    header_bytes != D1_JOURNAL_HEADER_BYTES)
		return D1_JOURNAL_CORRUPT;
	if (!d1_dec_u32(&cur, &total) || !d1_dec_u32(&cur, &complement))
		return D1_JOURNAL_CORRUPT;
	if (complement != ~total)
		return D1_JOURNAL_CORRUPT;
	if (total < D1_JOURNAL_HEADER_BYTES + D1_JOURNAL_TRAILER_BYTES ||
	    total > D1_JOURNAL_RECORD_MAX || (size_t)total > remaining)
		return D1_JOURNAL_CORRUPT;
	if (!d1_dec_uuid(&cur, &uuid))
		return D1_JOURNAL_CORRUPT;
	/* The expected UUID is an argument, not something the log asserts. */
	if (memcmp(&uuid, c->expect_uuid, sizeof(uuid)) != 0)
		return D1_JOURNAL_CORRUPT;
	if (!d1_dec_u64(&cur, &rlsn) || !d1_dec_u64(&cur, &rinc))
		return D1_JOURNAL_CORRUPT;
	if (!d1_dec_u32(&cur, &reserved) || reserved != 0u)
		return D1_JOURNAL_CORRUPT;
	/* LSNs are strictly increasing globally, so a gap is corruption. */
	if (rlsn != c->last_lsn + 1)
		return D1_JOURNAL_CORRUPT;

	trailer = c->buf + c->at + total - D1_JOURNAL_TRAILER_BYTES;
	stored = ((uint32_t)trailer[0] << 24) | ((uint32_t)trailer[1] << 16) |
		 ((uint32_t)trailer[2] << 8) | (uint32_t)trailer[3];
	if (d1_crc32c(c->buf + c->at, total - D1_JOURNAL_TRAILER_BYTES) !=
	    stored)
		return D1_JOURNAL_CORRUPT;

	*type = rtype;
	*lsn = rlsn;
	*incarnation = rinc;
	*body = c->buf + c->at + D1_JOURNAL_HEADER_BYTES;
	*len = total - D1_JOURNAL_HEADER_BYTES - D1_JOURNAL_TRAILER_BYTES;
	c->last_lsn = rlsn;
	c->at += total;
	return D1_JOURNAL_RECORD;
}
