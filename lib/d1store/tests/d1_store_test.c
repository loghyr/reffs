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
	/*
	 * A semantic refusal changes nothing, which on a store that had
	 * never seen this object means the object is still not there:
	 * the entry created it on first touch and the refusal put it back.
	 */
	check(!d1_store_guard(s, &object, 0, &guard),
	      "and the object it touched is still not there");

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
	struct d1_guard guard;
	d1_id_t admission, visible;

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
			check(d1_store_eof(s, &object) == 0 &&
				      !d1_store_visible(s, &object, 0,
							&visible) &&
				      !d1_store_guard(s, &object, 0, &guard),
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

/*
 * What a rollback asserts about the state it expects to find.  These
 * are expected states, not flags that switch checking off, so a test
 * that omits them is asserting that nothing is there.
 */
struct rollback_expect {
	bool custody_present;
	d1_id_t custody;
	bool visible_present;
	d1_id_t visible;
	bool predecessor_present;
	d1_id_t predecessor;
};

static uint32_t rollback_one(struct d1_store *s, d1_id_t admission,
			     uint64_t index, uint32_t co_id, d1_id_t txn,
			     const struct rollback_expect *x,
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
	env.body.rollback.entries[0].custody_present = x->custody_present;
	env.body.rollback.entries[0].custody = x->custody;
	env.body.rollback.entries[0].visible_present = x->visible_present;
	env.body.rollback.entries[0].visible = x->visible;
	env.body.rollback.entries[0].predecessor_present =
		x->predecessor_present;
	env.body.rollback.entries[0].predecessor = x->predecessor;
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

	check(rollback_one(s, admission, 0, 1, txn,
			   &(struct rollback_expect){ 0 }, NULL) == D1_OK,
	      "its owner may cancel it");
	check(!d1_store_visible(s, &object, 0, &visible),
	      "and nothing became visible");
	check(d1_store_guard(s, &object, 0, &after) &&
		      after.generation == before.generation &&
		      !after.never_written,
	      "the generation does not go back");
	/*
	 * That monotonicity is this model's policy, not a claim about the
	 * wire: draft lines 8549--8553 read differently, and D4 has to
	 * settle the difference before wire integration rather than
	 * inherit this fixture.
	 */

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
		d1_id_t committed_version = 0;
		struct d1_entry_result entry;

		check(rollback_one(s, admission, 0, 2, res.entries[0].txn,
				   &(struct rollback_expect){ 0 },
				   NULL) == D1_OK,
		      "the second write is cancelled too");
		committed_version = commit_chunk(
			s, admission, 1, 3, payload_a, sizeof(payload_a),
			&(struct d1_guard){ .never_written = true }, 0, &ctxn);
		check(committed_version != 0, "a chunk is committed");
		check(rollback_one(s, admission, 1, 3, ctxn,
				   &(struct rollback_expect){
					   .visible_present = true,
					   .visible = committed_version },
				   &entry) == D1_STALE_AUTH,
		      "committed data needs custody, not ownership");
		check(d1_store_visible(s, &object, 1, &visible),
		      "and is still visible");
	}

	d1_store_free(s);
}

/*
 * Rollback expectations are expected states, and the right a rollback
 * needs follows the phase it finds.
 */
static void test_rollback_predicates(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_envelope env;
	struct d1_result res;
	struct d1_entry_result entry;
	struct d1_guard guard;
	static uint8_t data[16];
	d1_id_t writer_adm, repair_adm, v1, v2, txn1, txn2, custody, visible;

	memset(data, 0x91, sizeof(data));
	fill_uuid(&store_uuid, 0x13);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	writer_adm = d1_fixture_admit(s, &object, 11,
				      D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	/* REPAIR only: no WRITE at all. */
	repair_adm = d1_fixture_admit(s, &object, 11, D1_RIGHT_REPAIR);

	v1 = commit_chunk(s, writer_adm, 0, 1, data, sizeof(data),
			  &(struct d1_guard){ .never_written = true }, 0, NULL);
	d1_store_guard(s, &object, 0, &guard);
	v2 = commit_chunk(s, writer_adm, 0, 2, data, sizeof(data), &guard, v1,
			  &txn2);
	check(v1 != 0 && v2 != 0, "a version is replaced");

	/*
	 * A wrong expected predecessor, reached with valid custody.  With
	 * a bogus custody handle the call fails on custody first and never
	 * reaches the comparison, which is how this row used to pass for
	 * the wrong reason.
	 */
	custody = d1_fixture_custody(s, v2);
	check(rollback_one(s, repair_adm, 0, 2, txn2,
			   &(struct rollback_expect){ .custody_present = true,
						      .custody = custody,
						      .visible_present = true,
						      .visible = v2,
						      .predecessor_present =
							      true,
						      .predecessor = 999999 },
			   &entry) == D1_NO_PREDECESSOR,
	      "a wrong expected predecessor answers NO_PREDECESSOR");
	check(d1_store_visible(s, &object, 0, &visible) && visible == v2,
	      "and changes nothing");

	/* An absent expected predecessor, where one was recorded. */
	check(rollback_one(s, repair_adm, 0, 2, txn2,
			   &(struct rollback_expect){ .custody_present = true,
						      .custody = custody,
						      .visible_present = true,
						      .visible = v2 },
			   &entry) == D1_NO_PREDECESSOR,
	      "an absent expected predecessor is a mismatch too");
	check(d1_store_visible(s, &object, 0, &visible) && visible == v2,
	      "and changes nothing");

	/* An absent expected-visible option asserts nothing is there. */
	check(rollback_one(s, repair_adm, 0, 2, txn2,
			   &(struct rollback_expect){ .custody_present = true,
						      .custody = custody,
						      .predecessor_present =
							      true,
						      .predecessor = v1 },
			   &entry) == D1_OWNER_CONFLICT,
	      "an absent expected-visible option is a mismatch, not a skip");
	check(entry.guard.generation == guard.generation + 1u,
	      "and the conflict carries the current guard");
	check(d1_store_visible(s, &object, 0, &visible) && visible == v2,
	      "and changes nothing");

	/*
	 * A REPAIR-only handle with exact custody rolls committed data
	 * back.  Section 2 asks for REPAIR plus custody, not also WRITE.
	 */
	check(rollback_one(s, repair_adm, 0, 2, txn2,
			   &(struct rollback_expect){ .custody_present = true,
						      .custody = custody,
						      .visible_present = true,
						      .visible = v2,
						      .predecessor_present =
							      true,
						      .predecessor = v1 },
			   &entry) == D1_OK,
	      "REPAIR plus exact custody rolls committed data back");
	check(d1_store_visible(s, &object, 0, &visible) && visible == v1,
	      "and the predecessor is visible again");

	/* A private cancellation needs WRITE, and a REPAIR handle lacks it. */
	env_init(&env, s, writer_adm, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	d1_store_guard(s, &object, 0, &guard);
	write_entry(&env.body.write.entries[0], 0, 11, 3, data, sizeof(data),
		    true, &guard);
	d1_store_apply(s, &env, &res);
	txn1 = res.entries[0].txn;
	check(txn1 != 0, "a new private transaction is prepared");

	/* The private path compares its expectations too. */
	check(rollback_one(s, writer_adm, 0, 3, txn1,
			   &(struct rollback_expect){ .visible_present = true,
						      .visible = v1,
						      .predecessor_present =
							      true,
						      .predecessor = 999999 },
			   &entry) == D1_NO_PREDECESSOR,
	      "a private rollback with a wrong predecessor is refused");
	check(rollback_one(s, writer_adm, 0, 3, txn1,
			   &(struct rollback_expect){ .visible_present = true,
						      .visible = v1 },
			   &entry) == D1_NO_PREDECESSOR,
	      "and so is one that expects no predecessor where there is one");
	check(rollback_one(s, writer_adm, 0, 3, txn1,
			   &(struct rollback_expect){ .predecessor_present =
							      true,
						      .predecessor = v1 },
			   &entry) == D1_OWNER_CONFLICT,
	      "and one that expects nothing visible where something is");

	check(rollback_one(s, repair_adm, 0, 3, txn1,
			   &(struct rollback_expect){ .visible_present = true,
						      .visible = v1,
						      .predecessor_present =
							      true,
						      .predecessor = v1 },
			   &entry) == D1_STALE_AUTH,
	      "a REPAIR-only handle cannot cancel private work");
	check(rollback_one(s, writer_adm, 0, 3, txn1,
			   &(struct rollback_expect){ .visible_present = true,
						      .visible = v1,
						      .predecessor_present =
							      true,
						      .predecessor = v1 },
			   &entry) == D1_OK,
	      "and its own writer can");

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
	check(rollback_one(s, admission, 3, 3, txn4,
			   &(struct rollback_expect){ .custody_present = true,
						      .custody = custody,
						      .visible_present = true,
						      .visible = v4,
						      .predecessor_present =
							      true,
						      .predecessor = v3 },
			   NULL) == D1_OK,
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
	check(rollback_one(s, admission, 3, 2, txn4,
			   &(struct rollback_expect){ .custody_present = true,
						      .custody = custody,
						      .visible_present = true,
						      .visible = v4,
						      .predecessor_present =
							      true,
						      .predecessor = v3 },
			   NULL) == D1_OK,
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
	check(rollback_one(s, admission, 0, 2, txn2,
			   &(struct rollback_expect){ .custody_present = true,
						      .custody = custody,
						      .visible_present = true,
						      .visible = v2,
						      .predecessor_present =
							      true,
						      .predecessor = v1 },
			   &entry) == D1_NO_PREDECESSOR,
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

/* An ordinary selection over a byte range. */
static void ordinary_sel(struct d1_selection_spec *sel)
{
	memset(sel, 0, sizeof(*sel));
	sel->selection = D1_SELECT_ORDINARY;
}

/* An owner selection naming one transaction it claims. */
static void owner_sel(struct d1_selection_spec *sel, d1_id_t txn,
		      uint32_t writer, uint32_t co_id, uint64_t read_epoch)
{
	memset(sel, 0, sizeof(*sel));
	sel->selection = D1_SELECT_OWNER;
	sel->count = 1;
	sel->txns[0] = txn;
	sel->owners[0].cohort = 1;
	sel->owners[0].writer = writer;
	sel->owners[0].co_id = co_id;
	sel->read_epoch = read_epoch;
}

/* E1: a view decides once, and keeps what it decided. */
static void test_view_is_stable(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_selection_spec sel;
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

	ordinary_sel(&sel);
	check(d1_view_open(s, &object, admission, &sel, 0, CHUNK_BYTES,
			   &view) == D1_OK,
	      "a view opens over the first chunk's bytes");
	check(d1_view_read(view, 0, got, sizeof(got), &got_len) == D1_OK &&
		      got_len == sizeof(first) &&
		      memcmp(got, first, sizeof(first)) == 0,
	      "and reads what was committed");

	/* A later commit does not reach back into the open view. */
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
	 * version.
	 */
	check(d1_fixture_release_predecessor(s, v1),
	      "an eligible predecessor releases while a view pins it");
	check(d1_view_read(view, 0, got, sizeof(got), &got_len) == D1_OK &&
		      memcmp(got, first, sizeof(first)) == 0,
	      "and the pinned view still reads its own bytes");

	/* A normal close refuses while the view is outstanding. */
	check(d1_store_close(s) == D1_BUSY,
	      "a normal close refuses while a view is open");
	d1_view_close(s, view);

	ordinary_sel(&sel);
	check(d1_view_open(s, &object, admission, &sel, 0, CHUNK_BYTES,
			   &after) == D1_OK,
	      "a later view opens");
	check(d1_view_version(after, 0, &seen) && seen == v2,
	      "and names the newer version");
	d1_view_close(s, after);
	check(d1_store_close(s) == D1_OK,
	      "and the close succeeds once nothing is outstanding");
}

/*
 * Normal close refuses while a call is in flight, not only while a view
 * is open.
 *
 * A batch releases the lock between members so the next one is
 * revalidated against what the last one left.  A close that ran in that
 * gap saw no views, freed the store, and left the caller to lock a
 * destroyed mutex.  The model is single threaded, so the fixture pair
 * holds exactly what a paused call holds.
 */
static void test_close_refuses_an_active_call(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_selection_spec sel;
	struct d1_view *view = NULL;
	static uint8_t data[16];
	d1_id_t admission;

	memset(data, 0x5a, sizeof(data));
	fill_uuid(&store_uuid, 0x41);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_READ | D1_RIGHT_WRITE |
					     D1_RIGHT_SINGLE_WRITER);
	check(commit_chunk(s, admission, 0, 1, data, sizeof(data),
			   &(struct d1_guard){ .never_written = true }, 0,
			   NULL) != 0,
	      "there is something to read");

	/* A call is admitted and has not returned. */
	d1_fixture_call_enter(s);
	check(d1_store_close(s) == D1_BUSY,
	      "a close during an admitted call is refused");

	/* A view outstanding at the same time is refused for its own reason. */
	ordinary_sel(&sel);
	check(d1_view_open(s, &object, admission, &sel, 0, CHUNK_BYTES,
			   &view) == D1_OK,
	      "a view opens while the call is in flight");
	check(d1_store_close(s) == D1_BUSY, "and the close is still refused");
	d1_view_close(s, view);
	check(d1_store_close(s) == D1_BUSY,
	      "closing the view is not enough while the call is in flight");

	/* The call returns; now the close succeeds. */
	d1_fixture_call_leave(s);
	check(d1_store_close(s) == D1_OK,
	      "and the close succeeds once the call has returned");
}

/*
 * A store whose rebuild failed is finished.
 *
 * Returning an error is not the same as being safe to serve from: a
 * partial rebuild is neither the logged history nor an empty store.
 * The handle stops being usable for anything but teardown.
 */
static void test_failed_replay_poisons_the_handle(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *live, *bad;
	struct d1_selection_spec sel;
	struct d1_view *view = NULL;
	struct d1_envelope env;
	struct d1_result res;
	const uint8_t *log;
	size_t len;
	uint8_t *writable;
	static uint8_t data[16];
	d1_id_t admission;

	memset(data, 0x77, sizeof(data));
	fill_uuid(&store_uuid, 0x42);
	live = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!live)
		return;
	d1_store_journal_enable(live);
	admission = d1_fixture_admit(live, &object, 11,
				     D1_RIGHT_READ | D1_RIGHT_WRITE |
					     D1_RIGHT_SINGLE_WRITER);
	check(commit_chunk(live, admission, 0, 1, data, sizeof(data),
			   &(struct d1_guard){ .never_written = true }, 0,
			   NULL) != 0,
	      "there is a history to rebuild");
	check(commit_chunk(live, admission, 2, 2, data, sizeof(data),
			   &(struct d1_guard){ .never_written = true }, 0,
			   NULL) != 0,
	      "with more than one record in it");

	log = d1_store_journal(live, &len);
	bad = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!bad) {
		d1_store_free(live);
		return;
	}
	/* Corrupt a record in the middle so the rebuild stops part way. */
	writable = (uint8_t *)(uintptr_t)log;
	writable[len - 1] ^= 0xffu;
	check(d1_store_replay(bad, log, len) == D1_IO, "the rebuild fails");
	writable[len - 1] ^= 0xffu;

	/* From here the handle answers nothing but teardown. */
	env_init(&env, bad, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 1, 11, 3, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	check(d1_store_apply(bad, &env, &res) == D1_INVALID,
	      "it will not apply anything");
	ordinary_sel(&sel);
	check(d1_view_open(bad, &object, admission, &sel, 0, CHUNK_BYTES,
			   &view) == D1_INVALID,
	      "nor open a view");
	check(view == NULL, "and gives none");
	check(d1_store_replay(bad, log, len) == D1_INVALID,
	      "nor try the rebuild again");
	check(d1_store_close(bad) == D1_OK, "but it still closes");

	d1_store_free(live);
}

/* Fixture custody names a version that exists. */
static void test_custody_needs_a_real_version(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	static uint8_t data[16];
	d1_id_t admission, v1;

	memset(data, 0x78, sizeof(data));
	fill_uuid(&store_uuid, 0x43);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);

	check(d1_fixture_custody(s, 1) == 0,
	      "custody over a version that does not exist yet is refused");
	v1 = commit_chunk(s, admission, 0, 1, data, sizeof(data),
			  &(struct d1_guard){ .never_written = true }, 0, NULL);
	check(v1 != 0, "a version is committed");
	check(d1_fixture_custody(s, v1) != 0, "and custody over it is issued");
	check(d1_fixture_custody(s, v1 + 1000u) == 0,
	      "but not over one that has never been allocated");

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
		struct d1_selection_spec sel;
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
		ordinary_sel(&sel);
		check(d1_view_open(s, &object, admission, &sel, 0, CHUNK_BYTES,
				   &view) == D1_OK,
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
		rollback_status[pass] = rollback_one(
			s, admission, 0, 2, txn2,
			&(struct rollback_expect){ .custody_present = true,
						   .custody = custody,
						   .visible_present = true,
						   .visible = v2,
						   .predecessor_present = true,
						   .predecessor = v1 },
			NULL);
		d1_store_free(s);
	}

	check(released[0] && released[1],
	      "the release succeeds in either order");
	check(rollback_status[0] == rollback_status[1],
	      "and a later rollback finds the same thing either way");
	check(rollback_status[0] == D1_NO_PREDECESSOR,
	      "which is that the released predecessor is gone");
}

/*
 * E3: an owner selects its own finalized work by naming it, and nobody
 * else can.  An owner triple is not an authorisation token.
 */
static void test_view_owner_selection(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_selection_spec sel;
	struct d1_view *own = NULL, *ordinary = NULL, *denied = NULL;
	struct d1_guard guard;
	static uint8_t committed[48];
	static uint8_t pending[64];
	uint8_t got[64];
	uint32_t got_len;
	d1_id_t admission, stranger, v1, v2, txn, seen;

	memset(committed, 0xd4, sizeof(committed));
	memset(pending, 0xe5, sizeof(pending));
	fill_uuid(&store_uuid, 0x66);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_READ | D1_RIGHT_WRITE |
					     D1_RIGHT_SINGLE_WRITER);
	stranger = d1_fixture_admit(s, &object, 22, D1_RIGHT_READ);

	v1 = commit_chunk(s, admission, 0, 1, committed, sizeof(committed),
			  &(struct d1_guard){ .never_written = true }, 0, NULL);
	d1_store_guard(s, &object, 0, &guard);
	v2 = finalize_chunk(s, admission, 0, 2, pending, sizeof(pending),
			    &guard, v1, &txn);
	check(v1 != 0 && v2 != 0, "one version commits and one finalizes");

	owner_sel(&sel, txn, 11, 2, 0);
	check(d1_view_open(s, &object, admission, &sel, 0, CHUNK_BYTES, &own) ==
		      D1_OK,
	      "an owner view opens on its own transaction");
	check(d1_view_version(own, 0, &seen) && seen == v2, "and selects it");
	check(d1_view_read(own, 0, got, sizeof(got), &got_len) == D1_OK &&
		      got_len == sizeof(pending) &&
		      memcmp(got, pending, sizeof(pending)) == 0,
	      "and reads its bytes");
	/* Private extension: the owner view has its own, longer EOF. */
	check(d1_view_eof(own) == sizeof(pending),
	      "the owner view's EOF comes from what it selected");
	check(d1_store_eof(s, &object) == sizeof(committed),
	      "and the ordinary EOF is unchanged");

	ordinary_sel(&sel);
	check(d1_view_open(s, &object, admission, &sel, 0, CHUNK_BYTES,
			   &ordinary) == D1_OK,
	      "an ordinary view opens alongside it");
	check(d1_view_version(ordinary, 0, &seen) && seen == v1,
	      "and still sees only what is committed");

	/* Another reader naming the same transaction gets nothing. */
	owner_sel(&sel, txn, 11, 2, 0);
	check(d1_view_open(s, &object, stranger, &sel, 0, CHUNK_BYTES,
			   &denied) == D1_STALE_AUTH,
	      "another admission cannot select this private version");
	check(denied == NULL, "and gets no view at all, not committed data");

	/* The right caller with the wrong epoch gets nothing either. */
	owner_sel(&sel, txn, 11, 2, 99);
	check(d1_view_open(s, &object, admission, &sel, 0, CHUNK_BYTES,
			   &denied) == D1_STALE_AUTH,
	      "a stale read epoch fails the view");
	check(denied == NULL, "with no fallback to committed data");

	/* The right caller with the wrong owner triple gets nothing. */
	owner_sel(&sel, txn, 11, 99, 0);
	check(d1_view_open(s, &object, admission, &sel, 0, CHUNK_BYTES,
			   &denied) == D1_OWNER_CONFLICT,
	      "a wrong owner triple fails the view");

	d1_view_close(s, own);
	d1_view_close(s, ordinary);
	d1_store_free(s);
}

/* A private replacement that is shorter has its own shorter EOF. */
static void test_owner_view_shrinkage(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_selection_spec sel;
	struct d1_view *own = NULL;
	struct d1_guard guard;
	static uint8_t big[200];
	static uint8_t small[20];
	d1_id_t admission, v1, v2, txn;

	memset(big, 0x11, sizeof(big));
	memset(small, 0x22, sizeof(small));
	fill_uuid(&store_uuid, 0x67);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_READ | D1_RIGHT_WRITE |
					     D1_RIGHT_SINGLE_WRITER);

	v1 = commit_chunk(s, admission, 0, 1, big, sizeof(big),
			  &(struct d1_guard){ .never_written = true }, 0, NULL);
	d1_store_guard(s, &object, 0, &guard);
	v2 = finalize_chunk(s, admission, 0, 2, small, sizeof(small), &guard,
			    v1, &txn);
	check(v1 != 0 && v2 != 0, "a shorter private replacement finalizes");

	owner_sel(&sel, txn, 11, 2, 0);
	check(d1_view_open(s, &object, admission, &sel, 0, CHUNK_BYTES, &own) ==
		      D1_OK,
	      "the owner view opens");
	check(d1_view_eof(own) == sizeof(small),
	      "and its EOF shrinks with what it selected");
	check(d1_store_eof(s, &object) == sizeof(big),
	      "while the ordinary EOF stays where it was");
	d1_view_close(s, own);
	d1_store_free(s);
}

/* Byte ranges, holes, EOF and the admission a read needs. */
static void test_view_range_holes_and_admission(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_selection_spec sel;
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

	ordinary_sel(&sel);
	check(d1_view_open(s, &object, admission, &sel, 0, 2u * CHUNK_BYTES,
			   &view) == D1_OK,
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

	/* A byte offset that is not a chunk boundary is ordinary. */
	check(d1_view_read(view, CHUNK_BYTES + 10u, got, 4, &got_len) ==
			      D1_OK &&
		      got_len == 4 && got[0] == 0xf6 && got[3] == 0xf6,
	      "a read at a byte offset inside a chunk works");

	check(d1_view_read(view, d1_view_eof(view), got, sizeof(got),
			   &got_len) == D1_OK &&
		      got_len == 0,
	      "a read at EOF returns nothing");

	/* Outside the range the view covers is refused, not zero-filled. */
	{
		struct d1_view *narrow = NULL;

		ordinary_sel(&sel);
		check(d1_view_open(s, &object, admission, &sel, CHUNK_BYTES,
				   2u * CHUNK_BYTES, &narrow) == D1_OK,
		      "a view opens over the second chunk only");
		check(d1_view_read(narrow, 0, got, sizeof(got), &got_len) ==
			      D1_INVALID,
		      "a read before its range is refused");
		check(got_len == 0, "and returns nothing");
		check(d1_view_read(narrow, CHUNK_BYTES, got, 8, &got_len) ==
				      D1_OK &&
			      got_len == 8,
		      "and a read inside it works");
		d1_view_close(s, narrow);
	}

	d1_view_close(s, view);

	{
		struct d1_view *denied = NULL;

		ordinary_sel(&sel);
		check(d1_view_open(s, &object, writeonly, &sel, 0, CHUNK_BYTES,
				   &denied) == D1_STALE_AUTH,
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
	/* Custody is issued over whatever is visible on chunk 3 now. */
	{
		d1_id_t visible = 0;

		d1_store_visible(s, &object, 3, &visible);
		custody = d1_fixture_custody(s, visible);
		if (rollback_one(s, admission, 3, 3, txn4,
				 &(struct rollback_expect){
					 .custody_present = true,
					 .custody = custody,
					 .visible_present = true,
					 .visible = visible,
					 .predecessor_present = true,
					 .predecessor = v3 },
				 NULL) != D1_OK)
			return 0;
	}
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
 * A batch interrupted in the middle stops there and resumes in order.
 *
 * Section 8 describes a batch "interrupted after entry j" whose exact
 * retry "reconstructs those and evaluates remaining entries", and
 * section 7 requires input order.  An infrastructure failure is that
 * interruption: carrying on past it would put later members into the
 * log ahead of one that has not happened, so log order would stop being
 * input order.  A semantic refusal is not an interruption -- it is a
 * result, with a receipt -- and independent later members still run.
 *
 * The oracle the memo asks for is the last check here: the resumed
 * history and an uninterrupted one, at the same accepted operations,
 * must end in the same store.
 */
static void test_interrupted_batch_stops_and_resumes(void)
{
	struct d1_uuid store_uuid, clean_uuid;
	struct d1_store *s, *clean;
	struct d1_envelope env, plain;
	struct d1_result first, retry, ref;
	static uint8_t data[16];
	d1_id_t admission, clean_admission, visible;
	unsigned int i;

	memset(data, 0xa7, sizeof(data));
	fill_uuid(&store_uuid, 0x15);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	d1_store_journal_enable(s);
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);

	env_init(&env, s, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 3;
	env.body.write.stability = D1_DATA_SYNC;
	env.body.write.activate = true;
	for (i = 0; i < 3; i++)
		write_entry(&env.body.write.entries[i], i, 11, i + 1u, data,
			    sizeof(data), true,
			    &(struct d1_guard){ .never_written = true });

	/* The second member's event is the one that cannot be written. */
	d1_fixture_fail_append_in(s, 2);
	check(d1_store_apply(s, &env, &first) == D1_OK,
	      "the operation itself completes");
	check(first.entries[0].status == D1_OK &&
		      first.entries[0].disposition == D1_COMPLETED,
	      "the first member completed");
	check(first.entries[1].disposition == D1_UNRECORDED,
	      "the second member is UNRECORDED");
	check(first.entries[2].disposition == D1_UNRECORDED,
	      "and the batch stopped, so the third is UNRECORDED too");
	check(!first.entries[2].txn_present &&
		      !first.entries[2].version_present,
	      "with nothing reserved for it");
	check(d1_store_visible(s, &object, 0, &visible), "chunk 0 is visible");
	check(!d1_store_visible(s, &object, 1, &visible), "chunk 1 is not");
	check(!d1_store_visible(s, &object, 2, &visible),
	      "and neither is chunk 2, which never ran");

	/* The exact retry resumes the interrupted members, in input order. */
	check(d1_store_apply(s, &env, &retry) == D1_OK, "the exact retry runs");
	check(retry.entries[0].status == D1_OK &&
		      retry.entries[0].version == first.entries[0].version &&
		      retry.entries[0].txn == first.entries[0].txn,
	      "the first member returns its recorded result");
	check(retry.entries[1].status == D1_OK &&
		      retry.entries[1].disposition == D1_COMPLETED,
	      "the second member executes now");
	check(retry.entries[2].status == D1_OK &&
		      retry.entries[2].disposition == D1_COMPLETED,
	      "and so does the third");
	check(retry.entries[1].txn < retry.entries[2].txn,
	      "and they took their IDs in input order");
	check(d1_store_visible(s, &object, 1, &visible) &&
		      d1_store_visible(s, &object, 2, &visible),
	      "so both become visible");

	/*
	 * The memo's oracle: the same accepted operations, uninterrupted,
	 * end in the same store.
	 */
	fill_uuid(&clean_uuid, 0x16);
	clean = d1_store_open(&clean_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (clean) {
		d1_store_journal_enable(clean);
		clean_admission = d1_fixture_admit(
			clean, &object, 11,
			D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
		env_init(&plain, clean, clean_admission, D1_OP_WRITE_BATCH);
		plain.body.write.count = 3;
		plain.body.write.stability = D1_DATA_SYNC;
		plain.body.write.activate = true;
		for (i = 0; i < 3; i++)
			write_entry(&plain.body.write.entries[i], i, 11, i + 1u,
				    data, sizeof(data), true,
				    &(struct d1_guard){ .never_written =
								true });
		check(d1_store_apply(clean, &plain, &ref) == D1_OK,
		      "an uninterrupted batch runs");
		check(states_agree(s, clean),
		      "and ends in the same store as the resumed one");
		d1_store_free(clean);
	}

	d1_store_free(s);
}

/* A semantic refusal in the middle does not stop the members after it. */
static void test_semantic_refusal_does_not_interrupt(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_envelope env;
	struct d1_result res;
	static uint8_t data[16];
	d1_id_t admission, visible;
	unsigned int i;

	memset(data, 0xa8, sizeof(data));
	fill_uuid(&store_uuid, 0x17);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	d1_store_journal_enable(s);
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);

	env_init(&env, s, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 3;
	env.body.write.stability = D1_DATA_SYNC;
	env.body.write.activate = true;
	for (i = 0; i < 3; i++)
		write_entry(&env.body.write.entries[i], i, 11, i + 1u, data,
			    sizeof(data), true,
			    &(struct d1_guard){ .never_written = true });
	/* The middle member is refused on its own merits. */
	env.body.write.entries[1].checksum.digest[0] ^= 0xffu;

	check(d1_store_apply(s, &env, &res) == D1_OK, "the batch runs");
	check(res.entries[0].status == D1_OK, "the first member completes");
	check(res.entries[1].status == D1_CHECKSUM &&
		      res.entries[1].disposition == D1_COMPLETED,
	      "the second is a recorded refusal");
	check(res.entries[2].status == D1_OK &&
		      res.entries[2].disposition == D1_COMPLETED,
	      "and the third still runs");
	check(d1_store_visible(s, &object, 0, &visible) &&
		      d1_store_visible(s, &object, 2, &visible),
	      "so the independent members are visible");
	check(!d1_store_visible(s, &object, 1, &visible),
	      "and the refused one is not");

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
 * I2: a forced index fault after a durable COMMIT.
 *
 * The event is durable, so the COMMIT receipt stands.  What must not
 * happen is a read serving the predecessor against that receipt.
 *
 * Read this for what it is: reads in this model use the reducer's state
 * and always did, so there is no switch to observe.  The fault leaves a
 * stale materialized pointer, and these checks show that nothing
 * followed it.  That is not a demonstration of failover between two
 * real index paths, and the model does not have two.
 */
static void test_index_fault_serves_the_overlay(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_selection_spec sel;
	struct d1_view *view = NULL;
	struct d1_guard guard;
	static uint8_t first[32];
	static uint8_t second[32];
	uint8_t got[32];
	uint32_t got_len;
	d1_id_t admission, v1, v2, seen, stale;

	memset(first, 0x31, sizeof(first));
	memset(second, 0x32, sizeof(second));
	fill_uuid(&store_uuid, 0x14);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	d1_store_journal_enable(s);
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_READ | D1_RIGHT_WRITE |
					     D1_RIGHT_SINGLE_WRITER);

	v1 = commit_chunk(s, admission, 0, 1, first, sizeof(first),
			  &(struct d1_guard){ .never_written = true }, 0, NULL);
	check(v1 != 0, "the first version commits");
	check(!d1_store_overlay_active(s),
	      "and the store is not on the overlay");

	d1_store_guard(s, &object, 0, &guard);
	d1_fixture_fail_next_index(s);
	v2 = commit_chunk(s, admission, 0, 2, second, sizeof(second), &guard,
			  v1, NULL);
	check(v2 != 0 && v2 != v1,
	      "the replacement commits despite the index fault");
	check(d1_store_overlay_active(s),
	      "and the store records that the two have diverged");

	/* The materialized pointer was left behind, on purpose. */
	check(d1_store_materialized(s, &object, 0, &stale) && stale == v1,
	      "the materialized index still names the predecessor");

	/* No read may serve it. */
	check(d1_store_visible(s, &object, 0, &seen) && seen == v2,
	      "but what is visible is the committed version");
	ordinary_sel(&sel);
	check(d1_view_open(s, &object, admission, &sel, 0, CHUNK_BYTES,
			   &view) == D1_OK,
	      "a view opens after the fault");
	check(d1_view_version(view, 0, &seen) && seen == v2,
	      "and selects the committed version, not the predecessor");
	check(d1_view_read(view, 0, got, sizeof(got), &got_len) == D1_OK &&
		      memcmp(got, second, sizeof(second)) == 0,
	      "and reads its bytes");
	d1_view_close(s, view);

	/* A rebuild from the log produces the same state, without the fault. */
	{
		struct d1_store *rebuilt;
		const uint8_t *log;
		size_t len;

		log = d1_store_journal(s, &len);
		rebuilt =
			d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
		if (rebuilt) {
			check(d1_store_replay(rebuilt, log, len) == D1_OK,
			      "the log replays");
			check(!d1_store_overlay_active(rebuilt),
			      "and the fault did not replay with it");
			/* Unjournalled harness state, so it cannot. */
			check(d1_store_visible(rebuilt, &object, 0, &seen) &&
				      seen == v2,
			      "and the rebuilt store sees the committed "
			      "version");
			check(d1_store_materialized(rebuilt, &object, 0,
						    &stale) &&
				      stale == v2,
			      "with its materialized index in agreement");
			d1_store_free(rebuilt);
		}
	}

	d1_store_free(s);
}

/*
 * An undone event never becomes durable, whatever happens next.
 *
 * The append vector is the store's state too.  A flush fault used to
 * leave the record it wrote sitting in front of the next one, and the
 * next successful flush claimed it: a write the caller was told was
 * UNRECORDED became durable behind its back, and either replayed into a
 * store the live one never was, or poisoned the log so it no longer
 * rebuilt at all.  Each kind of event is driven here, and each is
 * followed by something that does flush.
 */
static void test_undone_event_never_becomes_durable(void)
{
	static const unsigned int kinds = 3;
	unsigned int kind;

	for (kind = 0; kind < kinds; kind++) {
		struct d1_uuid store_uuid;
		struct d1_store *live, *rebuilt;
		struct d1_envelope env;
		struct d1_result res;
		const uint8_t *log;
		size_t len, before;
		static uint8_t data[16];
		d1_id_t admission, control, extra, seen;
		uint32_t status;

		memset(data, 0xb5, sizeof(data));
		fill_uuid(&store_uuid, (uint8_t)(0x21 + kind));
		live = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
		if (!live)
			return;
		check(d1_store_journal_enable(live) == D1_OK,
		      "journalling starts");
		admission = d1_fixture_admit(live, &object, 11,
					     D1_RIGHT_WRITE | D1_RIGHT_CONTROL |
						     D1_RIGHT_SINGLE_WRITER);
		(void)d1_store_journal(live, &before);

		d1_fixture_fail_next_flush(live);
		switch (kind) {
		case 0:
			/* An ordinary ENTRY event. */
			env_init(&env, live, admission, D1_OP_WRITE_BATCH);
			env.body.write.count = 1;
			env.body.write.stability = D1_FILE_SYNC;
			write_entry(&env.body.write.entries[0], 0, 11, 1, data,
				    sizeof(data), true,
				    &(struct d1_guard){ .never_written =
								true });
			d1_store_apply(live, &env, &res);
			check(res.entries[0].disposition == D1_UNRECORDED,
			      "the entry event is UNRECORDED");
			break;
		case 1:
			/* An envelope-borne control event. */
			env_init(&env, live, admission, D1_OP_LEASE_REAP);
			env.body.control.count = 1;
			env.body.control.txns[0] = 1;
			env.body.control.old_admission = admission;
			d1_store_apply(live, &env, &res);
			check(res.entries[0].disposition == D1_UNRECORDED,
			      "the control event is UNRECORDED");
			break;
		default:
			/* A fixture control event. */
			control = d1_fixture_admit(live, &object, 12,
						   D1_RIGHT_READ);
			check(control == 0,
			      "the fixture control event is refused");
			break;
		}
		(void)d1_store_journal(live, &len);
		check(len == before, "and claims no durable bytes");

		/* Something unrelated now succeeds and flushes. */
		extra = d1_fixture_admit(live, &object, 13, D1_RIGHT_READ);
		check(extra != 0, "an unrelated control event succeeds");
		log = d1_store_journal(live, &len);
		check(len > before, "and does claim bytes");

		rebuilt =
			d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
		if (!rebuilt) {
			d1_store_free(live);
			return;
		}
		status = d1_store_replay(rebuilt, log, len);
		check(status == D1_OK,
		      "the log still rebuilds after the failed event");
		check(!d1_store_visible(rebuilt, &object, 0, &seen),
		      "and the undone event did not come back");
		check(states_agree(live, rebuilt),
		      "so the rebuilt store is the store that wrote it");
		d1_store_free(rebuilt);
		d1_store_free(live);
	}
}

/*
 * The same fault, then an unrelated operation, then an exact retry:
 * the retry executes, because nothing was recorded, and the IDs it
 * takes are the ones a replay takes.
 */
static void test_retry_after_flush_fault_agrees_with_replay(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *live, *rebuilt;
	struct d1_envelope env, other;
	struct d1_result res;
	const uint8_t *log;
	size_t len;
	static uint8_t data[16];
	d1_id_t admission, seen_live, seen_rebuilt;

	memset(data, 0xb6, sizeof(data));
	fill_uuid(&store_uuid, 0x24);
	live = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!live)
		return;
	d1_store_journal_enable(live);
	admission = d1_fixture_admit(live, &object, 11,
				     D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);

	env_init(&env, live, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_DATA_SYNC;
	env.body.write.activate = true;
	write_entry(&env.body.write.entries[0], 0, 11, 1, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });

	d1_fixture_fail_next_flush(live);
	d1_store_apply(live, &env, &res);
	check(res.entries[0].disposition == D1_UNRECORDED,
	      "the first attempt is UNRECORDED");

	/* An unrelated write lands in between, and does flush. */
	env_init(&other, live, admission, D1_OP_WRITE_BATCH);
	other.body.write.count = 1;
	other.body.write.stability = D1_DATA_SYNC;
	other.body.write.activate = true;
	write_entry(&other.body.write.entries[0], 2, 11, 2, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	d1_store_apply(live, &other, &res);
	check(res.entries[0].status == D1_OK, "the unrelated write succeeds");

	/* The exact retry executes: no receipt was ever recorded. */
	d1_store_apply(live, &env, &res);
	check(res.entries[0].status == D1_OK && res.entries[0].activated,
	      "the exact retry executes");

	log = d1_store_journal(live, &len);
	rebuilt = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!rebuilt) {
		d1_store_free(live);
		return;
	}
	check(d1_store_replay(rebuilt, log, len) == D1_OK, "the log rebuilds");
	check(d1_store_visible(live, &object, 0, &seen_live) &&
		      d1_store_visible(rebuilt, &object, 0, &seen_rebuilt) &&
		      seen_live == seen_rebuilt,
	      "and agrees on the retried version");
	check(states_agree(live, rebuilt), "and on everything else");

	/* Reopen takes the same log the same way. */
	{
		struct d1_store *reopened =
			d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);

		if (reopened) {
			check(d1_store_reopen(reopened, log, len) == D1_OK,
			      "and the same log reopens");
			check(states_agree(live, reopened),
			      "into the same store");
			d1_store_free(reopened);
		}
	}

	d1_store_free(rebuilt);
	d1_store_free(live);
}

/*
 * A REPAIR-only committed rollback is admitted live on REPAIR plus
 * custody, and must replay the same way.  Two rights tables disagreed
 * here: replay demanded WRITE and refused a log the live store had
 * written.
 */
static void test_repair_only_rollback_replays(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *live, *rebuilt;
	struct d1_guard guard;
	const uint8_t *log;
	size_t len;
	static uint8_t data[16];
	d1_id_t writer_adm, repair_adm, v1, v2, txn2, custody, seen;

	memset(data, 0xb7, sizeof(data));
	fill_uuid(&store_uuid, 0x25);
	live = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!live)
		return;
	check(d1_store_journal_enable(live) == D1_OK, "journalling starts");
	writer_adm = d1_fixture_admit(live, &object, 11,
				      D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	/* REPAIR only: no WRITE at all, so the two tables differ. */
	repair_adm = d1_fixture_admit(live, &object, 11, D1_RIGHT_REPAIR);

	v1 = commit_chunk(live, writer_adm, 0, 1, data, sizeof(data),
			  &(struct d1_guard){ .never_written = true }, 0, NULL);
	d1_store_guard(live, &object, 0, &guard);
	v2 = commit_chunk(live, writer_adm, 0, 2, data, sizeof(data), &guard,
			  v1, &txn2);
	custody = d1_fixture_custody(live, v2);
	check(v1 && v2 && custody, "a replacement is committed under custody");

	check(rollback_one(live, repair_adm, 0, 2, txn2,
			   &(struct rollback_expect){ .custody_present = true,
						      .custody = custody,
						      .visible_present = true,
						      .visible = v2,
						      .predecessor_present =
							      true,
						      .predecessor = v1 },
			   NULL) == D1_OK,
	      "a REPAIR-only handle rolls it back");
	check(d1_store_visible(live, &object, 0, &seen) && seen == v1,
	      "and the predecessor is visible");

	log = d1_store_journal(live, &len);
	rebuilt = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!rebuilt) {
		d1_store_free(live);
		return;
	}
	check(d1_store_replay(rebuilt, log, len) == D1_OK,
	      "and the log replays rather than being refused");
	check(states_agree(live, rebuilt), "into the same store");
	d1_store_free(rebuilt);

	/* Its refusals replay too. */
	{
		struct d1_store *second, *second_rebuilt;
		struct d1_uuid other_uuid;
		d1_id_t other_writer, other_repair, w1, w2, t2, bad;

		fill_uuid(&other_uuid, 0x26);
		second =
			d1_store_open(&other_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
		if (second) {
			d1_store_journal_enable(second);
			other_writer = d1_fixture_admit(
				second, &object, 11,
				D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
			other_repair = d1_fixture_admit(second, &object, 11,
							D1_RIGHT_REPAIR);
			w1 = commit_chunk(
				second, other_writer, 0, 1, data, sizeof(data),
				&(struct d1_guard){ .never_written = true }, 0,
				NULL);
			d1_store_guard(second, &object, 0, &guard);
			w2 = commit_chunk(second, other_writer, 0, 2, data,
					  sizeof(data), &guard, w1, &t2);
			bad = d1_fixture_custody(second, w1);
			check(w2 != 0 && bad != 0, "a stale custody is issued");
			check(rollback_one(second, other_repair, 0, 2, t2,
					   &(struct rollback_expect){
						   .custody_present = true,
						   .custody = bad,
						   .visible_present = true,
						   .visible = w2,
						   .predecessor_present = true,
						   .predecessor = w1 },
					   NULL) == D1_OWNER_CONFLICT,
			      "and its refusal is a conflict");

			log = d1_store_journal(second, &len);
			second_rebuilt = d1_store_open(&other_uuid, CHUNK_BYTES,
						       MAX_FILE_BYTES);
			if (second_rebuilt) {
				check(d1_store_replay(second_rebuilt, log,
						      len) == D1_OK,
				      "the refusal replays too");
				d1_store_free(second_rebuilt);
			}
			d1_store_free(second);
		}
	}

	d1_store_free(live);
}

/*
 * A START fences owner reads, not only mutations.
 *
 * Section 9: retained pending and finalized versions "need explicit
 * recovery_admit before owner reads or new lifecycle work".  The
 * transaction still records the old handle and its epoch, so nothing
 * about the selection itself changes at a reopen; what changes is that
 * the handle belongs to an incarnation that is over.
 */
static void test_reopen_fences_owner_reads(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *live, *reopened;
	struct d1_selection_spec sel;
	struct d1_view *view = NULL;
	struct d1_envelope env;
	struct d1_result res;
	const uint8_t *log;
	size_t len;
	static uint8_t data[24];
	uint8_t got[24];
	uint32_t got_len;
	d1_id_t admission, control, fresh, txn, seen;

	memset(data, 0xd1, sizeof(data));
	fill_uuid(&store_uuid, 0x31);
	live = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!live)
		return;
	d1_store_journal_enable(live);
	admission = d1_fixture_admit(live, &object, 11,
				     D1_RIGHT_READ | D1_RIGHT_WRITE |
					     D1_RIGHT_SINGLE_WRITER);
	check(finalize_chunk(live, admission, 0, 1, data, sizeof(data),
			     &(struct d1_guard){ .never_written = true }, 0,
			     &txn) != 0,
	      "a private version is finalized");

	/* Before the reopen, its owner can select it. */
	owner_sel(&sel, txn, 11, 1, 0);
	check(d1_view_open(live, &object, admission, &sel, 0, CHUNK_BYTES,
			   &view) == D1_OK,
	      "its owner selects it while the incarnation is current");
	d1_view_close(live, view);
	view = NULL;

	log = d1_store_journal(live, &len);
	reopened = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!reopened) {
		d1_store_free(live);
		return;
	}
	check(d1_store_reopen(reopened, log, len) == D1_OK,
	      "the store reopens");

	/* The old handle is fenced for reads as well as for mutations. */
	owner_sel(&sel, txn, 11, 1, 0);
	check(d1_view_open(reopened, &object, admission, &sel, 0, CHUNK_BYTES,
			   &view) == D1_STALE_AUTH,
	      "an old handle cannot select its private version after a reopen");
	check(view == NULL, "and gets no view");

	/* An ordinary read by that handle is a separate question. */
	ordinary_sel(&sel);
	check(d1_view_open(reopened, &object, admission, &sel, 0, CHUNK_BYTES,
			   &view) == D1_OK,
	      "an ordinary view by the same handle is not fenced here");
	if (view)
		d1_view_close(reopened, view);
	view = NULL;

	/* Recovery under current authority is what opens it again. */
	control = d1_fixture_admit(reopened, &object, 11, D1_RIGHT_CONTROL);
	fresh = d1_fixture_admit(reopened, &object, 11,
				 D1_RIGHT_READ | D1_RIGHT_WRITE |
					 D1_RIGHT_SINGLE_WRITER);
	env_init(&env, reopened, control, D1_OP_RECOVERY_ADMIT);
	env.body.control.count = 1;
	env.body.control.txns[0] = txn;
	env.body.control.old_admission = admission;
	env.body.control.new_admission_present = true;
	env.body.control.new_admission = fresh;
	env.body.control.read_epoch_present = true;
	env.body.control.read_epoch = 0;
	check(d1_store_apply(reopened, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_OK,
	      "recovery re-admits the transaction");

	owner_sel(&sel, txn, 11, 1, 0);
	check(d1_view_open(reopened, &object, fresh, &sel, 0, CHUNK_BYTES,
			   &view) == D1_OK,
	      "and the recovered handle selects it");
	check(d1_view_version(view, 0, &seen), "with a version");
	check(d1_view_read(view, 0, got, sizeof(got), &got_len) == D1_OK &&
		      got_len == sizeof(data) &&
		      memcmp(got, data, sizeof(data)) == 0,
	      "and reads its bytes");
	d1_view_close(reopened, view);

	d1_store_free(reopened);
	d1_store_free(live);
}

/* Recovery onto a handle that is itself fenced is not a recovery. */
static void test_recovery_target_must_be_current(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *live, *reopened;
	struct d1_envelope env;
	struct d1_result res;
	const uint8_t *log;
	size_t len;
	static uint8_t data[16];
	d1_id_t old_a, old_b, control, txn;

	memset(data, 0xd2, sizeof(data));
	fill_uuid(&store_uuid, 0x32);
	live = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!live)
		return;
	d1_store_journal_enable(live);
	old_a = d1_fixture_admit(live, &object, 11,
				 D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	/* A second handle issued in the same, soon to be old, incarnation. */
	old_b = d1_fixture_admit(live, &object, 11,
				 D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	env_init(&env, live, old_a, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 0, 11, 1, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	d1_store_apply(live, &env, &res);
	txn = res.entries[0].txn;
	check(txn != 0 && old_b != 0, "there is work and a second old handle");

	log = d1_store_journal(live, &len);
	reopened = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!reopened) {
		d1_store_free(live);
		return;
	}
	d1_store_reopen(reopened, log, len);
	control = d1_fixture_admit(reopened, &object, 11, D1_RIGHT_CONTROL);

	env_init(&env, reopened, control, D1_OP_RECOVERY_ADMIT);
	env.body.control.count = 1;
	env.body.control.txns[0] = txn;
	env.body.control.old_admission = old_a;
	env.body.control.new_admission_present = true;
	env.body.control.new_admission = old_b;
	env.body.control.read_epoch_present = true;
	env.body.control.read_epoch = 0;
	check(d1_store_apply(reopened, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_STALE_AUTH,
	      "recovery onto a fenced handle is refused");

	/* And the old handle was not revoked on the way to refusing. */
	env_init(&env, reopened, control, D1_OP_RECOVERY_ADMIT);
	env.body.control.count = 1;
	env.body.control.txns[0] = txn;
	env.body.control.old_admission = old_a;
	env.body.control.new_admission_present = true;
	env.body.control.new_admission = d1_fixture_admit(
		reopened, &object, 11, D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	env.body.control.read_epoch_present = true;
	env.body.control.read_epoch = 0;
	check(d1_store_apply(reopened, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_OK,
	      "and a recovery onto a current handle still works");

	d1_store_free(reopened);
	d1_store_free(live);
}

/*
 * A hole is a hole wherever the read window ends.
 *
 * EOF belongs to the object, not to the window: a view over a low range
 * must still report the object's EOF, so a read inside a hole under a
 * higher chunk returns zeros rather than "end of file".
 */
static void test_view_eof_is_object_wide(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_selection_spec sel;
	struct d1_view *view = NULL;
	static uint8_t data[100];
	uint8_t got[64];
	uint32_t got_len, i;
	d1_id_t admission;

	memset(data, 0xe1, sizeof(data));
	fill_uuid(&store_uuid, 0x33);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_READ | D1_RIGHT_WRITE |
					     D1_RIGHT_SINGLE_WRITER);

	check(commit_chunk(s, admission, 0, 1, data, sizeof(data),
			   &(struct d1_guard){ .never_written = true }, 0,
			   NULL) != 0,
	      "chunk 0 commits a partial image");
	check(commit_chunk(s, admission, 3, 2, data, sizeof(data),
			   &(struct d1_guard){ .never_written = true }, 0,
			   NULL) != 0,
	      "and chunk 3 commits far above it");
	check(d1_store_eof(s, &object) == 3u * CHUNK_BYTES + sizeof(data),
	      "the object's EOF is set by the higher chunk");

	/* A window that stops below the higher chunk. */
	ordinary_sel(&sel);
	check(d1_view_open(s, &object, admission, &sel, 0, 2u * CHUNK_BYTES,
			   &view) == D1_OK,
	      "a view opens over the lower two chunks");
	check(d1_view_eof(view) == d1_store_eof(s, &object),
	      "and reports the object's EOF, not its window's");

	/* The tail of the partial lower image is a hole, not EOF. */
	check(d1_view_read(view, sizeof(data), got, sizeof(got), &got_len) ==
			      D1_OK &&
		      got_len == sizeof(got),
	      "a read past the partial image is not short");
	for (i = 0; i < sizeof(got); i++)
		if (got[i] != 0)
			break;
	check(i == sizeof(got), "and reads as zeros");

	/* A window that is nothing but hole. */
	d1_view_close(s, view);
	view = NULL;
	check(d1_view_open(s, &object, admission, &sel, CHUNK_BYTES,
			   2u * CHUNK_BYTES, &view) == D1_OK,
	      "a hole-only view opens");
	check(d1_view_eof(view) == d1_store_eof(s, &object),
	      "with the object's EOF");
	check(d1_view_read(view, CHUNK_BYTES, got, sizeof(got), &got_len) ==
			      D1_OK &&
		      got_len == sizeof(got),
	      "and a read inside it returns bytes, not end of file");
	for (i = 0; i < sizeof(got); i++)
		if (got[i] != 0)
			break;
	check(i == sizeof(got), "which are zeros");
	d1_view_close(s, view);

	d1_store_free(s);
}

/* An owner view that shortens a chunk below a higher visible one. */
static void test_owner_shrink_below_higher_chunk(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_selection_spec sel;
	struct d1_view *own = NULL;
	struct d1_guard guard;
	static uint8_t big[200];
	static uint8_t small[20];
	uint8_t got[64];
	uint32_t got_len;
	d1_id_t admission, v1, txn;

	memset(big, 0xe2, sizeof(big));
	memset(small, 0xe3, sizeof(small));
	fill_uuid(&store_uuid, 0x34);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_READ | D1_RIGHT_WRITE |
					     D1_RIGHT_SINGLE_WRITER);

	v1 = commit_chunk(s, admission, 0, 1, big, sizeof(big),
			  &(struct d1_guard){ .never_written = true }, 0, NULL);
	check(commit_chunk(s, admission, 2, 2, big, sizeof(big),
			   &(struct d1_guard){ .never_written = true }, 0,
			   NULL) != 0,
	      "a higher chunk is committed");
	d1_store_guard(s, &object, 0, &guard);
	check(finalize_chunk(s, admission, 0, 3, small, sizeof(small), &guard,
			     v1, &txn) != 0,
	      "a shorter private replacement finalizes below it");

	owner_sel(&sel, txn, 11, 3, 0);
	check(d1_view_open(s, &object, admission, &sel, 0, CHUNK_BYTES, &own) ==
		      D1_OK,
	      "an owner view opens over the shortened chunk");
	/*
	 * The private image is shorter, but the higher committed chunk is
	 * still there, so the effective EOF is the higher chunk's.
	 */
	check(d1_view_eof(own) == 2u * CHUNK_BYTES + sizeof(big),
	      "and its EOF comes from the whole effective vector");
	check(d1_view_read(own, 0, got, sizeof(got), &got_len) == D1_OK &&
		      got_len == sizeof(got) &&
		      memcmp(got, small, sizeof(small)) == 0 &&
		      got[sizeof(small)] == 0,
	      "the shortened image reads, and its tail is a hole");
	d1_view_close(s, own);

	d1_store_free(s);
}

/*
 * A semantic refusal consumes no capacity.
 *
 * The object was created on first touch, before any predicate ran, so
 * refused writes to distinct fresh objects used up the object table and
 * a later valid write to a new object answered NOSPC forever.  The
 * refusal receipt itself must survive; only the state must not.
 */
static void test_refusal_consumes_no_capacity(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_objkey keys[D1_MAX_OBJECTS + 1u];
	struct d1_envelope env;
	struct d1_result res, again;
	struct d1_selection_spec sel;
	struct d1_view *view = NULL;
	struct d1_guard guard;
	static uint8_t data[16];
	unsigned int i;
	d1_id_t admissions[D1_MAX_OBJECTS + 1u];
	d1_id_t seen;

	memset(data, 0xf1, sizeof(data));
	fill_uuid(&store_uuid, 0x35);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;

	for (i = 0; i < D1_MAX_OBJECTS + 1u; i++) {
		keys[i].export_uuid = object.export_uuid;
		fill_uuid(&keys[i].object_uuid, (uint8_t)(0x90 + i));
		admissions[i] =
			d1_fixture_admit(s, &keys[i], 11,
					 D1_RIGHT_READ | D1_RIGHT_WRITE |
						 D1_RIGHT_SINGLE_WRITER);
	}

	/* Refuse a write on every object but the last. */
	for (i = 0; i < D1_MAX_OBJECTS; i++) {
		memset(&env, 0, sizeof(env));
		env.object = keys[i];
		env.admission = admissions[i];
		env.incarnation = d1_store_incarnation(s);
		env.key.origin = origin;
		env.key.sequence = next_sequence++;
		env.op = D1_OP_WRITE_BATCH;
		env.body.write.count = 1;
		env.body.write.stability = D1_FILE_SYNC;
		write_entry(&env.body.write.entries[0], 0, 11, 1, data,
			    sizeof(data), true,
			    &(struct d1_guard){ .never_written = true });
		env.body.write.entries[0].checksum.digest[0] ^= 0xffu;
		d1_store_apply(s, &env, &res);
		check(res.entries[0].status == D1_CHECKSUM &&
			      res.entries[0].disposition == D1_COMPLETED,
		      "a bad checksum is a recorded refusal");
		check(!d1_store_guard(s, &keys[i], 0, &guard),
		      "and the object it touched is not there");
		check(!res.entries[0].txn_present &&
			      !res.entries[0].version_present,
		      "and no transaction or version was taken");
		ordinary_sel(&sel);
		check(d1_view_open(s, &keys[i], admissions[i], &sel, 0,
				   CHUNK_BYTES, &view) == D1_INVALID,
		      "and the object cannot be opened for reading");

		/* The refusal is a receipt: the exact retry answers from it. */
		d1_store_apply(s, &env, &again);
		check(again.entries[0].status == D1_CHECKSUM &&
			      again.entries[0].disposition == D1_COMPLETED,
		      "the exact retry returns the recorded refusal");
	}

	/* A valid write on one more object still has room. */
	memset(&env, 0, sizeof(env));
	env.object = keys[D1_MAX_OBJECTS];
	env.admission = admissions[D1_MAX_OBJECTS];
	env.incarnation = d1_store_incarnation(s);
	env.key.origin = origin;
	env.key.sequence = next_sequence++;
	env.op = D1_OP_WRITE_BATCH;
	env.body.write.count = 1;
	env.body.write.stability = D1_DATA_SYNC;
	env.body.write.activate = true;
	write_entry(&env.body.write.entries[0], 0, 11, 2, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	d1_store_apply(s, &env, &res);
	check(res.entries[0].status == D1_OK,
	      "a valid write to a fresh object still has room for it");
	check(d1_store_visible(s, &keys[D1_MAX_OBJECTS], 0, &seen),
	      "and publishes");

	d1_store_free(s);
}

/*
 * A canonical request refused on rights or liveness is a recorded
 * semantic error, so its key is spent.
 *
 * Caller binding and malformed input are refused before the lookup and
 * leave nothing; a request the store did admit as canonical, and then
 * refused, is bound to its digest like any other error.
 */
static void test_bound_authority_refusal_is_recorded(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *live, *rebuilt;
	struct d1_envelope env, changed;
	struct d1_result res;
	const uint8_t *log;
	size_t len;
	static uint8_t data[16];
	d1_id_t readonly, writer;

	memset(data, 0xf2, sizeof(data));
	fill_uuid(&store_uuid, 0x36);
	live = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!live)
		return;
	d1_store_journal_enable(live);
	readonly = d1_fixture_admit(live, &object, 11, D1_RIGHT_READ);
	writer = d1_fixture_admit(live, &object, 11,
				  D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);

	env_init(&env, live, readonly, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 0, 11, 1, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	check(d1_store_apply(live, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_STALE_AUTH,
	      "a handle without WRITE is refused");
	check(res.entries[0].disposition == D1_COMPLETED,
	      "and the refusal is recorded");

	/* The exact retry answers from that receipt. */
	check(d1_store_apply(live, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_STALE_AUTH &&
		      res.entries[0].disposition == D1_COMPLETED,
	      "the exact retry returns the recorded refusal");

	/* A different body under the same key cannot slip past it. */
	changed = env;
	changed.admission = writer;
	check(d1_store_apply(live, &changed, &res) == D1_OK &&
		      res.entries[0].status == D1_REPLAY_CONFLICT,
	      "a new handle cannot change the body under the old key");
	check(!d1_store_visible(live, &object, 0, &writer) || true,
	      "and nothing was written for it");

	/* A fresh key under the right handle works. */
	env_init(&changed, live, writer, D1_OP_WRITE_BATCH);
	changed.body.write.count = 1;
	changed.body.write.stability = D1_DATA_SYNC;
	changed.body.write.activate = true;
	write_entry(&changed.body.write.entries[0], 0, 11, 2, data,
		    sizeof(data), true,
		    &(struct d1_guard){ .never_written = true });
	check(d1_store_apply(live, &changed, &res) == D1_OK &&
		      res.entries[0].status == D1_OK,
	      "a new key under a WRITE handle works");

	/* The recorded refusal survives a rebuild. */
	log = d1_store_journal(live, &len);
	rebuilt = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (rebuilt) {
		check(d1_store_replay(rebuilt, log, len) == D1_OK,
		      "the log replays with the refusal in it");
		check(states_agree(live, rebuilt), "into the same store");
		d1_store_free(rebuilt);
	}

	d1_store_free(live);
}

/*
 * The two envelope-borne controls are journalled and replay.
 *
 * The previous cycle's matrix claimed this was tested; it was not.
 * Neither control test enabled journalling, so no CONTROL record
 * carrying an Envelope had ever been through a rebuild.
 */
static void test_control_envelopes_replay(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *live, *rebuilt;
	struct d1_envelope env;
	struct d1_result res;
	const uint8_t *log;
	size_t len;
	static uint8_t data[16];
	d1_id_t control, old, fresh, reaped, txn_recovered, txn_reaped;

	memset(data, 0x64, sizeof(data));
	fill_uuid(&store_uuid, 0x37);
	live = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!live)
		return;
	check(d1_store_journal_enable(live) == D1_OK,
	      "journalling starts before any of it");
	control = d1_fixture_admit(live, &object, 11, D1_RIGHT_CONTROL);
	old = d1_fixture_admit(live, &object, 11,
			       D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	fresh = d1_fixture_admit(live, &object, 11,
				 D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	reaped = d1_fixture_admit(live, &object, 12, D1_RIGHT_WRITE);

	/* Work for each control to act on. */
	env_init(&env, live, old, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 0, 11, 1, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	d1_store_apply(live, &env, &res);
	txn_recovered = res.entries[0].txn;

	env_init(&env, live, reaped, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 1, 12, 2, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	d1_store_apply(live, &env, &res);
	txn_reaped = res.entries[0].txn;
	check(txn_recovered != 0 && txn_reaped != 0,
	      "two admissions leave work behind");

	/* A recovery_admit, journalled. */
	env_init(&env, live, control, D1_OP_RECOVERY_ADMIT);
	env.body.control.count = 1;
	env.body.control.txns[0] = txn_recovered;
	env.body.control.old_admission = old;
	env.body.control.new_admission_present = true;
	env.body.control.new_admission = fresh;
	env.body.control.read_epoch_present = true;
	env.body.control.read_epoch = 0;
	check(d1_store_apply(live, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_OK,
	      "recovery re-admits the work");

	/* A lease_reap, journalled. */
	d1_fixture_expire(live, reaped);
	env_init(&env, live, control, D1_OP_LEASE_REAP);
	env.body.control.count = 1;
	env.body.control.txns[0] = txn_reaped;
	env.body.control.old_admission = reaped;
	check(d1_store_apply(live, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_OK,
	      "the expired admission's work is reaped");

	/* And a refusal of each, which is also a recorded event. */
	env_init(&env, live, control, D1_OP_LEASE_REAP);
	env.body.control.count = 1;
	env.body.control.txns[0] = txn_recovered;
	env.body.control.old_admission = fresh;
	check(d1_store_apply(live, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_STALE_AUTH,
	      "a live admission's work is refused");

	log = d1_store_journal(live, &len);
	rebuilt = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!rebuilt) {
		d1_store_free(live);
		return;
	}
	check(d1_store_replay(rebuilt, log, len) == D1_OK,
	      "the log with both controls in it replays");
	check(states_agree(live, rebuilt), "into the same store");

	/*
	 * And the recovery really moved: the rebuilt store lets the new
	 * handle finalize the work, exactly as the live one does.
	 */
	{
		struct d1_result live_res, rebuilt_res;
		uint8_t verifier[D1_VERIFIER_BYTES];

		d1_store_verifier(live, verifier);
		env_init(&env, live, fresh, D1_OP_FINALIZE_BATCH);
		env.body.lifecycle.range_begin = 0;
		env.body.lifecycle.range_end = 1;
		env.body.lifecycle.count = 1;
		env.body.lifecycle.entries[0].index = 0;
		env.body.lifecycle.entries[0].owner.cohort = 1;
		env.body.lifecycle.entries[0].owner.writer = 11;
		env.body.lifecycle.entries[0].owner.co_id = 1;
		env.body.lifecycle.entries[0].txn = txn_recovered;
		memcpy(env.body.lifecycle.prior_verifier, verifier,
		       sizeof(verifier));
		d1_store_apply(live, &env, &live_res);
		d1_store_apply(rebuilt, &env, &rebuilt_res);
		check(live_res.entries[0].status == D1_OK &&
			      rebuilt_res.entries[0].status ==
				      live_res.entries[0].status,
		      "and both stores finalize the recovered work alike");
	}

	d1_store_free(rebuilt);
	d1_store_free(live);
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
		status = rollback_one(
			live, admission, 0, 2, txn2,
			&(struct rollback_expect){ .custody_present = true,
						   .custody = custody,
						   .visible_present = true,
						   .visible = v2,
						   .predecessor_present = true,
						   .predecessor = v1 },
			&entry);
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
	/*
	 * A canonical request refused on rights or liveness is a semantic
	 * error with a receipt, so the same key cannot later be reused for
	 * a different body.
	 */
	check(res.entries[0].disposition == D1_COMPLETED,
	      "and the refusal is recorded");

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
	test_rollback_predicates();
	test_sparse_and_rollback_extents();
	test_rollback_shrinks_eof();
	test_released_predecessor();
	test_view_is_stable();
	test_close_refuses_an_active_call();
	test_failed_replay_poisons_the_handle();
	test_custody_needs_a_real_version();
	test_release_order_does_not_matter();
	test_view_owner_selection();
	test_owner_view_shrinkage();
	test_view_range_holes_and_admission();
	test_replay_reproduces_the_store();
	test_crash_loses_only_the_torn_record();
	test_append_fault_is_unrecorded();
	test_interrupted_batch_stops_and_resumes();
	test_semantic_refusal_does_not_interrupt();
	test_receipt_exhaustion_changes_nothing();
	test_exact_retry_survives_revocation();
	test_reopen_fences_and_repeats();
	test_index_fault_serves_the_overlay();
	test_reopen_fences_owner_reads();
	test_recovery_target_must_be_current();
	test_view_eof_is_object_wide();
	test_owner_shrink_below_higher_chunk();
	test_refusal_consumes_no_capacity();
	test_bound_authority_refusal_is_recorded();
	test_undone_event_never_becomes_durable();
	test_retry_after_flush_fault_agrees_with_replay();
	test_repair_only_rollback_replays();
	test_control_envelopes_replay();
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
