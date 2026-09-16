/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * D1 storage model: the request envelope every mutation shares.
 *
 * An Envelope is the object key, the admission handle, the incarnation,
 * the operation key and one tagged body.  Its canonical encoding is what
 * the request digest covers, so every field that changes the meaning of
 * a request -- including the incarnation, the handles, the expected
 * predecessors, the checksums and the activation flag -- is inside it.
 * Two requests that differ anywhere differ in their digest.
 *
 * Bodies use counted vectors.  There are no parallel optional arrays
 * that could disagree about their own length.
 */

#ifndef REFFS_D1_ENVELOPE_H
#define REFFS_D1_ENVELOPE_H

#include "d1_codec.h"

/* One entry of a write batch. */
struct d1_write_entry {
	uint64_t index;
	struct d1_owner owner;
	/*
	 * The guard predicate this write accepts.  Absent is only legal for
	 * a single-writer admission; the store, not the decoder, decides
	 * that, because it is an authority question and not a form question.
	 */
	bool guard_check;
	struct d1_guard expected;
	const uint8_t *payload;
	uint32_t payload_len;
	struct d1_checksum checksum;
};

struct d1_write_batch {
	uint32_t count;
	struct d1_write_entry entries[D1_BATCH_ENTRIES_MAX];
	uint32_t stability;
	bool activate;
};

/* One entry of a finalize or commit batch. */
struct d1_lifecycle_entry {
	uint64_t index;
	struct d1_owner owner;
	d1_txn_id txn;
	bool predecessor_present;
	d1_version_id predecessor;
};

struct d1_lifecycle_batch {
	/* Half-open chunk-index range; never a byte range. */
	uint64_t range_begin;
	uint64_t range_end;
	uint32_t count;
	struct d1_lifecycle_entry entries[D1_BATCH_ENTRIES_MAX];
	uint8_t prior_verifier[D1_VERIFIER_BYTES];
};

/* One entry of a rollback batch. */
struct d1_rollback_entry {
	uint64_t index;
	struct d1_owner owner;
	d1_txn_id txn;
	bool visible_present;
	d1_version_id visible;
	bool predecessor_present;
	d1_version_id predecessor;
	/*
	 * Repair custody, for rolling back committed data.  It is what
	 * authorises that; ordinary owner custody cannot roll back a
	 * committed version, including a replacement.
	 */
	bool custody_present;
	d1_custody_id custody;
};

struct d1_rollback_batch {
	uint64_t range_begin;
	uint64_t range_end;
	uint32_t count;
	struct d1_rollback_entry entries[D1_BATCH_ENTRIES_MAX];
};

/* A control operation naming transactions of one admission. */
struct d1_control_batch {
	uint32_t count;
	d1_txn_id txns[D1_BATCH_ENTRIES_MAX];
	d1_admission_id old_admission;
	/* recovery_admit only; absent for lease_reap. */
	bool new_admission_present;
	d1_admission_id new_admission;
	bool read_epoch_present;
	uint64_t read_epoch;
};

struct d1_envelope {
	struct d1_objkey object;
	d1_admission_id admission;
	uint64_t incarnation;
	struct d1_opkey key;
	/* One of enum d1_op. */
	uint32_t op;
	union {
		struct d1_write_batch write;
		struct d1_lifecycle_batch lifecycle;
		struct d1_rollback_batch rollback;
		struct d1_control_batch control;
	} body;
};

/*
 * Encode @env canonically into @buf.  Returns the encoded length, or
 * zero if it did not fit or carried something the form cannot express.
 */
size_t d1_envelope_encode(const struct d1_envelope *env, void *buf, size_t cap);

/*
 * Decode a canonical envelope.  Payload pointers refer into @buf, which
 * must outlive @env.  Returns false for anything that is not exactly one
 * canonical encoding of one representable envelope, including a trailing
 * byte, an unknown operation tag, a count outside the declared range and
 * an aggregate payload past the declared limit.
 */
bool d1_envelope_decode(const void *buf, size_t len, struct d1_envelope *env);

/* SHA-256 of the domain bytes followed by the canonical encoding. */
/*
 * The largest envelope the declared limits allow, with room to frame.
 * A caller that needs to encode or digest one owns a buffer this size;
 * the model keeps no shared scratch, because a buffer shared between
 * stores is protected by no single store's lock.
 */
#define D1_ENVELOPE_MAX (D1_BATCH_PAYLOAD_MAX + 65536u)

/*
 * Whether this typed envelope is one the canonical form can express.
 * Counts, lengths, tags, options, aggregate bytes and duplicate members
 * are all checked here, before anything reads a payload or writes a
 * byte.  The decoder applies the same test, so the two agree on exactly
 * which envelopes exist.
 *
 * "The same test" is why every question here is asked of the canonical
 * value alone.  A handle also carries a runtime domain and issuer, and
 * a decoder reading bytes has neither: two members naming one number
 * are one member twice to the decoder, so they must be one member twice
 * here as well, or an envelope would encode and then fail to decode.
 * Whose handle a number is, is asked at the store's door and not here.
 */
bool d1_envelope_validate(const struct d1_envelope *env);

/*
 * How many members this envelope's body declares.  A control operation
 * answers once for the whole of it, so it declares one.
 */
uint32_t d1_envelope_member_count(const struct d1_envelope *env);

bool d1_envelope_digest(const struct d1_envelope *env, void *scratch,
			size_t cap, uint8_t out[D1_DIGEST_BYTES]);

#endif /* REFFS_D1_ENVELOPE_H */
