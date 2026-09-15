/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * D1 storage model: the canonical envelope, its golden bytes and the
 * request digest it binds.
 *
 * The point of the digest is that two requests differing anywhere at all
 * differ in it.  The tests below change one field at a time and require
 * a different digest each time, rather than hashing one request and
 * declaring the property.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d1_digest.h"
#include "d1_envelope.h"

static unsigned int failures;

/* The digest scratch is the caller's; the model keeps none. */
static uint8_t *scratch;

static void check(bool ok, const char *what)
{
	if (!ok) {
		failures++;
		fprintf(stderr, "FAIL: %s\n", what);
	}
}

static const uint8_t payload[4] = { 'd', 'a', 't', 'a' };

static void fill_uuid(struct d1_uuid *u, uint8_t base)
{
	unsigned int i;

	for (i = 0; i < D1_UUID_BYTES; i++)
		u->bytes[i] = (uint8_t)(base + i);
}

/* A representative write envelope every case below starts from. */
static void make_write(struct d1_envelope *env)
{
	struct d1_write_entry *e;

	memset(env, 0, sizeof(*env));
	fill_uuid(&env->object.export_uuid, 0x00);
	fill_uuid(&env->object.object_uuid, 0x10);
	fill_uuid(&env->key.origin, 0x20);
	env->admission = 7;
	env->incarnation = 1;
	env->key.sequence = 5;
	env->key.ordinal = 0;
	env->op = D1_OP_WRITE_BATCH;
	env->body.write.count = 1;
	env->body.write.stability = D1_FILE_SYNC;
	env->body.write.activate = true;

	e = &env->body.write.entries[0];
	e->index = 0;
	e->owner.cohort = 42;
	e->owner.writer = 11;
	e->owner.co_id = 1;
	e->guard_check = true;
	e->expected.generation = 0;
	e->expected.writer = 0;
	e->expected.never_written = true;
	e->payload = payload;
	e->payload_len = sizeof(payload);
	d1_checksum_compute(D1_CKSUM_CRC32C, payload, sizeof(payload),
			    &e->checksum);
}

/* The golden bytes of that envelope, written out field by field. */
static void test_golden_write_envelope(void)
{
	/*
	 * The golden bytes, group by group.  The CRC32C of the payload
	 * is 0xaed87dd1, from a separate implementation of the same
	 * reflected polynomial that also reproduces the standard check
	 * value 0xe3069283 for "123456789".
	 */
	static const uint8_t want[] = {
		/* object key: export UUID then object UUID */
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
		0x10,
		0x11,
		0x12,
		0x13,
		0x14,
		0x15,
		0x16,
		0x17,
		0x18,
		0x19,
		0x1a,
		0x1b,
		0x1c,
		0x1d,
		0x1e,
		0x1f,
		/* admission handle 7 */
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x07,
		/* incarnation 1 */
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x01,
		/* operation key: origin UUID, sequence 5, ordinal 0 */
		0x20,
		0x21,
		0x22,
		0x23,
		0x24,
		0x25,
		0x26,
		0x27,
		0x28,
		0x29,
		0x2a,
		0x2b,
		0x2c,
		0x2d,
		0x2e,
		0x2f,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x05,
		0x00,
		0x00,
		0x00,
		0x00,
		/* operation tag: write_batch */
		0x00,
		0x00,
		0x00,
		0x01,
		/* entry count 1 */
		0x00,
		0x00,
		0x00,
		0x01,
		/* chunk index 0 */
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
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
		/* guard predicate present: generation 0, writer 0, never written */
		0x01,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x01,
		/* payload "data" */
		0x00,
		0x00,
		0x00,
		0x04,
		0x64,
		0x61,
		0x74,
		0x61,
		/* checksum: CRC32C tag, four bytes, 0xaed87dd1 */
		0x00,
		0x00,
		0x00,
		0x02,
		0x00,
		0x00,
		0x00,
		0x04,
		0xae,
		0xd8,
		0x7d,
		0xd1,
		/* requested stability FILE_SYNC */
		0x00,
		0x00,
		0x00,
		0x03,
		/* activation flag true */
		0x01,
	};
	struct d1_envelope env;
	uint8_t buf[512];
	size_t len;

	make_write(&env);
	len = d1_envelope_encode(&env, buf, sizeof(buf));
	check(len == sizeof(want), "golden write envelope length");
	if (len == sizeof(want) && memcmp(buf, want, len) != 0) {
		size_t i;

		failures++;
		fprintf(stderr, "FAIL: golden write envelope bytes\n  got ");
		for (i = 0; i < len; i++)
			fprintf(stderr, "%02x", buf[i]);
		fprintf(stderr, "\n");
	}
}

static void test_round_trip(void)
{
	struct d1_envelope env, back;
	uint8_t buf[512];
	size_t len;

	make_write(&env);
	len = d1_envelope_encode(&env, buf, sizeof(buf));
	check(len != 0, "write envelope encodes");
	check(d1_envelope_decode(buf, len, &back), "write envelope decodes");
	check(back.op == D1_OP_WRITE_BATCH && back.admission == 7 &&
		      back.incarnation == 1 && back.key.sequence == 5,
	      "envelope header survives");
	check(back.body.write.count == 1 &&
		      back.body.write.stability == D1_FILE_SYNC &&
		      back.body.write.activate,
	      "write batch survives");
	check(back.body.write.entries[0].payload_len == sizeof(payload) &&
		      memcmp(back.body.write.entries[0].payload, payload,
			     sizeof(payload)) == 0,
	      "payload survives");
	check(back.body.write.entries[0].guard_check &&
		      back.body.write.entries[0].expected.never_written,
	      "guard predicate survives");
}

/* Every field is inside the digest: change one, and it changes. */
static void test_digest_binds_every_field(void)
{
	struct d1_envelope env;
	uint8_t base[D1_DIGEST_BYTES];
	uint8_t other[D1_DIGEST_BYTES];
	unsigned int i;

	if (!scratch)
		return;
	make_write(&env);
	check(d1_envelope_digest(&env, scratch, D1_ENVELOPE_MAX, base),
	      "digest computes");

	for (i = 0; i < 10; i++) {
		struct d1_envelope v;
		const char *what;

		make_write(&v);
		switch (i) {
		case 0:
			v.admission++;
			what = "admission";
			break;
		case 1:
			v.incarnation++;
			what = "incarnation";
			break;
		case 2:
			v.key.sequence++;
			what = "sequence";
			break;
		case 3:
			v.key.ordinal++;
			what = "ordinal";
			break;
		case 4:
			v.body.write.entries[0].index++;
			what = "chunk index";
			break;
		case 5:
			v.body.write.entries[0].owner.co_id++;
			what = "owner";
			break;
		case 6:
			v.body.write.entries[0].expected.generation++;
			what = "expected guard";
			break;
		case 7:
			v.body.write.entries[0].checksum.digest[3] ^= 0xffu;
			what = "checksum";
			break;
		case 8:
			v.body.write.activate = false;
			what = "activation flag";
			break;
		default:
			v.body.write.stability = D1_UNSTABLE;
			what = "stability";
			break;
		}
		check(d1_envelope_digest(&v, scratch, D1_ENVELOPE_MAX, other),
		      "variant digest computes");
		if (memcmp(base, other, sizeof(base)) == 0) {
			failures++;
			fprintf(stderr, "FAIL: digest does not bind the %s\n",
				what);
		}
	}

	/* The same request twice is the same digest. */
	{
		struct d1_envelope again;

		make_write(&again);
		check(d1_envelope_digest(&again, scratch, D1_ENVELOPE_MAX,
					 other) &&
			      memcmp(base, other, sizeof(base)) == 0,
		      "the same request has the same digest");
	}
}

static void test_decoder_refusals(void)
{
	struct d1_envelope env, back;
	uint8_t buf[512];
	size_t len;

	make_write(&env);
	len = d1_envelope_encode(&env, buf, sizeof(buf));
	check(len != 0, "baseline encodes");

	/* A trailing byte is a different request, not this one. */
	buf[len] = 0;
	check(!d1_envelope_decode(buf, len + 1, &back),
	      "a trailing byte is refused");

	/* One byte short is not a short read, it is not this request. */
	check(!d1_envelope_decode(buf, len - 1, &back),
	      "a truncated envelope is refused");

	/* An operation tag this model does not carry decodes to nothing. */
	{
		uint8_t bad[512];

		memcpy(bad, buf, len);
		/* The op field follows objkey, admission, incarnation, opkey. */
		bad[32 + 8 + 8 + 28 + 3] = (uint8_t)D1_OP_CLEAR_ERROR;
		check(!d1_envelope_decode(bad, len, &back),
		      "an unrepresentable operation tag is refused");
	}

	/* A count outside the declared range is refused. */
	{
		uint8_t bad[512];
		size_t count_at = 32 + 8 + 8 + 28 + 4;

		memcpy(bad, buf, len);
		bad[count_at + 3] = 0;
		check(!d1_envelope_decode(bad, len, &back),
		      "a zero entry count is refused");
		bad[count_at + 3] = D1_BATCH_ENTRIES_MAX + 1;
		check(!d1_envelope_decode(bad, len, &back),
		      "an entry count past the limit is refused");
	}

	/* A stability value outside the three is refused. */
	{
		uint8_t bad[512];

		memcpy(bad, buf, len);
		bad[len - 5 + 3] = 4;
		check(!d1_envelope_decode(bad, len, &back),
		      "an unknown stability is refused");
	}

	/* An encode with no room yields nothing rather than a prefix. */
	{
		uint8_t small[8];

		check(d1_envelope_encode(&env, small, sizeof(small)) == 0,
		      "an envelope that does not fit encodes to nothing");
	}

	/* An operation this model cannot express never encodes. */
	env.op = D1_OP_BEGIN_REPAIR;
	check(d1_envelope_encode(&env, buf, sizeof(buf)) == 0,
	      "an unrepresentable operation never encodes");
}

static void test_lifecycle_and_control(void)
{
	struct d1_envelope env, back;
	uint8_t buf[512];
	size_t len;

	memset(&env, 0, sizeof(env));
	fill_uuid(&env.object.export_uuid, 0x40);
	fill_uuid(&env.object.object_uuid, 0x50);
	fill_uuid(&env.key.origin, 0x60);
	env.admission = 3;
	env.incarnation = 2;
	env.key.sequence = 9;
	env.op = D1_OP_COMMIT_BATCH;
	env.body.lifecycle.range_begin = 0;
	env.body.lifecycle.range_end = 2;
	env.body.lifecycle.count = 2;
	env.body.lifecycle.entries[0].index = 0;
	env.body.lifecycle.entries[0].owner.cohort = 5;
	env.body.lifecycle.entries[0].owner.writer = 11;
	env.body.lifecycle.entries[0].owner.co_id = 1;
	env.body.lifecycle.entries[0].txn = 11;
	env.body.lifecycle.entries[1].index = 1;
	env.body.lifecycle.entries[1].owner.cohort = 5;
	env.body.lifecycle.entries[1].owner.writer = 11;
	env.body.lifecycle.entries[1].owner.co_id = 2;
	env.body.lifecycle.entries[1].txn = 12;
	env.body.lifecycle.entries[1].predecessor_present = true;
	env.body.lifecycle.entries[1].predecessor = 4;
	memcpy(env.body.lifecycle.prior_verifier, "\0\0\0\0\0\0\0\2", 8);

	len = d1_envelope_encode(&env, buf, sizeof(buf));
	check(len != 0 && d1_envelope_decode(buf, len, &back),
	      "commit envelope round trips");
	check(back.body.lifecycle.count == 2 &&
		      back.body.lifecycle.range_end == 2 &&
		      back.body.lifecycle.entries[1].predecessor_present &&
		      back.body.lifecycle.entries[1].predecessor == 4,
	      "commit body survives");
	check(memcmp(back.body.lifecycle.prior_verifier,
		     env.body.lifecycle.prior_verifier, 8) == 0,
	      "prior verifier survives");

	memset(&env, 0, sizeof(env));
	env.op = D1_OP_RECOVERY_ADMIT;
	env.body.control.count = 1;
	env.body.control.txns[0] = 77;
	env.body.control.old_admission = 5;
	env.body.control.new_admission_present = true;
	env.body.control.new_admission = 6;
	env.body.control.read_epoch_present = true;
	env.body.control.read_epoch = 8;
	len = d1_envelope_encode(&env, buf, sizeof(buf));
	check(len != 0 && d1_envelope_decode(buf, len, &back),
	      "recovery_admit envelope round trips");
	check(back.body.control.txns[0] == 77 &&
		      back.body.control.new_admission == 6 &&
		      back.body.control.read_epoch == 8,
	      "control body survives");
}

int main(void)
{
	scratch = calloc(1, D1_ENVELOPE_MAX);
	if (!scratch) {
		fprintf(stderr, "FAIL: no scratch\n");
		return 1;
	}
	test_golden_write_envelope();
	test_round_trip();
	test_digest_binds_every_field();
	test_decoder_refusals();
	test_lifecycle_and_control();

	if (failures) {
		fprintf(stderr, "%u check(s) failed\n", failures);
		return 1;
	}
	free(scratch);
	printf("d1_envelope_test: all checks passed\n");
	return 0;
}
