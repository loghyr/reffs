/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

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
#include "d1_journal.h"
#include "d1_store.h"

struct d1_version {
	bool used;
	d1_id_t id;
	uint32_t object;
	uint64_t index;
	uint8_t bytes[D1_CHUNK_BYTES_MAX];
	uint32_t len;
	struct d1_checksum checksum;
	struct d1_owner owner;
	/* The predecessor this version displaced, if it is still retained. */
	bool predecessor_present;
	d1_id_t predecessor;
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
	d1_id_t id;
	uint32_t object;
	uint64_t index;
	struct d1_owner owner;
	d1_id_t admission;
	d1_id_t version;
	uint32_t phase;
	uint32_t mode;
	uint32_t stability;
	/*
	 * The read epoch an owner view must present to select this
	 * transaction's version.  Recovery re-admission grants a new one.
	 */
	uint64_t read_epoch;
	bool predecessor_present;
	d1_id_t predecessor;
	/* The guard this transaction installed when it was admitted. */
	struct d1_guard guard;
};

struct d1_chunk {
	struct d1_guard guard;
	bool pending_present;
	d1_id_t pending;
	bool visible_present;
	d1_id_t visible;
};

struct d1_object {
	bool used;
	struct d1_objkey key;
	struct d1_chunk chunks[D1_MAX_CHUNKS];
};

/*
 * One row of the fixture authority table.  Section 2 names the fields;
 * they are here because the bindings a call checks are exactly these,
 * and a handle that carried only a writer and some flags could not
 * answer "same principal, same issuer" when recovery re-admission asks.
 */
struct d1_admission {
	bool used;
	d1_id_t id;
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

/* (export UUID, cohort, writer, co_id) -> one object, chunk and version. */
struct d1_owner_assoc {
	bool used;
	struct d1_uuid export_uuid;
	struct d1_owner owner;
	uint32_t object;
	uint64_t index;
	d1_id_t version;
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
	uint64_t eof;
	bool present[D1_MAX_CHUNKS];
	d1_id_t version[D1_MAX_CHUNKS];
};

/* Repair custody over one exact version. */
struct d1_custody {
	bool used;
	d1_id_t id;
	d1_id_t version;
};

struct d1_receipt {
	bool used;
	struct d1_uuid export_uuid;
	struct d1_opkey key;
	uint32_t ordinal;
	uint8_t digest[D1_DIGEST_BYTES];
	struct d1_entry_result result;
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
	struct d1_uuid uuid;
	uint32_t chunk_bytes;
	uint64_t max_file_bytes;
	uint64_t incarnation;
	uint64_t index_epoch;

	/* Monotonic per-type counters; none is ever reused. */
	d1_id_t next_txn;
	d1_id_t next_version;
	d1_id_t next_admission;

	struct d1_object objects[D1_MAX_OBJECTS];
	struct d1_txn txns[D1_MAX_TXNS];
	struct d1_version versions[D1_MAX_VERSIONS];
	struct d1_admission admissions[D1_MAX_ADMISSIONS];
	struct d1_owner_assoc owners[D1_MAX_OWNERS];
	struct d1_receipt receipts[D1_MAX_RECEIPTS];
	struct d1_custody custody[D1_MAX_CUSTODY];
	struct d1_view views[D1_MAX_VIEWS];
	d1_id_t next_custody;

	/*
	 * Encoding scratch owned by this store and used only under its own
	 * lock.  Nothing is shared between stores, so two of them encoding
	 * at once cannot record each other's request identity.
	 */
	uint8_t *scratch;
	size_t scratch_cap;

	struct d1_journal journal;
	bool journaling;
	/*
	 * Set only while rebuilding from a log.  A replayed record was
	 * admitted when it was written, so it is not re-authorised; it is
	 * re-applied.  Nothing is appended and no fault can fire.
	 */
	bool replaying;
	struct d1_admission replay_admission;
};

static void d1_verifier_of(uint64_t incarnation, uint8_t out[D1_VERIFIER_BYTES])
{
	unsigned int i;

	for (i = 0; i < D1_VERIFIER_BYTES; i++)
		out[i] = (uint8_t)(incarnation >> (56 - 8 * i));
}

void d1_store_verifier(const struct d1_store *s, uint8_t out[D1_VERIFIER_BYTES])
{
	d1_verifier_of(s->incarnation, out);
}

uint64_t d1_store_incarnation(const struct d1_store *s)
{
	return s->incarnation;
}

struct d1_store *d1_store_open(const struct d1_uuid *store_uuid,
			       uint32_t chunk_bytes, uint64_t max_file_bytes)
{
	struct d1_store *s;

	if (chunk_bytes < D1_CHUNK_BYTES_MIN ||
	    chunk_bytes > D1_CHUNK_BYTES_MAX)
		return NULL;
	if (max_file_bytes == 0)
		return NULL;
	s = calloc(1, sizeof(*s));
	if (!s)
		return NULL;
	s->scratch_cap = D1_ENVELOPE_MAX;
	s->scratch = calloc(1, s->scratch_cap);
	if (!s->scratch) {
		free(s);
		return NULL;
	}
	if (pthread_mutex_init(&s->lock, NULL) != 0) {
		free(s->scratch);
		free(s);
		return NULL;
	}
	s->uuid = *store_uuid;
	s->chunk_bytes = chunk_bytes;
	s->max_file_bytes = max_file_bytes;
	/* The first incarnation is one; zero means no store has opened. */
	s->incarnation = 1;
	s->next_txn = 1;
	s->next_version = 1;
	s->next_admission = 1;
	s->next_custody = 1;
	return s;
}

void d1_store_free(struct d1_store *s)
{
	if (!s)
		return;
	d1_journal_fini(&s->journal);
	pthread_mutex_destroy(&s->lock);
	free(s->scratch);
	free(s);
}

void d1_fixture_fail_next_append(struct d1_store *s)
{
	pthread_mutex_lock(&s->lock);
	/* Faults are disabled throughout recovery, by construction. */
	if (!s->replaying)
		s->journal.fail_next = true;
	pthread_mutex_unlock(&s->lock);
}

const uint8_t *d1_store_journal(const struct d1_store *s, size_t *len)
{
	*len = s->journal.len;
	return s->journal.buf;
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

d1_id_t d1_fixture_admit_full(struct d1_store *s,
			      const struct d1_objkey *object,
			      const struct d1_fixture_authority *auth)
{
	d1_id_t id = 0;
	uint32_t i;

	/* Reserved writer IDs are never issued. */
	if (auth->writer == D1_WRITER_RESERVED_LOW ||
	    auth->writer == D1_WRITER_RESERVED_HIGH)
		return 0;
	pthread_mutex_lock(&s->lock);
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
		id = a->id;
		break;
	}
	pthread_mutex_unlock(&s->lock);
	return id;
}

/*
 * The common case: one issuer, and a principal derived from the writer
 * so that two handles for the same writer are the same principal and
 * two for different writers are not.  Tests that need a specific
 * binding use the full form.
 */
d1_id_t d1_fixture_admit(struct d1_store *s, const struct d1_objkey *object,
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

static struct d1_admission *d1_admission_find(struct d1_store *s, d1_id_t id)
{
	uint32_t i;

	if (!id)
		return NULL;
	for (i = 0; i < D1_MAX_ADMISSIONS; i++)
		if (s->admissions[i].used && s->admissions[i].id == id)
			return &s->admissions[i];
	return NULL;
}

void d1_fixture_revoke(struct d1_store *s, d1_id_t admission)
{
	struct d1_admission *a;

	pthread_mutex_lock(&s->lock);
	a = d1_admission_find(s, admission);
	if (a)
		a->revoked = true;
	pthread_mutex_unlock(&s->lock);
}

void d1_fixture_expire(struct d1_store *s, d1_id_t admission)
{
	struct d1_admission *a;

	pthread_mutex_lock(&s->lock);
	a = d1_admission_find(s, admission);
	if (a)
		a->expired = true;
	pthread_mutex_unlock(&s->lock);
}

d1_id_t d1_fixture_custody(struct d1_store *s, d1_id_t version)
{
	d1_id_t id = 0;
	uint32_t i;

	pthread_mutex_lock(&s->lock);
	for (i = 0; i < D1_MAX_CUSTODY; i++) {
		struct d1_custody *c = &s->custody[i];

		if (c->used)
			continue;
		c->used = true;
		c->id = s->next_custody++;
		c->version = version;
		id = c->id;
		break;
	}
	pthread_mutex_unlock(&s->lock);
	return id;
}

static struct d1_custody *d1_custody_find(struct d1_store *s, d1_id_t id)
{
	uint32_t i;

	if (!id)
		return NULL;
	for (i = 0; i < D1_MAX_CUSTODY; i++)
		if (s->custody[i].used && s->custody[i].id == id)
			return &s->custody[i];
	return NULL;
}

static struct d1_txn *d1_txn_find(struct d1_store *s, d1_id_t id)
{
	uint32_t i;

	if (!id)
		return NULL;
	for (i = 0; i < D1_MAX_TXNS; i++)
		if (s->txns[i].used && s->txns[i].id == id)
			return &s->txns[i];
	return NULL;
}

static struct d1_version *d1_version_find(struct d1_store *s, d1_id_t id)
{
	uint32_t i;

	if (!id)
		return NULL;
	for (i = 0; i < D1_MAX_VERSIONS; i++)
		if (s->versions[i].used && s->versions[i].id == id)
			return &s->versions[i];
	return NULL;
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
		if (a->owner.cohort == owner->cohort &&
		    a->owner.writer == owner->writer &&
		    a->owner.co_id == owner->co_id)
			return a;
	}
	return NULL;
}

static struct d1_owner_assoc *d1_owner_add(struct d1_store *s,
					   const struct d1_uuid *export_uuid,
					   const struct d1_owner *owner)
{
	uint32_t i;

	for (i = 0; i < D1_MAX_OWNERS; i++) {
		struct d1_owner_assoc *a = &s->owners[i];

		if (a->used)
			continue;
		memset(a, 0, sizeof(*a));
		a->used = true;
		a->export_uuid = *export_uuid;
		a->owner = *owner;
		return a;
	}
	return NULL;
}

/*
 * Receipt lookup.  An exact match returns the recorded result without
 * mutating anything, even across an incarnation change; the same key
 * with a different body is a conflict and never a new transition.
 */
static struct d1_receipt *d1_receipt_find(struct d1_store *s,
					  const struct d1_uuid *export_uuid,
					  const struct d1_opkey *key,
					  uint32_t ordinal)
{
	uint32_t i;

	for (i = 0; i < D1_MAX_RECEIPTS; i++) {
		struct d1_receipt *r = &s->receipts[i];

		if (!r->used || r->ordinal != ordinal)
			continue;
		if (memcmp(&r->export_uuid, export_uuid,
			   sizeof(*export_uuid)) != 0)
			continue;
		if (memcmp(&r->key.origin, &key->origin, sizeof(key->origin)) ==
			    0 &&
		    r->key.sequence == key->sequence &&
		    r->key.ordinal == key->ordinal)
			return r;
	}
	return NULL;
}

static struct d1_receipt *d1_receipt_add(struct d1_store *s,
					 const struct d1_uuid *export_uuid,
					 const struct d1_opkey *key,
					 uint32_t ordinal,
					 const uint8_t digest[D1_DIGEST_BYTES])
{
	uint32_t i;

	for (i = 0; i < D1_MAX_RECEIPTS; i++) {
		struct d1_receipt *r = &s->receipts[i];

		if (r->used)
			continue;
		memset(r, 0, sizeof(*r));
		r->used = true;
		r->export_uuid = *export_uuid;
		r->key = *key;
		r->ordinal = ordinal;
		memcpy(r->digest, digest, D1_DIGEST_BYTES);
		return r;
	}
	/* No room for a receipt is UNRECORDED, never a silent success. */
	return NULL;
}

static bool d1_visible_locked(struct d1_store *s,
			      const struct d1_objkey *object, uint64_t index,
			      d1_id_t *version)
{
	const struct d1_object *o = d1_object_find(s, object);

	if (!o || index >= D1_MAX_CHUNKS || !o->chunks[index].visible_present)
		return false;
	*version = o->chunks[index].visible;
	return true;
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
		v = d1_version_find(s, o->chunks[i].visible);
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
bool d1_fixture_release_predecessor(struct d1_store *s, d1_id_t version)
{
	struct d1_version *v;
	uint32_t i, c;
	bool ok = false;

	pthread_mutex_lock(&s->lock);
	v = d1_version_find(s, version);
	if (!v || v->released)
		goto out;
	for (i = 0; i < D1_MAX_OBJECTS; i++) {
		if (!s->objects[i].used)
			continue;
		for (c = 0; c < D1_MAX_CHUNKS; c++)
			if (s->objects[i].chunks[c].visible_present &&
			    s->objects[i].chunks[c].visible == version)
				goto out;
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
		if (t->version == version)
			goto out;
		if (t->predecessor_present && t->predecessor == version)
			goto out;
	}
	v->released = true;
	ok = true;
out:
	pthread_mutex_unlock(&s->lock);
	return ok;
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
		v = d1_version_find(s, o->chunks[i].visible);
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
	struct d1_admission *a;

	/*
	 * A record in the log was admitted when it was written.  Replay
	 * re-applies it; it does not re-authorise it, because the
	 * admission that authorised it may be long gone.
	 */
	if (s->replaying) {
		*out = &s->replay_admission;
		return D1_OK;
	}
	a = d1_admission_find(s, env->admission);
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

static bool d1_chunk_empty(struct d1_store *s, const struct d1_chunk *c)
{
	(void)s;
	return !c->visible_present && !c->pending_present;
}

/* The activation table of the memo, as a predicate. */
static bool d1_activation_allowed(bool single_writer, bool flag,
				  uint32_t stability, bool empty, bool *invalid)
{
	*invalid = false;
	if (!flag)
		return false;
	if (!single_writer) {
		/* A multi-writer request may not ask for activation at all. */
		*invalid = true;
		return false;
	}
	if (stability == D1_UNSTABLE)
		return false;
	return empty;
}

static uint32_t d1_do_write_entry(struct d1_store *s,
				  const struct d1_envelope *env,
				  const struct d1_write_entry *e,
				  struct d1_admission *a,
				  struct d1_entry_result *res)
{
	struct d1_object *o = d1_object_get(s, &env->object);
	struct d1_owner_assoc *assoc;
	struct d1_chunk *chunk;
	struct d1_txn *txn = NULL;
	struct d1_version *ver = NULL;
	uint64_t start, end;
	bool single_writer = (a->rights & D1_RIGHT_SINGLE_WRITER) != 0;
	bool activate, invalid;
	uint32_t i;

	if (!o)
		return D1_NOSPC;
	if (e->index >= D1_MAX_CHUNKS)
		return D1_INVALID;
	/* Bounds are the object's maximum, not its current EOF. */
	if (!d1_mul_u64(e->index, s->chunk_bytes, &start) ||
	    !d1_add_u64(start, e->payload_len, &end) || end > s->max_file_bytes)
		return D1_INVALID;
	if (e->payload_len < 1 || e->payload_len > s->chunk_bytes)
		return D1_INVALID;
	if (e->owner.writer != a->writer)
		return D1_INVALID;
	if (e->owner.writer == D1_WRITER_RESERVED_LOW ||
	    e->owner.writer == D1_WRITER_RESERVED_HIGH)
		return D1_INVALID;

	chunk = &o->chunks[e->index];
	res->guard = chunk->guard;

	/* An owner names one version, and only an exact replay reuses it. */
	assoc = d1_owner_find(s, &env->object.export_uuid, &e->owner);
	if (assoc &&
	    (assoc->object != d1_object_slot(s, o) || assoc->index != e->index))
		return D1_OWNER_CONFLICT;
	if (assoc)
		return D1_OWNER_CONFLICT;

	/* The guard predicate.  Absent is only a single writer's to omit. */
	if (!e->guard_check) {
		if (!single_writer)
			return D1_INVALID;
	} else {
		if (e->expected.never_written != chunk->guard.never_written ||
		    (!chunk->guard.never_written &&
		     (e->expected.generation != chunk->guard.generation ||
		      e->expected.writer != chunk->guard.writer)))
			return D1_GUARDED;
	}

	/* One uncommitted transaction per chunk in this first model. */
	if (chunk->pending_present)
		return D1_GUARDED;

	if (!d1_checksum_verify(&e->checksum, e->payload, e->payload_len))
		return D1_CHECKSUM;

	activate = d1_activation_allowed(single_writer,
					 env->body.write.activate,
					 env->body.write.stability,
					 d1_chunk_empty(s, chunk), &invalid);
	if (invalid)
		return D1_INVALID;

	for (i = 0; i < D1_MAX_TXNS && !txn; i++)
		if (!s->txns[i].used)
			txn = &s->txns[i];
	for (i = 0; i < D1_MAX_VERSIONS && !ver; i++)
		if (!s->versions[i].used)
			ver = &s->versions[i];
	if (!txn || !ver)
		return D1_NOSPC;

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

	assoc = d1_owner_add(s, &env->object.export_uuid, &e->owner);
	if (!assoc) {
		txn->used = false;
		ver->used = false;
		return D1_NOSPC;
	}
	assoc->object = ver->object;
	assoc->index = e->index;
	assoc->version = ver->id;

	/*
	 * The guard advances on every accepted write.  A never-written
	 * chunk's first success is generation zero with the granted writer;
	 * later successes increment, and nothing ever decreases it.
	 */
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
		chunk->visible_present = true;
		chunk->visible = ver->id;
		s->index_epoch++;
	} else {
		txn->phase = D1_PHASE_PREPARED;
		chunk->pending_present = true;
		chunk->pending = txn->id;
	}

	res->version_present = true;
	res->version = ver->id;
	res->txn_present = true;
	res->txn = txn->id;
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
				      struct d1_entry_result *res)
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

	txn = d1_txn_find(s, e->txn);
	if (!txn)
		return D1_INVALID;
	if (txn->index != e->index || txn->object != d1_object_slot(s, o))
		return D1_INVALID;
	if (txn->owner.cohort != e->owner.cohort ||
	    txn->owner.writer != e->owner.writer ||
	    txn->owner.co_id != e->owner.co_id)
		return D1_OWNER_CONFLICT;
	/* Only whole-cohort entry points advance a repair member. */
	if (txn->mode != D1_MODE_ORDINARY)
		return D1_INVALID;
	if (txn->admission != a->id)
		return D1_STALE_AUTH;

	chunk = &o->chunks[e->index];
	res->guard = chunk->guard;
	res->owner = txn->owner;
	res->txn_present = true;
	res->txn = txn->id;
	res->version_present = true;
	res->version = txn->version;

	if (txn->predecessor_present != e->predecessor_present ||
	    (e->predecessor_present && txn->predecessor != e->predecessor))
		return D1_NO_PREDECESSOR;

	if (!commit) {
		if (txn->phase != D1_PHASE_PREPARED)
			return D1_BAD_PHASE;
		txn->phase = D1_PHASE_FINALIZED;
		res->phase = txn->phase;
		return D1_OK;
	}

	if (txn->phase != D1_PHASE_FINALIZED)
		return D1_BAD_PHASE;
	ver = d1_version_find(s, txn->version);
	if (!ver)
		return D1_INVALID;
	/* Payload and extent metadata are replaced together. */
	txn->phase = D1_PHASE_COMMITTED;
	chunk->visible_present = true;
	chunk->visible = ver->id;
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
static uint32_t d1_do_rollback_entry(struct d1_store *s,
				     const struct d1_envelope *env,
				     const struct d1_rollback_entry *e,
				     struct d1_admission *a,
				     struct d1_entry_result *res)
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

	txn = d1_txn_find(s, e->txn);
	if (!txn || txn->index != e->index ||
	    txn->object != d1_object_slot(s, o))
		return D1_INVALID;
	if (txn->owner.cohort != e->owner.cohort ||
	    txn->owner.writer != e->owner.writer ||
	    txn->owner.co_id != e->owner.co_id)
		return D1_OWNER_CONFLICT;
	if (txn->mode != D1_MODE_ORDINARY)
		return D1_INVALID;

	chunk = &o->chunks[e->index];
	res->guard = chunk->guard;
	res->owner = txn->owner;
	res->txn_present = true;
	res->txn = txn->id;

	if (txn->phase == D1_PHASE_PREPARED ||
	    txn->phase == D1_PHASE_FINALIZED) {
		if (txn->admission != a->id)
			return D1_STALE_AUTH;
		/* Cancelling one's own private work needs no custody. */
		if (e->custody_present)
			return D1_INVALID;
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

	/* From here it is committed data, and custody is not optional. */
	if (!e->custody_present)
		return D1_STALE_AUTH;
	if ((a->rights & D1_RIGHT_REPAIR) != D1_RIGHT_REPAIR)
		return D1_STALE_AUTH;
	/*
	 * Replay re-applies a record that already held custody when it was
	 * written; the handle itself is fixture state that no log
	 * describes, so it is not looked up again.  Everything the record
	 * itself asserts is still checked.
	 */
	if (!s->replaying) {
		custody = d1_custody_find(s, e->custody);
		if (!custody)
			return D1_STALE_AUTH;
		/* Custody names the exact version it was issued over. */
		if (!chunk->visible_present ||
		    custody->version != chunk->visible)
			return D1_OWNER_CONFLICT;
	}
	if (!chunk->visible_present)
		return D1_OWNER_CONFLICT;
	if (chunk->visible != txn->version)
		return D1_OWNER_CONFLICT;
	if (e->visible_present && e->visible != chunk->visible)
		return D1_OWNER_CONFLICT;
	/* A pending transaction must be cancelled before this. */
	if (chunk->pending_present)
		return D1_GUARDED;

	ver = d1_version_find(s, txn->version);
	if (!ver)
		return D1_INVALID;
	res->version_present = true;
	res->version = ver->id;

	pred = ver->predecessor_present ? d1_version_find(s, ver->predecessor) :
					  NULL;
	if (!pred || pred->released) {
		/*
		 * Nothing to put back.  The current data stays, no episode
		 * is created, and the entry names the predecessor it was
		 * looking for.
		 */
		res->phase = txn->phase;
		return D1_NO_PREDECESSOR;
	}
	if (e->predecessor_present && e->predecessor != pred->id)
		return D1_OWNER_CONFLICT;

	/* Payload and extent are restored in the one transition. */
	chunk->visible = pred->id;
	txn->phase = D1_PHASE_ROLLED_BACK;
	s->index_epoch++;
	res->version = pred->id;
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
static uint32_t d1_do_control(struct d1_store *s, const struct d1_envelope *env,
			      struct d1_admission *a,
			      struct d1_entry_result *res)
{
	const struct d1_control_batch *cb = &env->body.control;
	struct d1_object *o = d1_object_find(s, &env->object);
	struct d1_admission *old, *fresh = NULL;
	struct d1_txn *named[D1_BATCH_ENTRIES_MAX];
	uint32_t i;

	(void)a;
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
		if (t->mode != D1_MODE_ORDINARY)
			return D1_INVALID;
		if (t->phase != D1_PHASE_PREPARED &&
		    t->phase != D1_PHASE_FINALIZED)
			return D1_BAD_PHASE;
		if (t->admission != old->id)
			return D1_OWNER_CONFLICT;
		named[i] = t;
	}

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
		if (memcmp(&fresh->issuer, &old->issuer,
			   sizeof(fresh->issuer)) != 0 ||
		    memcmp(&fresh->principal, &old->principal,
			   sizeof(fresh->principal)) != 0)
			return D1_STALE_AUTH;
		/* A read epoch the store has never reached is not a recovery. */
		if (cb->read_epoch > s->index_epoch)
			return D1_INVALID;

		for (i = 0; i < cb->count; i++) {
			named[i]->admission = fresh->id;
			named[i]->read_epoch = cb->read_epoch;
		}
		old->revoked = true;
		d1_store_verifier(s, res->verifier);
		return D1_OK;
	}

	/* lease_reap */
	for (i = 0; i < cb->count; i++) {
		struct d1_txn *t = named[i];
		struct d1_chunk *chunk = &o->chunks[t->index];

		if (chunk->pending_present && chunk->pending == t->id) {
			chunk->pending_present = false;
			chunk->pending = 0;
		}
		t->phase = D1_PHASE_ROLLED_BACK;
	}
	res->phase = D1_PHASE_ROLLED_BACK;
	d1_store_verifier(s, res->verifier);
	return D1_OK;
}

/* Defined with the rest of the journal, below. */
static bool d1_journal_intent(struct d1_store *s, const struct d1_envelope *env,
			      const struct d1_admission *a);

uint32_t d1_store_apply(struct d1_store *s, const struct d1_envelope *env,
			struct d1_result *out)
{
	uint8_t digest[D1_DIGEST_BYTES];
	struct d1_admission *a = NULL;
	uint32_t need, status, i;
	uint32_t count;
	bool commit;

	memset(out, 0, sizeof(*out));
	out->key = env->key;
	out->disposition = D1_COMPLETED;

	switch (env->op) {
	case D1_OP_WRITE_BATCH:
	case D1_OP_FINALIZE_BATCH:
	case D1_OP_COMMIT_BATCH:
	case D1_OP_ROLLBACK_BATCH:
		need = D1_RIGHT_WRITE;
		break;
	case D1_OP_RECOVERY_ADMIT:
	case D1_OP_LEASE_REAP:
		need = D1_RIGHT_CONTROL;
		break;
	default:
		/*
		 * Actor-driven repair, mixed rollback and clearing an error
		 * episode are not this slice's, and say so rather than doing
		 * part of the job.
		 */
		out->count = 0;
		return D1_UNSUPPORTED;
	}

	/*
	 * Shape first.  A request whose counts, lengths, tags or members
	 * are outside the canonical form is refused before anything reads
	 * a payload, hashes a byte or touches the store.
	 */
	if (!d1_envelope_validate(env))
		return D1_INVALID;
	pthread_mutex_lock(&s->lock);
	if (!d1_envelope_digest(env, s->scratch, s->scratch_cap, digest)) {
		pthread_mutex_unlock(&s->lock);
		return D1_INVALID;
	}
	pthread_mutex_unlock(&s->lock);

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
		/* A control operation answers once, for the whole of it. */
		count = 1;
		break;
	default:
		count = env->body.lifecycle.count;
		break;
	}
	out->count = count;

	/*
	 * Admission is an envelope-level question, so it is asked once.  A
	 * request that is not admitted leaves nothing at all behind -- no
	 * transition, no receipt and no record -- because it never entered
	 * the store.  UNRECORDED says exactly that.
	 */
	pthread_mutex_lock(&s->lock);
	status = d1_admission_check(s, env, need, &a);
	if (status == D1_OK && s->journaling && !s->replaying &&
	    !d1_journal_intent(s, env, a)) {
		/*
		 * The intent is written before anything is published, so a
		 * refused append leaves a store that never made the change
		 * and a log that never claimed it did.
		 */
		pthread_mutex_unlock(&s->lock);
		out->count = 0;
		out->disposition = D1_UNRECORDED;
		return D1_IO;
	}
	pthread_mutex_unlock(&s->lock);

	if (status != D1_OK) {
		for (i = 0; i < count; i++) {
			out->entries[i].status = status;
			out->entries[i].disposition = D1_UNRECORDED;
			out->entries[i].stability = D1_FILE_SYNC;
			d1_store_verifier(s, out->entries[i].verifier);
		}
		out->index_epoch = s->index_epoch;
		out->eof = d1_eof_locked(s, &env->object);
		return D1_OK;
	}

	for (i = 0; i < count; i++) {
		struct d1_entry_result *res = &out->entries[i];
		struct d1_receipt *receipt;

		pthread_mutex_lock(&s->lock);
		res->stability = D1_FILE_SYNC;
		res->disposition = D1_COMPLETED;
		d1_store_verifier(s, res->verifier);

		/*
		 * The key is looked up before anything is revalidated: an
		 * exact replay answers from its receipt without mutating,
		 * and the same key with a different body conflicts.
		 */
		receipt = d1_receipt_find(s, &env->object.export_uuid,
					  &env->key, i);
		if (receipt) {
			if (memcmp(receipt->digest, digest, D1_DIGEST_BYTES) !=
			    0) {
				res->status = D1_REPLAY_CONFLICT;
				pthread_mutex_unlock(&s->lock);
				continue;
			}
			*res = receipt->result;
			pthread_mutex_unlock(&s->lock);
			continue;
		}

		status = D1_OK;
		{
			if (env->op == D1_OP_WRITE_BATCH) {
				status = d1_do_write_entry(
					s, env, &env->body.write.entries[i], a,
					res);
			} else if (env->op == D1_OP_ROLLBACK_BATCH) {
				status = d1_do_rollback_entry(
					s, env, &env->body.rollback.entries[i],
					a, res);
			} else if (env->op == D1_OP_RECOVERY_ADMIT ||
				   env->op == D1_OP_LEASE_REAP) {
				status = d1_do_control(s, env, a, res);
				out->disposition = res->disposition;
			} else {
				if (!d1_verifier_matches(
					    s,
					    env->body.lifecycle.prior_verifier))
					status = D1_STALE_AUTH;
				else
					status = d1_do_lifecycle_entry(
						s, env,
						&env->body.lifecycle.entries[i],
						a, commit, res);
			}
		}
		res->status = status;

		/*
		 * A semantic error is a recorded receipt with no state
		 * change; only an inability to record one is UNRECORDED.
		 */
		receipt = d1_receipt_add(s, &env->object.export_uuid, &env->key,
					 i, digest);
		if (!receipt) {
			res->disposition = D1_UNRECORDED;
			res->status = D1_NOSPC;
			pthread_mutex_unlock(&s->lock);
			continue;
		}
		receipt->result = *res;
		pthread_mutex_unlock(&s->lock);
	}

	pthread_mutex_lock(&s->lock);
	out->index_epoch = s->index_epoch;
	out->eof = d1_eof_locked(s, &env->object);
	pthread_mutex_unlock(&s->lock);
	return D1_OK;
}

/*
 * Whether @a may read @object.  Reads are checked against the same
 * admission table as writes; a revoked or expired admission cannot open
 * a view, and one issued for a different object cannot either.
 */
static uint32_t d1_read_admission(struct d1_store *s, d1_id_t admission,
				  const struct d1_objkey *object)
{
	struct d1_admission *a = d1_admission_find(s, admission);

	if (!a || a->revoked || a->expired)
		return D1_STALE_AUTH;
	if (memcmp(&a->object, object, sizeof(*object)) != 0)
		return D1_STALE_AUTH;
	if ((a->rights & D1_RIGHT_READ) != D1_RIGHT_READ)
		return D1_STALE_AUTH;
	return D1_OK;
}

/* The owner's own finalized version on this chunk, if it has one. */
static struct d1_version *d1_own_finalized(struct d1_store *s,
					   const struct d1_chunk *c,
					   const struct d1_owner *owner)
{
	struct d1_txn *t;

	if (!owner || !c->pending_present)
		return NULL;
	t = d1_txn_find(s, c->pending);
	if (!t || t->phase != D1_PHASE_FINALIZED)
		return NULL;
	if (t->owner.cohort != owner->cohort ||
	    t->owner.writer != owner->writer || t->owner.co_id != owner->co_id)
		return NULL;
	return d1_version_find(s, t->version);
}

static void d1_view_unpin(struct d1_store *s, struct d1_view *v)
{
	uint32_t i;

	for (i = 0; i < D1_MAX_CHUNKS; i++) {
		struct d1_version *ver;

		if (!v->present[i])
			continue;
		ver = d1_version_find(s, v->version[i]);
		if (ver && ver->pins)
			ver->pins--;
		v->present[i] = false;
	}
}

uint32_t d1_view_open(struct d1_store *s, const struct d1_objkey *object,
		      d1_id_t admission, uint32_t selection,
		      const struct d1_owner *owner, uint64_t range_begin,
		      uint64_t range_end, struct d1_view **out)
{
	struct d1_object *o;
	struct d1_view *v = NULL;
	uint32_t status, i;

	*out = NULL;
	if (selection != D1_SELECT_ORDINARY && selection != D1_SELECT_OWNER)
		return D1_INVALID;
	if (range_begin >= range_end || range_end > D1_MAX_CHUNKS)
		return D1_INVALID;

	pthread_mutex_lock(&s->lock);
	status = d1_read_admission(s, admission, object);
	if (status != D1_OK)
		goto out;
	o = d1_object_find(s, object);
	if (!o) {
		status = D1_INVALID;
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
	v->object = d1_object_slot(s, o);
	v->range_begin = range_begin;
	v->range_end = range_end;

	for (i = (uint32_t)range_begin; i < (uint32_t)range_end; i++) {
		struct d1_chunk *c = &o->chunks[i];
		struct d1_version *ver = NULL;

		if (selection == D1_SELECT_OWNER)
			ver = d1_own_finalized(s, c, owner);
		if (!ver && c->visible_present)
			ver = d1_version_find(s, c->visible);
		if (!ver)
			continue;
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

	v->eof = d1_eof_locked(s, object);
	*out = v;
	status = D1_OK;
out:
	pthread_mutex_unlock(&s->lock);
	return status;
}

bool d1_view_version(const struct d1_view *v, uint64_t index, d1_id_t *ver)
{
	if (index >= D1_MAX_CHUNKS || !v->present[index])
		return false;
	*ver = v->version[index];
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
	uint32_t done = 0;

	*out_len = 0;
	if (!v->used || !s)
		return D1_INVALID;
	if (offset >= v->eof || len == 0)
		return D1_OK;
	/* The view's own EOF bounds the read, not the store's current one. */
	if (v->eof - offset < (uint64_t)len)
		len = (uint32_t)(v->eof - offset);

	pthread_mutex_lock(&s->lock);
	while (done < len) {
		uint64_t index = at / s->chunk_bytes;
		uint32_t within = (uint32_t)(at % s->chunk_bytes);
		uint32_t span = s->chunk_bytes - within;
		struct d1_version *ver = NULL;

		if (span > len - done)
			span = len - done;
		if (index < D1_MAX_CHUNKS && v->present[index])
			ver = d1_version_find(s, v->version[index]);
		/*
		 * A hole inside the view's EOF is zeros, and so is the part
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

void d1_view_close(struct d1_store *s, struct d1_view *v)
{
	if (!v)
		return;
	pthread_mutex_lock(&s->lock);
	d1_view_unpin(s, v);
	v->used = false;
	pthread_mutex_unlock(&s->lock);
}

/*
 * The journal, and rebuilding from it.
 *
 * Records are written ahead of the state they describe: the intent is
 * durable before anything is published, so a refused append leaves a
 * store that never made the change and a log that never claimed it did.
 * Replay then re-applies the intents in order through the same reducer,
 * which is what makes "same log, same state" a property of the code
 * rather than a promise about it.
 */

#define D1_START_PAYLOAD_BYTES (D1_UUID_BYTES + 8u + 4u + 8u)
#define D1_CHECKPOINT_PAYLOAD_BYTES (8u + 8u + 8u)

uint32_t d1_store_journal_enable(struct d1_store *s)
{
	uint8_t payload[D1_START_PAYLOAD_BYTES];
	struct d1_cursor cur;
	uint32_t status = D1_OK;

	pthread_mutex_lock(&s->lock);
	if (s->journaling) {
		status = D1_INVALID;
		goto out;
	}
	if (!d1_journal_init(&s->journal)) {
		status = D1_NOSPC;
		goto out;
	}
	d1_enc_init(&cur, payload, sizeof(payload));
	d1_enc_uuid(&cur, &s->uuid);
	d1_enc_u64(&cur, s->incarnation);
	d1_enc_u32(&cur, s->chunk_bytes);
	d1_enc_u64(&cur, s->max_file_bytes);
	if (cur.bad || !d1_journal_append(&s->journal, D1_REC_START, payload,
					  (uint32_t)cur.len)) {
		d1_journal_fini(&s->journal);
		status = D1_IO;
		goto out;
	}
	s->journaling = true;
out:
	pthread_mutex_unlock(&s->lock);
	return status;
}

uint32_t d1_store_checkpoint(struct d1_store *s)
{
	uint8_t payload[D1_CHECKPOINT_PAYLOAD_BYTES];
	struct d1_cursor cur;
	uint32_t status = D1_OK;

	pthread_mutex_lock(&s->lock);
	if (!s->journaling) {
		status = D1_INVALID;
		goto out;
	}
	d1_enc_init(&cur, payload, sizeof(payload));
	d1_enc_u64(&cur, s->next_txn);
	d1_enc_u64(&cur, s->next_version);
	d1_enc_u64(&cur, s->index_epoch);
	if (cur.bad || !d1_journal_append(&s->journal, D1_REC_CONTROL, payload,
					  (uint32_t)cur.len))
		status = D1_IO;
out:
	pthread_mutex_unlock(&s->lock);
	return status;
}

/*
 * Write the intent for one envelope.  The record carries the writer and
 * rights the operation was admitted under, because replay must reapply
 * it exactly as it was accepted and the admission itself is fixture
 * state that no log describes.
 */
static bool d1_journal_intent(struct d1_store *s, const struct d1_envelope *env,
			      const struct d1_admission *a)
{
	struct d1_cursor cur;
	size_t body;

	/* The store's own scratch, under the store's own lock. */
	d1_enc_init(&cur, s->scratch, s->scratch_cap);
	d1_enc_u32(&cur, a->writer);
	d1_enc_u32(&cur, a->rights);
	if (cur.bad)
		return false;
	body = d1_envelope_encode(env, s->scratch + cur.len,
				  s->scratch_cap - cur.len);
	if (!body || cur.len + body > D1_JOURNAL_RECORD_MAX)
		return false;
	return d1_journal_append(&s->journal, D1_REC_ENTRY, s->scratch,
				 (uint32_t)(cur.len + body));
}

static uint32_t d1_replay_start(struct d1_store *s, const uint8_t *payload,
				uint32_t len)
{
	struct d1_cursor cur;
	struct d1_uuid uuid;
	uint64_t incarnation, max_file_bytes;
	uint32_t chunk_bytes;

	d1_dec_init(&cur, payload, len);
	if (!d1_dec_uuid(&cur, &uuid) || !d1_dec_u64(&cur, &incarnation) ||
	    !d1_dec_u32(&cur, &chunk_bytes) ||
	    !d1_dec_u64(&cur, &max_file_bytes) || !d1_dec_finished(&cur))
		return D1_INVALID;
	/* A log only rebuilds the store it was written for. */
	if (memcmp(&uuid, &s->uuid, sizeof(uuid)) != 0 ||
	    chunk_bytes != s->chunk_bytes ||
	    max_file_bytes != s->max_file_bytes)
		return D1_INVALID;
	s->incarnation = incarnation;
	return D1_OK;
}

static uint32_t d1_replay_checkpoint(struct d1_store *s, const uint8_t *payload,
				     uint32_t len)
{
	struct d1_cursor cur;
	uint64_t txn, version, epoch;

	d1_dec_init(&cur, payload, len);
	if (!d1_dec_u64(&cur, &txn) || !d1_dec_u64(&cur, &version) ||
	    !d1_dec_u64(&cur, &epoch) || !d1_dec_finished(&cur))
		return D1_INVALID;
	/*
	 * Reaching a checkpoint and disagreeing with it means the replay
	 * produced a different store from the one that wrote the log.
	 * That is worth stopping for.
	 */
	if (txn != s->next_txn || version != s->next_version ||
	    epoch != s->index_epoch)
		return D1_INVALID;
	return D1_OK;
}

static uint32_t d1_replay_entry(struct d1_store *s, const uint8_t *payload,
				uint32_t len)
{
	struct d1_cursor cur;
	struct d1_envelope env;
	struct d1_result res;
	uint32_t writer, rights;

	d1_dec_init(&cur, payload, len);
	if (!d1_dec_u32(&cur, &writer) || !d1_dec_u32(&cur, &rights))
		return D1_INVALID;
	if (!d1_envelope_decode(payload + cur.len, len - cur.len, &env))
		return D1_INVALID;

	memset(&s->replay_admission, 0, sizeof(s->replay_admission));
	s->replay_admission.used = true;
	s->replay_admission.id = env.admission;
	s->replay_admission.object = env.object;
	s->replay_admission.writer = writer;
	s->replay_admission.rights = rights;
	s->replay_admission.incarnation = s->incarnation;

	/*
	 * The entry results are the log's business, not the replay's; what
	 * matters is that the same transitions happen in the same order.
	 */
	return d1_store_apply(s, &env, &res) == D1_UNSUPPORTED ? D1_INVALID :
								 D1_OK;
}

uint32_t d1_store_replay(struct d1_store *s, const uint8_t *log, size_t len)
{
	struct d1_journal_cursor c;
	const uint8_t *payload;
	uint32_t type, plen, status = D1_OK;
	uint64_t seq, expect = 1;
	bool saw_start = false;

	pthread_mutex_lock(&s->lock);
	if (s->journaling || s->replaying) {
		pthread_mutex_unlock(&s->lock);
		return D1_INVALID;
	}
	s->replaying = true;
	pthread_mutex_unlock(&s->lock);

	d1_journal_cursor_init(&c, log, len);
	while (d1_journal_next(&c, &type, &seq, &payload, &plen)) {
		/* A gap in the sequence is a log this reader cannot trust. */
		if (seq != expect) {
			status = D1_INVALID;
			break;
		}
		expect++;
		if (!saw_start && type != D1_REC_START) {
			status = D1_INVALID;
			break;
		}
		switch (type) {
		case D1_REC_START:
			if (saw_start) {
				status = D1_INVALID;
				break;
			}
			saw_start = true;
			status = d1_replay_start(s, payload, plen);
			break;
		case D1_REC_ENTRY:
			status = d1_replay_entry(s, payload, plen);
			break;
		case D1_REC_CONTROL:
			status = d1_replay_checkpoint(s, payload, plen);
			break;
		default:
			status = D1_INVALID;
			break;
		}
		if (status != D1_OK)
			break;
	}
	if (status == D1_OK && !saw_start)
		status = D1_INVALID;

	pthread_mutex_lock(&s->lock);
	s->replaying = false;
	pthread_mutex_unlock(&s->lock);
	return status;
}

/*
 * Public observers.  They take the model lock, so a caller reading
 * state while another thread mutates it sees one consistent answer
 * rather than a half-applied one.  The reducer uses the unlocked forms
 * above, because it already holds the lock.
 */
bool d1_store_visible(struct d1_store *s, const struct d1_objkey *object,
		      uint64_t index, d1_id_t *version)
{
	bool found;

	pthread_mutex_lock(&s->lock);
	found = d1_visible_locked(s, object, index, version);
	pthread_mutex_unlock(&s->lock);
	return found;
}

bool d1_store_guard(struct d1_store *s, const struct d1_objkey *object,
		    uint64_t index, struct d1_guard *guard)
{
	bool found;

	pthread_mutex_lock(&s->lock);
	found = d1_guard_locked(s, object, index, guard);
	pthread_mutex_unlock(&s->lock);
	return found;
}

uint64_t d1_store_eof(struct d1_store *s, const struct d1_objkey *object)
{
	uint64_t eof;

	pthread_mutex_lock(&s->lock);
	eof = d1_eof_locked(s, object);
	pthread_mutex_unlock(&s->lock);
	return eof;
}

uint32_t d1_store_holes(struct d1_store *s, const struct d1_objkey *object,
			struct d1_interval *out, uint32_t max)
{
	uint32_t n;

	pthread_mutex_lock(&s->lock);
	n = d1_holes_locked(s, object, out, max);
	pthread_mutex_unlock(&s->lock);
	return n;
}
