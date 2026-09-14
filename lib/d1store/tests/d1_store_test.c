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

	env_init(&env, s, admission, D1_OP_ROLLBACK_BATCH);
	env.body.rollback.count = 1;
	check(d1_store_apply(s, &env, &res) == D1_UNSUPPORTED,
	      "an unimplemented operation says so");
	check(res.count == 0, "and answers no entries");

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
	test_unsupported();

	if (failures) {
		fprintf(stderr, "%u check(s) failed\n", failures);
		return 1;
	}
	printf("d1_store_test: all checks passed\n");
	return 0;
}
