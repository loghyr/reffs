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
/*
 * Repair cohorts a store may open in its life.  A cohort is one local
 * repair over one object, and section 7 keeps its whole vector, so this
 * is a table of vectors rather than of members.
 *
 * Not a count of cohorts open at once: section 6's retain-everything
 * policy keeps a committed or aborted cohort's row for the life of the
 * store, so nothing gives a row back and the ninth repair of one chunk
 * is refused for want of room however few are outstanding.  That is
 * this model's choice and not a limit of the interface; what makes it
 * defensible is that no receipt or handle it issued ever becomes
 * unresolvable.
 */
#define D1_MAX_REPAIRS 8u
/*
 * Postconditions the store retains.  A refused rollback mints one and
 * a NOPRE repair consumes it; under section 6's retain-everything
 * policy a consumed row stays, so this is a lifetime cap and not a
 * count of outstanding ones.
 */
#define D1_MAX_POSTCONDS 32u
/*
 * ERROR episodes the store retains.  One mark_error opens one over the
 * vector it named, and an unlock ends it; the row stays under section
 * 6's retain-everything policy, so this is a lifetime cap.
 */
#define D1_MAX_EPISODES 16u
#define D1_MAX_INTERVALS 64u
#define D1_MAX_VIEWS 32u

struct d1_store;

/* What one entry, or one whole control operation, answered. */
struct d1_entry_result {
	uint32_t status;
	bool version_present;
	d1_version_id version;
	bool txn_present;
	d1_txn_id txn;
	/*
	 * The repair cohort this entry opened, for begin_repair, and the
	 * one it acted on for every later repair operation.  Absent for
	 * everything that is not a repair.
	 */
	bool cohort_present;
	d1_repair_id cohort;
	/*
	 * The transactions the cohort's members were issued, in member
	 * order.
	 *
	 * Section 4's result for begin_repair is "cohort and per-member
	 * transaction handles", and section 9 requires the logged result
	 * to contain every returned ID.  The cohort alone was not
	 * callable: every later repair call names each member's own
	 * transaction, and a caller outside this file had no way to learn
	 * them -- the suite reached them through a fixture, which is a
	 * test's privilege and not a contract.
	 *
	 * Empty for every operation that issues none, which is all of
	 * them but begin_repair.  It is a counted vector rather than an
	 * option per member because the count is the cohort's and the
	 * order is the request's.
	 */
	uint32_t member_txn_count;
	d1_txn_id member_txn[D1_BATCH_ENTRIES_MAX];
	/*
	 * The postcondition a rollback that answered NO_PREDECESSOR left
	 * behind, bound to the successor it did not replace.  Section 4
	 * makes it part of the rollback result; it is what a later NOPRE
	 * repair of that successor consumes.
	 */
	bool postcond_present;
	d1_postcond_id postcond;
	/*
	 * The ERROR episode a mark_error opened over its vector, which
	 * section 4 answers with.  Every later call about those members
	 * names it again.
	 */
	bool episode_present;
	d1_episode_id episode;
	/*
	 * Which member of a whole-vector call this answer is about.
	 *
	 * A repair answers once for its cohort, so a refusal that one
	 * member caused -- its guard, its checksum, its phase -- has to
	 * say which, or the guard beside it names a chunk the caller has
	 * to guess.  Absent for an answer that is the whole vector's, and
	 * for everything that is not a repair.
	 */
	bool member_present;
	uint32_t member;
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
	/*
	 * The operation's disposition, which is a different question from
	 * any member's.
	 *
	 * For a control operation there is one answer and this is it.  For
	 * an ordinary batch it says whether the operation as a whole left
	 * a record: COMPLETED when any member did, UNRECORDED when none
	 * did.  A batch stops at the first member it could not record, so
	 * "any" and "the first" are the same member, and a caller reading
	 * only this field learns whether there is anything to retry
	 * against -- not whether every member ran.  Which members ran is
	 * what the per-entry dispositions are for, and they are the ones
	 * to read.
	 */
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
 * Logical close.  Returns D1_BUSY while any view is outstanding or any
 * call has been admitted and not yet returned: a view's bytes and a
 * part-finished call's state both live in the store, so closing under
 * either is a use-after-free with a friendly name.
 *
 * On D1_OK the store stops admitting calls and every later one fails
 * closed -- the observers included, which answer as if they knew
 * nothing -- but the allocation is still there.  That is deliberate.  A
 * caller which has entered a public function and has not yet reached
 * the lock is not counted anywhere, and freeing the store here would
 * leave it to lock destroyed memory; leaving the allocation alive lets
 * it arrive, find the store closed, and be refused.  Read what you mean
 * to read before you close.  Closing twice is D1_OK, and closing does
 * not free.
 */
uint32_t d1_store_close(struct d1_store *s);

/*
 * Ordinary destruction, after a successful close.  Returns D1_BUSY for
 * a store that is not closed, because such a store is still admitting
 * calls.  Otherwise the memory goes and the caller drops its pointer.
 *
 * The precondition this cannot check, and the owner must meet, is that
 * every thread which may still be inside a public call has been
 * excluded and joined -- including one parked between a public
 * function's first instruction and its admission.  A close makes that
 * wait finite: the parked caller is refused as soon as it runs.
 */
uint32_t d1_store_destroy(struct d1_store *s);

/*
 * Crash teardown: destroy the whole simulated world, views included,
 * as a power loss would.  This is what crash tests use.  It is NOT a
 * normal close and NOT an ordinary destruction: it asks nothing, it
 * refuses nothing, and nothing may hold a view across it and then
 * read.  A live store torn down this way is a store that lost power.
 */
void d1_store_free(struct d1_store *s);

/*
 * Fixture control: a handle of @s with a chosen value.
 *
 * On the wire a handle is a bare u64, so a client can always name any
 * number for the store it is talking to -- a number that store never
 * issued, or one it issued for something else.  These model exactly
 * that and nothing more: the handle they make belongs to @s, so they
 * cannot hand one store another store's authority, and they cannot
 * change which table a value is looked up in.  What a value names is
 * still whatever row @s has under it, which for a number @s never
 * issued is no row at all.
 *
 * Which is the whole of what the runtime kind and token are.  They are
 * safety metadata that keeps a value in the domain and the store its
 * request field names; they are not a capability and not a secret, and
 * naming (target, domain, raw) here is the same thing as holding a
 * handle with that value.  What they do refuse is the two mistakes a
 * caller can make without meaning to: an admission copied unchanged
 * into a version still says admission, and a handle one live store
 * issued still names nothing in another.  Authority over an object is
 * the admission's to grant, here as everywhere else, and that is the
 * boundary this model enforces.
 */
d1_admission_id d1_fixture_admission_handle(struct d1_store *s, uint64_t raw);
d1_txn_id d1_fixture_txn_handle(struct d1_store *s, uint64_t raw);
d1_version_id d1_fixture_version_handle(struct d1_store *s, uint64_t raw);
d1_custody_id d1_fixture_custody_handle(struct d1_store *s, uint64_t raw);

/*
 * Fixture arms: answer as though there were no ID, or no index epoch,
 * left to give.
 *
 * Every counter in this model ascends and none is reused, so their
 * exhausted states are real states of the contract and unreachable
 * through the public API -- the tables are fixed size, rows are never
 * freed, and no history allocates two to the sixty-fourth of anything.
 * These arm that answer so it can be tested.
 *
 * They are arms, not settings.  While one is on, the store gives the
 * refusal an exhausted counter gives; nothing else changes, no counter
 * or epoch moves, no result carries a value the fixture chose, and the
 * log gains nothing.  Turning one off does not restore a number: it
 * resumes from whatever the history has derived, which is what the next
 * ordinary operation would have used anyway.  Like every other arm they
 * are refused on a store that is not serving, ignored during replay,
 * and cleared when a reconstruction begins.
 */
void d1_fixture_exhaust_ids(struct d1_store *s, bool on);
void d1_fixture_exhaust_epoch(struct d1_store *s, bool on);

/*
 * Fixture control: run @fn once, in the interval between a public
 * call's first instruction and its admission on @s -- the one interval
 * the store's own lock does not cover, and the one a close races.
 *
 * The arm's storage is outside the store, because the store is what may
 * be destroyed while a call is in it, and there is exactly one of it
 * for the process.  The arm names @s: only a call entering @s takes it,
 * a call on another store neither consumes nor runs it, and a null @fn
 * disarms this store's arm and no other.  @fn runs with no lock held.
 * It is unjournalled, one-shot, and forgotten when @s is reconstructed,
 * closed or destroyed, so nothing it was aimed at outlives the store or
 * the run that armed it.
 *
 * D1_OK armed it, replaced this store's own callback, or disarmed it.
 * D1_BUSY is another live store holding the one slot; its arm is left
 * exactly as it was and this store gets none.  D1_INVALID is a closed
 * or poisoned store, or one in the middle of recovery.
 */
uint32_t d1_fixture_before_admission(struct d1_store *s, void (*fn)(void *),
				     void *arg);

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

d1_admission_id d1_fixture_admit_full(struct d1_store *s,
				      const struct d1_objkey *object,
				      const struct d1_fixture_authority *auth);

/*
 * The common case: one issuer, and a principal derived from the writer,
 * so two handles for one writer share a principal and two for different
 * writers do not.
 */
d1_admission_id d1_fixture_admit(struct d1_store *s,
				 const struct d1_objkey *object,
				 uint32_t writer, uint32_t rights);
void d1_fixture_revoke(struct d1_store *s, d1_admission_id admission);
/* Expire an admission's lease without revoking its identity. */
void d1_fixture_expire(struct d1_store *s, d1_admission_id admission);

/*
 * Issue repair custody over one version.  Rolling back committed data
 * needs this and ordinary owner custody never suffices; the handle is
 * tied to the exact version it is issued for, so a stale pointer is a
 * conflict rather than a weaker check.
 */
d1_custody_id d1_fixture_custody(struct d1_store *s, d1_version_id version);

/*
 * Issue the cross-DS completion certificate clear_error requires.
 *
 * What issues one is outside D1 entirely: it is the other data server's
 * statement that its half of the repair is done.  The fixture stands in
 * for that issuer, and the model only ever compares what it issued
 * against what a clear_error carries.  Passing null withdraws it.
 */
void d1_fixture_certificate(struct d1_store *s,
			    const uint8_t certificate[D1_CERTIFICATE_BYTES]);

/*
 * Name a repair cohort by its canonical value.
 *
 * A handle a live begin_repair issued is the ordinary way to have one.
 * A rebuild, a reopen and a malformed-handle test all need to name one
 * without that call, so this makes the handle out of the number, for
 * this store, exactly as the other handle constructors do.
 */
d1_repair_id d1_fixture_repair_handle(struct d1_store *s, uint64_t raw);

/*
 * Name a rollback postcondition by its canonical value, for the same
 * reasons: a rebuild, a reopen and a stale-handle test all need one
 * without the refused rollback that would issue it.
 */
d1_postcond_id d1_fixture_postcond_handle(struct d1_store *s, uint64_t raw);

/* And an ERROR episode, for the same reasons. */
d1_episode_id d1_fixture_episode_handle(struct d1_store *s, uint64_t raw);

/*
 * What a postcondition the store holds was bound to, for the tests
 * that need to name a wrong one.  Answers false for a handle this
 * store never issued.
 */
bool d1_fixture_postcond(struct d1_store *s, d1_postcond_id id, uint64_t *index,
			 d1_version_id *successor, bool *consumed);

/*
 * The handles one member of a repair cohort holds: its own repair
 * transaction, and the replacement it has staged if it has staged one.
 *
 * Section 4 answers begin_repair with the cohort and the per-member
 * transaction handles.  This model's result carries one entry for the
 * whole cohort -- a repair takes one receipt -- so the per-member
 * handles are reached through the cohort rather than returned beside
 * it.  Whether the canonical result should instead carry a vector of
 * them is a shape question this slice does not settle.
 *
 * False when the cohort or the member does not exist.
 */
bool d1_fixture_repair_member(struct d1_store *s, d1_repair_id cohort,
			      uint32_t index, d1_txn_id *txn,
			      d1_version_id *version);
bool d1_fixture_repair_state(struct d1_store *s, d1_repair_id cohort,
			     uint32_t *phase, uint32_t *member_count);

/*
 * A transaction's phase and the version it carries.
 *
 * A repair member is a transaction, and the fields the cohort moves on
 * it are not reachable through any operation once section 5 refuses a
 * private rollback of a REPAIR member in every private phase.  They are
 * still part of the state recovery must reproduce: section 9 has a
 * rebuilt store re-execute the log and fail closed on any disagreement,
 * so a test that compares a live store against one rebuilt from its own
 * log has to be able to see them.
 *
 * False when no such transaction exists in this store.
 */
bool d1_fixture_txn_state(struct d1_store *s, d1_txn_id txn, uint32_t *phase,
			  d1_version_id *version);

/* The predecessor claim held by one retained version. */
bool d1_fixture_version_predecessor(struct d1_store *s, d1_version_id version,
				    bool *present, d1_version_id *predecessor);

/* Whether a canonical version row is still retained, and its release bit. */
bool d1_fixture_version_retained(struct d1_store *s, d1_version_id version,
				 bool *released);

/* Restore the recorded checksum after recovery found damaged bytes. */
bool d1_fixture_version_damage(struct d1_store *s, d1_version_id version,
			       const struct d1_checksum *checksum);

/*
 * Release the retention of one predecessor version.  Allowed only for a
 * version nothing makes visible and nothing else holds: it removes the
 * durable retention root and changes what a future rollback is eligible
 * for.  It says nothing about whether any bytes were freed.
 */
bool d1_fixture_release_predecessor(struct d1_store *s, d1_version_id version);

/*
 * Apply one envelope.  The returned status is the operation's own; per
 * entry results are in @out.  An operation this slice does not implement
 * answers D1_UNSUPPORTED and mutates nothing.
 *
 * Each member result carries a disposition as well as a status.
 * D1_COMPLETED means the answer comes from what is recorded under the
 * operation key, and the same question will be answered the same way
 * for as long as that record is kept -- a semantic refusal is recorded
 * exactly like a success.  D1_UNRECORDED means nothing was durably
 * decided: the member did not happen, it consumed no capacity, and the
 * caller may retry it.  Any UNRECORDED member ends the batch, whatever
 * refused it: the members after it are reported UNRECORDED too, no
 * receipt is invented for work that was never attempted, and nothing is
 * recorded over the hole an earlier member left -- which is what makes
 * the receipts under a key a dense prefix, and the log replayable.
 *
 * An operation key is bound by the whole Envelope from the moment any
 * member of it is recorded.  A retry with a changed body -- a different
 * payload, a different admission, anything the request digest covers --
 * is refused with D1_REPLAY_CONFLICT and mutates nothing, and it cannot
 * take the key from the original: the exact original request can still
 * be resumed and finished afterwards.  A key with no recorded member is
 * not bound, and is free for any request.
 *
 * That refusal is D1_COMPLETED in the sense above -- it is the answer
 * the key's record dictates -- and in no other.  No receipt is created
 * for the changed request, nothing of it is retained, and nothing about
 * it can be resumed; the record it was answered from belongs to the
 * Envelope that was there first.
 *
 * The binding holds between concurrent callers as well as between a
 * request and its own retry.  It is revalidated inside the lock
 * interval that records each member, not once for the call, so two
 * callers that both find the key free cannot then interleave their
 * members into one key: whichever records first owns it, and the other
 * is refused on every member and records nothing.
 *
 * The caller binding is settled before the key is: a handle that is not
 * bound to the object it names is answered D1_STALE_AUTH and is told
 * nothing about whether the key is in use.
 */
uint32_t d1_store_apply(struct d1_store *s, const struct d1_envelope *env,
			struct d1_result *out);
uint32_t d1_store_probe(struct d1_store *s, const struct d1_envelope *env,
			struct d1_result *out);

/* What the model currently makes visible, for the oracles to ask. */
bool d1_store_visible(struct d1_store *s, const struct d1_objkey *object,
		      uint64_t index, d1_version_id *version);
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
	d1_txn_id txns[D1_BATCH_ENTRIES_MAX];
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
 * to put a failure in.  The whole OWNER vector is judged before any of
 * it is selected, so the answer does not depend on the order the
 * members are written in, and two members resolving to one chunk are
 * not a selection at all.
 *
 * An OWNER selection substitutes FINALIZED versions only.  A member
 * naming a transaction that has already COMMITTED fails the view, even
 * though an ordinary read of that chunk would return the version it
 * committed.  The memo permits substituting matching FINALIZED versions
 * and is silent about committed ones; refusing is the conservative
 * reading, and a caller that wants the committed data can ask for it
 * ordinarily.  Chunks the vector does not name are supplied ordinarily
 * as usual.
 */
uint32_t d1_view_open(struct d1_store *s, const struct d1_objkey *object,
		      d1_admission_id admission,
		      const struct d1_selection_spec *sel, uint64_t byte_begin,
		      uint64_t byte_end, struct d1_view **out);

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
bool d1_view_version(const struct d1_view *v, uint64_t index,
		     d1_version_id *ver);

/* The EOF the view saw when it opened. */
uint64_t d1_view_eof(const struct d1_view *v);

/*
 * Drop the view's pins.  The view is gone once this returns, and a null
 * view is nothing to release.
 *
 * It releases through the store it was opened on, which the view
 * already knows: naming a store here as well would be an ownership
 * precondition the caller could get wrong, and getting it wrong would
 * clear a view out of one store while unpinning in another -- which is
 * how a store comes to see no live view and let a close through under
 * one.
 */
void d1_view_close(struct d1_view *v);

/*
 * Start journalling.  Writes the START record that opens this
 * incarnation.
 *
 * The log describes everything its store did, so it can only begin on a
 * store that has not done anything yet: a store holding unlogged
 * authority or data, and a handle whose own rebuild failed, both refuse
 * with D1_INVALID.
 *
 * The START record does NOT carry the geometry.  Chunk size and maximum
 * file size are trusted configuration supplied by the caller to
 * d1_store_open, and this log neither records nor checks them.
 * Identical geometry is therefore a precondition on the caller, not
 * something reconstruction can verify.  A mismatch is caught only
 * incidentally: a logged result whose value happens to depend on the
 * chunk size -- a resulting EOF, most often -- will not compare equal
 * on a target of another size, and the rebuild fails closed.  A history
 * whose logged results do not depend on it replays without complaint
 * and then derives different geometry-dependent quantities, such as a
 * private OWNER view's captured EOF.  Binding geometry to storage
 * identity belongs to a persistent backend, and is not done here.
 */
uint32_t d1_store_journal_enable(struct d1_store *s);

/*
 * A snapshot of the durable journal, for a test to truncate, corrupt or
 * replay.  The bytes are copied under the store's lock and belong to
 * the caller, who releases them with free(): they are a value and not a
 * window, so an append that grows the store's own buffer does not move
 * them, and they outlive the close and the destruction of the store
 * they came from.
 *
 * D1_OK writes the snapshot and its length.  A live store with an empty
 * journal is D1_OK, length zero and a null pointer, which free()
 * accepts.  D1_INVALID is a closed or poisoned store, which answers
 * nothing, as every other observer does.  D1_NOSPC is a snapshot that
 * could not be allocated; nothing of the store moved, so asking again
 * is ordinary.
 */
uint32_t d1_store_journal_snapshot(struct d1_store *s, uint8_t **out,
				   size_t *len);

/*
 * Rebuild a store from a log, read-only: no new records are written.
 *
 * The target must be pristine -- freshly opened, with no authority, no
 * data and no journal of its own -- and it must be exclusively the
 * caller's for the duration.  A populated target is refused with
 * D1_INVALID before anything is reduced, so a refused target is still
 * exactly the store it was.  There is no mechanism here for replacing
 * an active store underneath its callers, and none is wanted: a
 * reconstruction target is owned by one caller until it returns.
 *
 * A rebuild that fails part way leaves the handle poisoned, and
 * "serves nothing" is meant literally: it applies nothing, opens no
 * view, accepts no fixture authority, and every public observer answers
 * as if it knew nothing -- no visible version, no guard, no EOF, no
 * holes, no materialized pointer, a zero verifier and incarnation, and
 * no journal bytes.  A partial reduction is neither the logged history
 * nor an empty store, and there is deliberately no diagnostic route to
 * it: a caller that wants to see what a log builds reads the store it
 * is comparing against, before the rebuild that failed.  Only teardown
 * is left for a poisoned handle.
 */
uint32_t d1_store_replay(struct d1_store *s, const uint8_t *log,
			 size_t durable);

/* Rebuild a pristine fixture and resume its existing journal incarnation. */
uint32_t d1_fixture_restore_journal(struct d1_store *s, const uint8_t *log,
				    size_t durable);

/*
 * Actual reopen: rebuild, then open a new incarnation.
 *
 * Read-only reconstruction is not a reboot.  A reopen appends and
 * flushes a new START, which fences the old incarnation's mutation
 * admissions and publishes a new verifier, and it continues the log's
 * LSNs rather than starting them over.  Doing it twice is ordinary.
 *
 * The pristine target, exclusive ownership, geometry and poisoning
 * rules of d1_store_replay apply here unchanged, and one more: a reopen
 * whose rebuild succeeded and whose new journal then could not be
 * initialized, adopted or started poisons the handle too.  What it
 * would otherwise leave is a populated, mutable store that nothing is
 * recording and that can never be reconstructed into again.
 *
 * No unjournalled fixture arm survives either path: reconstruction
 * clears the fault arms and the member hook on entry, so the first
 * operation after a rebuild or a reopen is an ordinary one.
 */
uint32_t d1_store_reopen(struct d1_store *s, const uint8_t *log,
			 size_t durable);

/*
 * Fixture fault control: refuse the next journal append.  It is not
 * journalled, it does not survive a rebuild or a reopen, and it cannot
 * be armed during one -- so it can neither replay nor outlive the run
 * that armed it.
 *
 * "Next" includes the START that d1_store_journal_enable writes: an arm
 * set before the journal exists survives its creation, because the
 * fault belongs to the run and not to the buffer.  That is the only way
 * to ask for the one failure that happens before the log has a
 * frontier at all.
 */
void d1_fixture_fail_next_append(struct d1_store *s);

/*
 * The same, aimed: the nth append from now fails.  A batch writes one
 * event per member, so this can interrupt a batch in the middle rather
 * than only at its first member.
 */
void d1_fixture_fail_append_in(struct d1_store *s, uint32_t n);

/*
 * Fixture fault control: refuse the next flush, so an appended record is
 * never claimed durable.  Also unjournalled, also disabled during
 * recovery, and it reaches an enable's START on the same terms.
 */
void d1_fixture_fail_next_flush(struct d1_store *s);

/* Which step of a reopen's new START a fixture may fail. */
enum d1_reopen_start_fault {
	D1_REOPEN_START_OK = 0,
	D1_REOPEN_START_APPEND = 1,
	D1_REOPEN_START_FLUSH = 2,
};

/*
 * Arm the next reopen's START to fail at @which.
 *
 * A reopen is one transition -- rebuild, adopt, make a new START
 * durable -- and that START is the one failure it can have before its
 * own frontier.  Reduction clears the arms that describe operations, so
 * none of them ever reached this step; this arm describes the
 * transition, is consumed by it, and is dropped by a read-only rebuild.
 * A reopen that fails here leaves a handle only teardown may touch.
 */
void d1_fixture_fail_reopen_start(struct d1_store *s, uint32_t which);

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
bool d1_fixture_stale_index(struct d1_store *s, const struct d1_objkey *object,
			    uint64_t index);

/*
 * Fixture fault control: refuse the next journal snapshot, as an
 * allocation failure would.  It is not journalled, it does not survive
 * a rebuild or a reopen, and it is refused on a closed or poisoned
 * store and during recovery, like every other arm.
 */
void d1_fixture_fail_next_snapshot(struct d1_store *s);

/* Whether an index fault has left a materialized pointer behind. */
bool d1_store_overlay_active(struct d1_store *s);

/*
 * Fixture control: run @fn once, in the gap a batch leaves before the
 * member at @ordinal -- and at ordinal zero, in the gap between
 * deciding that its operation key is free and running the first member.
 *
 * Those gaps are where a second caller lands when it is preempted, and
 * they are the places from which two callers can reach one key: the
 * preflight gap, and every gap between two members.  A scheduler finds
 * one rarely enough that waiting for it is not evidence, so a test
 * opens one on purpose: @fn runs with no lock held and may make
 * ordinary calls of its own.  A batch that stops before @ordinal never
 * runs it, which is how a test proves the batch stopped.  The arm is
 * one-shot, unjournalled, and refused during recovery.
 */
void d1_fixture_before_member(struct d1_store *s, uint32_t ordinal,
			      void (*fn)(void *), void *arg);

/*
 * What the materialized index still says, so a test can prove a read
 * did not consult it.  There is no production use for this.
 */
bool d1_store_materialized(struct d1_store *s, const struct d1_objkey *object,
			   uint64_t index, d1_version_id *version);

#endif /* REFFS_D1_STORE_H */
