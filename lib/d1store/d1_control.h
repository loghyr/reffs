/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * D1 storage model: journalled control requests and canonical results.
 *
 * Section 9 requires a CONTROL record to carry a control request and
 * its result, and an ENTRY record to carry the Envelope, the entry
 * ordinal, the request digest and the complete result.  Recovery then
 * re-executes and compares what it computed against what was logged.
 *
 * That comparison is the whole point, so results have a canonical form
 * of their own.  A result that could not be encoded could not be
 * compared, and "same log, same store" would be an assertion rather
 * than a check.
 *
 * Fixture authority is journalled too, because the reducer's outcomes
 * depend on it: which handle is live, which custody binds which
 * version, which predecessor has been released.  Fault arms, read pins
 * and physical reclamation are NOT here -- they have no reducer-visible
 * effect and must not replay.
 */

#ifndef REFFS_D1_CONTROL_H
#define REFFS_D1_CONTROL_H

#include "d1_store.h"

/* Frozen numeric tags for the kinds of control a CONTROL record holds. */
enum d1_control_kind {
	/* An admitted caller's control operation, carried as an Envelope. */
	D1_CTL_ENVELOPE = 1,
	/* Fixture authority events, in the order section 2 names them. */
	D1_CTL_ADMIT = 2,
	D1_CTL_REVOKE = 3,
	D1_CTL_EXPIRE = 4,
	D1_CTL_CUSTODY = 5,
	D1_CTL_RELEASE = 6,
	/*
	 * The cross-DS completion certificate clear_error compares
	 * against.  It is fixture authority like the rest: what issues
	 * one is outside D1, the reducer's outcome depends on it, and a
	 * refusal decided on it has to be an answer replay can reach.
	 */
	D1_CTL_CERTIFICATE = 7,
};

/* One fixture control request.  The Envelope kind carries its own body. */
struct d1_control_request {
	uint32_t kind;
	struct d1_objkey object;
	/* D1_CTL_ADMIT */
	struct d1_fixture_authority auth;
	/* D1_CTL_REVOKE, D1_CTL_EXPIRE */
	d1_admission_id admission;
	/* D1_CTL_CUSTODY, D1_CTL_RELEASE */
	d1_version_id version;
	/* D1_CTL_CERTIFICATE */
	bool certificate_present;
	uint8_t certificate[D1_CERTIFICATE_BYTES];
};

/*
 * A fixture control's result.  IDs are derived deterministically from
 * event order, so the request does not carry the ID it will be given
 * and the result does; replay allocates again and compares.
 */
struct d1_control_result {
	uint32_t status;
	/*
	 * The raw form of whichever handle this kind issues or names --
	 * an admission for ADMIT, a custody for CUSTODY, a version for
	 * RELEASE.  It is a logged value that replay compares, not a
	 * handle anything resolves, so it carries no type and no issuer.
	 */
	uint64_t id;
};

size_t d1_control_request_encode(const struct d1_control_request *r, void *buf,
				 size_t cap);
bool d1_control_request_decode(const void *buf, size_t len,
			       struct d1_control_request *r);

size_t d1_control_result_encode(const struct d1_control_result *r, void *buf,
				size_t cap);
bool d1_control_result_decode(const void *buf, size_t len,
			      struct d1_control_result *r);

/*
 * One entry's complete result: the operation key it answers under, the
 * epoch and EOF it saw, its disposition and every returned field of
 * section 4.
 */
struct d1_complete_result {
	struct d1_opkey key;
	uint64_t index_epoch;
	uint64_t eof;
	uint32_t disposition;
	struct d1_entry_result entry;
};

size_t d1_complete_result_encode(const struct d1_complete_result *r, void *buf,
				 size_t cap);
bool d1_complete_result_decode(const void *buf, size_t len,
			       struct d1_complete_result *r);

/* Whether two complete results are the same result. */
bool d1_complete_result_equal(const struct d1_complete_result *a,
			      const struct d1_complete_result *b);

#endif /* REFFS_D1_CONTROL_H */
