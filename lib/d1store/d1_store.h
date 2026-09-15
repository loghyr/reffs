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
#define D1_MAX_CUSTODY 32u
#define D1_MAX_INTERVALS 64u
#define D1_MAX_VIEWS 32u

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
/*
 * One row of the fixture authority table, as section 2 names it.  The
 * bindings a call checks are exactly these fields, so they exist rather
 * than being implied by a writer ID and some flags.
 */
struct d1_fixture_authority {
	struct d1_uuid issuer;
	struct d1_uuid principal;
	uint8_t stateid[16];
	uint8_t session[16];
	uint32_t writer;
	uint32_t rights;
	uint64_t lease_epoch;
	uint64_t authority_epoch;
	uint64_t fence_sequence;
};

d1_id_t d1_fixture_admit_full(struct d1_store *s,
			      const struct d1_objkey *object,
			      const struct d1_fixture_authority *auth);

/*
 * The common case: one issuer, and a principal derived from the writer,
 * so two handles for one writer share a principal and two for different
 * writers do not.
 */
d1_id_t d1_fixture_admit(struct d1_store *s, const struct d1_objkey *object,
			 uint32_t writer, uint32_t rights);
void d1_fixture_revoke(struct d1_store *s, d1_id_t admission);
/* Expire an admission's lease without revoking its identity. */
void d1_fixture_expire(struct d1_store *s, d1_id_t admission);

/*
 * Issue repair custody over one version.  Rolling back committed data
 * needs this and ordinary owner custody never suffices; the handle is
 * tied to the exact version it is issued for, so a stale pointer is a
 * conflict rather than a weaker check.
 */
d1_id_t d1_fixture_custody(struct d1_store *s, d1_id_t version);

/*
 * Release the retention of one predecessor version.  Allowed only for a
 * version nothing makes visible and nothing else holds: it removes the
 * durable retention root and changes what a future rollback is eligible
 * for.  It says nothing about whether any bytes were freed.
 */
bool d1_fixture_release_predecessor(struct d1_store *s, d1_id_t version);

/*
 * Apply one envelope.  The returned status is the operation's own; per
 * entry results are in @out.  An operation this slice does not implement
 * answers D1_UNSUPPORTED and mutates nothing.
 */
uint32_t d1_store_apply(struct d1_store *s, const struct d1_envelope *env,
			struct d1_result *out);

/* What the model currently makes visible, for the oracles to ask. */
bool d1_store_visible(struct d1_store *s, const struct d1_objkey *object,
		      uint64_t index, d1_id_t *version);
bool d1_store_guard(struct d1_store *s, const struct d1_objkey *object,
		    uint64_t index, struct d1_guard *guard);
uint64_t d1_store_eof(struct d1_store *s, const struct d1_objkey *object);

/* One interval of the object's extent map. */
struct d1_interval {
	uint64_t start;
	uint64_t end;
};

/*
 * The holes of @object: the complement, within [0, EOF), of the
 * intervals its visible versions contribute.  Beyond EOF is not a hole,
 * it is a different answer, and is never reported here.  Returns how
 * many intervals were written.
 */
uint32_t d1_store_holes(struct d1_store *s, const struct d1_objkey *object,
			struct d1_interval *out, uint32_t max);

struct d1_view;

/*
 * Open a read view over [range_begin, range_end) of @object.
 *
 * The view decides once, at open, which version each chunk resolves to,
 * and pins it.  What it reads afterwards is what it decided, whatever
 * later commits or rollbacks do to the pointers -- that is what a view
 * is for.  ORDINARY selection takes the visible version; OWNER takes
 * @owner's own finalized version where it has one, and the visible
 * version everywhere else.
 *
 * Every selected payload is verified against its stored checksum here.
 * A failure rejects the whole view rather than one entry: this model
 * has no per-entry read result to put one in.
 */
uint32_t d1_view_open(struct d1_store *s, const struct d1_objkey *object,
		      d1_id_t admission, uint32_t selection,
		      const struct d1_owner *owner, uint64_t range_begin,
		      uint64_t range_end, struct d1_view **out);

/*
 * Read from the view.  Holes inside the view's EOF read as zeros; a
 * read starting at or past it returns zero bytes.
 */
uint32_t d1_view_read(struct d1_view *v, uint64_t offset, uint8_t *buf,
		      uint32_t len, uint32_t *out_len);

/* Which version the view selected for one chunk, if any. */
bool d1_view_version(const struct d1_view *v, uint64_t index, d1_id_t *ver);

/* The EOF the view saw when it opened. */
uint64_t d1_view_eof(const struct d1_view *v);

/* Drop the view's pins.  The view is gone once this returns. */
void d1_view_close(struct d1_store *s, struct d1_view *v);

/*
 * Start journalling.  Writes the START record that opens this
 * incarnation and names the geometry a replay must agree with.
 */
uint32_t d1_store_journal_enable(struct d1_store *s);

/* The journal bytes, for a test to truncate, corrupt or replay. */
const uint8_t *d1_store_journal(const struct d1_store *s, size_t *len);

/*
 * Write a checkpoint naming the counters and index epoch as they stand.
 * A replay that reaches it and disagrees has diverged, and says so
 * rather than carrying on.
 */
uint32_t d1_store_checkpoint(struct d1_store *s);

/*
 * Rebuild @s from @log.  @s must be freshly opened with the geometry
 * the log's START record names.  Records are applied in order until the
 * durable frontier; what lies past it was never written.
 */
uint32_t d1_store_replay(struct d1_store *s, const uint8_t *log,
			 size_t durable);

/*
 * Fixture fault control: refuse the next journal append.  It is not
 * journalled and does not survive, so it can neither replay nor outlive
 * the run that armed it.
 */
void d1_fixture_fail_next_append(struct d1_store *s);

/* Fixture fault control: refuse the next flush, so an appended record is
 * never claimed durable.  Also unjournalled and also disabled during
 * recovery. */
void d1_fixture_fail_next_flush(struct d1_store *s);

#endif /* REFFS_D1_STORE_H */
