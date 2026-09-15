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
 *
 * Every version reserves its payload inline at the maximum chunk size,
 * so an open store is a few hundred megabytes of mostly untouched
 * address space.  That is deliberate for a model with fixed tables and
 * no allocator of its own, and it is why physical reclamation is a
 * declared no-op: bytes are retained conservatively for the life of the
 * store.  It bounds how large a fixture can be, and nothing else.
 *
 * These sources carry no HAVE_CONFIG_H preamble, unlike most of the
 * repository's library code.  The model depends on nothing the build
 * configures: it is C11 and the C library, built here only so the
 * repository's check gate runs its tests.
 *
 * One mutex covers all model state, and the journal's byte vector grows
 * with realloc under it.  Allocating under the lock is accepted here:
 * this is a userspace model with no allocator of its own and no
 * interrupt context, and the memo's rule is that nothing which could
 * block on the outside world runs under the lock.
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
/*
 * Normal close.  Returns D1_BUSY while any view is outstanding or any
 * call has been admitted and not yet returned: a view's bytes and a
 * part-finished call's state both live in the store, so closing under
 * either is a use-after-free with a friendly name.
 *
 * On D1_OK the store is gone and the caller drops its pointer.  This
 * cannot protect a caller from using a handle it has already closed --
 * that is the caller's responsibility, and is a different thing from
 * the calls this refuses to close under, which were admitted first.
 */
uint32_t d1_store_close(struct d1_store *s);

/*
 * Fixture controls that hold what a paused call holds.  This model is
 * single threaded, so a test cannot stop a real call part way through;
 * between these two the store is in the state it is in between two
 * members of an ordinary batch, which is the window a close must not
 * slip through.
 */
void d1_fixture_call_enter(struct d1_store *s);
void d1_fixture_call_leave(struct d1_store *s);

/*
 * Crash teardown: destroy the whole simulated world, views included,
 * as a power loss would.  This is what crash tests use.  It is NOT a
 * normal close, and nothing may hold a view across it and then read.
 */
void d1_store_free(struct d1_store *s);

/* The current verifier, as a START publishes it. */
void d1_store_verifier(struct d1_store *s, uint8_t out[D1_VERIFIER_BYTES]);
uint64_t d1_store_incarnation(struct d1_store *s);

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
 * What a view selects.  ORDINARY sees only committed data.  OWNER names
 * the exact transactions it wants, with their owners, and the read
 * epoch it was granted; a named transaction that is not the caller's,
 * or an epoch that is not the one granted for it, fails the whole view
 * rather than quietly handing back committed data instead.
 */
struct d1_selection_spec {
	uint32_t selection;
	uint32_t count;
	d1_id_t txns[D1_BATCH_ENTRIES_MAX];
	struct d1_owner owners[D1_BATCH_ENTRIES_MAX];
	uint64_t read_epoch;
};

/*
 * Open a read view over the byte range [byte_begin, byte_end).
 *
 * The range is bytes, as section 6 specifies -- not chunk indices.  The
 * view resolves every chunk the range touches once, under the lock,
 * pins what it resolved before unlocking, and computes its own EOF and
 * extents from that selected vector.  What it reads afterwards is what
 * it decided, whatever later commits or rollbacks do to the pointers.
 *
 * Every selected payload is verified against its stored checksum here.
 * One inadmissible or unverifiable member fails the WHOLE view and
 * drops the pins it had taken: this model has no per-entry read result
 * to put a failure in.
 */
uint32_t d1_view_open(struct d1_store *s, const struct d1_objkey *object,
		      d1_id_t admission, const struct d1_selection_spec *sel,
		      uint64_t byte_begin, uint64_t byte_end,
		      struct d1_view **out);

/*
 * Read from the view.  The offset must lie inside the range the view
 * was opened over; a read outside it is refused rather than answered
 * with zeros.  Inside the range, a hole below the view's own EOF reads
 * as zeros, and a read at or past that EOF returns no bytes -- those
 * are different answers, not the same one.
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

uint32_t d1_store_replay(struct d1_store *s, const uint8_t *log,
			 size_t durable);

/*
 * Actual reopen: rebuild, then open a new incarnation.
 *
 * Read-only reconstruction is not a reboot.  A reopen appends and
 * flushes a new START, which fences the old incarnation's mutation
 * admissions and publishes a new verifier, and it continues the log's
 * LSNs rather than starting them over.  Doing it twice is ordinary.
 */
uint32_t d1_store_reopen(struct d1_store *s, const uint8_t *log,
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
/*
 * The same, aimed: the nth append from now fails.  A batch writes one
 * event per member, so this can interrupt a batch in the middle rather
 * than only at its first member.
 */
void d1_fixture_fail_append_in(struct d1_store *s, uint32_t n);

void d1_fixture_fail_next_flush(struct d1_store *s);

/*
 * Fixture fault control: force the next publication to leave the
 * materialized index behind.  The event is already durable, so the
 * COMMIT receipt stands.
 *
 * Reads in this model use the reducer's state and always did, so
 * nothing switches over; what the fault gives a test is a stale
 * materialized pointer to check against, showing that no read followed
 * it.  That is weaker than a failover between two real index paths, and
 * is not claimed to be one.  Unjournalled, and disabled during
 * recovery.
 */
void d1_fixture_fail_next_index(struct d1_store *s);

/* Whether an index fault has left a materialized pointer behind. */
bool d1_store_overlay_active(struct d1_store *s);

/*
 * What the materialized index still says, so a test can prove a read
 * did not consult it.  There is no production use for this.
 */
bool d1_store_materialized(struct d1_store *s, const struct d1_objkey *object,
			   uint64_t index, d1_id_t *version);

#endif /* REFFS_D1_STORE_H */
