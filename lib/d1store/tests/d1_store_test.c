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

	/*
	 * An index outside the named range makes the request malformed, so
	 * it is refused for the whole operation before any member is
	 * evaluated -- section 2's "wrong vector returns INVALID before
	 * member mutation".
	 */
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
	check(d1_store_apply(s, &env, &res) == D1_INVALID,
	      "a member outside the named range is refused");
	check(res.count == 0, "and no member result is produced");
	check(!d1_store_visible(s, &object, 0, &visible),
	      "and nothing was published");

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

	/*
	 * E4: logical release asks a question about durable state.  A live
	 * pin keeps the immutable bytes alive for the view that holds it;
	 * it does not decide whether a future rollback may reach the
	 * version.  Making release depend on pins would make the durable
	 * history depend on when a reader happened to close.
	 */
	check(d1_fixture_release_predecessor(s, v1),
	      "an eligible predecessor releases while a view pins it");
	check(d1_view_read(view, 0, got, sizeof(got), &got_len) == D1_OK &&
		      memcmp(got, first, sizeof(first)) == 0,
	      "and the pinned view still reads its own bytes");
	d1_view_close(s, view);

	/* A view opened after the commit sees the new version. */
	check(d1_view_open(s, &object, admission, D1_SELECT_ORDINARY, NULL, 0,
			   1, &after) == D1_OK,
	      "a later view opens");
	check(d1_view_version(after, 0, &seen) && seen == v2,
	      "and names the newer version");
	d1_view_close(s, after);

	d1_store_free(s);
}

/*
 * E5: release-before-unpin and unpin-before-release are the same
 * history.  Two stores run the same operations in the two orders and
 * must agree on the release disposition and on what a later rollback
 * finds.
 */
static void test_release_order_does_not_matter(void)
{
	static const bool release_first[] = { true, false };
	unsigned int pass;
	bool released[2] = { false, false };
	uint32_t rollback_status[2] = { 0, 0 };

	for (pass = 0; pass < 2; pass++) {
		struct d1_uuid store_uuid;
		struct d1_store *s;
		struct d1_view *view = NULL;
		struct d1_guard guard;
		static uint8_t data[32];
		d1_id_t admission, v1, v2, txn2, custody;

		memset(data, 0xc3, sizeof(data));
		fill_uuid(&store_uuid, (uint8_t)(0x55 + pass));
		s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
		if (!s)
			return;
		admission = d1_fixture_admit(s, &object, 11,
					     D1_RIGHT_READ | D1_RIGHT_WRITE |
						     D1_RIGHT_REPAIR |
						     D1_RIGHT_SINGLE_WRITER);

		v1 = commit_chunk(s, admission, 0, 1, data, sizeof(data),
				  &(struct d1_guard){ .never_written = true },
				  0, NULL);
		check(d1_view_open(s, &object, admission, D1_SELECT_ORDINARY,
				   NULL, 0, 1, &view) == D1_OK,
		      "a view pins the version");
		d1_store_guard(s, &object, 0, &guard);
		v2 = commit_chunk(s, admission, 0, 2, data, sizeof(data),
				  &guard, v1, &txn2);
		check(v1 != 0 && v2 != 0, "a new version displaces it");

		if (release_first[pass]) {
			released[pass] = d1_fixture_release_predecessor(s, v1);
			d1_view_close(s, view);
		} else {
			d1_view_close(s, view);
			released[pass] = d1_fixture_release_predecessor(s, v1);
		}

		custody = d1_fixture_custody(s, v2);
		rollback_status[pass] = rollback_one(s, admission, 0, 2, txn2,
						     true, custody, NULL);
		d1_store_free(s);
	}

	check(released[0] && released[1],
	      "the release succeeds in either order");
	check(rollback_status[0] == rollback_status[1],
	      "and a later rollback finds the same thing either way");
	check(rollback_status[0] == D1_NO_PREDECESSOR,
	      "which is that the released predecessor is gone");
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
static bool states_agree(struct d1_store *a, struct d1_store *b)
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
	/*
	 * A shorter durable length is a crash: the writer had claimed only
	 * that much, so everything after it is simply absent.
	 */
	check(d1_store_replay(after, log, prefix) == D1_OK,
	      "a log replayed at an earlier durable length replays");
	check(states_agree(before, after),
	      "and stops exactly where the writing stopped");

	/*
	 * A durable length falling inside a record is not a torn suffix.
	 * The writer claimed those bytes, so it is corruption of the
	 * durable prefix and must fail closed rather than quietly drop the
	 * record it lands in.
	 */
	{
		struct d1_store *torn =
			d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);

		if (torn) {
			check(d1_store_replay(torn, log, len - 1) == D1_IO,
			      "a durable length inside a record fails closed");
			d1_store_free(torn);
		}
	}

	/* And corruption of a completed record is never a clean end. */
	{
		struct d1_store *bad =
			d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
		uint8_t *writable = (uint8_t *)(uintptr_t)log;

		if (bad) {
			writable[len - 1] ^= 0xffu;
			check(d1_store_replay(bad, log, len) == D1_IO,
			      "a corrupt final record fails closed");
			writable[len - 1] ^= 0xffu;
			d1_store_free(bad);
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

/*
 * I1: a refused append publishes nothing and records nothing.
 *
 * Disposition is per entry for an ordinary batch, so the operation
 * succeeds and the entry says UNRECORDED.  What must be true is that
 * nothing happened: no transition, no consumed ID, no receipt, and a
 * log that did not grow.
 */
static void test_append_fault_is_unrecorded(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_envelope env;
	struct d1_result res;
	struct d1_guard guard;
	static uint8_t data[32];
	size_t before, after;
	d1_id_t admission, seen, first_txn;

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
	check(d1_store_apply(s, &env, &res) == D1_OK,
	      "the operation itself completes");
	check(res.entries[0].status == D1_IO,
	      "the entry reports the failure to record");
	check(res.entries[0].disposition == D1_UNRECORDED, "and is UNRECORDED");
	(void)d1_store_journal(s, &after);
	check(after == before, "the log did not grow");
	check(!d1_store_guard(s, &object, 0, &guard),
	      "the object was never even created");
	check(!d1_store_visible(s, &object, 0, &seen), "nothing is visible");
	check(d1_store_eof(s, &object) == 0, "and there is nothing to read");

	/*
	 * No receipt was recorded, so the exact same request may be tried
	 * again and execute.  Demanding a replay conflict for a key that
	 * was never recorded would be wrong.
	 */
	check(d1_store_apply(s, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_OK,
	      "the exact retry executes");
	first_txn = res.entries[0].txn;
	check(first_txn == 1,
	      "and consumes the first transaction ID, not the second");
	(void)d1_store_journal(s, &after);
	check(after > before, "and the log grew this time");
	check(d1_store_guard(s, &object, 0, &guard) && !guard.never_written,
	      "and the chunk now has a guard");

	/* A flush that does not happen is the same kind of nothing. */
	d1_fixture_fail_next_flush(s);
	(void)d1_store_journal(s, &before);
	env_init(&env, s, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 1, 11, 2, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	d1_store_apply(s, &env, &res);
	check(res.entries[0].disposition == D1_UNRECORDED,
	      "an unflushed event is UNRECORDED too");
	(void)d1_store_journal(s, &after);
	check(after == before, "and claims no new durable bytes");
	check(d1_store_guard(s, &object, 1, &guard) && guard.never_written,
	      "and left the chunk alone");

	d1_store_free(s);
}

/*
 * An inability to reserve a receipt is UNRECORDED with no state change.
 * The old behaviour published the write and then told the caller
 * nothing had been recorded, which no retry could repair.
 */
static void test_receipt_exhaustion_changes_nothing(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_envelope env;
	struct d1_result res;
	struct d1_guard guard;
	static uint8_t data[8];
	unsigned int i;
	d1_id_t admission, seen;
	bool filled = false;

	memset(data, 0x42, sizeof(data));
	fill_uuid(&store_uuid, 0xab);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);

	/* Fill the receipt table with recorded semantic errors. */
	for (i = 0; i < D1_MAX_RECEIPTS + 2u; i++) {
		env_init(&env, s, admission, D1_OP_WRITE_BATCH);
		env.body.write.count = 1;
		env.body.write.stability = D1_FILE_SYNC;
		write_entry(&env.body.write.entries[0], 0, 11, i + 1u, data,
			    sizeof(data), true,
			    &(struct d1_guard){ .generation = 900u + i,
						.writer = 11 });
		d1_store_apply(s, &env, &res);
		if (res.entries[0].status == D1_NOSPC) {
			filled = true;
			break;
		}
		if (res.entries[0].status != D1_GUARDED)
			break;
	}
	check(filled, "the receipt table fills with recorded errors");
	check(res.entries[0].disposition == D1_UNRECORDED,
	      "and the next request is UNRECORDED");

	/* Now a request that would otherwise succeed must change nothing. */
	env_init(&env, s, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_DATA_SYNC;
	env.body.write.activate = true;
	write_entry(&env.body.write.entries[0], 2, 11, 5000, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	d1_store_apply(s, &env, &res);
	check(res.entries[0].status == D1_NOSPC &&
		      res.entries[0].disposition == D1_UNRECORDED,
	      "a valid write with no receipt room is UNRECORDED");
	check(!res.entries[0].txn_present && !res.entries[0].version_present,
	      "and reserved no transaction or version");
	check(!d1_store_visible(s, &object, 2, &seen), "and published nothing");
	check(!d1_store_guard(s, &object, 2, &guard) || guard.never_written,
	      "and left the guard where it was");
	check(d1_store_eof(s, &object) == 0, "and did not move EOF");

	d1_store_free(s);
}

/*
 * H1: an exact retry returns its recorded receipt even after the
 * admission that made it is gone.  Retrieving a recorded result is not
 * a new mutation, so it is not gated on current authority.
 */
static void test_exact_retry_survives_revocation(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_envelope env, extend;
	struct d1_result first, again;
	static uint8_t data[8];
	d1_id_t admission, second;

	memset(data, 0x43, sizeof(data));
	fill_uuid(&store_uuid, 0xac);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	second = d1_fixture_admit(s, &object, 11,
				  D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);

	env_init(&env, s, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_DATA_SYNC;
	env.body.write.activate = true;
	write_entry(&env.body.write.entries[0], 0, 11, 1, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	check(d1_store_apply(s, &env, &first) == D1_OK &&
		      first.entries[0].status == D1_OK &&
		      first.entries[0].activated,
	      "the write activates");

	/* Something else moves EOF and the index epoch afterwards. */
	env_init(&extend, s, second, D1_OP_WRITE_BATCH);
	extend.body.write.count = 1;
	extend.body.write.stability = D1_DATA_SYNC;
	extend.body.write.activate = true;
	write_entry(&extend.body.write.entries[0], 4, 11, 2, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	d1_store_apply(s, &extend, &again);
	check(d1_store_eof(s, &object) > first.eof, "EOF has moved on");

	d1_fixture_revoke(s, admission);

	check(d1_store_apply(s, &env, &again) == D1_OK,
	      "the exact retry is answered");
	check(again.entries[0].status == D1_OK,
	      "with the recorded success, not a stale-authority error");
	check(again.entries[0].disposition == D1_COMPLETED,
	      "recorded, not unrecorded");
	check(again.entries[0].version == first.entries[0].version &&
		      again.entries[0].txn == first.entries[0].txn,
	      "naming the same version and transaction");
	check(again.eof == first.eof && again.index_epoch == first.index_epoch,
	      "and the EOF and epoch it saw, not today's");

	/* A different body under the same key is still a conflict. */
	{
		struct d1_envelope changed = env;
		struct d1_result conflict;

		changed.body.write.activate = false;
		check(d1_store_apply(s, &changed, &conflict) == D1_OK &&
			      conflict.entries[0].status == D1_REPLAY_CONFLICT,
		      "a changed body under the used key conflicts");
	}

	d1_store_free(s);
}

/*
 * H1: an actual reopen is not a read-only rebuild.  It opens a new
 * incarnation, fences the old handles, and can be done again.
 */
static void test_reopen_fences_and_repeats(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *a, *b, *c;
	struct d1_envelope env;
	struct d1_result res;
	const uint8_t *log;
	size_t len;
	static uint8_t data[8];
	d1_id_t admission, again, txn;
	uint8_t verifier_before[D1_VERIFIER_BYTES];
	uint8_t verifier_after[D1_VERIFIER_BYTES];

	memset(data, 0x44, sizeof(data));
	fill_uuid(&store_uuid, 0xad);
	a = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!a)
		return;
	d1_store_journal_enable(a);
	admission = d1_fixture_admit(a, &object, 11,
				     D1_RIGHT_WRITE | D1_RIGHT_CONTROL |
					     D1_RIGHT_SINGLE_WRITER);
	env_init(&env, a, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 0, 11, 1, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	d1_store_apply(a, &env, &res);
	txn = res.entries[0].txn;
	check(txn != 0, "there is pending work to recover");
	d1_store_verifier(a, verifier_before);
	log = d1_store_journal(a, &len);

	b = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!b) {
		d1_store_free(a);
		return;
	}
	check(d1_store_reopen(b, log, len) == D1_OK, "the store reopens");
	check(d1_store_incarnation(b) == d1_store_incarnation(a) + 1u,
	      "in a new incarnation");
	d1_store_verifier(b, verifier_after);
	check(memcmp(verifier_before, verifier_after,
		     sizeof(verifier_before)) != 0,
	      "with a new verifier");

	/* The old handle is fenced: its work needs re-admission. */
	env_init(&env, b, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 1, 11, 2, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	d1_store_apply(b, &env, &res);
	check(res.entries[0].status == D1_STALE_AUTH,
	      "an admission from before the reopen cannot mutate");

	/* A fresh handle in the new incarnation can. */
	again = d1_fixture_admit(b, &object, 11,
				 D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	check(again != 0 && again != admission,
	      "a new admission gets a new ID, never a reused one");
	env_init(&env, b, again, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 1, 11, 3, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	d1_store_apply(b, &env, &res);
	check(res.entries[0].status == D1_OK,
	      "and a handle from this incarnation can");

	/* Reopening again is ordinary. */
	log = d1_store_journal(b, &len);
	c = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (c) {
		check(d1_store_reopen(c, log, len) == D1_OK,
		      "the store reopens a second time");
		check(d1_store_incarnation(c) == d1_store_incarnation(b) + 1u,
		      "into a third incarnation");
		d1_store_free(c);
	}

	d1_store_free(b);
	d1_store_free(a);
}

/*
 * The outcomes that depend on fixture state must replay.
 *
 * A rollback's answer depends on which version the custody handle is
 * bound to and on whether the predecessor has been released.  Both are
 * fixture state, and both are journalled, so a rebuild reaches the same
 * answer for the same reason.  Before they were journalled, a replay of
 * these two histories produced a different store from the one that
 * wrote the log.
 */
static void test_custody_and_release_replay(void)
{
	static const bool release_case[] = { false, true };
	unsigned int pass;

	for (pass = 0; pass < 2; pass++) {
		struct d1_uuid store_uuid;
		struct d1_store *live, *rebuilt;
		struct d1_guard guard;
		struct d1_entry_result entry;
		const uint8_t *log;
		size_t len;
		static uint8_t data[32];
		d1_id_t admission, v1, v2, txn2, custody, live_visible;
		d1_id_t rebuilt_visible;
		uint32_t status;

		memset(data, 0x51, sizeof(data));
		fill_uuid(&store_uuid, (uint8_t)(0xb1 + pass));
		live = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
		if (!live)
			return;
		/* Journalling first, so the authority history is in the log. */
		check(d1_store_journal_enable(live) == D1_OK,
		      "journalling starts before any authority is issued");
		admission = d1_fixture_admit(live, &object, 11,
					     D1_RIGHT_WRITE | D1_RIGHT_REPAIR |
						     D1_RIGHT_SINGLE_WRITER);

		v1 = commit_chunk(live, admission, 0, 1, data, sizeof(data),
				  &(struct d1_guard){ .never_written = true },
				  0, NULL);
		d1_store_guard(live, &object, 0, &guard);
		v2 = commit_chunk(live, admission, 0, 2, data, sizeof(data),
				  &guard, v1, &txn2);
		check(v1 != 0 && v2 != 0, "two versions commit in turn");

		if (release_case[pass]) {
			/* The predecessor is released, so it is not eligible. */
			check(d1_fixture_release_predecessor(live, v1),
			      "the displaced predecessor releases");
			custody = d1_fixture_custody(live, v2);
		} else {
			/* Custody bound to the wrong version is a conflict. */
			custody = d1_fixture_custody(live, v1);
		}
		status = rollback_one(live, admission, 0, 2, txn2, true,
				      custody, &entry);
		check(status == (release_case[pass] ? D1_NO_PREDECESSOR :
						      D1_OWNER_CONFLICT),
		      "the rollback answers from the fixture state");
		check(d1_store_visible(live, &object, 0, &live_visible) &&
			      live_visible == v2,
		      "and the current data stays where it is");

		log = d1_store_journal(live, &len);
		rebuilt =
			d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
		if (!rebuilt) {
			d1_store_free(live);
			return;
		}
		check(d1_store_replay(rebuilt, log, len) == D1_OK,
		      "the log replays without diverging");
		check(d1_store_visible(rebuilt, &object, 0, &rebuilt_visible) &&
			      rebuilt_visible == live_visible,
		      "and the rebuilt store sees the same version");
		check(states_agree(live, rebuilt),
		      "and agrees with the store that wrote it");

		d1_store_free(rebuilt);
		d1_store_free(live);
	}
}

/* A log only rebuilds the store it was written for. */
static void test_replay_refuses_a_foreign_log(void)
{
	struct d1_uuid mine, theirs;
	struct d1_store *a, *b;
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
		check(d1_store_replay(b, log, len) == D1_IO,
		      "a log from another store is refused");
		d1_store_free(b);
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

/*
 * recovery_admit: the work moves, not just the handle.
 *
 * The point of re-admission is that a caller which has been away can
 * carry on with the transactions it left behind.  Proving a new handle
 * can write a fresh chunk proves nothing about that.
 */
static void test_recovery_admit(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_envelope env;
	struct d1_result res;
	uint8_t verifier[D1_VERIFIER_BYTES];
	static uint8_t data[16];
	d1_id_t control, old, fresh, txn;

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

	/* Work left behind under the old admission. */
	env_init(&env, s, old, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 0, 11, 1, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	d1_store_apply(s, &env, &res);
	check(res.entries[0].status == D1_OK,
	      "the old admission prepares work");
	txn = res.entries[0].txn;

	env_init(&env, s, control, D1_OP_RECOVERY_ADMIT);
	env.body.control.count = 1;
	env.body.control.txns[0] = txn;
	env.body.control.old_admission = old;
	env.body.control.new_admission_present = true;
	env.body.control.new_admission = fresh;
	env.body.control.read_epoch_present = true;
	env.body.control.read_epoch = 0;
	check(d1_store_apply(s, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_OK,
	      "recovery re-admits the named transaction");
	check(res.count == 1, "and answers once for the whole operation");
	check(memcmp(res.entries[0].verifier, verifier, sizeof(verifier)) == 0,
	      "and returns the verifier as it now stands");

	/* The whole point: the new admission can finish the old work. */
	env_init(&env, s, fresh, D1_OP_FINALIZE_BATCH);
	env.body.lifecycle.range_begin = 0;
	env.body.lifecycle.range_end = 1;
	env.body.lifecycle.count = 1;
	env.body.lifecycle.entries[0].index = 0;
	env.body.lifecycle.entries[0].owner.cohort = 1;
	env.body.lifecycle.entries[0].owner.writer = 11;
	env.body.lifecycle.entries[0].owner.co_id = 1;
	env.body.lifecycle.entries[0].txn = txn;
	memcpy(env.body.lifecycle.prior_verifier, verifier, sizeof(verifier));
	check(d1_store_apply(s, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_OK &&
		      res.entries[0].phase == D1_PHASE_FINALIZED,
	      "the recovered admission finalizes the recovered transaction");

	/* The superseded handle is finished. */
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

	d1_store_free(s);
}

/* An invalid recovery request changes nothing, including the old handle. */
static void test_recovery_admit_is_atomic(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_envelope env;
	struct d1_result res;
	static uint8_t data[16];
	d1_id_t control, old, fresh, stranger, txn, other_txn;

	memset(data, 0x62, sizeof(data));
	fill_uuid(&store_uuid, 0xdf);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	control = d1_fixture_admit(s, &object, 11, D1_RIGHT_CONTROL);
	old = d1_fixture_admit(s, &object, 11,
			       D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	fresh = d1_fixture_admit(s, &object, 11,
				 D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	stranger = d1_fixture_admit(s, &object, 12, D1_RIGHT_WRITE);

	env_init(&env, s, old, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 0, 11, 1, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	d1_store_apply(s, &env, &res);
	txn = res.entries[0].txn;

	env_init(&env, s, stranger, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 1, 12, 2, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	d1_store_apply(s, &env, &res);
	other_txn = res.entries[0].txn;
	check(txn != 0 && other_txn != 0, "two admissions prepare work");

	/* A read epoch the store has never reached. */
	env_init(&env, s, control, D1_OP_RECOVERY_ADMIT);
	env.body.control.count = 1;
	env.body.control.txns[0] = txn;
	env.body.control.old_admission = old;
	env.body.control.new_admission_present = true;
	env.body.control.new_admission = fresh;
	env.body.control.read_epoch_present = true;
	env.body.control.read_epoch = 1000000;
	check(d1_store_apply(s, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_INVALID,
	      "an epoch from the future is refused");

	/* Naming somebody else's transaction. */
	env_init(&env, s, control, D1_OP_RECOVERY_ADMIT);
	env.body.control.count = 1;
	env.body.control.txns[0] = other_txn;
	env.body.control.old_admission = old;
	env.body.control.new_admission_present = true;
	env.body.control.new_admission = fresh;
	env.body.control.read_epoch_present = true;
	env.body.control.read_epoch = 0;
	check(d1_store_apply(s, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_OWNER_CONFLICT,
	      "a transaction of another admission is refused");

	/*
	 * Neither refusal may have revoked anything on the way to failing:
	 * the old admission still works.
	 */
	env_init(&env, s, old, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 2, 11, 3, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	d1_store_apply(s, &env, &res);
	check(res.entries[0].status == D1_OK,
	      "a refused recovery revoked nothing");

	/* An admission nobody issued cannot be recovered from. */
	env_init(&env, s, control, D1_OP_RECOVERY_ADMIT);
	env.body.control.count = 1;
	env.body.control.txns[0] = txn;
	env.body.control.old_admission = 9999;
	env.body.control.new_admission_present = true;
	env.body.control.new_admission = fresh;
	env.body.control.read_epoch_present = true;
	env.body.control.read_epoch = 0;
	check(d1_store_apply(s, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_STALE_AUTH,
	      "an unknown admission is refused");

	d1_store_free(s);
}

/* lease_reap: cancelling the work of an owner whose lease is gone. */
static void test_lease_reap(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_envelope env;
	struct d1_result res;
	struct d1_guard before, after;
	static uint8_t data[24];
	d1_id_t control, writer, other, txn_a, txn_b, visible;

	memset(data, 0x71, sizeof(data));
	fill_uuid(&store_uuid, 0xee);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	control = d1_fixture_admit(s, &object, 11, D1_RIGHT_CONTROL);
	writer = d1_fixture_admit(s, &object, 11,
				  D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	other = d1_fixture_admit(s, &object, 12, D1_RIGHT_WRITE);

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

	/* A live lease keeps its own work. */
	env_init(&env, s, control, D1_OP_LEASE_REAP);
	env.body.control.old_admission = writer;
	env.body.control.count = 1;
	env.body.control.txns[0] = txn_a;
	check(d1_store_apply(s, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_STALE_AUTH,
	      "a live admission's work is not reapable");

	d1_fixture_expire(s, writer);

	/* Reaping needs the control right, not the write right. */
	env_init(&env, s, writer, D1_OP_LEASE_REAP);
	env.body.control.old_admission = writer;
	env.body.control.count = 1;
	env.body.control.txns[0] = txn_a;
	check(d1_store_apply(s, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_STALE_AUTH,
	      "a writer cannot reap");

	/*
	 * Every named member is validated before any is cancelled, so a
	 * reap naming one member it may not touch changes nothing.
	 */
	env_init(&env, s, control, D1_OP_LEASE_REAP);
	env.body.control.old_admission = writer;
	env.body.control.count = 2;
	env.body.control.txns[0] = txn_a;
	env.body.control.txns[1] = 999999;
	check(d1_store_apply(s, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_INVALID,
	      "a member that is not a transaction refuses the whole reap");

	env_init(&env, s, control, D1_OP_LEASE_REAP);
	env.body.control.old_admission = writer;
	env.body.control.count = 1;
	env.body.control.txns[0] = txn_a;
	check(d1_store_apply(s, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_OK,
	      "the surviving member was not cancelled by the refusal");
	check(res.entries[0].phase == D1_PHASE_ROLLED_BACK,
	      "the reaped transaction is ROLLED_BACK");

	/* The guard does not move back when work is reaped. */
	check(d1_store_guard(s, &object, 0, &after) &&
		      after.generation == before.generation,
	      "the reap did not move the guard back");

	/* A reap may only cancel what its own admission prepared. */
	env_init(&env, s, control, D1_OP_LEASE_REAP);
	env.body.control.old_admission = other;
	env.body.control.count = 1;
	env.body.control.txns[0] = txn_b;
	check(d1_store_apply(s, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_STALE_AUTH,
	      "another live admission cannot be reaped either");

	d1_fixture_expire(s, other);
	env_init(&env, s, control, D1_OP_LEASE_REAP);
	env.body.control.old_admission = other;
	env.body.control.count = 1;
	env.body.control.txns[0] = txn_b;
	check(d1_store_apply(s, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_OWNER_CONFLICT,
	      "another admission's transaction is not this reap's to cancel");
	check(!d1_store_visible(s, &object, 1, &visible),
	      "and nothing was published for it");

	d1_store_free(s);
}

/*
 * Malformed typed requests.
 *
 * The store's only entry point takes a typed envelope, so the decoder's
 * bounds are not what protects it: every in-tree caller builds one by
 * hand.  A request whose counts, lengths or members are outside the
 * canonical form must be refused before anything reads a payload or
 * hashes a byte, and refused without reading past a fixed array.
 */
static void test_malformed_requests_are_refused(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_envelope env;
	struct d1_result res;
	static uint8_t data[16];
	d1_id_t admission, visible;

	memset(data, 0x81, sizeof(data));
	fill_uuid(&store_uuid, 0x12);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);

	/* A digest length past the fixed digest array. */
	env_init(&env, s, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 0, 11, 1, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	env.body.write.entries[0].checksum.len = 4096;
	check(d1_store_apply(s, &env, &res) == D1_INVALID,
	      "a digest length past its array is refused, not read");

	/* A count past the entry vector. */
	env_init(&env, s, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = D1_BATCH_ENTRIES_MAX + 1u;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 0, 11, 2, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	check(d1_store_apply(s, &env, &res) == D1_INVALID,
	      "a count past the entry vector is refused");

	/* An empty batch is not a batch. */
	env_init(&env, s, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 0;
	env.body.write.stability = D1_FILE_SYNC;
	check(d1_store_apply(s, &env, &res) == D1_INVALID,
	      "an empty batch is refused");

	/* A stability outside the three. */
	env_init(&env, s, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = 99;
	write_entry(&env.body.write.entries[0], 0, 11, 3, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	check(d1_store_apply(s, &env, &res) == D1_INVALID,
	      "a stability outside the three is refused");

	/* One batch naming one chunk twice. */
	env_init(&env, s, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 2;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 0, 11, 4, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	write_entry(&env.body.write.entries[1], 0, 11, 5, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	check(d1_store_apply(s, &env, &res) == D1_INVALID,
	      "a batch naming one chunk twice is refused");

	/* A reserved writer ID cannot own anything. */
	env_init(&env, s, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 0, 0, 6, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	check(d1_store_apply(s, &env, &res) == D1_INVALID,
	      "a reserved writer ID is refused");
	env_init(&env, s, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 0, 0xffffffffu, 7, data,
		    sizeof(data), true,
		    &(struct d1_guard){ .never_written = true });
	check(d1_store_apply(s, &env, &res) == D1_INVALID,
	      "and so is the other reserved writer ID");

	/* An unsupported checksum algorithm. */
	env_init(&env, s, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 0, 11, 8, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	env.body.write.entries[0].checksum.alg = 77;
	check(d1_store_apply(s, &env, &res) == D1_INVALID,
	      "an unsupported checksum algorithm is refused");

	/* A control request carrying a field its operation has no use for. */
	env_init(&env, s, admission, D1_OP_LEASE_REAP);
	env.body.control.count = 1;
	env.body.control.txns[0] = 1;
	env.body.control.old_admission = admission;
	env.body.control.new_admission_present = true;
	env.body.control.new_admission = 2;
	check(d1_store_apply(s, &env, &res) == D1_INVALID,
	      "a reap naming a new admission is a different request");

	/* A recovery request missing the epoch it exists to grant. */
	env_init(&env, s, admission, D1_OP_RECOVERY_ADMIT);
	env.body.control.count = 1;
	env.body.control.txns[0] = 1;
	env.body.control.old_admission = admission;
	env.body.control.new_admission_present = true;
	env.body.control.new_admission = 2;
	check(d1_store_apply(s, &env, &res) == D1_INVALID,
	      "a recovery granting no read epoch is refused");

	check(!d1_store_visible(s, &object, 0, &visible),
	      "and none of them wrote anything");
	check(d1_store_eof(s, &object) == 0, "nor moved EOF");

	d1_store_free(s);
}

int main(void)
{
	fill_uuid(&object.export_uuid, 0x30);
	fill_uuid(&object.object_uuid, 0x40);
	fill_uuid(&origin, 0x50);

	test_malformed_requests_are_refused();
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
	test_release_order_does_not_matter();
	test_view_owner_selection();
	test_view_holes_and_admission();
	test_replay_reproduces_the_store();
	test_crash_loses_only_the_torn_record();
	test_append_fault_is_unrecorded();
	test_receipt_exhaustion_changes_nothing();
	test_exact_retry_survives_revocation();
	test_reopen_fences_and_repeats();
	test_custody_and_release_replay();
	test_replay_refuses_a_foreign_log();
	test_recovery_admit();
	test_recovery_admit_is_atomic();
	test_lease_reap();
	test_unsupported();

	if (failures) {
		fprintf(stderr, "%u check(s) failed\n", failures);
		return 1;
	}
	printf("d1_store_test: all checks passed\n");
	return 0;
}
