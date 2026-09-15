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

/*
 * Repair, error marking and unlocking are not this slice's.  They are
 * refused twice over: the canonical form cannot express them at all,
 * and the reducer refuses them even when handed one directly.
 */
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

	{
		static const uint32_t unimplemented[] = {
			D1_OP_MARK_ERROR,     D1_OP_BEGIN_REPAIR,
			D1_OP_PREPARE_REPAIR, D1_OP_FINALIZE_REPAIR,
			D1_OP_COMMIT_REPAIR,  D1_OP_ABORT_REPAIR,
			D1_OP_CLEAR_ERROR,    D1_OP_UNLOCK,
		};
		uint8_t buf[512];
		unsigned int i;

		for (i = 0; i < sizeof(unimplemented) / sizeof(*unimplemented);
		     i++) {
			env_init(&env, s, admission, unimplemented[i]);
			check(d1_envelope_encode(&env, buf, sizeof(buf)) == 0,
			      "the canonical form cannot express it");
			check(d1_store_apply(s, &env, &res) == D1_UNSUPPORTED,
			      "and the reducer says it does not implement it");
			check(res.count == 0, "and answers no entries");
			check(d1_store_eof(s, &object) == 0,
			      "and mutates nothing");
		}
	}

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

/* Do the model's state agree, as far as anything can observe it? */
static bool states_agree(const struct d1_store *a, const struct d1_store *b)
{
	struct d1_interval ha[D1_MAX_INTERVALS], hb[D1_MAX_INTERVALS];
	uint32_t na, nb, i;

	if (d1_store_eof(a, &object) != d1_store_eof(b, &object))
		return false;
	na = d1_store_holes(a, &object, ha, D1_MAX_INTERVALS);
	nb = d1_store_holes(b, &object, hb, D1_MAX_INTERVALS);
	if (na != nb)
		return false;
	for (i = 0; i < na; i++)
		if (ha[i].start != hb[i].start || ha[i].end != hb[i].end)
			return false;
	for (i = 0; i < D1_MAX_CHUNKS; i++) {
		d1_id_t va = 0, vb = 0;
		bool pa, pb;
		struct d1_guard ga, gb;

		pa = d1_store_visible(a, &object, i, &va);
		pb = d1_store_visible(b, &object, i, &vb);
		if (pa != pb || (pa && va != vb))
			return false;
		if (d1_store_guard(a, &object, i, &ga) !=
		    d1_store_guard(b, &object, i, &gb))
			return false;
		if (ga.never_written != gb.never_written ||
		    ga.generation != gb.generation || ga.writer != gb.writer)
			return false;
	}
	return true;
}

/* A short history, written the same way twice. */
static d1_id_t drive_history(struct d1_store *s, d1_id_t admission)
{
	static uint8_t small[100];
	static uint8_t tiny[50];
	static uint8_t big[512];
	struct d1_guard guard;
	d1_id_t v3, txn4, custody;

	memset(small, 0x21, sizeof(small));
	memset(tiny, 0x22, sizeof(tiny));
	memset(big, 0x23, sizeof(big));

	v3 = commit_chunk(s, admission, 3, 1, small, sizeof(small),
			  &(struct d1_guard){ .never_written = true }, 0, NULL);
	if (!v3)
		return 0;
	if (!commit_chunk(s, admission, 5, 2, tiny, sizeof(tiny),
			  &(struct d1_guard){ .never_written = true }, 0, NULL))
		return 0;
	d1_store_guard(s, &object, 3, &guard);
	if (!commit_chunk(s, admission, 3, 3, big, sizeof(big), &guard, v3,
			  &txn4))
		return 0;
	custody = d1_fixture_custody(s, 0);
	/* Custody is issued over whatever is visible on chunk 3 now. */
	{
		d1_id_t visible = 0;

		d1_store_visible(s, &object, 3, &visible);
		custody = d1_fixture_custody(s, visible);
	}
	if (rollback_one(s, admission, 3, 3, txn4, true, custody, NULL) !=
	    D1_OK)
		return 0;
	return v3;
}

/* H: the log rebuilds the store that wrote it. */
static void test_replay_reproduces_the_store(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *a, *b;
	const uint8_t *log;
	size_t len;
	d1_id_t admission;

	fill_uuid(&store_uuid, 0x88);
	a = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!a)
		return;
	check(d1_store_journal_enable(a) == D1_OK, "journalling starts");
	admission = d1_fixture_admit(a, &object, 11,
				     D1_RIGHT_READ | D1_RIGHT_WRITE |
					     D1_RIGHT_REPAIR |
					     D1_RIGHT_SINGLE_WRITER);
	check(drive_history(a, admission) != 0, "the history is written");
	check(d1_store_checkpoint(a) == D1_OK, "a checkpoint is written");

	log = d1_store_journal(a, &len);
	check(len > 0, "the log has bytes");

	b = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!b) {
		d1_store_free(a);
		return;
	}
	check(d1_store_replay(b, log, len) == D1_OK,
	      "the log replays without diverging");
	check(states_agree(a, b), "and rebuilds the same store");

	/* Recovery leaves no journal of its own and arms no faults. */
	{
		size_t blen = 1;

		(void)d1_store_journal(b, &blen);
		check(blen == 0, "a replayed store has written no log");
	}

	d1_store_free(b);
	d1_store_free(a);
}

/* H: a crash mid-record loses that record and nothing else. */
static void test_crash_loses_only_the_torn_record(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *a, *before, *after;
	const uint8_t *log;
	size_t len, prefix;
	d1_id_t admission, admission_b;
	static uint8_t data[64];

	memset(data, 0x31, sizeof(data));
	fill_uuid(&store_uuid, 0x99);
	a = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!a)
		return;
	d1_store_journal_enable(a);
	admission = d1_fixture_admit(a, &object, 11,
				     D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);

	check(commit_chunk(a, admission, 0, 1, data, sizeof(data),
			   &(struct d1_guard){ .never_written = true }, 0,
			   NULL) != 0,
	      "the first chunk commits");
	(void)d1_store_journal(a, &prefix);

	check(commit_chunk(a, admission, 2, 2, data, sizeof(data),
			   &(struct d1_guard){ .never_written = true }, 0,
			   NULL) != 0,
	      "the second chunk commits");
	log = d1_store_journal(a, &len);
	check(len > prefix, "and the log grew");

	/*
	 * A store built from only the first chunk is what a reader must
	 * see when the rest of the log did not survive.
	 */
	before = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!before) {
		d1_store_free(a);
		return;
	}
	d1_store_journal_enable(before);
	admission_b = d1_fixture_admit(before, &object, 11,
				       D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	commit_chunk(before, admission_b, 0, 1, data, sizeof(data),
		     &(struct d1_guard){ .never_written = true }, 0, NULL);

	after = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!after) {
		d1_store_free(before);
		d1_store_free(a);
		return;
	}
	/* Cut at a record boundary: everything after it is simply absent. */
	check(d1_store_replay(after, log, prefix) == D1_OK,
	      "a log cut at a record boundary replays");
	check(states_agree(before, after),
	      "and stops exactly where the writing stopped");

	/*
	 * Cutting one byte short tears only the final record.  The frontier
	 * is per record, not per operation: the write and finalize that
	 * preceded that commit did survive, so the chunk is not visible but
	 * its guard has moved.
	 */
	{
		struct d1_store *torn =
			d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
		struct d1_guard guard;
		d1_id_t seen;

		if (torn) {
			check(d1_store_replay(torn, log, len - 1) == D1_OK,
			      "a torn log still replays");
			check(!d1_store_visible(torn, &object, 2, &seen),
			      "the torn record never published its chunk");
			check(d1_store_guard(torn, &object, 2, &guard) &&
				      !guard.never_written,
			      "but the records before it did happen");
			d1_store_free(torn);
		}
	}

	/* The whole log gets the whole store. */
	{
		struct d1_store *whole =
			d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);

		if (whole) {
			check(d1_store_replay(whole, log, len) == D1_OK,
			      "the untruncated log replays");
			check(states_agree(a, whole),
			      "and rebuilds everything");
			d1_store_free(whole);
		}
	}

	d1_store_free(after);
	d1_store_free(before);
	d1_store_free(a);
}

/* I: a refused append publishes nothing and records nothing. */
static void test_append_fault_is_unrecorded(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_envelope env;
	struct d1_result res;
	struct d1_guard guard;
	static uint8_t data[32];
	size_t before, after;
	d1_id_t admission, seen;

	memset(data, 0x41, sizeof(data));
	fill_uuid(&store_uuid, 0xaa);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	d1_store_journal_enable(s);
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	(void)d1_store_journal(s, &before);

	d1_fixture_fail_next_append(s);
	env_init(&env, s, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 0, 11, 1, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	check(d1_store_apply(s, &env, &res) == D1_IO,
	      "an append that cannot happen fails the operation");
	check(res.disposition == D1_UNRECORDED,
	      "and the operation is UNRECORDED");
	check(res.count == 0, "and answers no entries");
	(void)d1_store_journal(s, &after);
	check(after == before, "the log did not grow");
	check(!d1_store_guard(s, &object, 0, &guard),
	      "the object was never even created");
	check(!d1_store_visible(s, &object, 0, &seen), "nothing is visible");
	check(d1_store_eof(s, &object) == 0, "and there is nothing to read");

	/* The fault is spent; the next attempt is ordinary. */
	env_init(&env, s, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 0, 11, 2, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	check(d1_store_apply(s, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_OK,
	      "the next write is admitted");
	(void)d1_store_journal(s, &after);
	check(after > before, "and the log grew this time");
	check(d1_store_guard(s, &object, 0, &guard) && !guard.never_written,
	      "and the chunk now has a guard");

	d1_store_free(s);
}

/* A log only rebuilds the store it was written for. */
static void test_replay_refuses_a_foreign_log(void)
{
	struct d1_uuid mine, theirs;
	struct d1_store *a, *b, *narrow;
	const uint8_t *log;
	size_t len;
	d1_id_t admission;
	static uint8_t data[16];

	memset(data, 0x51, sizeof(data));
	fill_uuid(&mine, 0xbb);
	fill_uuid(&theirs, 0xcc);
	a = d1_store_open(&mine, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!a)
		return;
	d1_store_journal_enable(a);
	admission = d1_fixture_admit(a, &object, 11,
				     D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	commit_chunk(a, admission, 0, 1, data, sizeof(data),
		     &(struct d1_guard){ .never_written = true }, 0, NULL);
	log = d1_store_journal(a, &len);

	b = d1_store_open(&theirs, CHUNK_BYTES, MAX_FILE_BYTES);
	if (b) {
		check(d1_store_replay(b, log, len) == D1_INVALID,
		      "a log from another store is refused");
		d1_store_free(b);
	}

	narrow = d1_store_open(&mine, CHUNK_BYTES / 2, MAX_FILE_BYTES);
	if (narrow) {
		check(d1_store_replay(narrow, log, len) == D1_INVALID,
		      "and so is a store with a different geometry");
		d1_store_free(narrow);
	}

	/* A log with no START record is not a log. */
	{
		struct d1_store *fresh =
			d1_store_open(&mine, CHUNK_BYTES, MAX_FILE_BYTES);

		if (fresh) {
			check(d1_store_replay(fresh, log, 0) == D1_INVALID,
			      "an empty log rebuilds nothing");
			d1_store_free(fresh);
		}
	}

	d1_store_free(a);
}

/* recovery_admit: one admission takes over from another. */
static void test_recovery_admit(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_envelope env;
	struct d1_result res;
	uint8_t verifier[D1_VERIFIER_BYTES];
	static uint8_t data[16];
	d1_id_t control, old, fresh;

	memset(data, 0x61, sizeof(data));
	fill_uuid(&store_uuid, 0xdd);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	control = d1_fixture_admit(s, &object, 11, D1_RIGHT_CONTROL);
	old = d1_fixture_admit(s, &object, 11,
			       D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	fresh = d1_fixture_admit(s, &object, 11,
				 D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	d1_store_verifier(s, verifier);

	/* The old admission works until it is superseded. */
	env_init(&env, s, old, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 0, 11, 1, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	d1_store_apply(s, &env, &res);
	check(res.entries[0].status == D1_OK, "the old admission can write");

	env_init(&env, s, control, D1_OP_RECOVERY_ADMIT);
	env.body.control.old_admission = old;
	env.body.control.new_admission_present = true;
	env.body.control.new_admission = fresh;
	check(d1_store_apply(s, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_OK,
	      "recovery admits the new admission");
	check(res.count == 1, "and answers once for the whole operation");
	check(memcmp(res.entries[0].verifier, verifier, sizeof(verifier)) == 0,
	      "and returns the verifier as it now stands");

	/* The superseded admission is finished. */
	env_init(&env, s, old, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 1, 11, 2, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	d1_store_apply(s, &env, &res);
	check(res.entries[0].status == D1_STALE_AUTH,
	      "the superseded admission cannot write");
	check(res.entries[0].disposition == D1_UNRECORDED,
	      "and leaves no receipt behind");

	/* The new one can. */
	env_init(&env, s, fresh, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 1, 11, 3, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	d1_store_apply(s, &env, &res);
	check(res.entries[0].status == D1_OK, "the new admission can");

	/* A read epoch the store has never reached is not a recovery. */
	env_init(&env, s, control, D1_OP_RECOVERY_ADMIT);
	env.body.control.old_admission = fresh;
	env.body.control.read_epoch_present = true;
	env.body.control.read_epoch = 1000000;
	check(d1_store_apply(s, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_INVALID,
	      "an epoch from the future is refused");

	/* An admission nobody issued cannot be recovered from. */
	env_init(&env, s, control, D1_OP_RECOVERY_ADMIT);
	env.body.control.old_admission = 9999;
	check(d1_store_apply(s, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_STALE_AUTH,
	      "an unknown admission is refused");

	d1_store_free(s);
}

/* lease_reap: cancelling the work of an owner that is gone. */
static void test_lease_reap(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_envelope env;
	struct d1_result res;
	struct d1_guard before, after;
	static uint8_t data[24];
	d1_id_t control, writer, other, txn_a, txn_b, committed, visible;

	memset(data, 0x71, sizeof(data));
	fill_uuid(&store_uuid, 0xee);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	control = d1_fixture_admit(s, &object, 11, D1_RIGHT_CONTROL);
	writer = d1_fixture_admit(s, &object, 11,
				  D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	other = d1_fixture_admit(s, &object, 12, D1_RIGHT_WRITE);

	/* Two prepared transactions on different chunks. */
	env_init(&env, s, writer, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 0, 11, 1, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	d1_store_apply(s, &env, &res);
	txn_a = res.entries[0].txn;
	env_init(&env, s, writer, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 1, 11, 2, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	d1_store_apply(s, &env, &res);
	txn_b = res.entries[0].txn;
	check(txn_a != 0 && txn_b != 0, "two transactions are prepared");
	d1_store_guard(s, &object, 0, &before);

	/* One committed transaction, which a reap may not touch. */
	committed = 0;
	check(commit_chunk(s, writer, 4, 3, data, sizeof(data),
			   &(struct d1_guard){ .never_written = true }, 0,
			   &committed) != 0,
	      "a third chunk is committed");

	/*
	 * A reap naming one member it may not touch cancels none of them:
	 * every member is validated before any is cancelled.
	 */
	env_init(&env, s, control, D1_OP_LEASE_REAP);
	env.body.control.old_admission = writer;
	env.body.control.count = 2;
	env.body.control.txns[0] = txn_a;
	env.body.control.txns[1] = committed;
	check(d1_store_apply(s, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_BAD_PHASE,
	      "a committed member is refused");
	check(d1_store_visible(s, &object, 4, &visible),
	      "and the committed chunk is untouched");

	env_init(&env, s, control, D1_OP_LEASE_REAP);
	env.body.control.old_admission = writer;
	env.body.control.count = 1;
	env.body.control.txns[0] = txn_a;
	/* The first chunk's transaction is still there to reap. */
	check(d1_store_apply(s, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_OK,
	      "the surviving member was not cancelled by the refusal");
	check(res.entries[0].phase == D1_PHASE_ROLLED_BACK,
	      "the reaped transaction is ROLLED_BACK");

	/* The chunk is free again, at the next generation, not the old one. */
	check(d1_store_guard(s, &object, 0, &after) &&
		      after.generation == before.generation,
	      "the reap did not move the guard back");
	env_init(&env, s, writer, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 0, 11, 4, data, sizeof(data),
		    true, &after);
	d1_store_apply(s, &env, &res);
	check(res.entries[0].status == D1_OK,
	      "and the chunk takes a new transaction");

	/* A reap may only cancel what its own admission prepared. */
	env_init(&env, s, control, D1_OP_LEASE_REAP);
	env.body.control.old_admission = other;
	env.body.control.count = 1;
	env.body.control.txns[0] = txn_b;
	check(d1_store_apply(s, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_OWNER_CONFLICT,
	      "another admission's transaction is not this reap's to cancel");

	/* Reaping needs the control right, not the write right. */
	env_init(&env, s, writer, D1_OP_LEASE_REAP);
	env.body.control.old_admission = writer;
	env.body.control.count = 1;
	env.body.control.txns[0] = txn_b;
	check(d1_store_apply(s, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_STALE_AUTH,
	      "a writer cannot reap");

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
	test_replay_reproduces_the_store();
	test_crash_loses_only_the_torn_record();
	test_append_fault_is_unrecorded();
	test_replay_refuses_a_foreign_log();
	test_recovery_admit();
	test_lease_reap();
	test_unsupported();

	if (failures) {
		fprintf(stderr, "%u check(s) failed\n", failures);
		return 1;
	}
	printf("d1_store_test: all checks passed\n");
	return 0;
}
