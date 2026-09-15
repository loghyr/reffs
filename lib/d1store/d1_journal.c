/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#include <stdlib.h>
#include <string.h>

#include "d1_codec.h"
#include "d1_digest.h"
#include "d1_journal.h"

bool d1_journal_init(struct d1_journal *j)
{
	memset(j, 0, sizeof(*j));
	j->cap = 4096;
	j->buf = calloc(1, j->cap);
	if (!j->buf)
		return false;
	/* Sequences start at one; zero means no record was ever written. */
	j->next_seq = 1;
	return true;
}

void d1_journal_fini(struct d1_journal *j)
{
	free(j->buf);
	memset(j, 0, sizeof(*j));
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

bool d1_journal_append(struct d1_journal *j, uint32_t type,
		       const uint8_t *payload, uint32_t len)
{
	struct d1_cursor cur;
	size_t total;
	uint32_t crc;

	/*
	 * An armed fault is consumed whether or not the append would have
	 * succeeded, so a test cannot accidentally arm one failure and
	 * observe two.
	 */
	if (j->fail_next) {
		j->fail_next = false;
		return false;
	}
	if (len > D1_JOURNAL_RECORD_MAX)
		return false;
	total = D1_JOURNAL_HEADER_BYTES + len + D1_JOURNAL_TRAILER_BYTES;
	if (!d1_journal_reserve(j, total))
		return false;

	d1_enc_init(&cur, j->buf + j->len, total);
	d1_enc_u32(&cur, D1_JOURNAL_MAGIC);
	d1_enc_u32(&cur, type);
	d1_enc_u64(&cur, j->next_seq);
	d1_enc_u32(&cur, len);
	d1_enc_raw(&cur, payload, len);
	if (cur.bad)
		return false;
	crc = d1_crc32c(j->buf + j->len, D1_JOURNAL_HEADER_BYTES + len);
	d1_enc_u32(&cur, crc);
	if (cur.bad)
		return false;

	j->len += total;
	j->next_seq++;
	return true;
}

void d1_journal_truncate(struct d1_journal *j, size_t len)
{
	if (len < j->len)
		j->len = len;
}

void d1_journal_cursor_init(struct d1_journal_cursor *c, const uint8_t *buf,
			    size_t len)
{
	c->buf = buf;
	c->len = len;
	c->at = 0;
}

bool d1_journal_next(struct d1_journal_cursor *c, uint32_t *type, uint64_t *seq,
		     const uint8_t **payload, uint32_t *len)
{
	struct d1_cursor cur;
	const uint8_t *trailer;
	uint32_t magic, rtype, rlen, stored, computed;
	uint64_t rseq;
	size_t remaining = c->len - c->at;

	if (c->at >= c->len || remaining < D1_JOURNAL_HEADER_BYTES)
		return false;
	d1_dec_init(&cur, c->buf + c->at, remaining);
	if (!d1_dec_u32(&cur, &magic) || magic != D1_JOURNAL_MAGIC)
		return false;
	if (!d1_dec_u32(&cur, &rtype) || !d1_dec_u64(&cur, &rseq) ||
	    !d1_dec_u32(&cur, &rlen))
		return false;
	if (rlen > D1_JOURNAL_RECORD_MAX)
		return false;
	/* The record must be wholly present before its CRC is worth reading. */
	if (remaining - D1_JOURNAL_HEADER_BYTES <
	    (size_t)rlen + D1_JOURNAL_TRAILER_BYTES)
		return false;

	computed = d1_crc32c(c->buf + c->at, D1_JOURNAL_HEADER_BYTES + rlen);
	/* The trailer is read exactly as the canonical form writes it. */
	trailer = c->buf + c->at + D1_JOURNAL_HEADER_BYTES + rlen;
	stored = ((uint32_t)trailer[0] << 24) | ((uint32_t)trailer[1] << 16) |
		 ((uint32_t)trailer[2] << 8) | (uint32_t)trailer[3];
	if (computed != stored)
		return false;

	*type = rtype;
	*seq = rseq;
	*payload = c->buf + c->at + D1_JOURNAL_HEADER_BYTES;
	*len = rlen;
	c->at += D1_JOURNAL_HEADER_BYTES + rlen + D1_JOURNAL_TRAILER_BYTES;
	return true;
}
