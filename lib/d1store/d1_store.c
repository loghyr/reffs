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

struct d1_admission {
	bool used;
	d1_id_t id;
	struct d1_objkey object;
	uint32_t writer;
	uint32_t rights;
	bool revoked;
	bool expired;
	uint64_t incarnation;
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
	if (pthread_mutex_init(&s->lock, NULL) != 0) {
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
	pthread_mutex_destroy(&s->lock);
	free(s);
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

d1_id_t d1_fixture_admit(struct d1_store *s, const struct d1_objkey *object,
			 uint32_t writer, uint32_t rights)
{
	uint32_t i;

	/* Reserved writer IDs are never issued. */
	if (writer == D1_WRITER_RESERVED_LOW ||
	    writer == D1_WRITER_RESERVED_HIGH)
		return 0;
	for (i = 0; i < D1_MAX_ADMISSIONS; i++) {
		struct d1_admission *a = &s->admissions[i];

		if (a->used)
			continue;
		memset(a, 0, sizeof(*a));
		a->used = true;
		a->id = s->next_admission++;
		a->object = *object;
		a->writer = writer;
		a->rights = rights;
		a->incarnation = s->incarnation;
		return a->id;
	}
	return 0;
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
	struct d1_admission *a = d1_admission_find(s, admission);

	if (a)
		a->revoked = true;
}

void d1_fixture_expire(struct d1_store *s, d1_id_t admission)
{
	struct d1_admission *a = d1_admission_find(s, admission);

	if (a)
		a->expired = true;
}

d1_id_t d1_fixture_custody(struct d1_store *s, d1_id_t version)
{
	uint32_t i;

	for (i = 0; i < D1_MAX_CUSTODY; i++) {
		struct d1_custody *c = &s->custody[i];

		if (c->used)
			continue;
		c->used = true;
		c->id = s->next_custody++;
		c->version = version;
		return c->id;
	}
	return 0;
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

bool d1_store_visible(const struct d1_store *s, const struct d1_objkey *object,
		      uint64_t index, d1_id_t *version)
{
	const struct d1_object *o =
		d1_object_find((struct d1_store *)(uintptr_t)s, object);

	if (!o || index >= D1_MAX_CHUNKS || !o->chunks[index].visible_present)
		return false;
	*version = o->chunks[index].visible;
	return true;
}

bool d1_store_guard(const struct d1_store *s, const struct d1_objkey *object,
		    uint64_t index, struct d1_guard *guard)
{
	const struct d1_object *o =
		d1_object_find((struct d1_store *)(uintptr_t)s, object);

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
uint64_t d1_store_eof(const struct d1_store *s, const struct d1_objkey *object)
{
	struct d1_store *m = (struct d1_store *)(uintptr_t)s;
	const struct d1_object *o = d1_object_find(m, object);
	uint64_t eof = 0;
	uint32_t i;

	if (!o)
		return 0;
	for (i = 0; i < D1_MAX_CHUNKS; i++) {
		const struct d1_version *v;
		uint64_t start, end;

		if (!o->chunks[i].visible_present)
			continue;
		v = d1_version_find(m, o->chunks[i].visible);
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
 * Eligible only when nothing depends on it: no chunk makes it visible,
 * no uncommitted transaction names it, no read pin holds it, and it has
 * not been released already.  What this changes is what a later
 * rollback may restore; it reports nothing about bytes.
 */
bool d1_fixture_release_predecessor(struct d1_store *s, d1_id_t version)
{
	struct d1_version *v = d1_version_find(s, version);
	uint32_t i, c;

	if (!v || v->released || v->pins)
		return false;
	for (i = 0; i < D1_MAX_OBJECTS; i++) {
		if (!s->objects[i].used)
			continue;
		for (c = 0; c < D1_MAX_CHUNKS; c++)
			if (s->objects[i].chunks[c].visible_present &&
			    s->objects[i].chunks[c].visible == version)
				return false;
	}
	for (i = 0; i < D1_MAX_TXNS; i++) {
		const struct d1_txn *t = &s->txns[i];

		if (!t->used || t->version != version)
			continue;
		if (t->phase == D1_PHASE_PREPARED ||
		    t->phase == D1_PHASE_FINALIZED)
			return false;
	}
	v->released = true;
	return true;
}

uint32_t d1_store_holes(const struct d1_store *s,
			const struct d1_objkey *object, struct d1_interval *out,
			uint32_t max)
{
	struct d1_store *m = (struct d1_store *)(uintptr_t)s;
	const struct d1_object *o = d1_object_find(m, object);
	uint64_t eof = d1_store_eof(s, object);
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
		v = d1_version_find(m, o->chunks[i].visible);
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
	custody = d1_custody_find(s, e->custody);
	if (!custody)
		return D1_STALE_AUTH;
	/* Custody names the exact version it was issued over. */
	if (!chunk->visible_present || custody->version != chunk->visible)
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
	default:
		/*
		 * Rollback, recovery and reaping arrive with the rest of this
		 * slice; repair never does.  Either way nothing is mutated.
		 */
		out->count = 0;
		return D1_UNSUPPORTED;
	}

	if (!d1_envelope_digest(env, digest))
		return D1_INVALID;

	commit = env->op == D1_OP_COMMIT_BATCH;
	switch (env->op) {
	case D1_OP_WRITE_BATCH:
		count = env->body.write.count;
		break;
	case D1_OP_ROLLBACK_BATCH:
		count = env->body.rollback.count;
		break;
	default:
		count = env->body.lifecycle.count;
		break;
	}
	out->count = count;

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

		status = d1_admission_check(s, env, need, &a);
		if (status == D1_OK) {
			if (env->op == D1_OP_WRITE_BATCH) {
				status = d1_do_write_entry(
					s, env, &env->body.write.entries[i], a,
					res);
			} else if (env->op == D1_OP_ROLLBACK_BATCH) {
				status = d1_do_rollback_entry(
					s, env, &env->body.rollback.entries[i],
					a, res);
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
	out->eof = d1_store_eof(s, &env->object);
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

	v->eof = d1_store_eof(s, object);
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
