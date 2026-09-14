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

/* The largest envelope the declared limits allow, with room to frame. */
#define D1_ENVELOPE_MAX (D1_BATCH_PAYLOAD_MAX + 65536u)

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

size_t d1_envelope_encode(const struct d1_envelope *env, void *buf, size_t cap)
{
	struct d1_cursor c;

	if (!d1_op_known(env->op))
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
	return d1_dec_finished(&c);
}

bool d1_envelope_digest(const struct d1_envelope *env,
			uint8_t out[D1_DIGEST_BYTES])
{
	static uint8_t scratch[D1_ENVELOPE_MAX];
	size_t len;

	len = d1_envelope_encode(env, scratch, sizeof(scratch));
	if (!len)
		return false;
	d1_request_digest(scratch, len, out);
	return true;
}
