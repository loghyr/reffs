/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * D1 storage model: journalled control requests and canonical results.
 *
 * Recovery compares what it computed against what was logged, so a
 * result has to have a form of its own.  These cases fix that form
 * with golden bytes, and check that the comparison actually
 * distinguishes results rather than agreeing with everything.
 */

#include <stdio.h>
#include <string.h>

#include "d1_control.h"

static unsigned int failures;

static void check(bool ok, const char *what)
{
	if (!ok) {
		failures++;
		fprintf(stderr, "FAIL: %s\n", what);
	}
}

static void fill_uuid(struct d1_uuid *u, uint8_t base)
{
	unsigned int i;

	for (i = 0; i < D1_UUID_BYTES; i++)
		u->bytes[i] = (uint8_t)(base + i);
}

static void make_result(struct d1_complete_result *r)
{
	memset(r, 0, sizeof(*r));
	fill_uuid(&r->key.origin, 0x50);
	r->key.sequence = 5;
	r->index_epoch = 3;
	r->eof = 4096;
	r->disposition = D1_COMPLETED;
	r->entry.status = D1_OK;
	r->entry.version_present = true;
	r->entry.version.raw = 7;
	r->entry.txn_present = true;
	r->entry.txn.raw = 9;
	r->entry.guard.generation = 2;
	r->entry.guard.writer = 11;
	r->entry.owner.cohort.raw = 42;
	r->entry.owner.writer = 11;
	r->entry.owner.co_id = 1;
	r->entry.stability = D1_FILE_SYNC;
	r->entry.activated = true;
	r->entry.phase = D1_PHASE_COMMITTED;
	r->entry.verifier[7] = 1;
}

/*
 * Where the member-transaction count sits in the golden result above:
 * operation key 28, index epoch 8, EOF 8, disposition 4, status 4, the
 * version and transaction options 9 each, and the absent cohort option
 * 1.  Written out rather than searched for, because a byte pattern as
 * short as a small count occurs in the fields before it.
 */
#define COUNT_AT 71u

/* The complete result, field by field. */
static void test_golden_complete_result(void)
{
	static const uint8_t want[] = {
		/* operation key: origin UUID */
		0x50,
		0x51,
		0x52,
		0x53,
		0x54,
		0x55,
		0x56,
		0x57,
		0x58,
		0x59,
		0x5a,
		0x5b,
		0x5c,
		0x5d,
		0x5e,
		0x5f,
		/* sequence 5 */
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x05,
		/* compound ordinal 0 */
		0x00,
		0x00,
		0x00,
		0x00,
		/* index epoch 3 */
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x03,
		/* EOF 4096 */
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x10,
		0x00,
		/* disposition COMPLETED */
		0x00,
		0x00,
		0x00,
		0x01,
		/* status OK */
		0x00,
		0x00,
		0x00,
		0x01,
		/* version present, 7 */
		0x01,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x07,
		/* transaction present, 9 */
		0x01,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x09,
		/* no repair cohort: this result is not a repair's */
		0x00,
		/* no member transactions: this result is not a begin's */
		0x00,
		0x00,
		0x00,
		0x00,
		/* no postcondition: this result is not a refused rollback's */
		0x00,
		/* no episode: this result is not a mark_error's */
		0x00,
		/* no member: this answer is not one member's */
		0x00,
		/* guard: generation 2, writer 11, written */
		0x00,
		0x00,
		0x00,
		0x02,
		0x00,
		0x00,
		0x00,
		0x0b,
		0x00,
		/* owner: cohort 42, writer 11, co_id 1 */
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x2a,
		0x00,
		0x00,
		0x00,
		0x0b,
		0x00,
		0x00,
		0x00,
		0x01,
		/* actual stability FILE_SYNC */
		0x00,
		0x00,
		0x00,
		0x03,
		/* activated true */
		0x01,
		/* phase COMMITTED */
		0x00,
		0x00,
		0x00,
		0x04,
		/* verifier for incarnation 1 */
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x01,
	};
	struct d1_complete_result r, back;
	uint8_t buf[512];
	size_t len;
	uint32_t i;

	make_result(&r);
	len = d1_complete_result_encode(&r, buf, sizeof(buf));
	check(len == sizeof(want), "the result is the expected length");
	check(len == sizeof(want) && memcmp(buf, want, sizeof(want)) == 0,
	      "and is byte for byte the golden result");
	check(d1_complete_result_decode(buf, len, &back),
	      "the golden bytes decode");
	check(d1_complete_result_equal(&r, &back), "and round trip");

	/*
	 * And the same result with a repair cohort in it.  The option is
	 * one byte longer than its absence plus the handle, at the offset
	 * the absent tag occupied, so the golden bytes above fix where it
	 * goes and these fix what goes there.
	 */
	make_result(&r);
	r.entry.member_txn_count = 2;
	r.entry.member_txn[0].raw = 0x0102030405060708ull;
	r.entry.member_txn[1].raw = 0x1112131415161718ull;
	len = d1_complete_result_encode(&r, buf, sizeof(buf));
	check(len == sizeof(want) + 16u,
	      "a result naming two member transactions is sixteen bytes "
	      "longer");
	if (len == sizeof(want) + 16u) {
		static const uint8_t named[20] = {
			0x00, 0x00, 0x00, 0x02, 0x01, 0x02, 0x03,
			0x04, 0x05, 0x06, 0x07, 0x08, 0x11, 0x12,
			0x13, 0x14, 0x15, 0x16, 0x17, 0x18
		};
		size_t at = 0;

		/* Immediately after the cohort option. */
		while (at + sizeof(named) <= len &&
		       memcmp(buf + at, named, sizeof(named)) != 0)
			at++;
		check(at + sizeof(named) <= len,
		      "and carries the count and the handles, big-endian");
	}
	check(d1_complete_result_decode(buf, len, &back) &&
		      d1_complete_result_equal(&r, &back),
	      "and that round trips too");
	r.entry.member_txn[1].raw = 0x1112131415161719ull;
	check(!d1_complete_result_equal(&r, &back),
	      "and the comparison notices which transactions they were");

	/*
	 * Section 8 has the decoder reject over-limit counts.  A count no
	 * cohort can have is a record this store could not have written,
	 * and believing it would read past the vector it declares.
	 */
	make_result(&r);
	r.entry.member_txn_count = 1;
	r.entry.member_txn[0].raw = 1;
	len = d1_complete_result_encode(&r, buf, sizeof(buf));
	check(len != 0 && d1_complete_result_decode(buf, len, &back),
	      "a result with one member transaction decodes");
	check(len > COUNT_AT + 4u && buf[COUNT_AT] == 0 &&
		      buf[COUNT_AT + 1u] == 0 && buf[COUNT_AT + 2u] == 0 &&
		      buf[COUNT_AT + 3u] == 1,
	      "and its count is the u32 after the cohort option");
	if (len > COUNT_AT + 4u) {
		buf[COUNT_AT + 3u] = (uint8_t)(D1_BATCH_ENTRIES_MAX + 1u);
		check(!d1_complete_result_decode(buf, len, &back),
		      "and a count past the batch limit is refused");
	}

	/*
	 * And the same at the limit, where the bytes are actually there.
	 * A full vector followed by a count one larger is the shape that
	 * reads past the array rather than running out of buffer first,
	 * so it is the one that says the count is checked against the
	 * limit and not merely against what is left to decode.
	 */
	make_result(&r);
	r.entry.member_txn_count = D1_BATCH_ENTRIES_MAX;
	for (i = 0; i < D1_BATCH_ENTRIES_MAX; i++)
		r.entry.member_txn[i].raw = 0x2000u + i;
	len = d1_complete_result_encode(&r, buf, sizeof(buf));
	check(len == sizeof(want) + 8u * D1_BATCH_ENTRIES_MAX,
	      "a full vector is eight bytes a member longer");
	check(len != 0 && d1_complete_result_decode(buf, len, &back) &&
		      d1_complete_result_equal(&r, &back),
	      "and round trips");
	check(len > COUNT_AT + 4u &&
		      buf[COUNT_AT + 3u] == (uint8_t)D1_BATCH_ENTRIES_MAX,
	      "with the count in the same place");
	if (len > COUNT_AT + 4u) {
		buf[COUNT_AT + 3u] = (uint8_t)(D1_BATCH_ENTRIES_MAX + 1u);
		check(!d1_complete_result_decode(buf, len, &back),
		      "and one more member than there can be is refused");
	}

	make_result(&r);
	r.entry.cohort_present = true;
	r.entry.cohort.raw = 0x1122334455667788ull;
	len = d1_complete_result_encode(&r, buf, sizeof(buf));
	check(len == sizeof(want) + 8u,
	      "a result naming a repair cohort is eight bytes longer");
	if (len == sizeof(want) + 8u) {
		static const uint8_t named[9] = { 0x01, 0x11, 0x22, 0x33, 0x44,
						  0x55, 0x66, 0x77, 0x88 };
		size_t at = 0;

		/* Immediately after the transaction option. */
		while (at + sizeof(named) <= len &&
		       memcmp(buf + at, named, sizeof(named)) != 0)
			at++;
		check(at + sizeof(named) <= len,
		      "and carries the tag and the handle, big-endian");
	}
	/* And the same for the postcondition a refused rollback returns. */
	make_result(&r);
	r.entry.postcond_present = true;
	r.entry.postcond.raw = 0x99aabbccddeeff00ull;
	len = d1_complete_result_encode(&r, buf, sizeof(buf));
	check(len == sizeof(want) + 8u,
	      "a result naming a postcondition is eight bytes longer");
	if (len == sizeof(want) + 8u) {
		static const uint8_t named[9] = { 0x01, 0x99, 0xaa, 0xbb, 0xcc,
						  0xdd, 0xee, 0xff, 0x00 };
		size_t at = 0;

		/* Immediately after the cohort option. */
		while (at + sizeof(named) <= len &&
		       memcmp(buf + at, named, sizeof(named)) != 0)
			at++;
		check(at + sizeof(named) <= len,
		      "and carries the tag and the handle, big-endian");
	}
	check(d1_complete_result_decode(buf, len, &back) &&
		      d1_complete_result_equal(&r, &back),
	      "and that round trips too");

	/* And the member a whole-vector refusal names, which is a u32. */
	make_result(&r);
	r.entry.member_present = true;
	r.entry.member = 0x01020304u;
	len = d1_complete_result_encode(&r, buf, sizeof(buf));
	check(len == sizeof(want) + 4u,
	      "a result naming a member is four bytes longer");
	if (len == sizeof(want) + 4u) {
		static const uint8_t named[5] = { 0x01, 0x01, 0x02, 0x03,
						  0x04 };
		size_t at = 0;

		/* Immediately after the episode option. */
		while (at + sizeof(named) <= len &&
		       memcmp(buf + at, named, sizeof(named)) != 0)
			at++;
		check(at + sizeof(named) <= len,
		      "and carries the tag and the ordinal, big-endian");
	}
	check(d1_complete_result_decode(buf, len, &back) &&
		      d1_complete_result_equal(&r, &back),
	      "and that round trips too");

	make_result(&r);
	r.entry.cohort_present = true;
	r.entry.cohort.raw = 0x1122334455667788ull;
	len = d1_complete_result_encode(&r, buf, sizeof(buf));
	check(d1_complete_result_decode(buf, len, &back) &&
		      d1_complete_result_equal(&r, &back),
	      "and it round trips");
	r.entry.cohort.raw = 0x1122334455667789ull;
	check(!d1_complete_result_equal(&r, &back),
	      "and the comparison notices which cohort it was");
}

/*
 * The comparison has to notice.  Each field is changed on its own and
 * must make the two results differ; a comparison that agreed with
 * everything would make recovery's check worthless.
 */
/*
 * Golden bytes for the certificate control record.
 *
 * The cross-DS completion certificate clear_error requires is issued
 * outside D1 entirely, so a fixture stands in for its issuer and this
 * record is what that issuance looks like on the wire.  It names no
 * object -- the zero key is part of what a reader checks -- and its
 * width is frozen with the rest.
 */
static void test_golden_certificate_request(void)
{
	static const uint8_t want[] = {
		/* control kind: certificate */
		0x00,
		0x00,
		0x00,
		0x07,
		/* object key: the zero key, because a certificate names none */
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		/* the certificate is present */
		0x01,
		/* and is its thirty-two bytes */
		0x5a,
		0x5a,
		0x5a,
		0x5a,
		0x5a,
		0x5a,
		0x5a,
		0x5a,
		0x5a,
		0x5a,
		0x5a,
		0x5a,
		0x5a,
		0x5a,
		0x5a,
		0x5a,
		0x5a,
		0x5a,
		0x5a,
		0x5a,
		0x5a,
		0x5a,
		0x5a,
		0x5a,
		0x5a,
		0x5a,
		0x5a,
		0x5a,
		0x5a,
		0x5a,
		0x5a,
		0x5a,
	};
	struct d1_control_request r;
	uint8_t buf[256];
	size_t len;

	memset(&r, 0, sizeof(r));
	r.kind = D1_CTL_CERTIFICATE;
	r.certificate_present = true;
	memset(r.certificate, 0x5a, sizeof(r.certificate));
	len = d1_control_request_encode(&r, buf, sizeof(buf));
	check(len == sizeof(want), "the certificate record is that long");
	check(len == sizeof(want) && memcmp(buf, want, sizeof(want)) == 0,
	      "and is byte for byte the golden record");
}

static void test_comparison_notices_every_field(void)
{
	struct d1_complete_result base, v;
	unsigned int i;

	make_result(&base);
	for (i = 0; i < 14; i++) {
		const char *what;

		v = base;
		switch (i) {
		case 0:
			v.key.sequence = 6;
			what = "sequence";
			break;
		case 1:
			v.key.ordinal = 1;
			what = "compound ordinal";
			break;
		case 2:
			v.index_epoch = 4;
			what = "index epoch";
			break;
		case 3:
			v.eof = 8192;
			what = "EOF";
			break;
		case 4:
			v.disposition = D1_UNRECORDED;
			what = "disposition";
			break;
		case 5:
			v.entry.status = D1_GUARDED;
			what = "status";
			break;
		case 6:
			v.entry.version.raw = 8;
			what = "version";
			break;
		case 7:
			v.entry.txn_present = false;
			what = "transaction presence";
			break;
		case 8:
			v.entry.guard.generation = 3;
			what = "guard generation";
			break;
		case 9:
			v.entry.owner.co_id = 2;
			what = "owner";
			break;
		case 10:
			v.entry.stability = D1_DATA_SYNC;
			what = "stability";
			break;
		case 11:
			v.entry.activated = false;
			what = "activation";
			break;
		case 12:
			v.entry.phase = D1_PHASE_PREPARED;
			what = "phase";
			break;
		default:
			v.entry.verifier[7] = 2;
			what = "verifier";
			break;
		}
		if (d1_complete_result_equal(&base, &v)) {
			failures++;
			fprintf(stderr, "FAIL: comparison ignores the %s\n",
				what);
		}
	}
	check(d1_complete_result_equal(&base, &base),
	      "and a result equals itself");
}

/* A fixture control request and its result round trip. */
static void test_control_round_trip(void)
{
	struct d1_control_request r, back;
	struct d1_control_result res, res_back;
	uint8_t buf[256];
	size_t len;

	memset(&r, 0, sizeof(r));
	r.kind = D1_CTL_ADMIT;
	fill_uuid(&r.object.export_uuid, 0x10);
	fill_uuid(&r.object.object_uuid, 0x20);
	fill_uuid(&r.auth.issuer, 0x30);
	fill_uuid(&r.auth.principal, 0x40);
	r.auth.writer = 11;
	r.auth.rights = D1_RIGHT_READ | D1_RIGHT_WRITE;
	r.auth.lease_epoch = 3;
	r.auth.authority_epoch = 4;
	r.auth.fence_sequence = 5;
	len = d1_control_request_encode(&r, buf, sizeof(buf));
	check(len != 0 && d1_control_request_decode(buf, len, &back),
	      "an admit request round trips");
	check(back.kind == r.kind && back.auth.writer == 11 &&
		      back.auth.rights == r.auth.rights &&
		      back.auth.lease_epoch == 3 &&
		      back.auth.authority_epoch == 4 &&
		      back.auth.fence_sequence == 5 &&
		      memcmp(&back.auth.principal, &r.auth.principal,
			     sizeof(r.auth.principal)) == 0,
	      "with its authority binding intact");

	memset(&r, 0, sizeof(r));
	r.kind = D1_CTL_RELEASE;
	r.version.raw = 12;
	len = d1_control_request_encode(&r, buf, sizeof(buf));
	check(len != 0 && d1_control_request_decode(buf, len, &back) &&
		      back.kind == D1_CTL_RELEASE && back.version.raw == 12,
	      "a release request round trips");

	memset(&res, 0, sizeof(res));
	res.status = D1_OK;
	res.id = 12;
	len = d1_control_result_encode(&res, buf, sizeof(buf));
	check(len != 0 && d1_control_result_decode(buf, len, &res_back) &&
		      res_back.status == D1_OK && res_back.id == 12,
	      "a control result round trips");

	/* The Envelope kind is carried as an Envelope, not as this. */
	memset(&r, 0, sizeof(r));
	r.kind = D1_CTL_ENVELOPE;
	check(d1_control_request_encode(&r, buf, sizeof(buf)) == 0,
	      "the envelope kind has no fixture encoding");
	check(!d1_control_request_decode(buf, 4, &back),
	      "and does not decode as one");
}

/* Trailing bytes and unknown tags are refused. */
static void test_control_refusals(void)
{
	struct d1_control_request r, back;
	struct d1_complete_result result, result_back;
	uint8_t buf[256];
	size_t len;

	memset(&r, 0, sizeof(r));
	r.kind = D1_CTL_EXPIRE;
	r.admission.raw = 3;
	len = d1_control_request_encode(&r, buf, sizeof(buf));
	check(len != 0, "a request encodes");
	check(!d1_control_request_decode(buf, len + 1u, &back),
	      "a trailing byte is a different request");
	buf[3] = 99;
	check(!d1_control_request_decode(buf, len, &back),
	      "an unknown control kind is refused");

	make_result(&result);
	len = d1_complete_result_encode(&result, buf, sizeof(buf));
	check(!d1_complete_result_decode(buf, len - 1u, &result_back),
	      "a truncated result is refused");
	check(d1_complete_result_encode(&result, buf, 4) == 0,
	      "and one with no room to encode is refused");
}

int main(void)
{
	test_golden_complete_result();
	test_golden_certificate_request();
	test_comparison_notices_every_field();
	test_control_round_trip();
	test_control_refusals();

	if (failures) {
		fprintf(stderr, "%u check(s) failed\n", failures);
		return 1;
	}
	printf("d1_control_test: all checks passed\n");
	return 0;
}
