/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * D1 storage model: state, admission and the ordinary write lifecycle.
 *
 * A write accepts its guard predicate, takes the next generation and
 * installs a PREPARED transaction; it does not become visible because it
 * is durable.  FINALIZE makes that version selectable by its own owner,
 * and COMMIT publishes it.  The three have different operation keys and
 * different receipts: a successful write is a successful write, and
 * never becomes a successful commit.
 *
 * The one exception is activation, and it is one event rather than a
 * shortcut through the other two: a single-writer request that asks for
 * it, on an empty chunk, with a stability stronger than UNSTABLE,
 * publishes owner, payload, extent, guard and receipt together.
 */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "d1_digest.h"
#include "d1_control.h"
#include "d1_journal.h"
#include "d1_store.h"

struct d1_version {
	bool used;
	uint64_t id;
	uint32_t object;
	uint64_t index;
	uint8_t bytes[D1_CHUNK_BYTES_MAX];
	uint32_t len;
	struct d1_checksum checksum;
	struct d1_owner owner;
	/* The predecessor this version displaced, if it is still retained. */
	bool predecessor_present;
	uint64_t predecessor;
	/*
	 * Whether this version is still retained as somebody's predecessor.
	 * Releasing it removes that root; the bytes are a separate question
	 * this model never answers in a receipt.
	 */
	bool released;
	/*
	 * Live read pins.  A pinned version's bytes stay whatever else
	 * happens to the pointers that named it.
	 */
	uint32_t pins;
};

struct d1_txn {
	bool used;
	uint64_t id;
	uint32_t object;
	uint64_t index;
	struct d1_owner owner;
	uint64_t admission;
	uint64_t version;
	uint32_t phase;
	uint32_t mode;
	uint32_t stability;
	/*
	 * The read epoch an owner view must present to select this
	 * transaction's version.  Recovery re-admission grants a new one.
	 */
	uint64_t read_epoch;
	bool predecessor_present;
	uint64_t predecessor;
	/* The guard this transaction installed when it was admitted. */
	struct d1_guard guard;
};

struct d1_chunk {
	struct d1_guard guard;
	bool pending_present;
	uint64_t pending;
	/* The reducer's state: what the durable events say is visible. */
	bool visible_present;
	uint64_t visible;
	/*
	 * The materialized index.  Reads in this model never consult it at
	 * all: they use the reducer's visible pointer, which is the
	 * authoritative state.  It exists so an injected fault can leave a
	 * stale pointer behind and a test can show that no read followed
	 * it.  That is weaker than demonstrating a failover between two
	 * real index paths, and this model does not claim otherwise.
	 */
	bool materialized_present;
	uint64_t materialized;
	bool index_stale;
	/*
	 * The repair cohort that has this chunk, if one has.  Section 7's
	 * "active repair locks block ordinary writers" is this: while a
	 * cohort holds a chunk, an ordinary write to it is refused, and
	 * the hold goes when the cohort commits, aborts or is unlocked.
	 */
	bool repair_present;
	uint64_t repair;
	/*
	 * The ERROR episode this chunk is in, if one was marked.  Section
	 * 7 requires a durable mark_error episode with continuous custody
	 * before an ERROR-mode repair, and requires the members to stay
	 * locked after commit until clear_error has seen the certificate.
	 */
	bool error_present;
	uint64_t error_episode;
	uint64_t error_custody;
	uint64_t error_version;
	bool error_cleared;
	/* Committed by a repair and not yet unlocked. */
	bool repair_locked;
};

struct d1_object {
	bool used;
	struct d1_objkey key;
	struct d1_chunk chunks[D1_MAX_CHUNKS];
};

/*
 * One member of a repair cohort, as the store keeps it.
 *
 * Section 7 has begin_repair capture the exact predecessor/current
 * vector, and every later call in the repair name that same vector in
 * the same order.  So the captured state lives here, not in the
 * requests: a later call states what it believes and is compared
 * against this, which is what makes "unchanged predecessors" a question
 * the store can answer rather than one the caller asserts.
 */
struct d1_repair_member {
	uint64_t index;
	uint32_t mode;
	struct d1_owner owner;
	uint64_t custody;
	/* The version this member repairs, as it stood at begin_repair. */
	uint64_t successor;
	bool predecessor_present;
	uint64_t predecessor;
	/* The private replacement prepare_repair staged, if it has. */
	bool staged;
	uint64_t version;
	/*
	 * The member's own repair transaction.  Section 4 gives
	 * begin_repair per-member transaction handles and section 5 has
	 * ordinary lifecycle and private rollback reject them as REPAIR
	 * members rather than as unknown numbers; section 9's recovery
	 * re-binds them like any other transaction, which is how an open
	 * cohort survives a reopen.  A cohort with no transactions made
	 * all of that unreachable.
	 */
	uint64_t txn;
};

/*
 * A repair cohort: one local repair, of one object, under one handle.
 *
 * Its phase is the whole cohort's.  Section 7 publishes the complete
 * local vector at one index epoch or publishes nothing, so there is no
 * per-member phase to disagree with it.
 */
struct d1_repair {
	bool used;
	uint64_t id;
	uint32_t object;
	uint64_t admission;
	uint32_t phase;
	uint32_t count;
	struct d1_repair_member member[D1_BATCH_ENTRIES_MAX];
};

/*
 * One row of the fixture authority table.  Section 2 names the fields;
 * they are here because the bindings a call checks are exactly these,
 * and a handle that carried only a writer and some flags could not
 * answer "same principal, same issuer" when recovery re-admission asks.
 */
struct d1_admission {
	bool used;
	uint64_t id;
	struct d1_uuid issuer;
	struct d1_objkey object;
	struct d1_uuid principal;
	uint32_t writer;
	uint8_t stateid[16];
	uint8_t session[16];
	uint64_t lease_epoch;
	uint64_t authority_epoch;
	uint64_t fence_sequence;
	uint64_t incarnation;
	uint32_t rights;
	bool revoked;
	bool expired;
};

/*
 * One ERROR episode, as mark_error opened it.
 *
 * Section 4 answers mark_error with "episode, quarantined vector" and
 * has begin_repair's ERROR members, clear_error and unlock name that
 * episode again.  The chunk keeps the version and the custody the
 * episode bound it to, because those are per member; this row is the
 * episode's identity and the vector it covers, which is what a caller
 * names and what every later call is checked against.
 *
 * Without it the episode was implicit -- a chunk was in "the" episode
 * because it carried a mark -- which is only unambiguous while one
 * chunk can be in one episode.  The memo does not say the identity is
 * dispensable there; it says the request carries it.
 */
struct d1_episode {
	bool used;
	uint64_t id;
	uint32_t object;
	uint32_t count;
	uint64_t index[D1_BATCH_ENTRIES_MAX];
};

/*
 * What a refused rollback left behind.
 *
 * Section 7: a rollback of committed data whose predecessor is missing
 * or released "leaves current data in place and records NO_PREDECESSOR
 * plus index, successor, predecessor disposition, owner, custody and
 * eligibility", and section 4 returns the caller a durable
 * postcondition handle for it.  That handle is what a later NOPRE
 * repair consumes, which is why this row exists at all: the memo
 * authorizes a NOPRE repair on a rollback that was actually attempted
 * and refused, not on a state that would refuse one if it were.
 *
 * So this is not partial mutation a refusal forgot to undo.  It is the
 * result the refusal returned, and the entry keeps it the way an
 * accepted entry keeps a version.
 */
struct d1_postcond {
	bool used;
	uint64_t id;
	uint32_t object;
	uint64_t index;
	/* The version left in place, and the owner it belongs to. */
	uint64_t successor;
	struct d1_owner owner;
	/* What the rollback found where the predecessor should be. */
	bool predecessor_present;
	uint64_t predecessor;
	bool predecessor_released;
	/* The repair custody the refused rollback was presented with. */
	uint64_t custody;
	bool consumed;
};

/* (export UUID, cohort, writer, co_id) -> one object, chunk and version. */
struct d1_owner_assoc {
	bool used;
	struct d1_uuid export_uuid;
	struct d1_owner owner;
	uint32_t object;
	uint64_t index;
	uint64_t version;
};

/*
 * A read view.  It holds one pinned version per chunk of its range and
 * the EOF it saw, so what it reads is what it decided at open.
 */
struct d1_view {
	bool used;
	struct d1_store *store;
	uint32_t object;
	uint64_t range_begin;
	uint64_t range_end;
	/*
	 * The EOF of the whole effective object as it stood at open, not
	 * of the window that was asked for.  A hole below it is a hole
	 * wherever the read window happens to end; making EOF window-local
	 * turned a hole under a higher chunk into end-of-file.
	 */
	uint64_t eof;
	uint64_t index_epoch;
	/* The extent every chunk contributed, captured under the lock. */
	bool extent_present[D1_MAX_CHUNKS];
	uint32_t extent_len[D1_MAX_CHUNKS];
	/* The versions this view pinned: the ones its window can read. */
	bool present[D1_MAX_CHUNKS];
	uint64_t version[D1_MAX_CHUNKS];
};

/* Repair custody over one exact version. */
struct d1_custody {
	bool used;
	uint64_t id;
	uint64_t version;
};

struct d1_receipt {
	bool used;
	struct d1_uuid export_uuid;
	struct d1_opkey key;
	uint32_t ordinal;
	uint8_t digest[D1_DIGEST_BYTES];
	/*
	 * The complete recorded result, including the epoch and EOF the
	 * request saw.  An exact replay returns these, not values
	 * recomputed from whatever the store looks like later.
	 */
	struct d1_complete_result result;
};

struct d1_store {
	/*
	 * One lock for all model state.  It is taken for a whole ordinary
	 * entry -- its admission, its transition, its published state and
	 * its receipt -- and released between entries, so the next entry is
	 * revalidated against whatever the previous one left.  Nothing that
	 * could block on the outside world happens under it.
	 */
	pthread_mutex_t lock;
	/*
	 * This live object's runtime token: what a handle it issued
	 * carries, and what a resolver compares before it reads a table.
	 *
	 * A reopen does not change it, because a reopen is this object
	 * being handed a log.  That says less than it sounds: a
	 * reconstruction target must be pristine, so an object that can
	 * be reopened has issued no handle for the token to matter to.
	 * What the token separates is two objects, which is the case the
	 * model actually creates -- a live store and a pristine target of
	 * the same name, at the same time.
	 */
	uint64_t instance;
	struct d1_uuid uuid;
	uint32_t chunk_bytes;
	uint64_t max_file_bytes;
	uint64_t incarnation;
	uint64_t index_epoch;

	/*
	 * Monotonic per-type counters; none is ever reused, and each
	 * refuses rather than wrap.  A store with one of them at its last
	 * value has no ID left to give, which is a want of room and not a
	 * malformed request.
	 */
	uint64_t next_txn;
	uint64_t next_version;
	uint64_t next_admission;

	struct d1_object objects[D1_MAX_OBJECTS];
	struct d1_txn txns[D1_MAX_TXNS];
	struct d1_version versions[D1_MAX_VERSIONS];
	struct d1_admission admissions[D1_MAX_ADMISSIONS];
	struct d1_owner_assoc owners[D1_MAX_OWNERS];
	struct d1_receipt receipts[D1_MAX_RECEIPTS];
	struct d1_custody custody[D1_MAX_CUSTODY];
	struct d1_repair repairs[D1_MAX_REPAIRS];
	struct d1_postcond postconds[D1_MAX_POSTCONDS];
	struct d1_episode episodes[D1_MAX_EPISODES];
	struct d1_view views[D1_MAX_VIEWS];
	uint64_t next_custody;
	uint64_t next_repair;
	uint64_t next_postcond;
	uint64_t next_episode;
	/*
	 * The cross-DS completion certificate clear_error requires.  What
	 * issues one is outside D1 entirely, so a fixture stands in for
	 * that issuer and this is what it issued; the model only ever
	 * compares it.
	 */
	bool certificate_present;
	uint8_t certificate[D1_CERTIFICATE_BYTES];
	/*
	 * Whether this store's next reopen should fail to make its new
	 * START durable, and how.  See d1_store_reopen.
	 */
	uint32_t fail_reopen_start;

	/*
	 * Encoding scratch owned by this store and used only under its own
	 * lock.  Nothing is shared between stores, so two of them encoding
	 * at once cannot record each other's request identity.
	 */
	uint8_t *scratch;
	size_t scratch_cap;
	/* Record assembly, also this store's and also under this lock. */
	uint8_t *record;
	size_t record_cap;

	/*
	 * Set when an injected index fault has left a materialized pointer
	 * behind.  It is bookkeeping, not a switch: reads already use the
	 * reducer's state and always did.  What the flag records is that
	 * the materialized pointer and the reducer have diverged, so a
	 * test can check that the divergence changed no answer.
	 */
	bool overlay_active;
	bool fail_next_index;
	/* One-shot: the next journal snapshot finds no memory. */
	bool fail_next_snapshot;
	/*
	 * Standing arms: while set, the store answers as though it had no
	 * ID or no epoch left to give.  They are consulted at the refusal
	 * sites and nowhere else, so all they can produce is the refusal
	 * that state produces, and nothing derived from them reaches a
	 * counter, a result or the log.
	 */
	bool exhaust_ids;
	bool exhaust_epoch;
	/*
	 * The fixture's window on a gap a batch leaves between two of its
	 * members -- and, at ordinal zero, on the gap between deciding
	 * that its operation key is free and running the first member.
	 * A second caller occupies such a gap by being preempted in it;
	 * a test occupies one on purpose.  See d1_fixture_before_member.
	 */
	void (*before_member)(void *);
	void *before_member_arg;
	uint32_t before_member_ordinal;

	/*
	 * Calls admitted and not yet returned.  A call is not one lock
	 * interval: an ordinary batch releases the lock between members so
	 * the next one is revalidated, and a close that ran in that gap
	 * would free the store out from under the caller.  Section 6 asks
	 * for BUSY while views OR active calls exist, so both are counted.
	 */
	uint32_t active_calls;
	bool closed;

	struct d1_journal journal;
	/* The last LSN a rebuild consumed, so a reopen continues from it. */
	uint64_t replayed_lsn;
	bool journaling;
	/*
	 * Set only while rebuilding from a log.  A replayed record was
	 * admitted when it was written, so it is not re-authorised; it is
	 * re-applied.  Nothing is appended and no fault can fire.
	 */
	bool replaying;
	/*
	 * A rebuild that failed part way leaves state that is neither the
	 * logged history nor an empty store.  Returning an error is not
	 * enough: the handle must stop being usable for anything but
	 * teardown, or a caller could go on serving from it.
	 */
	bool poisoned;
};

static void d1_verifier_of(uint64_t incarnation, uint8_t out[D1_VERIFIER_BYTES])
{
	unsigned int i;

	for (i = 0; i < D1_VERIFIER_BYTES; i++)
		out[i] = (uint8_t)(incarnation >> (56 - 8 * i));
}

/*
 * Public observers of mutable store state take the lock, like the
 * others the last cycle locked.  The reducer uses d1_verifier_of and
 * s->incarnation directly, because it already holds it.
 *
 * A view's observers -- d1_view_version, d1_view_eof -- do not, and do
 * not need to: everything they read was captured under the lock when
 * the view opened and is immutable for its lifetime.  What a view does
 * not survive is the store being torn down under it, which is what
 * d1_store_close refuses to do.
 */
/*
 * Whether this handle may still answer for the model.
 *
 * Two ways it stops.  A rebuild that stopped part way left state that
 * is neither the logged history nor an empty store, and a reopen that
 * rebuilt and then failed to start its new journal left a store nothing
 * is recording; either way the handle is poisoned.  And a logical close
 * fences the store: it stops admitting calls, and what it promises is
 * that everything after it fails closed.
 *
 * "Serves nothing" has to mean the observers in both cases.  A version,
 * an EOF, a guard or a verifier read out of a history the model
 * rejected is precisely the value a caller would mistake for the state
 * of the store; and a value read after a close is read from a store the
 * caller has already given up, in the window before its owner destroys
 * it.  So every public observer answers as if it knew nothing, and the
 * fixture may change nothing, and only teardown remains.
 *
 * There is deliberately no diagnostic back door here.  A test that
 * wants to look at a partial reduction reads the state before it
 * poisons the handle, which is what the reduction is being compared
 * against anyway; a test that wants to look at a store reads it before
 * closing it.
 */
static bool d1_store_serving(const struct d1_store *s)
{
	return !s->poisoned && !s->closed;
}

void d1_store_verifier(struct d1_store *s, uint8_t out[D1_VERIFIER_BYTES])
{
	pthread_mutex_lock(&s->lock);
	if (d1_store_serving(s))
		d1_verifier_of(s->incarnation, out);
	else
		memset(out, 0, D1_VERIFIER_BYTES);
	pthread_mutex_unlock(&s->lock);
}

uint64_t d1_store_incarnation(struct d1_store *s)
{
	uint64_t incarnation;

	pthread_mutex_lock(&s->lock);
	incarnation = d1_store_serving(s) ? s->incarnation : 0;
	pthread_mutex_unlock(&s->lock);
	return incarnation;
}

/*
 * The next live store object's token.
 *
 * This names one live C object for as long as it exists, and nothing
 * else.  It is not a store identity -- the UUID is that, and it is what
 * a journal carries and a rebuild checks.  This exists because two live
 * objects may share a UUID: the model deliberately opens a source and a
 * pristine replay target of the same name at once, and they have
 * separate locks, tables and counters.  A handle from one of them must
 * not select a row in the other.
 *
 * Zero is reserved for "no live store", so a decoded handle that replay
 * has not adopted names nothing anywhere.  The counter only ever
 * ascends, so no token is reused while a handle carrying it can still
 * be presented, and open fails rather than wrap.  None of it is
 * durable: a token means nothing after this process ends, and an
 * in-memory handle does not survive that either.
 */
static pthread_mutex_t d1_instance_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t d1_next_instance = 1;

static uint64_t d1_instance_take(void)
{
	uint64_t token = 0;

	pthread_mutex_lock(&d1_instance_lock);
	if (d1_next_instance != 0)
		token = d1_next_instance++;
	pthread_mutex_unlock(&d1_instance_lock);
	return token;
}

struct d1_store *d1_store_open(const struct d1_uuid *store_uuid,
			       uint32_t chunk_bytes, uint64_t max_file_bytes)
{
	struct d1_store *s;
	uint64_t token;

	if (chunk_bytes < D1_CHUNK_BYTES_MIN ||
	    chunk_bytes > D1_CHUNK_BYTES_MAX)
		return NULL;
	if (max_file_bytes == 0)
		return NULL;
	/* No token, no store: a handle with no issuer names nothing. */
	token = d1_instance_take();
	if (!token)
		return NULL;
	s = calloc(1, sizeof(*s));
	if (!s)
		return NULL;
	s->scratch_cap = D1_ENVELOPE_MAX;
	s->scratch = calloc(1, s->scratch_cap);
	s->record_cap = D1_JOURNAL_RECORD_MAX;
	s->record = calloc(1, s->record_cap);
	if (!s->scratch || !s->record) {
		free(s->record);
		free(s->scratch);
		free(s);
		return NULL;
	}
	if (pthread_mutex_init(&s->lock, NULL) != 0) {
		free(s->record);
		free(s->scratch);
		free(s);
		return NULL;
	}
	s->instance = token;
	s->uuid = *store_uuid;
	s->chunk_bytes = chunk_bytes;
	s->max_file_bytes = max_file_bytes;
	/* The first incarnation is one; zero means no store has opened. */
	s->incarnation = 1;
	s->next_txn = 1;
	s->next_version = 1;
	s->next_admission = 1;
	s->next_custody = 1;
	s->next_repair = 1;
	s->next_postcond = 1;
	s->next_episode = 1;
	return s;
}

/*
 * Whether this store is still exactly as it was opened.
 *
 * Reconstruction begins empty, so a target that already holds state
 * cannot be reduced into: the history would be applied on top of data
 * it does not describe, and the result would be neither the logged
 * store nor the one that was there.  This is a precondition, checked
 * before anything is touched, rather than an attempt to replace an
 * active store -- a reconstruction target is exclusively owned by the
 * caller doing the reconstruction.
 */
static bool d1_store_pristine(const struct d1_store *s)
{
	uint32_t i;

	if (s->incarnation != 1u || s->index_epoch != 0u)
		return false;
	if (s->next_txn != 1u || s->next_version != 1u ||
	    s->next_admission != 1u || s->next_custody != 1u ||
	    s->next_repair != 1u || s->next_postcond != 1u ||
	    s->next_episode != 1u)
		return false;
	if (s->journaling || s->replayed_lsn)
		return false;
	for (i = 0; i < D1_MAX_OBJECTS; i++)
		if (s->objects[i].used)
			return false;
	for (i = 0; i < D1_MAX_ADMISSIONS; i++)
		if (s->admissions[i].used)
			return false;
	for (i = 0; i < D1_MAX_TXNS; i++)
		if (s->txns[i].used)
			return false;
	for (i = 0; i < D1_MAX_VERSIONS; i++)
		if (s->versions[i].used)
			return false;
	for (i = 0; i < D1_MAX_RECEIPTS; i++)
		if (s->receipts[i].used)
			return false;
	for (i = 0; i < D1_MAX_CUSTODY; i++)
		if (s->custody[i].used)
			return false;
	for (i = 0; i < D1_MAX_REPAIRS; i++)
		if (s->repairs[i].used)
			return false;
	for (i = 0; i < D1_MAX_POSTCONDS; i++)
		if (s->postconds[i].used)
			return false;
	for (i = 0; i < D1_MAX_EPISODES; i++)
		if (s->episodes[i].used)
			return false;
	for (i = 0; i < D1_MAX_OWNERS; i++)
		if (s->owners[i].used)
			return false;
	for (i = 0; i < D1_MAX_VIEWS; i++)
		if (s->views[i].used)
			return false;
	return true;
}

/*
 * Fixture control: park a call at the door.
 *
 * This is the one interval the store's own lock cannot cover: a caller
 * that has entered a public function and has not yet acquired s->lock.
 * A close that destroyed the store there would leave that caller to
 * lock freed memory, so the arm that opens the interval has to live
 * outside the storage whose lifetime is in question -- which is why the
 * storage is here, with a mutex of its own, rather than a field of the
 * store.
 *
 * Out of the store is not the same as belonging to no store.  The arm
 * names its target, and only a call entering that store takes it: an
 * arm left over from another store or another run is not something an
 * unrelated call should be made to run, and its argument may not even
 * be alive any more.  It is one-shot -- taken and cleared before it
 * runs, so a hook that makes calls of its own does not re-enter itself
 * -- and it runs with neither lock held.
 *
 * Lock order, everywhere: the store's lock first, then this one.  The
 * runner takes only this one and has let it go before d1_call_enter
 * reaches for the store's, so the two are never held the other way
 * round.
 */
static pthread_mutex_t d1_admit_hook_lock = PTHREAD_MUTEX_INITIALIZER;
static const struct d1_store *d1_admit_hook_target;
static void (*d1_admit_hook)(void *);
static void *d1_admit_hook_arg;

/* With the hook lock held. */
static void d1_admit_hook_drop(void)
{
	d1_admit_hook_target = NULL;
	d1_admit_hook = NULL;
	d1_admit_hook_arg = NULL;
}

/*
 * Forget an arm aimed at @s.  One place, used by every path that ends a
 * store's life or its run: recovery entry, logical close, and the
 * teardown that both ordinary destruction and a crash go through.
 */
static void d1_admit_hook_forget(const struct d1_store *s)
{
	pthread_mutex_lock(&d1_admit_hook_lock);
	if (d1_admit_hook_target == s)
		d1_admit_hook_drop();
	pthread_mutex_unlock(&d1_admit_hook_lock);
}

static void d1_run_admit_hook(const struct d1_store *s)
{
	void (*fn)(void *) = NULL;
	void *arg = NULL;

	pthread_mutex_lock(&d1_admit_hook_lock);
	if (d1_admit_hook_target == s) {
		fn = d1_admit_hook;
		arg = d1_admit_hook_arg;
		d1_admit_hook_drop();
	}
	pthread_mutex_unlock(&d1_admit_hook_lock);
	if (fn)
		fn(arg);
}

/*
 * Admit a call, unless the store has been closed.
 *
 * A call is bracketed rather than a lock interval, because a call is
 * not a lock interval: an ordinary batch deliberately releases the lock
 * between members.
 *
 * The bracket starts here, which is after the caller entered the public
 * function: a close may win the race with a caller that has not reached
 * this lock.  That is why a close does not destroy the store -- see
 * d1_store_close.
 */
static bool d1_call_enter(struct d1_store *s)
{
	bool admitted;

	d1_run_admit_hook(s);
	pthread_mutex_lock(&s->lock);
	admitted = !s->closed && !s->poisoned;
	if (admitted)
		s->active_calls++;
	pthread_mutex_unlock(&s->lock);
	return admitted;
}

static void d1_call_leave(struct d1_store *s)
{
	pthread_mutex_lock(&s->lock);
	if (s->active_calls)
		s->active_calls--;
	pthread_mutex_unlock(&s->lock);
}

/*
 * Logical close: fence the store, and do not destroy it.
 *
 * A view's bytes live in the store, and so does the state an admitted
 * call is part way through; closing under either is not a close, it is
 * a use-after-free with a friendly name.  Both are refused.
 *
 * Close admission and call admission are settled under the same lock,
 * so a call cannot be admitted after the close has decided to proceed
 * and a close cannot proceed after a call has been admitted.  That is
 * the whole of what the lock settles, and it is less than it looks: a
 * caller which has entered a public function but has not yet reached
 * d1_call_enter's lock is counted nowhere, and a close which freed the
 * store would leave it to lock destroyed memory.  The ownership
 * protocol therefore has to begin outside the storage it protects, and
 * the smallest way to do that is not to free here at all.
 *
 * So this returns with the allocation intact and every later call
 * failing closed.  d1_store_destroy is where the memory goes, and its
 * precondition -- the owner has excluded and joined every caller -- is
 * one only the owner can state.  Closing twice is not an error.
 */
uint32_t d1_store_close(struct d1_store *s)
{
	uint32_t i;

	if (!s)
		return D1_OK;
	pthread_mutex_lock(&s->lock);
	if (s->active_calls) {
		pthread_mutex_unlock(&s->lock);
		return D1_BUSY;
	}
	for (i = 0; i < D1_MAX_VIEWS; i++) {
		if (s->views[i].used) {
			pthread_mutex_unlock(&s->lock);
			return D1_BUSY;
		}
	}
	s->closed = true;
	d1_admit_hook_forget(s);
	pthread_mutex_unlock(&s->lock);
	return D1_OK;
}

/*
 * Ordinary destruction, after a successful close.
 *
 * What this can check, it checks: a store which was never closed is
 * still admitting calls, so destroying it is refused.  What it cannot
 * check is the caller that is between the public function's first
 * instruction and d1_call_enter's lock, because nothing in the store
 * knows about it yet.  That one is the owner's to exclude and join,
 * and stating it is the reason this boundary has a name of its own.
 */
uint32_t d1_store_destroy(struct d1_store *s)
{
	bool ready;

	if (!s)
		return D1_OK;
	pthread_mutex_lock(&s->lock);
	ready = s->closed && !s->active_calls;
	pthread_mutex_unlock(&s->lock);
	if (!ready)
		return D1_BUSY;
	d1_store_free(s);
	return D1_OK;
}

/*
 * Crash teardown: the whole simulated world goes, views included, as a
 * power loss would take it.  A crash is not a normal close, and nothing
 * may hold a view across this and then read.
 */
void d1_store_free(struct d1_store *s)
{
	if (!s)
		return;
	d1_admit_hook_forget(s);
	d1_journal_fini(&s->journal);
	pthread_mutex_destroy(&s->lock);
	free(s->record);
	free(s->scratch);
	free(s);
}

void d1_fixture_fail_next_append(struct d1_store *s)
{
	pthread_mutex_lock(&s->lock);
	if (!d1_store_serving(s)) {
		pthread_mutex_unlock(&s->lock);
		return;
	}
	/* Faults are disabled throughout recovery, by construction. */
	if (!s->replaying)
		s->journal.fail_append_in = 1u;
	pthread_mutex_unlock(&s->lock);
}

void d1_fixture_fail_append_in(struct d1_store *s, uint32_t n)
{
	pthread_mutex_lock(&s->lock);
	if (!d1_store_serving(s)) {
		pthread_mutex_unlock(&s->lock);
		return;
	}
	if (!s->replaying)
		s->journal.fail_append_in = n;
	pthread_mutex_unlock(&s->lock);
}

/*
 * Arm the door hook for this store.
 *
 * There is one slot for the process, because the interval it opens is
 * the one the store's own lock cannot cover and the arm has to outlive
 * the store; a registry of them would be a framework this model does
 * not need.  One slot has to say so, though, rather than let the second
 * caller quietly take the first caller's arm away: arming while another
 * store holds the slot is D1_BUSY and leaves that store's arm exactly
 * as it was.  A store may replace its own callback as often as it
 * likes, and a null @fn disarms this store's arm and no other -- an arm
 * aimed at another store is not this caller's to drop.
 *
 * The gates are the ones every other fixture arm obeys: not a closed or
 * poisoned store, and not during recovery.
 */
uint32_t d1_fixture_before_admission(struct d1_store *s, void (*fn)(void *),
				     void *arg)
{
	uint32_t status;

	pthread_mutex_lock(&s->lock);
	if (!d1_store_serving(s) || s->replaying) {
		pthread_mutex_unlock(&s->lock);
		return D1_INVALID;
	}
	pthread_mutex_lock(&d1_admit_hook_lock);
	if (!fn) {
		if (d1_admit_hook_target == s)
			d1_admit_hook_drop();
		status = D1_OK;
	} else if (d1_admit_hook_target && d1_admit_hook_target != s) {
		status = D1_BUSY;
	} else {
		d1_admit_hook_target = s;
		d1_admit_hook = fn;
		d1_admit_hook_arg = arg;
		status = D1_OK;
	}
	pthread_mutex_unlock(&d1_admit_hook_lock);
	pthread_mutex_unlock(&s->lock);
	return status;
}

void d1_fixture_before_member(struct d1_store *s, uint32_t ordinal,
			      void (*fn)(void *), void *arg)
{
	pthread_mutex_lock(&s->lock);
	if (!d1_store_serving(s)) {
		pthread_mutex_unlock(&s->lock);
		return;
	}
	if (!s->replaying) {
		s->before_member = fn;
		s->before_member_arg = arg;
		s->before_member_ordinal = ordinal;
	}
	pthread_mutex_unlock(&s->lock);
}

/*
 * Run the armed hook, once, with the lock not held -- which is the
 * state a preempted caller would leave the store in, and which lets
 * the hook make an ordinary call of its own.
 *
 * A batch that stops before @ordinal never reaches this, which is
 * itself an oracle: a test can arm a later member and prove the batch
 * did not get there.
 */
static void d1_run_member_hook(struct d1_store *s, uint32_t ordinal)
{
	void (*fn)(void *) = NULL;
	void *arg = NULL;

	pthread_mutex_lock(&s->lock);
	if (s->before_member && s->before_member_ordinal == ordinal) {
		fn = s->before_member;
		arg = s->before_member_arg;
		s->before_member = NULL;
		s->before_member_arg = NULL;
	}
	pthread_mutex_unlock(&s->lock);
	if (fn)
		fn(arg);
}

/*
 * Arm the next reopen's START to fail.
 *
 * The other arms describe operations and are cleared by reduction; this
 * one describes the reopen transition, which is why it is not.  It is
 * one-shot and the transition consumes it, so it cannot reach anything
 * the store does afterwards -- and a read-only rebuild drops it, since
 * that handle will never reach a START at all.
 */
void d1_fixture_fail_reopen_start(struct d1_store *s, uint32_t which)
{
	pthread_mutex_lock(&s->lock);
	if (!d1_store_serving(s) || s->replaying) {
		pthread_mutex_unlock(&s->lock);
		return;
	}
	if (which == D1_REOPEN_START_APPEND || which == D1_REOPEN_START_FLUSH ||
	    which == D1_REOPEN_START_OK)
		s->fail_reopen_start = which;
	pthread_mutex_unlock(&s->lock);
}

void d1_fixture_fail_next_flush(struct d1_store *s)
{
	pthread_mutex_lock(&s->lock);
	if (!d1_store_serving(s)) {
		pthread_mutex_unlock(&s->lock);
		return;
	}
	if (!s->replaying)
		s->journal.fail_next_flush = true;
	pthread_mutex_unlock(&s->lock);
}

/*
 * The durable length is what a reader gets, not the append length.  A
 * test simulating a crash hands replay a smaller one.
 */
/*
 * Hand back the durable journal, as bytes the caller owns.
 *
 * The old form of this returned the store's own buffer and a length,
 * and it was the one public observer that never took the lock.  No
 * amount of locking could have saved it: the pointer it handed out
 * stayed interesting after the lock was dropped, and the next append
 * may reallocate the buffer underneath it.  A reader beside an
 * appending call could hold a pointer realloc was about to move, a
 * length from the other side of a flush, or read the fence while a
 * close was writing it.
 *
 * So the bytes are copied under the lock and the copy belongs to the
 * caller, who releases it with free().  What comes back is a value, not
 * a window: it does not change when the store does, and it outlives the
 * close and the destruction of the store it came from.  A live store
 * with an empty journal is D1_OK with a length of zero and no
 * allocation, and free(NULL) is a no-op, so a caller's release path
 * does not have to know which it got.
 */
uint32_t d1_store_journal_snapshot(struct d1_store *s, uint8_t **out,
				   size_t *len)
{
	uint8_t *copy = NULL;
	bool starved;
	size_t n;

	*out = NULL;
	*len = 0;
	pthread_mutex_lock(&s->lock);
	if (!d1_store_serving(s)) {
		pthread_mutex_unlock(&s->lock);
		return D1_INVALID;
	}
	/*
	 * The arm says the next snapshot, and means the next one: it is
	 * taken and spent here, before the length is looked at, so an
	 * empty journal consumes it exactly as a full one does.  Leaving
	 * it pending across a zero-byte snapshot would have made the
	 * header's "next" mean "next one that had something in it", which
	 * is a different promise and not the one it makes.
	 */
	starved = s->fail_next_snapshot;
	s->fail_next_snapshot = false;
	if (starved) {
		pthread_mutex_unlock(&s->lock);
		return D1_NOSPC;
	}
	n = s->journal.durable;
	if (n) {
		copy = malloc(n);
		if (!copy) {
			/* Nothing of the store moved, so ask again. */
			pthread_mutex_unlock(&s->lock);
			return D1_NOSPC;
		}
		memcpy(copy, s->journal.buf, n);
	}
	pthread_mutex_unlock(&s->lock);
	*out = copy;
	*len = n;
	return D1_OK;
}

static struct d1_object *d1_object_find(struct d1_store *s,
					const struct d1_objkey *key)
{
	uint32_t i;

	for (i = 0; i < D1_MAX_OBJECTS; i++) {
		struct d1_object *o = &s->objects[i];

		if (o->used && memcmp(&o->key, key, sizeof(*key)) == 0)
			return o;
	}
	return NULL;
}

static struct d1_object *d1_object_get(struct d1_store *s,
				       const struct d1_objkey *key)
{
	struct d1_object *o = d1_object_find(s, key);
	uint32_t i, c;

	if (o)
		return o;
	for (i = 0; i < D1_MAX_OBJECTS; i++) {
		if (s->objects[i].used)
			continue;
		o = &s->objects[i];
		memset(o, 0, sizeof(*o));
		o->used = true;
		o->key = *key;
		for (c = 0; c < D1_MAX_CHUNKS; c++)
			o->chunks[c].guard.never_written = true;
		return o;
	}
	return NULL;
}

static uint32_t d1_object_slot(const struct d1_store *s,
			       const struct d1_object *o)
{
	return (uint32_t)(o - s->objects);
}

/*
 * Turn one of this store's counter values into a handle of this store.
 *
 * These four are the only places inside the store where a raw number
 * becomes a bound handle: they stamp the domain, so a resolver knows
 * which table the value may name, and this object's token, so it knows
 * whose table.  Replay uses the same constructors to adopt a decoded
 * log's values for the store it is rebuilding, which is what makes a
 * reconstructed handle the rebuilding store's own.
 *
 * They are not the only way a caller reaches them.  The four public
 * d1_fixture_*_handle constructors pass a caller's raw number straight
 * through, deliberately: on the wire a handle is a bare u64, so a
 * client can always name any number for the store it is talking to, and
 * the fixture models exactly that and nothing more.  What it cannot do
 * is name another store's row or another domain's table.
 */
static d1_admission_id d1_admission_of(const struct d1_store *s, uint64_t raw)
{
	d1_admission_id id = d1_admission_none();

	if (raw) {
		id.raw = raw;
		id._kind = D1_HANDLE_ADMISSION;
		id._instance = s->instance;
	}
	return id;
}

static d1_txn_id d1_txn_of(const struct d1_store *s, uint64_t raw)
{
	d1_txn_id id = d1_txn_none();

	if (raw) {
		id.raw = raw;
		id._kind = D1_HANDLE_TXN;
		id._instance = s->instance;
	}
	return id;
}

static d1_version_id d1_version_of(const struct d1_store *s, uint64_t raw)
{
	d1_version_id id = d1_version_none();

	if (raw) {
		id.raw = raw;
		id._kind = D1_HANDLE_VERSION;
		id._instance = s->instance;
	}
	return id;
}

static d1_custody_id d1_custody_of(const struct d1_store *s, uint64_t raw)
{
	d1_custody_id id = d1_custody_none();

	if (raw) {
		id.raw = raw;
		id._kind = D1_HANDLE_CUSTODY;
		id._instance = s->instance;
	}
	return id;
}

/*
 * Whether a handle is one this live store issued or adopted, of the
 * domain the caller is presenting it as.
 *
 * The C type settles which parameter a value may be passed as, and the
 * compiler enforces that much.  It does not survive a copy of the
 * bytes, so the kind is asked again here; and a durable store name does
 * not tell two live objects of that name apart, so the token is asked
 * too.  A handle another live store issued names nothing here, even
 * when both have counted to the same number and carry the same UUID,
 * and a value nothing issued -- a decoder's output replay has not
 * adopted, or a literal with no kind at all -- names nothing either.
 */
static bool d1_handle_ours(const struct d1_store *s, uint64_t raw,
			   uint32_t kind, uint64_t instance, uint32_t want)
{
	return raw != 0 && kind == want && instance != 0 &&
	       instance == s->instance;
}

/*
 * A handle names a row here only if this store issued or adopted it.
 */
static struct d1_admission *d1_admission_find(struct d1_store *s,
					      d1_admission_id id)
{
	uint32_t i;

	if (!d1_handle_ours(s, id.raw, id._kind, id._instance,
			    D1_HANDLE_ADMISSION))
		return NULL;
	for (i = 0; i < D1_MAX_ADMISSIONS; i++)
		if (s->admissions[i].used && s->admissions[i].id == id.raw)
			return &s->admissions[i];
	return NULL;
}

static d1_repair_id d1_repair_of(const struct d1_store *s, uint64_t raw)
{
	d1_repair_id id = d1_repair_none();

	if (raw) {
		id.raw = raw;
		id._kind = D1_HANDLE_REPAIR;
		id._instance = s->instance;
	}
	return id;
}

static struct d1_custody *d1_custody_find(struct d1_store *s, d1_custody_id id)
{
	uint32_t i;

	if (!d1_handle_ours(s, id.raw, id._kind, id._instance,
			    D1_HANDLE_CUSTODY))
		return NULL;
	for (i = 0; i < D1_MAX_CUSTODY; i++)
		if (s->custody[i].used && s->custody[i].id == id.raw)
			return &s->custody[i];
	return NULL;
}

static struct d1_repair *d1_repair_find(struct d1_store *s, d1_repair_id id)
{
	uint32_t i;

	if (!d1_handle_ours(s, id.raw, id._kind, id._instance,
			    D1_HANDLE_REPAIR))
		return NULL;
	for (i = 0; i < D1_MAX_REPAIRS; i++)
		if (s->repairs[i].used && s->repairs[i].id == id.raw)
			return &s->repairs[i];
	return NULL;
}

static d1_postcond_id d1_postcond_of(const struct d1_store *s, uint64_t raw)
{
	d1_postcond_id id = d1_postcond_none();

	if (raw) {
		id.raw = raw;
		id._kind = D1_HANDLE_POSTCOND;
		id._instance = s->instance;
	}
	return id;
}

static struct d1_postcond *d1_postcond_find(struct d1_store *s,
					    d1_postcond_id id)
{
	uint32_t i;

	if (!d1_handle_ours(s, id.raw, id._kind, id._instance,
			    D1_HANDLE_POSTCOND))
		return NULL;
	for (i = 0; i < D1_MAX_POSTCONDS; i++)
		if (s->postconds[i].used && s->postconds[i].id == id.raw)
			return &s->postconds[i];
	return NULL;
}

static struct d1_postcond *d1_postcond_spare(struct d1_store *s)
{
	uint32_t i;

	for (i = 0; i < D1_MAX_POSTCONDS; i++)
		if (!s->postconds[i].used)
			return &s->postconds[i];
	return NULL;
}

static d1_episode_id d1_episode_of(const struct d1_store *s, uint64_t raw)
{
	d1_episode_id id = d1_episode_none();

	if (raw) {
		id.raw = raw;
		id._kind = D1_HANDLE_EPISODE;
		id._instance = s->instance;
	}
	return id;
}

static struct d1_episode *d1_episode_find(struct d1_store *s, d1_episode_id id)
{
	uint32_t i;

	if (!d1_handle_ours(s, id.raw, id._kind, id._instance,
			    D1_HANDLE_EPISODE))
		return NULL;
	for (i = 0; i < D1_MAX_EPISODES; i++)
		if (s->episodes[i].used && s->episodes[i].id == id.raw)
			return &s->episodes[i];
	return NULL;
}

static struct d1_episode *d1_episode_spare(struct d1_store *s)
{
	uint32_t i;

	for (i = 0; i < D1_MAX_EPISODES; i++)
		if (!s->episodes[i].used)
			return &s->episodes[i];
	return NULL;
}

static struct d1_txn *d1_txn_find(struct d1_store *s, d1_txn_id id)
{
	uint32_t i;

	if (!d1_handle_ours(s, id.raw, id._kind, id._instance, D1_HANDLE_TXN))
		return NULL;
	for (i = 0; i < D1_MAX_TXNS; i++)
		if (s->txns[i].used && s->txns[i].id == id.raw)
			return &s->txns[i];
	return NULL;
}

static struct d1_version *d1_version_find(struct d1_store *s, d1_version_id id)
{
	uint32_t i;

	if (!d1_handle_ours(s, id.raw, id._kind, id._instance,
			    D1_HANDLE_VERSION))
		return NULL;
	for (i = 0; i < D1_MAX_VERSIONS; i++)
		if (s->versions[i].used && s->versions[i].id == id.raw)
			return &s->versions[i];
	return NULL;
}

static bool d1_owner_same(const struct d1_owner *a, const struct d1_owner *b)
{
	return a->cohort.raw == b->cohort.raw && a->writer == b->writer &&
	       a->co_id == b->co_id;
}

static struct d1_owner_assoc *d1_owner_find(struct d1_store *s,
					    const struct d1_uuid *export_uuid,
					    const struct d1_owner *owner)
{
	uint32_t i;

	for (i = 0; i < D1_MAX_OWNERS; i++) {
		struct d1_owner_assoc *a = &s->owners[i];

		if (!a->used)
			continue;
		if (memcmp(&a->export_uuid, export_uuid,
			   sizeof(*export_uuid)) != 0)
			continue;
		if (d1_owner_same(&a->owner, owner))
			return a;
	}
	return NULL;
}

/*
 * A free association row, and the binding taken in it.
 *
 * They are two calls rather than one because a caller that takes a row
 * has to record the row's before-image for its undo before the row is
 * written: an undo that saved an already-overwritten row would restore
 * the binding it was meant to release.
 */
static struct d1_owner_assoc *d1_owner_spare(struct d1_store *s)
{
	uint32_t i;

	for (i = 0; i < D1_MAX_OWNERS; i++)
		if (!s->owners[i].used)
			return &s->owners[i];
	return NULL;
}

static void d1_owner_bind(struct d1_owner_assoc *a,
			  const struct d1_uuid *export_uuid,
			  const struct d1_owner *owner)
{
	memset(a, 0, sizeof(*a));
	a->used = true;
	a->export_uuid = *export_uuid;
	a->owner = *owner;
}

/*
 * Publish a new visible version for one chunk.
 *
 * The reducer's state always moves.  The materialized index moves with
 * it unless a fault is armed, in which case it is left behind.  Nothing
 * has to switch over, because nothing reads the materialized pointer:
 * the memo's requirement that no read serves the old pointer against a
 * new successful COMMIT receipt is met by construction here, and the
 * fault exists so a test can demonstrate that rather than assume it.
 */
static void d1_publish_visible(struct d1_store *s, struct d1_chunk *c,
			       uint64_t version)
{
	c->visible_present = true;
	c->visible = version;
	if (s->fail_next_index && !s->replaying) {
		s->fail_next_index = false;
		c->index_stale = true;
		s->overlay_active = true;
		return;
	}
	c->materialized_present = true;
	c->materialized = version;
}

/* What a read sees: the reducer's state, which is the authority. */
static bool d1_chunk_visible(const struct d1_store *s, const struct d1_chunk *c,
			     d1_version_id *version)
{
	if (!c->visible_present)
		return false;
	*version = d1_version_of(s, c->visible);
	return true;
}

static bool d1_visible_locked(struct d1_store *s,
			      const struct d1_objkey *object, uint64_t index,
			      d1_version_id *version)
{
	const struct d1_object *o = d1_object_find(s, object);

	if (!o || index >= D1_MAX_CHUNKS)
		return false;
	return d1_chunk_visible(s, &o->chunks[index], version);
}

static bool d1_guard_locked(struct d1_store *s, const struct d1_objkey *object,
			    uint64_t index, struct d1_guard *guard)
{
	const struct d1_object *o = d1_object_find(s, object);

	if (!o || index >= D1_MAX_CHUNKS)
		return false;
	*guard = o->chunks[index].guard;
	return true;
}

/*
 * EOF is the maximum end of the intervals the visible versions
 * contribute, and zero when none does.  It is derived here rather than
 * stored, so a rollback that restores a shorter version cannot leave a
 * stale high-water mark behind.
 */
static uint64_t d1_eof_locked(struct d1_store *s,
			      const struct d1_objkey *object)
{
	const struct d1_object *o = d1_object_find(s, object);
	uint64_t eof = 0;
	uint32_t i;

	if (!o)
		return 0;
	for (i = 0; i < D1_MAX_CHUNKS; i++) {
		const struct d1_version *v;
		uint64_t start, end;

		if (!o->chunks[i].visible_present)
			continue;
		v = d1_version_find(s, d1_version_of(s, o->chunks[i].visible));
		if (!v)
			continue;
		if (!d1_mul_u64(i, s->chunk_bytes, &start))
			continue;
		if (!d1_add_u64(start, v->len, &end))
			continue;
		if (end > eof)
			eof = end;
	}
	return eof;
}

/*
 * Receipt lookup.  An exact match returns the recorded result without
 * mutating anything, even across an incarnation change; the same key
 * with a different body is a conflict and never a new transition.
 */
/*
 * Whether this receipt belongs to that export and operation key.  The
 * entry ordinal is deliberately not part of the question: three callers
 * want the key itself, and only one of them also cares which member.
 */
static bool d1_receipt_keyed(const struct d1_receipt *r,
			     const struct d1_uuid *export_uuid,
			     const struct d1_opkey *key)
{
	if (!r->used)
		return false;
	if (memcmp(&r->export_uuid, export_uuid, sizeof(*export_uuid)) != 0)
		return false;
	return memcmp(&r->key.origin, &key->origin, sizeof(key->origin)) == 0 &&
	       r->key.sequence == key->sequence &&
	       r->key.ordinal == key->ordinal;
}

static struct d1_receipt *d1_receipt_find(struct d1_store *s,
					  const struct d1_uuid *export_uuid,
					  const struct d1_opkey *key,
					  uint32_t ordinal)
{
	uint32_t i;

	for (i = 0; i < D1_MAX_RECEIPTS; i++) {
		struct d1_receipt *r = &s->receipts[i];

		if (r->ordinal == ordinal &&
		    d1_receipt_keyed(r, export_uuid, key))
			return r;
	}
	return NULL;
}

static uint32_t d1_holes_locked(struct d1_store *s,
				const struct d1_objkey *object,
				struct d1_interval *out, uint32_t max)
{
	const struct d1_object *o = d1_object_find(s, object);
	uint64_t eof = d1_eof_locked(s, object);
	uint64_t at = 0;
	uint32_t n = 0;
	uint32_t i;

	if (!o || !eof)
		return 0;
	/*
	 * Chunks are walked in index order, so the intervals they
	 * contribute are already sorted and the complement is whatever
	 * lies between them, up to EOF and not past it.
	 */
	for (i = 0; i < D1_MAX_CHUNKS; i++) {
		const struct d1_version *v;
		uint64_t start, end;

		if (!o->chunks[i].visible_present)
			continue;
		v = d1_version_find(s, d1_version_of(s, o->chunks[i].visible));
		if (!v)
			continue;
		if (!d1_mul_u64(i, s->chunk_bytes, &start) ||
		    !d1_add_u64(start, v->len, &end))
			continue;
		if (start > at) {
			if (n >= max)
				return n;
			out[n].start = at;
			out[n].end = start;
			n++;
		}
		if (end > at)
			at = end;
	}
	return n;
}

/* Whether @a may act on @object at all, and with the rights it needs. */
static uint32_t d1_admission_check(struct d1_store *s,
				   const struct d1_envelope *env, uint32_t need,
				   struct d1_admission **out)
{
	/*
	 * Replay uses the real authority table, rebuilt from the same log.
	 * Nothing is fabricated: an admission that was revoked, expired or
	 * fenced when the entry ran is revoked, expired or fenced again,
	 * so re-execution reaches the same answer for the same reason.
	 */
	struct d1_admission *a = d1_admission_find(s, env->admission);
	if (!a || a->revoked || a->expired)
		return D1_STALE_AUTH;
	if (memcmp(&a->object, &env->object, sizeof(a->object)) != 0)
		return D1_STALE_AUTH;
	if ((a->rights & need) != need)
		return D1_STALE_AUTH;
	/* A mutation belongs to the incarnation its admission was made in. */
	if (a->incarnation != s->incarnation)
		return D1_STALE_AUTH;
	if (env->incarnation != s->incarnation)
		return D1_STALE_AUTH;
	*out = a;
	return D1_OK;
}

static bool d1_chunk_empty(struct d1_store *s __attribute__((unused)),
			   const struct d1_chunk *c)
{
	return !c->visible_present && !c->pending_present;
}

/* The activation table of the memo, as a predicate. */
/*
 * Whether asking for activation is a request this grant may make.
 *
 * This half depends on the request and the grant alone -- a
 * multi-writer request may not ask for activation at all -- so it is
 * asked before anything is looked up.  Whether the activation actually
 * happens depends on the chunk, and is the other half below.
 */
static bool d1_activation_asked_wrongly(bool single_writer, bool flag)
{
	return flag && !single_writer;
}

static bool d1_activation_allowed(bool flag, uint32_t stability, bool empty)
{
	if (!flag || stability == D1_UNSTABLE)
		return false;
	return empty;
}

/*
 * Everything one entry changed, so it can be put back exactly.
 *
 * The contract is that a failure to make an event durable leaves no
 * state change, no consumed ID and no receipt.  An entry runs inside
 * one lock interval, so nothing can observe what happens between its
 * first mutation and its durable event; recording the before-state and
 * restoring it on failure is therefore observationally identical to
 * computing a candidate and publishing it only after the frontier, and
 * it keeps the transition logic in one place instead of two.
 */
struct d1_undo {
	struct d1_chunk *chunk;
	struct d1_chunk chunk_before;
	struct d1_txn *txn;
	uint32_t txn_phase_before;
	uint64_t txn_admission_before;
	struct d1_object *fresh_object;
	struct d1_txn *fresh_txn;
	struct d1_version *fresh_version;
	struct d1_owner_assoc *fresh_assoc;
	struct d1_postcond *fresh_postcond;
	/*
	 * Whether what this entry did survives its own error status.
	 * Section 7's NO_PREDECESSOR rollback is the one entry that says
	 * yes: the postcondition it records is the result it returned,
	 * and section 4 hands the caller a handle to it.  Everything else
	 * a refused entry touched goes back; see d1_repair_undo, which
	 * says the same for the cohort abort.
	 */
	bool keep;
	uint64_t epoch_before;
	uint64_t next_txn_before;
	uint64_t next_version_before;
	uint64_t next_postcond_before;
	/*
	 * The injected index fault is consumed, and the overlay flag set,
	 * at publication.  They are unjournalled harness state, so they
	 * have no replay effect -- but "the before-state restored exactly"
	 * has to include them, or the claim is wrong in a small way.
	 */
	bool fail_index_before;
	bool overlay_before;
};

static void d1_undo_begin(struct d1_store *s, struct d1_undo *u)
{
	memset(u, 0, sizeof(*u));
	u->epoch_before = s->index_epoch;
	u->next_txn_before = s->next_txn;
	u->next_version_before = s->next_version;
	u->next_postcond_before = s->next_postcond;
	u->fail_index_before = s->fail_next_index;
	u->overlay_before = s->overlay_active;
}

/* Record a chunk's state before the first change to it. */
static void d1_undo_chunk(struct d1_undo *u, struct d1_chunk *c)
{
	if (!u->chunk) {
		u->chunk = c;
		u->chunk_before = *c;
	}
}

/* Record a transaction's phase and admission before the first change. */
static void d1_undo_txn(struct d1_undo *u, struct d1_txn *t)
{
	if (!u->txn) {
		u->txn = t;
		u->txn_phase_before = t->phase;
		u->txn_admission_before = t->admission;
	}
}

static void d1_undo_apply(struct d1_store *s, struct d1_undo *u)
{
	if (u->chunk)
		*u->chunk = u->chunk_before;
	if (u->txn) {
		u->txn->phase = u->txn_phase_before;
		u->txn->admission = u->txn_admission_before;
	}
	/* A refused entry consumes no durable ID that replay would not. */
	if (u->fresh_object)
		u->fresh_object->used = false;
	if (u->fresh_txn)
		u->fresh_txn->used = false;
	if (u->fresh_version)
		u->fresh_version->used = false;
	if (u->fresh_assoc)
		u->fresh_assoc->used = false;
	if (u->fresh_postcond)
		u->fresh_postcond->used = false;
	s->next_postcond = u->next_postcond_before;
	s->index_epoch = u->epoch_before;
	s->next_txn = u->next_txn_before;
	s->next_version = u->next_version_before;
	s->fail_next_index = u->fail_index_before;
	s->overlay_active = u->overlay_before;
}

/*
 * The counters as the exhaustion checks see them.
 *
 * An arm makes the check see the exhausted value, and reaches nothing
 * else: these three are read by the checks and by nothing that issues
 * an ID or publishes an epoch.  So an armed store answers exactly as an
 * exhausted one does, its counters are untouched, and the comparison
 * the production path makes is the comparison the tests drive -- rather
 * than a second condition beside it that the tests reach instead.
 */
static uint64_t d1_next_txn_seen(const struct d1_store *s)
{
	return s->exhaust_ids ? UINT64_MAX : s->next_txn;
}

static uint64_t d1_next_version_seen(const struct d1_store *s)
{
	return s->exhaust_ids ? UINT64_MAX : s->next_version;
}

static uint64_t d1_next_postcond_seen(const struct d1_store *s)
{
	return s->exhaust_ids ? UINT64_MAX : s->next_postcond;
}

static uint64_t d1_next_episode_seen(const struct d1_store *s)
{
	return s->exhaust_ids ? UINT64_MAX : s->next_episode;
}

static uint64_t d1_index_epoch_seen(const struct d1_store *s)
{
	return s->exhaust_epoch ? UINT64_MAX : s->index_epoch;
}

/*
 * The guard of the chunk an owner is already bound to.
 *
 * An owner conflict is not about the chunk the refused request named:
 * it is about the one the owner already belongs to, which is where the
 * caller's own earlier write went.  Section 5 gives the result the
 * current CAS guard, and telling the caller about the chunk it was
 * refused *for* rather than the one it is bound *to* says nothing it
 * could act on -- a never-written chunk it may not have, instead of the
 * live generation of the chunk it does.
 *
 * The association's coordinates are the store's own, not a caller's, so
 * they are checked here rather than trusted: an association that named
 * a slot outside the tables would be an internal fault, and reading
 * past them would turn it into a worse one.  A conflict whose
 * coordinates do not hold up answers with the initial guard, which is
 * what a caller learns nothing false from.
 */
static struct d1_guard d1_owner_guard(const struct d1_store *s,
				      const struct d1_owner_assoc *assoc)
{
	const struct d1_object *o;
	struct d1_guard guard;

	memset(&guard, 0, sizeof(guard));
	guard.never_written = true;
	if (assoc->object >= D1_MAX_OBJECTS || assoc->index >= D1_MAX_CHUNKS)
		return guard;
	o = &s->objects[assoc->object];
	if (!o->used)
		return guard;
	return o->chunks[assoc->index].guard;
}

/*
 * Whether an ordinary operation may touch this chunk at all.
 *
 * Section 7 quarantines a version an ERROR episode names: from the
 * mark until the clear and unlock that end the episode, no ordinary
 * write, finalize, commit or rollback may replace it.  A mark that did
 * not quarantine was worse than no mark: an ordinary writer replaced
 * the version the episode was bound to, and the episode then named a
 * version the chunk no longer held -- unrepairable, because a repair
 * opens over the version it was marked for, and uncloseable, because a
 * clear needs a repair to have committed.
 *
 * A repair that holds a chunk blocks ordinary writers for the same
 * reason and gives the same answer, and so does a member a repair has
 * published and not yet unlocked.  The three are one condition to a
 * caller -- somebody else owns this chunk until they are finished with
 * it -- and one status says so.
 */
static uint32_t d1_chunk_writable(const struct d1_chunk *c)
{
	if (c->error_present || c->repair_present || c->repair_locked)
		return D1_QUARANTINED;
	return D1_OK;
}

/*
 * Whether an ordinary read may see this chunk.
 *
 * A quarantined version is not readable until its episode is cleared.
 * A clear makes the members readable and leaves them locked, which is
 * why this asks a narrower question than d1_chunk_writable: a repair
 * that holds a chunk without an episode has not put its data in doubt.
 */
static bool d1_chunk_readable(const struct d1_chunk *c)
{
	return !c->error_present || c->error_cleared;
}

static uint32_t
d1_do_write_entry(struct d1_store *s, const struct d1_envelope *env,
		  const struct d1_write_entry *e, struct d1_admission *a,
		  struct d1_entry_result *res, struct d1_undo *u)
{
	struct d1_object *o;
	struct d1_owner_assoc *assoc;
	struct d1_chunk *chunk;
	struct d1_txn *txn = NULL;
	struct d1_version *ver = NULL;
	uint64_t start, end;
	bool single_writer = (a->rights & D1_RIGHT_SINGLE_WRITER) != 0;
	bool activate;
	uint32_t i;

	/*
	 * Shape before room, and the whole shape.  Bounds are the object's
	 * maximum, not its current EOF, and they depend on nothing but the
	 * store's geometry and this entry -- so they are asked before the
	 * object is looked up or created.  Asking afterwards made the
	 * answer to a malformed request depend on how full an internal
	 * table happened to be: with a slot free it was INVALID and
	 * recorded, and with every slot taken the identical request became
	 * NOSPC and retryable.
	 */
	if (!d1_mul_u64(e->index, s->chunk_bytes, &start) ||
	    !d1_add_u64(start, e->payload_len, &end) || end > s->max_file_bytes)
		return D1_INVALID;
	if (e->payload_len < 1 || e->payload_len > s->chunk_bytes)
		return D1_INVALID;
	/*
	 * Section 2 puts "writer-bearing input must match its granted
	 * writer ID" with the export/object/principal bindings, so a
	 * mismatch is a binding failure rather than a malformed request.
	 * The shape of the field was already checked before the store saw
	 * it; what fails here is whose writer it is.
	 */
	if (e->owner.writer != a->writer)
		return D1_STALE_AUTH;
	if (e->owner.writer == D1_WRITER_RESERVED_LOW ||
	    e->owner.writer == D1_WRITER_RESERVED_HIGH)
		return D1_INVALID;
	/* An absent guard predicate is only a single writer's to omit. */
	if (!e->guard_check && !single_writer)
		return D1_INVALID;
	/* And a multi-writer request may not ask for activation at all. */
	if (d1_activation_asked_wrongly(single_writer,
					env->body.write.activate))
		return D1_INVALID;
	if (!d1_checksum_verify(&e->checksum, e->payload, e->payload_len))
		return D1_CHECKSUM;
	/* An owner names one version, and only an exact replay reuses it. */
	assoc = d1_owner_find(s, &env->object.export_uuid, &e->owner);
	if (assoc) {
		/*
		 * Section 5: an OWNER_CONFLICT result carries the current
		 * CAS guard -- of the chunk the owner is bound to, which
		 * is the one the caller can do something about.  The
		 * answer is still independent of how full the object table
		 * is: the bound chunk already exists, because the binding
		 * came from a write that took it, and nothing is created
		 * to read it.  A zeroed guard decodes as "written,
		 * generation 0, writer 0", which is a different and wrong
		 * answer, and it is durable: the receipt is replayed and
		 * compared on every rebuild.
		 */
		res->guard = d1_owner_guard(s, assoc);
		return D1_OWNER_CONFLICT;
	}

	/*
	 * Everything above depends on the request and the grant it came
	 * with, and nothing else.  Asking any of it after the tables were
	 * consulted made a malformed request's answer depend on how full
	 * an internal table happened to be: with room it was a recorded
	 * semantic refusal, and with the table full the identical request
	 * became NOSPC and retryable.  From here on the answers do depend
	 * on the store, so from here on the store is asked.
	 *
	 * The order above is the answer for a request that is wrong in
	 * more than one way, and it is frozen: a receipt keeps the answer
	 * the request got, and replay compares it, so reordering these is
	 * a format change and not a tidy-up.  A request that reuses an
	 * owner and carries a bad checksum is answered CHECKSUM, because
	 * the bytes it brought are wrong before the binding it asks for
	 * is; a multi-writer request that asks to activate against a stale
	 * guard is answered INVALID, because a request its grant may not
	 * make is not a request whose expectations are worth comparing.
	 * Earlier revisions of this model answered both the other way
	 * round.  Nothing is deployed and no log outlives a revision, so
	 * this is a decision and not a compatibility claim.
	 */
	o = d1_object_find(s, &env->object);
	if (!o) {
		/* First touch of an object creates it, and a refusal
		 * afterwards must leave it uncreated. */
		o = d1_object_get(s, &env->object);
		if (!o)
			return D1_NOSPC;
		u->fresh_object = o;
	}
	/*
	 * The chunk table is capacity, not geometry.  Section 3 bounds an
	 * object by max_file_bytes, and this model keeps a fixed table
	 * that may not reach that far; a write the declared geometry
	 * allows is therefore a request the model has no room for, not a
	 * malformed one.  NOSPC is the answer, and the caller above turns
	 * it into an UNRECORDED member: no receipt, no durable ID, no
	 * owner association, and nothing in the log.
	 */
	if (e->index >= D1_MAX_CHUNKS)
		return D1_NOSPC;

	chunk = &o->chunks[e->index];
	res->guard = chunk->guard;

	/* Whose chunk this is, before anything about its contents. */
	if (d1_chunk_writable(chunk) != D1_OK)
		return D1_QUARANTINED;

	/* The guard predicate itself is a question about the chunk. */
	if (e->guard_check) {
		/*
		 * The never-written chunk accepts the initial guard, and the
		 * initial guard is (0,0).  The numbers are in the request
		 * and in its canonical bytes whether or not the chunk has
		 * been written, so matching the boolean alone accepted every
		 * numeric pair as if the fields were not there.  Its first
		 * successful write still takes generation 0 and the granted
		 * writer; that is a different statement about the result.
		 */
		if (e->expected.never_written != chunk->guard.never_written)
			return D1_GUARDED;
		if (e->expected.never_written) {
			if (e->expected.generation != 0u ||
			    e->expected.writer != 0u)
				return D1_GUARDED;
		} else if (e->expected.generation != chunk->guard.generation ||
			   e->expected.writer != chunk->guard.writer) {
			return D1_GUARDED;
		}
	}

	/* One uncommitted transaction per chunk in this first model. */
	if (chunk->pending_present)
		return D1_GUARDED;

	activate = d1_activation_allowed(env->body.write.activate,
					 env->body.write.stability,
					 d1_chunk_empty(s, chunk));

	/*
	 * Section 3: reject exhaustion, never reuse.  A write needs a
	 * transaction ID and a version ID, and a counter at its last value
	 * has none left to give -- issuing it and then wrapping would make
	 * the next write's ID zero, which means absent, and the one after
	 * that a number a retained row already holds.  The store has no
	 * room for this request, which is what NOSPC says, and it is
	 * refused before a row is taken, a counter moves, a guard advances
	 * or anything is published.
	 */
	if (d1_next_txn_seen(s) == UINT64_MAX ||
	    d1_next_version_seen(s) == UINT64_MAX)
		return D1_NOSPC;
	/*
	 * The epoch a publication would advance is the same kind of
	 * counter, and it is durable: a complete result carries it, a
	 * snapshot carries it, and recovery reads it as an ordering bound.
	 * A wrap would make an old epoch indistinguishable from a new one
	 * and a recorded high read epoch look like the future.
	 */
	if (activate && d1_index_epoch_seen(s) == UINT64_MAX)
		return D1_NOSPC;

	for (i = 0; i < D1_MAX_TXNS && !txn; i++)
		if (!s->txns[i].used)
			txn = &s->txns[i];
	for (i = 0; i < D1_MAX_VERSIONS && !ver; i++)
		if (!s->versions[i].used)
			ver = &s->versions[i];
	/*
	 * Defensive, and presently unreachable: every recorded member
	 * takes one receipt and at most one row of each table, the tables
	 * are the same size, and the receipt table is reserved first -- so
	 * it refuses at the count these would.  Kept because "the table
	 * is full" is a real answer of the model and the sizes are not a
	 * contract.
	 */
	if (!txn || !ver)
		return D1_NOSPC;

	u->fresh_version = ver;
	memset(ver, 0, sizeof(*ver));
	ver->used = true;
	ver->id = s->next_version++;
	ver->object = d1_object_slot(s, o);
	ver->index = e->index;
	memcpy(ver->bytes, e->payload, e->payload_len);
	ver->len = e->payload_len;
	ver->checksum = e->checksum;
	ver->owner = e->owner;
	ver->predecessor_present = chunk->visible_present;
	ver->predecessor = chunk->visible;

	u->fresh_txn = txn;
	memset(txn, 0, sizeof(*txn));
	txn->used = true;
	txn->id = s->next_txn++;
	txn->object = ver->object;
	txn->index = e->index;
	txn->owner = e->owner;
	txn->admission = a->id;
	txn->version = ver->id;
	txn->mode = D1_MODE_ORDINARY;
	txn->stability = env->body.write.stability;
	txn->predecessor_present = ver->predecessor_present;
	txn->predecessor = ver->predecessor;

	assoc = d1_owner_spare(s);
	/* Defensive and presently unreachable; see the row scan above. */
	if (!assoc)
		return D1_NOSPC;
	u->fresh_assoc = assoc;
	d1_owner_bind(assoc, &env->object.export_uuid, &e->owner);
	assoc->object = ver->object;
	assoc->index = e->index;
	assoc->version = ver->id;

	/*
	 * The guard advances on every accepted write.  A never-written
	 * chunk's first success is generation zero with the granted writer;
	 * later successes increment, and nothing ever decreases it.
	 */
	d1_undo_chunk(u, chunk);
	if (chunk->guard.never_written) {
		chunk->guard.generation = 0;
		chunk->guard.never_written = false;
	} else {
		if (chunk->guard.generation == UINT32_MAX)
			return D1_NOSPC;
		chunk->guard.generation++;
	}
	chunk->guard.writer = a->writer;
	txn->guard = chunk->guard;

	if (activate) {
		/* One event: owner, payload, extent, guard and receipt. */
		txn->phase = D1_PHASE_COMMITTED;
		d1_publish_visible(s, chunk, ver->id);
		s->index_epoch++;
	} else {
		txn->phase = D1_PHASE_PREPARED;
		chunk->pending_present = true;
		chunk->pending = txn->id;
	}

	res->version_present = true;
	res->version = d1_version_of(s, ver->id);
	res->txn_present = true;
	res->txn = d1_txn_of(s, txn->id);
	res->guard = chunk->guard;
	res->owner = e->owner;
	res->activated = activate;
	res->phase = txn->phase;
	return D1_OK;
}

static uint32_t d1_do_lifecycle_entry(struct d1_store *s,
				      const struct d1_envelope *env,
				      const struct d1_lifecycle_entry *e,
				      struct d1_admission *a, bool commit,
				      struct d1_entry_result *res,
				      struct d1_undo *u)
{
	struct d1_object *o = d1_object_find(s, &env->object);
	const struct d1_lifecycle_batch *l = &env->body.lifecycle;
	struct d1_chunk *chunk;
	struct d1_txn *txn;
	struct d1_version *ver;

	if (!o)
		return D1_INVALID;
	if (e->index >= D1_MAX_CHUNKS)
		return D1_INVALID;
	/* The member is resolved by owner, and its index checked. */
	if (e->index < l->range_begin || e->index >= l->range_end)
		return D1_INVALID;

	/*
	 * Section 5 says GUARDED and OWNER_CONFLICT results carry the
	 * current CAS guard, so it is captured as soon as the chunk is
	 * known -- before any check that can produce one.  A zeroed guard
	 * decodes as "written, generation 0, writer 0", which is a
	 * different and wrong answer.
	 */
	chunk = &o->chunks[e->index];
	res->guard = chunk->guard;
	if (d1_chunk_writable(chunk) != D1_OK)
		return D1_QUARANTINED;

	txn = d1_txn_find(s, e->txn);
	if (!txn)
		return D1_INVALID;
	if (txn->index != e->index || txn->object != d1_object_slot(s, o))
		return D1_INVALID;
	if (txn->owner.cohort.raw != e->owner.cohort.raw ||
	    txn->owner.writer != e->owner.writer ||
	    txn->owner.co_id != e->owner.co_id)
		return D1_OWNER_CONFLICT;
	/* Only whole-cohort entry points advance a repair member. */
	if (txn->mode != D1_MODE_ORDINARY)
		return D1_INVALID;
	if (txn->admission != a->id)
		return D1_STALE_AUTH;

	res->owner = txn->owner;
	res->txn_present = true;
	res->txn = d1_txn_of(s, txn->id);
	res->version_present = true;
	res->version = d1_version_of(s, txn->version);

	/*
	 * Phase before predecessor.  A transition asked for out of turn is
	 * refused for being out of turn, whatever it expected to find: a
	 * premature COMMIT of a PREPARED transaction is BAD_PHASE, not a
	 * verdict on a predecessor that the FINALIZE it skipped had not
	 * yet had the chance to be judged against.  The two answers are
	 * both recorded and replay compares them, so which one wins is
	 * part of the format; mana chose this order and it is written here
	 * rather than left to the order the checks happen to be in.
	 */
	if (txn->phase != (commit ? D1_PHASE_FINALIZED : D1_PHASE_PREPARED))
		return D1_BAD_PHASE;

	if (txn->predecessor_present != e->predecessor_present ||
	    (e->predecessor_present && txn->predecessor != e->predecessor.raw))
		return D1_NO_PREDECESSOR;

	if (!commit) {
		d1_undo_txn(u, txn);
		txn->phase = D1_PHASE_FINALIZED;
		res->phase = txn->phase;
		return D1_OK;
	}

	ver = d1_version_find(s, d1_version_of(s, txn->version));
	if (!ver)
		return D1_INVALID;
	/*
	 * What this transaction displaces has to be what it recorded
	 * displacing.  Section 7 calls a stale pointer a conflict, and a
	 * commit that published over something else would lose it without
	 * anything saying so.
	 *
	 * It is defensive today: a repair refuses to open over a chunk
	 * that still holds an uncommitted transaction, and holds the chunk
	 * until it is unlocked, so nothing can move the visible pointer
	 * under a finalized one.  That is a property of how the repair
	 * paths happen to be built, which is exactly the kind of thing
	 * this model has learned not to rely on, so it is asked rather
	 * than assumed.
	 */
	if (txn->predecessor_present != chunk->visible_present ||
	    (txn->predecessor_present && txn->predecessor != chunk->visible))
		return D1_OWNER_CONFLICT;
	/* The epoch this publication advances; see d1_do_write_entry. */
	if (d1_index_epoch_seen(s) == UINT64_MAX)
		return D1_NOSPC;
	/* Payload and extent metadata are replaced together. */
	d1_undo_txn(u, txn);
	d1_undo_chunk(u, chunk);
	txn->phase = D1_PHASE_COMMITTED;
	d1_publish_visible(s, chunk, ver->id);
	chunk->pending_present = false;
	chunk->pending = 0;
	s->index_epoch++;
	res->phase = txn->phase;
	return D1_OK;
}

/*
 * Roll one entry back.
 *
 * A private transaction is its owner's to cancel: PREPARED or FINALIZED,
 * validated by owner, transaction, admission and the predecessor it
 * recorded, it loses its pending slot and becomes ROLLED_BACK.  A
 * different chunk's visible version is not its business and is not
 * touched, and the CAS generation does not go back -- a rollback is not
 * an undo of the guard.
 *
 * Committed data is not the owner's to roll back.  That needs repair
 * custody over the exact version currently visible, and even then it can
 * only put back a predecessor that is still retained: a missing or
 * released one leaves the current data where it is and says
 * NO_PREDECESSOR, which describes what is there rather than granting
 * permission to remove it.
 */
static uint32_t
d1_do_rollback_entry(struct d1_store *s, const struct d1_envelope *env,
		     const struct d1_rollback_entry *e, struct d1_admission *a,
		     struct d1_entry_result *res, struct d1_undo *u)
{
	struct d1_object *o = d1_object_find(s, &env->object);
	const struct d1_rollback_batch *r = &env->body.rollback;
	struct d1_chunk *chunk;
	struct d1_txn *txn;
	struct d1_version *ver, *pred;
	struct d1_custody *custody;

	if (!o || e->index >= D1_MAX_CHUNKS)
		return D1_INVALID;
	if (e->index < r->range_begin || e->index >= r->range_end)
		return D1_INVALID;

	/*
	 * GUARDED and OWNER_CONFLICT results carry the current CAS guard,
	 * so it is captured as soon as the chunk is known -- before any
	 * check that can produce one.
	 */
	chunk = &o->chunks[e->index];
	res->guard = chunk->guard;
	if (d1_chunk_writable(chunk) != D1_OK)
		return D1_QUARANTINED;

	txn = d1_txn_find(s, e->txn);
	if (!txn || txn->index != e->index ||
	    txn->object != d1_object_slot(s, o))
		return D1_INVALID;
	if (txn->owner.cohort.raw != e->owner.cohort.raw ||
	    txn->owner.writer != e->owner.writer ||
	    txn->owner.co_id != e->owner.co_id)
		return D1_OWNER_CONFLICT;

	res->owner = txn->owner;
	res->txn_present = true;
	res->txn = d1_txn_of(s, txn->id);

	if (txn->phase == D1_PHASE_PREPARED ||
	    txn->phase == D1_PHASE_FINALIZED) {
		/*
		 * One's own private work, cancelled with WRITE and no
		 * custody -- and only a repair's whole-cohort entry points
		 * cancel a repair member, as section 5 requires.
		 */
		if (txn->mode != D1_MODE_ORDINARY)
			return D1_INVALID;
		if ((a->rights & D1_RIGHT_WRITE) != D1_RIGHT_WRITE)
			return D1_STALE_AUTH;
		if (txn->admission != a->id)
			return D1_STALE_AUTH;
		if (e->custody_present)
			return D1_INVALID;
		/*
		 * The options are expected states, not flags that switch
		 * checking off.  An absent option asserts that there is
		 * nothing there.
		 */
		if (e->visible_present != chunk->visible_present ||
		    (e->visible_present && e->visible.raw != chunk->visible))
			return D1_OWNER_CONFLICT;
		if (txn->predecessor_present != e->predecessor_present ||
		    (e->predecessor_present &&
		     txn->predecessor != e->predecessor.raw))
			return D1_NO_PREDECESSOR;
		d1_undo_chunk(u, chunk);
		d1_undo_txn(u, txn);
		if (chunk->pending_present && chunk->pending == txn->id) {
			chunk->pending_present = false;
			chunk->pending = 0;
		}
		txn->phase = D1_PHASE_ROLLED_BACK;
		res->phase = txn->phase;
		return D1_OK;
	}

	if (txn->phase != D1_PHASE_COMMITTED)
		return D1_BAD_PHASE;

	/*
	 * From here it is committed data.  Section 2 asks for REPAIR plus
	 * exact custody; it does not also ask for WRITE, and normal owner
	 * custody cannot roll back committed data at all.
	 *
	 * A committed repair replacement is rolled back here like any
	 * other committed version.  Section 7 is explicit that it needs
	 * fresh custody bound to THAT replacement's predecessor, and that
	 * its absence produces NO_PREDECESSOR rather than an
	 * unconditional rejection based on the mode it was written under.
	 */
	if (!e->custody_present)
		return D1_STALE_AUTH;
	if ((a->rights & D1_RIGHT_REPAIR) != D1_RIGHT_REPAIR)
		return D1_STALE_AUTH;
	if (e->visible_present != chunk->visible_present ||
	    (e->visible_present && e->visible.raw != chunk->visible))
		return D1_OWNER_CONFLICT;
	/*
	 * Custody is looked up on replay too.  It is journalled, so the
	 * rebuilt table holds the same handle bound to the same version --
	 * and the binding is not merely authorisation: it is the exact
	 * successor pointer section 7 requires, so skipping it would make
	 * replay reach a different answer than the live store did.
	 */
	custody = d1_custody_find(s, e->custody);
	if (!custody)
		return D1_STALE_AUTH;
	/* Custody names the exact version it was issued over. */
	if (!chunk->visible_present || custody->version != chunk->visible)
		return D1_OWNER_CONFLICT;
	if (chunk->visible != txn->version)
		return D1_OWNER_CONFLICT;
	/* A pending transaction must be cancelled before this. */
	if (chunk->pending_present)
		return D1_GUARDED;

	ver = d1_version_find(s, d1_version_of(s, txn->version));
	if (!ver)
		return D1_INVALID;
	res->version_present = true;
	res->version = d1_version_of(s, ver->id);

	/* The expected predecessor is compared for presence and value. */
	if (ver->predecessor_present != e->predecessor_present ||
	    (e->predecessor_present && ver->predecessor != e->predecessor.raw))
		return D1_NO_PREDECESSOR;
	pred = ver->predecessor_present ?
		       d1_version_find(s, d1_version_of(s, ver->predecessor)) :
		       NULL;
	if (!pred || pred->released) {
		struct d1_postcond *p;

		/*
		 * Nothing to put back.  The current data stays, no episode
		 * is created, and the entry names the predecessor it was
		 * looking for.
		 *
		 * Section 4 also answers with a durable postcondition
		 * handle, and section 7 has a NOPRE repair consume it.  So
		 * this refusal records one bound to the successor it did
		 * not replace, and keeps it: the postcondition is the
		 * result, not a mutation the refusal failed to undo.  A
		 * store with no row left has not recorded the result it
		 * would return, so it answers NOSPC and records nothing.
		 */
		p = d1_postcond_spare(s);
		if (!p || d1_next_postcond_seen(s) == UINT64_MAX)
			return D1_NOSPC;
		u->fresh_postcond = p;
		memset(p, 0, sizeof(*p));
		p->used = true;
		p->id = s->next_postcond++;
		p->object = d1_object_slot(s, o);
		p->index = e->index;
		p->successor = ver->id;
		p->owner = ver->owner;
		p->predecessor_present = ver->predecessor_present;
		p->predecessor = ver->predecessor;
		p->predecessor_released = pred != NULL;
		p->custody = e->custody.raw;
		res->postcond_present = true;
		res->postcond = d1_postcond_of(s, p->id);
		res->phase = txn->phase;
		u->keep = true;
		return D1_NO_PREDECESSOR;
	}
	/* The epoch this publication advances; see d1_do_write_entry. */
	if (d1_index_epoch_seen(s) == UINT64_MAX)
		return D1_NOSPC;
	/* Payload and extent are restored in the one transition. */
	d1_undo_chunk(u, chunk);
	d1_undo_txn(u, txn);
	d1_publish_visible(s, chunk, pred->id);
	txn->phase = D1_PHASE_ROLLED_BACK;
	s->index_epoch++;
	res->version = d1_version_of(s, pred->id);
	res->phase = txn->phase;
	return D1_OK;
}

static bool d1_verifier_matches(const struct d1_store *s,
				const uint8_t given[D1_VERIFIER_BYTES])
{
	uint8_t want[D1_VERIFIER_BYTES];

	d1_verifier_of(s->incarnation, want);
	return memcmp(want, given, D1_VERIFIER_BYTES) == 0;
}

/*
 * A repair transition is whole-cohort atomic, so its before-state is a
 * vector: the cohort row it took or changed, and every chunk it held.
 */
struct d1_repair_undo {
	/*
	 * Whether what this call did survives its own error status.
	 * Almost every refused transition in this model changes nothing;
	 * section 7's cohort abort is the exception it names explicitly --
	 * a failed payload checksum abandons every still-private member
	 * in one durable event, and the result carries ABORTED beside the
	 * error.  So the abort says so here rather than being undone with
	 * everything else.
	 */
	bool keep;
	struct d1_repair *fresh;
	struct d1_repair *cohort;
	struct d1_repair cohort_before;
	bool cohort_changed;
	uint32_t count;
	struct d1_chunk *chunk[D1_BATCH_ENTRIES_MAX];
	struct d1_chunk chunk_before[D1_BATCH_ENTRIES_MAX];
	/*
	 * The store's own counters, restored with everything else.  "The
	 * before-state restored exactly" includes the durable IDs a
	 * refused call took and the epoch it advanced: an epoch left
	 * behind by a call that published nothing is a number the log
	 * cannot account for, and the next rebuild disagrees with the
	 * store it rebuilt.  See d1_undo_apply, which says the same for
	 * an ordinary entry.
	 */
	uint64_t epoch_before;
	uint64_t next_version_before;
	uint64_t next_txn_before;
	uint64_t next_repair_before;
	uint64_t next_episode_before;
	struct d1_episode *fresh_episode;
	/* Rows a refused call took, which it does not get to keep. */
	uint32_t fresh_count;
	struct d1_version *fresh_version[D1_BATCH_ENTRIES_MAX];
	struct d1_txn *fresh_txn[D1_BATCH_ENTRIES_MAX];
	/*
	 * Association rows a member took or moved.  These are restored
	 * whole rather than released, because prepare_repair moves an
	 * existing binding onto the replacement it stages and only
	 * begin_repair takes an unused row.
	 */
	uint32_t owner_count;
	struct d1_owner_assoc *owner[D1_BATCH_ENTRIES_MAX];
	struct d1_owner_assoc owner_before[D1_BATCH_ENTRIES_MAX];
	/* Postconditions a NOPRE member consumed, restored unconsumed. */
	uint32_t postcond_count;
	struct d1_postcond *postcond[D1_BATCH_ENTRIES_MAX];
	struct d1_postcond postcond_before[D1_BATCH_ENTRIES_MAX];
	bool fail_index_before;
	bool overlay_before;
};

static void d1_repair_undo_begin(struct d1_repair_undo *u,
				 const struct d1_store *s)
{
	memset(u, 0, sizeof(*u));
	u->epoch_before = s->index_epoch;
	u->next_version_before = s->next_version;
	u->next_txn_before = s->next_txn;
	u->next_repair_before = s->next_repair;
	u->next_episode_before = s->next_episode;
	u->fail_index_before = s->fail_next_index;
	u->overlay_before = s->overlay_active;
}

static void d1_repair_undo_chunk(struct d1_repair_undo *u,
				 struct d1_chunk *chunk)
{
	uint32_t i;

	for (i = 0; i < u->count; i++)
		if (u->chunk[i] == chunk)
			return;
	if (u->count >= D1_BATCH_ENTRIES_MAX)
		return;
	u->chunk[u->count] = chunk;
	u->chunk_before[u->count] = *chunk;
	u->count++;
}

static void d1_repair_undo_fresh(struct d1_repair_undo *u,
				 struct d1_version *ver, struct d1_txn *txn)
{
	if (u->fresh_count >= D1_BATCH_ENTRIES_MAX)
		return;
	u->fresh_version[u->fresh_count] = ver;
	u->fresh_txn[u->fresh_count] = txn;
	u->fresh_count++;
}

static void d1_repair_undo_owner(struct d1_repair_undo *u,
				 struct d1_owner_assoc *assoc)
{
	uint32_t i;

	for (i = 0; i < u->owner_count; i++)
		if (u->owner[i] == assoc)
			return;
	if (u->owner_count >= D1_BATCH_ENTRIES_MAX)
		return;
	u->owner[u->owner_count] = assoc;
	u->owner_before[u->owner_count] = *assoc;
	u->owner_count++;
}

static void d1_repair_undo_postcond(struct d1_repair_undo *u,
				    struct d1_postcond *p)
{
	uint32_t i;

	for (i = 0; i < u->postcond_count; i++)
		if (u->postcond[i] == p)
			return;
	if (u->postcond_count >= D1_BATCH_ENTRIES_MAX)
		return;
	u->postcond[u->postcond_count] = p;
	u->postcond_before[u->postcond_count] = *p;
	u->postcond_count++;
}

static void d1_repair_undo_cohort(struct d1_repair_undo *u,
				  struct d1_repair *cohort)
{
	if (u->cohort_changed)
		return;
	u->cohort = cohort;
	u->cohort_before = *cohort;
	u->cohort_changed = true;
}

static void d1_repair_undo_apply(struct d1_store *s, struct d1_repair_undo *u)
{
	uint32_t i;

	for (i = 0; i < u->count; i++)
		*u->chunk[i] = u->chunk_before[i];
	if (u->cohort_changed)
		*u->cohort = u->cohort_before;
	if (u->fresh)
		u->fresh->used = false;
	if (u->fresh_episode)
		u->fresh_episode->used = false;
	for (i = 0; i < u->fresh_count; i++) {
		if (u->fresh_version[i])
			u->fresh_version[i]->used = false;
		if (u->fresh_txn[i])
			u->fresh_txn[i]->used = false;
	}
	for (i = 0; i < u->owner_count; i++)
		*u->owner[i] = u->owner_before[i];
	for (i = 0; i < u->postcond_count; i++)
		*u->postcond[i] = u->postcond_before[i];
	s->index_epoch = u->epoch_before;
	s->next_version = u->next_version_before;
	s->next_txn = u->next_txn_before;
	s->next_repair = u->next_repair_before;
	s->next_episode = u->next_episode_before;
	s->fail_next_index = u->fail_index_before;
	s->overlay_active = u->overlay_before;
}

/*
 * Open a repair over a vector of chunks.
 *
 * The whole vector is validated before a cohort row is taken, so a
 * request that names one chunk it may not repair admits no cohort at
 * all -- which is what stops a NOPRE repair widening from the member
 * that is eligible to the neighbour that is not.
 *
 * What each member states is compared against what the store holds: the
 * successor it repairs must still be the visible version, its custody
 * must be the custody issued over exactly that version, and the
 * predecessor it declares must be the one that version recorded.  That
 * is the "exact predecessor/current vector" section 7 captures, and
 * capturing it here is what lets every later call in the repair be
 * checked against it rather than against itself.
 */
static uint32_t d1_do_begin_repair(struct d1_store *s,
				   const struct d1_envelope *env,
				   struct d1_object *o,
				   struct d1_entry_result *res,
				   struct d1_repair_undo *u)
{
	const struct d1_repair_batch *rb = &env->body.repair;
	struct d1_episode *episode = NULL;
	struct d1_repair *row = NULL;
	uint32_t i;

	/*
	 * The episode the ERROR members are in, named once for the
	 * vector.  The decoder requires it exactly when the vector has an
	 * ERROR member, so its absence here is a vector that has none.
	 */
	if (rb->episode_present) {
		episode = d1_episode_find(s, rb->episode);
		if (!episode)
			return D1_STALE_AUTH;
		if (episode->object != d1_object_slot(s, o))
			return D1_INVALID;
	}

	for (i = 0; i < rb->count; i++) {
		const struct d1_repair_entry *e = &rb->entries[i];
		struct d1_owner_assoc *assoc;
		struct d1_chunk *chunk;
		struct d1_custody *custody;
		struct d1_version *ver;
		uint32_t j;

		if (e->index >= D1_MAX_CHUNKS)
			return D1_INVALID;
		chunk = &o->chunks[e->index];
		res->guard = chunk->guard;
		/* One repair at a time, and not over private work. */
		if (chunk->repair_present || chunk->repair_locked)
			return D1_QUARANTINED;
		if (chunk->pending_present)
			return D1_GUARDED;
		if (!chunk->visible_present ||
		    chunk->visible != e->successor.raw)
			return D1_OWNER_CONFLICT;
		custody = d1_custody_find(s, e->custody);
		if (!custody)
			return D1_STALE_AUTH;
		if (custody->version != chunk->visible)
			return D1_OWNER_CONFLICT;
		ver = d1_version_find(s, d1_version_of(s, chunk->visible));
		if (!ver)
			return D1_INVALID;
		if (ver->predecessor_present != e->predecessor_present ||
		    (e->predecessor_present &&
		     ver->predecessor != e->predecessor.raw))
			return D1_NO_PREDECESSOR;
		if (e->mode == D1_REPAIR_NOPRE) {
			struct d1_postcond *p;

			/* An episode is what an ERROR repair is for. */
			if (chunk->error_present)
				return D1_BAD_PHASE;
			/*
			 * Section 7's F2: only the members that are NOPRE
			 * cases authorize a NOPRE repair, never their
			 * neighbours -- and what makes a member one is the
			 * postcondition a refused rollback of this exact
			 * successor left behind, which this consumes.  A
			 * state that would answer NO_PREDECESSOR is not an
			 * authorization: no rollback was attempted, so no
			 * result authorized anything.
			 *
			 * The whole vector is validated before a row is
			 * taken, so naming an ineligible member admits no
			 * cohort at all, and a vector that names one
			 * postcondition twice admits none either.
			 */
			for (j = 0; j < i; j++)
				if (rb->entries[j].postcond_present &&
				    rb->entries[j].postcond.raw ==
					    e->postcond.raw)
					return D1_INVALID;
			p = d1_postcond_find(s, e->postcond);
			if (!p)
				return D1_STALE_AUTH;
			if (p->consumed)
				return D1_BAD_PHASE;
			if (p->object != d1_object_slot(s, o) ||
			    p->index != e->index)
				return D1_INVALID;
			/*
			 * Bound to the unchanged successor: the version the
			 * rollback left in place has to still be the one
			 * this member is repairing.
			 *
			 * The owner the postcondition recorded is not
			 * compared beside it, because it would say nothing
			 * further: section 3 gives a version one owner and
			 * an owner one version, so a postcondition whose
			 * successor is still visible is one whose owner is
			 * too.  It is recorded because section 7 records
			 * it; what it is for is the caller reading its own
			 * refusal back.
			 */
			if (p->successor != chunk->visible)
				return D1_OWNER_CONFLICT;
			/* And to the custody that rollback was made under. */
			if (p->custody != e->custody.raw)
				return D1_STALE_AUTH;
		} else {
			if (!chunk->error_present)
				return D1_BAD_PHASE;
			/* And in the episode this call is about. */
			if (!episode || chunk->error_episode != episode->id)
				return D1_BAD_PHASE;
			if (chunk->error_custody != e->custody.raw)
				return D1_STALE_AUTH;
			if (chunk->error_version != chunk->visible)
				return D1_OWNER_CONFLICT;
		}
		/*
		 * Section 3's association key belongs to the store, not to
		 * the ordinary write path: a replacement owner names one
		 * object, chunk and version for as long as the store lives.
		 * So a repair may not declare an owner an ordinary write
		 * already took, and -- because the binding is taken here --
		 * an ordinary write may not later take one a repair
		 * declared.  The vector is also checked against itself:
		 * two members sharing an owner would take two rows under
		 * one key, and which of them a later lookup found would be
		 * an accident of row order.
		 */
		for (j = 0; j < i; j++) {
			if (!d1_owner_same(&rb->entries[j].owner, &e->owner))
				continue;
			res->guard = o->chunks[rb->entries[j].index].guard;
			return D1_OWNER_CONFLICT;
		}
		assoc = d1_owner_find(s, &env->object.export_uuid, &e->owner);
		if (assoc) {
			res->guard = d1_owner_guard(s, assoc);
			return D1_OWNER_CONFLICT;
		}
	}

	for (i = 0; i < D1_MAX_REPAIRS && !row; i++)
		if (!s->repairs[i].used)
			row = &s->repairs[i];
	if (!row)
		return D1_NOSPC;
	/*
	 * Room for every member's transaction before any of them is
	 * taken: a cohort half-issued is not a cohort.
	 */
	for (i = 0; i < rb->count; i++) {
		struct d1_txn *spare = NULL;
		uint32_t j, seen = 0;

		for (j = 0; j < D1_MAX_TXNS; j++) {
			if (s->txns[j].used)
				continue;
			if (seen++ == i) {
				spare = &s->txns[j];
				break;
			}
		}
		if (!spare)
			return D1_NOSPC;
	}
	if (d1_next_txn_seen(s) == UINT64_MAX)
		return D1_NOSPC;
	for (i = 0; i < rb->count; i++) {
		struct d1_owner_assoc *spare = NULL;
		uint32_t j, seen = 0;

		for (j = 0; j < D1_MAX_OWNERS; j++) {
			if (s->owners[j].used)
				continue;
			if (seen++ == i) {
				spare = &s->owners[j];
				break;
			}
		}
		if (!spare)
			return D1_NOSPC;
	}

	u->fresh = row;
	memset(row, 0, sizeof(*row));
	row->used = true;
	row->id = s->next_repair++;
	row->object = d1_object_slot(s, o);
	row->admission = env->admission.raw;
	row->phase = D1_PHASE_ADMITTED;
	row->count = rb->count;
	for (i = 0; i < rb->count; i++) {
		const struct d1_repair_entry *e = &rb->entries[i];
		struct d1_repair_member *m = &row->member[i];
		struct d1_chunk *chunk = &o->chunks[e->index];

		struct d1_owner_assoc *assoc;
		struct d1_txn *txn = NULL;
		uint32_t j;

		m->index = e->index;
		m->mode = e->mode;
		m->owner = e->owner;
		m->custody = e->custody.raw;
		m->successor = e->successor.raw;
		m->predecessor_present = e->predecessor_present;
		m->predecessor = e->predecessor.raw;
		/*
		 * The member's own repair transaction, issued where section
		 * 4 issues it: begin_repair answers with the cohort and the
		 * per-member handles, and everything afterwards names them.
		 * It carries no version until prepare_repair stages one.
		 */
		for (j = 0; j < D1_MAX_TXNS && !txn; j++)
			if (!s->txns[j].used)
				txn = &s->txns[j];
		if (!txn)
			return D1_NOSPC;
		memset(txn, 0, sizeof(*txn));
		txn->used = true;
		txn->id = s->next_txn++;
		txn->object = row->object;
		txn->index = e->index;
		txn->owner = e->owner;
		txn->admission = env->admission.raw;
		txn->mode = D1_MODE_REPAIR;
		txn->phase = D1_PHASE_ADMITTED;
		txn->stability = D1_FILE_SYNC;
		txn->predecessor_present = true;
		txn->predecessor = e->successor.raw;
		txn->guard = chunk->guard;
		d1_repair_undo_fresh(u, NULL, txn);
		m->txn = txn->id;
		/*
		 * The association is bound to the member's object and
		 * chunk now and to its replacement at prepare_repair,
		 * which is where a replacement first exists.
		 */
		assoc = d1_owner_spare(s);
		if (!assoc)
			return D1_NOSPC;
		d1_repair_undo_owner(u, assoc);
		d1_owner_bind(assoc, &env->object.export_uuid, &e->owner);
		assoc->object = row->object;
		assoc->index = e->index;
		if (e->mode == D1_REPAIR_NOPRE) {
			struct d1_postcond *p =
				d1_postcond_find(s, e->postcond);

			/* Validated above; consumed exactly once. */
			if (!p)
				return D1_INVALID;
			d1_repair_undo_postcond(u, p);
			p->consumed = true;
		}
		d1_repair_undo_chunk(u, chunk);
		chunk->repair_present = true;
		chunk->repair = row->id;
	}
	res->cohort_present = true;
	res->cohort = d1_repair_of(s, row->id);
	res->phase = row->phase;
	return D1_OK;
}

/* Drop the private work a member staged, replacement and transaction. */
static void d1_repair_member_drop(struct d1_store *s,
				  struct d1_repair_member *m)
{
	struct d1_version *ver =
		d1_version_find(s, d1_version_of(s, m->version));
	struct d1_txn *txn = d1_txn_find(s, d1_txn_of(s, m->txn));

	if (ver)
		ver->used = false;
	if (txn)
		txn->used = false;
	m->staged = false;
	m->version = 0;
	m->txn = 0;
}

/*
 * Abandon a repair that has not published.
 *
 * Section 7: an aborted repair releases its private payloads but keeps
 * quarantine and custody until an explicit recovery or control
 * decision, and an already COMMITTED cohort cannot be aborted.  So this
 * releases the chunks the cohort held and the replacements it staged,
 * and leaves any ERROR episode exactly where it was.
 */
static uint32_t d1_do_abort_repair(struct d1_store *s, struct d1_repair *cohort,
				   struct d1_object *o,
				   struct d1_entry_result *res,
				   struct d1_repair_undo *u)
{
	uint32_t i;

	if (cohort->phase == D1_PHASE_COMMITTED ||
	    cohort->phase == D1_PHASE_ABORTED)
		return D1_BAD_PHASE;

	d1_repair_undo_cohort(u, cohort);
	for (i = 0; i < cohort->count; i++) {
		struct d1_repair_member *m = &cohort->member[i];
		struct d1_chunk *chunk = &o->chunks[m->index];

		d1_repair_undo_chunk(u, chunk);
		chunk->repair_present = false;
		chunk->repair = 0;
		d1_repair_member_drop(s, m);
	}
	cohort->phase = D1_PHASE_ABORTED;
	res->phase = cohort->phase;
	res->cohort_present = true;
	res->cohort = d1_repair_of(s, cohort->id);
	return D1_OK;
}

/*
 * Stage the whole replacement vector, or abort the whole cohort.
 *
 * Section 7: prepare_repair validates and stages the COMPLETE vector
 * atomically.  Once an authorized, correct-phase call has begun
 * evaluating member predicates, a failed payload checksum or a changed
 * predecessor aborts every still-private member in one durable cohort
 * event -- and the result carries ABORTED alongside its error status,
 * so the caller is told both what was wrong and what became of the
 * repair.  A failure before that point, one that could not stage
 * anything, leaves the cohort as it was.
 */
static uint32_t
d1_do_prepare_repair(struct d1_store *s, const struct d1_envelope *env,
		     struct d1_repair *cohort, struct d1_object *o,
		     struct d1_entry_result *res, struct d1_repair_undo *u)
{
	const struct d1_repair_batch *rb = &env->body.repair;
	uint32_t status = D1_OK;
	uint32_t i;

	if (cohort->phase != D1_PHASE_ADMITTED)
		return D1_BAD_PHASE;

	/*
	 * Room first, before any predicate is evaluated: a cohort that
	 * cannot be staged for want of a row has nothing to abort, and
	 * section 7 keeps allocation failure UNRECORDED and the previous
	 * whole cohort state intact.
	 */
	for (i = 0; i < cohort->count; i++) {
		struct d1_version *spare = NULL;
		uint32_t j, taken = 0;

		for (j = 0; j < D1_MAX_VERSIONS; j++) {
			if (s->versions[j].used)
				continue;
			if (taken++ == i) {
				spare = &s->versions[j];
				break;
			}
		}
		if (!spare)
			return D1_NOSPC;
	}
	if (d1_next_version_seen(s) == UINT64_MAX)
		return D1_NOSPC;

	d1_repair_undo_cohort(u, cohort);
	for (i = 0; i < cohort->count; i++) {
		const struct d1_repair_entry *e = &rb->entries[i];
		struct d1_repair_member *m = &cohort->member[i];
		struct d1_chunk *chunk = &o->chunks[m->index];
		struct d1_owner_assoc *assoc;
		struct d1_version *ver = NULL;
		struct d1_txn *txn = NULL;
		uint32_t j;

		res->guard = chunk->guard;
		/*
		 * The state the cohort captured has to still be there.  A
		 * successor that moved underneath the repair is section 7's
		 * "changed predecessor": the repair is about a version that
		 * is no longer the one it was opened over.
		 */
		if (!chunk->visible_present || chunk->visible != m->successor) {
			status = D1_OWNER_CONFLICT;
			break;
		}
		if (!d1_checksum_verify(&e->checksum, e->payload,
					e->payload_len)) {
			status = D1_CHECKSUM;
			break;
		}
		for (j = 0; j < D1_MAX_VERSIONS && !ver; j++)
			if (!s->versions[j].used)
				ver = &s->versions[j];
		txn = d1_txn_find(s, d1_txn_of(s, m->txn));
		if (!ver || !txn) {
			status = D1_NOSPC;
			break;
		}
		memset(ver, 0, sizeof(*ver));
		ver->used = true;
		ver->id = s->next_version++;
		ver->object = d1_object_slot(s, o);
		ver->index = m->index;
		memcpy(ver->bytes, e->payload, e->payload_len);
		ver->len = e->payload_len;
		ver->checksum = e->checksum;
		ver->owner = m->owner;
		/*
		 * Section 7: the replacement records the displaced
		 * successor as its predecessor if that payload is retained.
		 */
		ver->predecessor_present = true;
		ver->predecessor = m->successor;
		/*
		 * The transaction begin_repair issued now carries the
		 * replacement it stages.  Its phase follows the cohort
		 * rather than leading it: section 7 publishes the whole
		 * local vector or none of it.
		 */
		txn->version = ver->id;
		txn->phase = D1_PHASE_PREPARED;
		d1_repair_undo_fresh(u, ver, NULL);
		assoc = d1_owner_find(s, &env->object.export_uuid, &m->owner);
		if (assoc) {
			d1_repair_undo_owner(u, assoc);
			assoc->version = ver->id;
		}
		m->staged = true;
		m->version = ver->id;
	}

	if (status == D1_NOSPC)
		return status;
	if (status != D1_OK) {
		/*
		 * One durable cohort event: every still-private member is
		 * abandoned together, and the result says so.
		 */
		for (i = 0; i < cohort->count; i++) {
			struct d1_repair_member *m = &cohort->member[i];
			struct d1_chunk *chunk = &o->chunks[m->index];

			d1_repair_undo_chunk(u, chunk);
			chunk->repair_present = false;
			chunk->repair = 0;
			d1_repair_member_drop(s, m);
		}
		cohort->phase = D1_PHASE_ABORTED;
		res->phase = cohort->phase;
		res->cohort_present = true;
		res->cohort = d1_repair_of(s, cohort->id);
		u->keep = true;
		return status;
	}

	cohort->phase = D1_PHASE_PREPARED;
	res->phase = cohort->phase;
	res->cohort_present = true;
	res->cohort = d1_repair_of(s, cohort->id);
	return D1_OK;
}

/*
 * Move the whole cohort from PREPARED to FINALIZED.
 *
 * Section 7 finalizes the cohort rather than its members: there is no
 * half-finalized local set, so this is one phase change or none.
 */
static uint32_t d1_do_finalize_repair(struct d1_repair *cohort,
				      struct d1_entry_result *res,
				      struct d1_repair_undo *u,
				      const struct d1_store *s)
{
	if (cohort->phase != D1_PHASE_PREPARED)
		return D1_BAD_PHASE;
	d1_repair_undo_cohort(u, cohort);
	cohort->phase = D1_PHASE_FINALIZED;
	res->phase = cohort->phase;
	res->cohort_present = true;
	res->cohort = d1_repair_of(s, cohort->id);
	return D1_OK;
}

/*
 * Publish the whole local vector at one index epoch.
 *
 * Section 7 requires every member FINALIZED, the exact vector and
 * order, live custody and unchanged predecessors, and then publishes
 * the whole thing at once.  A premature COMMIT is refused without a
 * transition -- the memo's G2 -- so the cohort is still PREPARED
 * afterwards and a FINALIZE followed by a new COMMIT is what it needs.
 *
 * The members stay locked after this.  COMMIT never clears an ERROR
 * episode and never releases custody; unlock is a separate call and
 * clear_error is a separate call before it for the ERROR members.
 */
static uint32_t d1_do_commit_repair(struct d1_store *s,
				    struct d1_repair *cohort,
				    struct d1_object *o,
				    struct d1_entry_result *res,
				    struct d1_repair_undo *u)
{
	uint32_t i;

	if (cohort->phase != D1_PHASE_FINALIZED)
		return D1_BAD_PHASE;
	for (i = 0; i < cohort->count; i++) {
		struct d1_repair_member *m = &cohort->member[i];
		struct d1_chunk *chunk = &o->chunks[m->index];
		struct d1_custody *custody;

		res->guard = chunk->guard;
		if (!m->staged)
			return D1_INVALID;
		if (!chunk->visible_present || chunk->visible != m->successor)
			return D1_OWNER_CONFLICT;
		custody = d1_custody_find(s, d1_custody_of(s, m->custody));
		if (!custody || custody->version != m->successor)
			return D1_STALE_AUTH;
	}
	if (d1_index_epoch_seen(s) == UINT64_MAX)
		return D1_NOSPC;

	d1_repair_undo_cohort(u, cohort);
	for (i = 0; i < cohort->count; i++) {
		struct d1_repair_member *m = &cohort->member[i];
		struct d1_chunk *chunk = &o->chunks[m->index];

		struct d1_txn *txn = d1_txn_find(s, d1_txn_of(s, m->txn));

		d1_repair_undo_chunk(u, chunk);
		d1_publish_visible(s, chunk, m->version);
		chunk->repair_locked = true;
		if (txn)
			txn->phase = D1_PHASE_COMMITTED;
	}
	/* One epoch for the whole vector, not one for each member. */
	s->index_epoch++;
	cohort->phase = D1_PHASE_COMMITTED;
	res->phase = cohort->phase;
	res->cohort_present = true;
	res->cohort = d1_repair_of(s, cohort->id);
	return D1_OK;
}

/*
 * Open an ERROR episode over a vector of chunks.
 *
 * Section 7 requires "a durable mark_error episode with continuous
 * custody" before an ERROR-mode repair, so this is what makes one: it
 * binds the episode to the exact version in error and to the custody
 * issued over that version, and an ERROR repair later checks both are
 * still what they were.
 *
 * It is not a repair itself, so it opens no cohort and takes no chunk
 * away from ordinary writers.  What it does is make one possible.
 */
static uint32_t d1_do_mark_error(struct d1_store *s,
				 const struct d1_envelope *env,
				 struct d1_object *o,
				 struct d1_entry_result *res,
				 struct d1_repair_undo *u)
{
	const struct d1_repair_batch *rb = &env->body.repair;
	struct d1_episode *episode;
	uint32_t i;

	for (i = 0; i < rb->count; i++) {
		const struct d1_repair_entry *e = &rb->entries[i];
		struct d1_chunk *chunk;
		struct d1_custody *custody;
		struct d1_version *ver;

		if (e->index >= D1_MAX_CHUNKS)
			return D1_INVALID;
		chunk = &o->chunks[e->index];
		res->guard = chunk->guard;
		/* One episode at a time, and not over private work. */
		if (chunk->error_present)
			return D1_BAD_PHASE;
		if (chunk->pending_present || chunk->repair_present)
			return D1_GUARDED;
		if (!chunk->visible_present ||
		    chunk->visible != e->successor.raw)
			return D1_OWNER_CONFLICT;
		custody = d1_custody_find(s, e->custody);
		if (!custody)
			return D1_STALE_AUTH;
		if (custody->version != chunk->visible)
			return D1_OWNER_CONFLICT;
		ver = d1_version_find(s, d1_version_of(s, chunk->visible));
		if (!ver)
			return D1_INVALID;
		if (ver->predecessor_present != e->predecessor_present ||
		    (e->predecessor_present &&
		     ver->predecessor != e->predecessor.raw))
			return D1_NO_PREDECESSOR;
	}

	episode = d1_episode_spare(s);
	if (!episode || d1_next_episode_seen(s) == UINT64_MAX)
		return D1_NOSPC;
	u->fresh_episode = episode;
	memset(episode, 0, sizeof(*episode));
	episode->used = true;
	episode->id = s->next_episode++;
	episode->object = d1_object_slot(s, o);
	episode->count = rb->count;
	for (i = 0; i < rb->count; i++) {
		const struct d1_repair_entry *e = &rb->entries[i];
		struct d1_chunk *chunk = &o->chunks[e->index];

		episode->index[i] = e->index;
		d1_repair_undo_chunk(u, chunk);
		chunk->error_present = true;
		chunk->error_cleared = false;
		chunk->error_episode = episode->id;
		chunk->error_custody = e->custody.raw;
		chunk->error_version = chunk->visible;
	}
	res->phase = D1_PHASE_ADMITTED;
	res->episode_present = true;
	res->episode = d1_episode_of(s, episode->id);
	return D1_OK;
}

/*
 * Clear the ERROR episodes a committed repair replaced.
 *
 * Section 7: clear_error checks all ERROR members, the exact episode
 * and custody and the committed cohort, plus the fixture-issued
 * cross-DS completion certificate, and then makes those members
 * readable but locked.  It does not unlock them -- that is a separate
 * call -- and it clears only the ERROR members, because a NOPRE member
 * has no episode to clear.
 */
static uint32_t d1_do_clear_error(struct d1_store *s,
				  const struct d1_envelope *env,
				  struct d1_repair *cohort, struct d1_object *o,
				  struct d1_entry_result *res,
				  struct d1_repair_undo *u)
{
	const struct d1_repair_batch *rb = &env->body.repair;
	struct d1_episode *episode;
	uint32_t errors = 0;
	uint32_t i;

	if (cohort->phase != D1_PHASE_COMMITTED)
		return D1_BAD_PHASE;
	episode = d1_episode_find(s, rb->episode);
	if (!episode)
		return D1_STALE_AUTH;
	if (!s->certificate_present ||
	    memcmp(s->certificate, rb->certificate, D1_CERTIFICATE_BYTES) != 0)
		return D1_STALE_AUTH;
	/*
	 * Section 7 lets one local cohort carry both modes, and a NOPRE
	 * member has no episode to clear.  So the whole vector is
	 * validated -- it is the cohort's vector and every call names all
	 * of it -- while only the ERROR subset is cleared.  Refusing the
	 * call because a NOPRE member is in it would make the memo's
	 * mixed-mode unlock unreachable and leave an accepted cohort
	 * locked for the store's life; a cohort with nothing to clear is
	 * the request that is actually invalid.
	 */
	for (i = 0; i < cohort->count; i++) {
		struct d1_repair_member *m = &cohort->member[i];
		struct d1_chunk *chunk = &o->chunks[m->index];

		res->guard = chunk->guard;
		if (m->mode != D1_REPAIR_ERROR)
			continue;
		errors++;
		if (!chunk->error_present || chunk->error_cleared)
			return D1_BAD_PHASE;
		if (chunk->error_episode != episode->id)
			return D1_BAD_PHASE;
		if (chunk->error_custody != m->custody)
			return D1_STALE_AUTH;
		if (chunk->error_version != m->successor)
			return D1_OWNER_CONFLICT;
	}
	if (!errors)
		return D1_INVALID;

	for (i = 0; i < cohort->count; i++) {
		struct d1_repair_member *m = &cohort->member[i];
		struct d1_chunk *chunk = &o->chunks[m->index];

		if (m->mode != D1_REPAIR_ERROR)
			continue;
		d1_repair_undo_chunk(u, chunk);
		chunk->error_cleared = true;
	}
	res->phase = cohort->phase;
	res->cohort_present = true;
	res->cohort = d1_repair_of(s, cohort->id);
	return D1_OK;
}

/*
 * Release the members a committed repair still holds.
 *
 * Section 7: unlock releases NOPRE members only after COMMIT and ERROR
 * members only after clear, and for mixed modes every member's
 * predicate must pass before a whole-vector unlock.  So the vector is
 * judged first and released afterwards, which is what stops a cohort
 * half-unlocking.
 */
static uint32_t d1_do_unlock(struct d1_store *s, struct d1_repair *cohort,
			     struct d1_object *o, struct d1_entry_result *res,
			     struct d1_repair_undo *u)
{
	uint32_t i;

	if (cohort->phase != D1_PHASE_COMMITTED)
		return D1_BAD_PHASE;
	for (i = 0; i < cohort->count; i++) {
		struct d1_repair_member *m = &cohort->member[i];
		struct d1_chunk *chunk = &o->chunks[m->index];

		res->guard = chunk->guard;
		if (!chunk->repair_locked)
			return D1_BAD_PHASE;
		if (m->mode == D1_REPAIR_ERROR && !chunk->error_cleared)
			return D1_BAD_PHASE;
	}

	for (i = 0; i < cohort->count; i++) {
		struct d1_repair_member *m = &cohort->member[i];
		struct d1_chunk *chunk = &o->chunks[m->index];

		d1_repair_undo_chunk(u, chunk);
		chunk->repair_locked = false;
		chunk->repair_present = false;
		chunk->repair = 0;
		chunk->error_present = false;
		chunk->error_cleared = false;
		chunk->error_episode = 0;
		chunk->error_custody = 0;
		chunk->error_version = 0;
	}
	res->phase = cohort->phase;
	res->cohort_present = true;
	res->cohort = d1_repair_of(s, cohort->id);
	return D1_OK;
}

/*
 * The whole of a repair operation, whichever one it is.
 *
 * Every one of them names a vector, and every one but begin_repair
 * names the cohort that vector belongs to and must match it exactly --
 * same members, same order, same owners.  Section 7 wants "exact
 * vector/order" and this is where that is asked, once, for all of them.
 */
/*
 * The cohort whose ERROR members are in this episode.
 *
 * Section 4 lets unlock name the episode instead of the cohort, and
 * the members it releases are the cohort's either way, so the one has
 * to resolve to the other.  A cohort's ERROR members are all in one
 * episode, so the first member that matches settles it.
 */
static struct d1_repair *
d1_repair_of_episode(struct d1_store *s, struct d1_object *o, uint64_t episode)
{
	uint32_t i, j;

	for (i = 0; i < D1_MAX_REPAIRS; i++) {
		struct d1_repair *row = &s->repairs[i];

		if (!row->used || row->object != d1_object_slot(s, o))
			continue;
		for (j = 0; j < row->count; j++) {
			const struct d1_chunk *c =
				&o->chunks[row->member[j].index];

			if (row->member[j].mode == D1_REPAIR_ERROR &&
			    c->error_episode == episode)
				return row;
		}
	}
	return NULL;
}

static uint32_t d1_do_repair(struct d1_store *s, const struct d1_envelope *env,
			     struct d1_admission *a,
			     struct d1_entry_result *res,
			     struct d1_repair_undo *u)
{
	const struct d1_repair_batch *rb = &env->body.repair;
	struct d1_object *o = d1_object_find(s, &env->object);
	struct d1_repair *cohort = NULL;
	uint32_t i;

	if (!o)
		return D1_INVALID;
	if (env->op == D1_OP_MARK_ERROR)
		return d1_do_mark_error(s, env, o, res, u);
	if (env->op == D1_OP_BEGIN_REPAIR)
		return d1_do_begin_repair(s, env, o, res, u);

	if (rb->cohort_present) {
		cohort = d1_repair_find(s, rb->cohort);
	} else {
		/*
		 * An unlock that named the episode rather than the cohort.
		 * The decoder allows that for unlock and for nothing else.
		 */
		struct d1_episode *episode = d1_episode_find(s, rb->episode);

		if (!episode)
			return D1_STALE_AUTH;
		cohort = d1_repair_of_episode(s, o, episode->id);
	}
	if (!cohort)
		return D1_INVALID;
	if (cohort->object != d1_object_slot(s, o))
		return D1_INVALID;
	if (cohort->admission != a->id)
		return D1_STALE_AUTH;
	if (cohort->count != rb->count)
		return D1_INVALID;
	for (i = 0; i < rb->count; i++) {
		const struct d1_repair_entry *e = &rb->entries[i];
		const struct d1_repair_member *m = &cohort->member[i];

		if (m->index != e->index)
			return D1_INVALID;
		if (m->owner.cohort.raw != e->owner.cohort.raw ||
		    m->owner.writer != e->owner.writer ||
		    m->owner.co_id != e->owner.co_id)
			return D1_OWNER_CONFLICT;
	}

	switch (env->op) {
	case D1_OP_PREPARE_REPAIR:
		return d1_do_prepare_repair(s, env, cohort, o, res, u);
	case D1_OP_FINALIZE_REPAIR:
		return d1_do_finalize_repair(cohort, res, u, s);
	case D1_OP_COMMIT_REPAIR:
		return d1_do_commit_repair(s, cohort, o, res, u);
	case D1_OP_ABORT_REPAIR:
		return d1_do_abort_repair(s, cohort, o, res, u);
	case D1_OP_CLEAR_ERROR:
		return d1_do_clear_error(s, env, cohort, o, res, u);
	case D1_OP_UNLOCK:
		return d1_do_unlock(s, cohort, o, res, u);
	default:
		return D1_UNSUPPORTED;
	}
}

/*
 * The two ordinary control operations of this slice.
 *
 * Both act on a whole operation rather than on entries: their
 * disposition is the operation's, because there is nothing useful to
 * say about half a recovery or half a reap.
 *
 * recovery_admit supersedes one admission with another and answers the
 * verifier as it now stands, so a caller that has been away can tell
 * whether the store it is talking to is the store it left.
 *
 * lease_reap cancels transactions whose owner is gone.  It is an
 * ordinary entry point, so it refuses to touch a repair member: those
 * advance only through whole-cohort entry points, and letting a lease
 * sweep take one would break that atomicity from the side.
 */
/*
 * A control transition is whole-operation atomic, so its before-state
 * is a vector: every named transaction and every chunk it frees.
 */
struct d1_control_undo {
	uint32_t count;
	struct d1_txn *txn[D1_BATCH_ENTRIES_MAX];
	uint32_t phase[D1_BATCH_ENTRIES_MAX];
	uint64_t admission[D1_BATCH_ENTRIES_MAX];
	uint64_t read_epoch[D1_BATCH_ENTRIES_MAX];
	struct d1_chunk *chunk[D1_BATCH_ENTRIES_MAX];
	struct d1_chunk chunk_before[D1_BATCH_ENTRIES_MAX];
	/*
	 * The cohorts a recovery re-bound with their members.  A repair
	 * is bound to a handle too, and re-binding its transactions
	 * without it would leave the cohort naming a fenced one.
	 */
	struct d1_repair *cohort[D1_BATCH_ENTRIES_MAX];
	uint64_t cohort_admission[D1_BATCH_ENTRIES_MAX];
	struct d1_admission *old;
	bool old_revoked_before;
};

/* The cohort a repair transaction belongs to, if one still does. */
static struct d1_repair *d1_repair_of_member(struct d1_store *s, uint64_t txn)
{
	uint32_t i, j;

	for (i = 0; i < D1_MAX_REPAIRS; i++) {
		if (!s->repairs[i].used)
			continue;
		for (j = 0; j < s->repairs[i].count; j++)
			if (s->repairs[i].member[j].txn == txn)
				return &s->repairs[i];
	}
	return NULL;
}

static void d1_control_undo_apply(struct d1_control_undo *u)
{
	uint32_t i;

	for (i = 0; i < u->count; i++) {
		u->txn[i]->phase = u->phase[i];
		u->txn[i]->admission = u->admission[i];
		u->txn[i]->read_epoch = u->read_epoch[i];
		if (u->chunk[i])
			*u->chunk[i] = u->chunk_before[i];
		if (u->cohort[i])
			u->cohort[i]->admission = u->cohort_admission[i];
	}
	if (u->old)
		u->old->revoked = u->old_revoked_before;
}

static uint32_t d1_do_control(struct d1_store *s, const struct d1_envelope *env,
			      struct d1_admission *a __attribute__((unused)),
			      struct d1_entry_result *res,
			      struct d1_control_undo *u)
{
	const struct d1_control_batch *cb = &env->body.control;
	struct d1_object *o = d1_object_find(s, &env->object);
	struct d1_admission *old, *fresh = NULL;
	struct d1_txn *named[D1_BATCH_ENTRIES_MAX];
	uint32_t i;

	if (!o)
		return D1_INVALID;
	old = d1_admission_find(s, cb->old_admission);
	if (!old ||
	    memcmp(&old->object, &env->object, sizeof(old->object)) != 0)
		return D1_STALE_AUTH;
	/*
	 * Reaping is for work whose lease is gone.  Whether the named
	 * handle is still live is a question about the request's own
	 * authority, so it is settled before its members are examined.
	 */
	if (env->op == D1_OP_LEASE_REAP && !old->expired)
		return D1_STALE_AUTH;

	/*
	 * The whole request is validated before anything is revoked or
	 * rebound.  A semantic error that had already revoked the old
	 * handle would be a mutation on a failure path.
	 */
	for (i = 0; i < cb->count; i++) {
		struct d1_txn *t = d1_txn_find(s, cb->txns[i]);

		if (!t || t->object != d1_object_slot(s, o))
			return D1_INVALID;
		/*
		 * A lease sweep may not take a repair member: section 7
		 * advances and cancels those only through whole-cohort
		 * entry points, and letting a reap take one would break
		 * that atomicity from the side.  A recovery is the other
		 * case -- section 9 re-binds the work a fenced handle left,
		 * and a repair's members are work like any other.
		 */
		if (env->op == D1_OP_LEASE_REAP && t->mode != D1_MODE_ORDINARY)
			return D1_INVALID;
		/*
		 * Work a fenced handle left: pending or finalized for an
		 * ordinary transaction, and for a repair member also the
		 * admitted phase it sits in between begin_repair and the
		 * prepare that stages its replacement.  A cohort opened and
		 * not yet staged is work too, and leaving it unrecoverable
		 * would lock its chunks for the store's life.
		 */
		if (t->phase != D1_PHASE_PREPARED &&
		    t->phase != D1_PHASE_FINALIZED &&
		    !(t->mode == D1_MODE_REPAIR &&
		      t->phase == D1_PHASE_ADMITTED))
			return D1_BAD_PHASE;
		if (t->admission != old->id)
			return D1_OWNER_CONFLICT;
		named[i] = t;
		u->txn[i] = t;
		u->phase[i] = t->phase;
		u->admission[i] = t->admission;
		u->read_epoch[i] = t->read_epoch;
		u->chunk[i] = NULL;
		u->cohort[i] = NULL;
	}
	u->count = cb->count;
	u->old = old;
	u->old_revoked_before = old->revoked;

	if (env->op == D1_OP_RECOVERY_ADMIT) {
		fresh = d1_admission_find(s, cb->new_admission);
		if (!fresh || fresh->revoked || fresh->expired)
			return D1_STALE_AUTH;
		/*
		 * Re-admission moves work between handles for the same
		 * principal on the same object: unchanged owner, object and
		 * issuer binding, as section 2 requires.
		 */
		if (memcmp(&fresh->object, &env->object,
			   sizeof(fresh->object)) != 0)
			return D1_STALE_AUTH;
		if (fresh->writer != old->writer)
			return D1_STALE_AUTH;
		/*
		 * Recovery grants access in the incarnation that is running.
		 * Re-admitting onto another fenced handle would report a
		 * recovery that left the work just as unreachable.
		 */
		if (fresh->incarnation != s->incarnation)
			return D1_STALE_AUTH;
		if (memcmp(&fresh->issuer, &old->issuer,
			   sizeof(fresh->issuer)) != 0 ||
		    memcmp(&fresh->principal, &old->principal,
			   sizeof(fresh->principal)) != 0)
			return D1_STALE_AUTH;
		/*
		 * The fresh handle's rights are deliberately not checked
		 * against the work being moved.  Recovery re-binds pending
		 * and finalized transactions to a live handle; what that
		 * handle may then do with them is the ordinary authority
		 * question, asked at each later operation.  So a recovery
		 * onto a READ-only handle succeeds and its FINALIZE is then
		 * refused STALE_AUTH, which is recoverable by admitting
		 * again.  Refusing here instead would make recovery assert
		 * an intent the request does not carry.  The memo does not
		 * settle this; the choice is stated rather than implied.
		 */
		/* A read epoch the store has never reached is not a recovery. */
		if (cb->read_epoch > s->index_epoch)
			return D1_INVALID;

		for (i = 0; i < cb->count; i++) {
			named[i]->admission = fresh->id;
			named[i]->read_epoch = cb->read_epoch;
			if (named[i]->mode != D1_MODE_REPAIR)
				continue;
			u->cohort[i] = d1_repair_of_member(s, named[i]->id);
			if (u->cohort[i]) {
				u->cohort_admission[i] =
					u->cohort[i]->admission;
				u->cohort[i]->admission = fresh->id;
			}
		}
		old->revoked = true;
		d1_verifier_of(s->incarnation, res->verifier);
		return D1_OK;
	}

	/* lease_reap */
	for (i = 0; i < cb->count; i++) {
		struct d1_txn *t = named[i];
		struct d1_chunk *chunk = &o->chunks[t->index];

		u->chunk[i] = chunk;
		u->chunk_before[i] = *chunk;
		if (chunk->pending_present && chunk->pending == t->id) {
			chunk->pending_present = false;
			chunk->pending = 0;
		}
		t->phase = D1_PHASE_ROLLED_BACK;
	}
	res->phase = D1_PHASE_ROLLED_BACK;
	d1_verifier_of(s->incarnation, res->verifier);
	return D1_OK;
}

/*
 * One journal event, whole.
 *
 * An event is durable or it never happened.  If the append or the flush
 * fails, the bytes the append wrote are discarded and the LSNs they
 * took are given back, so the next event cannot find an orphan in front
 * of it and the next flush cannot claim a record whose reducer was
 * undone.  A record that was already successfully claimed is never
 * touched: the rollback only ever goes back to the durable length.
 */
static bool d1_journal_event(struct d1_store *s, uint32_t type,
			     const uint8_t *body, uint32_t len)
{
	if (!d1_journal_append(&s->journal, type, body, len) ||
	    !d1_journal_flush(&s->journal)) {
		d1_journal_rollback(&s->journal);
		return false;
	}
	return true;
}

/*
 * The durable event for one ordinary entry.
 *
 * Section 9 wants the whole Envelope, the entry ordinal, the request
 * digest and the complete result in each ENTRY record, because recovery
 * re-executes and compares what it computed against what was logged.
 * A whole-request intent could not support that comparison: it says
 * what was asked for, not what happened.
 *
 * The Envelope bytes are handed in rather than encoded again here.
 * They are the bytes the digest beside them was taken over, so the
 * record carries one reading of the request and not two that happen to
 * agree; and they are the call's own, so no two callers share a buffer
 * to write them from.
 */
static bool d1_journal_entry_event(struct d1_store *s, const uint8_t *env_bytes,
				   size_t env_len, uint32_t ordinal,
				   const uint8_t digest[D1_DIGEST_BYTES],
				   const struct d1_complete_result *complete)
{
	uint8_t result_bytes[256];
	struct d1_cursor cur;
	size_t res_len;

	res_len = d1_complete_result_encode(complete, result_bytes,
					    sizeof(result_bytes));
	if (!env_len || !res_len)
		return false;

	d1_enc_init(&cur, s->record, s->record_cap);
	d1_enc_bytes(&cur, env_bytes, env_len);
	d1_enc_u32(&cur, ordinal);
	d1_enc_raw(&cur, digest, D1_DIGEST_BYTES);
	d1_enc_bytes(&cur, result_bytes, res_len);
	if (cur.bad)
		return false;
	return d1_journal_event(s, D1_REC_ENTRY, s->record, (uint32_t)cur.len);
}

/*
 * The durable event for a control, whether an Envelope's or a fixture's.
 *
 * An Envelope control carries its request digest, for the reason an
 * ENTRY carries one: replay recomputes the digest from the record's own
 * bytes, so without a logged one to compare against, a record whose
 * bytes were altered would simply hash to a different value and be
 * believed.  The ENTRY path has had that cross-check since the first
 * slice and the CONTROL path had not, which was an asymmetry and not a
 * decision.  A fixture control carries no digest: its request is not an
 * Envelope, nothing binds an operation key to it, and there is nothing
 * for a digest to be the identity of.
 */
static bool d1_journal_control_event(struct d1_store *s, uint32_t kind,
				     const uint8_t *request, size_t request_len,
				     const uint8_t digest[D1_DIGEST_BYTES],
				     const uint8_t *result, size_t result_len)
{
	struct d1_cursor cur;

	d1_enc_init(&cur, s->record, s->record_cap);
	d1_enc_u32(&cur, kind);
	d1_enc_bytes(&cur, request, request_len);
	if (kind == D1_CTL_ENVELOPE)
		d1_enc_raw(&cur, digest, D1_DIGEST_BYTES);
	d1_enc_bytes(&cur, result, result_len);
	if (cur.bad)
		return false;
	return d1_journal_event(s, D1_REC_CONTROL, s->record,
				(uint32_t)cur.len);
}

/*
 * Whether this caller is bound to this object at all.
 *
 * Replay asks the same question of the same rebuilt table.  The special
 * case that used to skip it was one more place where replay was not the
 * reducer that ran live, and it bought nothing: the authority the entry
 * ran under is in the log.
 */
static bool d1_binding_ok(struct d1_store *s, const struct d1_envelope *env)
{
	struct d1_admission *a = d1_admission_find(s, env->admission);

	return a && memcmp(&a->object, &env->object, sizeof(a->object)) == 0;
}

/*
 * Whether a handle is absent, or one this store issued of its domain.
 *
 * Absent is allowed here because absence is a shape question and the
 * shape check has already been made: an option that says it is present
 * and carries zero is not a canonical request, and one that says it is
 * absent names nothing to check.
 */
static bool d1_owns_admission(const struct d1_store *s, d1_admission_id id)
{
	return !d1_admission_live(id) ||
	       d1_handle_ours(s, id.raw, id._kind, id._instance,
			      D1_HANDLE_ADMISSION);
}

static bool d1_owns_txn(const struct d1_store *s, d1_txn_id id)
{
	return !d1_txn_live(id) ||
	       d1_handle_ours(s, id.raw, id._kind, id._instance, D1_HANDLE_TXN);
}

static bool d1_owns_version(const struct d1_store *s, d1_version_id id)
{
	return !d1_version_live(id) ||
	       d1_handle_ours(s, id.raw, id._kind, id._instance,
			      D1_HANDLE_VERSION);
}

static bool d1_owns_custody(const struct d1_store *s, d1_custody_id id)
{
	return !d1_custody_live(id) ||
	       d1_handle_ours(s, id.raw, id._kind, id._instance,
			      D1_HANDLE_CUSTODY);
}

static bool d1_owns_repair(const struct d1_store *s, d1_repair_id id)
{
	return !d1_repair_live(id) ||
	       d1_handle_ours(s, id.raw, id._kind, id._instance,
			      D1_HANDLE_REPAIR);
}

/*
 * Whether every handle this request names belongs to this store.
 *
 * Which store issued a handle is a fact about the C value and about
 * nothing the canonical form carries: an ENTRY or CONTROL record holds
 * the raw number and no issuer at all.  So a refusal decided on the
 * issuer is an answer replay cannot reach, and recording one leaves a
 * history the store cannot rebuild -- the log says the reducer was
 * asked about a number, replay adopts that number for the target,
 * finds the row and answers something else.
 *
 * The question is therefore asked once, here, before a key is looked
 * up, a receipt is reserved, a table is read or a frame is appended.
 * A request naming a handle of another store, or of another domain, is
 * not this store's request: it is refused, nothing is recorded, and
 * nothing in the log ever has to explain it.  After this, every handle
 * the reducer sees is the store's own, so the reducer compares raw
 * values -- which is what the log carries and all it carries.
 *
 * Replay reaches the reducer through the same door, having adopted
 * every decoded value for the store it is rebuilding, so what it
 * executes is a history that passed the door when it was live.
 *
 * An optional handle is asked about only when its presence tag says it
 * is there.  Presence is the tag and never a test of the value, and an
 * absent option's value is not part of the request: the encoder writes
 * only the tag, the digest binds only what the encoder wrote, and the
 * decoder leaves the slot zero.  Asking about it anyway made two
 * requests with the same canonical bytes and the same digest get
 * different answers, decided by memory the request does not carry --
 * which is the same shape this door exists to remove, one field over.
 */
static bool d1_envelope_owned(const struct d1_store *s,
			      const struct d1_envelope *env)
{
	uint32_t i, n;

	if (!d1_owns_admission(s, env->admission))
		return false;
	switch (env->op) {
	case D1_OP_WRITE_BATCH:
		/* A write names no handle this store ever issued. */
		return true;
	case D1_OP_FINALIZE_BATCH:
	case D1_OP_COMMIT_BATCH:
		n = env->body.lifecycle.count;
		if (n > D1_BATCH_ENTRIES_MAX)
			return false;
		for (i = 0; i < n; i++) {
			const struct d1_lifecycle_entry *e =
				&env->body.lifecycle.entries[i];

			if (!d1_owns_txn(s, e->txn))
				return false;
			if (e->predecessor_present &&
			    !d1_owns_version(s, e->predecessor))
				return false;
		}
		return true;
	case D1_OP_ROLLBACK_BATCH:
		n = env->body.rollback.count;
		if (n > D1_BATCH_ENTRIES_MAX)
			return false;
		for (i = 0; i < n; i++) {
			const struct d1_rollback_entry *e =
				&env->body.rollback.entries[i];

			if (!d1_owns_txn(s, e->txn))
				return false;
			if (e->visible_present &&
			    !d1_owns_version(s, e->visible))
				return false;
			if (e->predecessor_present &&
			    !d1_owns_version(s, e->predecessor))
				return false;
			if (e->custody_present &&
			    !d1_owns_custody(s, e->custody))
				return false;
		}
		return true;
	case D1_OP_RECOVERY_ADMIT:
	case D1_OP_LEASE_REAP:
		n = env->body.control.count;
		if (n > D1_BATCH_ENTRIES_MAX)
			return false;
		for (i = 0; i < n; i++)
			if (!d1_owns_txn(s, env->body.control.txns[i]))
				return false;
		if (!d1_owns_admission(s, env->body.control.old_admission))
			return false;
		return !env->body.control.new_admission_present ||
		       d1_owns_admission(s, env->body.control.new_admission);
	case D1_OP_MARK_ERROR:
	case D1_OP_BEGIN_REPAIR:
	case D1_OP_PREPARE_REPAIR:
	case D1_OP_FINALIZE_REPAIR:
	case D1_OP_COMMIT_REPAIR:
	case D1_OP_ABORT_REPAIR:
	case D1_OP_CLEAR_ERROR:
	case D1_OP_UNLOCK:
		n = env->body.repair.count;
		if (n > D1_BATCH_ENTRIES_MAX)
			return false;
		for (i = 0; i < n; i++) {
			const struct d1_repair_entry *e =
				&env->body.repair.entries[i];

			if (e->custody_present &&
			    !d1_owns_custody(s, e->custody))
				return false;
			if (e->successor_present &&
			    !d1_owns_version(s, e->successor))
				return false;
			if (e->predecessor_present &&
			    !d1_owns_version(s, e->predecessor))
				return false;
		}
		return !env->body.repair.cohort_present ||
		       d1_owns_repair(s, env->body.repair.cohort);
	default:
		/*
		 * A body this slice cannot read is a body it cannot vouch
		 * for.  The operation itself is refused elsewhere; this
		 * says only that nothing here proved its handles.
		 */
		return false;
	}
}

/*
 * Whether this operation key is already recorded under a different
 * request.
 *
 * The receipt key includes the entry ordinal, but the digest binds the
 * whole Envelope, and section 8 says a new lease or admission cannot
 * change an old request body under the old key.  Checking only the
 * ordinal being executed let a changed retry take effect for the
 * members the interrupted original had not reached: the key ended up
 * holding two receipts with two different digests, and the original
 * request could never be finished.
 *
 * So the question is asked of the key, not of one ordinal, and it is
 * asked before any member runs.  A key with no recorded member at all
 * is still unbound and available.
 */
static bool d1_key_conflicts(struct d1_store *s,
			     const struct d1_uuid *export_uuid,
			     const struct d1_opkey *key,
			     const uint8_t digest[D1_DIGEST_BYTES])
{
	uint32_t i;

	for (i = 0; i < D1_MAX_RECEIPTS; i++) {
		const struct d1_receipt *r = &s->receipts[i];

		if (!d1_receipt_keyed(r, export_uuid, key))
			continue;
		if (memcmp(r->digest, digest, D1_DIGEST_BYTES) != 0)
			return true;
	}
	return false;
}

/*
 * How many members of this key already carry a receipt.
 *
 * A live batch records member j before it reaches member j+1, stops at
 * the first member it could not record, and answers an exact repeat of
 * a recorded member from its receipt rather than logging it again.  So
 * the receipts a key holds are always the dense prefix 0..n-1, and the
 * next ENTRY a log may legitimately carry for that key is member n --
 * which is one comparison, and covers a repeated ordinal as well as a
 * skipped one, because a repeated ordinal is already inside the count.
 */
static uint32_t d1_key_recorded(struct d1_store *s,
				const struct d1_uuid *export_uuid,
				const struct d1_opkey *key)
{
	uint32_t i, n = 0;

	for (i = 0; i < D1_MAX_RECEIPTS; i++)
		if (d1_receipt_keyed(&s->receipts[i], export_uuid, key))
			n++;
	return n;
}

/*
 * Reserve a receipt slot before anything mutates.
 *
 * Section 8 makes an inability to record a receipt an UNRECORDED
 * failure with no state change.  Reserving first is what makes that
 * true: a store that mutates and then discovers it cannot record has
 * already told the caller nothing happened while something did.
 */
static struct d1_receipt *d1_receipt_reserve(struct d1_store *s)
{
	uint32_t i;

	for (i = 0; i < D1_MAX_RECEIPTS; i++) {
		if (s->receipts[i].used)
			continue;
		memset(&s->receipts[i], 0, sizeof(s->receipts[i]));
		s->receipts[i].used = true;
		return &s->receipts[i];
	}
	return NULL;
}

/*
 * The rights an operation needs, for the live path and for replay
 * alike.  There is one table, because two would eventually disagree --
 * and did: replay demanded WRITE for a rollback the live path had
 * correctly admitted on REPAIR plus custody, so a valid log would not
 * rebuild.
 *
 * Rollback answers zero because the right it needs depends on the phase
 * of the transaction it names, which only the handler knows: WRITE to
 * cancel one's own private work, REPAIR plus exact custody for
 * committed data.
 */
/* Whether @op is one of section 7's repair operations. */
static bool d1_op_is_repair(uint32_t op)
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

static uint32_t d1_op_rights(uint32_t op)
{
	switch (op) {
	case D1_OP_WRITE_BATCH:
	case D1_OP_FINALIZE_BATCH:
	case D1_OP_COMMIT_BATCH:
		return D1_RIGHT_WRITE;
	case D1_OP_ROLLBACK_BATCH:
		return 0;
	case D1_OP_RECOVERY_ADMIT:
	case D1_OP_LEASE_REAP:
		return D1_RIGHT_CONTROL;
	default:
		/*
		 * Section 2 asks a repair for REPAIR, and section 7 asks it
		 * for custody as well; the custody is the reducer's
		 * question because it names an exact version.
		 */
		return D1_RIGHT_REPAIR;
	}
}

/*
 * Run one ordinary entry, whole: lookup, reservation, revalidation,
 * transition, durable event and receipt, all inside one lock interval.
 * The caller holds the lock and releases it between entries, so the
 * next one is revalidated against whatever this one left.
 */
static void d1_apply_one(struct d1_store *s, const struct d1_envelope *env,
			 const uint8_t *bytes, size_t len, uint32_t ordinal,
			 const uint8_t digest[D1_DIGEST_BYTES], uint32_t need,
			 bool commit, struct d1_complete_result *complete)
{
	struct d1_entry_result *res = &complete->entry;
	struct d1_admission *a = NULL;
	struct d1_receipt *slot;
	struct d1_undo undo;
	uint32_t status;

	memset(complete, 0, sizeof(*complete));
	complete->key = env->key;
	complete->disposition = D1_COMPLETED;
	res->stability = D1_FILE_SYNC;
	res->disposition = D1_COMPLETED;
	d1_verifier_of(s->incarnation, res->verifier);

	/*
	 * Section 8: validate the caller binding, look the key up, compare
	 * the digest, and return the recorded result without mutation --
	 * even after an incarnation change, and before any question about
	 * whether the admission is still live.  Retrieving a result is not
	 * a new mutation.
	 */
	/*
	 * Provenance first, so nothing is read on behalf of a request that
	 * is not this store's to answer -- d1_binding_ok reads the
	 * admission table, and asking it first would be one table read
	 * before the door.
	 *
	 * On the public path this is a re-check of a decision already
	 * made, because the request reaching here is the copy the call
	 * took after the door passed.  It is kept as defence in depth and
	 * because replay enters here directly, having adopted a decoded
	 * record for this store: the same question, asked of a request
	 * that arrived by another road.
	 */
	if (!d1_envelope_owned(s, env) || !d1_binding_ok(s, env)) {
		res->status = D1_STALE_AUTH;
		res->disposition = D1_UNRECORDED;
		complete->disposition = D1_UNRECORDED;
		return;
	}
	/*
	 * The whole-Envelope binding is an invariant of this transition,
	 * not of the call that reached it.  One preflight in the public
	 * path is not enough: two callers can both pass it while the key
	 * is unused and then interleave their members, because the lock is
	 * released between members, and replay enters here directly and
	 * never preflights at all.  So the key is asked again inside the
	 * lock interval that goes on to reserve the receipt, and a member
	 * whose key already names a different Envelope mutates nothing.
	 *
	 * This subsumes the per-ordinal digest comparison that used to
	 * stand here: a receipt at this ordinal with another digest is one
	 * of the receipts this question already asks about.
	 */
	if (d1_key_conflicts(s, &env->object.export_uuid, &env->key, digest)) {
		res->status = D1_REPLAY_CONFLICT;
		return;
	}
	slot = d1_receipt_find(s, &env->object.export_uuid, &env->key, ordinal);
	if (slot) {
		/* The complete historical result, epoch and EOF included. */
		*complete = slot->result;
		return;
	}

	slot = d1_receipt_reserve(s);
	if (!slot) {
		res->status = D1_NOSPC;
		res->disposition = D1_UNRECORDED;
		complete->disposition = D1_UNRECORDED;
		return;
	}

	/*
	 * A canonical request whose rights or liveness fail is a semantic
	 * error, not the absence of a request: section 8 records every
	 * ordinary semantic error that has room for a receipt, and section
	 * 4 lists STALE_AUTH among the statuses that are COMPLETED.  Only
	 * caller binding and malformed input are refused before the
	 * lookup, above, and those leave nothing behind.
	 *
	 * So this joins the normal path -- complete result, durable event,
	 * receipt -- and an exact retry answers from that receipt while a
	 * changed body under the same key conflicts.
	 */
	d1_undo_begin(s, &undo);
	status = d1_admission_check(s, env, need, &a);
	if (status != D1_OK)
		goto record;

	if (env->op == D1_OP_WRITE_BATCH)
		status = d1_do_write_entry(s, env,
					   &env->body.write.entries[ordinal], a,
					   res, &undo);
	else if (env->op == D1_OP_ROLLBACK_BATCH)
		status = d1_do_rollback_entry(
			s, env, &env->body.rollback.entries[ordinal], a, res,
			&undo);
	else if (!d1_verifier_matches(s, env->body.lifecycle.prior_verifier))
		status = D1_STALE_AUTH;
	else
		status = d1_do_lifecycle_entry(
			s, env, &env->body.lifecycle.entries[ordinal], a,
			commit, res, &undo);

	/*
	 * NOSPC and IO are reserved for UNRECORDED failures: no logged
	 * operation produces them as a completed receipt.
	 */
	if (status == D1_NOSPC || status == D1_IO) {
		d1_undo_apply(s, &undo);
		slot->used = false;
		res->status = status;
		res->disposition = D1_UNRECORDED;
		complete->disposition = D1_UNRECORDED;
		return;
	}

	/*
	 * A semantic refusal is recorded, but it changes nothing: whatever
	 * the entry touched on its way to being refused -- an object it
	 * created on first touch, a slot it took -- goes back.  The result
	 * fields the handler filled are copies taken before it mutated, so
	 * they still describe what the caller was refused against.
	 */
	if (status != D1_OK && !undo.keep)
		d1_undo_apply(s, &undo);

record:
	res->status = status;
	complete->index_epoch = s->index_epoch;
	complete->eof = d1_eof_locked(s, &env->object);

	if (s->journaling && !s->replaying &&
	    !d1_journal_entry_event(s, bytes, len, ordinal, digest, complete)) {
		/*
		 * The event did not become durable, so nothing happened:
		 * the transition is put back, the reserved ID is given
		 * back, and no receipt is recorded.
		 */
		d1_undo_apply(s, &undo);
		slot->used = false;
		memset(complete, 0, sizeof(*complete));
		complete->key = env->key;
		complete->disposition = D1_UNRECORDED;
		res->stability = D1_FILE_SYNC;
		res->disposition = D1_UNRECORDED;
		res->status = D1_IO;
		d1_verifier_of(s->incarnation, res->verifier);
		return;
	}

	slot->export_uuid = env->object.export_uuid;
	slot->key = env->key;
	slot->ordinal = ordinal;
	memcpy(slot->digest, digest, D1_DIGEST_BYTES);
	slot->result = *complete;
}

/* The same, for a control operation, which is atomic as a whole. */
static void d1_apply_control(struct d1_store *s, const struct d1_envelope *env,
			     const uint8_t *bytes, size_t len,
			     const uint8_t digest[D1_DIGEST_BYTES],
			     struct d1_complete_result *complete)
{
	struct d1_entry_result *res = &complete->entry;
	struct d1_control_undo undo;
	struct d1_admission *a = NULL;
	struct d1_receipt *slot;
	uint8_t result_bytes[256];
	size_t res_len;
	uint32_t status;

	memset(&undo, 0, sizeof(undo));
	memset(complete, 0, sizeof(*complete));
	complete->key = env->key;
	complete->disposition = D1_COMPLETED;
	res->stability = D1_FILE_SYNC;
	res->disposition = D1_COMPLETED;
	d1_verifier_of(s->incarnation, res->verifier);

	/* Provenance first, for the reason in d1_apply_one. */
	if (!d1_envelope_owned(s, env) || !d1_binding_ok(s, env)) {
		res->status = D1_STALE_AUTH;
		res->disposition = D1_UNRECORDED;
		complete->disposition = D1_UNRECORDED;
		return;
	}
	/* The same invariant, asked in the same place; see d1_apply_one. */
	if (d1_key_conflicts(s, &env->object.export_uuid, &env->key, digest)) {
		res->status = D1_REPLAY_CONFLICT;
		return;
	}
	slot = d1_receipt_find(s, &env->object.export_uuid, &env->key, 0);
	if (slot) {
		*complete = slot->result;
		return;
	}
	slot = d1_receipt_reserve(s);
	if (!slot) {
		res->status = D1_NOSPC;
		res->disposition = D1_UNRECORDED;
		complete->disposition = D1_UNRECORDED;
		return;
	}

	status = d1_admission_check(s, env, d1_op_rights(env->op), &a);
	if (status == D1_OK)
		status = d1_do_control(s, env, a, res, &undo);
	if (status == D1_NOSPC || status == D1_IO) {
		d1_control_undo_apply(&undo);
		slot->used = false;
		res->status = status;
		res->disposition = D1_UNRECORDED;
		complete->disposition = D1_UNRECORDED;
		return;
	}
	/* As for an ordinary entry: recorded, but nothing changed. */
	if (status != D1_OK)
		d1_control_undo_apply(&undo);
	res->status = status;
	complete->index_epoch = s->index_epoch;
	complete->eof = d1_eof_locked(s, &env->object);

	if (s->journaling && !s->replaying) {
		res_len = d1_complete_result_encode(complete, result_bytes,
						    sizeof(result_bytes));
		if (!len || !res_len ||
		    !d1_journal_control_event(s, D1_CTL_ENVELOPE, bytes, len,
					      digest, result_bytes, res_len)) {
			d1_control_undo_apply(&undo);
			slot->used = false;
			memset(complete, 0, sizeof(*complete));
			complete->key = env->key;
			complete->disposition = D1_UNRECORDED;
			res->stability = D1_FILE_SYNC;
			res->disposition = D1_UNRECORDED;
			res->status = D1_IO;
			d1_verifier_of(s->incarnation, res->verifier);
			return;
		}
	}

	slot->export_uuid = env->object.export_uuid;
	slot->key = env->key;
	slot->ordinal = 0;
	memcpy(slot->digest, digest, D1_DIGEST_BYTES);
	slot->result = *complete;
}

/*
 * The same, for a repair operation.
 *
 * Section 8 gives a repair one cohort receipt rather than independently
 * replayable member commits, so this answers once for the whole call
 * exactly as a control does, and for the same reason: there is nothing
 * useful to say about half a repair.
 */
static void d1_apply_repair(struct d1_store *s, const struct d1_envelope *env,
			    const uint8_t *bytes, size_t len,
			    const uint8_t digest[D1_DIGEST_BYTES],
			    struct d1_complete_result *complete)
{
	struct d1_entry_result *res = &complete->entry;
	struct d1_repair_undo undo;
	struct d1_admission *a = NULL;
	struct d1_receipt *slot;
	uint8_t result_bytes[256];
	size_t res_len;
	uint32_t status;

	d1_repair_undo_begin(&undo, s);
	memset(complete, 0, sizeof(*complete));
	complete->key = env->key;
	complete->disposition = D1_COMPLETED;
	res->stability = D1_FILE_SYNC;
	res->disposition = D1_COMPLETED;
	d1_verifier_of(s->incarnation, res->verifier);

	/* Provenance first, for the reason in d1_apply_one. */
	if (!d1_envelope_owned(s, env) || !d1_binding_ok(s, env)) {
		res->status = D1_STALE_AUTH;
		res->disposition = D1_UNRECORDED;
		complete->disposition = D1_UNRECORDED;
		return;
	}
	if (d1_key_conflicts(s, &env->object.export_uuid, &env->key, digest)) {
		res->status = D1_REPLAY_CONFLICT;
		return;
	}
	slot = d1_receipt_find(s, &env->object.export_uuid, &env->key, 0);
	if (slot) {
		*complete = slot->result;
		return;
	}
	slot = d1_receipt_reserve(s);
	if (!slot) {
		res->status = D1_NOSPC;
		res->disposition = D1_UNRECORDED;
		complete->disposition = D1_UNRECORDED;
		return;
	}

	status = d1_admission_check(s, env, d1_op_rights(env->op), &a);
	if (status == D1_OK)
		status = d1_do_repair(s, env, a, res, &undo);
	if (status == D1_NOSPC || status == D1_IO) {
		d1_repair_undo_apply(s, &undo);
		slot->used = false;
		res->status = status;
		res->disposition = D1_UNRECORDED;
		complete->disposition = D1_UNRECORDED;
		return;
	}
	/*
	 * A refusal changes nothing, unless it is the one section 7 makes
	 * a transition in its own right; see d1_repair_undo.
	 */
	if (status != D1_OK && !undo.keep)
		d1_repair_undo_apply(s, &undo);
	res->status = status;
	complete->index_epoch = s->index_epoch;
	complete->eof = d1_eof_locked(s, &env->object);

	if (s->journaling && !s->replaying) {
		res_len = d1_complete_result_encode(complete, result_bytes,
						    sizeof(result_bytes));
		if (!len || !res_len ||
		    !d1_journal_control_event(s, D1_CTL_ENVELOPE, bytes, len,
					      digest, result_bytes, res_len)) {
			d1_repair_undo_apply(s, &undo);
			slot->used = false;
			memset(complete, 0, sizeof(*complete));
			complete->key = env->key;
			complete->disposition = D1_UNRECORDED;
			res->stability = D1_FILE_SYNC;
			res->disposition = D1_UNRECORDED;
			res->status = D1_IO;
			d1_verifier_of(s->incarnation, res->verifier);
			return;
		}
	}

	slot->export_uuid = env->object.export_uuid;
	slot->key = env->key;
	slot->ordinal = 0;
	memcpy(slot->digest, digest, D1_DIGEST_BYTES);
	slot->result = *complete;
}

/*
 * One request, owned by the call that executes it.
 *
 * A caller owns its envelope and its payload bytes, and the model has
 * no claim on either after the call returns -- so it must have its own
 * copy before it accepts anything, and it must have it before anything
 * else in the call can run.  It did not.  The digest was taken from the
 * caller's memory, every member was executed from that same memory
 * across lock intervals the call does not hold, and the door hook ran
 * first of all.  A caller that changed a transaction, a payload byte,
 * an index or an owner in one of those gaps had the changed request
 * executed and journalled under the digest of the request that was
 * there before, and a caller that changed its admission at the door had
 * a request of another store admitted as a local one.
 *
 * So the call takes the request before it does anything else, into
 * storage it owns, and reads the caller's envelope no more.  Two
 * representations come out of that one reading, because two different
 * questions are asked of a request and the canonical form answers only
 * one of them:
 *
 *   @own is the typed request as the caller wrote it, struct for
 *   struct.  It carries what the canonical form deliberately does not
 *   -- which store issued each handle, and of which domain -- so it is
 *   what provenance and binding are asked of.  It holds no pointer:
 *   the payload bytes its entries named are in @bytes and nowhere
 *   else, so there is nothing left in it that could still address the
 *   caller's memory.
 *
 *   @bytes is the canonical encoding of that same reading, and @env is
 *   decoded back out of it.  This is the encoding the digest binds and
 *   the journal records, so the bytes that were hashed, the bytes that
 *   are recorded and the request that runs are one reading of one
 *   request.  @env's payloads point into @bytes, which lives until the
 *   call returns.
 *
 * Provenance is decided on @own, before @env exists, because adoption
 * stamps this store on every handle and a handle of another store must
 * be refused rather than naturalised.  After adoption the request is
 * the store's own, which is exactly what replay arranges for a record
 * it has decoded.
 */
struct d1_request {
	struct d1_envelope own;
	uint8_t *bytes;
	size_t len;
	struct d1_envelope env;
	uint8_t digest[D1_DIGEST_BYTES];
};

/*
 * Read the caller's request once, and own what was read.
 *
 * This runs before the call is bracketed and therefore before the door
 * hook, which is the point: the hook is the caller's, it runs with no
 * lock held, and what it does to the caller's own memory afterwards
 * reaches nothing here.
 *
 * The encoding is the copy.  A separate arena for the payload bytes
 * would be a second copy of what @bytes already holds, and the entries
 * of @own would then point into it -- so the pointers go instead, all
 * of them, including the ones past the declared count.  Nothing below
 * asks @own for a payload: the shape test reads the lengths and the
 * presence of a pointer, provenance reads the handles, and the bytes
 * themselves are read from @bytes by the request that runs.
 *
 * A zero @len is a request the canonical form cannot express, which is
 * the shape refusal, made here rather than asked again of memory that
 * is no longer the caller's.  Returning false is the narrower failure:
 * there was no buffer to read the request into at all.
 */
static bool d1_request_snapshot(const struct d1_envelope *env,
				struct d1_request *r)
{
	uint32_t i;

	memset(r, 0, sizeof(*r));
	r->own = *env;
	r->bytes = malloc(D1_ENVELOPE_MAX);
	if (r->bytes)
		r->len = d1_envelope_encode(env, r->bytes, D1_ENVELOPE_MAX);
	/*
	 * Only a write batch has payload pointers, and the body is a
	 * union: clearing them for any other operation would clear that
	 * operation's members instead.
	 */
	if (r->own.op == D1_OP_WRITE_BATCH)
		for (i = 0; i < D1_BATCH_ENTRIES_MAX; i++)
			r->own.body.write.entries[i].payload = NULL;
	return r->bytes != NULL;
}

static void d1_request_release(struct d1_request *r)
{
	free(r->bytes);
	r->bytes = NULL;
}

/* Defined with the replay path, which adopts a decoded record the same way. */
static void d1_envelope_adopt(const struct d1_store *s,
			      struct d1_envelope *env);

/*
 * Make the execution copy, out of the bytes the digest binds.
 *
 * The request was read and encoded before the call was bracketed; this
 * decodes it back, adopts it for this store and hashes the same bytes,
 * so what runs is what was hashed and what will be recorded.
 */
static bool d1_request_take(const struct d1_store *s, struct d1_request *r)
{
	if (!d1_envelope_decode(r->bytes, r->len, &r->env))
		return false;
	/* The request becomes this store's, exactly as replay's does. */
	d1_envelope_adopt(s, &r->env);
	d1_request_digest(r->bytes, r->len, r->digest);
	return true;
}

uint32_t d1_store_apply(struct d1_store *s, const struct d1_envelope *env,
			struct d1_result *out)
{
	uint8_t digest[D1_DIGEST_BYTES];
	struct d1_complete_result complete;
	struct d1_request req;
	uint32_t need, count, i;
	bool commit, conflict, owned, taken, read;

	memset(out, 0, sizeof(*out));
	/*
	 * Nothing has been recorded yet, and the early returns below --
	 * a closed store, an operation this slice cannot express, a
	 * malformed request -- record nothing at all.  The member loop
	 * sets this from its members when there are members; until then
	 * the honest answer is that the operation left no record.
	 */
	out->disposition = D1_UNRECORDED;

	/*
	 * The request is read and owned first, before the call is
	 * bracketed and so before the door hook the bracket runs.  That
	 * hook is a caller's, it runs with no lock held, and it used to
	 * run while the request was still the caller's memory: a foreign
	 * admission replaced with a local one there was admitted, and an
	 * operation replaced there was the operation that ran.  Nothing
	 * below reads @env again -- the request is @req from here.
	 */
	read = d1_request_snapshot(env, &req);
	env = &req.own;
	out->key = env->key;

	/*
	 * The call is bracketed for its whole length, not for each of its
	 * lock intervals: a close must not slip into the gap between two
	 * members of a batch.
	 */
	if (!d1_call_enter(s)) {
		d1_request_release(&req);
		return D1_INVALID;
	}

	switch (env->op) {
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
		/* One table, shared with replay; see d1_op_rights. */
		need = d1_op_rights(env->op);
		break;
	default:
		/*
		 * The repair operations that have no transition yet say so
		 * rather than doing part of the job, and so does anything
		 * outside this model.
		 */
		d1_call_leave(s);
		d1_request_release(&req);
		return D1_UNSUPPORTED;
	}

	/*
	 * Shape first.  A request whose counts, lengths, tags or members
	 * are outside the canonical form is refused before anything reads
	 * the store.  The test was made when the request was read: the
	 * canonical encoding is the copy, and a request the canonical form
	 * cannot express has no encoding and no length.
	 */
	if (read && !req.len) {
		d1_call_leave(s);
		d1_request_release(&req);
		return D1_INVALID;
	}

	commit = env->op == D1_OP_COMMIT_BATCH;
	switch (env->op) {
	case D1_OP_WRITE_BATCH:
		count = env->body.write.count;
		break;
	case D1_OP_ROLLBACK_BATCH:
		count = env->body.rollback.count;
		break;
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
		/*
		 * A control operation answers once for the whole of it, and
		 * so does a repair; see section 8's cohort receipt.
		 */
		count = 1;
		break;
	default:
		count = env->body.lifecycle.count;
		break;
	}
	/*
	 * The count is the request's own claim until the shape test has
	 * passed, and the refusal below is the one case that answers
	 * without it: bound it to the results there are room for.
	 */
	if (count > D1_BATCH_ENTRIES_MAX)
		count = D1_BATCH_ENTRIES_MAX;
	out->count = count;

	/*
	 * There was no buffer to read the request into, so there is no
	 * request: nothing has been looked up, reserved or recorded, and
	 * the answer is the retryable one.
	 */
	if (!read) {
		for (i = 0; i < count; i++) {
			struct d1_entry_result *res = &out->entries[i];

			memset(res, 0, sizeof(*res));
			res->status = D1_NOSPC;
			res->stability = D1_FILE_SYNC;
			res->disposition = D1_UNRECORDED;
			d1_store_verifier(s, res->verifier);
		}
		out->disposition = D1_UNRECORDED;
		d1_call_leave(s);
		d1_request_release(&req);
		return D1_OK;
	}

	/*
	 * One lock interval settles three things in the order section 8
	 * gives them: whose request this is, whether the caller is bound
	 * to the object it names, and only then whether the key is in use.
	 *
	 * Provenance and binding are asked of the request as it was read,
	 * which is the only place the answer exists: the canonical form
	 * carries a handle's number and not which store issued it, and
	 * taking the request adopts every handle for this store, so a
	 * handle of another store must be refused before that rather than
	 * naturalised by it.  A request that is not this store's is told
	 * nothing about whose key is in use -- it never reaches the
	 * lookup.
	 */
	pthread_mutex_lock(&s->lock);
	owned = d1_envelope_owned(s, env) && d1_binding_ok(s, env);
	taken = owned && d1_request_take(s, &req);
	conflict = taken && d1_key_conflicts(s, &req.env.object.export_uuid,
					     &req.env.key, req.digest);
	pthread_mutex_unlock(&s->lock);

	if (owned && !taken) {
		/*
		 * The request does not survive its own encoding.  Nothing
		 * was looked up and nothing reserved.
		 */
		d1_call_leave(s);
		d1_request_release(&req);
		out->count = 0;
		out->disposition = D1_UNRECORDED;
		return D1_INVALID;
	}
	if (!owned || conflict) {
		for (i = 0; i < count; i++) {
			struct d1_entry_result *res = &out->entries[i];

			memset(res, 0, sizeof(*res));
			res->status = owned ? D1_REPLAY_CONFLICT :
					      D1_STALE_AUTH;
			res->stability = D1_FILE_SYNC;
			res->disposition = owned ? D1_COMPLETED : D1_UNRECORDED;
			d1_store_verifier(s, res->verifier);
		}
		out->disposition = owned ? D1_COMPLETED : D1_UNRECORDED;
		d1_call_leave(s);
		d1_request_release(&req);
		return D1_OK;
	}

	/* From here the request is the store's own copy. */
	env = &req.env;
	memcpy(digest, req.digest, D1_DIGEST_BYTES);

	if (need == D1_RIGHT_CONTROL || d1_op_is_repair(env->op)) {
		/*
		 * The gap the fixture can open: everything decided above
		 * was decided under a lock this call no longer holds.
		 */
		d1_run_member_hook(s, 0);
		pthread_mutex_lock(&s->lock);
		if (d1_op_is_repair(env->op))
			d1_apply_repair(s, env, req.bytes, req.len, digest,
					&complete);
		else
			d1_apply_control(s, env, req.bytes, req.len, digest,
					 &complete);
		pthread_mutex_unlock(&s->lock);
		out->entries[0] = complete.entry;
		out->index_epoch = complete.index_epoch;
		out->eof = complete.eof;
		out->disposition = complete.disposition;
		d1_call_leave(s);
		d1_request_release(&req);
		return D1_OK;
	}

	for (i = 0; i < count; i++) {
		/*
		 * The gap before this member: at ordinal zero it is the gap
		 * left by the preflight above, and afterwards it is the gap
		 * this loop leaves between two members.  Everything decided
		 * outside the lock below was decided in a lock interval
		 * this call no longer holds.
		 */
		d1_run_member_hook(s, i);

		/*
		 * One lock interval per member, covering its validation,
		 * transition, durable event and receipt; released between
		 * members so the next is revalidated against what this one
		 * left.
		 */
		pthread_mutex_lock(&s->lock);
		d1_apply_one(s, env, req.bytes, req.len, i, digest, need,
			     commit, &complete);
		pthread_mutex_unlock(&s->lock);
		out->entries[i] = complete.entry;
		out->index_epoch = complete.index_epoch;
		out->eof = complete.eof;

		/*
		 * Section 8 describes a batch "interrupted after entry j":
		 * an infrastructure failure stops the batch there, and the
		 * exact retry reconstructs what was recorded and evaluates
		 * the rest in input order.  Carrying on past it would put
		 * later members in the log ahead of the one that has not
		 * happened yet, so log order would stop being input order.
		 *
		 * The disposition is the whole test, not the status.  Any
		 * UNRECORDED member is a member that left no receipt, and
		 * the dense prefix replay relies on is the claim that the
		 * receipts under a key are 0..n-1 -- so recording a later
		 * member over the hole an earlier one left writes a log
		 * this store cannot replay, for the rest of its life.
		 *
		 * The status was checked here once, and missed the member a
		 * live call could then produce on its own: caller binding
		 * was re-asked in every member's interval, so an admission
		 * installed between members turned member 0's STALE_AUTH
		 * into member 1's receipt at ordinal 1.  That schedule is
		 * gone -- binding is settled once, at the door, before the
		 * key is looked up -- and no live call now reaches this
		 * with an UNRECORDED member whose status is not NOSPC or
		 * IO.  The disposition stays the test anyway: it is the
		 * property the dense prefix needs, the status is a
		 * consequence of today's refusal set, and a member that
		 * records nothing must stop the batch whatever it was
		 * refused for.
		 *
		 * A semantic refusal is not an interruption: it is a result,
		 * it has a receipt, and independent later members still run.
		 */
		if (complete.disposition == D1_UNRECORDED) {
			for (i++; i < count; i++) {
				struct d1_entry_result *rest = &out->entries[i];

				memset(rest, 0, sizeof(*rest));
				rest->status = complete.entry.status;
				rest->stability = D1_FILE_SYNC;
				rest->disposition = D1_UNRECORDED;
				d1_verifier_of(s->incarnation, rest->verifier);
			}
			break;
		}
	}
	/*
	 * The operation's disposition, from the members: COMPLETED when
	 * any of them recorded, UNRECORDED when none did.  A batch stops
	 * at the first member it could not record, so the first member
	 * settles it; see struct d1_result.
	 */
	out->disposition = out->entries[0].disposition;
	d1_call_leave(s);
	d1_request_release(&req);
	return D1_OK;
}

/*
 * Whether @a may read @object.  Reads are checked against the same
 * admission table as writes; a revoked or expired admission cannot open
 * a view, and one issued for a different object cannot either.
 */
static uint32_t d1_read_admission(struct d1_store *s, d1_admission_id admission,
				  const struct d1_objkey *object, bool private,
				  struct d1_admission **out)
{
	struct d1_admission *a = d1_admission_find(s, admission);

	if (!a || a->revoked || a->expired)
		return D1_STALE_AUTH;
	if (memcmp(&a->object, object, sizeof(*object)) != 0)
		return D1_STALE_AUTH;
	if ((a->rights & D1_RIGHT_READ) != D1_RIGHT_READ)
		return D1_STALE_AUTH;
	/*
	 * A START fences the old incarnation's handles.  Section 9 says
	 * retained pending and finalized versions "need explicit
	 * recovery_admit before owner reads or new lifecycle work", so a
	 * handle from before the reopen cannot select private data even
	 * though the transaction still records it.
	 *
	 * This is deliberately narrower than fencing every read: the memo
	 * names owner reads, and whether an ordinary committed read by an
	 * old handle should also be fenced is a separate question this
	 * model does not answer here.
	 */
	if (private && a->incarnation != s->incarnation)
		return D1_STALE_AUTH;
	*out = a;
	return D1_OK;
}

/*
 * Resolve and validate the whole OWNER vector, before anything is
 * selected or pinned.
 *
 * Section 6: one inadmissible member fails the WHOLE view.  Checking a
 * member only when its chunk comes up made that depend on order -- a
 * later member naming the same chunk was never reached, so
 * [FINALIZED, COMMITTED] passed and [COMMITTED, FINALIZED] did not.
 * The same vector cannot be admissible one way round and not the other.
 *
 * An owner triple is not an authorisation token either: every named
 * transaction must be this caller's, under the handle opening the view
 * and the epoch granted for it.  PREPARED is never selectable, and this
 * model does not substitute a COMMITTED member -- a view that wants
 * committed data asks for an ordinary one.  Ordinary visible versions
 * still fill the chunks the vector does not name.
 */
static uint32_t d1_owner_resolve(struct d1_store *s,
				 const struct d1_selection_spec *sel,
				 const struct d1_admission *a, uint32_t object,
				 bool *present, uint64_t *chosen)
{
	uint32_t i, j;

	for (i = 0; i < sel->count; i++) {
		struct d1_txn *t = d1_txn_find(s, sel->txns[i]);
		struct d1_version *v;

		/* One vector never names one transaction twice. */
		for (j = 0; j < i; j++)
			if (d1_txn_eq(sel->txns[j], sel->txns[i]))
				return D1_INVALID;
		if (!t || t->object != object || t->index >= D1_MAX_CHUNKS)
			return D1_INVALID;
		if (t->owner.cohort.raw != sel->owners[i].cohort.raw ||
		    t->owner.writer != sel->owners[i].writer ||
		    t->owner.co_id != sel->owners[i].co_id)
			return D1_OWNER_CONFLICT;
		if (t->admission != a->id)
			return D1_STALE_AUTH;
		if (t->read_epoch != sel->read_epoch)
			return D1_STALE_AUTH;
		if (t->phase != D1_PHASE_FINALIZED)
			return D1_BAD_PHASE;
		/* Two members resolving to one chunk is not a selection. */
		if (present[t->index])
			return D1_INVALID;
		v = d1_version_find(s, d1_version_of(s, t->version));
		if (!v)
			return D1_INVALID;
		present[t->index] = true;
		chosen[t->index] = v->id;
	}
	return D1_OK;
}

static void d1_view_unpin(struct d1_store *s, struct d1_view *v)
{
	uint32_t i;

	for (i = 0; i < D1_MAX_CHUNKS; i++) {
		struct d1_version *ver;

		if (!v->present[i])
			continue;
		ver = d1_version_find(s, d1_version_of(s, v->version[i]));
		if (ver && ver->pins)
			ver->pins--;
		v->present[i] = false;
	}
}

uint32_t d1_view_open(struct d1_store *s, const struct d1_objkey *object,
		      d1_admission_id admission,
		      const struct d1_selection_spec *sel, uint64_t byte_begin,
		      uint64_t byte_end, struct d1_view **out)
{
	struct d1_admission *a = NULL;
	struct d1_object *o;
	struct d1_view *v = NULL;
	/*
	 * The read's own copy of what it was asked for.  A read is a
	 * request too, and the door hook the bracket runs is the caller's:
	 * the vector was checked here and read again after that hook, so
	 * a count raised in between was a count the resolver believed and
	 * the fixed vector it indexes is sixteen members long.  The copy
	 * is taken before the bracket, and nothing below reads @sel or
	 * @object again.
	 */
	struct d1_selection_spec want = *sel;
	struct d1_objkey key = *object;
	/* The version each chunk resolves to, settled before anything is
	 * pinned. */
	bool chosen_present[D1_MAX_CHUNKS] = { false };
	uint64_t chosen[D1_MAX_CHUNKS] = { 0 };
	uint64_t first, last;
	uint32_t status, i;

	*out = NULL;
	if (want.selection != D1_SELECT_ORDINARY &&
	    want.selection != D1_SELECT_OWNER)
		return D1_INVALID;
	if (want.selection == D1_SELECT_OWNER &&
	    (want.count < D1_BATCH_ENTRIES_MIN ||
	     want.count > D1_BATCH_ENTRIES_MAX))
		return D1_INVALID;
	if (want.selection == D1_SELECT_ORDINARY && want.count != 0)
		return D1_INVALID;
	if (byte_begin >= byte_end)
		return D1_INVALID;
	if (!d1_call_enter(s))
		return D1_INVALID;

	pthread_mutex_lock(&s->lock);
	if (byte_end > s->max_file_bytes) {
		status = D1_INVALID;
		goto out;
	}
	status = d1_read_admission(s, admission, &key,
				   want.selection == D1_SELECT_OWNER, &a);
	if (status != D1_OK)
		goto out;
	/*
	 * An object nobody has written yet is an object at EOF zero, not a
	 * missing one.  This API has no create: the store's geometry and
	 * the admission are the whole of an object's existence before its
	 * first write, and section 3 gives it initial EOF zero.  So an
	 * ordinary view of it opens, captures that zero, holds no extent
	 * and reads no bytes -- and takes no object slot, because a read
	 * is not journalled and a read that spent model capacity would be
	 * a read that changed the store.
	 *
	 * OWNER selection is the other question: it names transactions,
	 * and an object with no writes has none to name.
	 */
	o = d1_object_find(s, &key);
	if (!o && want.selection == D1_SELECT_OWNER) {
		status = D1_INVALID;
		goto out;
	}
	/*
	 * The byte range names the chunks it touches; it is not one.  A
	 * range that reaches past the table is not malformed for that
	 * reason: nothing above the table can hold a version, so those
	 * indices are simply above the captured EOF, which is an answer
	 * and not an error.  The loop below never reaches them.
	 */
	first = byte_begin / s->chunk_bytes;
	last = (byte_end - 1u) / s->chunk_bytes;
	/*
	 * The whole selection is settled before a view slot is taken, so a
	 * refused vector leaves no view and no pins to clean up.
	 */
	if (want.selection == D1_SELECT_OWNER) {
		status = d1_owner_resolve(s, &want, a, d1_object_slot(s, o),
					  chosen_present, chosen);
		if (status != D1_OK)
			goto out;
	}
	for (i = 0; i < D1_MAX_VIEWS && !v; i++)
		if (!s->views[i].used)
			v = &s->views[i];
	if (!v) {
		status = D1_NOSPC;
		goto out;
	}

	memset(v, 0, sizeof(*v));
	v->used = true;
	v->store = s;
	/* No slot to name when the object has not been created. */
	v->object = o ? d1_object_slot(s, o) : D1_MAX_OBJECTS;
	v->range_begin = byte_begin;
	v->range_end = byte_end;

	/*
	 * Two passes, because the two questions are different.  The extent
	 * of the whole effective object decides where EOF and the holes
	 * are, and every chunk answers it.  What the view may read is
	 * decided by the window it was opened over, and only those chunks
	 * are verified and pinned -- a view does not keep alive bytes it
	 * cannot return.
	 */
	for (i = 0; o && i < D1_MAX_CHUNKS; i++) {
		struct d1_chunk *c = &o->chunks[i];
		struct d1_version *ver = NULL;
		uint64_t start, end;

		if (chosen_present[i])
			ver = d1_version_find(s, d1_version_of(s, chosen[i]));
		if (!ver && c->visible_present)
			ver = d1_version_find(s, d1_version_of(s, c->visible));
		if (!ver)
			continue;
		v->extent_present[i] = true;
		v->extent_len[i] = ver->len;
		if (d1_mul_u64(i, s->chunk_bytes, &start) &&
		    d1_add_u64(start, ver->len, &end) && end > v->eof)
			v->eof = end;

		if (i < first || i > last)
			continue;
		/*
		 * Section 7 quarantines the version an uncleared episode
		 * names, so the window a view reads may not contain one.
		 * One inadmissible member fails the whole view, as section
		 * 6 requires, and the pins it had taken go back.
		 *
		 * The extent above is deliberately still counted.  EOF is
		 * derived from every chunk of the object and is a length,
		 * not the data the episode put in doubt; refusing to answer
		 * it would make a quarantine change the shape of the file
		 * rather than the readability of one chunk.
		 */
		if (!d1_chunk_readable(c)) {
			d1_view_unpin(s, v);
			v->used = false;
			status = D1_QUARANTINED;
			goto out;
		}
		/*
		 * A view that would hand back bytes it cannot vouch for is
		 * not opened at all.
		 */
		if (!d1_checksum_verify(&ver->checksum, ver->bytes, ver->len)) {
			d1_view_unpin(s, v);
			v->used = false;
			status = D1_CHECKSUM;
			goto out;
		}
		ver->pins++;
		v->present[i] = true;
		v->version[i] = ver->id;
	}
	v->index_epoch = s->index_epoch;

	*out = v;
	status = D1_OK;
out:
	pthread_mutex_unlock(&s->lock);
	d1_call_leave(s);
	return status;
}

bool d1_view_version(const struct d1_view *v, uint64_t index,
		     d1_version_id *ver)
{
	if (!v || index >= D1_MAX_CHUNKS || !v->present[index])
		return false;
	*ver = d1_version_of(v->store, v->version[index]);
	return true;
}

uint64_t d1_view_eof(const struct d1_view *v)
{
	return v->eof;
}

uint32_t d1_view_read(struct d1_view *v, uint64_t offset, uint8_t *buf,
		      uint32_t len, uint32_t *out_len)
{
	struct d1_store *s = v->store;
	uint64_t at = offset;
	uint64_t limit;
	uint32_t done = 0;

	*out_len = 0;
	if (!v->used || !s)
		return D1_INVALID;
	/* A read outside the range the view covers is not a hole. */
	if (offset < v->range_begin || offset >= v->range_end)
		return D1_INVALID;
	if (offset >= v->eof || len == 0)
		return D1_OK;

	/*
	 * Bounded by the range the view covers and by the EOF of the whole
	 * effective object it captured -- not by the highest chunk inside
	 * the window, which would report a hole under a higher chunk as
	 * end of file.
	 */
	limit = v->eof < v->range_end ? v->eof : v->range_end;
	if (limit - offset < (uint64_t)len)
		len = (uint32_t)(limit - offset);

	pthread_mutex_lock(&s->lock);
	while (done < len) {
		uint64_t index = at / s->chunk_bytes;
		uint32_t within = (uint32_t)(at % s->chunk_bytes);
		uint32_t span = s->chunk_bytes - within;
		struct d1_version *ver = NULL;

		if (span > len - done)
			span = len - done;
		if (index < D1_MAX_CHUNKS && v->present[index])
			ver = d1_version_find(
				s, d1_version_of(s, v->version[index]));
		/*
		 * A chunk inside the window is either pinned or a hole; one
		 * outside it never reaches here, because the read is clipped
		 * to the range above.
		 */
		/*
		 * A hole below the view's EOF is zeros, and so is the part
		 * of a chunk past a partial image.  Both are answers, not
		 * short reads.
		 */
		memset(buf + done, 0, span);
		if (ver && within < ver->len) {
			uint32_t have = ver->len - within;

			if (have > span)
				have = span;
			memcpy(buf + done, ver->bytes + within, have);
		}
		done += span;
		at += span;
	}
	pthread_mutex_unlock(&s->lock);
	*out_len = done;
	return D1_OK;
}

/*
 * Release a view, through the store it was opened on.
 *
 * This used to take the store as well, with nothing saying the two had
 * to match and nothing checking that they did.  A caller could close a
 * view of A while naming B: the call locked B, walked B's versions
 * while clearing A's view fields, and marked the view unused -- after
 * which A saw no live view, let a close and a destroy through, and the
 * caller was left holding a view into freed storage.  That is an
 * ownership precondition the design never had, invented by the extra
 * argument, and the way to retire it is to stop asking for it: a view
 * records the store it came from, and that is the only store that can
 * release it.
 */
void d1_view_close(struct d1_view *v)
{
	struct d1_store *s;

	if (!v || !v->store)
		return;
	s = v->store;
	pthread_mutex_lock(&s->lock);
	d1_view_unpin(s, v);
	v->used = false;
	pthread_mutex_unlock(&s->lock);
}

/*
 * Fixture authority, as durable control events.
 *
 * Which handle is live, which custody binds which version and which
 * predecessor has been released all change what the reducer answers.
 * So each of these is logged, and a rebuild re-executes it rather than
 * inventing it.  Each has a locked core the rebuild calls directly and
 * a public form that takes the lock and writes the event.
 */
static d1_admission_id d1_admit_locked(struct d1_store *s,
				       const struct d1_objkey *object,
				       const struct d1_fixture_authority *auth)
{
	uint32_t i;

	/* Reserved writer IDs are never issued. */
	if (auth->writer == D1_WRITER_RESERVED_LOW ||
	    auth->writer == D1_WRITER_RESERVED_HIGH)
		return d1_admission_of(s, 0);
	/* And a counter that has run out issues nothing rather than wrap. */
	if (s->next_admission == UINT64_MAX)
		return d1_admission_of(s, 0);
	for (i = 0; i < D1_MAX_ADMISSIONS; i++) {
		struct d1_admission *a = &s->admissions[i];

		if (a->used)
			continue;
		memset(a, 0, sizeof(*a));
		a->used = true;
		a->id = s->next_admission++;
		a->issuer = auth->issuer;
		a->object = *object;
		a->principal = auth->principal;
		a->writer = auth->writer;
		memcpy(a->stateid, auth->stateid, sizeof(a->stateid));
		memcpy(a->session, auth->session, sizeof(a->session));
		a->lease_epoch = auth->lease_epoch;
		a->authority_epoch = auth->authority_epoch;
		a->fence_sequence = auth->fence_sequence;
		a->rights = auth->rights;
		a->incarnation = s->incarnation;
		return d1_admission_of(s, a->id);
	}
	return d1_admission_of(s, 0);
}

static bool d1_revoke_locked(struct d1_store *s, d1_admission_id admission)
{
	struct d1_admission *a = d1_admission_find(s, admission);

	if (!a)
		return false;
	a->revoked = true;
	return true;
}

static bool d1_expire_locked(struct d1_store *s, d1_admission_id admission)
{
	struct d1_admission *a = d1_admission_find(s, admission);

	if (!a)
		return false;
	a->expired = true;
	return true;
}

/*
 * Issue repair custody over an existing version.
 *
 * The fixture is the harness, not a production authority, but it is
 * still not allowed to bind a handle to an ID that has not been
 * allocated: such a handle would silently become valid later, when the
 * counter reached it.  Custody names a version that is there now.
 */
static d1_custody_id d1_custody_locked(struct d1_store *s,
				       d1_version_id version)
{
	uint32_t i;

	if (!d1_version_find(s, version))
		return d1_custody_of(s, 0);
	if (s->next_custody == UINT64_MAX)
		return d1_custody_of(s, 0);
	for (i = 0; i < D1_MAX_CUSTODY; i++) {
		struct d1_custody *c = &s->custody[i];

		if (c->used)
			continue;
		c->used = true;
		c->id = s->next_custody++;
		c->version = version.raw;
		return d1_custody_of(s, c->id);
	}
	return d1_custody_of(s, 0);
}

/*
 * Release one predecessor's retention root.
 *
 * Eligibility is a question about durable state alone: the version is
 * not visible, no uncommitted transaction is that version or records it
 * as its predecessor, and it has not been released already.  Live read
 * pins are deliberately NOT part of this test -- a pin keeps the
 * immutable bytes alive for the view that holds it, and says nothing
 * about whether a future rollback may still reach the version.  Making
 * release depend on pins would make the durable history depend on the
 * order in which views happened to close.
 *
 * Physical reclamation is a declared no-op in this model: bytes are
 * retained conservatively for the life of the store.  That is a stated
 * limitation of the model's capacity, not a licence to answer the
 * logical question differently.
 */
static bool d1_release_locked(struct d1_store *s, d1_version_id version)
{
	struct d1_version *v = d1_version_find(s, version);
	uint32_t i, c;

	if (!v || v->released)
		return false;
	for (i = 0; i < D1_MAX_OBJECTS; i++) {
		if (!s->objects[i].used)
			continue;
		for (c = 0; c < D1_MAX_CHUNKS; c++)
			if (s->objects[i].chunks[c].visible_present &&
			    s->objects[i].chunks[c].visible == version.raw)
				return false;
	}
	for (i = 0; i < D1_MAX_TXNS; i++) {
		const struct d1_txn *t = &s->txns[i];

		if (!t->used)
			continue;
		if (t->phase != D1_PHASE_PREPARED &&
		    t->phase != D1_PHASE_FINALIZED)
			continue;
		/*
		 * An uncommitted transaction that would restore this version
		 * on cancellation still depends on it, which is a different
		 * question from whether the version is itself private.
		 */
		if (t->version == version.raw)
			return false;
		if (t->predecessor_present && t->predecessor == version.raw)
			return false;
	}
	v->released = true;
	return true;
}

/*
 * Fixture fault control: the next journal snapshot finds no memory.
 * The snapshot is the one observer that has to allocate, so this is the
 * only way to reach its failure answer on purpose.
 */
void d1_fixture_fail_next_snapshot(struct d1_store *s)
{
	pthread_mutex_lock(&s->lock);
	if (!d1_store_serving(s)) {
		pthread_mutex_unlock(&s->lock);
		return;
	}
	if (!s->replaying)
		s->fail_next_snapshot = true;
	pthread_mutex_unlock(&s->lock);
}

/*
 * Fixture arms: answer as though the ID counters, or the index epoch,
 * had nothing left to give.
 *
 * These exist because the exhausted states are real states of the
 * contract that no history reaches: the tables are fixed size, rows are
 * never freed, and nothing allocates two to the sixty-fourth of
 * anything.  They are arms and not settings.  An arm can only ever
 * produce the NOSPC refusal the exhausted state produces, so it has no
 * reducer-visible durable effect: no counter moves, no epoch moves, no
 * result carries a value it chose, and the log gains nothing.  Turning
 * one off does not restore a number -- there is no number to restore --
 * it resumes from whatever the history has derived.
 *
 * Setting a value directly, which is what these replaced, did have such
 * an effect: it placed a number later results were derived from, wrote
 * no event to say so, and left a log that could not rebuild its store.
 */
void d1_fixture_exhaust_ids(struct d1_store *s, bool on)
{
	pthread_mutex_lock(&s->lock);
	if (!d1_store_serving(s)) {
		pthread_mutex_unlock(&s->lock);
		return;
	}
	if (!s->replaying)
		s->exhaust_ids = on;
	pthread_mutex_unlock(&s->lock);
}

void d1_fixture_exhaust_epoch(struct d1_store *s, bool on)
{
	pthread_mutex_lock(&s->lock);
	if (!d1_store_serving(s)) {
		pthread_mutex_unlock(&s->lock);
		return;
	}
	if (!s->replaying)
		s->exhaust_epoch = on;
	pthread_mutex_unlock(&s->lock);
}

void d1_fixture_fail_next_index(struct d1_store *s)
{
	pthread_mutex_lock(&s->lock);
	if (!d1_store_serving(s)) {
		pthread_mutex_unlock(&s->lock);
		return;
	}
	if (!s->replaying)
		s->fail_next_index = true;
	pthread_mutex_unlock(&s->lock);
}

bool d1_store_overlay_active(struct d1_store *s)
{
	bool active;

	pthread_mutex_lock(&s->lock);
	active = d1_store_serving(s) && s->overlay_active;
	pthread_mutex_unlock(&s->lock);
	return active;
}

/*
 * What the materialized index still says, for a test to prove that a
 * read did not consult it.  There is no production use for this.
 */
bool d1_store_materialized(struct d1_store *s, const struct d1_objkey *object,
			   uint64_t index, d1_version_id *version)
{
	struct d1_object *o;
	bool found = false;

	pthread_mutex_lock(&s->lock);
	o = d1_store_serving(s) ? d1_object_find(s, object) : NULL;
	if (o && index < D1_MAX_CHUNKS &&
	    o->chunks[index].materialized_present) {
		*version = d1_version_of(s, o->chunks[index].materialized);
		found = true;
	}
	pthread_mutex_unlock(&s->lock);
	return found;
}

/* Write one fixture control event.  Called with the lock held. */
static bool d1_journal_fixture(struct d1_store *s,
			       const struct d1_control_request *request,
			       const struct d1_control_result *result)
{
	uint8_t request_bytes[256];
	uint8_t result_bytes[64];
	size_t request_len, result_len;

	if (!s->journaling || s->replaying)
		return true;
	request_len = d1_control_request_encode(request, request_bytes,
						sizeof(request_bytes));
	result_len = d1_control_result_encode(result, result_bytes,
					      sizeof(result_bytes));
	if (!request_len || !result_len)
		return false;
	return d1_journal_control_event(s, request->kind, request_bytes,
					request_len, NULL, result_bytes,
					result_len);
}

d1_admission_id d1_fixture_admission_handle(struct d1_store *s, uint64_t raw)
{
	return d1_admission_of(s, raw);
}

d1_txn_id d1_fixture_txn_handle(struct d1_store *s, uint64_t raw)
{
	return d1_txn_of(s, raw);
}

d1_version_id d1_fixture_version_handle(struct d1_store *s, uint64_t raw)
{
	return d1_version_of(s, raw);
}

d1_custody_id d1_fixture_custody_handle(struct d1_store *s, uint64_t raw)
{
	return d1_custody_of(s, raw);
}

d1_admission_id d1_fixture_admit_full(struct d1_store *s,
				      const struct d1_objkey *object,
				      const struct d1_fixture_authority *auth)
{
	struct d1_control_request request;
	struct d1_control_result result;
	struct d1_admission *a;
	d1_admission_id id;

	memset(&request, 0, sizeof(request));
	request.kind = D1_CTL_ADMIT;
	request.object = *object;
	request.auth = *auth;

	pthread_mutex_lock(&s->lock);
	if (!d1_store_serving(s)) {
		pthread_mutex_unlock(&s->lock);
		return d1_admission_of(s, 0);
	}
	id = d1_admit_locked(s, object, auth);
	memset(&result, 0, sizeof(result));
	result.id = id.raw;
	result.status = d1_admission_live(id) ? D1_OK : D1_NOSPC;
	if (d1_admission_live(id) &&
	    !d1_journal_fixture(s, &request, &result)) {
		/* An event that is not durable did not happen. */
		a = d1_admission_find(s, id);
		if (a)
			a->used = false;
		s->next_admission--;
		id = d1_admission_of(s, 0);
	}
	pthread_mutex_unlock(&s->lock);
	return id;
}

/*
 * The common case: one issuer, and a principal derived from the writer,
 * so two handles for one writer share a principal and two for different
 * writers do not.  Tests that need a specific binding use the full form.
 */
d1_admission_id d1_fixture_admit(struct d1_store *s,
				 const struct d1_objkey *object,
				 uint32_t writer, uint32_t rights)
{
	struct d1_fixture_authority auth;
	unsigned int i;

	memset(&auth, 0, sizeof(auth));
	for (i = 0; i < D1_UUID_BYTES; i++) {
		auth.issuer.bytes[i] = (uint8_t)(0xf0u + i);
		auth.principal.bytes[i] = (uint8_t)(writer + i);
	}
	auth.writer = writer;
	auth.rights = rights;
	return d1_fixture_admit_full(s, object, &auth);
}

void d1_fixture_revoke(struct d1_store *s, d1_admission_id admission)
{
	struct d1_control_request request;
	struct d1_control_result result;
	struct d1_admission *a;
	bool before;

	memset(&request, 0, sizeof(request));
	request.kind = D1_CTL_REVOKE;
	request.admission = admission;

	pthread_mutex_lock(&s->lock);
	if (!d1_store_serving(s)) {
		pthread_mutex_unlock(&s->lock);
		return;
	}
	/*
	 * A handle of another store is not this store's to refuse in
	 * writing: the CONTROL record would carry the number and not the
	 * issuer, and replay would adopt the number, find the row and
	 * revoke it.  See d1_envelope_owned.
	 */
	if (!d1_owns_admission(s, admission)) {
		pthread_mutex_unlock(&s->lock);
		return;
	}
	a = d1_admission_find(s, admission);
	before = a ? a->revoked : false;
	memset(&result, 0, sizeof(result));
	result.status = d1_revoke_locked(s, admission) ? D1_OK : D1_STALE_AUTH;
	result.id = admission.raw;
	if (a)
		request.object = a->object;
	if (!d1_journal_fixture(s, &request, &result) && a)
		a->revoked = before;
	pthread_mutex_unlock(&s->lock);
}

void d1_fixture_expire(struct d1_store *s, d1_admission_id admission)
{
	struct d1_control_request request;
	struct d1_control_result result;
	struct d1_admission *a;
	bool before;

	memset(&request, 0, sizeof(request));
	request.kind = D1_CTL_EXPIRE;
	request.admission = admission;

	pthread_mutex_lock(&s->lock);
	if (!d1_store_serving(s)) {
		pthread_mutex_unlock(&s->lock);
		return;
	}
	/* The same, before anything is written; see d1_fixture_revoke. */
	if (!d1_owns_admission(s, admission)) {
		pthread_mutex_unlock(&s->lock);
		return;
	}
	a = d1_admission_find(s, admission);
	before = a ? a->expired : false;
	memset(&result, 0, sizeof(result));
	result.status = d1_expire_locked(s, admission) ? D1_OK : D1_STALE_AUTH;
	result.id = admission.raw;
	if (a)
		request.object = a->object;
	if (!d1_journal_fixture(s, &request, &result) && a)
		a->expired = before;
	pthread_mutex_unlock(&s->lock);
}

static void
d1_certificate_locked(struct d1_store *s, bool present,
		      const uint8_t certificate[D1_CERTIFICATE_BYTES])
{
	s->certificate_present = present;
	if (present)
		memcpy(s->certificate, certificate, D1_CERTIFICATE_BYTES);
	else
		memset(s->certificate, 0, D1_CERTIFICATE_BYTES);
}

void d1_fixture_certificate(struct d1_store *s,
			    const uint8_t certificate[D1_CERTIFICATE_BYTES])
{
	struct d1_control_request request;
	struct d1_control_result result;
	uint8_t was[D1_CERTIFICATE_BYTES];
	bool was_present;

	memset(&request, 0, sizeof(request));
	request.kind = D1_CTL_CERTIFICATE;
	request.certificate_present = certificate != NULL;
	if (certificate)
		memcpy(request.certificate, certificate, D1_CERTIFICATE_BYTES);

	pthread_mutex_lock(&s->lock);
	if (!d1_store_serving(s) || s->replaying) {
		pthread_mutex_unlock(&s->lock);
		return;
	}
	was_present = s->certificate_present;
	memcpy(was, s->certificate, sizeof(was));
	d1_certificate_locked(s, request.certificate_present,
			      request.certificate);
	memset(&result, 0, sizeof(result));
	result.status = D1_OK;
	/*
	 * Journalled like every other piece of fixture authority, and for
	 * the same reason: clear_error's answer depends on it, so a
	 * recorded refusal that turned on which certificate had been
	 * issued must be an answer a rebuild can reach.  An event that
	 * cannot be made durable puts the store back.
	 */
	if (!d1_journal_fixture(s, &request, &result))
		d1_certificate_locked(s, was_present, was);
	pthread_mutex_unlock(&s->lock);
}

d1_repair_id d1_fixture_repair_handle(struct d1_store *s, uint64_t raw)
{
	return d1_repair_of(s, raw);
}

d1_postcond_id d1_fixture_postcond_handle(struct d1_store *s, uint64_t raw)
{
	return d1_postcond_of(s, raw);
}

d1_episode_id d1_fixture_episode_handle(struct d1_store *s, uint64_t raw)
{
	return d1_episode_of(s, raw);
}

bool d1_fixture_postcond(struct d1_store *s, d1_postcond_id id, uint64_t *index,
			 d1_version_id *successor, bool *consumed)
{
	const struct d1_postcond *p;
	bool found = false;

	pthread_mutex_lock(&s->lock);
	p = d1_postcond_find(s, id);
	if (p) {
		*index = p->index;
		*successor = d1_version_of(s, p->successor);
		*consumed = p->consumed;
		found = true;
	}
	pthread_mutex_unlock(&s->lock);
	return found;
}

bool d1_fixture_repair_member(struct d1_store *s, d1_repair_id cohort,
			      uint32_t index, d1_txn_id *txn,
			      d1_version_id *version)
{
	const struct d1_repair *row;
	bool found = false;

	pthread_mutex_lock(&s->lock);
	row = d1_repair_find(s, cohort);
	if (row && index < row->count) {
		*txn = d1_txn_of(s, row->member[index].txn);
		*version =
			row->member[index].staged ?
				d1_version_of(s, row->member[index].version) :
				d1_version_none();
		found = true;
	}
	pthread_mutex_unlock(&s->lock);
	return found;
}

d1_custody_id d1_fixture_custody(struct d1_store *s, d1_version_id version)
{
	struct d1_control_request request;
	struct d1_control_result result;
	struct d1_custody *c;
	d1_custody_id id;
	uint32_t i;

	memset(&request, 0, sizeof(request));
	request.kind = D1_CTL_CUSTODY;
	request.version = version;

	pthread_mutex_lock(&s->lock);
	if (!d1_store_serving(s)) {
		pthread_mutex_unlock(&s->lock);
		return d1_custody_of(s, 0);
	}
	id = d1_custody_locked(s, version);
	memset(&result, 0, sizeof(result));
	result.id = id.raw;
	result.status = d1_custody_live(id) ? D1_OK : D1_NOSPC;
	if (d1_custody_live(id) && !d1_journal_fixture(s, &request, &result)) {
		for (i = 0; i < D1_MAX_CUSTODY; i++) {
			c = &s->custody[i];
			if (c->used && c->id == id.raw)
				c->used = false;
		}
		s->next_custody--;
		id = d1_custody_of(s, 0);
	}
	pthread_mutex_unlock(&s->lock);
	return id;
}

bool d1_fixture_release_predecessor(struct d1_store *s, d1_version_id version)
{
	struct d1_control_request request;
	struct d1_control_result result;
	struct d1_version *v;
	bool ok;

	memset(&request, 0, sizeof(request));
	request.kind = D1_CTL_RELEASE;
	request.version = version;

	pthread_mutex_lock(&s->lock);
	if (!d1_store_serving(s)) {
		pthread_mutex_unlock(&s->lock);
		return false;
	}
	/* The same, before anything is written; see d1_fixture_revoke. */
	if (!d1_owns_version(s, version)) {
		pthread_mutex_unlock(&s->lock);
		return false;
	}
	ok = d1_release_locked(s, version);
	memset(&result, 0, sizeof(result));
	result.id = version.raw;
	result.status = ok ? D1_OK : D1_INVALID;
	if (!d1_journal_fixture(s, &request, &result) && ok) {
		v = d1_version_find(s, version);
		if (v)
			v->released = false;
		ok = false;
	}
	pthread_mutex_unlock(&s->lock);
	return ok;
}

/*
 * The journal, and rebuilding from it.
 *
 * Recovery is deterministic re-execution of the reducer with logging
 * disabled: begin empty, validate the frame, decode the inputs, apply
 * the same table-driven operation including the fixture authority
 * state, and compare the complete computed result and digest against
 * the logged ones.  Any disagreement fails closed.
 *
 * That is why the fixture's own control events are journalled.  Which
 * handle is live, which custody binds which version and which
 * predecessor has been released all change what the reducer answers, so
 * a replay that invented them would be re-executing a different
 * history.  Fault arms, read pins and physical reclamation are not
 * journalled, because they change no reducer outcome.
 */

/* previous incarnation, new incarnation, verifier. */
#define D1_START_PAYLOAD_BYTES (8u + 8u + D1_VERIFIER_BYTES)

static uint32_t d1_start_append(struct d1_store *s)
{
	uint8_t payload[D1_START_PAYLOAD_BYTES];
	uint8_t verifier[D1_VERIFIER_BYTES];
	struct d1_cursor cur;

	/*
	 * A START header carries its NEW incarnation; every later record
	 * matches the latest START.  The first one chains from zero.
	 */
	d1_journal_set_incarnation(&s->journal, s->incarnation);
	d1_verifier_of(s->incarnation, verifier);
	d1_enc_init(&cur, payload, sizeof(payload));
	d1_enc_u64(&cur, s->incarnation - 1u);
	d1_enc_u64(&cur, s->incarnation);
	d1_enc_raw(&cur, verifier, sizeof(verifier));
	if (cur.bad ||
	    !d1_journal_append(&s->journal, D1_REC_START, payload,
			       (uint32_t)cur.len) ||
	    !d1_journal_flush(&s->journal))
		return D1_IO;
	return D1_OK;
}

uint32_t d1_store_journal_enable(struct d1_store *s)
{
	uint32_t status = D1_OK;
	uint32_t arm_append;
	bool arm_flush;

	pthread_mutex_lock(&s->lock);
	/*
	 * A log describes everything the store did.  Enabling it on a
	 * store that has already done unlogged work would produce a log
	 * that cannot rebuild its own store, so the baseline has to be the
	 * one the first START describes.
	 */
	if (s->journaling || s->poisoned || s->closed ||
	    !d1_store_pristine(s)) {
		status = D1_INVALID;
		goto out;
	}
	/*
	 * A fresh journal is a new buffer, not a new run.  The fault the
	 * caller armed describes this process, and on a store that has
	 * never logged anything the next append and the next flush are
	 * the START's own.  Initialization zeroes the whole structure, so
	 * the arms used to be erased between being set and being reached,
	 * which quietly made the one pre-frontier failure this model has
	 * -- a START that never becomes durable -- impossible to ask for.
	 */
	arm_append = s->journal.fail_append_in;
	arm_flush = s->journal.fail_next_flush;
	if (!d1_journal_init(&s->journal, &s->uuid)) {
		status = D1_NOSPC;
		goto out;
	}
	s->journal.fail_append_in = arm_append;
	s->journal.fail_next_flush = arm_flush;
	status = d1_start_append(s);
	if (status != D1_OK) {
		d1_journal_fini(&s->journal);
		goto out;
	}
	s->journaling = true;
out:
	pthread_mutex_unlock(&s->lock);
	return status;
}

static uint32_t d1_replay_start(struct d1_store *s, const uint8_t *payload,
				uint32_t len, uint64_t header_incarnation,
				unsigned int ordinal)
{
	struct d1_cursor cur;
	uint8_t want[D1_VERIFIER_BYTES];
	uint8_t verifier[D1_VERIFIER_BYTES];
	uint64_t previous, fresh;

	d1_dec_init(&cur, payload, len);
	if (!d1_dec_u64(&cur, &previous) || !d1_dec_u64(&cur, &fresh) ||
	    !d1_dec_raw(&cur, verifier, sizeof(verifier)) ||
	    !d1_dec_finished(&cur))
		return D1_INVALID;
	/* Incarnations chain: each from the one before it. */
	if (fresh != previous + 1u || fresh != header_incarnation)
		return D1_INVALID;
	/*
	 * The first START opens the store's first incarnation; every later
	 * one supersedes the incarnation the log has reached so far.
	 */
	if (previous != (ordinal == 1u ? 0u : s->incarnation))
		return D1_INVALID;
	d1_verifier_of(fresh, want);
	if (memcmp(want, verifier, sizeof(want)) != 0)
		return D1_INVALID;
	s->incarnation = fresh;
	/*
	 * The new START fences the old incarnation's mutation admissions:
	 * every mutation checks that its handle was issued in the current
	 * incarnation, so a handle from before the START is dead without
	 * anything having to walk the table.  Stable pending and finalized
	 * work stays retained, and needs an explicit recovery_admit under
	 * a current handle before it can be used again.
	 */
	return D1_OK;
}

/*
 * Bind a decoded record's handles to the store replaying it.
 *
 * A log carries values, not provenance: nothing that comes off the wire
 * names a store, and a lookup refuses a handle no store issued.
 * Recovery is deterministic re-execution by this store, so the handles
 * a record names are this store's, and these two functions are the only
 * place that says so.  They are not a general cast: every handle they
 * make is bound to the target and to nothing else, they are static to
 * this file, and no public entry point reaches them.
 */
static void d1_envelope_adopt(const struct d1_store *s, struct d1_envelope *env)
{
	uint32_t i;

	env->admission = d1_admission_of(s, env->admission.raw);
	switch (env->op) {
	case D1_OP_FINALIZE_BATCH:
	case D1_OP_COMMIT_BATCH:
		for (i = 0; i < env->body.lifecycle.count; i++) {
			struct d1_lifecycle_entry *e =
				&env->body.lifecycle.entries[i];

			e->txn = d1_txn_of(s, e->txn.raw);
			e->predecessor = d1_version_of(s, e->predecessor.raw);
		}
		break;
	case D1_OP_ROLLBACK_BATCH:
		for (i = 0; i < env->body.rollback.count; i++) {
			struct d1_rollback_entry *e =
				&env->body.rollback.entries[i];

			e->txn = d1_txn_of(s, e->txn.raw);
			e->visible = d1_version_of(s, e->visible.raw);
			e->predecessor = d1_version_of(s, e->predecessor.raw);
			e->custody = d1_custody_of(s, e->custody.raw);
		}
		break;
	case D1_OP_RECOVERY_ADMIT:
	case D1_OP_LEASE_REAP:
		for (i = 0; i < env->body.control.count; i++)
			env->body.control.txns[i] =
				d1_txn_of(s, env->body.control.txns[i].raw);
		env->body.control.old_admission =
			d1_admission_of(s, env->body.control.old_admission.raw);
		env->body.control.new_admission =
			d1_admission_of(s, env->body.control.new_admission.raw);
		break;
	case D1_OP_MARK_ERROR:
	case D1_OP_BEGIN_REPAIR:
	case D1_OP_PREPARE_REPAIR:
	case D1_OP_FINALIZE_REPAIR:
	case D1_OP_COMMIT_REPAIR:
	case D1_OP_ABORT_REPAIR:
	case D1_OP_CLEAR_ERROR:
	case D1_OP_UNLOCK:
		for (i = 0; i < env->body.repair.count; i++) {
			struct d1_repair_entry *e =
				&env->body.repair.entries[i];

			e->custody = d1_custody_of(s, e->custody.raw);
			e->postcond = d1_postcond_of(s, e->postcond.raw);
			e->successor = d1_version_of(s, e->successor.raw);
			e->predecessor = d1_version_of(s, e->predecessor.raw);
		}
		env->body.repair.cohort =
			d1_repair_of(s, env->body.repair.cohort.raw);
		env->body.repair.episode =
			d1_episode_of(s, env->body.repair.episode.raw);
		break;
	default:
		/* A write batch carries an owner, which no store issues. */
		break;
	}
}

static void d1_control_adopt(const struct d1_store *s,
			     struct d1_control_request *r)
{
	r->admission = d1_admission_of(s, r->admission.raw);
	r->version = d1_version_of(s, r->version.raw);
}

/*
 * Whether the receipt this record claims is there.
 *
 * Replay asks it twice, once on each side of the reducer: a receipt
 * that was absent before and present after is the one this reduction
 * created.  The lookup is by object, key and ordinal, so the receipt it
 * finds is this record's by construction, and the digest in it is the
 * digest the reducer was handed.
 */
static bool d1_record_receipt(struct d1_store *s, const struct d1_envelope *env,
			      uint32_t ordinal)
{
	return d1_receipt_find(s, &env->object.export_uuid, &env->key,
			       ordinal) != NULL;
}

static uint32_t d1_replay_entry(struct d1_store *s, const uint8_t *body,
				uint32_t len)
{
	struct d1_envelope env;
	struct d1_complete_result logged, computed;
	struct d1_cursor cur;
	const uint8_t *env_bytes, *result_bytes;
	uint8_t digest[D1_DIGEST_BYTES];
	uint8_t logged_digest[D1_DIGEST_BYTES];
	uint32_t env_len, result_len, ordinal;

	d1_dec_init(&cur, body, len);
	if (!d1_dec_bytes_ref(&cur, &env_bytes, &env_len, D1_ENVELOPE_MAX) ||
	    !d1_dec_u32(&cur, &ordinal) ||
	    !d1_dec_raw(&cur, logged_digest, sizeof(logged_digest)) ||
	    !d1_dec_bytes_ref(&cur, &result_bytes, &result_len, 4096u) ||
	    !d1_dec_finished(&cur))
		return D1_INVALID;
	if (!d1_envelope_decode(env_bytes, env_len, &env))
		return D1_INVALID;
	d1_envelope_adopt(s, &env);
	if (!d1_complete_result_decode(result_bytes, result_len, &logged))
		return D1_INVALID;
	/*
	 * The record category has to agree with what it carries.  An ENTRY
	 * is one member of an ordinary batch, so a control operation or a
	 * repair in one is a record the live encoder cannot emit -- both
	 * answer once for a whole call and are appended as CONTROL -- and
	 * an ordinal past the body's own count names a member that does
	 * not exist.
	 *
	 * The repair half is asked here rather than left to the reducer
	 * because the repair and lifecycle bodies share a union: an ENTRY
	 * carrying a repair had its lifecycle arm read out of the
	 * repair's bytes, was reduced, and took a receipt at ordinal zero
	 * that the live store never took.
	 */
	if (d1_op_rights(env.op) == D1_RIGHT_CONTROL || d1_op_is_repair(env.op))
		return D1_INVALID;
	if (ordinal >= d1_envelope_member_count(&env))
		return D1_INVALID;

	/* The store computes the digest; it never trusts a logged one. */
	if (!d1_envelope_digest(&env, s->scratch, s->scratch_cap, digest))
		return D1_INVALID;
	if (memcmp(digest, logged_digest, sizeof(digest)) != 0)
		return D1_INVALID;

	/*
	 * The receipts a key holds are the dense prefix of its members
	 * (see d1_key_recorded), so a record that repeats an ordinal or
	 * skips one is a record the live writer could not have emitted.
	 * Refusing it here, before the reducer runs, is what stops a
	 * spliced log from reconstructing a state no caller ever claimed.
	 * A member of the same key carrying a different Envelope digest is
	 * refused inside the reducer, which replay shares.
	 */
	if (ordinal != d1_key_recorded(s, &env.object.export_uuid, &env.key))
		return D1_INVALID;

	/*
	 * A durable record is a claim that an event happened, and the
	 * event an ENTRY claims is a receipt.  Equal results do not say
	 * that: a member whose caller binding fails, a member whose key
	 * already names a different Envelope, an exact repeat answered
	 * from the receipt it already has and a member that could not
	 * reserve one all return exactly the result they returned live,
	 * and the live writer appends none of them.  Accepting one would
	 * carry the frontier over an event that never happened.  So the
	 * record is accepted only if reducing it created the receipt the
	 * record names, which is a question asked on both sides of the
	 * reducer rather than derived from the status it returned.
	 */
	if (d1_record_receipt(s, &env, ordinal))
		return D1_INVALID;
	d1_apply_one(s, &env, env_bytes, env_len, ordinal, digest,
		     d1_op_rights(env.op), env.op == D1_OP_COMMIT_BATCH,
		     &computed);
	if (!d1_record_receipt(s, &env, ordinal))
		return D1_INVALID;
	/* Re-execution that disagrees with the log is not this history. */
	if (!d1_complete_result_equal(&computed, &logged))
		return D1_INVALID;
	return D1_OK;
}

/*
 * Whether a fixture control record is one a live writer could have
 * built.
 *
 * The request codec carries only the fields its kind uses -- the
 * authority for ADMIT, the handle for REVOKE and EXPIRE, the version
 * for CUSTODY and RELEASE -- so a decoded request has the canonical
 * zero in every field its kind does not carry, whatever the bytes say.
 * One field is on the wire for every kind: the object.  And for three
 * of the five kinds it is not the request at all, it is derived, so a
 * record can carry one thing and mean another.
 *
 * Replay dispatched on the handle alone and never looked, so a record
 * whose object disagreed with its handle still revoked that handle:
 * CRC-valid, result-equal, and a transition against an object the
 * request does not name.  So the derived half is checked, before
 * anything moves:
 *
 *   ADMIT    the object is the request.  Nothing to derive.
 *   REVOKE,  the object is the one the located admission holds -- or
 *   EXPIRE   the canonical zero, when there was no admission to take
 *            it from, which is what the writer leaves in that case.
 *   CUSTODY, the version is the request and the object is untouched,
 *   RELEASE  so the canonical zero is the only object they carry.
 */
static bool d1_control_canonical(struct d1_store *s,
				 const struct d1_control_request *r)
{
	const struct d1_admission *a;
	struct d1_objkey no_object;

	memset(&no_object, 0, sizeof(no_object));
	switch (r->kind) {
	case D1_CTL_ADMIT:
		return true;
	case D1_CTL_REVOKE:
	case D1_CTL_EXPIRE:
		a = d1_admission_find(s, r->admission);
		return memcmp(&r->object, a ? &a->object : &no_object,
			      sizeof(r->object)) == 0;
	case D1_CTL_CERTIFICATE:
		/* It names no object and no handle; only its own bytes. */
		return memcmp(&r->object, &no_object, sizeof(no_object)) == 0;
	default:
		return memcmp(&r->object, &no_object, sizeof(no_object)) == 0;
	}
}

static uint32_t d1_replay_control(struct d1_store *s, const uint8_t *body,
				  uint32_t len)
{
	struct d1_control_request request;
	struct d1_control_result logged_ctl, computed_ctl;
	struct d1_complete_result logged, computed;
	struct d1_envelope env;
	struct d1_cursor cur;
	const uint8_t *request_bytes, *result_bytes;
	uint8_t digest[D1_DIGEST_BYTES];
	uint8_t logged_digest[D1_DIGEST_BYTES];
	uint32_t kind, request_len, result_len;

	d1_dec_init(&cur, body, len);
	if (!d1_dec_u32(&cur, &kind) ||
	    !d1_dec_bytes_ref(&cur, &request_bytes, &request_len,
			      D1_ENVELOPE_MAX))
		return D1_INVALID;
	/*
	 * The outer tag is checked before anything is dispatched on it.
	 * It used to be compared against D1_CTL_ENVELOPE and otherwise
	 * ignored, so a record carrying any other value -- including one
	 * no encoder can produce -- was reduced on the strength of its
	 * inner tag alone.  It is read first here for a second reason
	 * too: the digest an Envelope control carries is part of the
	 * record's shape, so what to read next depends on it.
	 */
	if (kind < D1_CTL_ENVELOPE || kind > D1_CTL_CERTIFICATE)
		return D1_INVALID;
	if (kind == D1_CTL_ENVELOPE &&
	    !d1_dec_raw(&cur, logged_digest, sizeof(logged_digest)))
		return D1_INVALID;
	if (!d1_dec_bytes_ref(&cur, &result_bytes, &result_len, 4096u) ||
	    !d1_dec_finished(&cur))
		return D1_INVALID;

	if (kind == D1_CTL_ENVELOPE) {
		if (!d1_envelope_decode(request_bytes, request_len, &env))
			return D1_INVALID;
		d1_envelope_adopt(s, &env);
		/*
		 * A CONTROL record carries a control operation or a repair.
		 * Both answer once for the whole call, which is why they
		 * share a record kind and a receipt at ordinal zero.
		 */
		if (d1_op_rights(env.op) != D1_RIGHT_CONTROL &&
		    !d1_op_is_repair(env.op))
			return D1_INVALID;
		if (!d1_complete_result_decode(result_bytes, result_len,
					       &logged))
			return D1_INVALID;
		if (!d1_envelope_digest(&env, s->scratch, s->scratch_cap,
					digest))
			return D1_INVALID;
		/* The store computes it; the record only claims it. */
		if (memcmp(digest, logged_digest, sizeof(digest)) != 0)
			return D1_INVALID;
		/*
		 * The same emission invariant, and the only one a control
		 * record has: a control operation answers once, so its
		 * receipt is member zero, and a duplicate of a record the
		 * writer appended once is answered from that receipt with
		 * the identical result.  See d1_record_receipt.
		 */
		if (d1_record_receipt(s, &env, 0))
			return D1_INVALID;
		if (d1_op_is_repair(env.op))
			d1_apply_repair(s, &env, request_bytes, request_len,
					digest, &computed);
		else
			d1_apply_control(s, &env, request_bytes, request_len,
					 digest, &computed);
		if (!d1_record_receipt(s, &env, 0))
			return D1_INVALID;
		if (!d1_complete_result_equal(&computed, &logged))
			return D1_INVALID;
		return D1_OK;
	}

	if (!d1_control_request_decode(request_bytes, request_len, &request))
		return D1_INVALID;
	d1_control_adopt(s, &request);
	/* And the outer tag agrees with the schema it actually carries. */
	if (request.kind != kind)
		return D1_INVALID;
	/* And every field of it is the one the live writer would have set. */
	if (!d1_control_canonical(s, &request))
		return D1_INVALID;
	if (!d1_control_result_decode(result_bytes, result_len, &logged_ctl))
		return D1_INVALID;
	/*
	 * And its result is an outcome this kind's writer appends.  A
	 * fixture control has no receipt, so there is no before-and-after
	 * question to ask about it; what stands in for one is the rule
	 * each live writer already follows.  ADMIT and CUSTODY journal
	 * only an allocation: d1_fixture_admit_full and d1_fixture_custody
	 * append nothing when the ID came back zero, so a record carrying
	 * their zero ID is a record no writer emits -- and result equality
	 * cannot notice, because the reducer refuses the same request the
	 * same way and computes exactly the pair that was logged.
	 *
	 * The other three journal every outcome they reach, including a
	 * refusal and a repeat, so their unsuccessful records are real
	 * history and are not held to this.
	 */
	if ((request.kind == D1_CTL_ADMIT || request.kind == D1_CTL_CUSTODY) &&
	    logged_ctl.id == 0)
		return D1_INVALID;
	memset(&computed_ctl, 0, sizeof(computed_ctl));
	switch (request.kind) {
	case D1_CTL_ADMIT:
		computed_ctl.id =
			d1_admit_locked(s, &request.object, &request.auth).raw;
		computed_ctl.status = computed_ctl.id ? D1_OK : D1_NOSPC;
		break;
	case D1_CTL_REVOKE:
		computed_ctl.id = request.admission.raw;
		computed_ctl.status = d1_revoke_locked(s, request.admission) ?
					      D1_OK :
					      D1_STALE_AUTH;
		break;
	case D1_CTL_EXPIRE:
		computed_ctl.id = request.admission.raw;
		computed_ctl.status = d1_expire_locked(s, request.admission) ?
					      D1_OK :
					      D1_STALE_AUTH;
		break;
	case D1_CTL_CUSTODY:
		computed_ctl.id = d1_custody_locked(s, request.version).raw;
		computed_ctl.status = computed_ctl.id ? D1_OK : D1_NOSPC;
		break;
	case D1_CTL_CERTIFICATE:
		d1_certificate_locked(s, request.certificate_present,
				      request.certificate);
		computed_ctl.id = 0;
		computed_ctl.status = D1_OK;
		break;
	default:
		computed_ctl.id = request.version.raw;
		computed_ctl.status = d1_release_locked(s, request.version) ?
					      D1_OK :
					      D1_INVALID;
		break;
	}
	if (computed_ctl.status != logged_ctl.status ||
	    computed_ctl.id != logged_ctl.id)
		return D1_INVALID;
	return D1_OK;
}

/*
 * Rebuild @s from the durable prefix of @log.
 *
 * @durable is the length the writer claimed, not the length of the
 * buffer.  Bytes past it were never claimed and are not read.  Anything
 * malformed inside it is corruption of data the writer did claim, and
 * fails closed -- a completed record never quietly disappears, and a
 * corrupt prefix is never reported as a successful replay.
 *
 * This is read-only reconstruction.  It does not open a new incarnation;
 * d1_store_reopen does that, on top of this.
 */
/*
 * Reduction, with the store's lock already held.
 *
 * A reopen is one transition, not two: it rebuilds, adopts the durable
 * prefix and makes a new START durable, and a caller that entered
 * between any two of those would be mutating a store that is live and
 * journalling nothing.  Its own journal could then not rebuild it, for
 * the rest of its life.  So the fence is the caller's lock interval and
 * the body of the reduction lives here, where either entry point can
 * hold it for as long as it needs.
 */
static uint32_t d1_replay_locked(struct d1_store *s, const uint8_t *log,
				 size_t durable)
{
	struct d1_journal_cursor c;
	const uint8_t *body;
	enum d1_journal_read got;
	uint32_t type, blen, status = D1_OK;
	uint64_t lsn, incarnation;
	unsigned int starts = 0;

	if (s->replaying || s->poisoned || s->closed)
		return D1_INVALID;
	/*
	 * The target must be exactly as it was opened.  Refusing here
	 * changes nothing, which is the point: a store that was rejected
	 * is still the store it was, and is not poisoned.
	 */
	if (!d1_store_pristine(s) || s->active_calls)
		return D1_INVALID;
	/*
	 * Section 9: clear every fault arm before replay, and admit no new
	 * one until it ends.  Suppressing a fault during reduction is not
	 * the same as resetting the harness world -- an arm set before
	 * reconstruction used to survive it and fire on the first live
	 * operation afterwards, which is an unjournalled control changing
	 * post-recovery behaviour.
	 */
	s->fail_next_index = false;
	s->fail_next_snapshot = false;
	s->exhaust_ids = false;
	s->exhaust_epoch = false;
	s->overlay_active = false;
	s->journal.fail_append_in = 0;
	s->journal.fail_next_flush = false;
	s->before_member = NULL;
	s->before_member_arg = NULL;
	s->before_member_ordinal = 0;
	d1_admit_hook_forget(s);
	s->replaying = true;

	d1_journal_cursor_init(&c, log, durable, &s->uuid);
	for (;;) {
		got = d1_journal_next(&c, &type, &lsn, &incarnation, &body,
				      &blen);
		if (got == D1_JOURNAL_CLEAN_END)
			break;
		if (got == D1_JOURNAL_CORRUPT) {
			status = D1_IO;
			break;
		}
		if (!starts && type != D1_REC_START) {
			status = D1_INVALID;
			break;
		}
		/*
		 * Every record after a START carries that START's
		 * incarnation; one that does not belongs to a history this
		 * log is not.
		 */
		if (starts && type != D1_REC_START &&
		    incarnation != s->incarnation) {
			status = D1_INVALID;
			break;
		}
		switch (type) {
		case D1_REC_START:
			starts++;
			status = d1_replay_start(s, body, blen, incarnation,
						 starts);
			break;
		case D1_REC_ENTRY:
			status = d1_replay_entry(s, body, blen);
			break;
		case D1_REC_CONTROL:
			status = d1_replay_control(s, body, blen);
			break;
		default:
			status = D1_INVALID;
			break;
		}
		if (status != D1_OK)
			break;
	}
	if (status == D1_OK && !starts)
		status = D1_INVALID;
	/* The log's own LSNs continue if this store goes on to write. */
	s->replayed_lsn = c.last_lsn;
	s->replaying = false;
	/*
	 * A rebuild that stopped part way leaves state that is neither the
	 * logged history nor an empty store.  The handle is finished: only
	 * teardown may touch it from here.
	 */
	if (status != D1_OK)
		s->poisoned = true;
	return status;
}

uint32_t d1_store_replay(struct d1_store *s, const uint8_t *log, size_t durable)
{
	uint32_t status;

	pthread_mutex_lock(&s->lock);
	/*
	 * A read-only rebuild reaches no START of its own, so an arm aimed
	 * at one would sit on the store waiting for a reopen that this
	 * handle can no longer have.  It goes here, with every other arm.
	 */
	s->fail_reopen_start = D1_REOPEN_START_OK;
	status = d1_replay_locked(s, log, durable);
	pthread_mutex_unlock(&s->lock);
	return status;
}

/*
 * Actual reopen: rebuild, then open a new incarnation.
 *
 * Read-only reconstruction is not a reboot.  A reopen appends and
 * flushes a new START, which fences the old incarnation's mutation
 * admissions and publishes a new verifier, and it continues the log's
 * LSNs rather than starting them again.  Doing it twice is ordinary.
 */
uint32_t d1_store_reopen(struct d1_store *s, const uint8_t *log, size_t durable)
{
	uint32_t status;
	uint32_t arm;

	/*
	 * One lock interval covers the whole transition: the reduction,
	 * the adoption of the durable prefix and the new START becoming
	 * durable.  It used to cover the reduction and then be taken again
	 * for the rest, and a call admitted in that gap found a store that
	 * was rebuilt, live and journalling nothing -- so its mutation was
	 * real and unrecorded, and the log could no longer rebuild the
	 * store it belonged to.  Nothing in the gap was wrong on its own;
	 * the gap was.
	 */
	pthread_mutex_lock(&s->lock);
	/*
	 * The arm for this transition's own START.  It is deliberately not
	 * one of the arms reduction clears: those describe operations, and
	 * an arm that survived reconstruction would be an unjournalled
	 * control changing what the store did afterwards.  This one names
	 * the transition itself, which is the one pre-frontier failure a
	 * reopen has, and it cannot outlive it -- the transition consumes
	 * it, and a read-only rebuild drops it.
	 */
	arm = s->fail_reopen_start;
	s->fail_reopen_start = D1_REOPEN_START_OK;
	status = d1_replay_locked(s, log, durable);
	if (status != D1_OK) {
		pthread_mutex_unlock(&s->lock);
		return status;
	}

	/*
	 * Everything from here runs on a store that has already been
	 * rebuilt, so a failure cannot be answered by leaving the handle
	 * alone: the reduction happened.  What it would leave is a
	 * populated, mutable store with no journal recording it and no way
	 * to reconstruct into it again, which is the same thing a partial
	 * rebuild leaves and is poisoned for the same reason.
	 */
	if (!d1_journal_init(&s->journal, &s->uuid)) {
		s->poisoned = true;
		pthread_mutex_unlock(&s->lock);
		return D1_NOSPC;
	}
	/* Copy the durable prefix forward; the new START follows it. */
	if (durable && !d1_journal_adopt(&s->journal, log, durable,
					 s->replayed_lsn + 1u)) {
		d1_journal_fini(&s->journal);
		s->poisoned = true;
		pthread_mutex_unlock(&s->lock);
		return D1_NOSPC;
	}
	if (s->incarnation == UINT64_MAX) {
		d1_journal_fini(&s->journal);
		s->poisoned = true;
		pthread_mutex_unlock(&s->lock);
		return D1_NOSPC;
	}
	s->incarnation++;
	if (arm == D1_REOPEN_START_APPEND)
		s->journal.fail_append_in = 1u;
	else if (arm == D1_REOPEN_START_FLUSH)
		s->journal.fail_next_flush = true;
	status = d1_start_append(s);
	if (status != D1_OK) {
		s->incarnation--;
		d1_journal_fini(&s->journal);
		s->poisoned = true;
		pthread_mutex_unlock(&s->lock);
		return status;
	}
	s->journaling = true;
	pthread_mutex_unlock(&s->lock);
	return D1_OK;
}

/*
 * Public observers.  They take the model lock, so a caller reading
 * state while another thread mutates it sees one consistent answer
 * rather than a half-applied one.  The reducer uses the unlocked forms
 * above, because it already holds the lock.
 */
bool d1_store_visible(struct d1_store *s, const struct d1_objkey *object,
		      uint64_t index, d1_version_id *version)
{
	bool found;

	pthread_mutex_lock(&s->lock);
	found = d1_store_serving(s) &&
		d1_visible_locked(s, object, index, version);
	pthread_mutex_unlock(&s->lock);
	return found;
}

bool d1_store_guard(struct d1_store *s, const struct d1_objkey *object,
		    uint64_t index, struct d1_guard *guard)
{
	bool found;

	pthread_mutex_lock(&s->lock);
	found = d1_store_serving(s) && d1_guard_locked(s, object, index, guard);
	pthread_mutex_unlock(&s->lock);
	return found;
}

uint64_t d1_store_eof(struct d1_store *s, const struct d1_objkey *object)
{
	uint64_t eof;

	pthread_mutex_lock(&s->lock);
	eof = d1_store_serving(s) ? d1_eof_locked(s, object) : 0;
	pthread_mutex_unlock(&s->lock);
	return eof;
}

uint32_t d1_store_holes(struct d1_store *s, const struct d1_objkey *object,
			struct d1_interval *out, uint32_t max)
{
	uint32_t n;

	pthread_mutex_lock(&s->lock);
	n = d1_store_serving(s) ? d1_holes_locked(s, object, out, max) : 0;
	pthread_mutex_unlock(&s->lock);
	return n;
}
