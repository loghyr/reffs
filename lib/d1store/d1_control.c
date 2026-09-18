/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <string.h>

#include "d1_codec.h"
#include "d1_control.h"

static bool d1_kind_known(uint32_t kind)
{
	return kind >= D1_CTL_ENVELOPE && kind <= D1_CTL_CERTIFICATE;
}

size_t d1_control_request_encode(const struct d1_control_request *r, void *buf,
				 size_t cap)
{
	struct d1_cursor c;

	if (!d1_kind_known(r->kind) || r->kind == D1_CTL_ENVELOPE)
		return 0;
	d1_enc_init(&c, buf, cap);
	d1_enc_u32(&c, r->kind);
	d1_enc_objkey(&c, &r->object);
	switch (r->kind) {
	case D1_CTL_ADMIT:
		d1_enc_uuid(&c, &r->auth.issuer);
		d1_enc_uuid(&c, &r->auth.principal);
		d1_enc_raw(&c, r->auth.stateid, sizeof(r->auth.stateid));
		d1_enc_raw(&c, r->auth.session, sizeof(r->auth.session));
		d1_enc_u32(&c, r->auth.writer);
		d1_enc_u32(&c, r->auth.rights);
		d1_enc_u64(&c, r->auth.lease_epoch);
		d1_enc_u64(&c, r->auth.authority_epoch);
		d1_enc_u64(&c, r->auth.fence_sequence);
		break;
	case D1_CTL_REVOKE:
	case D1_CTL_EXPIRE:
		d1_enc_u64(&c, r->admission.raw);
		break;
	case D1_CTL_CERTIFICATE:
		d1_enc_u8(&c, r->certificate_present ? 1u : 0u);
		if (r->certificate_present)
			d1_enc_raw(&c, r->certificate, D1_CERTIFICATE_BYTES);
		break;
	default:
		d1_enc_u64(&c, r->version.raw);
		break;
	}
	if (!d1_cursor_ok(&c))
		return 0;
	return c.len;
}

bool d1_control_request_decode(const void *buf, size_t len,
			       struct d1_control_request *r)
{
	struct d1_cursor c;

	memset(r, 0, sizeof(*r));
	d1_dec_init(&c, buf, len);
	if (!d1_dec_u32(&c, &r->kind) || !d1_kind_known(r->kind) ||
	    r->kind == D1_CTL_ENVELOPE)
		return false;
	if (!d1_dec_objkey(&c, &r->object))
		return false;
	switch (r->kind) {
	case D1_CTL_ADMIT:
		if (!d1_dec_uuid(&c, &r->auth.issuer) ||
		    !d1_dec_uuid(&c, &r->auth.principal) ||
		    !d1_dec_raw(&c, r->auth.stateid, sizeof(r->auth.stateid)) ||
		    !d1_dec_raw(&c, r->auth.session, sizeof(r->auth.session)) ||
		    !d1_dec_u32(&c, &r->auth.writer) ||
		    !d1_dec_u32(&c, &r->auth.rights) ||
		    !d1_dec_u64(&c, &r->auth.lease_epoch) ||
		    !d1_dec_u64(&c, &r->auth.authority_epoch) ||
		    !d1_dec_u64(&c, &r->auth.fence_sequence))
			return false;
		break;
	case D1_CTL_REVOKE:
	case D1_CTL_EXPIRE:
		if (!d1_dec_u64(&c, &r->admission.raw))
			return false;
		break;
	case D1_CTL_CERTIFICATE: {
		uint8_t tag;

		if (!d1_dec_u8(&c, &tag) || tag > 1u) {
			c.bad = true;
			return false;
		}
		r->certificate_present = tag == 1u;
		if (r->certificate_present &&
		    !d1_dec_raw(&c, r->certificate, D1_CERTIFICATE_BYTES))
			return false;
		break;
	}
	default:
		if (!d1_dec_u64(&c, &r->version.raw))
			return false;
		break;
	}
	return d1_dec_finished(&c);
}

size_t d1_control_result_encode(const struct d1_control_result *r, void *buf,
				size_t cap)
{
	struct d1_cursor c;

	d1_enc_init(&c, buf, cap);
	d1_enc_u32(&c, r->status);
	d1_enc_u64(&c, r->id);
	if (!d1_cursor_ok(&c))
		return 0;
	return c.len;
}

bool d1_control_result_decode(const void *buf, size_t len,
			      struct d1_control_result *r)
{
	struct d1_cursor c;

	memset(r, 0, sizeof(*r));
	d1_dec_init(&c, buf, len);
	return d1_dec_u32(&c, &r->status) && d1_dec_u64(&c, &r->id) &&
	       d1_dec_finished(&c);
}

size_t d1_complete_result_encode(const struct d1_complete_result *r, void *buf,
				 size_t cap)
{
	const struct d1_entry_result *e = &r->entry;
	struct d1_cursor c;

	d1_enc_init(&c, buf, cap);
	d1_enc_opkey(&c, &r->key);
	d1_enc_u64(&c, r->index_epoch);
	d1_enc_u64(&c, r->eof);
	d1_enc_u32(&c, r->disposition);
	d1_enc_u32(&c, e->status);
	d1_enc_opt_u64(&c, e->version_present, e->version.raw);
	d1_enc_opt_u64(&c, e->txn_present, e->txn.raw);
	d1_enc_opt_u64(&c, e->cohort_present, e->cohort.raw);
	d1_enc_guard(&c, &e->guard);
	d1_enc_owner(&c, &e->owner);
	d1_enc_u32(&c, e->stability);
	d1_enc_bool(&c, e->activated);
	d1_enc_u32(&c, e->phase);
	d1_enc_raw(&c, e->verifier, sizeof(e->verifier));
	if (!d1_cursor_ok(&c))
		return 0;
	return c.len;
}

bool d1_complete_result_decode(const void *buf, size_t len,
			       struct d1_complete_result *r)
{
	struct d1_entry_result *e = &r->entry;
	struct d1_cursor c;

	memset(r, 0, sizeof(*r));
	d1_dec_init(&c, buf, len);
	if (!d1_dec_opkey(&c, &r->key) || !d1_dec_u64(&c, &r->index_epoch) ||
	    !d1_dec_u64(&c, &r->eof) || !d1_dec_u32(&c, &r->disposition) ||
	    !d1_dec_u32(&c, &e->status) ||
	    !d1_dec_opt_u64(&c, &e->version_present, &e->version.raw) ||
	    !d1_dec_opt_u64(&c, &e->txn_present, &e->txn.raw) ||
	    !d1_dec_opt_u64(&c, &e->cohort_present, &e->cohort.raw) ||
	    !d1_dec_guard(&c, &e->guard) || !d1_dec_owner(&c, &e->owner) ||
	    !d1_dec_u32(&c, &e->stability) || !d1_dec_bool(&c, &e->activated) ||
	    !d1_dec_u32(&c, &e->phase) ||
	    !d1_dec_raw(&c, e->verifier, sizeof(e->verifier)))
		return false;
	return d1_dec_finished(&c);
}

bool d1_complete_result_equal(const struct d1_complete_result *a,
			      const struct d1_complete_result *b)
{
	/*
	 * Compared field by field rather than with memcmp: both structs
	 * carry padding the canonical form never encodes, and comparing
	 * bytes nobody wrote is how a comparison agrees with one build and
	 * disagrees with another.
	 */
	if (memcmp(&a->key.origin, &b->key.origin, sizeof(a->key.origin)) != 0)
		return false;
	if (a->key.sequence != b->key.sequence ||
	    a->key.ordinal != b->key.ordinal)
		return false;
	if (a->index_epoch != b->index_epoch || a->eof != b->eof)
		return false;
	if (a->disposition != b->disposition)
		return false;
	if (a->entry.status != b->entry.status)
		return false;
	/*
	 * Handles are compared by the value the log carries.  Provenance
	 * is runtime state and is never encoded, so a decoded result has
	 * none and there is nothing there to compare: what the log records
	 * is which handle the reducer named, and that is the raw value.
	 */
	if (a->entry.version_present != b->entry.version_present ||
	    (a->entry.version_present &&
	     a->entry.version.raw != b->entry.version.raw))
		return false;
	if (a->entry.txn_present != b->entry.txn_present ||
	    (a->entry.txn_present && a->entry.txn.raw != b->entry.txn.raw))
		return false;
	if (a->entry.cohort_present != b->entry.cohort_present ||
	    (a->entry.cohort_present &&
	     a->entry.cohort.raw != b->entry.cohort.raw))
		return false;
	if (a->entry.guard.generation != b->entry.guard.generation ||
	    a->entry.guard.writer != b->entry.guard.writer ||
	    a->entry.guard.never_written != b->entry.guard.never_written)
		return false;
	if (a->entry.owner.cohort.raw != b->entry.owner.cohort.raw ||
	    a->entry.owner.writer != b->entry.owner.writer ||
	    a->entry.owner.co_id != b->entry.owner.co_id)
		return false;
	if (a->entry.stability != b->entry.stability ||
	    a->entry.activated != b->entry.activated ||
	    a->entry.phase != b->entry.phase)
		return false;
	return memcmp(a->entry.verifier, b->entry.verifier,
		      sizeof(a->entry.verifier)) == 0;
}
