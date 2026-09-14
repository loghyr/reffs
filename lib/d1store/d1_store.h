/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * D1 storage model: the store and its one entry point.
 *
 * Everything the model holds is behind one mutex, taken for a whole
 * ordinary entry -- its admission, its transition, its published state
 * and its receipt -- and released between entries of a batch, so the
 * next entry is revalidated against whatever the previous one left.  No
 * callback, no I/O and no external lookup happens under it.
 *
 * Declared bounds of this model, which are model limits and not protocol
 * ones: a store holds a few objects, each object a few chunks, and the
 * tables below are sized for the oracles.  Exhaustion refuses an
 * operation; it never silently discards replay protection.
 */

#ifndef REFFS_D1_STORE_H
#define REFFS_D1_STORE_H

#include "d1_envelope.h"

#define D1_MAX_OBJECTS 4u
#define D1_MAX_CHUNKS 64u
#define D1_MAX_TXNS 256u
#define D1_MAX_VERSIONS 256u
#define D1_MAX_RECEIPTS 256u
#define D1_MAX_ADMISSIONS 32u
#define D1_MAX_OWNERS 256u

struct d1_store;

/* What one entry, or one whole control operation, answered. */
struct d1_entry_result {
	uint32_t status;
	bool version_present;
	d1_id_t version;
	bool txn_present;
	d1_id_t txn;
	/* The guard as it stands after the entry, or as it stood on refusal. */
	struct d1_guard guard;
	struct d1_owner owner;
	/* Actual durability, which this model always answers FILE_SYNC. */
	uint32_t stability;
	bool activated;
	uint32_t phase;
	uint8_t verifier[D1_VERIFIER_BYTES];
	/* Per entry for an ordinary batch; the operation's for a control. */
	uint32_t disposition;
};

struct d1_result {
	struct d1_opkey key;
	uint64_t index_epoch;
	uint64_t eof;
	uint32_t disposition;
	uint32_t count;
	struct d1_entry_result entries[D1_BATCH_ENTRIES_MAX];
};

/*
 * Open a store.  @chunk_bytes and @max_file_bytes are the geometry every
 * object in it uses; a geometry outside the declared range is refused
 * and the store is not created.
 */
struct d1_store *d1_store_open(const struct d1_uuid *store_uuid,
			       uint32_t chunk_bytes, uint64_t max_file_bytes);
void d1_store_free(struct d1_store *s);

/* The current verifier, as a START publishes it. */
void d1_store_verifier(const struct d1_store *s,
		       uint8_t out[D1_VERIFIER_BYTES]);
uint64_t d1_store_incarnation(const struct d1_store *s);

/*
 * Fixture controls.  These are the harness, not wire operations and not
 * a control plane: an admitted data caller cannot reach them.
 */
d1_id_t d1_fixture_admit(struct d1_store *s, const struct d1_objkey *object,
			 uint32_t writer, uint32_t rights);
void d1_fixture_revoke(struct d1_store *s, d1_id_t admission);
/* Expire an admission's lease without revoking its identity. */
void d1_fixture_expire(struct d1_store *s, d1_id_t admission);

/*
 * Apply one envelope.  The returned status is the operation's own; per
 * entry results are in @out.  An operation this slice does not implement
 * answers D1_UNSUPPORTED and mutates nothing.
 */
uint32_t d1_store_apply(struct d1_store *s, const struct d1_envelope *env,
			struct d1_result *out);

/* What the model currently makes visible, for the oracles to ask. */
bool d1_store_visible(const struct d1_store *s, const struct d1_objkey *object,
		      uint64_t index, d1_id_t *version);
bool d1_store_guard(const struct d1_store *s, const struct d1_objkey *object,
		    uint64_t index, struct d1_guard *guard);
uint64_t d1_store_eof(const struct d1_store *s, const struct d1_objkey *object);

#endif /* REFFS_D1_STORE_H */
