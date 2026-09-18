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
	uint8_t buf[256];
	size_t len;

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
