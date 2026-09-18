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

/*
 * One member of a repair, in every operation a repair passes through.
 *
 * Section 7 gives the repair one local cohort that may carry both
 * modes, tagged per member, and a vector captured at begin_repair that
 * every later call names again in the same order.  So one member shape
 * serves all of them and each operation says which of its options it
 * requires: begin_repair names the mode, the custody and the exact
 * state it captured; prepare_repair names the replacement it stages;
 * finalize, commit, abort, clear_error and unlock name the vector and
 * nothing else.  An option a call has no use for is a different
 * request, and the decoder refuses it.
 */
struct d1_repair_entry {
	uint64_t index;
	/* begin_repair only; one of enum d1_repair_mode. */
	uint32_t mode;
	struct d1_owner owner;
	/*
	 * The member's repair transaction: named by prepare_repair, which
	 * stages a replacement under it, and by finalize_repair and
	 * commit_repair, which move it.  begin_repair issues it, so it
	 * does not name one.
	 */
	bool txn_present;
	d1_txn_id txn;
	/* The repair custody over the successor this member repairs. */
	bool custody_present;
	d1_custody_id custody;
	/*
	 * begin_repair of a NOPRE member only: the postcondition a
	 * refused rollback of this successor left behind, which is what
	 * authorizes the repair.  Section 7 has NOPRE consume it; an
	 * ERROR member has an episode instead and carries none.
	 */
	bool postcond_present;
	d1_postcond_id postcond;
	/* The state the member captured and expects to find unchanged. */
	bool successor_present;
	d1_version_id successor;
	bool predecessor_present;
	d1_version_id predecessor;
	/* prepare_repair only: the replacement this member stages. */
	bool payload_present;
	const uint8_t *payload;
	uint32_t payload_len;
	struct d1_checksum checksum;
};

struct d1_repair_batch {
	uint64_t range_begin;
	uint64_t range_end;
	uint32_t count;
	struct d1_repair_entry entries[D1_BATCH_ENTRIES_MAX];
	/* Absent exactly for the two operations that open a repair. */
	bool cohort_present;
	d1_repair_id cohort;
	/*
	 * The ERROR episode this call is about: named by a begin_repair
	 * that carries an ERROR member, by clear_error, and by an unlock
	 * that names the episode instead of the cohort.  A cohort's ERROR
	 * members all belong to one episode, so one name serves the
	 * vector.
	 */
	bool episode_present;
	d1_episode_id episode;
	/* abort_repair only: the phase the caller believes it is in. */
	bool phase_present;
	uint32_t phase;
	/*
	 * finalize_repair and commit_repair only: the verifier the caller
	 * last saw.  Section 9's post-reboot check, which the ordinary
	 * lifecycle path has and the repair path did not.
	 */
	bool verifier_present;
	uint8_t prior_verifier[D1_VERIFIER_BYTES];
	/* clear_error only. */
	bool certificate_present;
	uint8_t certificate[D1_CERTIFICATE_BYTES];
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
		struct d1_repair_batch repair;
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
