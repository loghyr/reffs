/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * D1 storage model: journal record framing.
 *
 * What these cases are really asking is whether a reader that arrives
 * after a crash can tell three things apart using only the bytes and
 * the durable length it is given: a prefix it consumed exactly, a
 * suffix the writer never claimed, and corruption of data the writer
 * did claim.  The third must fail closed, including in the last record,
 * because a completed record quietly disappearing is data loss reported
 * as success.
 */

#include <stdio.h>
#include <string.h>

#include "d1_digest.h"
#include "d1_journal.h"

static unsigned int failures;

static void check(bool ok, const char *what)
{
	if (!ok) {
		failures++;
		fprintf(stderr, "FAIL: %s\n", what);
	}
}

static struct d1_uuid the_uuid;

static void fill_uuid(struct d1_uuid *u, uint8_t base)
{
	unsigned int i;

	for (i = 0; i < D1_UUID_BYTES; i++)
		u->bytes[i] = (uint8_t)(base + i);
}

static const uint8_t body_abc[3] = { 0x61, 0x62, 0x63 };

/* Read the whole durable prefix; answer how it ended and how many records. */
static enum d1_journal_read drain(const struct d1_journal *j, size_t durable,
				  unsigned int *count)
{
	struct d1_journal_cursor c;
	const uint8_t *body;
	uint32_t type, len;
	uint64_t lsn, incarnation;
	enum d1_journal_read got;

	*count = 0;
	d1_journal_cursor_init(&c, j->buf, durable, &the_uuid);
	for (;;) {
		got = d1_journal_next(&c, &type, &lsn, &incarnation, &body,
				      &len);
		if (got != D1_JOURNAL_RECORD)
			return got;
		(*count)++;
	}
}

/*
 * One record, byte for byte, in the layout section 9 fixes.  The CRC32C
 * comes from a separate implementation of the same reflected
 * polynomial, which reproduces that polynomial's published check value
 * 0xe3069283 for "123456789".
 */
static void test_golden_record(void)
{
	static const uint8_t want[] = {
		/* magic "D1J1" */
		0x44,
		0x31,
		0x4a,
		0x31,
		/* format 2 */
		0x00,
		0x02,
		/* record type START */
		0x00,
		0x01,
		/* header_bytes 56 */
		0x00,
		0x00,
		0x00,
		0x38,
		/* total_bytes 63 */
		0x00,
		0x00,
		0x00,
		0x3f,
		/* its bitwise complement */
		0xff,
		0xff,
		0xff,
		0xc0,
		/* store UUID */
		0x00,
		0x01,
		0x02,
		0x03,
		0x04,
		0x05,
		0x06,
		0x07,
		0x08,
		0x09,
		0x0a,
		0x0b,
		0x0c,
		0x0d,
		0x0e,
		0x0f,
		/* LSN 1 */
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x01,
		/* incarnation 1 */
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x01,
		/* reserved zero */
		0x00,
		0x00,
		0x00,
		0x00,
		/* body "abc" */
		0x61,
		0x62,
		0x63,
		/* CRC32C of everything above */
		0x53,
		0x72,
		0x3e,
		0xc7,
	};
	struct d1_journal j;

	check(d1_journal_init(&j, &the_uuid), "the journal initialises");
	d1_journal_set_incarnation(&j, 1);
	check(d1_journal_append(&j, D1_REC_START, body_abc, sizeof(body_abc)),
	      "the record appends");
	check(j.len == sizeof(want), "the record is the expected length");
	check(j.len == sizeof(want) && memcmp(j.buf, want, sizeof(want)) == 0,
	      "and is byte for byte the golden record");
	check(j.durable == 0, "an appended record is not yet durable");
	check(d1_journal_flush(&j), "the flush succeeds");
	check(j.durable == j.len, "and claims it");
	d1_journal_fini(&j);
}

static void test_round_trip(void)
{
	struct d1_journal j;
	struct d1_journal_cursor c;
	const uint8_t *body;
	uint32_t type, len;
	uint64_t lsn, incarnation;
	unsigned int n = 0;

	if (!d1_journal_init(&j, &the_uuid))
		return;
	d1_journal_set_incarnation(&j, 1);
	d1_journal_append(&j, D1_REC_START, body_abc, sizeof(body_abc));
	d1_journal_append(&j, D1_REC_ENTRY, body_abc, sizeof(body_abc));
	d1_journal_append(&j, D1_REC_CONTROL, NULL, 0);
	d1_journal_flush(&j);

	d1_journal_cursor_init(&c, j.buf, j.durable, &the_uuid);
	while (d1_journal_next(&c, &type, &lsn, &incarnation, &body, &len) ==
	       D1_JOURNAL_RECORD) {
		n++;
		check(lsn == n, "LSNs run in order from one");
		check(incarnation == 1, "and carry the current incarnation");
		if (n == 1)
			check(type == D1_REC_START && len == 3 &&
				      memcmp(body, body_abc, 3) == 0,
			      "the first record round trips");
		if (n == 3)
			check(type == D1_REC_CONTROL && len == 0,
			      "an empty body is a body");
	}
	check(n == 3, "every durable record is read back");
	d1_journal_fini(&j);
}

/*
 * The append length is not the durable length.  Bytes the writer never
 * claimed are simply not read, and the prefix that was claimed ends
 * cleanly.
 */
static void test_unflushed_is_not_read(void)
{
	struct d1_journal j;
	unsigned int n;

	if (!d1_journal_init(&j, &the_uuid))
		return;
	d1_journal_set_incarnation(&j, 1);
	d1_journal_append(&j, D1_REC_START, body_abc, sizeof(body_abc));
	d1_journal_flush(&j);
	d1_journal_append(&j, D1_REC_ENTRY, body_abc, sizeof(body_abc));

	check(j.len > j.durable, "there are appended but unclaimed bytes");
	check(drain(&j, j.durable, &n) == D1_JOURNAL_CLEAN_END && n == 1,
	      "the claimed prefix ends cleanly with one record");
	check(d1_journal_flush(&j), "a later flush claims the rest");
	check(drain(&j, j.durable, &n) == D1_JOURNAL_CLEAN_END && n == 2,
	      "and then both records are read");
	d1_journal_fini(&j);
}

/*
 * Corruption anywhere inside the durable prefix fails closed, including
 * in its final record.  This is the case that separates a torn suffix
 * from lost data: the writer claimed these bytes.
 */
static void test_durable_corruption_fails_closed(void)
{
	struct d1_journal j;
	size_t at;
	unsigned int n;
	bool all_closed = true;
	unsigned int bad_at = 0;

	if (!d1_journal_init(&j, &the_uuid))
		return;
	d1_journal_set_incarnation(&j, 1);
	d1_journal_append(&j, D1_REC_START, body_abc, sizeof(body_abc));
	d1_journal_append(&j, D1_REC_ENTRY, body_abc, sizeof(body_abc));
	d1_journal_flush(&j);

	for (at = 0; at < j.durable; at++) {
		enum d1_journal_read got;

		j.buf[at] ^= 0xffu;
		got = drain(&j, j.durable, &n);
		j.buf[at] ^= 0xffu;
		if (got != D1_JOURNAL_CORRUPT) {
			all_closed = false;
			bad_at = (unsigned int)at;
			break;
		}
	}
	if (!all_closed)
		fprintf(stderr, "first accepted corruption at offset %u\n",
			bad_at);
	check(all_closed,
	      "a flipped bit anywhere in the durable prefix fails closed");

	/* Truncating inside the prefix is corruption of it, not an end. */
	check(drain(&j, j.durable - 1u, &n) == D1_JOURNAL_CORRUPT,
	      "a durable length inside a record fails closed");
	check(drain(&j, j.durable, &n) == D1_JOURNAL_CLEAN_END && n == 2,
	      "and the intact prefix still reads both records");
	d1_journal_fini(&j);
}

/* A record is not trusted to say which store or which place it is from. */
static void test_identity_and_order(void)
{
	struct d1_journal j;
	struct d1_journal_cursor c;
	struct d1_uuid other;
	const uint8_t *body;
	uint32_t type, len;
	uint64_t lsn, incarnation;
	unsigned int n;

	if (!d1_journal_init(&j, &the_uuid))
		return;
	d1_journal_set_incarnation(&j, 1);
	d1_journal_append(&j, D1_REC_START, body_abc, sizeof(body_abc));
	d1_journal_append(&j, D1_REC_ENTRY, body_abc, sizeof(body_abc));
	d1_journal_flush(&j);

	fill_uuid(&other, 0x77);
	d1_journal_cursor_init(&c, j.buf, j.durable, &other);
	check(d1_journal_next(&c, &type, &lsn, &incarnation, &body, &len) ==
		      D1_JOURNAL_CORRUPT,
	      "a log from another store rebuilds nothing");

	/* An LSN that skips is a log with a record missing from the middle. */
	j.buf[43] = 9;
	check(drain(&j, j.durable, &n) == D1_JOURNAL_CORRUPT,
	      "a gap in the LSNs fails closed");
	j.buf[43] = 1;

	/* The length complement is what catches a corrupted length. */
	j.buf[15] ^= 0x01u;
	check(drain(&j, j.durable, &n) == D1_JOURNAL_CORRUPT,
	      "a length that disagrees with its complement fails closed");
	j.buf[15] ^= 0x01u;
	check(drain(&j, j.durable, &n) == D1_JOURNAL_CLEAN_END && n == 2,
	      "and the restored log reads cleanly again");
	d1_journal_fini(&j);
}

/* An armed fault refuses once and leaves nothing behind. */
static void test_faults_leave_no_residue(void)
{
	struct d1_journal j;
	size_t before_len, before_durable;
	uint64_t before_lsn;
	unsigned int n;

	if (!d1_journal_init(&j, &the_uuid))
		return;
	d1_journal_set_incarnation(&j, 1);
	d1_journal_append(&j, D1_REC_START, body_abc, sizeof(body_abc));
	d1_journal_flush(&j);
	before_len = j.len;
	before_durable = j.durable;
	before_lsn = j.next_lsn;

	j.fail_append_in = 1u;
	check(!d1_journal_append(&j, D1_REC_ENTRY, body_abc, sizeof(body_abc)),
	      "an armed append fault refuses the append");
	check(j.len == before_len, "the log is byte for byte unchanged");
	check(j.next_lsn == before_lsn, "and no LSN was consumed");

	/* A flush fault leaves the record appended but never claimed. */
	check(d1_journal_append(&j, D1_REC_ENTRY, body_abc, sizeof(body_abc)),
	      "the next append succeeds");
	j.fail_next_flush = true;
	check(!d1_journal_flush(&j), "an armed flush fault refuses the flush");
	check(j.durable == before_durable, "the durable length did not move");
	check(drain(&j, j.durable, &n) == D1_JOURNAL_CLEAN_END && n == 1,
	      "so a reader sees only what was claimed");

	/*
	 * A buffer that still holds an unclaimed append is a legitimate
	 * state for this primitive to be in, and a later flush would claim
	 * it.  Whether that is allowed to happen is the enclosing event's
	 * business, not the buffer's: see d1_journal_rollback below and
	 * the store-level oracle that uses it.
	 */
	check(d1_journal_flush(&j), "the fault is spent");
	check(drain(&j, j.durable, &n) == D1_JOURNAL_CLEAN_END && n == 2,
	      "and this primitive would claim the appended record");
	d1_journal_fini(&j);
}

/*
 * The format is a field of the frame, and the frame is this suite's.
 *
 * Section 9 puts the format in the header of every record, and the
 * reader checks it there rather than inferring it from anything the
 * body says.  A log written by another build is not a log this one may
 * read part of: it fails closed at the first record, including when
 * that record is the START.
 *
 * The store suite has always killed a reader that stopped checking,
 * because a whole history in an unreadable format stops rebuilding.
 * The check belongs to the framing, though, so the framing suite asks
 * it directly: this is the one that owns d1_journal.c.
 */
static void test_a_record_of_another_format(void)
{
	struct d1_journal j;
	struct d1_journal_cursor c;
	const uint8_t *body;
	uint32_t type, len, total, crc, i;
	uint64_t lsn, incarnation;
	unsigned int count;

	if (!d1_journal_init(&j, &the_uuid))
		return;
	d1_journal_set_incarnation(&j, 1);
	check(d1_journal_append(&j, D1_REC_START, body_abc, sizeof(body_abc)) &&
		      d1_journal_append(&j, D1_REC_ENTRY, body_abc,
					sizeof(body_abc)) &&
		      d1_journal_flush(&j),
	      "two records are written and flushed");
	check(drain(&j, j.durable, &count) == D1_JOURNAL_CLEAN_END &&
		      count == 2u,
	      "and both are read back");
	check(j.buf[4] == 0u && j.buf[5] == (uint8_t)D1_JOURNAL_FORMAT,
	      "the first record carries the format this build writes");

	/*
	 * The second record is restamped with the first format and its
	 * trailer recomputed, so nothing but the format is wrong: a
	 * reader that had stopped looking would consume it.
	 */
	total = (uint32_t)j.buf[12] << 24 | (uint32_t)j.buf[13] << 16 |
		(uint32_t)j.buf[14] << 8 | (uint32_t)j.buf[15];
	check(total > 0u && (size_t)total < j.durable,
	      "the first record's length is inside the log");
	if (total == 0u || (size_t)total >= j.durable) {
		d1_journal_fini(&j);
		return;
	}
	j.buf[total + 4u] = 0u;
	j.buf[total + 5u] = 1u;
	crc = d1_crc32c(j.buf + total,
			(size_t)(j.durable - total) - D1_JOURNAL_TRAILER_BYTES);
	for (i = 0; i < 4u; i++)
		j.buf[j.durable - 4u + i] = (uint8_t)(crc >> (24u - 8u * i));
	d1_journal_cursor_init(&c, j.buf, j.durable, &the_uuid);
	check(d1_journal_next(&c, &type, &lsn, &incarnation, &body, &len) ==
		      D1_JOURNAL_RECORD,
	      "the record before it still reads");
	check(d1_journal_next(&c, &type, &lsn, &incarnation, &body, &len) ==
		      D1_JOURNAL_CORRUPT,
	      "and a record of another format is refused, not skipped");

	/* And the same when it is the first record in the log. */
	j.buf[4] = 0u;
	j.buf[5] = 1u;
	crc = d1_crc32c(j.buf, (size_t)total - D1_JOURNAL_TRAILER_BYTES);
	for (i = 0; i < 4u; i++)
		j.buf[total - 4u + i] = (uint8_t)(crc >> (24u - 8u * i));
	d1_journal_cursor_init(&c, j.buf, j.durable, &the_uuid);
	check(d1_journal_next(&c, &type, &lsn, &incarnation, &body, &len) ==
		      D1_JOURNAL_CORRUPT,
	      "a log whose first record is another format reads nothing");
	d1_journal_fini(&j);
}

/*
 * Rolling an event back discards what it appended and gives its LSNs
 * back, without ever touching what was already claimed.
 */
static void test_rollback_discards_only_the_unclaimed(void)
{
	struct d1_journal j;
	size_t durable_before;
	uint64_t lsn_before;
	unsigned int n;

	if (!d1_journal_init(&j, &the_uuid))
		return;
	d1_journal_set_incarnation(&j, 1);
	d1_journal_append(&j, D1_REC_START, body_abc, sizeof(body_abc));
	d1_journal_flush(&j);
	durable_before = j.durable;
	lsn_before = j.next_lsn;

	check(d1_journal_append(&j, D1_REC_ENTRY, body_abc, sizeof(body_abc)),
	      "a second record appends");
	check(j.len > j.durable, "and is not yet claimed");
	check(j.next_lsn == lsn_before + 1u, "and took an LSN");

	d1_journal_rollback(&j);
	check(j.len == durable_before, "the rollback discards its bytes");
	check(j.durable == durable_before,
	      "without moving what was already claimed");
	check(j.next_lsn == lsn_before, "and gives the LSN back");
	check(drain(&j, j.durable, &n) == D1_JOURNAL_CLEAN_END && n == 1,
	      "so the claimed prefix still reads exactly one record");

	/* The next record takes the LSN the rolled-back one had. */
	check(d1_journal_append(&j, D1_REC_CONTROL, NULL, 0),
	      "a later record appends");
	check(d1_journal_flush(&j), "and is claimed");
	check(drain(&j, j.durable, &n) == D1_JOURNAL_CLEAN_END && n == 2,
	      "leaving a log with no gap in it");
	d1_journal_fini(&j);
}

/* A record larger than the model allows is never written. */
static void test_oversized_record(void)
{
	struct d1_journal j;

	if (!d1_journal_init(&j, &the_uuid))
		return;
	d1_journal_set_incarnation(&j, 1);
	check(!d1_journal_append(&j, D1_REC_START, body_abc,
				 D1_JOURNAL_RECORD_MAX),
	      "a body that would exceed the record limit is refused");
	check(j.len == 0, "and nothing was written");
	check(!d1_journal_append(&j, 99u, body_abc, sizeof(body_abc)),
	      "a record type outside the table is refused");
	d1_journal_fini(&j);
}

int main(void)
{
	fill_uuid(&the_uuid, 0x00);

	test_golden_record();
	test_round_trip();
	test_unflushed_is_not_read();
	test_durable_corruption_fails_closed();
	test_identity_and_order();
	test_faults_leave_no_residue();
	test_rollback_discards_only_the_unclaimed();
	test_a_record_of_another_format();
	test_oversized_record();

	if (failures) {
		fprintf(stderr, "%u check(s) failed\n", failures);
		return 1;
	}
	printf("d1_journal_test: all checks passed\n");
	return 0;
}
