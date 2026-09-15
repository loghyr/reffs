/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * D1 storage model: the canonical envelope.
 *
 * The field order below is the format.  It is frozen, it is covered by
 * golden fixtures, and the decoder accepts nothing else: a body decodes
 * only if its counts are within the declared range, its aggregate
 * payload within the declared limit, and the buffer ends exactly where
 * the envelope does.
 */

#include <string.h>

#include "d1_digest.h"
#include "d1_envelope.h"

static void d1_enc_opt_guard(struct d1_cursor *c, bool present,
			     const struct d1_guard *g)
{
	d1_enc_u8(c, present ? 1u : 0u);
	if (present)
		d1_enc_guard(c, g);
}

static bool d1_dec_opt_guard(struct d1_cursor *c, bool *present,
			     struct d1_guard *g)
{
	uint8_t tag;

	if (!d1_dec_u8(c, &tag))
		return false;
	if (tag > 1u) {
		c->bad = true;
		return false;
	}
	*present = tag == 1u;
	if (!*present) {
		memset(g, 0, sizeof(*g));
		return true;
	}
	return d1_dec_guard(c, g);
}

static void d1_enc_write(struct d1_cursor *c, const struct d1_write_batch *w)
{
	uint32_t i;

	d1_enc_u32(c, w->count);
	for (i = 0; i < w->count; i++) {
		const struct d1_write_entry *e = &w->entries[i];

		d1_enc_u64(c, e->index);
		d1_enc_owner(c, &e->owner);
		d1_enc_opt_guard(c, e->guard_check, &e->expected);
		d1_enc_bytes(c, e->payload, e->payload_len);
		d1_enc_checksum(c, &e->checksum);
	}
	d1_enc_u32(c, w->stability);
	d1_enc_bool(c, w->activate);
}

static bool d1_dec_write(struct d1_cursor *c, struct d1_write_batch *w)
{
	uint64_t aggregate = 0;
	uint32_t i;

	if (!d1_dec_u32(c, &w->count))
		return false;
	if (w->count < D1_BATCH_ENTRIES_MIN ||
	    w->count > D1_BATCH_ENTRIES_MAX) {
		c->bad = true;
		return false;
	}
	for (i = 0; i < w->count; i++) {
		struct d1_write_entry *e = &w->entries[i];

		if (!d1_dec_u64(c, &e->index) || !d1_dec_owner(c, &e->owner) ||
		    !d1_dec_opt_guard(c, &e->guard_check, &e->expected) ||
		    !d1_dec_bytes_ref(c, &e->payload, &e->payload_len,
				      D1_CHUNK_BYTES_MAX) ||
		    !d1_dec_checksum(c, &e->checksum))
			return false;
		/*
		 * Summed as it is decoded, so a batch past the declared
		 * aggregate is refused while it is being read rather than
		 * after it has been assembled.
		 */
		if (!d1_add_u64(aggregate, e->payload_len, &aggregate) ||
		    aggregate > D1_BATCH_PAYLOAD_MAX) {
			c->bad = true;
			return false;
		}
	}
	if (!d1_dec_u32(c, &w->stability) || !d1_dec_bool(c, &w->activate))
		return false;
	if (w->stability < D1_UNSTABLE || w->stability > D1_FILE_SYNC) {
		c->bad = true;
		return false;
	}
	return true;
}

static void d1_enc_lifecycle(struct d1_cursor *c,
			     const struct d1_lifecycle_batch *l)
{
	uint32_t i;

	d1_enc_u64(c, l->range_begin);
	d1_enc_u64(c, l->range_end);
	d1_enc_u32(c, l->count);
	for (i = 0; i < l->count; i++) {
		const struct d1_lifecycle_entry *e = &l->entries[i];

		d1_enc_u64(c, e->index);
		d1_enc_owner(c, &e->owner);
		d1_enc_u64(c, e->txn);
		d1_enc_opt_u64(c, e->predecessor_present, e->predecessor);
	}
	d1_enc_raw(c, l->prior_verifier, sizeof(l->prior_verifier));
}

static bool d1_dec_lifecycle(struct d1_cursor *c, struct d1_lifecycle_batch *l)
{
	uint32_t i;

	if (!d1_dec_u64(c, &l->range_begin) || !d1_dec_u64(c, &l->range_end) ||
	    !d1_dec_u32(c, &l->count))
		return false;
	if (l->count < D1_BATCH_ENTRIES_MIN ||
	    l->count > D1_BATCH_ENTRIES_MAX) {
		c->bad = true;
		return false;
	}
	for (i = 0; i < l->count; i++) {
		struct d1_lifecycle_entry *e = &l->entries[i];

		if (!d1_dec_u64(c, &e->index) || !d1_dec_owner(c, &e->owner) ||
		    !d1_dec_u64(c, &e->txn) ||
		    !d1_dec_opt_u64(c, &e->predecessor_present,
				    &e->predecessor))
			return false;
	}
	return d1_dec_raw(c, l->prior_verifier, sizeof(l->prior_verifier));
}

static void d1_enc_rollback(struct d1_cursor *c,
			    const struct d1_rollback_batch *r)
{
	uint32_t i;

	d1_enc_u64(c, r->range_begin);
	d1_enc_u64(c, r->range_end);
	d1_enc_u32(c, r->count);
	for (i = 0; i < r->count; i++) {
		const struct d1_rollback_entry *e = &r->entries[i];

		d1_enc_u64(c, e->index);
		d1_enc_owner(c, &e->owner);
		d1_enc_u64(c, e->txn);
		d1_enc_opt_u64(c, e->visible_present, e->visible);
		d1_enc_opt_u64(c, e->predecessor_present, e->predecessor);
		d1_enc_opt_u64(c, e->custody_present, e->custody);
	}
}

static bool d1_dec_rollback(struct d1_cursor *c, struct d1_rollback_batch *r)
{
	uint32_t i;

	if (!d1_dec_u64(c, &r->range_begin) || !d1_dec_u64(c, &r->range_end) ||
	    !d1_dec_u32(c, &r->count))
		return false;
	if (r->count < D1_BATCH_ENTRIES_MIN ||
	    r->count > D1_BATCH_ENTRIES_MAX) {
		c->bad = true;
		return false;
	}
	for (i = 0; i < r->count; i++) {
		struct d1_rollback_entry *e = &r->entries[i];

		if (!d1_dec_u64(c, &e->index) || !d1_dec_owner(c, &e->owner) ||
		    !d1_dec_u64(c, &e->txn) ||
		    !d1_dec_opt_u64(c, &e->visible_present, &e->visible) ||
		    !d1_dec_opt_u64(c, &e->predecessor_present,
				    &e->predecessor) ||
		    !d1_dec_opt_u64(c, &e->custody_present, &e->custody))
			return false;
	}
	return true;
}

static void d1_enc_control(struct d1_cursor *c,
			   const struct d1_control_batch *k)
{
	uint32_t i;

	d1_enc_u32(c, k->count);
	for (i = 0; i < k->count; i++)
		d1_enc_u64(c, k->txns[i]);
	d1_enc_u64(c, k->old_admission);
	d1_enc_opt_u64(c, k->new_admission_present, k->new_admission);
	d1_enc_opt_u64(c, k->read_epoch_present, k->read_epoch);
}

static bool d1_dec_control(struct d1_cursor *c, struct d1_control_batch *k)
{
	uint32_t i;

	if (!d1_dec_u32(c, &k->count))
		return false;
	if (k->count < D1_BATCH_ENTRIES_MIN ||
	    k->count > D1_BATCH_ENTRIES_MAX) {
		c->bad = true;
		return false;
	}
	for (i = 0; i < k->count; i++)
		if (!d1_dec_u64(c, &k->txns[i]))
			return false;
	return d1_dec_u64(c, &k->old_admission) &&
	       d1_dec_opt_u64(c, &k->new_admission_present,
			      &k->new_admission) &&
	       d1_dec_opt_u64(c, &k->read_epoch_present, &k->read_epoch);
}

static bool d1_count_ok(uint32_t count)
{
	return count >= D1_BATCH_ENTRIES_MIN && count <= D1_BATCH_ENTRIES_MAX;
}

static bool d1_stability_ok(uint32_t stability)
{
	return stability == D1_UNSTABLE || stability == D1_DATA_SYNC ||
	       stability == D1_FILE_SYNC;
}

static bool d1_checksum_shape_ok(const struct d1_checksum *s)
{
	if (s->alg != D1_CKSUM_CRC32 && s->alg != D1_CKSUM_CRC32C)
		return false;
	/* Both declared algorithms are four big-endian digest bytes. */
	return s->len == 4u && s->len <= sizeof(s->digest);
}

static bool d1_writer_ok(uint32_t writer)
{
	return writer != D1_WRITER_RESERVED_LOW &&
	       writer != D1_WRITER_RESERVED_HIGH;
}

static bool d1_validate_write(const struct d1_write_batch *w)
{
	uint64_t aggregate = 0;
	uint32_t i, j;

	if (!d1_count_ok(w->count) || !d1_stability_ok(w->stability))
		return false;
	for (i = 0; i < w->count; i++) {
		const struct d1_write_entry *e = &w->entries[i];

		if (!e->payload || e->payload_len < 1u ||
		    e->payload_len > D1_CHUNK_BYTES_MAX)
			return false;
		if (!d1_checksum_shape_ok(&e->checksum))
			return false;
		if (!d1_writer_ok(e->owner.writer))
			return false;
		if (!d1_add_u64(aggregate, e->payload_len, &aggregate) ||
		    aggregate > D1_BATCH_PAYLOAD_MAX)
			return false;
		/* One batch never names one chunk twice. */
		for (j = 0; j < i; j++)
			if (w->entries[j].index == e->index)
				return false;
	}
	return true;
}

static bool d1_validate_lifecycle(const struct d1_lifecycle_batch *l)
{
	uint32_t i, j;

	if (!d1_count_ok(l->count) || l->range_begin >= l->range_end)
		return false;
	for (i = 0; i < l->count; i++) {
		const struct d1_lifecycle_entry *e = &l->entries[i];

		if (e->index < l->range_begin || e->index >= l->range_end)
			return false;
		if (e->txn == 0)
			return false;
		if (!d1_writer_ok(e->owner.writer))
			return false;
		for (j = 0; j < i; j++)
			if (l->entries[j].index == e->index ||
			    l->entries[j].txn == e->txn)
				return false;
	}
	return true;
}

static bool d1_validate_rollback(const struct d1_rollback_batch *r)
{
	uint32_t i, j;

	if (!d1_count_ok(r->count) || r->range_begin >= r->range_end)
		return false;
	for (i = 0; i < r->count; i++) {
		const struct d1_rollback_entry *e = &r->entries[i];

		if (e->index < r->range_begin || e->index >= r->range_end)
			return false;
		if (e->txn == 0)
			return false;
		if (!d1_writer_ok(e->owner.writer))
			return false;
		if (e->visible_present && e->visible == 0)
			return false;
		if (e->predecessor_present && e->predecessor == 0)
			return false;
		if (e->custody_present && e->custody == 0)
			return false;
		for (j = 0; j < i; j++)
			if (r->entries[j].index == e->index ||
			    r->entries[j].txn == e->txn)
				return false;
	}
	return true;
}

/*
 * Both control operations name the exact transactions they act on.
 * recovery_admit additionally names the new admission and the read
 * epoch it grants; lease_reap names neither, and a request carrying a
 * field its operation has no use for is a different request.
 */
static bool d1_validate_control(uint32_t op, const struct d1_control_batch *k)
{
	uint32_t i, j;

	if (!d1_count_ok(k->count) || k->old_admission == 0)
		return false;
	for (i = 0; i < k->count; i++) {
		if (k->txns[i] == 0)
			return false;
		for (j = 0; j < i; j++)
			if (k->txns[j] == k->txns[i])
				return false;
	}
	if (op == D1_OP_RECOVERY_ADMIT) {
		if (!k->new_admission_present || k->new_admission == 0)
			return false;
		if (k->new_admission == k->old_admission)
			return false;
		if (!k->read_epoch_present)
			return false;
		return true;
	}
	return !k->new_admission_present && !k->read_epoch_present;
}

/* Whether this slice can express a body for @op at all. */
static bool d1_op_known(uint32_t op)
{
	switch (op) {
	case D1_OP_WRITE_BATCH:
	case D1_OP_FINALIZE_BATCH:
	case D1_OP_COMMIT_BATCH:
	case D1_OP_ROLLBACK_BATCH:
	case D1_OP_RECOVERY_ADMIT:
	case D1_OP_LEASE_REAP:
		return true;
	default:
		return false;
	}
}

bool d1_envelope_validate(const struct d1_envelope *env)
{
	if (!d1_op_known(env->op))
		return false;
	switch (env->op) {
	case D1_OP_WRITE_BATCH:
		return d1_validate_write(&env->body.write);
	case D1_OP_FINALIZE_BATCH:
	case D1_OP_COMMIT_BATCH:
		return d1_validate_lifecycle(&env->body.lifecycle);
	case D1_OP_ROLLBACK_BATCH:
		return d1_validate_rollback(&env->body.rollback);
	default:
		return d1_validate_control(env->op, &env->body.control);
	}
}

size_t d1_envelope_encode(const struct d1_envelope *env, void *buf, size_t cap)
{
	struct d1_cursor c;

	/*
	 * Nothing is read out of the typed request until its shape has
	 * been checked, so an over-long count or digest length cannot walk
	 * off the end of a fixed array on the way to being encoded.
	 */
	if (!d1_envelope_validate(env))
		return 0;
	d1_enc_init(&c, buf, cap);
	d1_enc_objkey(&c, &env->object);
	d1_enc_u64(&c, env->admission);
	d1_enc_u64(&c, env->incarnation);
	d1_enc_opkey(&c, &env->key);
	d1_enc_u32(&c, env->op);
	switch (env->op) {
	case D1_OP_WRITE_BATCH:
		d1_enc_write(&c, &env->body.write);
		break;
	case D1_OP_FINALIZE_BATCH:
	case D1_OP_COMMIT_BATCH:
		d1_enc_lifecycle(&c, &env->body.lifecycle);
		break;
	case D1_OP_ROLLBACK_BATCH:
		d1_enc_rollback(&c, &env->body.rollback);
		break;
	default:
		d1_enc_control(&c, &env->body.control);
		break;
	}
	if (!d1_cursor_ok(&c))
		return 0;
	return c.len;
}

bool d1_envelope_decode(const void *buf, size_t len, struct d1_envelope *env)
{
	struct d1_cursor c;
	bool ok;

	memset(env, 0, sizeof(*env));
	d1_dec_init(&c, buf, len);
	if (!d1_dec_objkey(&c, &env->object) ||
	    !d1_dec_u64(&c, &env->admission) ||
	    !d1_dec_u64(&c, &env->incarnation) ||
	    !d1_dec_opkey(&c, &env->key) || !d1_dec_u32(&c, &env->op))
		return false;
	if (!d1_op_known(env->op))
		return false;
	switch (env->op) {
	case D1_OP_WRITE_BATCH:
		ok = d1_dec_write(&c, &env->body.write);
		break;
	case D1_OP_FINALIZE_BATCH:
	case D1_OP_COMMIT_BATCH:
		ok = d1_dec_lifecycle(&c, &env->body.lifecycle);
		break;
	case D1_OP_ROLLBACK_BATCH:
		ok = d1_dec_rollback(&c, &env->body.rollback);
		break;
	default:
		ok = d1_dec_control(&c, &env->body.control);
		break;
	}
	if (!ok)
		return false;
	/* A trailing byte is a different request, not this one. */
	if (!d1_dec_finished(&c))
		return false;
	/*
	 * The decoder accepts exactly the envelopes the encoder can write:
	 * one test, applied on both sides, so there is no shape that
	 * survives one direction and not the other.
	 */
	return d1_envelope_validate(env);
}

bool d1_envelope_digest(const struct d1_envelope *env, void *scratch,
			size_t cap, uint8_t out[D1_DIGEST_BYTES])
{
	size_t len;

	/*
	 * The scratch is the caller's.  A buffer shared between stores is
	 * protected by no single store's lock, and two callers hashing at
	 * once would each record the other's request identity.
	 */
	len = d1_envelope_encode(env, scratch, cap);
	if (!len)
		return false;
	d1_request_digest(scratch, len, out);
	return true;
}
