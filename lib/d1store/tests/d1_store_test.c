/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * D1 storage model: the ordinary write lifecycle, activation and owner
 * collisions -- the A, B and C traces of the design.
 *
 * The thing each case is really asking is whether a successful write is
 * only a successful write: that durability does not publish, that
 * finalize does not publish, and that the one operation which does
 * publish in a single step says so in its own result.
 */

#include <stdio.h>
#include <string.h>

#include "d1_digest.h"
#include "d1_store.h"

static unsigned int failures;

static void check(bool ok, const char *what)
{
	if (!ok) {
		failures++;
		fprintf(stderr, "FAIL: %s\n", what);
	}
}

#define CHUNK_BYTES 4096u
#define MAX_FILE_BYTES (1024u * 1024u)

static struct d1_objkey object;
static struct d1_uuid origin;
static uint64_t next_sequence = 1;

static void fill_uuid(struct d1_uuid *u, uint8_t base)
{
	unsigned int i;

	for (i = 0; i < D1_UUID_BYTES; i++)
		u->bytes[i] = (uint8_t)(base + i);
}

static void env_init(struct d1_envelope *env, struct d1_store *s,
		     d1_id_t admission, uint32_t op)
{
	memset(env, 0, sizeof(*env));
	env->object = object;
	env->admission = admission;
	env->incarnation = d1_store_incarnation(s);
	env->key.origin = origin;
	env->key.sequence = next_sequence++;
	env->op = op;
}

static void write_entry(struct d1_write_entry *e, uint64_t index,
			uint32_t writer, uint32_t co_id, const uint8_t *data,
			uint32_t len, bool guard_check,
			const struct d1_guard *expected)
{
	memset(e, 0, sizeof(*e));
	e->index = index;
	e->owner.cohort = 1;
	e->owner.writer = writer;
	e->owner.co_id = co_id;
	e->guard_check = guard_check;
	if (expected)
		e->expected = *expected;
	e->payload = data;
	e->payload_len = len;
	d1_checksum_compute(D1_CKSUM_CRC32C, data, len, &e->checksum);
}

static const uint8_t payload_a[8] = { 0x41, 0x41, 0x41, 0x41,
				      0x41, 0x41, 0x41, 0x41 };
static const uint8_t payload_b[8] = { 0x42, 0x42, 0x42, 0x42,
				      0x42, 0x42, 0x42, 0x42 };

/* A: write, finalize, commit, and what each of them does not do. */
static void test_ordinary_lifecycle(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_envelope env;
	struct d1_result res;
	struct d1_guard guard;
	d1_id_t admission, txn, version;
	d1_id_t visible;
	uint8_t verifier[D1_VERIFIER_BYTES];

	fill_uuid(&store_uuid, 0x90);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	check(s != NULL, "store opens");
	if (!s)
		return;
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_READ | D1_RIGHT_WRITE |
					     D1_RIGHT_SINGLE_WRITER);
	check(admission != 0, "fixture issues an admission");
	d1_store_verifier(s, verifier);

	/* A1: a stable write is PREPARED, and nothing is visible. */
	env_init(&env, s, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	env.body.write.activate = false;
	write_entry(&env.body.write.entries[0], 0, 11, 1, payload_a,
		    sizeof(payload_a), true,
		    &(struct d1_guard){ .never_written = true });
	check(d1_store_apply(s, &env, &res) == D1_OK, "write applies");
	check(res.entries[0].status == D1_OK, "write succeeds");
	check(res.entries[0].phase == D1_PHASE_PREPARED, "write is PREPARED");
	check(!res.entries[0].activated, "write did not activate");
	check(res.entries[0].stability == D1_FILE_SYNC,
	      "actual durability is FILE_SYNC");
	check(!d1_store_visible(s, &object, 0, &visible),
	      "a durable write is still not visible");
	check(d1_store_guard(s, &object, 0, &guard) && !guard.never_written &&
		      guard.generation == 0 && guard.writer == 11,
	      "the first success is generation zero with its writer");
	txn = res.entries[0].txn;
	version = res.entries[0].version;

	/* A2: finalize does not publish either. */
	env_init(&env, s, admission, D1_OP_FINALIZE_BATCH);
	env.body.lifecycle.range_begin = 0;
	env.body.lifecycle.range_end = 1;
	env.body.lifecycle.count = 1;
	env.body.lifecycle.entries[0].index = 0;
	env.body.lifecycle.entries[0].owner.cohort = 1;
	env.body.lifecycle.entries[0].owner.writer = 11;
	env.body.lifecycle.entries[0].owner.co_id = 1;
	env.body.lifecycle.entries[0].txn = txn;
	memcpy(env.body.lifecycle.prior_verifier, verifier, sizeof(verifier));
	check(d1_store_apply(s, &env, &res) == D1_OK, "finalize applies");
	check(res.entries[0].status == D1_OK &&
		      res.entries[0].phase == D1_PHASE_FINALIZED,
	      "finalize reaches FINALIZED");
	check(!d1_store_visible(s, &object, 0, &visible),
	      "a finalized version is still not visible");

	/* A3/A4: commit publishes, and the extent follows the payload. */
	env_init(&env, s, admission, D1_OP_COMMIT_BATCH);
	env.body.lifecycle.range_begin = 0;
	env.body.lifecycle.range_end = 1;
	env.body.lifecycle.count = 1;
	env.body.lifecycle.entries[0].index = 0;
	env.body.lifecycle.entries[0].owner.cohort = 1;
	env.body.lifecycle.entries[0].owner.writer = 11;
	env.body.lifecycle.entries[0].owner.co_id = 1;
	env.body.lifecycle.entries[0].txn = txn;
	memcpy(env.body.lifecycle.prior_verifier, verifier, sizeof(verifier));
	check(d1_store_apply(s, &env, &res) == D1_OK, "commit applies");
	check(res.entries[0].status == D1_OK &&
		      res.entries[0].phase == D1_PHASE_COMMITTED,
	      "commit reaches COMMITTED");
	check(d1_store_visible(s, &object, 0, &visible) && visible == version,
	      "commit publishes the version the write made");
	check(d1_store_eof(s, &object) == sizeof(payload_a),
	      "EOF follows the committed extent");

	/* A5: the same key with a different body changes nothing. */
	{
		struct d1_envelope replay = env;
		struct d1_result before, after;

		check(d1_store_apply(s, &env, &before) == D1_OK,
		      "exact replay applies");
		check(before.entries[0].status == D1_OK &&
			      before.entries[0].phase == D1_PHASE_COMMITTED,
		      "exact replay returns the recorded result");

		replay.body.lifecycle.entries[0].owner.co_id = 99;
		check(d1_store_apply(s, &replay, &after) == D1_OK,
		      "conflicting replay applies");
		check(after.entries[0].status == D1_REPLAY_CONFLICT,
		      "a changed body under a used key conflicts");
		check(d1_store_visible(s, &object, 0, &visible) &&
			      visible == version,
		      "and changes nothing");
	}

	d1_store_free(s);
}

/* B: the activation table, cell by cell. */
static void test_activation(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_envelope env;
	struct d1_result res;
	struct d1_guard guard, before;
	d1_id_t single, multi, visible;

	fill_uuid(&store_uuid, 0xa0);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	single = d1_fixture_admit(s, &object, 11,
				  D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	multi = d1_fixture_admit(s, &object, 12, D1_RIGHT_WRITE);

	/* B1: single writer, flag, DATA_SYNC, empty chunk: one event. */
	env_init(&env, s, single, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_DATA_SYNC;
	env.body.write.activate = true;
	write_entry(&env.body.write.entries[0], 0, 11, 1, payload_a,
		    sizeof(payload_a), true,
		    &(struct d1_guard){ .never_written = true });
	d1_store_apply(s, &env, &res);
	check(res.entries[0].status == D1_OK && res.entries[0].activated,
	      "an empty chunk activates");
	check(res.entries[0].phase == D1_PHASE_COMMITTED,
	      "activation commits in one event");
	check(d1_store_visible(s, &object, 0, &visible),
	      "and the chunk is visible");

	/* B2: the same request on a nonempty chunk does not activate. */
	d1_store_guard(s, &object, 0, &guard);
	env_init(&env, s, single, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_DATA_SYNC;
	env.body.write.activate = true;
	write_entry(&env.body.write.entries[0], 0, 11, 2, payload_b,
		    sizeof(payload_b), true, &guard);
	d1_store_apply(s, &env, &res);
	check(res.entries[0].status == D1_OK && !res.entries[0].activated,
	      "a nonempty chunk does not activate");
	check(res.entries[0].phase == D1_PHASE_PREPARED,
	      "it is PREPARED and still needs finalize and commit");

	/* B2: UNSTABLE never activates, even on an empty chunk. */
	env_init(&env, s, single, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_UNSTABLE;
	env.body.write.activate = true;
	write_entry(&env.body.write.entries[0], 5, 11, 3, payload_a,
		    sizeof(payload_a), true,
		    &(struct d1_guard){ .never_written = true });
	d1_store_apply(s, &env, &res);
	check(res.entries[0].status == D1_OK && !res.entries[0].activated,
	      "an UNSTABLE request does not activate");
	check(!d1_store_visible(s, &object, 5, &visible),
	      "and publishes nothing");

	/* B3: a multi-writer request asking to activate is refused. */
	d1_store_guard(s, &object, 7, &before);
	env_init(&env, s, multi, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	env.body.write.activate = true;
	write_entry(&env.body.write.entries[0], 7, 12, 4, payload_a,
		    sizeof(payload_a), true,
		    &(struct d1_guard){ .never_written = true });
	d1_store_apply(s, &env, &res);
	check(res.entries[0].status == D1_INVALID,
	      "a multi-writer activation request is refused");
	check(d1_store_guard(s, &object, 7, &guard) &&
		      guard.never_written == before.never_written &&
		      guard.generation == before.generation,
	      "and the guard is untouched");
	check(!d1_store_visible(s, &object, 7, &visible), "and nothing wrote");

	d1_store_free(s);
}

/* C: an owner names one version, on one chunk, of one object. */
static void test_owner_collision(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_envelope env;
	struct d1_result res;
	d1_id_t admission, visible;

	fill_uuid(&store_uuid, 0xb0);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);

	env_init(&env, s, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 0, 11, 1, payload_a,
		    sizeof(payload_a), true,
		    &(struct d1_guard){ .never_written = true });
	d1_store_apply(s, &env, &res);
	check(res.entries[0].status == D1_OK, "the first write succeeds");

	/* C1: the same owner on another chunk is a conflict. */
	env_init(&env, s, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 2, 11, 1, payload_b,
		    sizeof(payload_b), true,
		    &(struct d1_guard){ .never_written = true });
	d1_store_apply(s, &env, &res);
	check(res.entries[0].status == D1_OWNER_CONFLICT,
	      "an owner reused for another chunk conflicts");
	check(!d1_store_visible(s, &object, 2, &visible),
	      "and writes nothing there");

	d1_store_free(s);
}

/* The predicates a write is admitted by, each refused on its own. */
static void test_write_refusals(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_envelope env;
	struct d1_result res;
	struct d1_guard guard;
	d1_id_t admission, other;

	fill_uuid(&store_uuid, 0xc0);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	other = d1_fixture_admit(s, &object, 12, D1_RIGHT_READ);

	/* A wrong guard is refused and the result carries the current one. */
	env_init(&env, s, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 0, 11, 1, payload_a,
		    sizeof(payload_a), true,
		    &(struct d1_guard){ .generation = 4, .writer = 11 });
	d1_store_apply(s, &env, &res);
	check(res.entries[0].status == D1_GUARDED,
	      "a guard that does not hold is refused");
	check(res.entries[0].guard.never_written,
	      "and the refusal carries the current guard");

	/* A payload that does not match its checksum is refused. */
	env_init(&env, s, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 0, 11, 2, payload_a,
		    sizeof(payload_a), true,
		    &(struct d1_guard){ .never_written = true });
	env.body.write.entries[0].checksum.digest[0] ^= 0xffu;
	d1_store_apply(s, &env, &res);
	check(res.entries[0].status == D1_CHECKSUM,
	      "a payload that fails its checksum is refused");
	check(d1_store_guard(s, &object, 0, &guard) && guard.never_written,
	      "and the chunk is untouched");

	/* An admission without WRITE cannot write. */
	env_init(&env, s, other, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 0, 12, 3, payload_a,
		    sizeof(payload_a), true,
		    &(struct d1_guard){ .never_written = true });
	d1_store_apply(s, &env, &res);
	check(res.entries[0].status == D1_STALE_AUTH,
	      "an admission without the right is refused");

	/* A second uncommitted transaction on a chunk is refused. */
	env_init(&env, s, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 1, 11, 4, payload_a,
		    sizeof(payload_a), true,
		    &(struct d1_guard){ .never_written = true });
	d1_store_apply(s, &env, &res);
	check(res.entries[0].status == D1_OK, "the first write is admitted");
	d1_store_guard(s, &object, 1, &guard);
	env_init(&env, s, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 1, 11, 5, payload_b,
		    sizeof(payload_b), true, &guard);
	d1_store_apply(s, &env, &res);
	check(res.entries[0].status == D1_GUARDED,
	      "a second uncommitted transaction is refused");

	/* A revoked admission is dead, even with a fresh guard. */
	d1_fixture_revoke(s, admission);
	env_init(&env, s, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 3, 11, 6, payload_a,
		    sizeof(payload_a), true,
		    &(struct d1_guard){ .never_written = true });
	d1_store_apply(s, &env, &res);
	check(res.entries[0].status == D1_STALE_AUTH,
	      "a revoked admission cannot write");

	d1_store_free(s);
}

/* Phase order: commit before finalize is refused and leaves PREPARED. */
static void test_phase_order(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_envelope env;
	struct d1_result res;
	uint8_t verifier[D1_VERIFIER_BYTES];
	d1_id_t admission, txn, visible;

	fill_uuid(&store_uuid, 0xd0);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	d1_store_verifier(s, verifier);

	env_init(&env, s, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 0, 11, 1, payload_a,
		    sizeof(payload_a), true,
		    &(struct d1_guard){ .never_written = true });
	d1_store_apply(s, &env, &res);
	txn = res.entries[0].txn;

	env_init(&env, s, admission, D1_OP_COMMIT_BATCH);
	env.body.lifecycle.range_begin = 0;
	env.body.lifecycle.range_end = 1;
	env.body.lifecycle.count = 1;
	env.body.lifecycle.entries[0].index = 0;
	env.body.lifecycle.entries[0].owner.cohort = 1;
	env.body.lifecycle.entries[0].owner.writer = 11;
	env.body.lifecycle.entries[0].owner.co_id = 1;
	env.body.lifecycle.entries[0].txn = txn;
	memcpy(env.body.lifecycle.prior_verifier, verifier, sizeof(verifier));
	d1_store_apply(s, &env, &res);
	check(res.entries[0].status == D1_BAD_PHASE,
	      "commit before finalize is refused");
	check(!d1_store_visible(s, &object, 0, &visible),
	      "and publishes nothing");

	/* An index outside the named range is not this call's member. */
	env_init(&env, s, admission, D1_OP_FINALIZE_BATCH);
	env.body.lifecycle.range_begin = 2;
	env.body.lifecycle.range_end = 3;
	env.body.lifecycle.count = 1;
	env.body.lifecycle.entries[0].index = 0;
	env.body.lifecycle.entries[0].owner.cohort = 1;
	env.body.lifecycle.entries[0].owner.writer = 11;
	env.body.lifecycle.entries[0].owner.co_id = 1;
	env.body.lifecycle.entries[0].txn = txn;
	memcpy(env.body.lifecycle.prior_verifier, verifier, sizeof(verifier));
	d1_store_apply(s, &env, &res);
	check(res.entries[0].status == D1_INVALID,
	      "a member outside the named range is refused");

	d1_store_free(s);
}

/* An operation this slice does not implement mutates nothing. */
static void test_unsupported(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_envelope env;
	struct d1_result res;
	d1_id_t admission;

	fill_uuid(&store_uuid, 0xe0);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	admission = d1_fixture_admit(s, &object, 11, D1_RIGHT_WRITE);

	env_init(&env, s, admission, D1_OP_LEASE_REAP);
	env.body.control.count = 1;
	env.body.control.txns[0] = 1;
	check(d1_store_apply(s, &env, &res) == D1_UNSUPPORTED,
	      "an unimplemented operation says so");
	check(res.count == 0, "and answers no entries");

	d1_store_free(s);
}

/* Write, finalize and commit one chunk, and answer its version. */
static d1_id_t commit_chunk(struct d1_store *s, d1_id_t admission,
			    uint64_t index, uint32_t co_id, const uint8_t *data,
			    uint32_t len, const struct d1_guard *expected,
			    d1_id_t predecessor, d1_id_t *txn_out)
{
	struct d1_envelope env;
	struct d1_result res;
	uint8_t verifier[D1_VERIFIER_BYTES];
	d1_id_t txn, version;
	unsigned int pass;

	d1_store_verifier(s, verifier);
	env_init(&env, s, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], index, 11, co_id, data, len,
		    true, expected);
	if (d1_store_apply(s, &env, &res) != D1_OK ||
	    res.entries[0].status != D1_OK)
		return 0;
	txn = res.entries[0].txn;
	version = res.entries[0].version;

	for (pass = 0; pass < 2; pass++) {
		env_init(&env, s, admission,
			 pass == 0 ? D1_OP_FINALIZE_BATCH : D1_OP_COMMIT_BATCH);
		env.body.lifecycle.range_begin = index;
		env.body.lifecycle.range_end = index + 1;
		env.body.lifecycle.count = 1;
		env.body.lifecycle.entries[0].index = index;
		env.body.lifecycle.entries[0].owner.cohort = 1;
		env.body.lifecycle.entries[0].owner.writer = 11;
		env.body.lifecycle.entries[0].owner.co_id = co_id;
		env.body.lifecycle.entries[0].txn = txn;
		/*
		 * Finalize and commit name the predecessor the write
		 * recorded; a caller that does not know it is not the
		 * caller that wrote it.
		 */
		env.body.lifecycle.entries[0].predecessor_present =
			predecessor != 0;
		env.body.lifecycle.entries[0].predecessor = predecessor;
		memcpy(env.body.lifecycle.prior_verifier, verifier,
		       sizeof(verifier));
		if (d1_store_apply(s, &env, &res) != D1_OK ||
		    res.entries[0].status != D1_OK)
			return 0;
	}
	if (txn_out)
		*txn_out = txn;
	return version;
}

static uint32_t rollback_one(struct d1_store *s, d1_id_t admission,
			     uint64_t index, uint32_t co_id, d1_id_t txn,
			     bool custody_present, d1_id_t custody,
			     struct d1_entry_result *out)
{
	struct d1_envelope env;
	struct d1_result res;

	env_init(&env, s, admission, D1_OP_ROLLBACK_BATCH);
	env.body.rollback.range_begin = index;
	env.body.rollback.range_end = index + 1;
	env.body.rollback.count = 1;
	env.body.rollback.entries[0].index = index;
	env.body.rollback.entries[0].owner.cohort = 1;
	env.body.rollback.entries[0].owner.writer = 11;
	env.body.rollback.entries[0].owner.co_id = co_id;
	env.body.rollback.entries[0].txn = txn;
	env.body.rollback.entries[0].custody_present = custody_present;
	env.body.rollback.entries[0].custody = custody;
	if (d1_store_apply(s, &env, &res) != D1_OK)
		return D1_INVALID;
	if (out)
		*out = res.entries[0];
	return res.entries[0].status;
}

/* Cancelling one's own private work, and what it does not undo. */
static void test_private_rollback(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_envelope env;
	struct d1_result res;
	struct d1_guard before, after;
	d1_id_t admission, txn, visible;

	fill_uuid(&store_uuid, 0xf0);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);

	env_init(&env, s, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 0, 11, 1, payload_a,
		    sizeof(payload_a), true,
		    &(struct d1_guard){ .never_written = true });
	d1_store_apply(s, &env, &res);
	check(res.entries[0].status == D1_OK, "a write is admitted");
	txn = res.entries[0].txn;
	d1_store_guard(s, &object, 0, &before);

	check(rollback_one(s, admission, 0, 1, txn, false, 0, NULL) == D1_OK,
	      "its owner may cancel it");
	check(!d1_store_visible(s, &object, 0, &visible),
	      "and nothing became visible");
	check(d1_store_guard(s, &object, 0, &after) &&
		      after.generation == before.generation &&
		      !after.never_written,
	      "the generation does not go back");

	/* The pending slot is free again, for a new owner. */
	env_init(&env, s, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 0, 11, 2, payload_b,
		    sizeof(payload_b), true, &after);
	d1_store_apply(s, &env, &res);
	check(res.entries[0].status == D1_OK,
	      "a new owner may write the chunk again");
	check(res.entries[0].guard.generation == before.generation + 1,
	      "and the generation moves on rather than back");

	/* Committed data is not the owner's to roll back. */
	{
		d1_id_t ctxn = 0;
		struct d1_entry_result entry;

		check(rollback_one(s, admission, 0, 2, res.entries[0].txn,
				   false, 0, NULL) == D1_OK,
		      "the second write is cancelled too");
		check(commit_chunk(s, admission, 1, 3, payload_a,
				   sizeof(payload_a),
				   &(struct d1_guard){ .never_written = true },
				   0, &ctxn) != 0,
		      "a chunk is committed");
		check(rollback_one(s, admission, 1, 3, ctxn, false, 0,
				   &entry) == D1_STALE_AUTH,
		      "committed data needs custody, not ownership");
		check(d1_store_visible(s, &object, 1, &visible),
		      "and is still visible");
	}

	d1_store_free(s);
}

/* D: sparse commits, holes, and what a rollback does to EOF. */
static void test_sparse_and_rollback_extents(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_interval holes[D1_MAX_INTERVALS];
	struct d1_guard guard;
	static uint8_t big[4096];
	static uint8_t small[100];
	static uint8_t tiny[50];
	d1_id_t admission, v3, v4, v5, txn4, custody, visible;
	uint32_t n;

	memset(big, 0x11, sizeof(big));
	memset(small, 0x22, sizeof(small));
	memset(tiny, 0x33, sizeof(tiny));

	fill_uuid(&store_uuid, 0x11);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_WRITE | D1_RIGHT_REPAIR |
					     D1_RIGHT_SINGLE_WRITER);

	/* D1: a sparse commit at chunk 3 of 100 bytes. */
	v3 = commit_chunk(s, admission, 3, 1, small, sizeof(small),
			  &(struct d1_guard){ .never_written = true }, 0, NULL);
	check(v3 != 0, "the sparse chunk commits");
	check(d1_store_eof(s, &object) == 3 * CHUNK_BYTES + sizeof(small),
	      "EOF is the end of the only interval");
	n = d1_store_holes(s, &object, holes, D1_MAX_INTERVALS);
	check(n == 1 && holes[0].start == 0 && holes[0].end == 3 * CHUNK_BYTES,
	      "everything before it is an explicit hole");

	/* D2: a later chunk 5 of 50 bytes. */
	v5 = commit_chunk(s, admission, 5, 2, tiny, sizeof(tiny),
			  &(struct d1_guard){ .never_written = true }, 0, NULL);
	check(v5 != 0, "the later chunk commits");
	check(d1_store_eof(s, &object) == 5 * CHUNK_BYTES + sizeof(tiny),
	      "EOF follows the highest interval");
	n = d1_store_holes(s, &object, holes, D1_MAX_INTERVALS);
	check(n == 2 && holes[1].start == 3 * CHUNK_BYTES + sizeof(small) &&
		      holes[1].end == 5 * CHUNK_BYTES,
	      "the gap between them is a hole");

	/* D3: replace chunk 3 with a full image, then roll it back. */
	d1_store_guard(s, &object, 3, &guard);
	v4 = commit_chunk(s, admission, 3, 3, big, sizeof(big), &guard, v3,
			  &txn4);
	check(v4 != 0 && v4 != v3, "the replacement commits");
	check(d1_store_eof(s, &object) == 5 * CHUNK_BYTES + sizeof(tiny),
	      "the higher chunk still sets EOF");

	custody = d1_fixture_custody(s, v4);
	check(custody != 0, "the fixture issues custody over it");
	check(rollback_one(s, admission, 3, 3, txn4, true, custody, NULL) ==
		      D1_OK,
	      "custody rolls the replacement back");
	check(d1_store_visible(s, &object, 3, &visible) && visible == v3,
	      "and the predecessor is visible again");
	check(d1_store_eof(s, &object) == 5 * CHUNK_BYTES + sizeof(tiny),
	      "EOF stays high because chunk 5 still holds it");
	n = d1_store_holes(s, &object, holes, D1_MAX_INTERVALS);
	check(n == 2 && holes[1].start == 3 * CHUNK_BYTES + sizeof(small),
	      "and the hole vector is restored with it");

	d1_store_free(s);
}

/* D4: the same rollback with nothing above it shrinks EOF. */
static void test_rollback_shrinks_eof(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_guard guard;
	static uint8_t big[4096];
	static uint8_t small[100];
	d1_id_t admission, v3, v4, txn4, custody, visible;

	memset(big, 0x44, sizeof(big));
	memset(small, 0x55, sizeof(small));

	fill_uuid(&store_uuid, 0x22);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_WRITE | D1_RIGHT_REPAIR |
					     D1_RIGHT_SINGLE_WRITER);

	v3 = commit_chunk(s, admission, 3, 1, small, sizeof(small),
			  &(struct d1_guard){ .never_written = true }, 0, NULL);
	check(d1_store_eof(s, &object) == 3 * CHUNK_BYTES + sizeof(small),
	      "the short image sets EOF");
	d1_store_guard(s, &object, 3, &guard);
	v4 = commit_chunk(s, admission, 3, 2, big, sizeof(big), &guard, v3,
			  &txn4);
	check(v4 != 0, "the full image commits");
	check(d1_store_eof(s, &object) == 4 * CHUNK_BYTES,
	      "and EOF grows with it");

	custody = d1_fixture_custody(s, v4);
	check(rollback_one(s, admission, 3, 2, txn4, true, custody, NULL) ==
		      D1_OK,
	      "the replacement rolls back");
	check(d1_store_visible(s, &object, 3, &visible) && visible == v3,
	      "the predecessor is visible");
	check(d1_store_eof(s, &object) == 3 * CHUNK_BYTES + sizeof(small),
	      "and EOF shrinks with the payload it came from");

	d1_store_free(s);
}

/* A predecessor that has been released is not a predecessor any more. */
static void test_released_predecessor(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_guard guard;
	struct d1_entry_result entry;
	static uint8_t data[64];
	d1_id_t admission, v1, v2, txn2, custody, visible;

	memset(data, 0x66, sizeof(data));
	fill_uuid(&store_uuid, 0x33);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_WRITE | D1_RIGHT_REPAIR |
					     D1_RIGHT_SINGLE_WRITER);

	v1 = commit_chunk(s, admission, 0, 1, data, sizeof(data),
			  &(struct d1_guard){ .never_written = true }, 0, NULL);
	d1_store_guard(s, &object, 0, &guard);
	v2 = commit_chunk(s, admission, 0, 2, data, sizeof(data), &guard, v1,
			  &txn2);
	check(v1 != 0 && v2 != 0, "two versions commit in turn");

	check(d1_fixture_release_predecessor(s, v1),
	      "the displaced predecessor may be released");
	check(!d1_fixture_release_predecessor(s, v2),
	      "the visible version may not");

	custody = d1_fixture_custody(s, v2);
	check(rollback_one(s, admission, 0, 2, txn2, true, custody, &entry) ==
		      D1_NO_PREDECESSOR,
	      "a released predecessor is no longer eligible");
	check(d1_store_visible(s, &object, 0, &visible) && visible == v2,
	      "and the current data stays exactly where it is");

	d1_store_free(s);
}

/* Write and finalize one chunk, stopping short of commit. */
static d1_id_t finalize_chunk(struct d1_store *s, d1_id_t admission,
			      uint64_t index, uint32_t co_id,
			      const uint8_t *data, uint32_t len,
			      const struct d1_guard *expected,
			      d1_id_t predecessor, d1_id_t *txn_out)
{
	struct d1_envelope env;
	struct d1_result res;
	uint8_t verifier[D1_VERIFIER_BYTES];
	d1_id_t txn, version;

	d1_store_verifier(s, verifier);
	env_init(&env, s, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], index, 11, co_id, data, len,
		    true, expected);
	if (d1_store_apply(s, &env, &res) != D1_OK ||
	    res.entries[0].status != D1_OK)
		return 0;
	txn = res.entries[0].txn;
	version = res.entries[0].version;

	env_init(&env, s, admission, D1_OP_FINALIZE_BATCH);
	env.body.lifecycle.range_begin = index;
	env.body.lifecycle.range_end = index + 1;
	env.body.lifecycle.count = 1;
	env.body.lifecycle.entries[0].index = index;
	env.body.lifecycle.entries[0].owner.cohort = 1;
	env.body.lifecycle.entries[0].owner.writer = 11;
	env.body.lifecycle.entries[0].owner.co_id = co_id;
	env.body.lifecycle.entries[0].txn = txn;
	env.body.lifecycle.entries[0].predecessor_present = predecessor != 0;
	env.body.lifecycle.entries[0].predecessor = predecessor;
	memcpy(env.body.lifecycle.prior_verifier, verifier, sizeof(verifier));
	if (d1_store_apply(s, &env, &res) != D1_OK ||
	    res.entries[0].status != D1_OK)
		return 0;
	if (txn_out)
		*txn_out = txn;
	return version;
}

/* E: a view decides once, and keeps what it decided. */
static void test_view_is_stable(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_view *view = NULL, *after = NULL;
	struct d1_guard guard;
	static uint8_t first[64];
	static uint8_t second[64];
	uint8_t got[64];
	uint32_t got_len;
	d1_id_t admission, v1, v2, seen;

	memset(first, 0xa1, sizeof(first));
	memset(second, 0xb2, sizeof(second));
	fill_uuid(&store_uuid, 0x44);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_READ | D1_RIGHT_WRITE |
					     D1_RIGHT_SINGLE_WRITER);

	v1 = commit_chunk(s, admission, 0, 1, first, sizeof(first),
			  &(struct d1_guard){ .never_written = true }, 0, NULL);
	check(v1 != 0, "the first version commits");

	check(d1_view_open(s, &object, admission, D1_SELECT_ORDINARY, NULL, 0,
			   1, &view) == D1_OK,
	      "a view opens over the committed chunk");
	check(d1_view_read(view, 0, got, sizeof(got), &got_len) == D1_OK &&
		      got_len == sizeof(first) &&
		      memcmp(got, first, sizeof(first)) == 0,
	      "and reads what was committed");

	/* E1: a later commit does not reach back into the open view. */
	d1_store_guard(s, &object, 0, &guard);
	v2 = commit_chunk(s, admission, 0, 2, second, sizeof(second), &guard,
			  v1, NULL);
	check(v2 != 0 && v2 != v1, "a second version commits over it");
	check(d1_view_version(view, 0, &seen) && seen == v1,
	      "the open view still names the version it chose");
	check(d1_view_read(view, 0, got, sizeof(got), &got_len) == D1_OK &&
		      memcmp(got, first, sizeof(first)) == 0,
	      "and still reads its bytes");

	/* E2: what the view pins cannot be released underneath it. */
	check(!d1_fixture_release_predecessor(s, v1),
	      "a pinned predecessor is not releasable");
	d1_view_close(s, view);
	check(d1_fixture_release_predecessor(s, v1),
	      "and becomes releasable once the view closes");

	/* A view opened after the commit sees the new version. */
	check(d1_view_open(s, &object, admission, D1_SELECT_ORDINARY, NULL, 0,
			   1, &after) == D1_OK,
	      "a later view opens");
	check(d1_view_version(after, 0, &seen) && seen == v2,
	      "and names the newer version");
	d1_view_close(s, after);

	d1_store_free(s);
}

/* E5: a version pinned twice needs both pins dropped. */
static void test_view_unpin_order(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_view *a = NULL, *b = NULL;
	struct d1_guard guard;
	static uint8_t data[32];
	d1_id_t admission, v1, v2;

	memset(data, 0xc3, sizeof(data));
	fill_uuid(&store_uuid, 0x55);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_READ | D1_RIGHT_WRITE |
					     D1_RIGHT_SINGLE_WRITER);

	v1 = commit_chunk(s, admission, 0, 1, data, sizeof(data),
			  &(struct d1_guard){ .never_written = true }, 0, NULL);
	check(d1_view_open(s, &object, admission, D1_SELECT_ORDINARY, NULL, 0,
			   1, &a) == D1_OK,
	      "the first view opens");
	check(d1_view_open(s, &object, admission, D1_SELECT_ORDINARY, NULL, 0,
			   1, &b) == D1_OK,
	      "the second view opens");

	d1_store_guard(s, &object, 0, &guard);
	v2 = commit_chunk(s, admission, 0, 2, data, sizeof(data), &guard, v1,
			  NULL);
	check(v2 != 0, "a new version displaces it");

	d1_view_close(s, a);
	check(!d1_fixture_release_predecessor(s, v1),
	      "one of two pins dropped is not enough");
	d1_view_close(s, b);
	check(d1_fixture_release_predecessor(s, v1),
	      "the last pin dropped makes it releasable");

	d1_store_free(s);
}

/* E3: an owner may select its own finalized version. */
static void test_view_owner_selection(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_view *own = NULL, *ordinary = NULL;
	struct d1_guard guard;
	static uint8_t committed[48];
	static uint8_t pending[48];
	uint8_t got[48];
	uint32_t got_len;
	struct d1_owner owner = { .cohort = 1, .writer = 11, .co_id = 2 };
	d1_id_t admission, v1, v2, seen;

	memset(committed, 0xd4, sizeof(committed));
	memset(pending, 0xe5, sizeof(pending));
	fill_uuid(&store_uuid, 0x66);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_READ | D1_RIGHT_WRITE |
					     D1_RIGHT_SINGLE_WRITER);

	v1 = commit_chunk(s, admission, 0, 1, committed, sizeof(committed),
			  &(struct d1_guard){ .never_written = true }, 0, NULL);
	d1_store_guard(s, &object, 0, &guard);
	v2 = finalize_chunk(s, admission, 0, 2, pending, sizeof(pending),
			    &guard, v1, NULL);
	check(v1 != 0 && v2 != 0, "one version commits and one finalizes");

	check(d1_view_open(s, &object, admission, D1_SELECT_OWNER, &owner, 0, 1,
			   &own) == D1_OK,
	      "an owner view opens");
	check(d1_view_version(own, 0, &seen) && seen == v2,
	      "and selects the owner's own finalized version");
	check(d1_view_read(own, 0, got, sizeof(got), &got_len) == D1_OK &&
		      memcmp(got, pending, sizeof(pending)) == 0,
	      "and reads its bytes");

	check(d1_view_open(s, &object, admission, D1_SELECT_ORDINARY, NULL, 0,
			   1, &ordinary) == D1_OK,
	      "an ordinary view opens alongside it");
	check(d1_view_version(ordinary, 0, &seen) && seen == v1,
	      "and still sees only what is committed");

	/* Another owner's view falls back to the committed version. */
	{
		struct d1_view *other = NULL;
		struct d1_owner stranger = { .cohort = 1,
					     .writer = 11,
					     .co_id = 99 };

		check(d1_view_open(s, &object, admission, D1_SELECT_OWNER,
				   &stranger, 0, 1, &other) == D1_OK,
		      "a different owner's view opens");
		check(d1_view_version(other, 0, &seen) && seen == v1,
		      "and does not see somebody else's finalized version");
		d1_view_close(s, other);
	}

	d1_view_close(s, own);
	d1_view_close(s, ordinary);
	d1_store_free(s);
}

/* Holes, EOF and the admission a read needs. */
static void test_view_holes_and_admission(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_view *view = NULL;
	static uint8_t data[100];
	uint8_t got[256];
	uint32_t got_len, i;
	d1_id_t admission, writeonly;

	memset(data, 0xf6, sizeof(data));
	fill_uuid(&store_uuid, 0x77);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_READ | D1_RIGHT_WRITE |
					     D1_RIGHT_SINGLE_WRITER);
	writeonly = d1_fixture_admit(s, &object, 12, D1_RIGHT_WRITE);

	/* Only chunk 1 is written, so chunk 0 is a hole below EOF. */
	check(commit_chunk(s, admission, 1, 1, data, sizeof(data),
			   &(struct d1_guard){ .never_written = true }, 0,
			   NULL) != 0,
	      "the sparse chunk commits");
	check(d1_view_open(s, &object, admission, D1_SELECT_ORDINARY, NULL, 0,
			   2, &view) == D1_OK,
	      "a view opens across the hole");
	check(d1_view_eof(view) == CHUNK_BYTES + sizeof(data),
	      "the view records the EOF it saw");

	check(d1_view_read(view, 0, got, sizeof(got), &got_len) == D1_OK &&
		      got_len == sizeof(got),
	      "a read inside the hole is not short");
	for (i = 0; i < sizeof(got); i++)
		if (got[i] != 0)
			break;
	check(i == sizeof(got), "and the hole reads as zeros");

	check(d1_view_read(view, d1_view_eof(view), got, sizeof(got),
			   &got_len) == D1_OK &&
		      got_len == 0,
	      "a read at EOF returns nothing");

	/* The tail of a partial image is zeros, not the next chunk. */
	check(d1_view_read(view, CHUNK_BYTES + sizeof(data) - 1, got, 16,
			   &got_len) == D1_OK &&
		      got_len == 1 && got[0] == 0xf6,
	      "the last byte of the image is the image");

	d1_view_close(s, view);

	{
		struct d1_view *denied = NULL;

		check(d1_view_open(s, &object, writeonly, D1_SELECT_ORDINARY,
				   NULL, 0, 1, &denied) == D1_STALE_AUTH,
		      "an admission without READ cannot open a view");
		check(denied == NULL, "and gets no view");
	}

	d1_store_free(s);
}

int main(void)
{
	fill_uuid(&object.export_uuid, 0x30);
	fill_uuid(&object.object_uuid, 0x40);
	fill_uuid(&origin, 0x50);

	test_ordinary_lifecycle();
	test_activation();
	test_owner_collision();
	test_write_refusals();
	test_phase_order();
	test_private_rollback();
	test_sparse_and_rollback_extents();
	test_rollback_shrinks_eof();
	test_released_predecessor();
	test_view_is_stable();
	test_view_unpin_order();
	test_view_owner_selection();
	test_view_holes_and_admission();
	test_unsupported();

	if (failures) {
		fprintf(stderr, "%u check(s) failed\n", failures);
		return 1;
	}
	printf("d1_store_test: all checks passed\n");
	return 0;
}
