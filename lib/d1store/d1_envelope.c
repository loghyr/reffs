/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

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
		d1_enc_u64(c, e->txn.raw);
		d1_enc_opt_u64(c, e->predecessor_present, e->predecessor.raw);
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
		    !d1_dec_u64(c, &e->txn.raw) ||
		    !d1_dec_opt_u64(c, &e->predecessor_present,
				    &e->predecessor.raw))
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
		d1_enc_u64(c, e->txn.raw);
		d1_enc_opt_u64(c, e->visible_present, e->visible.raw);
		d1_enc_opt_u64(c, e->predecessor_present, e->predecessor.raw);
		d1_enc_opt_u64(c, e->custody_present, e->custody.raw);
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
		    !d1_dec_u64(c, &e->txn.raw) ||
		    !d1_dec_opt_u64(c, &e->visible_present, &e->visible.raw) ||
		    !d1_dec_opt_u64(c, &e->predecessor_present,
				    &e->predecessor.raw) ||
		    !d1_dec_opt_u64(c, &e->custody_present, &e->custody.raw))
			return false;
	}
	return true;
}

static void d1_enc_repair(struct d1_cursor *c, const struct d1_repair_batch *r)
{
	uint32_t i;

	d1_enc_u64(c, r->range_begin);
	d1_enc_u64(c, r->range_end);
	d1_enc_u32(c, r->count);
	for (i = 0; i < r->count; i++) {
		const struct d1_repair_entry *e = &r->entries[i];

		d1_enc_u64(c, e->index);
		d1_enc_u32(c, e->mode);
		d1_enc_owner(c, &e->owner);
		d1_enc_opt_u64(c, e->custody_present, e->custody.raw);
		d1_enc_opt_u64(c, e->successor_present, e->successor.raw);
		d1_enc_opt_u64(c, e->predecessor_present, e->predecessor.raw);
		d1_enc_u8(c, e->payload_present ? 1u : 0u);
		if (e->payload_present) {
			d1_enc_bytes(c, e->payload, e->payload_len);
			d1_enc_checksum(c, &e->checksum);
		}
	}
	d1_enc_opt_u64(c, r->cohort_present, r->cohort.raw);
	d1_enc_u8(c, r->certificate_present ? 1u : 0u);
	if (r->certificate_present)
		d1_enc_raw(c, r->certificate, D1_CERTIFICATE_BYTES);
}

static bool d1_dec_repair(struct d1_cursor *c, struct d1_repair_batch *r)
{
	uint64_t aggregate = 0;
	uint8_t tag;
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
		struct d1_repair_entry *e = &r->entries[i];

		if (!d1_dec_u64(c, &e->index) || !d1_dec_u32(c, &e->mode) ||
		    !d1_dec_owner(c, &e->owner) ||
		    !d1_dec_opt_u64(c, &e->custody_present, &e->custody.raw) ||
		    !d1_dec_opt_u64(c, &e->successor_present,
				    &e->successor.raw) ||
		    !d1_dec_opt_u64(c, &e->predecessor_present,
				    &e->predecessor.raw) ||
		    !d1_dec_u8(c, &tag))
			return false;
		if (tag > 1u) {
			c->bad = true;
			return false;
		}
		e->payload_present = tag == 1u;
		if (!e->payload_present)
			continue;
		if (!d1_dec_bytes_ref(c, &e->payload, &e->payload_len,
				      D1_CHUNK_BYTES_MAX) ||
		    !d1_dec_checksum(c, &e->checksum))
			return false;
		/* Summed while it is read; see d1_dec_write. */
		if (!d1_add_u64(aggregate, e->payload_len, &aggregate) ||
		    aggregate > D1_BATCH_PAYLOAD_MAX) {
			c->bad = true;
			return false;
		}
	}
	if (!d1_dec_opt_u64(c, &r->cohort_present, &r->cohort.raw) ||
	    !d1_dec_u8(c, &tag))
		return false;
	if (tag > 1u) {
		c->bad = true;
		return false;
	}
	r->certificate_present = tag == 1u;
	if (!r->certificate_present)
		return true;
	return d1_dec_raw(c, r->certificate, D1_CERTIFICATE_BYTES);
}

static void d1_enc_control(struct d1_cursor *c,
			   const struct d1_control_batch *k)
{
	uint32_t i;

	d1_enc_u32(c, k->count);
	for (i = 0; i < k->count; i++)
		d1_enc_u64(c, k->txns[i].raw);
	d1_enc_u64(c, k->old_admission.raw);
	d1_enc_opt_u64(c, k->new_admission_present, k->new_admission.raw);
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
		if (!d1_dec_u64(c, &k->txns[i].raw))
			return false;
	return d1_dec_u64(c, &k->old_admission.raw) &&
	       d1_dec_opt_u64(c, &k->new_admission_present,
			      &k->new_admission.raw) &&
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

/*
 * The shape of an owner, wherever a request carries one.
 *
 * Section 3 makes zero the absent value of a typed ID, and the cohort
 * is one: an owner that names cohort zero names no cohort, so it is not
 * an owner at all and the request that carries it is malformed.  The
 * writer has its two reserved values, which are a different rule for a
 * different field.  The co_id is neither -- it is an opaque u32 the
 * model only ever compares -- so zero is an ordinary value there and is
 * not refused.
 *
 * Three request shapes carry an owner, and all three ask this, so a
 * shape cannot be repaired in one place and left malformed in another.
 */
static bool d1_owner_ok(const struct d1_owner *o)
{
	return d1_cohort_live(o->cohort) && d1_writer_ok(o->writer);
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
		if (!d1_owner_ok(&e->owner))
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
		if (!d1_txn_live(e->txn))
			return false;
		if (!d1_owner_ok(&e->owner))
			return false;
		/*
		 * A typed ID of zero is the absent one, so an option that
		 * says it is present and carries zero is not a canonical
		 * request -- the same rule a rollback entry's options are
		 * already held to, and the same reason: two encodings of
		 * "no predecessor" would be two requests that mean one
		 * thing, and one of them would reach the reducer.
		 */
		if (e->predecessor_present && !d1_version_live(e->predecessor))
			return false;
		for (j = 0; j < i; j++)
			if (l->entries[j].index == e->index ||
			    d1_txn_raw(l->entries[j].txn) == d1_txn_raw(e->txn))
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
		if (!d1_txn_live(e->txn))
			return false;
		if (!d1_owner_ok(&e->owner))
			return false;
		if (e->visible_present && !d1_version_live(e->visible))
			return false;
		if (e->predecessor_present && !d1_version_live(e->predecessor))
			return false;
		if (e->custody_present && !d1_custody_live(e->custody))
			return false;
		for (j = 0; j < i; j++)
			if (r->entries[j].index == e->index ||
			    d1_txn_raw(r->entries[j].txn) == d1_txn_raw(e->txn))
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

	if (!d1_count_ok(k->count) || !d1_admission_live(k->old_admission))
		return false;
	for (i = 0; i < k->count; i++) {
		if (!d1_txn_live(k->txns[i]))
			return false;
		for (j = 0; j < i; j++)
			if (d1_txn_raw(k->txns[j]) == d1_txn_raw(k->txns[i]))
				return false;
	}
	if (op == D1_OP_RECOVERY_ADMIT) {
		if (!k->new_admission_present ||
		    !d1_admission_live(k->new_admission))
			return false;
		if (d1_admission_raw(k->new_admission) ==
		    d1_admission_raw(k->old_admission))
			return false;
		if (!k->read_epoch_present)
			return false;
		return true;
	}
	return !k->new_admission_present && !k->read_epoch_present;
}

/*
 * Which options each repair operation requires, and which it refuses.
 *
 * The whole vector is one shape, so what separates the operations is
 * exactly this table.  An option a call has no use for is not ignored:
 * carrying it is a different request, it would be bound by the digest,
 * and two requests that mean one thing must not both exist.
 */
static bool d1_validate_repair(uint32_t op, const struct d1_repair_batch *r)
{
	bool wants_cohort = op != D1_OP_MARK_ERROR && op != D1_OP_BEGIN_REPAIR;
	bool wants_mode = op == D1_OP_BEGIN_REPAIR;
	bool wants_custody = op == D1_OP_MARK_ERROR || op == D1_OP_BEGIN_REPAIR;
	bool wants_state = op == D1_OP_MARK_ERROR || op == D1_OP_BEGIN_REPAIR;
	bool wants_payload = op == D1_OP_PREPARE_REPAIR;
	bool wants_certificate = op == D1_OP_CLEAR_ERROR;
	uint32_t i, j;

	if (!d1_count_ok(r->count) || r->range_begin >= r->range_end)
		return false;
	if (r->cohort_present != wants_cohort)
		return false;
	if (r->cohort_present && !d1_repair_live(r->cohort))
		return false;
	if (r->certificate_present != wants_certificate)
		return false;
	for (i = 0; i < r->count; i++) {
		const struct d1_repair_entry *e = &r->entries[i];

		if (e->index < r->range_begin || e->index >= r->range_end)
			return false;
		if (!d1_owner_ok(&e->owner))
			return false;
		if (wants_mode) {
			if (e->mode != D1_REPAIR_ERROR &&
			    e->mode != D1_REPAIR_NOPRE)
				return false;
		} else if (e->mode != 0u) {
			return false;
		}
		if (e->custody_present != wants_custody)
			return false;
		if (e->custody_present && !d1_custody_live(e->custody))
			return false;
		/*
		 * The captured state: the successor is what the member
		 * repairs and is required wherever the state is, and the
		 * predecessor is the option section 7 leaves genuinely
		 * optional -- its absence is what NOPRE is about.
		 */
		if (e->successor_present != wants_state)
			return false;
		if (e->successor_present && !d1_version_live(e->successor))
			return false;
		if (!wants_state && e->predecessor_present)
			return false;
		if (e->predecessor_present && !d1_version_live(e->predecessor))
			return false;
		if (e->payload_present != wants_payload)
			return false;
		if (e->payload_present && (!e->payload || e->payload_len < 1u ||
					   e->payload_len > D1_CHUNK_BYTES_MAX))
			return false;
		if (e->payload_present && !d1_checksum_shape_ok(&e->checksum))
			return false;
		/* One repair never names one chunk twice. */
		for (j = 0; j < i; j++)
			if (r->entries[j].index == e->index)
				return false;
	}
	return true;
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
	case D1_OP_MARK_ERROR:
	case D1_OP_BEGIN_REPAIR:
	case D1_OP_PREPARE_REPAIR:
	case D1_OP_FINALIZE_REPAIR:
	case D1_OP_COMMIT_REPAIR:
	case D1_OP_ABORT_REPAIR:
	case D1_OP_CLEAR_ERROR:
	case D1_OP_UNLOCK:
		return true;
	default:
		return false;
	}
}

/* Whether @op carries a repair vector rather than one of the others. */
static bool d1_op_repairs(uint32_t op)
{
	switch (op) {
	case D1_OP_MARK_ERROR:
	case D1_OP_BEGIN_REPAIR:
	case D1_OP_PREPARE_REPAIR:
	case D1_OP_FINALIZE_REPAIR:
	case D1_OP_COMMIT_REPAIR:
	case D1_OP_ABORT_REPAIR:
	case D1_OP_CLEAR_ERROR:
	case D1_OP_UNLOCK:
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
	case D1_OP_RECOVERY_ADMIT:
	case D1_OP_LEASE_REAP:
		return d1_validate_control(env->op, &env->body.control);
	default:
		return d1_validate_repair(env->op, &env->body.repair);
	}
}

uint32_t d1_envelope_member_count(const struct d1_envelope *env)
{
	switch (env->op) {
	case D1_OP_WRITE_BATCH:
		return env->body.write.count;
	case D1_OP_FINALIZE_BATCH:
	case D1_OP_COMMIT_BATCH:
		return env->body.lifecycle.count;
	case D1_OP_ROLLBACK_BATCH:
		return env->body.rollback.count;
	case D1_OP_RECOVERY_ADMIT:
	case D1_OP_LEASE_REAP:
		/* A control answers once, for the whole operation. */
		return 1u;
	default:
		/*
		 * A repair answers once for the whole cohort too: section 8
		 * gives it one cohort receipt rather than independently
		 * replayable member commits.
		 */
		return d1_op_repairs(env->op) ? 1u : 0u;
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
	d1_enc_u64(&c, env->admission.raw);
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
	case D1_OP_RECOVERY_ADMIT:
	case D1_OP_LEASE_REAP:
		d1_enc_control(&c, &env->body.control);
		break;
	default:
		d1_enc_repair(&c, &env->body.repair);
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
	    !d1_dec_u64(&c, &env->admission.raw) ||
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
	case D1_OP_RECOVERY_ADMIT:
	case D1_OP_LEASE_REAP:
		ok = d1_dec_control(&c, &env->body.control);
		break;
	default:
		ok = d1_dec_repair(&c, &env->body.repair);
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
