/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * D1 storage model: the ordinary write lifecycle, activation and owner
 * collisions -- the A, B and C traces of the design.
 *
 * The thing each case is really asking is whether a successful write is
 * only a successful write: that durability does not publish, that
 * finalize does not publish, and that the one operation which does
 * publish in a single step says so in its own result.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d1_control.h"
#include "d1_digest.h"
#include "d1_journal.h"
#include "d1_store.h"

static unsigned int failures;

/*
 * The durable journal, as bytes this file owns.
 *
 * The store hands back a snapshot the caller must free.  Rather than
 * unwind an allocation on every early return, the harness keeps the
 * last four and frees the fifth-oldest as it goes: four, because the
 * most any one test holds at once is two, and because a rotation that
 * frees too early is caught immediately by AddressSanitizer rather than
 * quietly read.  journal_release_all() empties it at the end of main.
 */
static uint8_t *held_journals[4];
static unsigned int held_next;

static const uint8_t *journal_of(struct d1_store *s, size_t *len)
{
	uint8_t *out = NULL;

	*len = 0;
	(void)d1_store_journal_snapshot(s, &out, len);
	free(held_journals[held_next]);
	held_journals[held_next] = out;
	held_next = (held_next + 1u) % 4u;
	return out;
}

static void journal_release_all(void)
{
	unsigned int i;

	for (i = 0; i < 4u; i++) {
		free(held_journals[i]);
		held_journals[i] = NULL;
	}
}

static void check(bool ok, const char *what)
{
	if (!ok) {
		failures++;
		fprintf(stderr, "FAIL: %s\n", what);
	}
}

static uint32_t get_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint32_t record_bytes(const uint8_t *r)
{
	return get_be32(r + 12);
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
		     d1_admission_id admission, uint32_t op)
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
	e->owner.cohort.raw = 1;
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
	d1_admission_id admission;
	d1_txn_id txn;
	d1_version_id version;
	d1_version_id visible;
	uint8_t verifier[D1_VERIFIER_BYTES];

	fill_uuid(&store_uuid, 0x90);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	check(s != NULL, "store opens");
	if (!s)
		return;
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_READ | D1_RIGHT_WRITE |
					     D1_RIGHT_SINGLE_WRITER);
	check(d1_admission_live(admission), "fixture issues an admission");
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
	env.body.lifecycle.entries[0].owner.cohort.raw = 1;
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
	env.body.lifecycle.entries[0].owner.cohort.raw = 1;
	env.body.lifecycle.entries[0].owner.writer = 11;
	env.body.lifecycle.entries[0].owner.co_id = 1;
	env.body.lifecycle.entries[0].txn = txn;
	memcpy(env.body.lifecycle.prior_verifier, verifier, sizeof(verifier));
	check(d1_store_apply(s, &env, &res) == D1_OK, "commit applies");
	check(res.entries[0].status == D1_OK &&
		      res.entries[0].phase == D1_PHASE_COMMITTED,
	      "commit reaches COMMITTED");
	check(d1_store_visible(s, &object, 0, &visible) &&
		      d1_version_eq(visible, version),
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
			      d1_version_eq(visible, version),
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
	d1_admission_id single;
	d1_admission_id multi;
	d1_version_id visible;

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
	d1_admission_id admission;
	d1_version_id visible;

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
	d1_admission_id admission;
	d1_admission_id other;

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
	d1_admission_id admission;
	d1_txn_id txn;
	d1_version_id visible;

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
	env.body.lifecycle.entries[0].owner.cohort.raw = 1;
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
	env.body.lifecycle.entries[0].owner.cohort.raw = 1;
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
	d1_admission_id admission;
	d1_version_id visible;

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

/*
 * The same number, named at another store.
 *
 * A handle is a C value bound to one live store; a client's is a bare
 * u64 on the wire.  A client that comes back after a restart presents
 * the number it was given to whatever store is serving now, and this is
 * that: not the old handle, but the old number, at the store that has
 * to decide what it means.
 */
static d1_admission_id renamed_admission(struct d1_store *s, d1_admission_id id)
{
	return d1_fixture_admission_handle(s, d1_admission_raw(id));
}

static d1_txn_id renamed_txn(struct d1_store *s, d1_txn_id id)
{
	return d1_fixture_txn_handle(s, d1_txn_raw(id));
}

static d1_version_id renamed_version(struct d1_store *s, d1_version_id id)
{
	return d1_fixture_version_handle(s, d1_version_raw(id));
}

/*
 * Whether two histories agree about a version.
 *
 * By value, not as handles: a rebuild is a different live store, so its
 * handles are its own, and the raw number is what the log carried and
 * all it carried.
 */
static bool same_version_value(d1_version_id a, d1_version_id b)
{
	return d1_version_raw(a) == d1_version_raw(b);
}

/* Write, finalize and commit one chunk, and answer its version. */
static d1_version_id commit_chunk(struct d1_store *s, d1_admission_id admission,
				  uint64_t index, uint32_t co_id,
				  const uint8_t *data, uint32_t len,
				  const struct d1_guard *expected,
				  d1_version_id predecessor, d1_txn_id *txn_out)
{
	struct d1_envelope env;
	struct d1_result res;
	uint8_t verifier[D1_VERIFIER_BYTES];
	d1_txn_id txn;
	d1_version_id version;
	unsigned int pass;

	d1_store_verifier(s, verifier);
	env_init(&env, s, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], index, 11, co_id, data, len,
		    true, expected);
	if (d1_store_apply(s, &env, &res) != D1_OK ||
	    res.entries[0].status != D1_OK)
		return d1_version_none();
	txn = res.entries[0].txn;
	version = res.entries[0].version;

	for (pass = 0; pass < 2; pass++) {
		env_init(&env, s, admission,
			 pass == 0 ? D1_OP_FINALIZE_BATCH : D1_OP_COMMIT_BATCH);
		env.body.lifecycle.range_begin = index;
		env.body.lifecycle.range_end = index + 1;
		env.body.lifecycle.count = 1;
		env.body.lifecycle.entries[0].index = index;
		env.body.lifecycle.entries[0].owner.cohort.raw = 1;
		env.body.lifecycle.entries[0].owner.writer = 11;
		env.body.lifecycle.entries[0].owner.co_id = co_id;
		env.body.lifecycle.entries[0].txn = txn;
		/*
		 * Finalize and commit name the predecessor the write
		 * recorded; a caller that does not know it is not the
		 * caller that wrote it.
		 */
		env.body.lifecycle.entries[0].predecessor_present =
			d1_version_live(predecessor);
		env.body.lifecycle.entries[0].predecessor = predecessor;
		memcpy(env.body.lifecycle.prior_verifier, verifier,
		       sizeof(verifier));
		if (d1_store_apply(s, &env, &res) != D1_OK ||
		    res.entries[0].status != D1_OK)
			return d1_version_none();
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
	d1_custody_id custody;
	bool visible_present;
	d1_version_id visible;
	bool predecessor_present;
	d1_version_id predecessor;
};

static uint32_t rollback_one(struct d1_store *s, d1_admission_id admission,
			     uint64_t index, uint32_t co_id, d1_txn_id txn,
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
	env.body.rollback.entries[0].owner.cohort.raw = 1;
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
	d1_admission_id admission;
	d1_txn_id txn;
	d1_version_id visible;

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
		d1_txn_id ctxn = { 0 };
		d1_version_id committed_version = { 0 };
		struct d1_entry_result entry;

		check(rollback_one(s, admission, 0, 2, res.entries[0].txn,
				   &(struct rollback_expect){ 0 },
				   NULL) == D1_OK,
		      "the second write is cancelled too");
		committed_version = commit_chunk(
			s, admission, 1, 3, payload_a, sizeof(payload_a),
			&(struct d1_guard){ .never_written = true },
			d1_version_none(), &ctxn);
		check(d1_version_live(committed_version),
		      "a chunk is committed");
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
	d1_admission_id writer_adm;
	d1_admission_id repair_adm;
	d1_version_id v1;
	d1_version_id v2;
	d1_txn_id txn1;
	d1_txn_id txn2;
	d1_custody_id custody;
	d1_version_id visible;

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
			  &(struct d1_guard){ .never_written = true },
			  d1_version_none(), NULL);
	d1_store_guard(s, &object, 0, &guard);
	v2 = commit_chunk(s, writer_adm, 0, 2, data, sizeof(data), &guard, v1,
			  &txn2);
	check(d1_version_live(v1) && d1_version_live(v2),
	      "a version is replaced");

	/*
	 * A wrong expected predecessor, reached with valid custody.  With
	 * a bogus custody handle the call fails on custody first and never
	 * reaches the comparison, which is how this row used to pass for
	 * the wrong reason.
	 */
	custody = d1_fixture_custody(s, v2);
	check(rollback_one(s, repair_adm, 0, 2, txn2,
			   &(struct rollback_expect){
				   .custody_present = true,
				   .custody = custody,
				   .visible_present = true,
				   .visible = v2,
				   .predecessor_present = true,
				   .predecessor = d1_fixture_version_handle(
					   s, 999999) },
			   &entry) == D1_NO_PREDECESSOR,
	      "a wrong expected predecessor answers NO_PREDECESSOR");
	check(d1_store_visible(s, &object, 0, &visible) &&
		      d1_version_eq(visible, v2),
	      "and changes nothing");

	/* An absent expected predecessor, where one was recorded. */
	check(rollback_one(s, repair_adm, 0, 2, txn2,
			   &(struct rollback_expect){ .custody_present = true,
						      .custody = custody,
						      .visible_present = true,
						      .visible = v2 },
			   &entry) == D1_NO_PREDECESSOR,
	      "an absent expected predecessor is a mismatch too");
	check(d1_store_visible(s, &object, 0, &visible) &&
		      d1_version_eq(visible, v2),
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
	check(d1_store_visible(s, &object, 0, &visible) &&
		      d1_version_eq(visible, v2),
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
	check(d1_store_visible(s, &object, 0, &visible) &&
		      d1_version_eq(visible, v1),
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
	check(d1_txn_live(txn1), "a new private transaction is prepared");

	/* The private path compares its expectations too. */
	check(rollback_one(s, writer_adm, 0, 3, txn1,
			   &(struct rollback_expect){
				   .visible_present = true,
				   .visible = v1,
				   .predecessor_present = true,
				   .predecessor = d1_fixture_version_handle(
					   s, 999999) },
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
	d1_admission_id admission;
	d1_version_id v3;
	d1_version_id v4;
	d1_version_id v5;
	d1_txn_id txn4;
	d1_custody_id custody;
	d1_version_id visible;
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
			  &(struct d1_guard){ .never_written = true },
			  d1_version_none(), NULL);
	check(d1_version_live(v3), "the sparse chunk commits");
	check(d1_store_eof(s, &object) == 3 * CHUNK_BYTES + sizeof(small),
	      "EOF is the end of the only interval");
	n = d1_store_holes(s, &object, holes, D1_MAX_INTERVALS);
	check(n == 1 && holes[0].start == 0 && holes[0].end == 3 * CHUNK_BYTES,
	      "everything before it is an explicit hole");

	/* D2: a later chunk 5 of 50 bytes. */
	v5 = commit_chunk(s, admission, 5, 2, tiny, sizeof(tiny),
			  &(struct d1_guard){ .never_written = true },
			  d1_version_none(), NULL);
	check(d1_version_live(v5), "the later chunk commits");
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
	check(d1_version_live(v4) && !d1_version_eq(v4, v3),
	      "the replacement commits");
	check(d1_store_eof(s, &object) == 5 * CHUNK_BYTES + sizeof(tiny),
	      "the higher chunk still sets EOF");

	custody = d1_fixture_custody(s, v4);
	check(d1_custody_live(custody), "the fixture issues custody over it");
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
	check(d1_store_visible(s, &object, 3, &visible) &&
		      d1_version_eq(visible, v3),
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
	d1_admission_id admission;
	d1_version_id v3;
	d1_version_id v4;
	d1_txn_id txn4;
	d1_custody_id custody;
	d1_version_id visible;

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
			  &(struct d1_guard){ .never_written = true },
			  d1_version_none(), NULL);
	check(d1_store_eof(s, &object) == 3 * CHUNK_BYTES + sizeof(small),
	      "the short image sets EOF");
	d1_store_guard(s, &object, 3, &guard);
	v4 = commit_chunk(s, admission, 3, 2, big, sizeof(big), &guard, v3,
			  &txn4);
	check(d1_version_live(v4), "the full image commits");
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
	check(d1_store_visible(s, &object, 3, &visible) &&
		      d1_version_eq(visible, v3),
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
	d1_admission_id admission;
	d1_version_id v1;
	d1_version_id v2;
	d1_txn_id txn2;
	d1_custody_id custody;
	d1_version_id visible;

	memset(data, 0x66, sizeof(data));
	fill_uuid(&store_uuid, 0x33);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_WRITE | D1_RIGHT_REPAIR |
					     D1_RIGHT_SINGLE_WRITER);

	v1 = commit_chunk(s, admission, 0, 1, data, sizeof(data),
			  &(struct d1_guard){ .never_written = true },
			  d1_version_none(), NULL);
	d1_store_guard(s, &object, 0, &guard);
	v2 = commit_chunk(s, admission, 0, 2, data, sizeof(data), &guard, v1,
			  &txn2);
	check(d1_version_live(v1) && d1_version_live(v2),
	      "two versions commit in turn");

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
	check(d1_store_visible(s, &object, 0, &visible) &&
		      d1_version_eq(visible, v2),
	      "and the current data stays exactly where it is");

	d1_store_free(s);
}

/* Write and finalize one chunk, stopping short of commit. */
static d1_version_id
finalize_chunk(struct d1_store *s, d1_admission_id admission, uint64_t index,
	       uint32_t co_id, const uint8_t *data, uint32_t len,
	       const struct d1_guard *expected, d1_version_id predecessor,
	       d1_txn_id *txn_out)
{
	struct d1_envelope env;
	struct d1_result res;
	uint8_t verifier[D1_VERIFIER_BYTES];
	d1_txn_id txn;
	d1_version_id version;

	d1_store_verifier(s, verifier);
	env_init(&env, s, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], index, 11, co_id, data, len,
		    true, expected);
	if (d1_store_apply(s, &env, &res) != D1_OK ||
	    res.entries[0].status != D1_OK)
		return d1_version_none();
	txn = res.entries[0].txn;
	version = res.entries[0].version;

	env_init(&env, s, admission, D1_OP_FINALIZE_BATCH);
	env.body.lifecycle.range_begin = index;
	env.body.lifecycle.range_end = index + 1;
	env.body.lifecycle.count = 1;
	env.body.lifecycle.entries[0].index = index;
	env.body.lifecycle.entries[0].owner.cohort.raw = 1;
	env.body.lifecycle.entries[0].owner.writer = 11;
	env.body.lifecycle.entries[0].owner.co_id = co_id;
	env.body.lifecycle.entries[0].txn = txn;
	env.body.lifecycle.entries[0].predecessor_present =
		d1_version_live(predecessor);
	env.body.lifecycle.entries[0].predecessor = predecessor;
	memcpy(env.body.lifecycle.prior_verifier, verifier, sizeof(verifier));
	if (d1_store_apply(s, &env, &res) != D1_OK ||
	    res.entries[0].status != D1_OK)
		return d1_version_none();
	if (txn_out)
		*txn_out = txn;
	return version;
}

/* Finalize a transaction that already exists, and report how it went. */
static uint32_t finalize_txn(struct d1_store *s, d1_admission_id admission,
			     uint64_t index, uint32_t writer, uint32_t co_id,
			     d1_txn_id txn)
{
	struct d1_envelope env;
	struct d1_result res;
	uint8_t verifier[D1_VERIFIER_BYTES];
	uint32_t status;

	d1_store_verifier(s, verifier);
	env_init(&env, s, admission, D1_OP_FINALIZE_BATCH);
	env.body.lifecycle.range_begin = index;
	env.body.lifecycle.range_end = index + 1;
	env.body.lifecycle.count = 1;
	env.body.lifecycle.entries[0].index = index;
	env.body.lifecycle.entries[0].owner.cohort.raw = 1;
	env.body.lifecycle.entries[0].owner.writer = writer;
	env.body.lifecycle.entries[0].owner.co_id = co_id;
	env.body.lifecycle.entries[0].txn = txn;
	memcpy(env.body.lifecycle.prior_verifier, verifier, sizeof(verifier));
	status = d1_store_apply(s, &env, &res);
	if (status != D1_OK)
		return status;
	return res.entries[0].status;
}

/* An ordinary selection over a byte range. */
static void ordinary_sel(struct d1_selection_spec *sel)
{
	memset(sel, 0, sizeof(*sel));
	sel->selection = D1_SELECT_ORDINARY;
}

/* An owner selection naming one transaction it claims. */
static void owner_sel(struct d1_selection_spec *sel, d1_txn_id txn,
		      uint32_t writer, uint32_t co_id, uint64_t read_epoch)
{
	memset(sel, 0, sizeof(*sel));
	sel->selection = D1_SELECT_OWNER;
	sel->count = 1;
	sel->txns[0] = txn;
	sel->owners[0].cohort.raw = 1;
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
	d1_admission_id admission;
	d1_version_id v1;
	d1_version_id v2;
	d1_version_id seen;

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
			  &(struct d1_guard){ .never_written = true },
			  d1_version_none(), NULL);
	check(d1_version_live(v1), "the first version commits");

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
	check(d1_version_live(v2) && !d1_version_eq(v2, v1),
	      "a second version commits over it");
	check(d1_view_version(view, 0, &seen) && d1_version_eq(seen, v1),
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
	d1_view_close(view);

	ordinary_sel(&sel);
	check(d1_view_open(s, &object, admission, &sel, 0, CHUNK_BYTES,
			   &after) == D1_OK,
	      "a later view opens");
	check(d1_view_version(after, 0, &seen) && d1_version_eq(seen, v2),
	      "and names the newer version");
	d1_view_close(after);
	check(d1_store_close(s) == D1_OK,
	      "and the close succeeds once nothing is outstanding");
	check(d1_store_destroy(s) == D1_OK, "and then it is destroyed");
}

/*
 * Normal close refuses while a call is in flight, not only while a view
 * is open.
 *
 * A batch releases the lock between members so the next one is
 * revalidated against what the last one left.  A close that ran in that
 * gap saw no views, freed the store, and left the caller to lock a
 * destroyed mutex.
 *
 * This used to be held by a pair of fixture calls that entered and left
 * the bracket without a thread.  They were an ownership pair the caller
 * had to get right and nothing checked: leaving on one store gave back
 * a hold taken on another, so a real worker's hold could be handed away
 * and its store closed and destroyed underneath it.  The hold is a real
 * worker's now, parked between two members of its own batch, which is
 * the state the fixture pair was standing in for anyway.
 */
struct parked_member {
	struct d1_store *s;
	struct d1_envelope env;
	struct d1_result out;
	uint32_t status;
	pthread_mutex_t m;
	pthread_cond_t cv;
	bool parked;
	bool resume;
	/* Which primitive failed inside the worker, if one did. */
	unsigned int err;
};

static void hold_between_members(void *arg)
{
	struct parked_member *p = arg;

	if (pthread_mutex_lock(&p->m) != 0) {
		p->err = 1;
		return;
	}
	p->parked = true;
	if (pthread_cond_signal(&p->cv) != 0)
		p->err = 2;
	while (!p->resume && !p->err)
		if (pthread_cond_wait(&p->cv, &p->m) != 0)
			p->err = 3;
	if (pthread_mutex_unlock(&p->m) != 0)
		p->err = 4;
}

static void *run_batch(void *arg)
{
	struct parked_member *p = arg;

	p->status = d1_store_apply(p->s, &p->env, &p->out);
	return NULL;
}

/* Let a parked worker go, whether or not anything else went well. */
static void release_parked_member(struct parked_member *p)
{
	if (pthread_mutex_lock(&p->m) != 0) {
		p->err = 5;
		return;
	}
	p->resume = true;
	if (pthread_cond_signal(&p->cv) != 0)
		p->err = 6;
	if (pthread_mutex_unlock(&p->m) != 0)
		p->err = 7;
}

static void test_close_refuses_an_active_call(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_selection_spec sel;
	struct d1_view *view = NULL;
	struct parked_member p;
	pthread_t worker;
	static uint8_t data[16];
	d1_admission_id admission;
	d1_version_id seen;

	memset(data, 0x5a, sizeof(data));
	fill_uuid(&store_uuid, 0x41);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_READ | D1_RIGHT_WRITE |
					     D1_RIGHT_SINGLE_WRITER);
	check(d1_version_live(
		      commit_chunk(s, admission, 0, 1, data, sizeof(data),
				   &(struct d1_guard){ .never_written = true },
				   d1_version_none(), NULL)),
	      "there is something to read");

	check(d1_store_destroy(s) == D1_BUSY,
	      "destroying a store that was never closed is refused");

	/*
	 * A two-member batch, parked in the gap between them.  Member 0
	 * activates as it writes, so while the worker is held the store
	 * shows the first index published and the second untouched: the
	 * call is admitted, and it is between members.
	 */
	memset(&p, 0, sizeof(p));
	p.s = s;
	env_init(&p.env, s, admission, D1_OP_WRITE_BATCH);
	p.env.body.write.count = 2;
	p.env.body.write.stability = D1_FILE_SYNC;
	p.env.body.write.activate = true;
	write_entry(&p.env.body.write.entries[0], 1, 11, 2, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	write_entry(&p.env.body.write.entries[1], 2, 11, 3, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });

	if (pthread_mutex_init(&p.m, NULL) != 0) {
		check(false, "the worker's mutex starts");
		d1_store_free(s);
		return;
	}
	if (pthread_cond_init(&p.cv, NULL) != 0) {
		check(false, "the worker's condition starts");
		check(pthread_mutex_destroy(&p.m) == 0, "and is released");
		d1_store_free(s);
		return;
	}
	d1_fixture_before_member(s, 1, hold_between_members, &p);
	if (pthread_create(&worker, NULL, run_batch, &p) != 0) {
		check(false, "the worker starts");
		d1_fixture_before_member(s, 1, NULL, NULL);
		check(pthread_cond_destroy(&p.cv) == 0,
		      "its condition is released");
		check(pthread_mutex_destroy(&p.m) == 0, "and its mutex");
		d1_store_free(s);
		return;
	}
	if (pthread_mutex_lock(&p.m) != 0) {
		p.err = 8;
	} else {
		while (!p.parked && !p.err)
			if (pthread_cond_wait(&p.cv, &p.m) != 0)
				p.err = 9;
		if (pthread_mutex_unlock(&p.m) != 0)
			p.err = 10;
	}
	if (p.err) {
		check(false, "the worker parks between members");
		release_parked_member(&p);
		check(pthread_join(worker, NULL) == 0, "and is joined anyway");
		check(pthread_cond_destroy(&p.cv) == 0,
		      "its condition is released");
		check(pthread_mutex_destroy(&p.m) == 0, "and its mutex");
		d1_store_free(s);
		return;
	}

	check(d1_store_visible(s, &object, 1, &seen),
	      "member 0 of the parked batch has published");
	check(!d1_store_visible(s, &object, 2, &seen),
	      "and member 1 has not run yet");

	/* The worker's own hold. */
	check(d1_store_close(s) == D1_BUSY,
	      "a close during an admitted call is refused");

	/* A view outstanding at the same time is refused for its own reason. */
	ordinary_sel(&sel);
	check(d1_view_open(s, &object, admission, &sel, 0, CHUNK_BYTES,
			   &view) == D1_OK,
	      "a view opens while the call is in flight");
	check(d1_store_close(s) == D1_BUSY, "and the close is still refused");
	d1_view_close(view);
	view = NULL;
	check(d1_store_close(s) == D1_BUSY,
	      "closing the view is not enough while the call is in flight");

	/* The call returns, and it returns a real result. */
	release_parked_member(&p);
	check(pthread_join(worker, NULL) == 0, "the worker is joined");
	check(p.err == 0, "with every primitive inside it succeeding");
	check(p.status == D1_OK && p.out.count == 2 &&
		      p.out.entries[0].status == D1_OK &&
		      p.out.entries[1].status == D1_OK,
	      "and both members of its batch completed");
	check(d1_store_visible(s, &object, 2, &seen),
	      "so member 1 published after the release");

	/* And a view alone still holds the store open, for its own reason. */
	check(d1_view_open(s, &object, admission, &sel, 0, CHUNK_BYTES,
			   &view) == D1_OK,
	      "a view opens again with no call in flight");
	check(d1_store_close(s) == D1_BUSY,
	      "and the close is refused for the view alone");
	d1_view_close(view);

	check(d1_store_close(s) == D1_OK,
	      "and the close succeeds once the call has returned");
	check(d1_store_destroy(s) == D1_OK, "and then it is destroyed");
	check(pthread_cond_destroy(&p.cv) == 0, "the condition is destroyed");
	check(pthread_mutex_destroy(&p.m) == 0, "and the mutex with it");
}

/*
 * A close wins the race with a call that has not been admitted yet, and
 * the call still lands somewhere valid.
 *
 * The fixture pair above holds what a paused call holds, but it can
 * only pause a call that was already admitted.  The interval this one
 * needs is the earlier one: a real thread inside d1_store_apply which
 * has not yet reached the lock where calls are counted.  A close that
 * freed the store there left that thread to lock destroyed memory,
 * which is why a close no longer frees.  The thread is joined before
 * the store is destroyed, because that is the destroy contract.
 */
struct parked_call {
	struct d1_store *s;
	struct d1_envelope env;
	struct d1_result out;
	uint32_t status;
	pthread_mutex_t m;
	pthread_cond_t cv;
	bool parked;
	bool resume;
	/*
	 * Which primitive failed, if one did.  The parked thread cannot
	 * call check() -- the counter it keeps is not shared state -- so
	 * it records the step here and the main thread reports it after
	 * the join.
	 */
	unsigned int err;
};

static void park_at_the_door(void *arg)
{
	struct parked_call *p = arg;

	if (pthread_mutex_lock(&p->m) != 0) {
		p->err = 1;
		return;
	}
	p->parked = true;
	if (pthread_cond_signal(&p->cv) != 0)
		p->err = 2;
	while (!p->resume && !p->err)
		if (pthread_cond_wait(&p->cv, &p->m) != 0)
			p->err = 3;
	if (pthread_mutex_unlock(&p->m) != 0)
		p->err = 4;
}

static void *run_parked_call(void *arg)
{
	struct parked_call *p = arg;

	p->status = d1_store_apply(p->s, &p->env, &p->out);
	return NULL;
}

/*
 * Set a caller going and wait until it is parked at the door.
 *
 * Every step that can fail is checked, and a step that failed must not
 * be waited on: there is no worker to signal the condition variable, so
 * the wait below would never end.  Only what was initialized is
 * released, and the arm is dropped, so a failed setup leaves the store
 * exactly as it found it.  @parked_setup_fault fails one step on
 * purpose, so those paths are executed rather than argued about.
 */
static unsigned int parked_setup_fault;

static bool parked_step_fails(unsigned int step)
{
	return parked_setup_fault == step;
}

static bool park_a_caller(struct parked_call *p, struct d1_store *s,
			  d1_admission_id admission, pthread_t *caller)
{
	memset(p, 0, sizeof(*p));
	p->s = s;
	env_init(&p->env, s, admission, D1_OP_WRITE_BATCH);
	p->env.body.write.count = 1;
	p->env.body.write.stability = D1_FILE_SYNC;
	write_entry(&p->env.body.write.entries[0], 0, 11, 1, payload_a,
		    sizeof(payload_a), true,
		    &(struct d1_guard){ .never_written = true });

	if (parked_step_fails(1) || pthread_mutex_init(&p->m, NULL) != 0)
		return false;
	if (parked_step_fails(2) || pthread_cond_init(&p->cv, NULL) != 0) {
		if (pthread_mutex_destroy(&p->m) != 0)
			p->err = 5;
		return false;
	}
	if (d1_fixture_before_admission(s, park_at_the_door, p) != D1_OK) {
		p->err = 6;
		if (pthread_cond_destroy(&p->cv) != 0)
			p->err = 7;
		if (pthread_mutex_destroy(&p->m) != 0)
			p->err = 8;
		return false;
	}
	if (parked_step_fails(3) ||
	    pthread_create(caller, NULL, run_parked_call, p) != 0) {
		if (d1_fixture_before_admission(s, NULL, NULL) != D1_OK)
			p->err = 9;
		if (pthread_cond_destroy(&p->cv) != 0)
			p->err = 10;
		if (pthread_mutex_destroy(&p->m) != 0)
			p->err = 11;
		return false;
	}
	if (pthread_mutex_lock(&p->m) != 0) {
		p->err = 12;
		return true;
	}
	while (!p->parked && !p->err)
		if (pthread_cond_wait(&p->cv, &p->m) != 0)
			p->err = 13;
	if (pthread_mutex_unlock(&p->m) != 0)
		p->err = 14;
	return true;
}

static void test_close_does_not_destroy_under_an_arriving_call(void)
{
	static const char *const step[] = {
		"", "a caller whose mutex will not start is not waited for",
		"a caller whose condition will not start is not waited for",
		"a caller whose thread will not start is not waited for"
	};
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct parked_call p;
	pthread_t caller;
	unsigned int fault;
	d1_admission_id admission;
	d1_version_id seen;

	fill_uuid(&store_uuid, 0x4e);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_READ | D1_RIGHT_WRITE |
					     D1_RIGHT_SINGLE_WRITER);

	/* Each step of the setup, failed on purpose: report, never wait. */
	for (fault = 1; fault <= 3; fault++) {
		parked_setup_fault = fault;
		check(!park_a_caller(&p, s, admission, &caller), step[fault]);
		check(!p.parked, "and there is no caller to have waited for");
		parked_setup_fault = 0;
	}

	/* The caller is inside d1_store_apply and has not been admitted. */
	check(park_a_caller(&p, s, admission, &caller),
	      "a real caller enters the store and parks at the door");

	check(d1_store_close(s) == D1_OK,
	      "a close wins against a call that is not admitted yet");

	/* It resumes, and it must land on a store that is still there. */
	check(pthread_mutex_lock(&p.m) == 0, "the resume takes the lock");
	p.resume = true;
	check(pthread_cond_signal(&p.cv) == 0, "and signals the caller");
	check(pthread_mutex_unlock(&p.m) == 0, "and lets the lock go");
	check(pthread_join(caller, NULL) == 0, "and the caller is joined");
	check(p.err == 0, "with every primitive inside it succeeding");
	check(p.status == D1_INVALID,
	      "the arriving call is refused rather than admitted");
	check(p.out.count == 0, "and it recorded nothing");
	check(!d1_store_visible(s, &object, 0, &seen),
	      "and a closed store answers no observer");

	/* Only now, with the caller joined, does the memory go. */
	check(d1_store_destroy(s) == D1_OK,
	      "and destruction follows the join, not the close");
	check(pthread_cond_destroy(&p.cv) == 0, "the condition is destroyed");
	check(pthread_mutex_destroy(&p.m) == 0, "and the mutex with it");
}

/*
 * A view is released through the store it was opened on.
 *
 * The release used to take the store as well, with nothing saying the
 * two had to match and nothing checking that they did.  Naming the
 * wrong store unpinned in one store while clearing a view out of
 * another, and the store that really held the view then saw none: it
 * closed, it destroyed, and the caller was left holding a view into
 * freed memory.  There is no store argument now, so there is no
 * mismatch to make.
 *
 * The property this keeps is the one the close fence is for: while a
 * view is live its store refuses to close, and only releasing that view
 * lets it.
 */
static void test_a_view_is_released_through_its_own_store(void)
{
	struct d1_uuid uuid_a, uuid_b;
	struct d1_store *a, *b;
	struct d1_selection_spec sel;
	struct d1_view *view = NULL, *other = NULL, *again = NULL;
	static uint8_t data_a[16];
	static uint8_t data_b[16];
	static uint8_t got[32];
	uint32_t got_len = 0;
	d1_admission_id admit_a;
	d1_admission_id admit_b;
	d1_version_id seen;

	memset(data_a, 0x2a, sizeof(data_a));
	memset(data_b, 0x2b, sizeof(data_b));
	fill_uuid(&uuid_a, 0x2a);
	fill_uuid(&uuid_b, 0x2b);
	a = d1_store_open(&uuid_a, CHUNK_BYTES, MAX_FILE_BYTES);
	b = d1_store_open(&uuid_b, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!a || !b) {
		d1_store_free(a);
		d1_store_free(b);
		return;
	}
	admit_a = d1_fixture_admit(a, &object, 11,
				   D1_RIGHT_READ | D1_RIGHT_WRITE |
					   D1_RIGHT_SINGLE_WRITER);
	admit_b = d1_fixture_admit(b, &object, 11,
				   D1_RIGHT_READ | D1_RIGHT_WRITE |
					   D1_RIGHT_SINGLE_WRITER);
	check(d1_version_live(
		      commit_chunk(a, admit_a, 0, 1, data_a, sizeof(data_a),
				   &(struct d1_guard){ .never_written = true },
				   d1_version_none(), NULL)),
	      "A has something to pin");
	check(d1_version_live(
		      commit_chunk(b, admit_b, 0, 1, data_b, sizeof(data_b),
				   &(struct d1_guard){ .never_written = true },
				   d1_version_none(), NULL)),
	      "and so has B");

	/* A pinned view of A, and a view of B to watch for damage. */
	ordinary_sel(&sel);
	check(d1_view_open(a, &object, admit_a, &sel, 0, CHUNK_BYTES, &view) ==
		      D1_OK,
	      "a view of A opens and pins");
	check(d1_view_open(b, &object, admit_b, &sel, 0, CHUNK_BYTES, &other) ==
		      D1_OK,
	      "and one of B beside it");
	check(d1_store_close(a) == D1_BUSY,
	      "A refuses to close while its view is live");

	/* Releasing it needs no store, so it cannot name the wrong one. */
	d1_view_close(view);
	check(d1_store_close(a) == D1_OK, "and closes once the view is gone");
	check(d1_store_destroy(a) == D1_OK, "and destroys");

	/* B is exactly as it was: its view still reads, and still pins. */
	check(d1_view_version(other, 0, &seen),
	      "B's view still names its version");
	check(d1_view_eof(other) == sizeof(data_b), "and its EOF is B's");
	check(d1_view_read(other, 0, got, sizeof(got), &got_len) == D1_OK &&
		      got_len == sizeof(data_b) &&
		      memcmp(got, data_b, sizeof(data_b)) == 0,
	      "and it reads B's bytes, not A's");
	check(d1_store_close(b) == D1_BUSY, "and B is still held open by it");
	d1_view_close(other);

	/* And B has lost no capacity: it opens another view, and writes. */
	check(d1_view_open(b, &object, admit_b, &sel, 0, CHUNK_BYTES, &again) ==
		      D1_OK,
	      "B opens another view");
	check(d1_view_eof(again) == sizeof(data_b), "with the same EOF");
	d1_view_close(again);
	check(d1_version_live(
		      commit_chunk(b, admit_b, 1, 2, data_b, sizeof(data_b),
				   &(struct d1_guard){ .never_written = true },
				   d1_version_none(), NULL)),
	      "and still takes a write");
	check(d1_store_close(b) == D1_OK && d1_store_destroy(b) == D1_OK,
	      "then B closes and destroys of its own accord");
}

/* A door hook that only counts, for the arms that must not be run. */
struct door_count {
	unsigned int fired;
};

static void count_the_door(void *arg)
{
	((struct door_count *)arg)->fired++;
}

/*
 * The door arm belongs to the store it was aimed at.
 *
 * Its storage has to be outside the store -- that is the whole point of
 * it, because the store is what a close may destroy underneath an
 * arriving call.  Outside the store is not the same as belonging to no
 * store, though.  An arm left over from another store, or from a run
 * that has ended, is not something an unrelated call should be made to
 * run: its argument may no longer be alive, and a real callback changes
 * what the store it fires on goes on to answer.  So the arm names its
 * target, and every path that ends a store or a run forgets it.
 */
static void test_the_door_arm_belongs_to_its_store(void)
{
	struct d1_uuid uuid_a, uuid_b;
	struct d1_store *a, *b, *target;
	struct door_count seen, other_seen;
	struct d1_envelope env;
	struct d1_result res;
	static uint8_t data[16];
	const uint8_t *log;
	size_t len;
	d1_admission_id admit_a;
	d1_admission_id admit_b;

	memset(data, 0x3c, sizeof(data));
	fill_uuid(&uuid_a, 0x3c);
	fill_uuid(&uuid_b, 0x4c);

	/* One store's arm is not the next store's to consume. */
	a = d1_store_open(&uuid_a, CHUNK_BYTES, MAX_FILE_BYTES);
	b = d1_store_open(&uuid_b, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!a || !b) {
		d1_store_free(a);
		d1_store_free(b);
		return;
	}
	admit_a = d1_fixture_admit(a, &object, 11,
				   D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	admit_b = d1_fixture_admit(b, &object, 11,
				   D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	memset(&seen, 0, sizeof(seen));
	d1_fixture_before_admission(a, count_the_door, &seen);

	env_init(&env, b, admit_b, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 0, 11, 1, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	check(d1_store_apply(b, &env, &res) == D1_OK && seen.fired == 0,
	      "a call on another store neither runs the arm nor takes it");

	env_init(&env, a, admit_a, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 0, 11, 1, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	check(d1_store_apply(a, &env, &res) == D1_OK && seen.fired == 1,
	      "and the store it was aimed at still runs it, once");
	check(d1_store_apply(a, &env, &res) == D1_OK && seen.fired == 1,
	      "and only once");

	/*
	 * One slot for the process, and it says so.  A second store cannot
	 * take it while the first holds it, and the refusal leaves the
	 * first exactly as it was rather than quietly dropping it.
	 */
	memset(&seen, 0, sizeof(seen));
	memset(&other_seen, 0, sizeof(other_seen));
	check(d1_fixture_before_admission(a, count_the_door, &seen) == D1_OK,
	      "A takes the slot");
	check(d1_fixture_before_admission(b, count_the_door, &other_seen) ==
		      D1_BUSY,
	      "B is told the slot is taken");
	check(d1_fixture_before_admission(a, count_the_door, &seen) == D1_OK,
	      "while A may replace its own callback");

	/* Disarming B's arm is not disarming A's. */
	check(d1_fixture_before_admission(b, NULL, NULL) == D1_OK,
	      "B disarms what it does not have");
	check(d1_store_apply(a, &env, &res) == D1_OK && seen.fired == 1 &&
		      other_seen.fired == 0,
	      "and A's arm is still A's");

	/* Nor is forgetting B's arm on the way out. */
	memset(&seen, 0, sizeof(seen));
	check(d1_fixture_before_admission(a, count_the_door, &seen) == D1_OK,
	      "A takes the slot again");
	check(d1_store_close(b) == D1_OK && d1_store_destroy(b) == D1_OK,
	      "B closes and is destroyed");
	check(d1_store_apply(a, &env, &res) == D1_OK && seen.fired == 1,
	      "and A's arm survived B's teardown");
	b = d1_store_open(&uuid_b, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!b) {
		d1_store_free(a);
		return;
	}
	admit_b = d1_fixture_admit(b, &object, 11,
				   D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);

	/*
	 * A close forgets it, and the proof needs no second store: the
	 * door runs before the closed fence is read, so an arm left on a
	 * closed store would fire for a call that is about to be refused.
	 */
	memset(&seen, 0, sizeof(seen));
	d1_fixture_before_admission(a, count_the_door, &seen);
	check(d1_store_close(a) == D1_OK, "the armed store closes");
	check(d1_store_apply(a, &env, &res) == D1_INVALID && seen.fired == 0,
	      "and a call it refuses does not run the arm it forgot");
	check(d1_store_destroy(a) == D1_OK, "and it is destroyed");
	d1_store_free(b);

	/*
	 * Crash teardown forgets it too, and there the hazard is the
	 * address: whatever is allocated next may be where the store was.
	 * This host's allocator hands it straight back, so the leg below
	 * runs; on one that does not, there is nothing to inherit the arm
	 * and the leg is skipped rather than faked.
	 */
	a = d1_store_open(&uuid_a, CHUNK_BYTES, MAX_FILE_BYTES);
	if (a) {
		struct d1_store *again;

		memset(&seen, 0, sizeof(seen));
		d1_fixture_before_admission(a, count_the_door, &seen);
		d1_store_free(a);
		again = d1_store_open(&uuid_a, CHUNK_BYTES, MAX_FILE_BYTES);
		if (again == a) {
			admit_a = d1_fixture_admit(
				again, &object, 11,
				D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
			env_init(&env, again, admit_a, D1_OP_WRITE_BATCH);
			env.body.write.count = 1;
			env.body.write.stability = D1_FILE_SYNC;
			write_entry(&env.body.write.entries[0], 0, 11, 1, data,
				    sizeof(data), true,
				    &(struct d1_guard){ .never_written =
								true });
			check(d1_store_apply(again, &env, &res) == D1_OK &&
				      seen.fired == 0,
			      "and its arm is not inherited with its address");
		}
		d1_store_free(again);
	}

	/* And reconstruction forgets it, as it forgets every other arm. */
	a = d1_store_open(&uuid_a, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!a)
		return;
	d1_store_journal_enable(a);
	admit_a = d1_fixture_admit(a, &object, 11,
				   D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	check(d1_version_live(
		      commit_chunk(a, admit_a, 0, 1, data, sizeof(data),
				   &(struct d1_guard){ .never_written = true },
				   d1_version_none(), NULL)),
	      "a history to rebuild");
	log = journal_of(a, &len);

	target = d1_store_open(&uuid_a, CHUNK_BYTES, MAX_FILE_BYTES);
	if (target) {
		memset(&seen, 0, sizeof(seen));
		d1_fixture_before_admission(target, count_the_door, &seen);
		check(d1_store_replay(target, log, len) == D1_OK &&
			      seen.fired == 0,
		      "the rebuild itself does not run it");
		check(d1_version_live(commit_chunk(
			      target, renamed_admission(target, admit_a), 2, 9,
			      data, sizeof(data),
			      &(struct d1_guard){ .never_written = true },
			      d1_version_none(), NULL)) &&
			      seen.fired == 0,
		      "and the first call after the rebuild does not either");
		d1_store_free(target);
	}
	target = d1_store_open(&uuid_a, CHUNK_BYTES, MAX_FILE_BYTES);
	if (target) {
		d1_admission_id fresh;

		memset(&seen, 0, sizeof(seen));
		d1_fixture_before_admission(target, count_the_door, &seen);
		check(d1_store_reopen(target, log, len) == D1_OK &&
			      seen.fired == 0,
		      "nor does the reopen");
		fresh = d1_fixture_admit(target, &object, 11,
					 D1_RIGHT_WRITE |
						 D1_RIGHT_SINGLE_WRITER);
		check(d1_version_live(commit_chunk(
			      target, fresh, 3, 9, data, sizeof(data),
			      &(struct d1_guard){ .never_written = true },
			      d1_version_none(), NULL)) &&
			      seen.fired == 0,
		      "nor the first call after it");
		d1_store_free(target);
	}

	/*
	 * Arming is refused where every other fixture arm is refused, and
	 * the proof has to call the store it was refused on: destroying it
	 * forgets an arm the gate should never have taken, so a leg that
	 * destroys first passes with the gate removed.  The door runs
	 * before the closed fence is read, so a wrongly accepted arm fires
	 * for a call the store is about to refuse.
	 */
	memset(&seen, 0, sizeof(seen));
	check(d1_store_close(a) == D1_OK, "the armed store closes");
	check(d1_fixture_before_admission(a, count_the_door, &seen) ==
		      D1_INVALID,
	      "a closed store takes no arm");
	check(d1_store_apply(a, &env, &res) == D1_INVALID && seen.fired == 0,
	      "and the call it refuses runs nothing");
	check(d1_store_destroy(a) == D1_OK, "and it destroys");

	/* The same of a poisoned one, which a failed rebuild leaves. */
	a = d1_store_open(&uuid_a, CHUNK_BYTES, MAX_FILE_BYTES);
	if (a && len > 8u) {
		static uint8_t torn[1u << 16];

		memcpy(torn, log, len);
		torn[len - 1u] ^= 0xffu;
		check(d1_store_replay(a, torn, len) == D1_IO,
		      "a corrupt log poisons its target");
		memset(&seen, 0, sizeof(seen));
		check(d1_fixture_before_admission(a, count_the_door, &seen) ==
			      D1_INVALID,
		      "and a poisoned store takes no arm");
		check(d1_store_apply(a, &env, &res) == D1_INVALID &&
			      seen.fired == 0,
		      "nor runs one for the call it refuses");
		d1_store_free(a);
	} else {
		d1_store_free(a);
	}
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
	struct d1_guard guard;
	struct d1_interval holes[4];
	uint8_t verifier[D1_VERIFIER_BYTES];
	const uint8_t *log;
	size_t len, badlen;
	uint8_t *writable;
	static uint8_t data[16];
	d1_admission_id admission;
	d1_version_id seen;
	unsigned int i;
	bool zero = true;

	memset(data, 0x77, sizeof(data));
	fill_uuid(&store_uuid, 0x42);
	live = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!live)
		return;
	d1_store_journal_enable(live);
	admission = d1_fixture_admit(live, &object, 11,
				     D1_RIGHT_READ | D1_RIGHT_WRITE |
					     D1_RIGHT_SINGLE_WRITER);
	check(d1_version_live(
		      commit_chunk(live, admission, 0, 1, data, sizeof(data),
				   &(struct d1_guard){ .never_written = true },
				   d1_version_none(), NULL)),
	      "there is a history to rebuild");
	check(d1_version_live(
		      commit_chunk(live, admission, 2, 2, data, sizeof(data),
				   &(struct d1_guard){ .never_written = true },
				   d1_version_none(), NULL)),
	      "with more than one record in it");
	/*
	 * Two committed chunks with a gap between them, so the prefix the
	 * rebuild does reduce has a visible version, an EOF, a guard and
	 * an extent list to be wrongly served from.
	 */
	check(d1_version_live(
		      commit_chunk(live, admission, 4, 3, data, sizeof(data),
				   &(struct d1_guard){ .never_written = true },
				   d1_version_none(), NULL)),
	      "and a third, past a hole");

	log = journal_of(live, &len);
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

	/*
	 * And "nothing" includes the observers.  The rebuild got as far as
	 * the committed chunks before the corrupt record stopped it, so
	 * every one of these would otherwise hand back a value out of a
	 * history the model refused.
	 */
	check(!d1_store_visible(bad, &object, 0, &seen),
	      "it makes nothing visible");
	check(!d1_store_guard(bad, &object, 0, &guard),
	      "it has no guard to give");
	check(d1_store_eof(bad, &object) == 0, "no EOF");
	check(d1_store_holes(bad, &object, holes, 4) == 0, "no extents");
	check(!d1_store_materialized(bad, &object, 0, &seen),
	      "no materialized pointer");
	check(d1_store_incarnation(bad) == 0, "no incarnation");
	d1_store_verifier(bad, verifier);
	for (i = 0; i < D1_VERIFIER_BYTES; i++)
		zero = zero && verifier[i] == 0;
	check(zero, "a zero verifier");
	/*
	 * The gate on d1_store_journal is not what this observes: a replay
	 * target owns no journal of its own, so the length is zero with
	 * the gate and without it.  The line records the state, and the
	 * matrix says the gate has no killing test.
	 */
	(void)journal_of(bad, &badlen);
	check(badlen == 0, "and no journal bytes, gate or no gate");

	check(d1_store_close(bad) == D1_OK, "but it still closes");
	check(d1_store_destroy(bad) == D1_OK, "and is destroyed");

	d1_store_free(live);
}

/*
 * A logical close is a fence, and a fence every public entry point is
 * on the same side of.
 *
 * The allocation stays alive after a close so that a caller arriving at
 * the door finds a store rather than freed memory -- but what it finds
 * is a store that answers nothing.  The header promises that everything
 * after a successful close fails closed, and an observer that kept
 * answering would be the one thing that did not: a version, an EOF or a
 * journal read back between the close and the owner's destroy is read
 * from a store its caller has already given up.
 */
static void test_a_closed_store_answers_nothing(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s, *other;
	struct d1_selection_spec sel;
	struct d1_view *view = NULL;
	struct d1_envelope env;
	struct d1_result res;
	struct d1_guard guard;
	struct d1_interval holes[4];
	static uint8_t data[16];
	uint8_t verifier[D1_VERIFIER_BYTES];
	const uint8_t *log;
	size_t len, after = 1;
	unsigned int i;
	bool zero = true;
	d1_admission_id admission;
	d1_version_id seen;

	memset(data, 0x2e, sizeof(data));
	fill_uuid(&store_uuid, 0x2e);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	d1_store_journal_enable(s);
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_READ | D1_RIGHT_WRITE |
					     D1_RIGHT_SINGLE_WRITER);
	check(d1_version_live(
		      commit_chunk(s, admission, 0, 1, data, sizeof(data),
				   &(struct d1_guard){ .never_written = true },
				   d1_version_none(), NULL)),
	      "a store with something to answer about");
	/* An index fault leaves an overlay, so those two have answers too. */
	d1_fixture_fail_next_index(s);
	check(d1_version_live(
		      commit_chunk(s, admission, 1, 2, data, sizeof(data),
				   &(struct d1_guard){ .never_written = true },
				   d1_version_none(), NULL)),
	      "and a materialized pointer left behind by a fault");
	check(d1_store_visible(s, &object, 0, &seen) &&
		      d1_store_guard(s, &object, 0, &guard) &&
		      d1_store_eof(s, &object) != 0 &&
		      d1_store_incarnation(s) != 0 &&
		      d1_store_overlay_active(s),
	      "every observer has an answer while it is open");
	log = journal_of(s, &len);
	check(log && len, "and a journal");

	check(d1_store_close(s) == D1_OK, "it closes");

	/* Every public observer, one by one. */
	check(!d1_store_visible(s, &object, 0, &seen),
	      "a closed store makes nothing visible");
	check(!d1_store_guard(s, &object, 0, &guard),
	      "it has no guard to give");
	check(d1_store_eof(s, &object) == 0, "no EOF");
	check(d1_store_holes(s, &object, holes, 4) == 0, "no extents");
	check(!d1_store_materialized(s, &object, 1, &seen),
	      "no materialized pointer");
	check(!d1_store_overlay_active(s), "no overlay");
	check(d1_store_incarnation(s) == 0, "no incarnation");
	d1_store_verifier(s, verifier);
	for (i = 0; i < D1_VERIFIER_BYTES; i++)
		zero = zero && verifier[i] == 0;
	check(zero, "a zero verifier");
	(void)journal_of(s, &after);
	check(after == 0, "and no journal bytes");

	/* And every entry point that would change it, or open a view. */
	env_init(&env, s, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 2, 11, 3, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	check(d1_store_apply(s, &env, &res) == D1_INVALID,
	      "it applies nothing");
	ordinary_sel(&sel);
	check(d1_view_open(s, &object, admission, &sel, 0, CHUNK_BYTES,
			   &view) == D1_INVALID,
	      "it opens no view");
	check(d1_store_journal_enable(s) == D1_INVALID, "it starts no journal");
	check(!d1_admission_live(
		      d1_fixture_admit(s, &object, 12, D1_RIGHT_WRITE)),
	      "and the fixture installs no authority in it");

	check(d1_store_close(s) == D1_OK, "closing it again is not an error");
	check(d1_store_destroy(s) == D1_OK, "and it destroys");

	/* A closed store is not a recovery target either. */
	other = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (other) {
		check(d1_store_close(other) == D1_OK, "a fresh store closes");
		check(d1_store_replay(other, log, len) == D1_INVALID,
		      "and is no target for a rebuild");
		check(d1_store_reopen(other, log, len) == D1_INVALID,
		      "nor for a reopen");
		check(d1_store_destroy(other) == D1_OK, "it destroys too");
	}
}

/* Fixture custody names a version that exists. */
static void test_custody_needs_a_real_version(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	static uint8_t data[16];
	d1_admission_id admission;
	d1_version_id v1;

	memset(data, 0x78, sizeof(data));
	fill_uuid(&store_uuid, 0x43);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);

	check(!d1_custody_live(
		      d1_fixture_custody(s, d1_fixture_version_handle(s, 1))),
	      "custody over a version that does not exist yet is refused");
	v1 = commit_chunk(s, admission, 0, 1, data, sizeof(data),
			  &(struct d1_guard){ .never_written = true },
			  d1_version_none(), NULL);
	check(d1_version_live(v1), "a version is committed");
	check(d1_custody_live(d1_fixture_custody(s, v1)),
	      "and custody over it is issued");
	check(!d1_custody_live(d1_fixture_custody(
		      s, d1_fixture_version_handle(s, v1.raw + 1000u))),
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
		d1_admission_id admission;
		d1_version_id v1;
		d1_version_id v2;
		d1_txn_id txn2;
		d1_custody_id custody;

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
				  d1_version_none(), NULL);
		ordinary_sel(&sel);
		check(d1_view_open(s, &object, admission, &sel, 0, CHUNK_BYTES,
				   &view) == D1_OK,
		      "a view pins the version");
		d1_store_guard(s, &object, 0, &guard);
		v2 = commit_chunk(s, admission, 0, 2, data, sizeof(data),
				  &guard, v1, &txn2);
		check(d1_version_live(v1) && d1_version_live(v2),
		      "a new version displaces it");

		if (release_first[pass]) {
			released[pass] = d1_fixture_release_predecessor(s, v1);
			d1_view_close(view);
		} else {
			d1_view_close(view);
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
	d1_admission_id admission;
	d1_admission_id stranger;
	d1_version_id v1;
	d1_version_id v2;
	d1_txn_id txn;
	d1_version_id seen;

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
			  &(struct d1_guard){ .never_written = true },
			  d1_version_none(), NULL);
	d1_store_guard(s, &object, 0, &guard);
	v2 = finalize_chunk(s, admission, 0, 2, pending, sizeof(pending),
			    &guard, v1, &txn);
	check(d1_version_live(v1) && d1_version_live(v2),
	      "one version commits and one finalizes");

	owner_sel(&sel, txn, 11, 2, 0);
	check(d1_view_open(s, &object, admission, &sel, 0, CHUNK_BYTES, &own) ==
		      D1_OK,
	      "an owner view opens on its own transaction");
	check(d1_view_version(own, 0, &seen) && d1_version_eq(seen, v2),
	      "and selects it");
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
	check(d1_view_version(ordinary, 0, &seen) && d1_version_eq(seen, v1),
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

	d1_view_close(own);
	d1_view_close(ordinary);
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
	d1_admission_id admission;
	d1_version_id v1;
	d1_version_id v2;
	d1_txn_id txn;

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
			  &(struct d1_guard){ .never_written = true },
			  d1_version_none(), NULL);
	d1_store_guard(s, &object, 0, &guard);
	v2 = finalize_chunk(s, admission, 0, 2, small, sizeof(small), &guard,
			    v1, &txn);
	check(d1_version_live(v1) && d1_version_live(v2),
	      "a shorter private replacement finalizes");

	owner_sel(&sel, txn, 11, 2, 0);
	check(d1_view_open(s, &object, admission, &sel, 0, CHUNK_BYTES, &own) ==
		      D1_OK,
	      "the owner view opens");
	check(d1_view_eof(own) == sizeof(small),
	      "and its EOF shrinks with what it selected");
	check(d1_store_eof(s, &object) == sizeof(big),
	      "while the ordinary EOF stays where it was");
	d1_view_close(own);
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
	d1_admission_id admission;
	d1_admission_id writeonly;

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
	check(d1_version_live(
		      commit_chunk(s, admission, 1, 1, data, sizeof(data),
				   &(struct d1_guard){ .never_written = true },
				   d1_version_none(), NULL)),
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
		d1_view_close(narrow);
	}

	d1_view_close(view);

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
/*
 * Do two stores hold the same state for one object?
 *
 * Handles are compared by value rather than as handles.  Two stores
 * that ran the same history hold the same version numbers, and a
 * handle's provenance is which store issued it -- which is exactly the
 * thing these two differ in, and never the thing this question asks
 * about.
 */
static bool object_states_agree(struct d1_store *a, struct d1_store *b,
				const struct d1_objkey *key)
{
	struct d1_interval ha[D1_MAX_INTERVALS], hb[D1_MAX_INTERVALS];
	uint32_t na, nb, i;

	if (d1_store_eof(a, key) != d1_store_eof(b, key))
		return false;
	na = d1_store_holes(a, key, ha, D1_MAX_INTERVALS);
	nb = d1_store_holes(b, key, hb, D1_MAX_INTERVALS);
	if (na != nb)
		return false;
	for (i = 0; i < na; i++)
		if (ha[i].start != hb[i].start || ha[i].end != hb[i].end)
			return false;
	for (i = 0; i < D1_MAX_CHUNKS; i++) {
		d1_version_id va = { 0 }, vb = { 0 };
		bool pa, pb, qa, qb;
		struct d1_guard ga, gb;

		pa = d1_store_visible(a, key, i, &va);
		pb = d1_store_visible(b, key, i, &vb);
		if (pa != pb || (pa && va.raw != vb.raw))
			return false;
		qa = d1_store_guard(a, key, i, &ga);
		qb = d1_store_guard(b, key, i, &gb);
		if (qa != qb)
			return false;
		/*
		 * Only compare the guard when there was one to fetch.  A
		 * lookup that answers "no such object" leaves the caller's
		 * struct untouched, so comparing it reads whatever the stack
		 * happened to hold -- which agreed under one set of build
		 * flags and disagreed under another.
		 */
		if (!qa)
			continue;
		if (ga.never_written != gb.never_written ||
		    ga.generation != gb.generation || ga.writer != gb.writer)
			return false;
	}
	return true;
}

/* The file-scope object, which is the one most tests write. */
static bool states_agree(struct d1_store *a, struct d1_store *b)
{
	return object_states_agree(a, b, &object);
}

/* A short history, written the same way twice. */
static d1_version_id drive_history(struct d1_store *s,
				   d1_admission_id admission)
{
	static uint8_t small[100];
	static uint8_t tiny[50];
	static uint8_t big[512];
	struct d1_guard guard;
	d1_version_id v3;
	d1_txn_id txn4;
	d1_custody_id custody;

	memset(small, 0x21, sizeof(small));
	memset(tiny, 0x22, sizeof(tiny));
	memset(big, 0x23, sizeof(big));

	v3 = commit_chunk(s, admission, 3, 1, small, sizeof(small),
			  &(struct d1_guard){ .never_written = true },
			  d1_version_none(), NULL);
	if (!d1_version_live(v3))
		return d1_version_none();
	if (!d1_version_live(
		    commit_chunk(s, admission, 5, 2, tiny, sizeof(tiny),
				 &(struct d1_guard){ .never_written = true },
				 d1_version_none(), NULL)))
		return d1_version_none();
	d1_store_guard(s, &object, 3, &guard);
	if (!d1_version_live(commit_chunk(s, admission, 3, 3, big, sizeof(big),
					  &guard, v3, &txn4)))
		return d1_version_none();
	/* Custody is issued over whatever is visible on chunk 3 now. */
	{
		d1_version_id visible = { 0 };

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
			return d1_version_none();
	}
	return v3;
}

/*
 * A journal snapshot is a value, not a window on the store.
 *
 * The observer this replaced returned the store's own buffer, and no
 * lock could have made that safe: the pointer stayed interesting after
 * the lock was dropped, and the next append may reallocate the buffer
 * underneath it.  What the caller gets now is bytes of its own, so the
 * store may grow, close and be destroyed without touching them.
 */
static void test_a_journal_snapshot_is_a_value(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s, *target;
	static uint8_t data[64];
	static uint8_t kept[1u << 18];
	uint8_t *snap = NULL, *again = NULL;
	size_t len = 0, grown = 0, after = 1;
	unsigned int i;
	d1_admission_id admission;

	memset(data, 0x21, sizeof(data));
	fill_uuid(&store_uuid, 0x21);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;

	/* A live store that has logged nothing has an empty snapshot. */
	check(d1_store_journal_snapshot(s, &snap, &len) == D1_OK &&
		      snap == NULL && len == 0,
	      "a live store with no journal snapshots to nothing");
	free(snap);

	d1_store_journal_enable(s);
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	check(d1_version_live(
		      commit_chunk(s, admission, 0, 1, data, sizeof(data),
				   &(struct d1_guard){ .never_written = true },
				   d1_version_none(), NULL)),
	      "a history to snapshot");

	check(d1_store_journal_snapshot(s, &snap, &len) == D1_OK && snap &&
		      len && len <= sizeof(kept),
	      "the snapshot is taken");
	if (!snap || len > sizeof(kept)) {
		free(snap);
		d1_store_free(s);
		return;
	}
	memcpy(kept, snap, len);

	/*
	 * Enough further writes to take the journal past its first buffer
	 * and make it move.  The snapshot must not move with it.
	 */
	for (i = 1; i < 20u; i++)
		(void)commit_chunk(s, admission, i, i + 1u, data, sizeof(data),
				   &(struct d1_guard){ .never_written = true },
				   d1_version_none(), NULL);
	check(d1_store_journal_snapshot(s, &again, &grown) == D1_OK &&
		      grown > len,
	      "the store's own journal grows past it");
	check(memcmp(snap, kept, len) == 0,
	      "and the snapshot taken before it is byte for byte what it was");
	free(again);

	/* A closed store answers no snapshot, and the old one is untouched. */
	check(d1_store_close(s) == D1_OK, "the store closes");
	check(d1_store_journal_snapshot(s, &again, &after) == D1_INVALID &&
		      again == NULL && after == 0,
	      "a closed store snapshots nothing");
	check(memcmp(snap, kept, len) == 0,
	      "and the close did not reach into the snapshot either");
	check(d1_store_destroy(s) == D1_OK, "the store is destroyed");
	check(memcmp(snap, kept, len) == 0,
	      "and the snapshot outlives the store it came from");

	/* And it is still a log: it rebuilds a store of its own. */
	target = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (target) {
		d1_version_id seen;

		check(d1_store_replay(target, snap, len) == D1_OK,
		      "the snapshot replays after its store is gone");
		check(d1_store_visible(target, &object, 0, &seen),
		      "into the state it was taken at");
		d1_store_free(target);
	}
	free(snap);
}

/* A snapshot that finds no memory says so, and changes nothing. */
static void test_a_journal_snapshot_can_find_no_memory(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	static uint8_t data[16];
	uint8_t *snap = NULL;
	size_t len = 1, again = 0;
	d1_admission_id admission;

	memset(data, 0x22, sizeof(data));
	fill_uuid(&store_uuid, 0x22);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	d1_store_journal_enable(s);
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	check(d1_version_live(
		      commit_chunk(s, admission, 0, 1, data, sizeof(data),
				   &(struct d1_guard){ .never_written = true },
				   d1_version_none(), NULL)),
	      "a history to fail to snapshot");

	d1_fixture_fail_next_snapshot(s);
	check(d1_store_journal_snapshot(s, &snap, &len) == D1_NOSPC &&
		      snap == NULL && len == 0,
	      "a snapshot with no memory for it is refused");
	check(d1_store_journal_snapshot(s, &snap, &again) == D1_OK && snap &&
		      again != 0,
	      "and nothing of the store moved, so the next one is ordinary");
	free(snap);
	snap = NULL;
	d1_store_free(s);

	/*
	 * The arm says the next snapshot, so an empty one spends it too.
	 * A store that has logged nothing is where that is visible: the
	 * armed call is refused, and the one after it answers empty.
	 */
	fill_uuid(&store_uuid, 0x2c);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	d1_fixture_fail_next_snapshot(s);
	len = 1;
	check(d1_store_journal_snapshot(s, &snap, &len) == D1_NOSPC &&
		      snap == NULL && len == 0,
	      "an empty snapshot is refused by the arm as any other is");
	len = 1;
	check(d1_store_journal_snapshot(s, &snap, &len) == D1_OK &&
		      snap == NULL && len == 0,
	      "and the arm is spent, so the next one is the empty answer");
	free(snap);
	d1_store_free(s);
}

/*
 * Snapshots taken beside a store that is being appended to and closed.
 *
 * This is the case the old observer could not survive, and the one
 * ThreadSanitizer is pointed at: the reader runs for the whole of a
 * run of writes and a close, and every snapshot it gets back must be a
 * whole number of whole records -- never a buffer the store was in the
 * middle of moving, and never a length from the other side of a flush.
 */
struct snapper {
	struct d1_store *s;
	unsigned int taken;
	unsigned int refused;
	unsigned int torn;
	/*
	 * Which primitive failed, if one did.  The reader cannot call
	 * check() from another thread, so it records the step and the main
	 * thread reports it after the join -- and a failed lock ends the
	 * loop with a reason rather than silently.
	 */
	unsigned int err;
	bool stop;
	pthread_mutex_t m;
};

static bool snapshot_is_whole(const uint8_t *buf, size_t len)
{
	size_t off = 0;

	while (off < len) {
		uint32_t total;

		if (len - off <
		    D1_JOURNAL_HEADER_BYTES + D1_JOURNAL_TRAILER_BYTES)
			return false;
		total = record_bytes(buf + off);
		if (total < D1_JOURNAL_HEADER_BYTES +
				    D1_JOURNAL_TRAILER_BYTES ||
		    total > len - off)
			return false;
		off += total;
	}
	return true;
}

static void *take_snapshots(void *arg)
{
	struct snapper *sn = arg;

	for (;;) {
		uint8_t *buf = NULL;
		size_t len = 0;
		bool stop;

		if (d1_store_journal_snapshot(sn->s, &buf, &len) == D1_OK) {
			if (!snapshot_is_whole(buf, len))
				sn->torn++;
			sn->taken++;
		} else {
			sn->refused++;
		}
		free(buf);
		if (pthread_mutex_lock(&sn->m) != 0) {
			sn->err = 1;
			break;
		}
		stop = sn->stop;
		if (pthread_mutex_unlock(&sn->m) != 0) {
			sn->err = 2;
			break;
		}
		if (stop)
			break;
	}
	return NULL;
}

static void test_snapshots_run_beside_appends_and_a_close(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct snapper sn;
	pthread_t reader;
	static uint8_t data[64];
	unsigned int i;
	d1_admission_id admission;

	memset(data, 0x23, sizeof(data));
	fill_uuid(&store_uuid, 0x23);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	d1_store_journal_enable(s);
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);

	memset(&sn, 0, sizeof(sn));
	sn.s = s;
	if (pthread_mutex_init(&sn.m, NULL) != 0) {
		check(false, "the snapshot reader's mutex starts");
		d1_store_free(s);
		return;
	}
	if (pthread_create(&reader, NULL, take_snapshots, &sn) != 0) {
		check(false, "the snapshot reader starts");
		check(pthread_mutex_destroy(&sn.m) == 0,
		      "and its mutex is released again");
		d1_store_free(s);
		return;
	}

	/* Enough writes to grow the journal buffer more than once. */
	for (i = 0; i < 40u; i++)
		(void)commit_chunk(s, admission, i % D1_MAX_CHUNKS, i + 1u,
				   data, sizeof(data),
				   i < D1_MAX_CHUNKS ?
					   &(struct d1_guard){ .never_written =
								       true } :
					   NULL,
				   d1_version_none(), NULL);
	check(d1_store_close(s) == D1_OK, "the store closes under the reader");

	check(pthread_mutex_lock(&sn.m) == 0, "the reader is told to stop");
	sn.stop = true;
	check(pthread_mutex_unlock(&sn.m) == 0, "and released");
	check(pthread_join(reader, NULL) == 0, "and joined");

	check(sn.taken + sn.refused > 0, "the reader took snapshots");
	check(sn.err == 0, "with every primitive inside it succeeding");
	check(sn.torn == 0, "and not one of them was a torn journal");
	check(d1_store_destroy(s) == D1_OK, "then the store is destroyed");
	check(pthread_mutex_destroy(&sn.m) == 0, "and the reader's mutex");
}

/*
 * A cohort of zero is no cohort, so it is no owner either.
 *
 * Section 3 makes zero the absent value of a typed ID, and the cohort
 * is one.  The shape check asked only about the writer, so a canonical
 * one-entry write naming cohort zero validated, took a transaction and
 * a version, answered D1_OK, was appended and replayed -- a request
 * built out of an absent handle, recorded as history.  The co_id beside
 * it is an opaque u32 and zero is an ordinary value there; this is
 * about the typed one.
 *
 * All three request shapes that carry an owner ask the same question,
 * so all three are asked here, and the write is followed to the end:
 * nothing of the store moved and nothing reached the log.
 */
static void test_an_owner_names_a_cohort(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_envelope env, decoded;
	struct d1_result res;
	struct d1_guard guard;
	static uint8_t data[16];
	static uint8_t bytes[4096];
	static uint8_t other[4096];
	size_t before = 0, after = 1, env_len, other_len, i;
	size_t first = 0, last = 0;
	d1_admission_id admission;
	d1_txn_id txn;
	d1_version_id seen;

	memset(data, 0x24, sizeof(data));
	fill_uuid(&store_uuid, 0x24);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	d1_store_journal_enable(s);
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	(void)journal_of(s, &before);

	/* A write whose only fault is the absent cohort. */
	env_init(&env, s, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 0, 11, 2, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	check(d1_envelope_validate(&env), "the request is otherwise canonical");
	env.body.write.entries[0].owner.cohort.raw = 0;
	check(!d1_envelope_validate(&env),
	      "a write owner with no cohort is not a canonical request");
	check(d1_store_apply(s, &env, &res) == D1_INVALID,
	      "and the call is refused before anything reads the body");
	check(d1_envelope_encode(&env, bytes, sizeof(bytes)) == 0,
	      "and the encoder will not write one");
	check(!d1_store_visible(s, &object, 0, &seen) &&
		      !d1_store_guard(s, &object, 0, &guard) &&
		      d1_store_eof(s, &object) == 0,
	      "nothing of the store moved");
	(void)journal_of(s, &after);
	check(after == before, "and nothing reached the log");

	/* The decoder refuses the bytes too, not only the encoder. */
	env.body.write.entries[0].owner.cohort.raw = 0x5a5au;
	env_len = d1_envelope_encode(&env, bytes, sizeof(bytes));
	env.body.write.entries[0].owner.cohort.raw = 0xa5a5u;
	other_len = d1_envelope_encode(&env, other, sizeof(other));
	check(env_len != 0 && env_len == other_len,
	      "two canonical forms of it encode to one length");
	if (!env_len || env_len != other_len) {
		d1_store_free(s);
		return;
	}
	for (i = 0; i < env_len; i++) {
		if (bytes[i] == other[i])
			continue;
		if (!last)
			first = i;
		last = i;
	}
	check(last != 0 && last + 1u - first <= 8u,
	      "and differ in nothing but that ID");
	check(d1_envelope_decode(bytes, env_len, &decoded),
	      "the canonical encoding decodes");
	for (i = first; i <= last; i++)
		bytes[i] = 0;
	check(!d1_envelope_decode(bytes, env_len, &decoded),
	      "and the same bytes with the cohort zeroed do not");

	/* The same question of the other two shapes that carry an owner. */
	env.body.write.entries[0].owner.cohort.raw = 1;
	check(d1_store_apply(s, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_OK,
	      "the write with a cohort succeeds");
	txn = res.entries[0].txn;

	env_init(&env, s, admission, D1_OP_FINALIZE_BATCH);
	env.body.lifecycle.range_begin = 0;
	env.body.lifecycle.range_end = 1;
	env.body.lifecycle.count = 1;
	env.body.lifecycle.entries[0].index = 0;
	env.body.lifecycle.entries[0].owner.cohort.raw = 0;
	env.body.lifecycle.entries[0].owner.writer = 11;
	env.body.lifecycle.entries[0].owner.co_id = 2;
	env.body.lifecycle.entries[0].txn = txn;
	d1_store_verifier(s, env.body.lifecycle.prior_verifier);
	check(!d1_envelope_validate(&env) &&
		      d1_store_apply(s, &env, &res) == D1_INVALID &&
		      d1_envelope_encode(&env, bytes, sizeof(bytes)) == 0,
	      "a lifecycle owner with no cohort is refused the same way");

	env_init(&env, s, admission, D1_OP_ROLLBACK_BATCH);
	env.body.rollback.range_begin = 0;
	env.body.rollback.range_end = 1;
	env.body.rollback.count = 1;
	env.body.rollback.entries[0].index = 0;
	env.body.rollback.entries[0].owner.cohort.raw = 0;
	env.body.rollback.entries[0].owner.writer = 11;
	env.body.rollback.entries[0].owner.co_id = 2;
	env.body.rollback.entries[0].txn = txn;
	check(!d1_envelope_validate(&env) &&
		      d1_store_apply(s, &env, &res) == D1_INVALID &&
		      d1_envelope_encode(&env, bytes, sizeof(bytes)) == 0,
	      "and so is a rollback owner with no cohort");

	d1_store_free(s);
}

/*
 * The chunk table is capacity, not geometry.
 *
 * A store opened for a million bytes of 4096-byte chunks has room in
 * its declared geometry for chunk 64, and this model keeps a table of
 * sixty-four.  It used to answer that write D1_INVALID with a recorded
 * receipt, which says the request was malformed -- it was not, it was
 * the geometry the store was opened with -- and quietly redefined the
 * bound the caller gave.  Running out of table is running out of room,
 * so it is NOSPC and UNRECORDED: nothing recorded, nothing logged,
 * nothing spent, and the caller may come back when there is room.
 *
 * A read is a different question again.  A range that reaches past the
 * table is not malformed for that reason; nothing above the table can
 * hold a version, so those bytes are simply past EOF.
 */
static void test_the_chunk_table_is_capacity(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s, *target;
	struct d1_envelope env;
	struct d1_result res;
	struct d1_selection_spec sel;
	struct d1_view *view = NULL;
	static uint8_t data[16];
	static uint8_t got[64];
	const uint8_t *log;
	size_t before = 0, after = 1, len;
	uint32_t got_len = 1;
	d1_admission_id admission;
	d1_version_id seen;

	memset(data, 0x25, sizeof(data));
	fill_uuid(&store_uuid, 0x25);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	d1_store_journal_enable(s);
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_READ | D1_RIGHT_WRITE |
					     D1_RIGHT_SINGLE_WRITER);

	/* The last index the table holds is an ordinary write. */
	check(d1_version_live(commit_chunk(
		      s, admission, D1_MAX_CHUNKS - 1u, 1, data, sizeof(data),
		      &(struct d1_guard){ .never_written = true },
		      d1_version_none(), NULL)),
	      "the last chunk the table holds is written and committed");
	check(d1_store_visible(s, &object, D1_MAX_CHUNKS - 1u, &seen),
	      "and is visible");
	(void)journal_of(s, &before);

	/* The first index past it is within the declared geometry. */
	check((uint64_t)D1_MAX_CHUNKS * CHUNK_BYTES + sizeof(data) <=
		      MAX_FILE_BYTES,
	      "the index past the table is inside the declared geometry");
	env_init(&env, s, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], D1_MAX_CHUNKS, 11, 9, data,
		    sizeof(data), true,
		    &(struct d1_guard){ .never_written = true });
	check(d1_store_apply(s, &env, &res) == D1_OK,
	      "the write past the table applies");
	check(res.entries[0].status == D1_NOSPC,
	      "and answers out of room, not malformed");
	check(res.entries[0].disposition == D1_UNRECORDED,
	      "with nothing recorded for it");
	check(!d1_txn_live(res.entries[0].txn) &&
		      !d1_version_live(res.entries[0].version),
	      "and no durable ID spent");
	(void)journal_of(s, &after);
	check(after == before, "and nothing in the log");

	/* An exact retry is a fresh question, because nothing was kept. */
	check(d1_store_apply(s, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_NOSPC &&
		      res.entries[0].disposition == D1_UNRECORDED,
	      "the exact retry is answered afresh, not from a receipt");

	/* A read window that crosses the boundary is an ordinary read. */
	ordinary_sel(&sel);
	check(d1_view_open(s, &object, admission, &sel,
			   (uint64_t)(D1_MAX_CHUNKS - 1u) * CHUNK_BYTES,
			   (uint64_t)(D1_MAX_CHUNKS + 1u) * CHUNK_BYTES,
			   &view) == D1_OK,
	      "a window crossing the end of the table opens");
	if (view) {
		check(d1_view_eof(view) ==
			      (uint64_t)(D1_MAX_CHUNKS - 1u) * CHUNK_BYTES +
				      sizeof(data),
		      "with the EOF the table's last chunk gives it");
		check(d1_view_read(view, (uint64_t)D1_MAX_CHUNKS * CHUNK_BYTES,
				   got, sizeof(got), &got_len) == D1_OK &&
			      got_len == 0,
		      "and past the table it reads no bytes, as past EOF");
		d1_view_close(view);
	}

	/* What was written still rebuilds, and the refusal left no trace. */
	log = journal_of(s, &len);
	target = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (target && log) {
		check(d1_store_replay(target, log, len) == D1_OK,
		      "the log it did write replays");
		check(states_agree(s, target), "into the same store");
		d1_store_free(target);
	}
	d1_store_free(s);

	/*
	 * And the two bounds are asked in the right order.  On geometry of
	 * exactly the table's worth of chunks, index 64 is not a request
	 * the model has no room for -- it is outside the geometry the
	 * store was opened with, which is a malformed request and a
	 * recorded refusal.  Asking the table first would answer it out of
	 * room, which would be the model's own limit standing in for the
	 * caller's declared one.
	 */
	fill_uuid(&store_uuid, 0x2d);
	s = d1_store_open(&store_uuid, CHUNK_BYTES,
			  (uint64_t)D1_MAX_CHUNKS * CHUNK_BYTES);
	if (!s)
		return;
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	env_init(&env, s, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], D1_MAX_CHUNKS, 11, 1, data,
		    sizeof(data), true,
		    &(struct d1_guard){ .never_written = true });
	check(d1_store_apply(s, &env, &res) == D1_OK, "the write applies");
	check(res.entries[0].status == D1_INVALID,
	      "a write outside the declared geometry is malformed");
	check(res.entries[0].disposition == D1_COMPLETED,
	      "and is a recorded refusal, not a shortage");
	d1_store_free(s);
}

/*
 * A malformed request is malformed whether or not there is room.
 *
 * The chunk bounds depend on nothing but the store's geometry and the
 * entry, but they used to be asked after the object was looked up or
 * created -- so with an object slot free an out-of-geometry index was
 * INVALID and recorded, and with every slot taken the identical request
 * became NOSPC and retryable.  The shape of a request cannot depend on
 * how full an internal table happens to be.
 *
 * Both answers are checked here against a full table: out of geometry
 * is still the recorded refusal, and in geometry is still the shortage.
 */
static void test_geometry_is_asked_before_the_object_table(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s, *target;
	struct d1_objkey keys[D1_MAX_OBJECTS + 1u];
	struct d1_envelope env;
	struct d1_result res;
	static uint8_t data[16];
	const uint8_t *log;
	size_t len, before = 0, after = 1;
	unsigned int i;
	d1_admission_id admissions[D1_MAX_OBJECTS + 1u];
	d1_txn_id txn;
	d1_version_id version;

	memset(data, 0x2e, sizeof(data));
	fill_uuid(&store_uuid, 0x2f);
	/* Geometry of exactly the table's worth of chunks. */
	s = d1_store_open(&store_uuid, CHUNK_BYTES,
			  (uint64_t)D1_MAX_CHUNKS * CHUNK_BYTES);
	if (!s)
		return;
	d1_store_journal_enable(s);
	for (i = 0; i < D1_MAX_OBJECTS + 1u; i++) {
		keys[i].export_uuid = object.export_uuid;
		fill_uuid(&keys[i].object_uuid, (uint8_t)(0xe0 + i));
		admissions[i] = d1_fixture_admit(
			s, &keys[i], 11,
			D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	}

	/* Every object slot taken. */
	for (i = 0; i < D1_MAX_OBJECTS; i++) {
		env_init(&env, s, admissions[i], D1_OP_WRITE_BATCH);
		env.object = keys[i];
		env.body.write.count = 1;
		env.body.write.stability = D1_FILE_SYNC;
		write_entry(&env.body.write.entries[0], 0, 11, i + 1u, data,
			    sizeof(data), true,
			    &(struct d1_guard){ .never_written = true });
		check(d1_store_apply(s, &env, &res) == D1_OK &&
			      res.entries[0].status == D1_OK,
		      "every object slot is taken");
	}
	(void)journal_of(s, &before);

	/*
	 * The fifth object, at an index the declared geometry does not
	 * reach.  There is no room for the object either, and the answer
	 * must still be about the request.
	 */
	env_init(&env, s, admissions[D1_MAX_OBJECTS], D1_OP_WRITE_BATCH);
	env.object = keys[D1_MAX_OBJECTS];
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], D1_MAX_CHUNKS, 11, 9, data,
		    sizeof(data), true,
		    &(struct d1_guard){ .never_written = true });
	check(d1_store_apply(s, &env, &res) == D1_OK, "the write applies");
	check(res.entries[0].status == D1_INVALID,
	      "an out-of-geometry index is malformed with the table full");
	check(res.entries[0].disposition == D1_COMPLETED,
	      "and is a recorded refusal, as it is with the table free");
	check(!res.entries[0].txn_present && !res.entries[0].version_present,
	      "and spends no ID");
	txn = res.entries[0].txn;
	version = res.entries[0].version;
	check(!d1_txn_live(txn) && !d1_version_live(version), "nor names one");

	/* Being recorded, the exact retry answers from the receipt. */
	check(d1_store_apply(s, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_INVALID &&
		      res.entries[0].disposition == D1_COMPLETED,
	      "and the exact retry answers from the record it left");

	/*
	 * And the same fifth object in geometry is the other answer: the
	 * request is well formed, and the model has no room for it.
	 */
	env_init(&env, s, admissions[D1_MAX_OBJECTS], D1_OP_WRITE_BATCH);
	env.object = keys[D1_MAX_OBJECTS];
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 0, 11, 9, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	check(d1_store_apply(s, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_NOSPC &&
		      res.entries[0].disposition == D1_UNRECORDED,
	      "an in-geometry write with no object slot is a shortage");
	check(d1_store_apply(s, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_NOSPC &&
		      res.entries[0].disposition == D1_UNRECORDED,
	      "and its retry is a fresh question, not a record");

	/* The log carries the refusal and not the shortage, and replays. */
	log = journal_of(s, &after);
	check(after > before, "the recorded refusal reached the log");
	target = d1_store_open(&store_uuid, CHUNK_BYTES,
			       (uint64_t)D1_MAX_CHUNKS * CHUNK_BYTES);
	if (target && log) {
		bool same = true;

		len = after;
		check(d1_store_replay(target, log, len) == D1_OK,
		      "and the log replays");
		/*
		 * Over the objects this test actually wrote.  Asking about
		 * the file-scope object, which it never touches, would be
		 * answered the same way by an empty store.
		 */
		for (i = 0; i < D1_MAX_OBJECTS + 1u; i++)
			same = same && object_states_agree(s, target, &keys[i]);
		check(same, "into the same store");
		d1_store_free(target);
	}
	d1_store_free(s);
}

/*
 * What a request is wrong about does not depend on how full the store
 * is.
 *
 * Five of a write's answers depend on the request and the grant it came
 * with and on nothing else: a multi-writer grant may not ask for
 * activation, it may not omit the guard predicate, the writer must be
 * the granted one, the checksum must verify, and an owner already bound
 * elsewhere is a conflict.  All five used to be asked after the object
 * and chunk tables had been consulted, so with room they were semantic
 * refusals with receipts and with the tables full the identical request
 * became NOSPC and retryable -- the same request, two different kinds
 * of wrong, decided by capacity.
 *
 * Both tables are full here: every object slot is taken, and the index
 * asked for is past the chunk table on a geometry that reaches well
 * beyond it.
 */
static void test_a_requests_shape_does_not_depend_on_room(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_objkey keys[D1_MAX_OBJECTS + 1u];
	struct d1_envelope env;
	struct d1_result res, again;
	static uint8_t data[16];
	unsigned int i;
	d1_admission_id admissions[D1_MAX_OBJECTS + 1u];
	d1_admission_id multi;

	memset(data, 0x35, sizeof(data));
	fill_uuid(&store_uuid, 0x35);
	/* A geometry far larger than the fixed chunk table. */
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	check((uint64_t)D1_MAX_CHUNKS * CHUNK_BYTES < MAX_FILE_BYTES,
	      "the geometry reaches past the chunk table");
	for (i = 0; i < D1_MAX_OBJECTS + 1u; i++) {
		keys[i].export_uuid = object.export_uuid;
		fill_uuid(&keys[i].object_uuid, (uint8_t)(0x50 + i));
		admissions[i] = d1_fixture_admit(
			s, &keys[i], 11,
			D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	}
	multi = d1_fixture_admit(s, &keys[D1_MAX_OBJECTS], 12, D1_RIGHT_WRITE);

	/* Every object slot taken. */
	for (i = 0; i < D1_MAX_OBJECTS; i++) {
		env_init(&env, s, admissions[i], D1_OP_WRITE_BATCH);
		env.object = keys[i];
		env.body.write.count = 1;
		env.body.write.stability = D1_FILE_SYNC;
		write_entry(&env.body.write.entries[0], 0, 11, i + 1u, data,
			    sizeof(data), true,
			    &(struct d1_guard){ .never_written = true });
		check(d1_store_apply(s, &env, &res) == D1_OK &&
			      res.entries[0].status == D1_OK,
		      "every object slot is taken");
	}

	/* A multi-writer grant asking to activate, past the table. */
	env_init(&env, s, multi, D1_OP_WRITE_BATCH);
	env.object = keys[D1_MAX_OBJECTS];
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	env.body.write.activate = true;
	write_entry(&env.body.write.entries[0], D1_MAX_CHUNKS, 12, 41, data,
		    sizeof(data), true,
		    &(struct d1_guard){ .never_written = true });
	check(d1_store_apply(s, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_INVALID &&
		      res.entries[0].disposition == D1_COMPLETED,
	      "a multi-writer request to activate is malformed, table or not");
	check(!res.entries[0].txn_present && !res.entries[0].version_present,
	      "and spends no ID");
	check(d1_store_apply(s, &env, &again) == D1_OK &&
		      again.entries[0].status == D1_INVALID &&
		      again.entries[0].disposition == D1_COMPLETED,
	      "and its exact retry answers from the record it left");

	/* A writer that is not the granted one, past the table. */
	env_init(&env, s, admissions[D1_MAX_OBJECTS], D1_OP_WRITE_BATCH);
	env.object = keys[D1_MAX_OBJECTS];
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], D1_MAX_CHUNKS, 99, 42, data,
		    sizeof(data), true,
		    &(struct d1_guard){ .never_written = true });
	check(d1_store_apply(s, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_STALE_AUTH &&
		      res.entries[0].disposition == D1_COMPLETED,
	      "a writer that is not the granted one is refused, table or not");
	check(d1_store_apply(s, &env, &again) == D1_OK &&
		      again.entries[0].status == D1_STALE_AUTH &&
		      again.entries[0].disposition == D1_COMPLETED,
	      "and it too answers from its record on retry");

	/* A checksum that does not verify, past the table. */
	env_init(&env, s, admissions[D1_MAX_OBJECTS], D1_OP_WRITE_BATCH);
	env.object = keys[D1_MAX_OBJECTS];
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], D1_MAX_CHUNKS, 11, 43, data,
		    sizeof(data), true,
		    &(struct d1_guard){ .never_written = true });
	env.body.write.entries[0].checksum.digest[0] ^= 0xffu;
	check(d1_store_apply(s, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_CHECKSUM &&
		      res.entries[0].disposition == D1_COMPLETED,
	      "a checksum that does not verify is refused, table or not");

	/* And a well-formed one past the table is still out of room. */
	env_init(&env, s, admissions[D1_MAX_OBJECTS], D1_OP_WRITE_BATCH);
	env.object = keys[D1_MAX_OBJECTS];
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], D1_MAX_CHUNKS, 11, 44, data,
		    sizeof(data), true,
		    &(struct d1_guard){ .never_written = true });
	check(d1_store_apply(s, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_NOSPC &&
		      res.entries[0].disposition == D1_UNRECORDED,
	      "while a well-formed request past the table is out of room");

	d1_store_free(s);
}

/*
 * A chunk holds a chunk.
 *
 * The declared chunk size bounds one entry's payload, and the check
 * that says so had no test: deleting it let an image of twice the chunk
 * size through, EOF moved to the end of it, and a read of the tail
 * answered zeros rather than the bytes the caller supplied.  A payload
 * one byte over is otherwise canonical and well inside the file, so
 * nothing else refuses it.
 */
static void test_a_payload_fits_its_chunk(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_envelope env;
	struct d1_result res, again;
	static uint8_t data[CHUNK_BYTES + 1u];
	d1_admission_id admission;
	d1_version_id seen;

	memset(data, 0x36, sizeof(data));
	fill_uuid(&store_uuid, 0x36);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	check((uint64_t)sizeof(data) < MAX_FILE_BYTES,
	      "one byte over a chunk is well inside the file");

	env_init(&env, s, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	env.body.write.activate = true;
	write_entry(&env.body.write.entries[0], 0, 11, 1, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	check(d1_store_apply(s, &env, &res) == D1_OK,
	      "a payload one byte over the chunk applies");
	check(res.entries[0].status == D1_INVALID &&
		      res.entries[0].disposition == D1_COMPLETED,
	      "and is malformed");
	check(!res.entries[0].txn_present && !res.entries[0].version_present,
	      "spending no transaction and no version");
	check(!d1_store_visible(s, &object, 0, &seen), "and changing nothing");
	check(d1_store_eof(s, &object) == 0, "not even the EOF");
	check(d1_store_apply(s, &env, &again) == D1_OK &&
		      again.entries[0].status == D1_INVALID &&
		      again.entries[0].disposition == D1_COMPLETED,
	      "and its exact retry answers from the record it left");

	/* A payload of exactly one chunk is the one that fits. */
	env_init(&env, s, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	env.body.write.activate = true;
	write_entry(&env.body.write.entries[0], 0, 11, 2, data, CHUNK_BYTES,
		    true, &(struct d1_guard){ .never_written = true });
	check(d1_store_apply(s, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_OK,
	      "and exactly one chunk is accepted");
	check(d1_store_eof(s, &object) == CHUNK_BYTES,
	      "with the EOF the payload gives it");

	d1_store_free(s);
}

/*
 * An object nobody has written yet can still be read.
 *
 * Section 3 gives a new object initial EOF zero, and this API has no
 * create: the store's geometry and the admission are the whole of an
 * object's existence before its first write.  An ordinary view of one
 * used to be refused as if the object were missing, which made the
 * initial state the one state a reader could not observe.  It opens
 * now, at EOF zero, with nothing to read -- and it takes no object
 * slot, because a read is not journalled and a read that spent model
 * capacity would be a read that changed the store.
 */
static void test_the_empty_object_can_be_read(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_objkey keys[D1_MAX_OBJECTS + 2u];
	struct d1_selection_spec sel;
	struct d1_view *view = NULL;
	struct d1_envelope env;
	struct d1_result res;
	static uint8_t data[16];
	static uint8_t got[32];
	unsigned int i;
	uint32_t got_len = 1;
	d1_admission_id admissions[D1_MAX_OBJECTS + 2u];
	d1_version_id seen;

	memset(data, 0x26, sizeof(data));
	fill_uuid(&store_uuid, 0x26);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	for (i = 0; i < D1_MAX_OBJECTS + 2u; i++) {
		keys[i].export_uuid = object.export_uuid;
		fill_uuid(&keys[i].object_uuid, (uint8_t)(0xc0 + i));
		admissions[i] =
			d1_fixture_admit(s, &keys[i], 11,
					 D1_RIGHT_READ | D1_RIGHT_WRITE |
						 D1_RIGHT_SINGLE_WRITER);
	}

	/* The initial state is a state, and it can be observed. */
	ordinary_sel(&sel);
	check(d1_view_open(s, &keys[0], admissions[0], &sel, 0, CHUNK_BYTES,
			   &view) == D1_OK,
	      "an ordinary view of an object with no writes opens");
	if (view) {
		check(d1_view_eof(view) == 0, "at EOF zero");
		check(!d1_view_version(view, 0, &seen),
		      "with no version in it");
		check(d1_view_read(view, 0, got, sizeof(got), &got_len) ==
				      D1_OK &&
			      got_len == 0,
		      "and reads no bytes, as a read at EOF does");
		d1_view_close(view);
		view = NULL;
	}

	/* OWNER selection is the other question, and still has no answer. */
	memset(&sel, 0, sizeof(sel));
	sel.selection = D1_SELECT_OWNER;
	sel.count = 1;
	sel.txns[0] = d1_txn_none();
	check(d1_view_open(s, &keys[0], admissions[0], &sel, 0, CHUNK_BYTES,
			   &view) == D1_INVALID,
	      "an owner view over transactions that do not exist does not");

	/*
	 * And the read took no slot.  The key read here is never written,
	 * and every one of the store's slots is then taken by a different
	 * key, the last of them included: a read that had quietly created
	 * an object would have spent the slot that write needs, and the
	 * write would answer NOSPC.  Reading the same keys that are
	 * written afterwards proves nothing -- the slots a slot-taking
	 * read filled would be the very ones those writes go on to find.
	 */
	ordinary_sel(&sel);
	check(d1_view_open(s, &keys[D1_MAX_OBJECTS], admissions[D1_MAX_OBJECTS],
			   &sel, 0, CHUNK_BYTES, &view) == D1_OK,
	      "a key that is never written is read once");
	if (view) {
		d1_view_close(view);
		view = NULL;
	}
	for (i = 0; i < D1_MAX_OBJECTS; i++) {
		env_init(&env, s, admissions[i], D1_OP_WRITE_BATCH);
		env.object = keys[i];
		env.body.write.count = 1;
		env.body.write.stability = D1_FILE_SYNC;
		write_entry(&env.body.write.entries[0], 0, 11, i + 1u, data,
			    sizeof(data), true,
			    &(struct d1_guard){ .never_written = true });
		check(d1_store_apply(s, &env, &res) == D1_OK &&
			      res.entries[0].status == D1_OK,
		      "and every slot is still free for a write of its own");
	}
	env_init(&env, s, admissions[D1_MAX_OBJECTS + 1u], D1_OP_WRITE_BATCH);
	env.object = keys[D1_MAX_OBJECTS + 1u];
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 0, 11, 9, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	check(d1_store_apply(s, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_NOSPC,
	      "and only the one past them runs out of slots");

	d1_store_free(s);
}

/*
 * A copy of a handle's bytes is not a handle of another domain.
 *
 * Distinct C types stop a caller writing the substitution down, and
 * that is worth having, but it is a compile-time fact about one
 * expression and not a property of the value.  The four wrappers had
 * the same layout and public fields, so a memcpy from an admission into
 * a version, or a literal assembled from an admission's number, made
 * the version the compiler had refused to make -- and the first
 * admission and the first version are both one, so it selected a real
 * row and took custody of it.
 *
 * The value carries its domain now, and a resolver asks for it before
 * it chooses a table.  Bytes copied out of an admission still say
 * admission; a literal says nothing at all.
 */
static void test_a_handle_keeps_its_domain_through_a_copy(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	static uint8_t data[16];
	d1_admission_id admission;
	d1_version_id version, copied, literal;

	memset(data, 0x37, sizeof(data));
	fill_uuid(&store_uuid, 0x37);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	version = commit_chunk(s, admission, 0, 1, data, sizeof(data),
			       &(struct d1_guard){ .never_written = true },
			       d1_version_none(), NULL);
	check(d1_admission_raw(admission) == 1 && d1_version_raw(version) == 1,
	      "the first admission and the first version are both one");

	/* The positive control: the store's own version takes custody. */
	check(d1_custody_live(d1_fixture_custody(s, version)),
	      "the version this store issued names its row");

	/* The blind review's memcpy, byte for byte. */
	memset(&copied, 0, sizeof(copied));
	memcpy(&copied, &admission,
	       sizeof(copied) < sizeof(admission) ? sizeof(copied) :
						    sizeof(admission));
	check(d1_version_raw(copied) == d1_version_raw(version),
	      "an admission's bytes carry the same number as the version");
	check(!d1_custody_live(d1_fixture_custody(s, copied)),
	      "and still name nothing in the version table");

	/* And the literal assembled from the number a caller can read. */
	literal = (d1_version_id){ .raw = d1_admission_raw(admission) };
	check(d1_version_raw(literal) == d1_version_raw(version),
	      "a literal carries the same number too");
	check(!d1_custody_live(d1_fixture_custody(s, literal)),
	      "and names nothing either");

	/* The store's own handle is unharmed by any of it. */
	check(d1_custody_live(d1_fixture_custody(s, version)),
	      "while the store's own handle still names its row");

	d1_store_free(s);
}

/*
 * Two live stores of one name are two stores.
 *
 * The UUID is a store's durable name -- what a journal carries and what
 * a rebuild checks -- and it is not a name for one live object.  This
 * model opens two objects of one name on purpose: a source and a
 * pristine target for its own log.  They have separate locks, tables
 * and counters, they count to the same numbers, and they can be granted
 * different authority over the same object.  A handle from one of them
 * must not select a row in the other, and a durable name cannot tell
 * them apart, so a handle carries the live object that issued it.
 */
static void test_two_live_stores_of_one_name_are_two_stores(void)
{
	struct d1_uuid shared_uuid;
	struct d1_store *a, *b;
	struct d1_envelope env;
	struct d1_result res;
	struct d1_selection_spec sel;
	struct d1_view *view = NULL;
	static uint8_t data[16];
	d1_admission_id read_a, write_b;
	d1_version_id v_b, seen;

	memset(data, 0x38, sizeof(data));
	fill_uuid(&shared_uuid, 0x38);
	a = d1_store_open(&shared_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	b = d1_store_open(&shared_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!a || !b) {
		d1_store_free(a);
		d1_store_free(b);
		return;
	}

	/* One name, two objects, two different grants, one number. */
	read_a = d1_fixture_admit(a, &object, 11, D1_RIGHT_READ);
	write_b = d1_fixture_admit(b, &object, 11,
				   D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	check(d1_admission_raw(read_a) == d1_admission_raw(write_b),
	      "two stores of one name count to the same admission");
	check(!d1_admission_eq(read_a, write_b),
	      "and it is not the same admission");

	/* A's read-only handle, presented to B, which granted a writer. */
	env_init(&env, b, read_a, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	env.body.write.activate = true;
	write_entry(&env.body.write.entries[0], 0, 11, 1, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	check(d1_store_apply(b, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_STALE_AUTH,
	      "A's handle authorizes nothing in B, one name or not");
	check(res.entries[0].disposition == D1_UNRECORDED,
	      "recording nothing there");
	check(!d1_store_visible(b, &object, 0, &seen),
	      "and publishing nothing there");

	/* B's own handle, the same request, does the work. */
	env.admission = write_b;
	env.key.sequence = next_sequence++;
	check(d1_store_apply(b, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_OK,
	      "while B's own handle does the work");
	v_b = res.entries[0].version;

	/* Nor does A's handle read there. */
	ordinary_sel(&sel);
	check(d1_view_open(b, &object, read_a, &sel, 0, CHUNK_BYTES, &view) ==
		      D1_STALE_AUTH,
	      "nor does it open a view there");

	/* And B's version is B's, in a store that never wrote it. */
	check(!d1_custody_live(d1_fixture_custody(a, v_b)),
	      "B's version takes no custody in A");
	check(d1_custody_live(d1_fixture_custody(b, v_b)),
	      "and takes custody in B");

	d1_store_free(a);
	d1_store_free(b);
}

/*
 * A handle a decoder made names nothing, whatever the store is called.
 *
 * A decoder reads a number out of canonical bytes and has no store to
 * bind it to.  Replay binds it, to the store it is rebuilding, and
 * until then the value names nothing anywhere -- including in a store
 * opened under the all-zero name, which is a store like any other and
 * not a wildcard.
 */
static void test_a_decoded_handle_names_no_store(void)
{
	struct d1_uuid store_uuid, zero_uuid;
	struct d1_store *s, *zero;
	struct d1_envelope env, decoded;
	struct d1_result res;
	uint8_t bytes[D1_ENVELOPE_MAX];
	size_t len;
	static uint8_t data[16];
	d1_admission_id admission;
	d1_version_id seen;

	memset(data, 0x39, sizeof(data));
	fill_uuid(&store_uuid, 0x39);
	memset(&zero_uuid, 0, sizeof(zero_uuid));
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	zero = d1_store_open(&zero_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s || !zero) {
		d1_store_free(s);
		d1_store_free(zero);
		return;
	}
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	check(d1_admission_raw(admission) == 1, "a handle to put on the wire");

	env_init(&env, s, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	env.body.write.activate = true;
	write_entry(&env.body.write.entries[0], 0, 11, 1, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	len = d1_envelope_encode(&env, bytes, sizeof(bytes));
	check(len != 0, "which encodes");

	memset(&decoded, 0, sizeof(decoded));
	check(d1_envelope_decode(bytes, len, &decoded),
	      "and decodes back to the same request");
	check(d1_admission_raw(decoded.admission) == 1,
	      "carrying the number and nothing else");

	/*
	 * The zero-named store admits its own first handle at the same
	 * number, so the only thing that can separate them is the issuer.
	 */
	(void)d1_fixture_admit(zero, &object, 11,
			       D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	decoded.incarnation = d1_store_incarnation(zero);
	check(d1_store_apply(zero, &decoded, &res) == D1_OK &&
		      res.entries[0].status == D1_STALE_AUTH &&
		      res.entries[0].disposition == D1_UNRECORDED,
	      "a decoded handle authorizes nothing in the zero-named store");
	check(!d1_store_visible(zero, &object, 0, &seen),
	      "and publishes nothing there");
	check(!d1_custody_live(
		      d1_fixture_custody(zero, (d1_version_id){ .raw = 1 })),
	      "nor does an untyped value with the same number");

	d1_store_free(s);
	d1_store_free(zero);
}

/*
 * A handle names one kind of thing, in one store.
 *
 * Every store starts each of its counters at one, so two stores' first
 * admissions were both the number one -- and one number is all a handle
 * used to be.  An envelope carrying A's admission applied to B, and B
 * resolved the number against its own unrelated authority row and did
 * the work; an admission passed where a version belongs was resolved as
 * a version, and issued custody over it.  Neither is reachable now: the
 * handle carries the domain, which settles which table is asked, and
 * the live store that issued it, which settles whose table.
 *
 * What a caller can still do is name any number for the store it is
 * talking to, because that is all the wire carries.  A number the store
 * never issued names no row, which is the answer it always was.
 */
static void test_a_handle_names_its_own_store(void)
{
	struct d1_uuid uuid_a, uuid_b;
	struct d1_store *a, *b;
	struct d1_envelope env;
	struct d1_result res;
	struct d1_selection_spec sel;
	struct d1_view *view = NULL;
	static uint8_t data[16];
	d1_admission_id admit_a, admit_b;
	d1_version_id v_a, seen;
	d1_txn_id txn_a;
	d1_custody_id cust;

	memset(data, 0x30, sizeof(data));
	fill_uuid(&uuid_a, 0x30);
	fill_uuid(&uuid_b, 0x31);
	a = d1_store_open(&uuid_a, CHUNK_BYTES, MAX_FILE_BYTES);
	b = d1_store_open(&uuid_b, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!a || !b) {
		d1_store_free(a);
		d1_store_free(b);
		return;
	}
	admit_a = d1_fixture_admit(a, &object, 11,
				   D1_RIGHT_READ | D1_RIGHT_WRITE |
					   D1_RIGHT_SINGLE_WRITER);
	admit_b = d1_fixture_admit(b, &object, 11,
				   D1_RIGHT_READ | D1_RIGHT_WRITE |
					   D1_RIGHT_SINGLE_WRITER);
	check(d1_admission_live(admit_a) && d1_admission_live(admit_b),
	      "both stores admit a handle");
	check(admit_a.raw == admit_b.raw,
	      "and both counted to the same number");
	check(!d1_admission_eq(admit_a, admit_b),
	      "which is not the same handle");

	/* The positive control: A's handle works in A. */
	env_init(&env, a, admit_a, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	env.body.write.activate = true;
	write_entry(&env.body.write.entries[0], 0, 11, 1, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	check(d1_store_apply(a, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_OK,
	      "a store's own handle authorizes work in it");
	v_a = res.entries[0].version;
	txn_a = res.entries[0].txn;

	/* The same envelope, the same number, the other store. */
	env.object = object;
	env.incarnation = d1_store_incarnation(b);
	env.key.sequence = next_sequence++;
	check(d1_store_apply(b, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_STALE_AUTH,
	      "and authorizes nothing in another store");
	check(res.entries[0].disposition == D1_UNRECORDED,
	      "recording nothing there");
	check(!d1_store_visible(b, &object, 0, &seen),
	      "and publishing nothing there");

	/* A read is the same question. */
	ordinary_sel(&sel);
	check(d1_view_open(b, &object, admit_a, &sel, 0, CHUNK_BYTES, &view) ==
		      D1_STALE_AUTH,
	      "nor does it open a view there");

	/* A transaction and a version are no more portable. */
	owner_sel(&sel, txn_a, 11, 1, 0);
	check(d1_view_open(b, &object, admit_b, &sel, 0, CHUNK_BYTES, &view) ==
		      D1_INVALID,
	      "another store's transaction selects nothing here");
	check(!d1_custody_live(d1_fixture_custody(b, v_a)),
	      "and another store's version takes no custody here");
	check(d1_custody_live(d1_fixture_custody(a, v_a)),
	      "while its own store issues custody over it");
	check(!d1_fixture_release_predecessor(b, v_a),
	      "and another store's version is released by nobody else");

	/*
	 * A number the store never issued is refused by the store that
	 * would have issued it, which is the answer that was always
	 * right: it names no row.
	 */
	cust = d1_fixture_custody(a, d1_fixture_version_handle(a, 999999u));
	check(!d1_custody_live(cust),
	      "a version number this store never issued names nothing");

	d1_store_free(a);
	d1_store_free(b);
}

/*
 * A handle of the wrong kind is not a handle.
 *
 * The cross-type half of the same defect: an admission and a version
 * were both a u64, so an admission with the value one could be handed
 * to custody, where version one existed, and custody was issued over a
 * version the caller never named.  There is no conversion between the
 * two now -- the compiler refuses it, which is why this test proves the
 * property with the values instead.
 */
static void test_a_handle_names_its_own_kind(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_envelope env;
	struct d1_result res;
	static uint8_t data[16];
	d1_admission_id admission;
	d1_version_id version;
	d1_txn_id txn;

	memset(data, 0x32, sizeof(data));
	fill_uuid(&store_uuid, 0x32);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	env_init(&env, s, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	env.body.write.activate = true;
	write_entry(&env.body.write.entries[0], 0, 11, 1, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	check(d1_store_apply(s, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_OK,
	      "a write issues a transaction and a version");
	version = res.entries[0].version;
	txn = res.entries[0].txn;

	/*
	 * The collision the blind probe used: the first admission, the
	 * first transaction and the first version are all the number one.
	 */
	check(admission.raw == 1 && txn.raw == 1 && version.raw == 1,
	      "and all three are the number one");

	/*
	 * d1_fixture_custody(s, admission) does not compile: its parameter
	 * is a version handle and an admission is a different type.  What
	 * a caller can still do is name the admission's number as a
	 * version -- and that is a version handle, which resolves as one.
	 */
	check(d1_custody_live(d1_fixture_custody(
		      s, d1_fixture_version_handle(s, admission.raw))),
	      "naming the number as a version reaches the version");
	check(d1_custody_live(d1_fixture_custody(s, version)),
	      "which is the same thing as naming the version");

	/* And a transaction's number is not an admission's authority. */
	env_init(&env, s, d1_fixture_admission_handle(s, txn.raw),
		 D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 1, 11, 2, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	check(d1_store_apply(s, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_OK,
	      "the number one is also this store's admission, and works");

	d1_store_free(s);
}

/*
 * Replay reconstructs the handles the log names, for the store doing
 * the rebuilding.
 *
 * A journal carries values and no provenance at all, so a decoded
 * record names nothing until the store rebuilding it adopts the values
 * as its own.  What it adopts them for is the store doing the
 * rebuilding: a rebuild is a second live store, with its own tables and
 * its own token, even when it carries the name the log was written
 * under.  So the two agree about every value the log carried and about
 * nothing else, and the source's handles are not the rebuild's.
 */
static void test_replay_rebuilds_the_same_handles(void)
{
	struct d1_uuid store_uuid, other_uuid;
	struct d1_store *live, *rebuilt, *foreign;
	static uint8_t data[16];
	const uint8_t *log;
	size_t len;
	d1_admission_id admission;
	d1_version_id v1, seen;

	memset(data, 0x33, sizeof(data));
	fill_uuid(&store_uuid, 0x33);
	fill_uuid(&other_uuid, 0x34);
	live = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!live)
		return;
	d1_store_journal_enable(live);
	admission = d1_fixture_admit(live, &object, 11,
				     D1_RIGHT_READ | D1_RIGHT_WRITE |
					     D1_RIGHT_SINGLE_WRITER);
	v1 = commit_chunk(live, admission, 0, 1, data, sizeof(data),
			  &(struct d1_guard){ .never_written = true },
			  d1_version_none(), NULL);
	check(d1_version_live(v1), "a history to rebuild");
	log = journal_of(live, &len);

	rebuilt = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (rebuilt && log) {
		check(d1_store_replay(rebuilt, log, len) == D1_OK,
		      "the log rebuilds a store of the same name");
		check(d1_store_visible(rebuilt, &object, 0, &seen),
		      "which has the version the log recorded");
		check(same_version_value(seen, v1),
		      "carrying the value the log carried");
		check(!d1_version_eq(seen, v1),
		      "and not the source's handle, which is the source's");
		check(d1_custody_live(d1_fixture_custody(
			      rebuilt, renamed_version(rebuilt, v1))),
		      "so the rebuild names that value in its own table");
		check(!d1_custody_live(d1_fixture_custody(rebuilt, v1)),
		      "while the source's handle names nothing there");
		d1_store_free(rebuilt);
	}

	/* And a store of another name is another store, log or no log. */
	foreign = d1_store_open(&other_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (foreign) {
		check(d1_store_replay(foreign, log, len) == D1_IO,
		      "a store of another name refuses the log");
		check(!d1_custody_live(d1_fixture_custody(foreign, v1)),
		      "and does not know its handles either");
		d1_store_free(foreign);
	}
	d1_store_free(live);
}

/* H: the log rebuilds the store that wrote it. */
static void test_replay_reproduces_the_store(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *a, *b;
	const uint8_t *log;
	size_t len;
	d1_admission_id admission;

	fill_uuid(&store_uuid, 0x88);
	a = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!a)
		return;
	check(d1_store_journal_enable(a) == D1_OK, "journalling starts");
	admission = d1_fixture_admit(a, &object, 11,
				     D1_RIGHT_READ | D1_RIGHT_WRITE |
					     D1_RIGHT_REPAIR |
					     D1_RIGHT_SINGLE_WRITER);
	check(d1_version_live(drive_history(a, admission)),
	      "the history is written");

	log = journal_of(a, &len);
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

		(void)journal_of(b, &blen);
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
	d1_admission_id admission;
	d1_admission_id admission_b;
	static uint8_t data[64];

	memset(data, 0x31, sizeof(data));
	fill_uuid(&store_uuid, 0x99);
	a = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!a)
		return;
	d1_store_journal_enable(a);
	admission = d1_fixture_admit(a, &object, 11,
				     D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);

	check(d1_version_live(
		      commit_chunk(a, admission, 0, 1, data, sizeof(data),
				   &(struct d1_guard){ .never_written = true },
				   d1_version_none(), NULL)),
	      "the first chunk commits");
	(void)journal_of(a, &prefix);

	check(d1_version_live(
		      commit_chunk(a, admission, 2, 2, data, sizeof(data),
				   &(struct d1_guard){ .never_written = true },
				   d1_version_none(), NULL)),
	      "the second chunk commits");
	log = journal_of(a, &len);
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
		     &(struct d1_guard){ .never_written = true },
		     d1_version_none(), NULL);

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
	d1_admission_id admission;
	d1_version_id seen;
	d1_txn_id first_txn;

	memset(data, 0x41, sizeof(data));
	fill_uuid(&store_uuid, 0xaa);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	d1_store_journal_enable(s);
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	(void)journal_of(s, &before);

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
	(void)journal_of(s, &after);
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
	check(first_txn.raw == 1,
	      "and consumes the first transaction ID, not the second");
	(void)journal_of(s, &after);
	check(after > before, "and the log grew this time");
	check(d1_store_guard(s, &object, 0, &guard) && !guard.never_written,
	      "and the chunk now has a guard");

	/* A flush that does not happen is the same kind of nothing. */
	d1_fixture_fail_next_flush(s);
	(void)journal_of(s, &before);
	env_init(&env, s, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 1, 11, 2, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	d1_store_apply(s, &env, &res);
	check(res.entries[0].disposition == D1_UNRECORDED,
	      "an unflushed event is UNRECORDED too");
	(void)journal_of(s, &after);
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
	d1_admission_id admission;
	d1_admission_id clean_admission;
	d1_version_id visible;
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
		      d1_version_eq(retry.entries[0].version,
				    first.entries[0].version) &&
		      d1_txn_eq(retry.entries[0].txn, first.entries[0].txn),
	      "the first member returns its recorded result");
	check(retry.entries[1].status == D1_OK &&
		      retry.entries[1].disposition == D1_COMPLETED,
	      "the second member executes now");
	check(retry.entries[2].status == D1_OK &&
		      retry.entries[2].disposition == D1_COMPLETED,
	      "and so does the third");
	check(retry.entries[1].txn.raw < retry.entries[2].txn.raw,
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
	d1_admission_id admission;
	d1_version_id visible;
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
	d1_admission_id admission;
	d1_version_id seen;
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
	d1_admission_id admission;
	d1_admission_id second;

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
	check(d1_version_eq(again.entries[0].version,
			    first.entries[0].version) &&
		      d1_txn_eq(again.entries[0].txn, first.entries[0].txn),
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
	d1_admission_id admission;
	d1_admission_id again;
	d1_txn_id txn;
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
	check(d1_txn_live(txn), "there is pending work to recover");
	d1_store_verifier(a, verifier_before);
	log = journal_of(a, &len);

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
	check(d1_admission_live(again) && !d1_admission_eq(again, admission),
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
	log = journal_of(b, &len);
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
	d1_admission_id admission;
	d1_version_id v1;
	d1_version_id v2;
	d1_version_id seen;
	d1_version_id stale;

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
			  &(struct d1_guard){ .never_written = true },
			  d1_version_none(), NULL);
	check(d1_version_live(v1), "the first version commits");
	check(!d1_store_overlay_active(s),
	      "and the store is not on the overlay");

	d1_store_guard(s, &object, 0, &guard);
	d1_fixture_fail_next_index(s);
	v2 = commit_chunk(s, admission, 0, 2, second, sizeof(second), &guard,
			  v1, NULL);
	check(d1_version_live(v2) && !d1_version_eq(v2, v1),
	      "the replacement commits despite the index fault");
	check(d1_store_overlay_active(s),
	      "and the store records that the two have diverged");

	/* The materialized pointer was left behind, on purpose. */
	check(d1_store_materialized(s, &object, 0, &stale) &&
		      d1_version_eq(stale, v1),
	      "the materialized index still names the predecessor");

	/* No read may serve it. */
	check(d1_store_visible(s, &object, 0, &seen) && d1_version_eq(seen, v2),
	      "but what is visible is the committed version");
	ordinary_sel(&sel);
	check(d1_view_open(s, &object, admission, &sel, 0, CHUNK_BYTES,
			   &view) == D1_OK,
	      "a view opens after the fault");
	check(d1_view_version(view, 0, &seen) && d1_version_eq(seen, v2),
	      "and selects the committed version, not the predecessor");
	check(d1_view_read(view, 0, got, sizeof(got), &got_len) == D1_OK &&
		      memcmp(got, second, sizeof(second)) == 0,
	      "and reads its bytes");
	d1_view_close(view);

	/* A rebuild from the log produces the same state, without the fault. */
	{
		struct d1_store *rebuilt;
		const uint8_t *log;
		size_t len;

		log = journal_of(s, &len);
		rebuilt =
			d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
		if (rebuilt) {
			check(d1_store_replay(rebuilt, log, len) == D1_OK,
			      "the log replays");
			check(!d1_store_overlay_active(rebuilt),
			      "and the fault did not replay with it");
			/* Unjournalled harness state, so it cannot. */
			check(d1_store_visible(rebuilt, &object, 0, &seen) &&
				      same_version_value(seen, v2),
			      "and the rebuilt store sees the committed "
			      "version");
			check(d1_store_materialized(rebuilt, &object, 0,
						    &stale) &&
				      same_version_value(stale, v2),
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
		d1_admission_id admission;
		d1_admission_id control;
		d1_admission_id extra;
		d1_version_id seen;
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

		switch (kind) {
		case 0:
			/* An ordinary ENTRY event. */
			(void)journal_of(live, &before);
			d1_fixture_fail_next_flush(live);
			env_init(&env, live, admission, D1_OP_WRITE_BATCH);
			env.body.write.count = 1;
			env.body.write.stability = D1_FILE_SYNC;
			env.body.write.activate = true;
			write_entry(&env.body.write.entries[0], 0, 11, 1, data,
				    sizeof(data), true,
				    &(struct d1_guard){ .never_written =
								true });
			d1_store_apply(live, &env, &res);
			check(res.entries[0].disposition == D1_UNRECORDED,
			      "the entry event is UNRECORDED");
			(void)journal_of(live, &len);
			check(len == before, "and claims no durable bytes");
			check(!d1_store_visible(live, &object, 0, &seen),
			      "and published nothing");
			break;
		case 1: {
			/*
			 * An envelope-borne control that really transitions
			 * something.  A recovery_admit moves work from an
			 * expired handle to a live one, and the move is
			 * observable: only the handle that owns the work can
			 * finalize it.  An invalid control would be refused
			 * before it ever reached the journal, and so would
			 * prove nothing about the undo.
			 */
			d1_admission_id old;
			d1_admission_id fresh;
			d1_txn_id txn;

			old = d1_fixture_admit(live, &object, 11,
					       D1_RIGHT_WRITE |
						       D1_RIGHT_SINGLE_WRITER);
			fresh = d1_fixture_admit(
				live, &object, 11,
				D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
			env_init(&env, live, old, D1_OP_WRITE_BATCH);
			env.body.write.count = 1;
			env.body.write.stability = D1_FILE_SYNC;
			write_entry(&env.body.write.entries[0], 1, 11, 1, data,
				    sizeof(data), true,
				    &(struct d1_guard){ .never_written =
								true });
			check(d1_store_apply(live, &env, &res) == D1_OK &&
				      res.entries[0].status == D1_OK,
			      "an expiring handle leaves prepared work");
			txn = res.entries[0].txn;
			d1_fixture_expire(live, old);

			env_init(&env, live, admission, D1_OP_RECOVERY_ADMIT);
			env.body.control.count = 1;
			env.body.control.txns[0] = txn;
			env.body.control.old_admission = old;
			env.body.control.new_admission_present = true;
			env.body.control.new_admission = fresh;
			env.body.control.read_epoch_present = true;
			env.body.control.read_epoch = 0;

			(void)journal_of(live, &before);
			d1_fixture_fail_next_flush(live);
			d1_store_apply(live, &env, &res);
			check(res.entries[0].disposition == D1_UNRECORDED,
			      "the control event is UNRECORDED");
			(void)journal_of(live, &len);
			check(len == before, "and claims no durable bytes");

			/*
			 * The transition it would have made is not there:
			 * the new handle still does not own the work.  This
			 * is the assertion the journal undo has to earn --
			 * without it the publication stands in memory and
			 * the finalize below succeeds.
			 */
			check(finalize_txn(live, fresh, 1, 11, 1, txn) ==
				      D1_STALE_AUTH,
			      "and the work did not move to the new handle");

			/* The same control, unobstructed, does move it. */
			check(d1_store_apply(live, &env, &res) == D1_OK &&
				      res.entries[0].status == D1_OK,
			      "the same control succeeds once nothing fails");
			check(finalize_txn(live, fresh, 1, 11, 1, txn) == D1_OK,
			      "and then the new handle owns the work");
			break;
		}
		default:
			/* A fixture control event. */
			(void)journal_of(live, &before);
			d1_fixture_fail_next_flush(live);
			control = d1_fixture_admit(live, &object, 12,
						   D1_RIGHT_READ);
			check(!d1_admission_live(control),
			      "the fixture control event is refused");
			(void)journal_of(live, &len);
			check(len == before, "and claims no durable bytes");
			break;
		}

		/* Something unrelated now succeeds and flushes. */
		(void)journal_of(live, &before);
		extra = d1_fixture_admit(live, &object, 13, D1_RIGHT_READ);
		check(d1_admission_live(extra),
		      "an unrelated control event succeeds");
		log = journal_of(live, &len);
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
	d1_admission_id admission;
	d1_version_id seen_live;
	d1_version_id seen_rebuilt;

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

	log = journal_of(live, &len);
	rebuilt = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!rebuilt) {
		d1_store_free(live);
		return;
	}
	check(d1_store_replay(rebuilt, log, len) == D1_OK, "the log rebuilds");
	check(d1_store_visible(live, &object, 0, &seen_live) &&
		      d1_store_visible(rebuilt, &object, 0, &seen_rebuilt) &&
		      same_version_value(seen_live, seen_rebuilt),
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
	d1_admission_id writer_adm;
	d1_admission_id repair_adm;
	d1_version_id v1;
	d1_version_id v2;
	d1_txn_id txn2;
	d1_custody_id custody;
	d1_version_id seen;

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
			  &(struct d1_guard){ .never_written = true },
			  d1_version_none(), NULL);
	d1_store_guard(live, &object, 0, &guard);
	v2 = commit_chunk(live, writer_adm, 0, 2, data, sizeof(data), &guard,
			  v1, &txn2);
	custody = d1_fixture_custody(live, v2);
	check(d1_version_live(v1) && d1_version_live(v2) &&
		      d1_custody_live(custody),
	      "a replacement is committed under custody");

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
	check(d1_store_visible(live, &object, 0, &seen) &&
		      d1_version_eq(seen, v1),
	      "and the predecessor is visible");

	log = journal_of(live, &len);
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
		d1_admission_id other_writer;
		d1_admission_id other_repair;
		d1_version_id w1;
		d1_version_id w2;
		d1_txn_id t2;
		d1_custody_id bad;

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
				&(struct d1_guard){ .never_written = true },
				d1_version_none(), NULL);
			d1_store_guard(second, &object, 0, &guard);
			w2 = commit_chunk(second, other_writer, 0, 2, data,
					  sizeof(data), &guard, w1, &t2);
			bad = d1_fixture_custody(second, w1);
			check(d1_version_live(w2) && d1_custody_live(bad),
			      "a stale custody is issued");
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

			log = journal_of(second, &len);
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
	d1_admission_id admission;
	d1_admission_id control;
	d1_admission_id fresh;
	d1_txn_id txn;
	d1_version_id seen;

	memset(data, 0xd1, sizeof(data));
	fill_uuid(&store_uuid, 0x31);
	live = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!live)
		return;
	d1_store_journal_enable(live);
	admission = d1_fixture_admit(live, &object, 11,
				     D1_RIGHT_READ | D1_RIGHT_WRITE |
					     D1_RIGHT_SINGLE_WRITER);
	check(d1_version_live(finalize_chunk(
		      live, admission, 0, 1, data, sizeof(data),
		      &(struct d1_guard){ .never_written = true },
		      d1_version_none(), &txn)),
	      "a private version is finalized");

	/* Before the reopen, its owner can select it. */
	owner_sel(&sel, txn, 11, 1, 0);
	check(d1_view_open(live, &object, admission, &sel, 0, CHUNK_BYTES,
			   &view) == D1_OK,
	      "its owner selects it while the incarnation is current");
	d1_view_close(view);
	view = NULL;

	log = journal_of(live, &len);
	reopened = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!reopened) {
		d1_store_free(live);
		return;
	}
	check(d1_store_reopen(reopened, log, len) == D1_OK,
	      "the store reopens");

	/*
	 * The client comes back holding numbers, which is all it was ever
	 * given, and presents them to the store serving now.
	 */
	admission = renamed_admission(reopened, admission);
	txn = renamed_txn(reopened, txn);

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
		d1_view_close(view);
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
	d1_view_close(view);

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
	d1_admission_id old_a;
	d1_admission_id old_b;
	d1_admission_id control;
	d1_txn_id txn;

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
	check(d1_txn_live(txn) && d1_admission_live(old_b),
	      "there is work and a second old handle");

	log = journal_of(live, &len);
	reopened = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!reopened) {
		d1_store_free(live);
		return;
	}
	d1_store_reopen(reopened, log, len);
	/* The numbers the client kept, presented to the store serving now. */
	old_a = renamed_admission(reopened, old_a);
	old_b = renamed_admission(reopened, old_b);
	txn = renamed_txn(reopened, txn);
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
	d1_admission_id admission;

	memset(data, 0xe1, sizeof(data));
	fill_uuid(&store_uuid, 0x33);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_READ | D1_RIGHT_WRITE |
					     D1_RIGHT_SINGLE_WRITER);

	check(d1_version_live(
		      commit_chunk(s, admission, 0, 1, data, sizeof(data),
				   &(struct d1_guard){ .never_written = true },
				   d1_version_none(), NULL)),
	      "chunk 0 commits a partial image");
	check(d1_version_live(
		      commit_chunk(s, admission, 3, 2, data, sizeof(data),
				   &(struct d1_guard){ .never_written = true },
				   d1_version_none(), NULL)),
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
	d1_view_close(view);
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
	d1_view_close(view);

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
	d1_admission_id admission;
	d1_version_id v1;
	d1_txn_id txn;

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
			  &(struct d1_guard){ .never_written = true },
			  d1_version_none(), NULL);
	check(d1_version_live(
		      commit_chunk(s, admission, 2, 2, big, sizeof(big),
				   &(struct d1_guard){ .never_written = true },
				   d1_version_none(), NULL)),
	      "a higher chunk is committed");
	d1_store_guard(s, &object, 0, &guard);
	check(d1_version_live(finalize_chunk(s, admission, 0, 3, small,
					     sizeof(small), &guard, v1, &txn)),
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
	d1_view_close(own);

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
	d1_admission_id admissions[D1_MAX_OBJECTS + 1u];
	d1_version_id seen;

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
		/*
		 * And it reads as the untouched object it still is: an
		 * ordinary view of an object with no writes opens, and a
		 * refused write must leave one exactly that empty.
		 */
		ordinary_sel(&sel);
		check(d1_view_open(s, &keys[i], admissions[i], &sel, 0,
				   CHUNK_BYTES, &view) == D1_OK,
		      "and the object reads as the empty one it still is");
		if (view) {
			check(d1_view_eof(view) == 0 &&
				      !d1_view_version(view, 0, &seen),
			      "with no EOF and no version in it");
			d1_view_close(view);
			view = NULL;
		}

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
	/*
	 * And it is the first transaction and version the store ever
	 * issued: the refusals took no IDs either.
	 */
	check(res.entries[0].txn.raw == 1 && res.entries[0].version.raw == 1,
	      "taking the first IDs, so the refusals consumed none");

	/*
	 * The owner association is free too.  A refused write must not
	 * leave its owner claimed, or the caller could never use it again.
	 */
	memset(&env, 0, sizeof(env));
	env.object = keys[0];
	env.admission = admissions[0];
	env.incarnation = d1_store_incarnation(s);
	env.key.origin = origin;
	env.key.sequence = next_sequence++;
	env.op = D1_OP_WRITE_BATCH;
	env.body.write.count = 1;
	env.body.write.stability = D1_DATA_SYNC;
	env.body.write.activate = true;
	write_entry(&env.body.write.entries[0], 0, 11, 1, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	d1_store_apply(s, &env, &res);
	check(res.entries[0].status == D1_OK,
	      "the owner the refused write named is free to use");
	check(d1_store_visible(s, &keys[0], 0, &seen),
	      "and the object it could not create is created now");

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
	d1_admission_id readonly;
	d1_admission_id writer;

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
	log = journal_of(live, &len);
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
	d1_admission_id control;
	d1_admission_id old;
	d1_admission_id fresh;
	d1_admission_id reaped;
	d1_txn_id txn_recovered;
	d1_txn_id txn_reaped;

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
	check(d1_txn_live(txn_recovered) && d1_txn_live(txn_reaped),
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

	log = journal_of(live, &len);
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
		env.body.lifecycle.entries[0].owner.cohort.raw = 1;
		env.body.lifecycle.entries[0].owner.writer = 11;
		env.body.lifecycle.entries[0].owner.co_id = 1;
		env.body.lifecycle.entries[0].txn = txn_recovered;
		memcpy(env.body.lifecycle.prior_verifier, verifier,
		       sizeof(verifier));
		d1_store_apply(live, &env, &live_res);
		/*
		 * The same request at the rebuilt store, named at it: the
		 * numbers are the ones the log carried, and the store they
		 * are presented to is the one that has to resolve them.
		 */
		env.admission = renamed_admission(rebuilt, fresh);
		env.incarnation = d1_store_incarnation(rebuilt);
		env.body.lifecycle.entries[0].txn =
			renamed_txn(rebuilt, txn_recovered);
		d1_store_verifier(rebuilt, verifier);
		memcpy(env.body.lifecycle.prior_verifier, verifier,
		       sizeof(verifier));
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
 * A control acts on every transaction it names, or on none of them.
 *
 * A recovery_admit that names a transaction it may not move refuses the
 * whole request, and the members ahead of the bad one stay where they
 * were.  A refusal is still a recorded event, so the retry has to see
 * the same thing.
 */
static void test_control_members_are_all_or_nothing(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *live, *rebuilt;
	struct d1_envelope env;
	struct d1_result res;
	const uint8_t *log;
	size_t len;
	static uint8_t data[16];
	d1_admission_id control;
	d1_admission_id old;
	d1_admission_id fresh;
	d1_admission_id other;
	d1_txn_id first;
	d1_txn_id second;

	memset(data, 0x41, sizeof(data));
	fill_uuid(&store_uuid, 0x5d);
	live = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!live)
		return;
	check(d1_store_journal_enable(live) == D1_OK, "journalling starts");
	control = d1_fixture_admit(live, &object, 11, D1_RIGHT_CONTROL);
	old = d1_fixture_admit(live, &object, 11,
			       D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	fresh = d1_fixture_admit(live, &object, 11,
				 D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	other = d1_fixture_admit(live, &object, 11,
				 D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);

	/* One prepared transaction under each of two handles. */
	env_init(&env, live, old, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 0, 11, 1, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	check(d1_store_apply(live, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_OK,
	      "the expiring handle prepares work");
	first = res.entries[0].txn;

	env_init(&env, live, other, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 1, 11, 2, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	check(d1_store_apply(live, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_OK,
	      "and another handle prepares its own");
	second = res.entries[0].txn;

	d1_fixture_expire(live, old);

	/* A request naming both: the second is not the old handle's. */
	env_init(&env, live, control, D1_OP_RECOVERY_ADMIT);
	env.body.control.count = 2;
	env.body.control.txns[0] = first;
	env.body.control.txns[1] = second;
	env.body.control.old_admission = old;
	env.body.control.new_admission_present = true;
	env.body.control.new_admission = fresh;
	env.body.control.read_epoch_present = true;
	env.body.control.read_epoch = 0;
	check(d1_store_apply(live, &env, &res) == D1_OK &&
		      res.entries[0].status != D1_OK,
	      "a control naming work it may not move is refused");
	check(res.entries[0].disposition == D1_COMPLETED,
	      "and the refusal is recorded");
	check(finalize_txn(live, fresh, 0, 11, 1, first) == D1_STALE_AUTH,
	      "and the member ahead of the bad one did not move");
	check(finalize_txn(live, other, 1, 11, 2, second) == D1_OK,
	      "while the one it had no business with is untouched");

	/* The exact retry answers from the record. */
	check(d1_store_apply(live, &env, &res) == D1_OK &&
		      res.entries[0].status != D1_OK &&
		      res.entries[0].disposition == D1_COMPLETED,
	      "and the retry returns the recorded refusal");

	/* And the admissible request still moves what it names. */
	env_init(&env, live, control, D1_OP_RECOVERY_ADMIT);
	env.body.control.count = 1;
	env.body.control.txns[0] = first;
	env.body.control.old_admission = old;
	env.body.control.new_admission_present = true;
	env.body.control.new_admission = fresh;
	env.body.control.read_epoch_present = true;
	env.body.control.read_epoch = 0;
	check(d1_store_apply(live, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_OK,
	      "the request without the bad member succeeds");
	check(finalize_txn(live, fresh, 0, 11, 1, first) == D1_OK,
	      "and the work is the new handle's");

	log = journal_of(live, &len);
	rebuilt = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (rebuilt) {
		check(d1_store_replay(rebuilt, log, len) == D1_OK,
		      "the refusal and the success both rebuild");
		check(states_agree(live, rebuilt), "into the same store");
		d1_store_free(rebuilt);
	}
	d1_store_free(live);
}

/*
 * Reconstruction begins empty, and says so rather than reducing on top
 * of whatever it finds.
 *
 * A history applied to a populated target produces neither the logged
 * store nor the one that was there.  The refusal comes before anything
 * is touched, so a rejected target is still exactly the store it was --
 * and is not poisoned, because nothing was half-applied.
 */
static void test_replay_requires_a_pristine_target(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *source, *populated, *fresh;
	const uint8_t *start_only;
	const uint8_t *full;
	size_t start_len, full_len;
	static uint8_t data[8];
	d1_admission_id admission;
	d1_version_id seen_before;
	d1_version_id seen_after;

	memset(data, 0x2a, sizeof(data));
	fill_uuid(&store_uuid, 0x51);

	/* A START-only log, and a longer one from the same store. */
	source = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!source)
		return;
	check(d1_store_journal_enable(source) == D1_OK, "journalling starts");
	start_only = journal_of(source, &start_len);
	check(start_len > 0, "the START record is durable");
	{
		static uint8_t saved[4096];

		check(start_len <= sizeof(saved), "the START record is small");
		memcpy(saved, start_only, start_len);
		admission = d1_fixture_admit(source, &object, 11,
					     D1_RIGHT_WRITE |
						     D1_RIGHT_SINGLE_WRITER);
		check(d1_version_live(commit_chunk(
			      source, admission, 0, 1, data, sizeof(data),
			      &(struct d1_guard){ .never_written = true },
			      d1_version_none(), NULL)),
		      "and the source goes on to commit");
		full = journal_of(source, &full_len);

		/* A target that has already done work of its own. */
		populated =
			d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
		if (!populated) {
			d1_store_free(source);
			return;
		}
		admission = d1_fixture_admit(populated, &object, 11,
					     D1_RIGHT_WRITE |
						     D1_RIGHT_SINGLE_WRITER);
		check(d1_version_live(commit_chunk(
			      populated, admission, 0, 1, data, sizeof(data),
			      &(struct d1_guard){ .never_written = true },
			      d1_version_none(), NULL)),
		      "the target has data of its own");
		check(d1_store_visible(populated, &object, 0, &seen_before),
		      "which is visible");

		check(d1_store_replay(populated, saved, start_len) ==
			      D1_INVALID,
		      "a START-only history is refused by a populated target");
		check(d1_store_replay(populated, full, full_len) == D1_INVALID,
		      "and so is a longer one");
		check(d1_store_visible(populated, &object, 0, &seen_after) &&
			      d1_version_eq(seen_after, seen_before),
		      "and the target still holds exactly what it had");
		/* Refused, not poisoned: it still works. */
		check(d1_store_eof(populated, &object) == sizeof(data),
		      "and still answers");
		check(d1_admission_live(d1_fixture_admit(populated, &object, 12,
							 D1_RIGHT_READ)),
		      "and still admits");
		d1_store_free(populated);

		/* A correct fresh target still reconstructs. */
		fresh = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
		if (fresh) {
			check(d1_store_replay(fresh, full, full_len) == D1_OK,
			      "a fresh target reconstructs it");
			check(states_agree(source, fresh),
			      "into the same store");
			/* And a second reconstruction is refused. */
			check(d1_store_replay(fresh, full, full_len) ==
				      D1_INVALID,
			      "but it cannot be reconstructed into twice");
			d1_store_free(fresh);
		}
	}

	d1_store_free(source);
}

/*
 * A log describes everything its store did, so journalling cannot start
 * after the store has already done something.
 */
static void test_journal_enable_requires_a_pristine_store(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s, *poisoned;
	static uint8_t data[8];
	d1_admission_id admission;

	memset(data, 0x2b, sizeof(data));
	fill_uuid(&store_uuid, 0x52);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	check(d1_admission_live(admission),
	      "an admission is issued without a journal");
	check(d1_store_journal_enable(s) == D1_INVALID,
	      "journalling cannot start once authority exists unlogged");
	d1_store_free(s);

	/* Nor on a store whose rebuild failed. */
	{
		struct d1_store *live;
		const uint8_t *log;
		size_t len;
		uint8_t *writable;

		fill_uuid(&store_uuid, 0x53);
		live = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
		if (!live)
			return;
		d1_store_journal_enable(live);
		admission = d1_fixture_admit(live, &object, 11,
					     D1_RIGHT_WRITE |
						     D1_RIGHT_SINGLE_WRITER);
		commit_chunk(live, admission, 0, 1, data, sizeof(data),
			     &(struct d1_guard){ .never_written = true },
			     d1_version_none(), NULL);
		log = journal_of(live, &len);

		poisoned =
			d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
		if (poisoned) {
			writable = (uint8_t *)(uintptr_t)log;
			writable[len - 1] ^= 0xffu;
			check(d1_store_replay(poisoned, log, len) == D1_IO,
			      "a corrupt log fails the rebuild");
			writable[len - 1] ^= 0xffu;
			check(d1_store_journal_enable(poisoned) == D1_INVALID,
			      "and a poisoned handle cannot start a journal");
			check(!d1_admission_live(d1_fixture_admit(
				      poisoned, &object, 11, D1_RIGHT_READ)),
			      "nor accept fixture authority");
			check(!d1_custody_live(d1_fixture_custody(
				      poisoned,
				      d1_fixture_version_handle(poisoned, 1))),
			      "nor issue custody");
			d1_store_free(poisoned);
		}
		d1_store_free(live);
	}
}

/*
 * The never-written chunk accepts the initial guard, and the initial
 * guard is (0,0).  Those numbers are in the request and in its
 * canonical bytes whether or not the chunk has been written.
 */
static void test_initial_guard_is_zero_zero(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_envelope env;
	struct d1_result res, again;
	struct d1_guard guard;
	static uint8_t data[8];
	unsigned int pass;
	d1_admission_id admission;
	d1_version_id seen;

	memset(data, 0x2c, sizeof(data));
	fill_uuid(&store_uuid, 0x54);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);

	/* Generation and writer, each wrong on its own. */
	for (pass = 0; pass < 2; pass++) {
		struct d1_guard expected = { .never_written = true };

		if (pass == 0)
			expected.generation = 123;
		else
			expected.writer = 987;

		env_init(&env, s, admission, D1_OP_WRITE_BATCH);
		env.body.write.count = 1;
		env.body.write.stability = D1_FILE_SYNC;
		write_entry(&env.body.write.entries[0], 0, 11, pass + 1u, data,
			    sizeof(data), true, &expected);
		check(d1_store_apply(s, &env, &res) == D1_OK &&
			      res.entries[0].status == D1_GUARDED,
		      "a never-written predicate that is not (0,0) is refused");
		check(res.entries[0].disposition == D1_COMPLETED,
		      "and the refusal is recorded");
		check(res.entries[0].guard.never_written,
		      "and carries the current guard");
		check(!res.entries[0].txn_present &&
			      !res.entries[0].version_present,
		      "and reserved nothing");
		check(!d1_store_guard(s, &object, 0, &guard),
		      "and left the object uncreated");

		/* The exact retry answers from the recorded refusal. */
		check(d1_store_apply(s, &env, &again) == D1_OK &&
			      again.entries[0].status == D1_GUARDED &&
			      again.entries[0].disposition == D1_COMPLETED,
		      "and the exact retry returns it");
	}

	/* The initial guard itself is accepted, and its result is (0, 11). */
	env_init(&env, s, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_DATA_SYNC;
	env.body.write.activate = true;
	write_entry(&env.body.write.entries[0], 0, 11, 3, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	check(d1_store_apply(s, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_OK,
	      "the initial guard (0,0) is accepted");
	check(res.entries[0].guard.generation == 0 &&
		      res.entries[0].guard.writer == 11 &&
		      !res.entries[0].guard.never_written,
	      "and the first success is generation 0 with the granted writer");
	check(d1_store_visible(s, &object, 0, &seen), "and it publishes");

	d1_store_free(s);
}

/* An owner whose writer is not the granted one is a binding failure. */
static void test_writer_must_be_the_granted_one(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_envelope env;
	struct d1_result res;
	struct d1_guard guard;
	static uint8_t data[8];
	d1_admission_id admission;
	d1_version_id seen;

	memset(data, 0x2d, sizeof(data));
	fill_uuid(&store_uuid, 0x55);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);

	env_init(&env, s, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	/* A valid, unreserved writer ID that is simply not this one. */
	write_entry(&env.body.write.entries[0], 0, 12, 1, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	check(d1_store_apply(s, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_STALE_AUTH,
	      "an owner naming another writer is a binding failure");
	check(res.entries[0].disposition == D1_COMPLETED,
	      "recorded like any other semantic error");
	check(!d1_store_guard(s, &object, 0, &guard),
	      "and the object it touched is not there");
	check(!d1_store_visible(s, &object, 0, &seen), "and nothing wrote");

	d1_store_free(s);
}

/*
 * The whole OWNER vector is admissible, or the view is not opened.
 *
 * Validating a member only when its chunk came up made admissibility
 * depend on order: a later member naming the same chunk was never
 * reached.  The same vector cannot be good one way round and bad the
 * other, and a refused vector must leave no view and no pins.
 */
static void test_owner_vector_is_validated_whole(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_selection_spec sel;
	struct d1_view *view = NULL;
	struct d1_guard guard;
	static uint8_t data[32];
	struct d1_view *held[D1_MAX_VIEWS];
	d1_admission_id admission;
	d1_version_id v1;
	d1_txn_id t1;
	d1_txn_id t2;
	unsigned int pass, n;

	memset(data, 0x3a, sizeof(data));
	fill_uuid(&store_uuid, 0x56);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_READ | D1_RIGHT_WRITE |
					     D1_RIGHT_REPAIR |
					     D1_RIGHT_SINGLE_WRITER);

	v1 = commit_chunk(s, admission, 0, 1, data, sizeof(data),
			  &(struct d1_guard){ .never_written = true },
			  d1_version_none(), &t1);
	d1_store_guard(s, &object, 0, &guard);
	check(d1_version_live(finalize_chunk(s, admission, 0, 2, data,
					     sizeof(data), &guard, v1, &t2)),
	      "a committed and a finalized transaction share a chunk");

	/* Both orders of the same vector answer the same way. */
	for (pass = 0; pass < 2; pass++) {
		memset(&sel, 0, sizeof(sel));
		sel.selection = D1_SELECT_OWNER;
		sel.count = 2;
		sel.txns[0] = pass ? t1 : t2;
		sel.txns[1] = pass ? t2 : t1;
		sel.owners[0].cohort.raw = 1;
		sel.owners[0].writer = 11;
		sel.owners[0].co_id = pass ? 1 : 2;
		sel.owners[1].cohort.raw = 1;
		sel.owners[1].writer = 11;
		sel.owners[1].co_id = pass ? 2 : 1;
		check(d1_view_open(s, &object, admission, &sel, 0, CHUNK_BYTES,
				   &view) == D1_BAD_PHASE,
		      "a committed member fails the view whichever order it is in");
		check(view == NULL, "and no view is returned");
	}

	/* Two members resolving to one chunk is not a selection. */
	{
		d1_txn_id t3;
		struct d1_guard g2;

		/* Cancel the finalized one so the chunk is free again. */
		check(rollback_one(s, admission, 0, 2, t2,
				   &(struct rollback_expect){
					   .visible_present = true,
					   .visible = v1,
					   .predecessor_present = true,
					   .predecessor = v1 },
				   NULL) == D1_OK,
		      "the finalized transaction is cancelled");
		d1_store_guard(s, &object, 0, &g2);
		check(d1_version_live(finalize_chunk(s, admission, 0, 3, data,
						     sizeof(data), &g2, v1,
						     &t3)),
		      "and a new one finalizes on the same chunk");

		memset(&sel, 0, sizeof(sel));
		sel.selection = D1_SELECT_OWNER;
		sel.count = 2;
		sel.txns[0] = t3;
		sel.txns[1] = t3;
		sel.owners[0].cohort.raw = 1;
		sel.owners[0].writer = 11;
		sel.owners[0].co_id = 3;
		sel.owners[1] = sel.owners[0];
		check(d1_view_open(s, &object, admission, &sel, 0, CHUNK_BYTES,
				   &view) == D1_INVALID,
		      "a vector naming one transaction twice is refused");
		check(view == NULL, "with no view");

		/* And the good single-member vector still works. */
		owner_sel(&sel, t3, 11, 3, 0);
		check(d1_view_open(s, &object, admission, &sel, 0, CHUNK_BYTES,
				   &view) == D1_OK,
		      "while the admissible vector opens");
		d1_view_close(view);
		view = NULL;

		/*
		 * A refused vector leaves nothing behind.  Pin counts are
		 * not publicly observable, so what is asserted here is what
		 * is: every view slot is still free, which a view leaked by
		 * a refusal would have taken.  The no-pin half holds by
		 * construction -- d1_view_open takes no slot and pins
		 * nothing until d1_owner_resolve has accepted the whole
		 * vector -- and is stated rather than claimed as tested.
		 */
		for (n = 0; n < D1_MAX_VIEWS; n++)
			if (d1_view_open(s, &object, admission, &sel, 0,
					 CHUNK_BYTES, &held[n]) != D1_OK)
				break;
		check(n == D1_MAX_VIEWS, "every view slot is still free");
		while (n--)
			d1_view_close(held[n]);
	}

	d1_store_free(s);
}

/*
 * A record's outer tag is checked before anything is dispatched on it,
 * and has to agree with the schema it carries.
 *
 * These records cannot come from the live encoder.  Each is built by
 * copying a valid log, changing one field and recomputing that record's
 * CRC, so the semantic decoder is tested rather than the checksum.
 */
static void test_record_tags_are_validated(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *live, *target;
	static uint8_t copy[65536];
	const uint8_t *log;
	size_t len, at;
	static uint8_t data[8];
	d1_admission_id admission;
	uint32_t total, crc;
	unsigned int pass;

	memset(data, 0x3b, sizeof(data));
	fill_uuid(&store_uuid, 0x57);
	live = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!live)
		return;
	d1_store_journal_enable(live);
	admission = d1_fixture_admit(live, &object, 11,
				     D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	check(d1_version_live(
		      commit_chunk(live, admission, 0, 1, data, sizeof(data),
				   &(struct d1_guard){ .never_written = true },
				   d1_version_none(), NULL)),
	      "a history with a CONTROL and several ENTRYs");
	log = journal_of(live, &len);
	check(len <= sizeof(copy), "the log fits the fixture buffer");
	if (len > sizeof(copy)) {
		d1_store_free(live);
		return;
	}

	/*
	 * The second record is the fixture ADMIT control.  Its body begins
	 * with the outer kind, at the first byte after the 56-byte header.
	 */
	memcpy(copy, log, len);
	total = ((uint32_t)copy[12] << 24) | ((uint32_t)copy[13] << 16) |
		((uint32_t)copy[14] << 8) | (uint32_t)copy[15];
	at = total;

	for (pass = 0; pass < 2; pass++) {
		uint32_t body = (uint32_t)(at + D1_JOURNAL_HEADER_BYTES);
		uint32_t record_total;

		memcpy(copy, log, len);
		record_total = ((uint32_t)copy[at + 12] << 24) |
			       ((uint32_t)copy[at + 13] << 16) |
			       ((uint32_t)copy[at + 14] << 8) |
			       (uint32_t)copy[at + 15];
		if (pass == 0) {
			/* An outer kind no encoder can produce. */
			copy[body + 0] = 0xde;
			copy[body + 1] = 0xad;
			copy[body + 2] = 0xbe;
			copy[body + 3] = 0xef;
		} else {
			/* A known outer kind that is not the inner one. */
			copy[body + 0] = 0;
			copy[body + 1] = 0;
			copy[body + 2] = 0;
			copy[body + 3] = (uint8_t)D1_CTL_RELEASE;
		}
		/* Recompute the record's CRC so only the tag is wrong. */
		crc = d1_crc32c(copy + at,
				record_total - D1_JOURNAL_TRAILER_BYTES);
		copy[at + record_total - 4] = (uint8_t)(crc >> 24);
		copy[at + record_total - 3] = (uint8_t)(crc >> 16);
		copy[at + record_total - 2] = (uint8_t)(crc >> 8);
		copy[at + record_total - 1] = (uint8_t)crc;

		target =
			d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
		if (target) {
			check(d1_store_replay(target, copy, len) == D1_INVALID,
			      pass == 0 ?
				      "an unknown outer control tag is refused" :
				      "a mismatched outer control tag is refused");
			d1_store_free(target);
		}
	}

	/* The untouched log still rebuilds, so the fixture is honest. */
	target = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (target) {
		check(d1_store_replay(target, log, len) == D1_OK,
		      "and the unmodified log still rebuilds");
		check(states_agree(live, target), "into the same store");
		d1_store_free(target);
	}

	d1_store_free(live);
}

/*
 * A changed body cannot splice itself into an operation key that is
 * already recorded, and cannot stop the original from finishing.
 */
static void test_operation_key_binds_the_whole_envelope(void)
{
	static const unsigned int where[] = { 2, 3 };
	unsigned int pass;

	for (pass = 0; pass < 2; pass++) {
		struct d1_uuid store_uuid;
		struct d1_store *live, *rebuilt;
		struct d1_envelope env, changed;
		struct d1_result res;
		const uint8_t *log;
		size_t len;
		static uint8_t data[16];
		static uint8_t other[16];
		d1_admission_id admission;
		d1_admission_id spare;
		d1_version_id visible;
		unsigned int i;

		memset(data, 0x3c, sizeof(data));
		memset(other, 0x3d, sizeof(other));
		fill_uuid(&store_uuid, (uint8_t)(0x58 + pass));
		live = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
		if (!live)
			return;
		d1_store_journal_enable(live);
		admission = d1_fixture_admit(live, &object, 11,
					     D1_RIGHT_WRITE |
						     D1_RIGHT_SINGLE_WRITER);
		spare = d1_fixture_admit(live, &object, 11,
					 D1_RIGHT_WRITE |
						 D1_RIGHT_SINGLE_WRITER);

		env_init(&env, live, admission, D1_OP_WRITE_BATCH);
		env.body.write.count = 3;
		env.body.write.stability = D1_DATA_SYNC;
		env.body.write.activate = true;
		for (i = 0; i < 3; i++)
			write_entry(&env.body.write.entries[i], i, 11, i + 1u,
				    data, sizeof(data), true,
				    &(struct d1_guard){ .never_written =
								true });

		/*
		 * Interrupt once a member is already recorded: after the
		 * first, then at the last.  A batch that recorded nothing
		 * leaves the key free, which is its own test.
		 */
		d1_fixture_fail_append_in(live, where[pass]);
		check(d1_store_apply(live, &env, &res) == D1_OK,
		      "the batch runs and is interrupted");
		check(res.entries[where[pass] - 2u].disposition == D1_COMPLETED,
		      "the members before the fault are recorded");
		check(res.entries[where[pass] - 1u].disposition ==
			      D1_UNRECORDED,
		      "and the one it was aimed at is not");

		/* A changed later payload under the same key. */
		changed = env;
		write_entry(&changed.body.write.entries[2], 2, 11, 3, other,
			    sizeof(other), true,
			    &(struct d1_guard){ .never_written = true });
		check(d1_store_apply(live, &changed, &res) == D1_OK,
		      "a changed retry is answered");
		for (i = 0; i < 3; i++)
			check(res.entries[i].status == D1_REPLAY_CONFLICT,
			      "and every member conflicts");
		check(!res.entries[2].txn_present, "and none of them executed");

		/* A changed admission under the same key, likewise. */
		changed = env;
		changed.admission = spare;
		check(d1_store_apply(live, &changed, &res) == D1_OK &&
			      res.entries[0].status == D1_REPLAY_CONFLICT,
		      "and so does a changed admission");

		/* The original exact request can still be finished. */
		check(d1_store_apply(live, &env, &res) == D1_OK,
		      "the original exact retry runs");
		for (i = 0; i < 3; i++)
			check(res.entries[i].status == D1_OK,
			      "and every member of it succeeds");
		for (i = 0; i < 3; i++)
			check(d1_store_visible(live, &object, i, &visible),
			      "so every chunk is visible");

		/* And the history it wrote rebuilds. */
		log = journal_of(live, &len);
		rebuilt =
			d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
		if (rebuilt) {
			check(d1_store_replay(rebuilt, log, len) == D1_OK,
			      "the log rebuilds");
			check(states_agree(live, rebuilt),
			      "into the same store");
			d1_store_free(rebuilt);
		}
		d1_store_free(live);
	}
}

/* A key with nothing recorded under it is still free. */
static void test_unused_key_is_not_bound(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s;
	struct d1_envelope env, changed;
	struct d1_result res;
	static uint8_t data[16];
	static uint8_t other[16];
	d1_admission_id admission;
	d1_version_id visible;

	memset(data, 0x3e, sizeof(data));
	memset(other, 0x3f, sizeof(other));
	fill_uuid(&store_uuid, 0x5a);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	d1_store_journal_enable(s);
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);

	env_init(&env, s, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_DATA_SYNC;
	env.body.write.activate = true;
	write_entry(&env.body.write.entries[0], 0, 11, 1, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });

	/* The only member fails to become durable, so nothing is recorded. */
	d1_fixture_fail_next_flush(s);
	d1_store_apply(s, &env, &res);
	check(res.entries[0].disposition == D1_UNRECORDED,
	      "the only member is UNRECORDED");

	/* A different body may therefore use that key. */
	changed = env;
	write_entry(&changed.body.write.entries[0], 0, 11, 2, other,
		    sizeof(other), true,
		    &(struct d1_guard){ .never_written = true });
	check(d1_store_apply(s, &changed, &res) == D1_OK &&
		      res.entries[0].status == D1_OK,
	      "a key with no recorded member is still free");
	check(d1_store_visible(s, &object, 0, &visible), "and it publishes");

	d1_store_free(s);
}

/* Fault arms do not survive a reconstruction or a reopen. */
static void note_hook_fired(void *arg)
{
	*(bool *)arg = true;
}

static void test_recovery_clears_fault_arms(void)
{
	static const bool use_reopen[] = { false, true };
	unsigned int pass;

	for (pass = 0; pass < 2; pass++) {
		struct d1_uuid store_uuid;
		struct d1_store *live, *target;
		const uint8_t *log;
		size_t len, before, after;
		static uint8_t data[8];
		uint8_t *snap = NULL;
		size_t snap_len = 0;
		bool hook_fired = false;
		d1_admission_id admission;
		d1_admission_id fresh;
		d1_version_id seen;

		memset(data, 0x40, sizeof(data));
		fill_uuid(&store_uuid, (uint8_t)(0x5b + pass));
		live = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
		if (!live)
			return;
		d1_store_journal_enable(live);
		admission = d1_fixture_admit(live, &object, 11,
					     D1_RIGHT_WRITE |
						     D1_RIGHT_SINGLE_WRITER);
		check(d1_version_live(commit_chunk(
			      live, admission, 0, 1, data, sizeof(data),
			      &(struct d1_guard){ .never_written = true },
			      d1_version_none(), NULL)),
		      "a history to rebuild");
		log = journal_of(live, &len);

		target =
			d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
		if (!target) {
			d1_store_free(live);
			return;
		}
		/* Every arm the harness has, set before the rebuild. */
		d1_fixture_fail_next_index(target);
		d1_fixture_fail_next_append(target);
		d1_fixture_fail_next_flush(target);
		d1_fixture_fail_next_snapshot(target);
		d1_fixture_before_member(target, 0, note_hook_fired,
					 &hook_fired);

		if (use_reopen[pass])
			check(d1_store_reopen(target, log, len) == D1_OK,
			      "the store reopens");
		else
			check(d1_store_replay(target, log, len) == D1_OK,
			      "the store rebuilds");
		check(!d1_store_overlay_active(target),
		      "and no fault fired during it");

		/*
		 * The snapshot arm did not survive either, and this is the
		 * first snapshot after the rebuild -- any earlier one would
		 * spend the arm before the question is asked.  A reopen
		 * leaves a journal with a START in it; a read-only rebuild
		 * leaves none, and an empty snapshot would answer the same
		 * whether the arm were there or not.
		 */
		if (use_reopen[pass]) {
			check(d1_store_journal_snapshot(target, &snap,
							&snap_len) == D1_OK &&
				      snap_len != 0,
			      "and the first snapshot after it is ordinary");
			free(snap);
			snap = NULL;
		}

		/* The first operation afterwards is ordinary. */
		if (use_reopen[pass]) {
			fresh = d1_fixture_admit(
				target, &object, 11,
				D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
			check(d1_admission_live(fresh),
			      "a fresh handle is admitted");
			(void)journal_of(target, &before);
			check(d1_version_live(commit_chunk(
				      target, fresh, 2, 9, data, sizeof(data),
				      &(struct d1_guard){ .never_written =
								  true },
				      d1_version_none(), NULL)),
			      "and its first commit succeeds");
			(void)journal_of(target, &after);
			check(after > before,
			      "with its events actually claimed");
			check(!d1_store_overlay_active(target),
			      "and no index fault fired");
			check(d1_store_visible(target, &object, 2, &seen),
			      "and it published");
		} else {
			/*
			 * The arm fires on a publication, not on the rebuild,
			 * so reading the flag after the rebuild is not an
			 * oracle for it.  The first eligible operation
			 * afterwards is -- and after a read-only rebuild the
			 * log's own admission, named at the target, is the
			 * one that can make it.
			 */
			check(!d1_store_overlay_active(target),
			      "and the arm did not survive the rebuild");
			check(d1_version_live(commit_chunk(
				      target,
				      renamed_admission(target, admission), 2,
				      9, data, sizeof(data),
				      &(struct d1_guard){ .never_written =
								  true },
				      d1_version_none(), NULL)),
			      "the first commit after it succeeds");
			check(!d1_store_overlay_active(target),
			      "with no index fault firing");
			check(d1_store_visible(target, &object, 2, &seen),
			      "and it published");
		}
		check(!hook_fired,
		      "and the member hook set before it never ran");

		d1_store_free(target);
		d1_store_free(live);
	}
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
		d1_admission_id admission;
		d1_version_id v1;
		d1_version_id v2;
		d1_txn_id txn2;
		d1_custody_id custody;
		d1_version_id live_visible;
		d1_version_id rebuilt_visible;
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
				  d1_version_none(), NULL);
		d1_store_guard(live, &object, 0, &guard);
		v2 = commit_chunk(live, admission, 0, 2, data, sizeof(data),
				  &guard, v1, &txn2);
		check(d1_version_live(v1) && d1_version_live(v2),
		      "two versions commit in turn");

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
			      d1_version_eq(live_visible, v2),
		      "and the current data stays where it is");

		log = journal_of(live, &len);
		rebuilt =
			d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
		if (!rebuilt) {
			d1_store_free(live);
			return;
		}
		check(d1_store_replay(rebuilt, log, len) == D1_OK,
		      "the log replays without diverging");
		check(d1_store_visible(rebuilt, &object, 0, &rebuilt_visible) &&
			      same_version_value(rebuilt_visible, live_visible),
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
	d1_admission_id admission;
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
		     &(struct d1_guard){ .never_written = true },
		     d1_version_none(), NULL);
	log = journal_of(a, &len);

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
	d1_admission_id control;
	d1_admission_id old;
	d1_admission_id fresh;
	d1_txn_id txn;

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
	env.body.lifecycle.entries[0].owner.cohort.raw = 1;
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
	d1_admission_id control;
	d1_admission_id old;
	d1_admission_id fresh;
	d1_admission_id stranger;
	d1_txn_id txn;
	d1_txn_id other_txn;

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
	check(d1_txn_live(txn) && d1_txn_live(other_txn),
	      "two admissions prepare work");

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
	env.body.control.old_admission = d1_fixture_admission_handle(s, 9999);
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
	d1_admission_id control;
	d1_admission_id writer;
	d1_admission_id other;
	d1_txn_id txn_a;
	d1_txn_id txn_b;
	d1_version_id visible;

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
	check(d1_txn_live(txn_a) && d1_txn_live(txn_b),
	      "two transactions are prepared");
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
	env.body.control.txns[1] = d1_fixture_txn_handle(s, 999999);
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
	d1_admission_id admission;
	d1_version_id visible;

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
	env.body.control.txns[0] = d1_fixture_txn_handle(s, 1);
	env.body.control.old_admission = admission;
	env.body.control.new_admission_present = true;
	env.body.control.new_admission = d1_fixture_admission_handle(s, 2);
	check(d1_store_apply(s, &env, &res) == D1_INVALID,
	      "a reap naming a new admission is a different request");

	/* A recovery request missing the epoch it exists to grant. */
	env_init(&env, s, admission, D1_OP_RECOVERY_ADMIT);
	env.body.control.count = 1;
	env.body.control.txns[0] = d1_fixture_txn_handle(s, 1);
	env.body.control.old_admission = admission;
	env.body.control.new_admission_present = true;
	env.body.control.new_admission = d1_fixture_admission_handle(s, 2);
	check(d1_store_apply(s, &env, &res) == D1_INVALID,
	      "a recovery granting no read epoch is refused");

	check(!d1_store_visible(s, &object, 0, &visible),
	      "and none of them wrote anything");
	check(d1_store_eof(s, &object) == 0, "nor moved EOF");

	d1_store_free(s);
}

/*
 * Two callers under one operation key cannot splice their bodies.
 *
 * The preflight in d1_store_apply is an early answer, not the
 * guarantee.  The guarantee is the same question asked inside the lock
 * interval that records each member, and the difference between the two
 * is exactly the gap a second caller occupies when it is preempted
 * between finding the key free and running its first member.  A
 * scheduler reaches that gap rarely enough that not reaching it proves
 * nothing, so the fixture opens it instead of waiting for one.
 */
struct other_caller {
	struct d1_store *store;
	struct d1_envelope env;
	struct d1_result res;
	uint32_t call;
	bool ran;
};

static void run_other_caller(void *arg)
{
	struct other_caller *o = arg;

	o->ran = true;
	o->call = d1_store_apply(o->store, &o->env, &o->res);
}

static void test_concurrent_callers_cannot_splice_a_key(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *s, *rebuilt;
	struct other_caller first;
	struct d1_envelope second;
	struct d1_result res, retry;
	const uint8_t *log;
	size_t len;
	static uint8_t mine[16];
	static uint8_t theirs[16];
	d1_admission_id admission;
	d1_version_id seen;
	unsigned int i;

	memset(mine, 0x61, sizeof(mine));
	memset(theirs, 0x62, sizeof(theirs));
	fill_uuid(&store_uuid, 0x63);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	d1_store_journal_enable(s);
	admission = d1_fixture_admit(s, &object, 11,
				     D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);

	/* One key, two two-member bodies differing only in member 1. */
	memset(&first, 0, sizeof(first));
	first.store = s;
	env_init(&first.env, s, admission, D1_OP_WRITE_BATCH);
	first.env.body.write.count = 2;
	first.env.body.write.stability = D1_FILE_SYNC;
	write_entry(&first.env.body.write.entries[0], 0, 11, 1, mine,
		    sizeof(mine), true,
		    &(struct d1_guard){ .never_written = true });
	write_entry(&first.env.body.write.entries[1], 1, 11, 2, mine,
		    sizeof(mine), true,
		    &(struct d1_guard){ .never_written = true });
	second = first.env;
	write_entry(&second.body.write.entries[1], 1, 11, 2, theirs,
		    sizeof(theirs), true,
		    &(struct d1_guard){ .never_written = true });

	/*
	 * The second caller reaches the gap first and finds the key free.
	 * The first caller runs to completion inside it, which is what a
	 * preemption there amounts to.
	 */
	d1_fixture_before_member(s, 0, run_other_caller, &first);
	check(d1_store_apply(s, &second, &res) == D1_OK,
	      "the second caller's batch is admitted");
	check(first.ran, "after the first caller ran inside its window");
	check(first.call == D1_OK && first.res.entries[0].status == D1_OK &&
		      first.res.entries[1].status == D1_OK,
	      "and recorded both of its members");
	for (i = 0; i < 2; i++)
		check(res.entries[i].status == D1_REPLAY_CONFLICT,
		      "every member of the second body is refused");
	check(res.disposition == D1_COMPLETED,
	      "answered from the record the key already holds");

	/* Nothing of the second body reached the store. */
	check(d1_store_apply(s, &first.env, &retry) == D1_OK,
	      "the first caller's exact request still answers");
	for (i = 0; i < 2; i++) {
		check(retry.entries[i].status == D1_OK,
		      "from its own receipts, member by member");
		check(d1_version_eq(retry.entries[i].version,
				    first.res.entries[i].version),
		      "with the versions it was given the first time");
	}
	check(!d1_store_visible(s, &object, 1, &seen),
	      "and a private write published nothing either way");

	log = journal_of(s, &len);
	rebuilt = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (rebuilt) {
		check(d1_store_replay(rebuilt, log, len) == D1_OK,
		      "the log the pair left rebuilds");
		check(states_agree(s, rebuilt), "into the same store");
		d1_store_free(rebuilt);
	}
	d1_store_free(s);
}

/* Big-endian stores, the framing section 9 fixes. */
static void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static void put_be64(uint8_t *p, uint64_t v)
{
	put_be32(p, (uint32_t)(v >> 32));
	put_be32(p + 4, (uint32_t)v);
}

/* The framed length of the record at @r. */

/* Where each record of a durable log begins. */
static unsigned int index_log(const uint8_t *log, size_t len, size_t *at,
			      unsigned int max)
{
	unsigned int n = 0;
	size_t off = 0;

	while (off < len && n < max) {
		at[n++] = off;
		off += record_bytes(log + off);
	}
	return n;
}

/* Copy a framed record, re-stamp its LSN, recompute its CRC. */
static size_t restamp_record(uint8_t *dst, const uint8_t *src, uint64_t lsn)
{
	uint32_t total = record_bytes(src);

	memcpy(dst, src, total);
	put_be64(dst + 36, lsn);
	put_be32(dst + total - D1_JOURNAL_TRAILER_BYTES,
		 d1_crc32c(dst, total - D1_JOURNAL_TRAILER_BYTES));
	return total;
}

/*
 * A log that repeats, skips or splices a member of one operation key.
 *
 * None of these can come from the live writer: it records member j
 * before it reaches member j+1, stops at the first member it could not
 * record, and answers an exact repeat from the receipt rather than
 * logging it again.  Each case is built by reframing real records --
 * LSN re-stamped, CRC recomputed -- so what is under test is the
 * semantic rule and not the checksum.
 */
static void test_replay_refuses_a_spliced_key(void)
{
	static const char *const what[] = {
		"a member recorded twice is refused",
		"a member whose predecessor was never recorded is refused",
		"a member of another body under the same key is refused",
	};
	unsigned int pass;

	for (pass = 0; pass < 3; pass++) {
		struct d1_uuid store_uuid;
		struct d1_store *live, *other, *target;
		struct d1_envelope env, changed;
		struct d1_result res;
		static uint8_t copy[65536];
		static uint8_t mine[16];
		static uint8_t theirs[16];
		const uint8_t *log, *log_b = NULL;
		size_t len, len_b = 0, at[16], at_b[16], used = 0;
		unsigned int records, records_b = 0, i;
		const uint32_t rights = D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER;
		d1_admission_id admission;
		d1_admission_id second;

		memset(mine, 0x64, sizeof(mine));
		memset(theirs, 0x65, sizeof(theirs));
		fill_uuid(&store_uuid, (uint8_t)(0x66 + pass));
		live = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
		if (!live)
			return;
		d1_store_journal_enable(live);
		admission = d1_fixture_admit(live, &object, 11,
					     D1_RIGHT_WRITE |
						     D1_RIGHT_SINGLE_WRITER);
		env_init(&env, live, admission, D1_OP_WRITE_BATCH);
		env.body.write.count = 2;
		env.body.write.stability = D1_FILE_SYNC;
		write_entry(&env.body.write.entries[0], 0, 11, 1, mine,
			    sizeof(mine), true,
			    &(struct d1_guard){ .never_written = true });
		/*
		 * The skipped-prefix case needs a member 1 whose answer does
		 * not depend on member 0 having run, or the record is caught
		 * by the ordinary result comparison instead of by the rule
		 * under test.  A guard refusal is such an answer: it carries
		 * the chunk's own guard and no allocated identifier.
		 */
		write_entry(&env.body.write.entries[1], 1, 11, 2, mine,
			    sizeof(mine), true,
			    &(struct d1_guard){ .never_written = pass != 1 });
		check(d1_store_apply(live, &env, &res) == D1_OK &&
			      res.entries[1].status ==
				      (pass == 1 ? D1_GUARDED : D1_OK),
		      "a two-member batch under one key");
		log = journal_of(live, &len);
		records = index_log(log, len, at, 16);
		check(records == 4, "logs a START, a control and two entries");

		/*
		 * The splice needs the same two members written by a second
		 * store from the same starting point, so its member 1 is a
		 * record whose own history is identical up to that point.
		 */
		if (pass == 2) {
			other = d1_store_open(&store_uuid, CHUNK_BYTES,
					      MAX_FILE_BYTES);
			if (!other) {
				d1_store_free(live);
				return;
			}
			d1_store_journal_enable(other);
			second = d1_fixture_admit(other, &object, 11, rights);
			/*
			 * The same number, and not the same handle: two
			 * live stores of one name count alike and are two
			 * stores.  The splice needs the number to match,
			 * because that is what the record carries.
			 */
			check(d1_admission_raw(second) ==
				      d1_admission_raw(admission),
			      "the second store counts to the same number");
			check(!d1_admission_eq(second, admission),
			      "and it is not the same handle");
			changed = env;
			changed.admission = second;
			changed.incarnation = d1_store_incarnation(other);
			write_entry(&changed.body.write.entries[1], 1, 11, 2,
				    theirs, sizeof(theirs), true,
				    &(struct d1_guard){ .never_written =
								true });
			check(d1_store_apply(other, &changed, &res) == D1_OK &&
				      res.entries[1].status == D1_OK,
			      "and records the changed body under that key");
			log_b = journal_of(other, &len_b);
			records_b = index_log(log_b, len_b, at_b, 16);
		} else {
			other = NULL;
		}
		check(len <= sizeof(copy), "the log fits the fixture buffer");
		if (len > sizeof(copy) || records != 4 ||
		    (pass == 2 && records_b != 4)) {
			d1_store_free(other);
			d1_store_free(live);
			return;
		}

		if (pass == 0) {
			/* Everything, then member 1 a second time. */
			memcpy(copy, log, len);
			used = len;
			used += restamp_record(copy + used, log + at[3], 5);
		} else if (pass == 1) {
			/* Member 1, with member 0 never recorded. */
			for (i = 0; i < 2; i++)
				used += restamp_record(copy + used, log + at[i],
						       i + 1);
			used += restamp_record(copy + used, log + at[3], 3);
		} else {
			/* Member 0 from one body, member 1 from the other. */
			for (i = 0; i < 3; i++)
				used += restamp_record(copy + used, log + at[i],
						       i + 1);
			used += restamp_record(copy + used, log_b + at_b[3], 4);
		}

		target =
			d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
		if (target) {
			check(d1_store_replay(target, copy, used) == D1_INVALID,
			      what[pass]);
			d1_store_free(target);
		}
		target =
			d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
		if (target) {
			check(d1_store_replay(target, log, len) == D1_OK,
			      "while the log as written still rebuilds");
			check(states_agree(live, target),
			      "into the same store");
			d1_store_free(target);
		}
		d1_store_free(other);
		d1_store_free(live);
	}
}

/*
 * The caller binding is settled before the operation key is.
 *
 * Memo section 8 orders the two: validate the caller binding, look the
 * key up, compare the digest.  A handle that is not bound to the object
 * it names has no business being told whether someone else's key is in
 * use, and the answer it gets must be its own failure rather than a
 * conflict with a record it could never have written.
 */
static void test_caller_binding_is_settled_first(void)
{
	struct d1_uuid store_uuid;
	struct d1_objkey elsewhere;
	struct d1_store *s;
	struct d1_envelope env, stranger;
	struct d1_result res;
	static uint8_t data[16];
	d1_admission_id mine;
	d1_admission_id theirs;
	d1_version_id seen;

	memset(data, 0x71, sizeof(data));
	fill_uuid(&store_uuid, 0x72);
	elsewhere.export_uuid = object.export_uuid;
	fill_uuid(&elsewhere.object_uuid, 0x73);
	s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!s)
		return;
	d1_store_journal_enable(s);
	mine = d1_fixture_admit(s, &object, 11,
				D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	theirs = d1_fixture_admit(s, &elsewhere, 12,
				  D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	check(d1_admission_live(mine) && d1_admission_live(theirs),
	      "two handles, on two objects");

	env_init(&env, s, mine, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 0, 11, 1, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	check(d1_store_apply(s, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_OK,
	      "the bound handle records a member under its key");

	/* The same key, from a handle bound to a different object. */
	stranger = env;
	stranger.admission = theirs;
	check(d1_store_apply(s, &stranger, &res) == D1_OK,
	      "a stranger submits the same key");
	check(res.entries[0].status == D1_STALE_AUTH,
	      "and is answered for its own binding, not for the key");
	check(res.entries[0].disposition == D1_UNRECORDED,
	      "with nothing recorded for it");

	/* And the key's owner is undisturbed. */
	check(d1_store_apply(s, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_OK,
	      "the original request still answers from its receipt");
	check(!d1_store_visible(s, &object, 0, &seen),
	      "and nothing was published either way");
	d1_store_free(s);
}

/* Big-endian stores, the framing section 9 fixes. */
static void put_be16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

/* Frame one record around a body the fixture built itself. */
static size_t frame_record(uint8_t *dst, uint32_t type,
			   const struct d1_uuid *uuid, uint64_t lsn,
			   uint64_t incarnation, const uint8_t *body,
			   uint32_t blen)
{
	uint32_t total =
		D1_JOURNAL_HEADER_BYTES + blen + D1_JOURNAL_TRAILER_BYTES;

	memset(dst, 0, total);
	put_be32(dst + 0, D1_JOURNAL_MAGIC);
	put_be16(dst + 4, (uint16_t)D1_JOURNAL_FORMAT);
	put_be16(dst + 6, (uint16_t)type);
	put_be32(dst + 8, D1_JOURNAL_HEADER_BYTES);
	put_be32(dst + 12, total);
	put_be32(dst + 16, ~total);
	memcpy(dst + 20, uuid->bytes, D1_UUID_BYTES);
	put_be64(dst + 36, lsn);
	put_be64(dst + 44, incarnation);
	memcpy(dst + D1_JOURNAL_HEADER_BYTES, body, blen);
	put_be32(dst + total - D1_JOURNAL_TRAILER_BYTES,
		 d1_crc32c(dst, total - D1_JOURNAL_TRAILER_BYTES));
	return total;
}

/* The pieces of an ENTRY record's body. */
struct entry_parts {
	const uint8_t *env;
	uint32_t env_len;
	uint32_t ordinal;
	const uint8_t *digest;
	const uint8_t *result;
	uint32_t result_len;
};

static void split_entry(struct entry_parts *p, const uint8_t *record)
{
	const uint8_t *b = record + D1_JOURNAL_HEADER_BYTES;

	p->env_len = get_be32(b);
	p->env = b + 4;
	p->ordinal = get_be32(p->env + p->env_len);
	p->digest = p->env + p->env_len + 4;
	p->result_len = get_be32(p->digest + D1_DIGEST_BYTES);
	p->result = p->digest + D1_DIGEST_BYTES + 4;
}

/* body := bytes(envelope) u32(ordinal) raw(digest) bytes(result) */
static uint32_t entry_body(uint8_t *out, const uint8_t *env_bytes,
			   uint32_t env_len, uint32_t ordinal,
			   const uint8_t *digest, const uint8_t *res_bytes,
			   uint32_t res_len)
{
	uint32_t at = 0;

	put_be32(out + at, env_len);
	at += 4;
	memcpy(out + at, env_bytes, env_len);
	at += env_len;
	put_be32(out + at, ordinal);
	at += 4;
	memcpy(out + at, digest, D1_DIGEST_BYTES);
	at += D1_DIGEST_BYTES;
	put_be32(out + at, res_len);
	at += 4;
	memcpy(out + at, res_bytes, res_len);
	return at + res_len;
}

/* body := u32(kind) bytes(request) bytes(result) */
static uint32_t control_body(uint8_t *out, uint32_t kind, const uint8_t *req,
			     uint32_t req_len, const uint8_t *res_bytes,
			     uint32_t res_len)
{
	uint32_t at = 0;

	put_be32(out + at, kind);
	at += 4;
	put_be32(out + at, req_len);
	at += 4;
	memcpy(out + at, req, req_len);
	at += req_len;
	put_be32(out + at, res_len);
	at += 4;
	memcpy(out + at, res_bytes, res_len);
	return at + res_len;
}

/*
 * The result the reducer computes for a member it refuses before the
 * handler touches anything: the epoch, EOF and verifier the store is
 * already at, and the refusal.  The epoch and EOF are taken from the
 * record before it rather than assumed.
 */
static void refusal_result(struct d1_complete_result *r,
			   const struct d1_complete_result *base,
			   const struct d1_opkey *key, uint32_t status)
{
	memset(r, 0, sizeof(*r));
	r->key = *key;
	r->index_epoch = base->index_epoch;
	r->eof = base->eof;
	r->disposition = D1_COMPLETED;
	r->entry.status = status;
	r->entry.stability = D1_FILE_SYNC;
	r->entry.disposition = D1_COMPLETED;
	memcpy(r->entry.verifier, base->entry.verifier, D1_VERIFIER_BYTES);
}

/*
 * A record's category has to agree with what it carries.
 *
 * An ENTRY is one member of an ordinary batch: a control operation in
 * one, or an ordinal past the body's own member count, names something
 * the live encoder cannot produce.  So does a CONTROL record carrying
 * an ordinary operation.  Each of these is built with the logged result
 * the reducer would actually compute for it, so that what refuses the
 * record is the category check and not a later disagreement.
 */
static void test_records_must_name_what_they_carry(void)
{
	static const char *const what[] = {
		"an ENTRY carrying a control operation is refused",
		"an ENTRY ordinal past its body's members is refused",
		"a CONTROL record carrying an ordinary operation is refused",
	};
	unsigned int pass;

	for (pass = 0; pass < 3; pass++) {
		struct d1_uuid store_uuid;
		struct d1_store *live, *target;
		struct d1_envelope env, crafted;
		struct d1_complete_result base, made;
		struct entry_parts parts;
		struct d1_result res;
		static uint8_t copy[65536];
		static uint8_t scratch[65536];
		static uint8_t body[8192];
		static uint8_t bytes[4096];
		static uint8_t result[512];
		static uint8_t data[16];
		uint8_t digest[D1_DIGEST_BYTES];
		const uint8_t *log;
		size_t len, at[8], used, env_len, res_len;
		unsigned int records;
		uint32_t blen;
		d1_admission_id admission;

		memset(data, 0x6a, sizeof(data));
		fill_uuid(&store_uuid, (uint8_t)(0x6b + pass));
		live = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
		if (!live)
			return;
		d1_store_journal_enable(live);
		admission = d1_fixture_admit(live, &object, 11,
					     D1_RIGHT_WRITE |
						     D1_RIGHT_SINGLE_WRITER);
		env_init(&env, live, admission, D1_OP_WRITE_BATCH);
		env.body.write.count = 1;
		env.body.write.stability = D1_FILE_SYNC;
		write_entry(&env.body.write.entries[0], 0, 11, 1, data,
			    sizeof(data), true,
			    &(struct d1_guard){ .never_written = true });
		check(d1_store_apply(live, &env, &res) == D1_OK &&
			      res.entries[0].status == D1_OK,
		      "a one-member batch to build on");
		log = journal_of(live, &len);
		records = index_log(log, len, at, 8);
		check(records == 3 && len <= sizeof(copy),
		      "logged as a START, a control and one entry");
		if (records != 3 || len > sizeof(copy)) {
			d1_store_free(live);
			return;
		}
		split_entry(&parts, log + at[2]);
		check(d1_complete_result_decode(parts.result, parts.result_len,
						&base),
		      "and its recorded result decodes");

		if (pass == 0) {
			/* A control operation inside an ENTRY. */
			env_init(&crafted, live, admission, D1_OP_LEASE_REAP);
			crafted.body.control.count = 1;
			crafted.body.control.txns[0] = res.entries[0].txn;
			crafted.body.control.old_admission = admission;
			/*
			 * The reducer would run it as an ordinary member:
			 * a reap needs CONTROL rights this handle has not
			 * got, which is a recorded refusal.
			 */
			refusal_result(&made, &base, &crafted.key,
				       D1_STALE_AUTH);
		} else if (pass == 1) {
			/* One member, and a record claiming a second. */
			crafted = env;
			/*
			 * Member 1 of a one-member body decodes as zero, and
			 * a zero-length payload is malformed.
			 */
			refusal_result(&made, &base, &crafted.key, D1_INVALID);
		} else {
			/* An ordinary operation inside a CONTROL record. */
			env_init(&crafted, live, admission, D1_OP_WRITE_BATCH);
			crafted.body.write.count = 1;
			crafted.body.write.stability = D1_FILE_SYNC;
			write_entry(&crafted.body.write.entries[0], 1, 11, 2,
				    data, sizeof(data), true,
				    &(struct d1_guard){ .never_written =
								true });
			refusal_result(&made, &base, &crafted.key,
				       D1_STALE_AUTH);
		}

		env_len = d1_envelope_encode(&crafted, bytes, sizeof(bytes));
		res_len = d1_complete_result_encode(&made, result,
						    sizeof(result));
		check(env_len && res_len, "the crafted record encodes");
		check(d1_envelope_digest(&crafted, scratch, sizeof(scratch),
					 digest),
		      "and carries the digest the store will recompute");
		if (!env_len || !res_len) {
			d1_store_free(live);
			return;
		}
		if (pass == 2)
			blen = control_body(body, D1_CTL_ENVELOPE, bytes,
					    (uint32_t)env_len, result,
					    (uint32_t)res_len);
		else
			blen = entry_body(body, bytes, (uint32_t)env_len,
					  pass == 1 ? 1u : 0u, digest, result,
					  (uint32_t)res_len);

		memcpy(copy, log, len);
		used = len;
		used += frame_record(copy + used,
				     pass == 2 ? D1_REC_CONTROL : D1_REC_ENTRY,
				     &store_uuid, 4, 1, body, blen);

		target =
			d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
		if (target) {
			check(d1_store_replay(target, copy, used) == D1_INVALID,
			      what[pass]);
			d1_store_free(target);
		}
		target =
			d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
		if (target) {
			check(d1_store_replay(target, log, len) == D1_OK,
			      "while the log as written still rebuilds");
			check(states_agree(live, target),
			      "into the same store");
			d1_store_free(target);
		}
		d1_store_free(live);
	}
}

/*
 * The result the reducer computes for a member it refuses before the
 * handler touches anything and before it has taken a receipt: the key,
 * the refusal, and the verifier the store is at.  Epoch and EOF stay
 * zero, because the memset that starts the reduction is all that ever
 * sets them on this path.
 */
static void crafted_result(struct d1_complete_result *r,
			   const struct d1_opkey *key, uint32_t status,
			   uint32_t disposition, const uint8_t *verifier)
{
	memset(r, 0, sizeof(*r));
	r->key = *key;
	r->disposition = disposition;
	r->entry.status = status;
	r->entry.stability = D1_FILE_SYNC;
	r->entry.disposition = disposition;
	memcpy(r->entry.verifier, verifier, D1_VERIFIER_BYTES);
}

/*
 * A durable record claims that something happened, and equal results do
 * not make that claim true.
 *
 * A member refused for its caller binding, a member whose key already
 * names a different Envelope, and an exact repeat of a control the
 * writer appended once all recompute to exactly the result they
 * returned live -- and the live writer appends none of them, because
 * none of them recorded anything.  Each record here carries the result
 * the reducer really computes, so what refuses it is the question of
 * whether reducing it created the receipt it names, and not a
 * disagreement about the answer.
 */
static void test_replay_requires_the_record_to_have_happened(void)
{
	static const char *const what[] = {
		"a duplicated control record is refused",
		"an ENTRY whose caller was never bound is refused",
		"an ENTRY refused for its key is refused",
	};
	unsigned int pass;

	for (pass = 0; pass < 3; pass++) {
		struct d1_uuid store_uuid;
		struct d1_store *live, *target;
		struct d1_envelope env, crafted;
		struct d1_complete_result made;
		struct d1_result res;
		static uint8_t copy[65536];
		static uint8_t scratch[65536];
		static uint8_t body[8192];
		static uint8_t bytes[4096];
		static uint8_t result[512];
		static uint8_t data[16];
		uint8_t digest[D1_DIGEST_BYTES];
		uint8_t verifier[D1_VERIFIER_BYTES];
		const uint8_t *log;
		size_t len, at[16], used, env_len, res_len;
		unsigned int records;
		uint32_t blen = 0, type = D1_REC_ENTRY, ordinal = 0;
		uint64_t lsn;
		d1_admission_id admission;
		d1_admission_id control;
		d1_txn_id txn;

		memset(data, 0x71, sizeof(data));
		fill_uuid(&store_uuid, (uint8_t)(0x71 + pass));
		live = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
		if (!live)
			return;
		d1_store_journal_enable(live);
		d1_store_verifier(live, verifier);
		admission = d1_fixture_admit(live, &object, 11,
					     D1_RIGHT_WRITE |
						     D1_RIGHT_SINGLE_WRITER);
		env_init(&env, live, admission, D1_OP_WRITE_BATCH);
		env.body.write.count = 1;
		env.body.write.stability = D1_FILE_SYNC;
		write_entry(&env.body.write.entries[0], 0, 11, 1, data,
			    sizeof(data), true,
			    &(struct d1_guard){ .never_written = true });
		check(d1_store_apply(live, &env, &res) == D1_OK &&
			      res.entries[0].status == D1_OK,
		      "a one-member batch to build on");
		txn = res.entries[0].txn;

		if (pass == 0) {
			/*
			 * A control operation the reducer records as a
			 * refusal: a live admission's work cannot be reaped.
			 * It is a recorded event, so it has a receipt and a
			 * record, which is what a duplicate needs.
			 */
			control = d1_fixture_admit(live, &object, 11,
						   D1_RIGHT_CONTROL);
			env_init(&env, live, control, D1_OP_LEASE_REAP);
			env.body.control.count = 1;
			env.body.control.txns[0] = txn;
			env.body.control.old_admission = admission;
			check(d1_store_apply(live, &env, &res) == D1_OK &&
				      res.entries[0].status == D1_STALE_AUTH,
			      "and one control record, recorded as a refusal");
		}

		log = journal_of(live, &len);
		records = index_log(log, len, at, 16);
		lsn = (uint64_t)records + 1u;
		check(len + 4096u <= sizeof(copy), "the log fits a copy");
		if (len + 4096u > sizeof(copy)) {
			d1_store_free(live);
			return;
		}

		if (pass == 0) {
			/* The last record again, under the next LSN. */
			const uint8_t *last = log + at[records - 1];

			blen = record_bytes(last) - D1_JOURNAL_HEADER_BYTES -
			       D1_JOURNAL_TRAILER_BYTES;
			memcpy(body, last + D1_JOURNAL_HEADER_BYTES, blen);
			type = D1_REC_CONTROL;
		} else {
			if (pass == 1) {
				/* A handle the store never issued. */
				env_init(&crafted, live,
					 d1_fixture_admission_handle(
						 live, admission.raw + 7u),
					 D1_OP_WRITE_BATCH);
				crafted.body.write.count = 1;
				crafted.body.write.stability = D1_FILE_SYNC;
				write_entry(&crafted.body.write.entries[0], 1,
					    11, 2, data, sizeof(data), true,
					    &(struct d1_guard){ .never_written =
									true });
				crafted_result(&made, &crafted.key,
					       D1_STALE_AUTH, D1_UNRECORDED,
					       verifier);
				ordinal = 0;
			} else {
				/*
				 * The recorded key, a changed body, and the
				 * next ordinal the dense prefix allows.
				 */
				crafted = env;
				crafted.body.write.count = 2;
				write_entry(&crafted.body.write.entries[1], 1,
					    11, 2, data, sizeof(data), true,
					    &(struct d1_guard){ .never_written =
									true });
				crafted_result(&made, &crafted.key,
					       D1_REPLAY_CONFLICT, D1_COMPLETED,
					       verifier);
				ordinal = 1;
			}
			env_len = d1_envelope_encode(&crafted, bytes,
						     sizeof(bytes));
			res_len = d1_complete_result_encode(&made, result,
							    sizeof(result));
			check(env_len && res_len, "the crafted record encodes");
			check(d1_envelope_digest(&crafted, scratch,
						 sizeof(scratch), digest),
			      "and carries the digest the store recomputes");
			if (!env_len || !res_len) {
				d1_store_free(live);
				return;
			}
			blen = entry_body(body, bytes, (uint32_t)env_len,
					  ordinal, digest, result,
					  (uint32_t)res_len);
			type = D1_REC_ENTRY;
		}

		memcpy(copy, log, len);
		used = len + frame_record(copy + len, type, &store_uuid, lsn, 1,
					  body, blen);

		target =
			d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
		if (target) {
			check(d1_store_replay(target, copy, used) == D1_INVALID,
			      what[pass]);
			d1_store_free(target);
		}
		target =
			d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
		if (target) {
			check(d1_store_replay(target, log, len) == D1_OK,
			      "while the log as written still rebuilds");
			check(states_agree(live, target),
			      "into the same store");
			d1_store_free(target);
		}
		d1_store_free(live);
	}
}

/*
 * The same invariant at the frontier where the reducer runs out of
 * room.  A member that could not reserve a receipt recorded nothing and
 * consumed nothing, so a record of it is a record the writer never
 * appended -- and its result is an honest NOSPC, not a disagreement.
 */
static void test_replay_refuses_a_record_that_found_no_room(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *live, *target;
	struct d1_envelope env;
	struct d1_complete_result made;
	struct d1_result res;
	static uint8_t copy[1u << 19];
	static uint8_t scratch[65536];
	static uint8_t body[8192];
	static uint8_t bytes[4096];
	static uint8_t result[512];
	static uint8_t data[8];
	static size_t at[D1_MAX_RECEIPTS + 8u];
	uint8_t digest[D1_DIGEST_BYTES];
	uint8_t verifier[D1_VERIFIER_BYTES];
	const uint8_t *log;
	size_t len, used, env_len, res_len;
	unsigned int i, records;
	uint32_t blen;
	uint64_t lsn;
	bool filled = false;
	d1_admission_id admission;

	memset(data, 0x74, sizeof(data));
	fill_uuid(&store_uuid, 0x74);
	live = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!live)
		return;
	d1_store_journal_enable(live);
	d1_store_verifier(live, verifier);
	admission = d1_fixture_admit(live, &object, 11,
				     D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);

	/* Recorded refusals, all of them logged, until there is no room. */
	memset(&res, 0, sizeof(res));
	for (i = 0; i < D1_MAX_RECEIPTS + 2u; i++) {
		env_init(&env, live, admission, D1_OP_WRITE_BATCH);
		env.body.write.count = 1;
		env.body.write.stability = D1_FILE_SYNC;
		write_entry(&env.body.write.entries[0], 0, 11, i + 1u, data,
			    sizeof(data), true,
			    &(struct d1_guard){ .generation = 900u + i,
						.writer = 11 });
		d1_store_apply(live, &env, &res);
		if (res.entries[0].status == D1_NOSPC) {
			filled = true;
			break;
		}
		if (res.entries[0].status != D1_GUARDED)
			break;
	}
	check(filled && res.entries[0].disposition == D1_UNRECORDED,
	      "the receipt table fills and the next member is UNRECORDED");
	if (!filled) {
		d1_store_free(live);
		return;
	}

	/* @env is that member: it answered NOSPC and was never logged. */
	crafted_result(&made, &env.key, D1_NOSPC, D1_UNRECORDED, verifier);
	env_len = d1_envelope_encode(&env, bytes, sizeof(bytes));
	res_len = d1_complete_result_encode(&made, result, sizeof(result));
	check(env_len && res_len &&
		      d1_envelope_digest(&env, scratch, sizeof(scratch),
					 digest),
	      "the record it would have written encodes");
	log = journal_of(live, &len);
	records = index_log(log, len, at, D1_MAX_RECEIPTS + 8u);
	lsn = (uint64_t)records + 1u;
	check(len + 4096u <= sizeof(copy), "and the full log fits a copy");
	if (!env_len || !res_len || len + 4096u > sizeof(copy)) {
		d1_store_free(live);
		return;
	}
	blen = entry_body(body, bytes, (uint32_t)env_len, 0, digest, result,
			  (uint32_t)res_len);
	memcpy(copy, log, len);
	used = len + frame_record(copy + len, D1_REC_ENTRY, &store_uuid, lsn, 1,
				  body, blen);

	target = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (target) {
		check(d1_store_replay(target, copy, used) == D1_INVALID,
		      "a record of a member that found no room is refused");
		d1_store_free(target);
	}
	target = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (target) {
		check(d1_store_replay(target, log, len) == D1_OK,
		      "while the log as written still rebuilds");
		check(states_agree(live, target), "into the same store");
		d1_store_free(target);
	}
	d1_store_free(live);
}

/*
 * The other half of the control record's emission proof.
 *
 * A control answers once, so its receipt is member zero and the
 * question asked before the reducer refuses a duplicate.  That is not
 * the whole proof, and the matrix used to say it was.  A control the
 * live store answered UNRECORDED -- a handle not bound to the object,
 * say -- has no receipt before and none after, and appends nothing; a
 * record of it passes the before question untouched and is refused only
 * by the one asked after.  Both halves are live, and each refuses a
 * class the other does not.
 */
static void test_a_control_record_that_recorded_nothing(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *live, *target;
	struct d1_objkey elsewhere;
	struct d1_envelope env, crafted;
	struct d1_complete_result made;
	struct d1_result res;
	static uint8_t copy[65536];
	static uint8_t scratch[65536];
	static uint8_t body[8192];
	static uint8_t bytes[4096];
	static uint8_t result[512];
	static uint8_t data[16];
	uint8_t digest[D1_DIGEST_BYTES];
	uint8_t verifier[D1_VERIFIER_BYTES];
	const uint8_t *log;
	size_t len, before = 0, at[16], used, env_len, res_len;
	unsigned int records;
	uint32_t blen;
	d1_admission_id admission;
	d1_admission_id stranger;
	d1_txn_id txn;

	memset(data, 0x27, sizeof(data));
	fill_uuid(&store_uuid, 0x27);
	elsewhere.export_uuid = object.export_uuid;
	fill_uuid(&elsewhere.object_uuid, 0x28);
	live = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!live)
		return;
	d1_store_journal_enable(live);
	d1_store_verifier(live, verifier);
	admission = d1_fixture_admit(live, &object, 11,
				     D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	/* A control handle for a different object: admitted, not bound. */
	stranger = d1_fixture_admit(live, &elsewhere, 11, D1_RIGHT_CONTROL);

	env_init(&env, live, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 0, 11, 1, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	check(d1_store_apply(live, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_OK,
	      "a history with work in it");
	txn = res.entries[0].txn;

	/* The live control the writer appends nothing for. */
	(void)journal_of(live, &before);
	env_init(&crafted, live, stranger, D1_OP_LEASE_REAP);
	crafted.object = object;
	crafted.body.control.count = 1;
	crafted.body.control.txns[0] = txn;
	crafted.body.control.old_admission = admission;
	check(d1_store_apply(live, &crafted, &res) == D1_OK &&
		      res.entries[0].status == D1_STALE_AUTH &&
		      res.entries[0].disposition == D1_UNRECORDED,
	      "a control whose handle is bound elsewhere records nothing");
	log = journal_of(live, &len);
	check(len == before, "and the writer appended nothing for it");

	/* A record of it, carrying the result the reducer computes. */
	crafted_result(&made, &crafted.key, D1_STALE_AUTH, D1_UNRECORDED,
		       verifier);
	env_len = d1_envelope_encode(&crafted, bytes, sizeof(bytes));
	res_len = d1_complete_result_encode(&made, result, sizeof(result));
	check(env_len && res_len &&
		      d1_envelope_digest(&crafted, scratch, sizeof(scratch),
					 digest),
	      "the record it did not write encodes");
	records = index_log(log, len, at, 16);
	check(len + 4096u <= sizeof(copy), "and the log fits a copy");
	if (!env_len || !res_len || len + 4096u > sizeof(copy)) {
		d1_store_free(live);
		return;
	}
	blen = control_body(body, D1_CTL_ENVELOPE, bytes, (uint32_t)env_len,
			    result, (uint32_t)res_len);
	memcpy(copy, log, len);
	used = len + frame_record(copy + len, D1_REC_CONTROL, &store_uuid,
				  (uint64_t)records + 1u, 1, body, blen);

	target = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (target) {
		check(d1_store_replay(target, copy, used) == D1_INVALID,
		      "a control record that recorded nothing is refused");
		d1_store_free(target);
	}
	target = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (target) {
		check(d1_store_replay(target, log, len) == D1_OK,
		      "while the log as written still rebuilds");
		check(states_agree(live, target), "into the same store");
		d1_store_free(target);
	}
	d1_store_free(live);
}

/* An admission that arrives in the gap between two members. */
struct late_admit {
	struct d1_store *s;
	bool fired;
};

static void admit_the_handle(void *arg)
{
	struct late_admit *l = arg;

	l->fired = true;
	(void)d1_fixture_admit(l->s, &object, 11,
			       D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
}

/*
 * A batch stops at the first member it could not record, whatever the
 * reason it could not.
 *
 * Caller binding is asked again inside every member's lock interval, so
 * an admission installed between two members of one call makes member 0
 * STALE_AUTH and lets member 1 take a receipt at ordinal 1.  The
 * receipts under a key are supposed to be the dense prefix 0..n-1, and
 * replay refuses a log that is not -- so a call that carried on past
 * the hole would write a log its own store could never replay again,
 * for the rest of its life.  What ends the batch is the disposition,
 * not the two statuses that used to stand for it.
 */
static void test_a_batch_stops_at_its_first_unrecorded_member(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *live, *target;
	struct d1_envelope env;
	struct d1_result res;
	struct late_admit late;
	static uint8_t data[16];
	const uint8_t *log;
	size_t len, at[8];
	unsigned int records;
	d1_admission_id admission;

	memset(data, 0x75, sizeof(data));
	fill_uuid(&store_uuid, 0x75);
	live = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!live)
		return;
	check(d1_store_journal_enable(live) == D1_OK, "journalling is on");

	/*
	 * The batch names the handle the fixture is about to issue, which
	 * is what a client does when it learns a handle before the
	 * authority has installed it.
	 */
	memset(&late, 0, sizeof(late));
	late.s = live;
	env_init(&env, live, d1_fixture_admission_handle(live, 1),
		 D1_OP_WRITE_BATCH);
	env.body.write.count = 2;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 0, 11, 1, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	write_entry(&env.body.write.entries[1], 1, 11, 2, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	d1_fixture_before_member(live, 1, admit_the_handle, &late);

	check(d1_store_apply(live, &env, &res) == D1_OK, "the batch applies");
	check(res.entries[0].status == D1_STALE_AUTH &&
		      res.entries[0].disposition == D1_UNRECORDED,
	      "member 0 is refused for its binding and records nothing");
	check(res.entries[1].disposition == D1_UNRECORDED,
	      "and member 1 is unrecorded with it");
	check(!late.fired, "the batch never reached the gap before member 1");

	log = journal_of(live, &len);
	records = index_log(log, len, at, 8);
	check(records == 1, "so nothing but the START is in the log");

	target = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (target) {
		check(d1_store_replay(target, log, len) == D1_OK,
		      "which is a log this store can still replay");
		d1_store_free(target);
	}

	/* The handle arrives, and the exact request finishes. */
	d1_fixture_before_member(live, 1, NULL, NULL);
	admission = d1_fixture_admit(live, &object, 11,
				     D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	check(admission.raw == 1,
	      "the handle the request named is the one issued");
	check(d1_store_apply(live, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_OK &&
		      res.entries[1].status == D1_OK,
	      "and the exact retry completes both members");

	log = journal_of(live, &len);
	target = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (target) {
		check(d1_store_replay(target, log, len) == D1_OK,
		      "and the log it then wrote replays");
		check(states_agree(live, target), "into the same store");
		d1_store_free(target);
	}
	d1_store_free(live);
}

static uint16_t get_be16(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

/* The pieces of a CONTROL record's body. */
struct control_parts {
	uint32_t kind;
	const uint8_t *request;
	uint32_t request_len;
	const uint8_t *result;
	uint32_t result_len;
};

static void split_control(struct control_parts *p, const uint8_t *record)
{
	const uint8_t *b = record + D1_JOURNAL_HEADER_BYTES;

	p->kind = get_be32(b);
	p->request_len = get_be32(b + 4);
	p->request = b + 8;
	p->result_len = get_be32(p->request + p->request_len);
	p->result = p->request + p->request_len + 4;
}

/* Which record is the fixture control of @kind, or -1. */
static int find_control(const uint8_t *log, const size_t *at,
			unsigned int records, uint32_t kind)
{
	unsigned int i;

	for (i = 0; i < records; i++) {
		const uint8_t *r = log + at[i];

		if (get_be16(r + 6) == D1_REC_CONTROL &&
		    get_be32(r + D1_JOURNAL_HEADER_BYTES) == kind)
			return (int)i;
	}
	return -1;
}

/* A copy of @log with record @which replaced by @rec. */
static size_t splice_record(uint8_t *dst, const uint8_t *log, size_t len,
			    const size_t *at, unsigned int which,
			    const uint8_t *rec, size_t rec_len)
{
	size_t head = at[which];
	size_t tail = head + record_bytes(log + head);

	memcpy(dst, log, head);
	memcpy(dst + head, rec, rec_len);
	memcpy(dst + head + rec_len, log + tail, len - tail);
	return head + rec_len + (len - tail);
}

/*
 * A fixture control record is the whole request, not the one field the
 * replay dispatcher needs.
 *
 * The codec carries only the fields each kind uses, so a decode already
 * puts the canonical zero in the rest.  The object is the exception: it
 * is on the wire for every kind, and for three of them it is derived
 * rather than requested, so a record can carry one thing and mean
 * another.  Replay dispatched on the handle alone and would transition
 * it against an object the request does not name.  Every negative here
 * keeps the logged result exactly as written, so what refuses it is the
 * canonical form and not the result comparison.
 */
static void test_fixture_control_records_are_canonical(void)
{
	static const char *const what[] = {
		"a REVOKE naming an object its handle does not hold is refused",
		"a CUSTODY carrying an object its writer zeroes is refused",
		"a REVOKE of no handle at all carrying an object is refused",
	};
	static const uint32_t kinds[] = { D1_CTL_REVOKE, D1_CTL_CUSTODY,
					  D1_CTL_REVOKE };
	unsigned int pass;

	for (pass = 0; pass < 3; pass++) {
		struct d1_uuid store_uuid;
		struct d1_store *live, *target;
		struct d1_control_request request;
		struct control_parts parts;
		struct d1_envelope env;
		struct d1_result res;
		static uint8_t copy[65536];
		static uint8_t record[4096];
		static uint8_t body[2048];
		static uint8_t bytes[1024];
		static uint8_t data[16];
		const uint8_t *log;
		size_t len, at[16], used, req_len;
		unsigned int records;
		uint32_t blen;
		int which;
		d1_admission_id admission;
		d1_version_id version;

		memset(data, 0x78, sizeof(data));
		fill_uuid(&store_uuid, (uint8_t)(0x78 + pass));
		live = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
		if (!live)
			return;
		d1_store_journal_enable(live);
		admission = d1_fixture_admit(live, &object, 11,
					     D1_RIGHT_WRITE |
						     D1_RIGHT_SINGLE_WRITER);
		env_init(&env, live, admission, D1_OP_WRITE_BATCH);
		env.body.write.count = 1;
		env.body.write.stability = D1_FILE_SYNC;
		write_entry(&env.body.write.entries[0], 0, 11, 1, data,
			    sizeof(data), true,
			    &(struct d1_guard){ .never_written = true });
		check(d1_store_apply(live, &env, &res) == D1_OK &&
			      res.entries[0].status == D1_OK,
		      "a version for custody to name");
		version = res.entries[0].version;
		if (pass == 2) {
			/*
			 * A revoke of a handle the store never issued is
			 * logged too, with the zero object the writer had
			 * nothing to fill it from.
			 */
			d1_fixture_revoke(live,
					  d1_fixture_admission_handle(
						  live, admission.raw + 50u));
		} else {
			check(d1_custody_live(
				      d1_fixture_custody(live, version)),
			      "custody is issued and logged");
			d1_fixture_revoke(live, admission);
		}

		log = journal_of(live, &len);
		records = index_log(log, len, at, 16);
		which = find_control(log, at, records, kinds[pass]);
		check(which >= 0, "the record it wrote is in the log");
		check(len + 4096u <= sizeof(copy), "and the log fits a copy");
		if (which < 0 || len + 4096u > sizeof(copy)) {
			d1_store_free(live);
			return;
		}
		split_control(&parts, log + at[which]);
		check(d1_control_request_decode(parts.request,
						parts.request_len, &request) &&
			      request.kind == kinds[pass],
		      "and its request decodes as the kind it claims");

		/* The record put back untouched is still the log as written. */
		blen = control_body(body, parts.kind, parts.request,
				    parts.request_len, parts.result,
				    parts.result_len);
		used = splice_record(
			copy, log, len, at, (unsigned int)which, record,
			frame_record(record, D1_REC_CONTROL, &store_uuid,
				     (uint64_t)which + 1u, 1, body, blen));
		target =
			d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
		if (target) {
			check(d1_store_replay(target, copy, used) == D1_OK,
			      "the record reframed unchanged still replays");
			check(states_agree(live, target),
			      "into the same store");
			d1_store_free(target);
		}

		/* And now one field the live writer would never have set. */
		if (pass == 0)
			request.object.object_uuid.bytes[0] ^= 0xffu;
		else
			request.object = object;
		req_len = d1_control_request_encode(&request, bytes,
						    sizeof(bytes));
		check(req_len != 0, "the altered request encodes");
		if (!req_len) {
			d1_store_free(live);
			return;
		}
		blen = control_body(body, parts.kind, bytes, (uint32_t)req_len,
				    parts.result, parts.result_len);
		used = splice_record(
			copy, log, len, at, (unsigned int)which, record,
			frame_record(record, D1_REC_CONTROL, &store_uuid,
				     (uint64_t)which + 1u, 1, body, blen));
		target =
			d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
		if (target) {
			check(d1_store_replay(target, copy, used) == D1_INVALID,
			      what[pass]);
			d1_store_free(target);
		}
		d1_store_free(live);
	}
}

/*
 * A fixture control record has no receipt, so its emission rule is the
 * one its own writer follows.
 *
 * ADMIT and CUSTODY append nothing when the allocation came back zero,
 * so a record carrying their zero ID is a record no writer emits -- and
 * result equality cannot notice it, because the reducer refuses the same
 * request the same way and recomputes exactly the pair that was logged.
 * Both negatives below carry that exact pair.  Accepting one moved the
 * accepted frontier over an event that never happened, and the next
 * reopen adopted the invented LSN, so the reopen is checked too.
 */
static void test_fixture_records_carry_an_outcome_that_was_logged(void)
{
	static const char *const what[] = {
		"an ADMIT that allocated nothing is refused",
		"a CUSTODY that allocated nothing is refused",
	};
	unsigned int pass;

	for (pass = 0; pass < 2; pass++) {
		struct d1_uuid store_uuid;
		struct d1_store *live, *target;
		struct d1_control_request request;
		struct d1_control_result result;
		struct d1_envelope env;
		struct d1_result res;
		static uint8_t copy[65536];
		static uint8_t body[2048];
		static uint8_t bytes[1024];
		static uint8_t encoded[512];
		static uint8_t data[16];
		const uint8_t *log;
		size_t len, at[16], used, req_len, res_len;
		unsigned int i, records;
		uint32_t blen;
		size_t after = 0;
		d1_admission_id admission;

		memset(data, 0x7f, sizeof(data));
		fill_uuid(&store_uuid, (uint8_t)(0x7f + pass));
		live = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
		if (!live)
			return;
		d1_store_journal_enable(live);
		admission = d1_fixture_admit(live, &object, 11,
					     D1_RIGHT_WRITE |
						     D1_RIGHT_SINGLE_WRITER);
		env_init(&env, live, admission, D1_OP_WRITE_BATCH);
		env.body.write.count = 1;
		env.body.write.stability = D1_FILE_SYNC;
		write_entry(&env.body.write.entries[0], 0, 11, 1, data,
			    sizeof(data), true,
			    &(struct d1_guard){ .never_written = true });
		check(d1_store_apply(live, &env, &res) == D1_OK &&
			      res.entries[0].status == D1_OK,
		      "a history to append to");

		/* The live writer really does append nothing for these. */
		(void)journal_of(live, &len);
		memset(&request, 0, sizeof(request));
		if (pass == 0) {
			request.kind = D1_CTL_ADMIT;
			request.object = object;
			for (i = 0; i < D1_UUID_BYTES; i++) {
				request.auth.issuer.bytes[i] =
					(uint8_t)(0xf0u + i);
				request.auth.principal.bytes[i] = (uint8_t)i;
			}
			request.auth.writer = D1_WRITER_RESERVED_LOW;
			request.auth.rights = D1_RIGHT_WRITE;
			check(!d1_admission_live(d1_fixture_admit_full(
				      live, &object, &request.auth)),
			      "a reserved writer is never admitted");
		} else {
			request.kind = D1_CTL_CUSTODY;
			request.version =
				d1_fixture_version_handle(live, 999999u);
			check(!d1_custody_live(d1_fixture_custody(
				      live, request.version)),
			      "custody over no version is never issued");
		}
		log = journal_of(live, &after);
		check(after == len, "and the refusal was not journalled");

		memset(&result, 0, sizeof(result));
		result.status = D1_NOSPC;
		result.id = 0;
		req_len = d1_control_request_encode(&request, bytes,
						    sizeof(bytes));
		res_len = d1_control_result_encode(&result, encoded,
						   sizeof(encoded));
		records = index_log(log, after, at, 16);
		check(req_len && res_len && after + 4096u <= sizeof(copy),
		      "the record it did not write encodes");
		if (!req_len || !res_len || after + 4096u > sizeof(copy)) {
			d1_store_free(live);
			return;
		}
		blen = control_body(body, request.kind, bytes,
				    (uint32_t)req_len, encoded,
				    (uint32_t)res_len);
		memcpy(copy, log, after);
		used = after + frame_record(copy + after, D1_REC_CONTROL,
					    &store_uuid, (uint64_t)records + 1u,
					    1, body, blen);

		target =
			d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
		if (target) {
			check(d1_store_replay(target, copy, used) == D1_INVALID,
			      what[pass]);
			d1_store_free(target);
		}
		/* And a reopen does not adopt the frontier it invented. */
		target =
			d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
		if (target) {
			size_t adopted = 1;

			check(d1_store_reopen(target, copy, used) == D1_INVALID,
			      "and a reopen over it fails");
			(void)journal_of(target, &adopted);
			check(adopted == 0,
			      "with no journal of its own to carry the LSN");
			d1_store_free(target);
		}
		target =
			d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
		if (target) {
			check(d1_store_replay(target, log, after) == D1_OK,
			      "while the log as written still rebuilds");
			check(states_agree(live, target),
			      "into the same store");
			d1_store_free(target);
		}
		d1_store_free(live);
	}
}

/*
 * A typed ID of zero is the absent one.
 *
 * So an option that says it is present and carries zero is two
 * encodings of one request, and the rollback body has always refused
 * it.  The lifecycle body has the same option and did not, which let a
 * malformed request reach the reducer and be answered with a recorded
 * semantic refusal.
 *
 * The record leg is an oracle in its own right.  It carries the raw
 * digest of the bytes actually in it and the result the reducer really
 * computes for the body they decode to, so putting the check back
 * fails it as well as the four direct legs above it.
 */
static void test_lifecycle_options_are_canonical(void)
{
	struct d1_uuid store_uuid;
	struct d1_store *live, *target;
	struct d1_envelope env, decoded;
	struct d1_complete_result base, made;
	struct entry_parts parts;
	struct d1_result res;
	static uint8_t data[16];
	static uint8_t copy[65536];
	static uint8_t body[8192];
	static uint8_t bytes[4096];
	static uint8_t other[4096];
	static uint8_t result[512];
	uint8_t digest[D1_DIGEST_BYTES];
	uint8_t verifier[D1_VERIFIER_BYTES];
	const uint8_t *log;
	size_t len, at[16], used, env_len, other_len, res_len, i;
	size_t first = 0, last = 0;
	unsigned int records;
	uint32_t blen;
	d1_admission_id admission;
	d1_txn_id txn;

	memset(data, 0x7c, sizeof(data));
	fill_uuid(&store_uuid, 0x7c);
	live = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (!live)
		return;
	d1_store_journal_enable(live);
	d1_store_verifier(live, verifier);
	admission = d1_fixture_admit(live, &object, 11,
				     D1_RIGHT_WRITE | D1_RIGHT_SINGLE_WRITER);
	env_init(&env, live, admission, D1_OP_WRITE_BATCH);
	env.body.write.count = 1;
	env.body.write.stability = D1_FILE_SYNC;
	write_entry(&env.body.write.entries[0], 0, 11, 1, data, sizeof(data),
		    true, &(struct d1_guard){ .never_written = true });
	check(d1_store_apply(live, &env, &res) == D1_OK &&
		      res.entries[0].status == D1_OK,
	      "a prepared version to finalize");
	txn = res.entries[0].txn;

	env_init(&env, live, admission, D1_OP_FINALIZE_BATCH);
	env.body.lifecycle.range_begin = 0;
	env.body.lifecycle.range_end = 1;
	env.body.lifecycle.count = 1;
	env.body.lifecycle.entries[0].index = 0;
	env.body.lifecycle.entries[0].owner.cohort.raw = 1;
	env.body.lifecycle.entries[0].owner.writer = 11;
	env.body.lifecycle.entries[0].owner.co_id = 1;
	env.body.lifecycle.entries[0].txn = txn;
	env.body.lifecycle.entries[0].predecessor_present = true;
	env.body.lifecycle.entries[0].predecessor = d1_version_none();
	/*
	 * A prior verifier that does not match, so that if validation ever
	 * stopped refusing this body the reducer would answer a recorded
	 * STALE_AUTH -- a refusal decided before any handler touches the
	 * result, which is a result this test can state exactly.
	 */
	memcpy(env.body.lifecycle.prior_verifier, verifier, sizeof(verifier));
	env.body.lifecycle.prior_verifier[0] ^= 0xffu;
	check(!d1_envelope_validate(&env),
	      "a present predecessor of zero is not a canonical request");
	check(d1_store_apply(live, &env, &res) == D1_INVALID,
	      "and the call is refused before anything reads the body");
	check(d1_envelope_encode(&env, bytes, sizeof(bytes)) == 0,
	      "and the encoder will not write one");

	/*
	 * A key of its own from here on.  If validation ever stopped
	 * refusing this body, the call above would have recorded one
	 * under the key it was built with, and the crafted record would
	 * then be a repeat rather than the thing being tested.
	 */
	env.key.sequence = next_sequence++;

	/*
	 * Two canonical encodings that differ only in the ID say where the
	 * ID is, without this test knowing the layout.  Both values are
	 * small, so zeroing the bytes that differ zeroes the whole ID and
	 * leaves the present flag exactly where it was.
	 */
	env.body.lifecycle.entries[0].predecessor =
		d1_fixture_version_handle(live, 0x5a5au);
	env_len = d1_envelope_encode(&env, bytes, sizeof(bytes));
	env.body.lifecycle.entries[0].predecessor =
		d1_fixture_version_handle(live, 0xa5a5u);
	other_len = d1_envelope_encode(&env, other, sizeof(other));
	check(env_len != 0 && env_len == other_len,
	      "two canonical forms of it encode to one length");
	if (!env_len || env_len != other_len) {
		d1_store_free(live);
		return;
	}
	for (i = 0; i < env_len; i++) {
		if (bytes[i] == other[i])
			continue;
		if (!last)
			first = i;
		last = i;
	}
	check(last != 0 && last + 1u - first <= 8u,
	      "and differ in nothing but that ID");
	check(d1_envelope_decode(bytes, env_len, &decoded),
	      "the canonical encoding decodes");
	for (i = first; i <= last; i++)
		bytes[i] = 0;
	check(!d1_envelope_decode(bytes, env_len, &decoded),
	      "and the same bytes with the ID zeroed do not");

	/*
	 * And a record carrying them is refused before the reducer runs.
	 * The record has to be one the reducer would otherwise accept, or
	 * it would be refused for the wrong reason: its digest is taken
	 * over the bytes actually in it rather than over a sibling
	 * encoding, and its logged result is the one the reducer computes
	 * for the body those bytes decode to.
	 */
	log = journal_of(live, &len);
	records = index_log(log, len, at, 16);
	check(records >= 3 && len + 4096u <= sizeof(copy),
	      "the log holds the write, and fits a copy");
	if (records < 3 || len + 4096u > sizeof(copy)) {
		d1_store_free(live);
		return;
	}
	split_entry(&parts, log + at[records - 1]);
	check(d1_complete_result_decode(parts.result, parts.result_len, &base),
	      "and its recorded result decodes");
	d1_request_digest(bytes, env_len, digest);
	refusal_result(&made, &base, &env.key, D1_STALE_AUTH);
	res_len = d1_complete_result_encode(&made, result, sizeof(result));
	if (!res_len) {
		d1_store_free(live);
		return;
	}
	blen = entry_body(body, bytes, (uint32_t)env_len, 0, digest, result,
			  (uint32_t)res_len);
	memcpy(copy, log, len);
	used = len + frame_record(copy + len, D1_REC_ENTRY, &store_uuid,
				  (uint64_t)records + 1u, 1, body, blen);
	target = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (target) {
		check(d1_store_replay(target, copy, used) == D1_INVALID,
		      "a record carrying a present zero ID is refused");
		d1_store_free(target);
	}
	target = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
	if (target) {
		check(d1_store_replay(target, log, len) == D1_OK,
		      "while the log as written still rebuilds");
		check(states_agree(live, target), "into the same store");
		d1_store_free(target);
	}
	d1_store_free(live);
}

/*
 * The one failure that happens before the log has a frontier: the START
 * that opens it.
 *
 * The fault controls say they refuse the next append and the next
 * flush.  On a store that has never logged anything those are the
 * START's own, so arming one before journalling exists has to reach it.
 */
static void test_start_can_fail_to_become_durable(void)
{
	static const bool flush_case[] = { false, true };
	unsigned int pass;

	for (pass = 0; pass < 2; pass++) {
		struct d1_uuid store_uuid;
		struct d1_store *s, *rebuilt;
		const uint8_t *log;
		size_t len;
		static uint8_t data[16];
		d1_admission_id admission;
		d1_version_id seen;

		memset(data, 0x6e, sizeof(data));
		fill_uuid(&store_uuid, (uint8_t)(0x6f + pass));
		s = d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
		if (!s)
			return;
		if (flush_case[pass])
			d1_fixture_fail_next_flush(s);
		else
			d1_fixture_fail_next_append(s);
		if (flush_case[pass])
			check(d1_store_journal_enable(s) == D1_IO,
			      "a START that cannot be flushed fails");
		else
			check(d1_store_journal_enable(s) == D1_IO,
			      "a START that cannot be appended fails");
		(void)journal_of(s, &len);
		check(len == 0, "with nothing claimed durable");
		check(!d1_store_visible(s, &object, 0, &seen),
		      "and the store untouched");

		/* The arm is spent, so the store can still start logging. */
		check(d1_store_journal_enable(s) == D1_OK,
		      "and a second attempt opens the log");
		admission = d1_fixture_admit(s, &object, 11,
					     D1_RIGHT_WRITE |
						     D1_RIGHT_SINGLE_WRITER);
		check(d1_version_live(commit_chunk(
			      s, admission, 0, 1, data, sizeof(data),
			      &(struct d1_guard){ .never_written = true },
			      d1_version_none(), NULL)),
		      "which then records ordinary work");
		log = journal_of(s, &len);
		rebuilt =
			d1_store_open(&store_uuid, CHUNK_BYTES, MAX_FILE_BYTES);
		if (rebuilt) {
			check(d1_store_replay(rebuilt, log, len) == D1_OK,
			      "and the log it wrote rebuilds");
			check(states_agree(s, rebuilt), "into the same store");
			d1_store_free(rebuilt);
		}
		d1_store_free(s);
	}
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
	test_close_does_not_destroy_under_an_arriving_call();
	test_a_closed_store_answers_nothing();
	test_a_view_is_released_through_its_own_store();
	test_the_door_arm_belongs_to_its_store();
	test_an_owner_names_a_cohort();
	test_the_chunk_table_is_capacity();
	test_a_handle_names_its_own_store();
	test_a_handle_keeps_its_domain_through_a_copy();
	test_two_live_stores_of_one_name_are_two_stores();
	test_a_decoded_handle_names_no_store();
	test_a_handle_names_its_own_kind();
	test_replay_rebuilds_the_same_handles();
	test_geometry_is_asked_before_the_object_table();
	test_a_requests_shape_does_not_depend_on_room();
	test_a_payload_fits_its_chunk();
	test_the_empty_object_can_be_read();
	test_a_journal_snapshot_is_a_value();
	test_a_journal_snapshot_can_find_no_memory();
	test_snapshots_run_beside_appends_and_a_close();
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
	test_control_members_are_all_or_nothing();
	test_replay_requires_a_pristine_target();
	test_journal_enable_requires_a_pristine_store();
	test_initial_guard_is_zero_zero();
	test_writer_must_be_the_granted_one();
	test_owner_vector_is_validated_whole();
	test_record_tags_are_validated();
	test_operation_key_binds_the_whole_envelope();
	test_unused_key_is_not_bound();
	test_recovery_clears_fault_arms();
	test_concurrent_callers_cannot_splice_a_key();
	test_replay_refuses_a_spliced_key();
	test_records_must_name_what_they_carry();
	test_replay_requires_the_record_to_have_happened();
	test_replay_refuses_a_record_that_found_no_room();
	test_a_control_record_that_recorded_nothing();
	test_a_batch_stops_at_its_first_unrecorded_member();
	test_fixture_control_records_are_canonical();
	test_fixture_records_carry_an_outcome_that_was_logged();
	test_lifecycle_options_are_canonical();
	test_start_can_fail_to_become_durable();
	test_caller_binding_is_settled_first();
	test_control_envelopes_replay();
	test_custody_and_release_replay();
	test_replay_refuses_a_foreign_log();
	test_recovery_admit();
	test_recovery_admit_is_atomic();
	test_lease_reap();
	test_unsupported();

	journal_release_all();

	if (failures) {
		fprintf(stderr, "%u check(s) failed\n", failures);
		return 1;
	}
	printf("d1_store_test: all checks passed\n");
	return 0;
}
