/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

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
/* A second one, for the encoding of what the first one decoded. */
static uint8_t *again;

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
	env->admission.raw = 7;
	env->incarnation = 1;
	env->key.sequence = 5;
	env->key.ordinal = 0;
	env->op = D1_OP_WRITE_BATCH;
	env->body.write.count = 1;
	env->body.write.stability = D1_FILE_SYNC;
	env->body.write.activate = true;

	e = &env->body.write.entries[0];
	e->index = 0;
	e->owner.cohort.raw = 42;
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
	check(back.op == D1_OP_WRITE_BATCH && back.admission.raw == 7 &&
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

/*
 * Nothing the reducer reads is lost between encode and decode.
 *
 * The store executes the request it decodes out of its own canonical
 * copy, so a field the encoder writes and the decoder misplaces is not
 * a codec curiosity: it is a request executed differently from the one
 * that was validated, under a digest that binds the bytes and not the
 * difference.  The round-trip test beside this one checks a write
 * envelope's header, counts and payload; this checks every field of
 * every body the reducer reads, each set to a value nothing else uses.
 */
static bool owner_same(const struct d1_owner *a, const struct d1_owner *b)
{
	return a->cohort.raw == b->cohort.raw && a->writer == b->writer &&
	       a->co_id == b->co_id;
}

static bool header_same(const struct d1_envelope *a,
			const struct d1_envelope *b)
{
	return memcmp(&a->object, &b->object, sizeof(a->object)) == 0 &&
	       a->admission.raw == b->admission.raw &&
	       a->incarnation == b->incarnation &&
	       memcmp(&a->key.origin, &b->key.origin, sizeof(a->key.origin)) ==
		       0 &&
	       a->key.sequence == b->key.sequence &&
	       a->key.ordinal == b->key.ordinal && a->op == b->op;
}

/*
 * Encode, decode, and encode again -- and require the same bytes.
 *
 * Field equality is not the whole property the store rests on.  Replay
 * decodes a record's Envelope bytes, encodes what it decoded, and
 * digests that; the digest it compares against was taken by the writer
 * over the record's own bytes.  So the two encodings must be the same
 * bytes and not merely the same request, or a store would refuse the
 * log it wrote itself.
 */
static bool round_trip(const struct d1_envelope *env, struct d1_envelope *back)
{
	size_t len, again_len;

	if (!scratch || !again)
		return false;
	len = d1_envelope_encode(env, scratch, D1_ENVELOPE_MAX);
	if (!len)
		return false;
	if (!d1_envelope_decode(scratch, len, back))
		return false;
	again_len = d1_envelope_encode(back, again, D1_ENVELOPE_MAX);
	return again_len == len && memcmp(again, scratch, len) == 0;
}

static void test_the_round_trip_keeps_every_field(void)
{
	struct d1_envelope env, back;
	struct d1_write_entry *w;
	struct d1_lifecycle_entry *l;
	struct d1_rollback_entry *r;
	struct d1_control_batch *k;
	unsigned int i;

	/* A write, with every field distinctive. */
	make_write(&env);
	env.incarnation = 0x0102030405060708ull;
	env.key.sequence = 0x1122334455667788ull;
	env.key.ordinal = 3;
	env.body.write.count = 2;
	env.body.write.stability = D1_DATA_SYNC;
	env.body.write.activate = false;
	w = &env.body.write.entries[0];
	w->index = 5;
	w->owner.cohort.raw = 0x9988776655443322ull;
	w->owner.writer = 0x1234u;
	w->owner.co_id = 0x5678u;
	w->guard_check = true;
	w->expected.never_written = false;
	w->expected.generation = 0x4321u;
	w->expected.writer = 0x8765u;
	env.body.write.entries[1] = *w;
	env.body.write.entries[1].index = 6;
	env.body.write.entries[1].owner.co_id = 0x5679u;
	check(round_trip(&env, &back), "a write envelope round-trips");
	check(header_same(&env, &back), "and its header is unchanged");
	check(back.body.write.count == 2 &&
		      back.body.write.stability == D1_DATA_SYNC &&
		      !back.body.write.activate,
	      "with its count, stability and activation");
	for (i = 0; i < 2; i++) {
		const struct d1_write_entry *a = &env.body.write.entries[i];
		const struct d1_write_entry *b = &back.body.write.entries[i];

		check(a->index == b->index && owner_same(&a->owner, &b->owner),
		      "each entry keeps its index and owner");
		check(a->guard_check == b->guard_check &&
			      a->expected.never_written ==
				      b->expected.never_written &&
			      a->expected.generation ==
				      b->expected.generation &&
			      a->expected.writer == b->expected.writer,
		      "and its guard predicate exactly");
		check(a->payload_len == b->payload_len &&
			      memcmp(a->payload, b->payload, a->payload_len) ==
				      0,
		      "and its payload bytes");
		check(memcmp(&a->checksum, &b->checksum, sizeof(a->checksum)) ==
			      0,
		      "and its checksum");
	}

	/* A lifecycle batch, including the option a member may omit. */
	make_write(&env);
	env.op = D1_OP_COMMIT_BATCH;
	memset(&env.body, 0, sizeof(env.body));
	env.body.lifecycle.range_begin = 4;
	env.body.lifecycle.range_end = 9;
	env.body.lifecycle.count = 2;
	for (i = 0; i < D1_VERIFIER_BYTES; i++)
		env.body.lifecycle.prior_verifier[i] = (uint8_t)(0xa0 + i);
	l = &env.body.lifecycle.entries[0];
	l->index = 4;
	l->owner.cohort.raw = 11;
	l->owner.writer = 22;
	l->owner.co_id = 33;
	l->txn.raw = 0xfeedfaceull;
	l->predecessor_present = true;
	l->predecessor.raw = 0xdeadbeefull;
	env.body.lifecycle.entries[1] = *l;
	env.body.lifecycle.entries[1].index = 5;
	env.body.lifecycle.entries[1].txn.raw = 0xfeedfacfull;
	env.body.lifecycle.entries[1].predecessor_present = false;
	env.body.lifecycle.entries[1].predecessor.raw = 0;
	check(round_trip(&env, &back), "a lifecycle envelope round-trips");
	check(header_same(&env, &back), "and its header is unchanged");
	check(back.body.lifecycle.range_begin == 4 &&
		      back.body.lifecycle.range_end == 9 &&
		      back.body.lifecycle.count == 2 &&
		      memcmp(back.body.lifecycle.prior_verifier,
			     env.body.lifecycle.prior_verifier,
			     D1_VERIFIER_BYTES) == 0,
	      "with its range, count and prior verifier");
	for (i = 0; i < 2; i++) {
		const struct d1_lifecycle_entry *a =
			&env.body.lifecycle.entries[i];
		const struct d1_lifecycle_entry *b =
			&back.body.lifecycle.entries[i];

		check(a->index == b->index &&
			      owner_same(&a->owner, &b->owner) &&
			      a->txn.raw == b->txn.raw,
		      "each entry keeps its index, owner and transaction");
		check(a->predecessor_present == b->predecessor_present &&
			      a->predecessor.raw == b->predecessor.raw,
		      "and its predecessor option exactly as it was");
	}

	/* A rollback batch, with all three of its options set and clear. */
	make_write(&env);
	env.op = D1_OP_ROLLBACK_BATCH;
	memset(&env.body, 0, sizeof(env.body));
	env.body.rollback.range_begin = 2;
	env.body.rollback.range_end = 8;
	env.body.rollback.count = 2;
	r = &env.body.rollback.entries[0];
	r->index = 2;
	r->owner.cohort.raw = 44;
	r->owner.writer = 55;
	r->owner.co_id = 66;
	r->txn.raw = 0x1111ull;
	r->visible_present = true;
	r->visible.raw = 0x2222ull;
	r->predecessor_present = true;
	r->predecessor.raw = 0x3333ull;
	r->custody_present = true;
	r->custody.raw = 0x4444ull;
	env.body.rollback.entries[1] = *r;
	env.body.rollback.entries[1].index = 3;
	env.body.rollback.entries[1].txn.raw = 0x1112ull;
	env.body.rollback.entries[1].visible_present = false;
	env.body.rollback.entries[1].visible.raw = 0;
	env.body.rollback.entries[1].predecessor_present = false;
	env.body.rollback.entries[1].predecessor.raw = 0;
	env.body.rollback.entries[1].custody_present = false;
	env.body.rollback.entries[1].custody.raw = 0;
	check(round_trip(&env, &back), "a rollback envelope round-trips");
	check(back.body.rollback.range_begin == 2 &&
		      back.body.rollback.range_end == 8 &&
		      back.body.rollback.count == 2,
	      "with its range and count");
	for (i = 0; i < 2; i++) {
		const struct d1_rollback_entry *a =
			&env.body.rollback.entries[i];
		const struct d1_rollback_entry *b =
			&back.body.rollback.entries[i];

		check(a->index == b->index &&
			      owner_same(&a->owner, &b->owner) &&
			      a->txn.raw == b->txn.raw,
		      "each entry keeps its index, owner and transaction");
		check(a->visible_present == b->visible_present &&
			      a->visible.raw == b->visible.raw &&
			      a->predecessor_present ==
				      b->predecessor_present &&
			      a->predecessor.raw == b->predecessor.raw &&
			      a->custody_present == b->custody_present &&
			      a->custody.raw == b->custody.raw,
		      "and all three of its options exactly as they were");
	}

	/* A control operation, with its two optional fields present. */
	make_write(&env);
	env.op = D1_OP_RECOVERY_ADMIT;
	memset(&env.body, 0, sizeof(env.body));
	k = &env.body.control;
	k->count = 2;
	k->txns[0].raw = 0xaaaaull;
	k->txns[1].raw = 0xbbbbull;
	k->old_admission.raw = 0xccccull;
	k->new_admission_present = true;
	k->new_admission.raw = 0xddddull;
	k->read_epoch_present = true;
	k->read_epoch = 0x123456789aull;
	check(round_trip(&env, &back), "a control envelope round-trips");
	check(header_same(&env, &back), "and its header is unchanged");
	check(back.body.control.count == 2 &&
		      back.body.control.txns[0].raw == 0xaaaaull &&
		      back.body.control.txns[1].raw == 0xbbbbull,
	      "with every transaction it named");
	check(back.body.control.old_admission.raw == 0xccccull &&
		      back.body.control.new_admission_present &&
		      back.body.control.new_admission.raw == 0xddddull,
	      "and both admissions");
	check(back.body.control.read_epoch_present &&
		      back.body.control.read_epoch == 0x123456789aull,
	      "and the read epoch it granted");
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
			v.admission.raw++;
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

	/*
	 * An operation this model cannot express never encodes.  The tag
	 * is one no enum value uses, because every operation the enum
	 * names now has a body.
	 */
	env.op = 0xd1d1d1d1u;
	check(d1_envelope_encode(&env, buf, sizeof(buf)) == 0,
	      "an unrepresentable operation never encodes");
}

/*
 * A repair envelope for @op, with every field the operation uses set to
 * a value nothing else uses.
 */
static void make_repair(struct d1_envelope *env, uint32_t op)
{
	struct d1_repair_entry *e;
	bool state = op == D1_OP_MARK_ERROR || op == D1_OP_BEGIN_REPAIR;
	unsigned int i;

	memset(env, 0, sizeof(*env));
	fill_uuid(&env->object.export_uuid, 0x60);
	fill_uuid(&env->object.object_uuid, 0x70);
	fill_uuid(&env->key.origin, 0x80);
	env->admission.raw = 0x1122334455667788ull;
	env->incarnation = 0x0102030405060708ull;
	env->key.sequence = 0x99aabbccddeeff00ull;
	env->key.ordinal = 5;
	env->op = op;
	env->body.repair.range_begin = 4;
	env->body.repair.range_end = 6;
	env->body.repair.count = 2;
	for (i = 0; i < 2u; i++) {
		e = &env->body.repair.entries[i];
		e->index = 4u + i;
		e->mode = op != D1_OP_BEGIN_REPAIR ? 0u :
			  i == 0		   ? D1_REPAIR_ERROR :
						     D1_REPAIR_NOPRE;
		e->owner.cohort.raw = 0x2200u + i;
		e->owner.writer = 0x3300u + i;
		e->owner.co_id = 0x4400u + i;
		e->custody_present = state;
		e->custody.raw = state ? 0x5500u + i : 0u;
		e->successor_present = state;
		e->successor.raw = state ? 0x6600u + i : 0u;
		/* The one genuinely optional field: NOPRE has none. */
		e->predecessor_present = state && i == 0;
		e->predecessor.raw = e->predecessor_present ? 0x7700u : 0u;
		e->payload_present = op == D1_OP_PREPARE_REPAIR;
		if (e->payload_present) {
			e->payload = payload;
			e->payload_len = (uint32_t)sizeof(payload);
			d1_checksum_compute(D1_CKSUM_CRC32C, payload,
					    sizeof(payload), &e->checksum);
		}
	}
	env->body.repair.cohort_present = op != D1_OP_MARK_ERROR &&
					  op != D1_OP_BEGIN_REPAIR;
	env->body.repair.cohort.raw =
		env->body.repair.cohort_present ? 0x8800u : 0u;
	env->body.repair.certificate_present = op == D1_OP_CLEAR_ERROR;
	if (env->body.repair.certificate_present)
		memset(env->body.repair.certificate, 0x9a,
		       D1_CERTIFICATE_BYTES);
}

static bool repair_entry_same(const struct d1_repair_entry *a,
			      const struct d1_repair_entry *b)
{
	if (a->index != b->index || a->mode != b->mode ||
	    !owner_same(&a->owner, &b->owner))
		return false;
	if (a->custody_present != b->custody_present ||
	    a->custody.raw != b->custody.raw)
		return false;
	if (a->successor_present != b->successor_present ||
	    a->successor.raw != b->successor.raw)
		return false;
	if (a->predecessor_present != b->predecessor_present ||
	    a->predecessor.raw != b->predecessor.raw)
		return false;
	if (a->payload_present != b->payload_present)
		return false;
	if (!a->payload_present)
		return true;
	return a->payload_len == b->payload_len &&
	       memcmp(a->payload, b->payload, a->payload_len) == 0 &&
	       a->checksum.alg == b->checksum.alg &&
	       a->checksum.len == b->checksum.len &&
	       memcmp(a->checksum.digest, b->checksum.digest,
		      a->checksum.len) == 0;
}

/*
 * Every repair operation round-trips, field for field and byte for
 * byte.
 *
 * The eight of them share one vector shape and differ only in which of
 * its options they require, so the round trip is the place that shows
 * each one carries what it claims and nothing else survives that it
 * should not.
 */
static void test_every_repair_operation_round_trips(void)
{
	static const uint32_t ops[] = {
		D1_OP_MARK_ERROR,     D1_OP_BEGIN_REPAIR,
		D1_OP_PREPARE_REPAIR, D1_OP_FINALIZE_REPAIR,
		D1_OP_COMMIT_REPAIR,  D1_OP_ABORT_REPAIR,
		D1_OP_CLEAR_ERROR,    D1_OP_UNLOCK,
	};
	struct d1_envelope env, back;
	unsigned int i, j;

	for (i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
		const struct d1_repair_batch *a, *b;

		make_repair(&env, ops[i]);
		check(round_trip(&env, &back), "a repair envelope round-trips");
		check(header_same(&env, &back), "its header survives");
		a = &env.body.repair;
		b = &back.body.repair;
		check(a->range_begin == b->range_begin &&
			      a->range_end == b->range_end &&
			      a->count == b->count,
		      "and its range and count");
		check(a->cohort_present == b->cohort_present &&
			      a->cohort.raw == b->cohort.raw,
		      "and the cohort it names, or does not");
		check(a->certificate_present == b->certificate_present &&
			      (!a->certificate_present ||
			       memcmp(a->certificate, b->certificate,
				      D1_CERTIFICATE_BYTES) == 0),
		      "and the certificate it carries, or does not");
		for (j = 0; j < a->count; j++)
			check(repair_entry_same(&a->entries[j], &b->entries[j]),
			      "and every field of every member");
		/* The decoded handles carry no domain and no issuer. */
		check(b->cohort._kind == D1_HANDLE_NONE &&
			      b->cohort._instance == 0,
		      "and the cohort it decodes to names nothing yet");
	}
}

/* One row: an option carried by an operation that has no use for it. */
struct repair_misfit {
	const char *what;
	uint32_t op;
	/* Which option to add to, or remove from, a correct request. */
	unsigned int field;
};

/*
 * An option an operation does not use is a different request.
 *
 * The eight operations share one vector, so the only thing separating
 * them is which options each requires.  If an unused one were ignored
 * rather than refused, two requests that mean the same thing would both
 * exist -- and the digest binds them differently, so a store would have
 * two identities for one intent.
 */
static void test_a_repair_option_belongs_to_its_operation(void)
{
	struct d1_envelope env;
	uint8_t buf[512];

	/* A cohort where one is opened, and none where one is named. */
	make_repair(&env, D1_OP_BEGIN_REPAIR);
	env.body.repair.cohort_present = true;
	env.body.repair.cohort.raw = 9;
	check(d1_envelope_encode(&env, buf, sizeof(buf)) == 0,
	      "begin_repair names no cohort");
	make_repair(&env, D1_OP_COMMIT_REPAIR);
	env.body.repair.cohort_present = false;
	check(d1_envelope_encode(&env, buf, sizeof(buf)) == 0,
	      "and commit_repair names one");

	/* A mode where the cohort already has one. */
	make_repair(&env, D1_OP_FINALIZE_REPAIR);
	env.body.repair.entries[0].mode = D1_REPAIR_NOPRE;
	check(d1_envelope_encode(&env, buf, sizeof(buf)) == 0,
	      "only begin_repair tags a member's mode");
	make_repair(&env, D1_OP_BEGIN_REPAIR);
	env.body.repair.entries[0].mode = 0;
	check(d1_envelope_encode(&env, buf, sizeof(buf)) == 0,
	      "and it must tag every member");
	make_repair(&env, D1_OP_BEGIN_REPAIR);
	env.body.repair.entries[0].mode = 3;
	check(d1_envelope_encode(&env, buf, sizeof(buf)) == 0,
	      "with a mode this model has");

	/* The captured state, and the replacement. */
	make_repair(&env, D1_OP_UNLOCK);
	env.body.repair.entries[1].custody_present = true;
	env.body.repair.entries[1].custody.raw = 4;
	check(d1_envelope_encode(&env, buf, sizeof(buf)) == 0,
	      "an unlock carries no custody");
	make_repair(&env, D1_OP_ABORT_REPAIR);
	env.body.repair.entries[0].successor_present = true;
	env.body.repair.entries[0].successor.raw = 4;
	check(d1_envelope_encode(&env, buf, sizeof(buf)) == 0,
	      "nor does an abort carry the state it captured");
	make_repair(&env, D1_OP_COMMIT_REPAIR);
	env.body.repair.entries[0].payload_present = true;
	env.body.repair.entries[0].payload = payload;
	env.body.repair.entries[0].payload_len = (uint32_t)sizeof(payload);
	d1_checksum_compute(D1_CKSUM_CRC32C, payload, sizeof(payload),
			    &env.body.repair.entries[0].checksum);
	check(d1_envelope_encode(&env, buf, sizeof(buf)) == 0,
	      "a commit stages nothing; prepare_repair already did");
	make_repair(&env, D1_OP_PREPARE_REPAIR);
	env.body.repair.entries[1].payload_present = false;
	check(d1_envelope_encode(&env, buf, sizeof(buf)) == 0,
	      "and a prepare stages its whole vector");

	/* The certificate, which is clear_error's alone. */
	make_repair(&env, D1_OP_UNLOCK);
	env.body.repair.certificate_present = true;
	check(d1_envelope_encode(&env, buf, sizeof(buf)) == 0,
	      "only clear_error carries a certificate");
	make_repair(&env, D1_OP_CLEAR_ERROR);
	env.body.repair.certificate_present = false;
	check(d1_envelope_encode(&env, buf, sizeof(buf)) == 0,
	      "and it always carries one");

	/* And the shape rules the whole vector keeps. */
	make_repair(&env, D1_OP_BEGIN_REPAIR);
	env.body.repair.entries[1].index = env.body.repair.entries[0].index;
	check(d1_envelope_encode(&env, buf, sizeof(buf)) == 0,
	      "one repair never names one chunk twice");
	make_repair(&env, D1_OP_BEGIN_REPAIR);
	env.body.repair.range_end = env.body.repair.range_begin;
	check(d1_envelope_encode(&env, buf, sizeof(buf)) == 0,
	      "and its range is a range");
	make_repair(&env, D1_OP_BEGIN_REPAIR);
	env.body.repair.entries[0].index = env.body.repair.range_end;
	check(d1_envelope_encode(&env, buf, sizeof(buf)) == 0,
	      "and every member is inside it");
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
	env.admission.raw = 3;
	env.incarnation = 2;
	env.key.sequence = 9;
	env.op = D1_OP_COMMIT_BATCH;
	env.body.lifecycle.range_begin = 0;
	env.body.lifecycle.range_end = 2;
	env.body.lifecycle.count = 2;
	env.body.lifecycle.entries[0].index = 0;
	env.body.lifecycle.entries[0].owner.cohort.raw = 5;
	env.body.lifecycle.entries[0].owner.writer = 11;
	env.body.lifecycle.entries[0].owner.co_id = 1;
	env.body.lifecycle.entries[0].txn.raw = 11;
	env.body.lifecycle.entries[1].index = 1;
	env.body.lifecycle.entries[1].owner.cohort.raw = 5;
	env.body.lifecycle.entries[1].owner.writer = 11;
	env.body.lifecycle.entries[1].owner.co_id = 2;
	env.body.lifecycle.entries[1].txn.raw = 12;
	env.body.lifecycle.entries[1].predecessor_present = true;
	env.body.lifecycle.entries[1].predecessor.raw = 4;
	memcpy(env.body.lifecycle.prior_verifier, "\0\0\0\0\0\0\0\2", 8);

	len = d1_envelope_encode(&env, buf, sizeof(buf));
	check(len != 0 && d1_envelope_decode(buf, len, &back),
	      "commit envelope round trips");
	check(back.body.lifecycle.count == 2 &&
		      back.body.lifecycle.range_end == 2 &&
		      back.body.lifecycle.entries[1].predecessor_present &&
		      back.body.lifecycle.entries[1].predecessor.raw == 4,
	      "commit body survives");
	check(memcmp(back.body.lifecycle.prior_verifier,
		     env.body.lifecycle.prior_verifier, 8) == 0,
	      "prior verifier survives");

	memset(&env, 0, sizeof(env));
	env.op = D1_OP_RECOVERY_ADMIT;
	env.body.control.count = 1;
	env.body.control.txns[0].raw = 77;
	env.body.control.old_admission.raw = 5;
	env.body.control.new_admission_present = true;
	env.body.control.new_admission.raw = 6;
	env.body.control.read_epoch_present = true;
	env.body.control.read_epoch = 8;
	len = d1_envelope_encode(&env, buf, sizeof(buf));
	check(len != 0 && d1_envelope_decode(buf, len, &back),
	      "recovery_admit envelope round trips");
	check(back.body.control.txns[0].raw == 77 &&
		      back.body.control.new_admission.raw == 6 &&
		      back.body.control.read_epoch == 8,
	      "control body survives");
}

int main(void)
{
	scratch = calloc(1, D1_ENVELOPE_MAX);
	again = calloc(1, D1_ENVELOPE_MAX);
	if (!scratch || !again) {
		fprintf(stderr, "FAIL: no scratch\n");
		return 1;
	}
	test_golden_write_envelope();
	test_round_trip();
	test_the_round_trip_keeps_every_field();
	test_digest_binds_every_field();
	test_decoder_refusals();
	test_lifecycle_and_control();
	test_every_repair_operation_round_trips();
	test_a_repair_option_belongs_to_its_operation();

	if (failures) {
		fprintf(stderr, "%u check(s) failed\n", failures);
		return 1;
	}
	free(scratch);
	free(again);
	printf("d1_envelope_test: all checks passed\n");
	return 0;
}
