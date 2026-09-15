/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * D1 storage model: journal record framing.
 *
 * What these cases are really asking is whether a reader that arrives
 * after a crash can tell where the writing stopped, using only the
 * bytes -- no separate note about how far the writer got.
 */

#include <stdio.h>
#include <string.h>

#include "d1_journal.h"

static unsigned int failures;

static void check(bool ok, const char *what)
{
	if (!ok) {
		failures++;
		fprintf(stderr, "FAIL: %s\n", what);
	}
}

static const uint8_t payload_abc[3] = { 0x61, 0x62, 0x63 };

/*
 * One record, byte for byte.  The CRC32C comes from a separate
 * implementation of the same reflected polynomial, which reproduces
 * that polynomial's published check value 0xe3069283 for "123456789".
 */
static void test_golden_record(void)
{
	static const uint8_t want[] = {
		/* magic "D1J1" */
		0x44,
		0x31,
		0x4a,
		0x31,
		/* record type START */
		0x00,
		0x00,
		0x00,
		0x01,
		/* sequence 1 */
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x01,
		/* payload length 3 */
		0x00,
		0x00,
		0x00,
		0x03,
		/* payload "abc" */
		0x61,
		0x62,
		0x63,
		/* CRC32C of everything above */
		0xa6,
		0xd7,
		0x61,
		0xb7,
	};
	struct d1_journal j;

	check(d1_journal_init(&j), "the journal initialises");
	check(d1_journal_append(&j, D1_REC_START, payload_abc,
				sizeof(payload_abc)),
	      "the record appends");
	check(j.len == sizeof(want), "the record is the expected length");
	check(j.len == sizeof(want) && memcmp(j.buf, want, sizeof(want)) == 0,
	      "and is byte for byte the golden record");
	d1_journal_fini(&j);
}

static void test_round_trip(void)
{
	struct d1_journal j;
	struct d1_journal_cursor c;
	const uint8_t *payload;
	uint32_t type, len;
	uint64_t seq;
	unsigned int n = 0;

	if (!d1_journal_init(&j))
		return;
	d1_journal_append(&j, D1_REC_START, payload_abc, sizeof(payload_abc));
	d1_journal_append(&j, D1_REC_ENTRY, payload_abc, sizeof(payload_abc));
	d1_journal_append(&j, D1_REC_CONTROL, NULL, 0);

	d1_journal_cursor_init(&c, j.buf, j.len);
	while (d1_journal_next(&c, &type, &seq, &payload, &len)) {
		n++;
		check(seq == n, "sequences run in order from one");
		if (n == 1)
			check(type == D1_REC_START && len == 3 &&
				      memcmp(payload, payload_abc, 3) == 0,
			      "the first record round trips");
		if (n == 3)
			check(type == D1_REC_CONTROL && len == 0,
			      "an empty payload is a payload");
	}
	check(n == 3, "every whole record is read back");
	d1_journal_fini(&j);
}

/* A crash in the middle of a write leaves a tail nobody should read. */
static void test_torn_tail(void)
{
	struct d1_journal j;
	struct d1_journal_cursor c;
	const uint8_t *payload;
	uint32_t type, len;
	uint64_t seq;
	size_t whole;
	unsigned int n;
	size_t cut;

	if (!d1_journal_init(&j))
		return;
	d1_journal_append(&j, D1_REC_START, payload_abc, sizeof(payload_abc));
	whole = j.len;
	d1_journal_append(&j, D1_REC_ENTRY, payload_abc, sizeof(payload_abc));

	/* Every cut inside the second record must read back exactly one. */
	for (cut = whole; cut < j.len; cut++) {
		struct d1_journal probe = j;

		probe.len = cut;
		d1_journal_cursor_init(&c, probe.buf, probe.len);
		n = 0;
		while (d1_journal_next(&c, &type, &seq, &payload, &len))
			n++;
		if (n != 1) {
			check(false, "a torn record is never read");
			break;
		}
	}
	check(cut == j.len, "no cut inside the last record yielded it");

	/* The whole log reads both again. */
	d1_journal_cursor_init(&c, j.buf, j.len);
	n = 0;
	while (d1_journal_next(&c, &type, &seq, &payload, &len))
		n++;
	check(n == 2, "an untruncated log reads both records");
	d1_journal_fini(&j);
}

/* Corruption anywhere in a record stops the read at that record. */
static void test_corruption_stops_the_read(void)
{
	size_t at;
	struct d1_journal probe;
	struct d1_journal_cursor c;
	const uint8_t *payload;
	uint32_t type, len;
	uint64_t seq;
	size_t first_len;
	bool all_stopped = true;

	if (!d1_journal_init(&probe))
		return;
	d1_journal_append(&probe, D1_REC_START, payload_abc,
			  sizeof(payload_abc));
	first_len = probe.len;
	d1_journal_append(&probe, D1_REC_ENTRY, payload_abc,
			  sizeof(payload_abc));

	for (at = first_len; at < probe.len; at++) {
		unsigned int n = 0;

		probe.buf[at] ^= 0xffu;
		d1_journal_cursor_init(&c, probe.buf, probe.len);
		while (d1_journal_next(&c, &type, &seq, &payload, &len))
			n++;
		probe.buf[at] ^= 0xffu;
		if (n != 1) {
			all_stopped = false;
			break;
		}
	}
	check(all_stopped,
	      "a flipped bit anywhere in the last record hides it");

	/* A flipped bit in the first record hides the second one too. */
	probe.buf[0] ^= 0xffu;
	d1_journal_cursor_init(&c, probe.buf, probe.len);
	check(!d1_journal_next(&c, &type, &seq, &payload, &len),
	      "and a corrupt first record ends the log there");
	probe.buf[0] ^= 0xffu;

	d1_journal_fini(&probe);
}

/* An armed fault refuses one append and leaves nothing behind. */
static void test_fault_leaves_no_residue(void)
{
	struct d1_journal j;
	struct d1_journal_cursor c;
	const uint8_t *payload;
	uint32_t type, len;
	uint64_t seq;
	size_t before;
	unsigned int n = 0;

	if (!d1_journal_init(&j))
		return;
	d1_journal_append(&j, D1_REC_START, payload_abc, sizeof(payload_abc));
	before = j.len;

	j.fail_next = true;
	check(!d1_journal_append(&j, D1_REC_ENTRY, payload_abc,
				 sizeof(payload_abc)),
	      "an armed fault refuses the append");
	check(j.len == before, "and the log is byte for byte unchanged");

	check(d1_journal_append(&j, D1_REC_ENTRY, payload_abc,
				sizeof(payload_abc)),
	      "the next append succeeds");
	d1_journal_cursor_init(&c, j.buf, j.len);
	while (d1_journal_next(&c, &type, &seq, &payload, &len))
		n++;
	check(n == 2, "and exactly the two written records are there");
	d1_journal_fini(&j);
}

/* A record claiming more than the model allows is not a record. */
static void test_oversized_claim(void)
{
	struct d1_journal j;
	struct d1_journal_cursor c;
	const uint8_t *payload;
	uint32_t type, len;
	uint64_t seq;

	if (!d1_journal_init(&j))
		return;
	d1_journal_append(&j, D1_REC_START, payload_abc, sizeof(payload_abc));
	/* Overwrite the declared length with something absurd. */
	j.buf[16] = 0xffu;
	j.buf[17] = 0xffu;
	j.buf[18] = 0xffu;
	j.buf[19] = 0xffu;
	d1_journal_cursor_init(&c, j.buf, j.len);
	check(!d1_journal_next(&c, &type, &seq, &payload, &len),
	      "a length past the model's limit is refused");

	check(!d1_journal_append(&j, D1_REC_ENTRY, payload_abc,
				 D1_JOURNAL_RECORD_MAX + 1u),
	      "and one that large is never written");
	d1_journal_fini(&j);
}

int main(void)
{
	test_golden_record();
	test_round_trip();
	test_torn_tail();
	test_corruption_stops_the_read();
	test_fault_leaves_no_residue();
	test_oversized_claim();

	if (failures) {
		fprintf(stderr, "%u check(s) failed\n", failures);
		return 1;
	}
	printf("d1_journal_test: all checks passed\n");
	return 0;
}
