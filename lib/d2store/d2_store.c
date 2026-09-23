/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "d1_codec.h"
#include "d2_store.h"

struct d2_store {
	pthread_mutex_t lock;
	struct d2_files *files;
	struct d1_store *model;
	struct d1_uuid uuid;
	uint32_t chunk_bytes;
	uint64_t max_file_bytes;
	uint64_t next_payload_seq;
	uint64_t synthetic_sequence;
	uint32_t registered_count;
	struct {
		struct d1_objkey object;
		uint8_t file_key[32];
	} registered[D1_MAX_OBJECTS];
	struct {
		bool used;
		uint64_t id;
		struct d1_objkey object;
		struct d1_fixture_authority auth;
	} admissions[D1_MAX_ADMISSIONS];
	struct {
		bool used;
		struct d1_uuid export_uuid;
		struct d1_opkey key;
		uint8_t digest[D1_DIGEST_BYTES];
		struct d1_result result;
	} receipts[D1_MAX_RECEIPTS];
	struct {
		bool used;
		uint64_t txn_id;
		uint64_t version_id;
		uint64_t admission_id;
	} work[D1_MAX_TXNS];
	struct {
		bool used;
		uint64_t custody_id;
		uint64_t version_id;
		uint64_t admission_id;
	} custodies[D1_MAX_CUSTODY];
	bool fenced;
};

struct d2_replay {
	struct d2_files *files;
	struct d2_store *store;
	uint64_t highest_payload_id;
	uint64_t last_incarnation;
	uint32_t status;
};

static bool d2_same_object(const struct d1_objkey *a, const struct d1_objkey *b)
{
	return !memcmp(a, b, sizeof(*a));
}

static bool d2_same_key(const struct d1_opkey *a, const struct d1_opkey *b)
{
	return !memcmp(a->origin.bytes, b->origin.bytes, D1_UUID_BYTES) &&
	       a->sequence == b->sequence && a->ordinal == b->ordinal;
}

static int d2_receipt_lookup(const struct d2_store *s,
			     const struct d1_envelope *env,
			     const uint8_t digest[D1_DIGEST_BYTES],
			     struct d1_result *result)
{
	uint32_t i;

	for (i = 0; i < D1_MAX_RECEIPTS; i++) {
		if (!s->receipts[i].used ||
		    memcmp(s->receipts[i].export_uuid.bytes,
			   env->object.export_uuid.bytes, D1_UUID_BYTES) ||
		    !d2_same_key(&s->receipts[i].key, &env->key))
			continue;
		if (!memcmp(s->receipts[i].digest, digest, D1_DIGEST_BYTES)) {
			*result = s->receipts[i].result;
			return 1;
		}
		memset(result, 0, sizeof(*result));
		result->key = env->key;
		result->count = 1;
		result->disposition = D1_COMPLETED;
		result->entries[0].status = D1_REPLAY_CONFLICT;
		result->entries[0].stability = D1_FILE_SYNC;
		result->entries[0].disposition = D1_COMPLETED;
		d1_store_verifier(s->model, result->entries[0].verifier);
		return 2;
	}
	return 0;
}

static bool d2_receipt_remember(struct d2_store *s,
				const struct d1_uuid *export_uuid,
				const struct d1_opkey *key,
				const uint8_t digest[D1_DIGEST_BYTES],
				const struct d1_result *result)
{
	uint32_t i;

	for (i = 0; i < D1_MAX_RECEIPTS; i++) {
		if (s->receipts[i].used) {
			if (!memcmp(s->receipts[i].export_uuid.bytes,
				    export_uuid->bytes, D1_UUID_BYTES) &&
			    d2_same_key(&s->receipts[i].key, key))
				return !memcmp(s->receipts[i].digest, digest,
					       D1_DIGEST_BYTES);
			continue;
		}
		s->receipts[i].used = true;
		s->receipts[i].export_uuid = *export_uuid;
		s->receipts[i].key = *key;
		memcpy(s->receipts[i].digest, digest, D1_DIGEST_BYTES);
		s->receipts[i].result = *result;
		return true;
	}
	return false;
}

static bool d2_work_remember(struct d2_store *s, uint64_t txn_id,
			     uint64_t version_id, uint64_t admission_id)
{
	uint32_t i;

	if (!txn_id || !version_id || !admission_id)
		return true;
	for (i = 0; i < D1_MAX_TXNS; i++) {
		if (s->work[i].used && s->work[i].txn_id == txn_id)
			return s->work[i].version_id == version_id &&
			       s->work[i].admission_id == admission_id;
		if (!s->work[i].used) {
			s->work[i].used = true;
			s->work[i].txn_id = txn_id;
			s->work[i].version_id = version_id;
			s->work[i].admission_id = admission_id;
			return true;
		}
	}
	return false;
}

static const typeof(((struct d2_store *)0)->work[0]) *
d2_work_by_version(const struct d2_store *s, uint64_t version_id)
{
	uint32_t i;

	for (i = 0; i < D1_MAX_TXNS; i++)
		if (s->work[i].used && s->work[i].version_id == version_id)
			return &s->work[i];
	return NULL;
}

static const typeof(((struct d2_store *)0)->work[0]) *
d2_work_by_txn(const struct d2_store *s, uint64_t txn_id)
{
	uint32_t i;

	for (i = 0; i < D1_MAX_TXNS; i++)
		if (s->work[i].used && s->work[i].txn_id == txn_id)
			return &s->work[i];
	return NULL;
}

static bool d2_custody_remember(struct d2_store *s, uint64_t custody_id,
				uint64_t version_id, uint64_t admission_id)
{
	uint32_t i;

	for (i = 0; i < D1_MAX_CUSTODY; i++) {
		if (s->custodies[i].used &&
		    s->custodies[i].custody_id == custody_id)
			return s->custodies[i].version_id == version_id &&
			       s->custodies[i].admission_id == admission_id;
		if (!s->custodies[i].used) {
			s->custodies[i].used = true;
			s->custodies[i].custody_id = custody_id;
			s->custodies[i].version_id = version_id;
			s->custodies[i].admission_id = admission_id;
			return true;
		}
	}
	return false;
}

static const typeof(((struct d2_store *)0)->custodies[0]) *
d2_custody_by_version(const struct d2_store *s, uint64_t version_id)
{
	uint32_t i;

	for (i = 0; i < D1_MAX_CUSTODY; i++)
		if (s->custodies[i].used &&
		    s->custodies[i].version_id == version_id)
			return &s->custodies[i];
	return NULL;
}

static bool d2_rollback_supported(const struct d2_store *s,
				  const struct d1_envelope *env)
{
	const typeof(s->work[0]) *work;
	const typeof(s->custodies[0]) *custody;
	const struct d1_rollback_entry *entry = &env->body.rollback.entries[0];

	if (!entry->custody_present)
		return true;
	work = d2_work_by_txn(s, entry->txn.raw);
	if (!work)
		return false;
	custody = d2_custody_by_version(s, work->version_id);
	return custody && custody->custody_id == entry->custody.raw &&
	       custody->admission_id == env->admission.raw;
}

static void d2_stateid(uint64_t admission, uint32_t *seqid, uint8_t other[12])
{
	static const uint8_t prefix[8] = { 'D', '2', 'B', 'S',
					   'T', 'A', 'T', 'E' };
	uint8_t stateid[16];
	unsigned int i;

	memcpy(stateid, prefix, sizeof(prefix));
	for (i = 0; i < 8; i++)
		stateid[8 + i] = (uint8_t)(admission >> (56 - 8 * i));
	*seqid = ((uint32_t)stateid[0] << 24) | ((uint32_t)stateid[1] << 16) |
		 ((uint32_t)stateid[2] << 8) | stateid[3];
	memcpy(other, stateid + 4, 12);
}

static const struct d1_fixture_authority *
d2_admission_find(const struct d2_store *s, uint64_t id)
{
	uint32_t i;

	for (i = 0; i < D1_MAX_ADMISSIONS; i++)
		if (s->admissions[i].used && s->admissions[i].id == id)
			return &s->admissions[i].auth;
	return NULL;
}

static bool d2_admission_matches(const struct d2_store *s, uint64_t id,
				 const struct d1_objkey *object)
{
	uint32_t i;

	for (i = 0; i < D1_MAX_ADMISSIONS; i++)
		if (s->admissions[i].used && s->admissions[i].id == id)
			return d2_same_object(&s->admissions[i].object, object);
	return false;
}

static bool d2_env_digest(const struct d1_envelope *env,
			  uint8_t digest[D1_DIGEST_BYTES])
{
	uint8_t *scratch = malloc(D1_ENVELOPE_MAX);
	bool ok;

	if (!scratch)
		return false;
	ok = d1_envelope_digest(env, scratch, D1_ENVELOPE_MAX, digest);
	free(scratch);
	return ok;
}

static bool d2_admission_remember(struct d2_store *s, uint64_t id,
				  const struct d1_objkey *object,
				  const struct d1_fixture_authority *auth)
{
	uint32_t i;

	for (i = 0; i < D1_MAX_ADMISSIONS; i++) {
		if (s->admissions[i].used && s->admissions[i].id == id)
			return d2_same_object(&s->admissions[i].object,
					      object) &&
			       !memcmp(&s->admissions[i].auth, auth,
				       sizeof(*auth));
		if (!s->admissions[i].used) {
			s->admissions[i].used = true;
			s->admissions[i].id = id;
			s->admissions[i].object = *object;
			s->admissions[i].auth = *auth;
			return true;
		}
	}
	return false;
}

static void d2_admission_encode(struct d2_store *s, uint64_t id,
				struct d2_admission_block *out)
{
	const struct d1_fixture_authority *auth = d2_admission_find(s, id);

	memset(out, 0, sizeof(*out));
	if (!auth)
		return;
	memcpy(out->session, auth->session, 16);
	memcpy(out->principal, auth->principal.bytes, 16);
	memcpy(out->issuer, auth->issuer.bytes, 16);
	out->authority_epoch = auth->authority_epoch;
	out->fence_sequence = auth->fence_sequence;
	out->lease_epoch = auth->lease_epoch;
	out->client_id = id;
	d2_stateid(id, &out->stateid_seqid, out->stateid_other);
	out->writer = auth->writer;
	out->rights = auth->rights;
}

static bool d2_replay_registration(struct d2_replay *r,
				   const struct d2_control *control)
{
	struct d1_objkey object;
	uint32_t i;

	if (control->subtype != D2_CTL_FILE_REGISTER ||
	    control->transition != D2_COMMITTED || control->status != D1_OK)
		return true;
	memcpy(object.export_uuid.bytes, control->body + 36, 16);
	memcpy(object.object_uuid.bytes, control->body + 52, 16);
	for (i = 0; i < r->store->registered_count; i++)
		if (!memcmp(r->store->registered[i].file_key, control->body,
			    32))
			return false;
	if (r->store->registered_count == D1_MAX_OBJECTS)
		return false;
	i = r->store->registered_count++;
	r->store->registered[i].object = object;
	memcpy(r->store->registered[i].file_key, control->body, 32);
	return true;
}

static bool d2_replay_liveness(struct d2_replay *r,
			       const struct d2_control *control)
{
	const struct d1_fixture_authority *auth;
	struct d1_cursor cursor;
	uint8_t session[16], principal[16], other[12];
	uint64_t client_id, lease_epoch;
	uint32_t reason, seqid, expected_seqid;

	if (control->subtype != D2_CTL_REVOKE_STATEID &&
	    control->subtype != D2_CTL_LEASE_EXPIRE)
		return true;
	client_id = control->admission_client_id;
	auth = d2_admission_find(r->store, client_id);
	if (!auth || control->transition != D2_COMMITTED ||
	    control->status != D1_OK ||
	    memcmp(control->admission_issuer, auth->issuer.bytes, 16) ||
	    control->admission_authority_epoch != auth->authority_epoch)
		return false;
	d1_dec_init(&cursor, control->body, control->body_len);
	if (control->subtype == D2_CTL_REVOKE_STATEID) {
		d2_stateid(client_id, &expected_seqid, other);
		if (!d1_dec_u32(&cursor, &seqid) ||
		    !d1_dec_raw(&cursor, session, 12) ||
		    !d1_dec_u32(&cursor, &reason) ||
		    !d1_dec_raw(&cursor, session, 16) ||
		    !d1_dec_u64(&cursor, &client_id) ||
		    !d1_dec_raw(&cursor, principal, 16) ||
		    !d1_dec_finished(&cursor) || seqid != expected_seqid ||
		    memcmp(control->body + 4, other, 12) ||
		    client_id != control->admission_client_id || !reason)
			return false;
		d1_fixture_revoke(r->store->model,
				  d1_fixture_admission_handle(r->store->model,
							      client_id));
	} else {
		if (!d1_dec_raw(&cursor, session, 16) ||
		    !d1_dec_u64(&cursor, &client_id) ||
		    !d1_dec_raw(&cursor, principal, 16) ||
		    !d1_dec_u64(&cursor, &lease_epoch) ||
		    !d1_dec_u32(&cursor, &reason) ||
		    !d1_dec_finished(&cursor) ||
		    client_id != control->admission_client_id || !reason ||
		    lease_epoch != auth->lease_epoch)
			return false;
		d1_fixture_expire(r->store->model,
				  d1_fixture_admission_handle(r->store->model,
							      client_id));
	}
	return !memcmp(session, auth->session, 16) &&
	       !memcmp(principal, auth->principal.bytes, 16);
}

static bool d2_replay_custody(struct d2_replay *r,
			      const struct d2_control *control)
{
	const typeof(r->store->work[0]) *work;
	const struct d1_fixture_authority *auth;
	struct d1_cursor cursor;
	d1_custody_id custody;
	uint8_t holder[16];
	uint64_t custody_id, version_id;
	uint32_t op;

	if (control->subtype != D2_CTL_CUSTODY)
		return true;
	d1_dec_init(&cursor, control->body, control->body_len);
	if (!d1_dec_u64(&cursor, &custody_id) ||
	    !d1_dec_u64(&cursor, &version_id) || !d1_dec_u32(&cursor, &op) ||
	    !d1_dec_raw(&cursor, holder, sizeof(holder)) ||
	    !d1_dec_finished(&cursor) || op != 1 ||
	    control->transition != D2_COMMITTED || control->status != D1_OK)
		return false;
	work = d2_work_by_version(r->store, version_id);
	if (!work || work->admission_id != control->admission_client_id)
		return false;
	auth = d2_admission_find(r->store, work->admission_id);
	if (!auth || memcmp(holder, auth->principal.bytes, sizeof(holder)) ||
	    memcmp(control->admission_issuer, auth->issuer.bytes, 16) ||
	    control->admission_authority_epoch != auth->authority_epoch)
		return false;
	custody = d1_fixture_custody(r->store->model,
				     d1_fixture_version_handle(r->store->model,
							       version_id));
	return custody.raw == custody_id &&
	       d2_custody_remember(r->store, custody_id, version_id,
				   work->admission_id);
}

static bool d2_replay_admission(struct d2_replay *r,
				const struct d2_entry *entry)
{
	struct d1_fixture_authority auth = { 0 };
	struct d1_objkey *object = NULL;
	uint8_t other[12];
	uint32_t i, seqid;

	if (!entry->admission.client_id)
		return true;
	for (i = 0; i < r->store->registered_count; i++)
		if (!memcmp(r->store->registered[i].file_key, entry->file_key,
			    32)) {
			object = &r->store->registered[i].object;
			break;
		}
	if (!object)
		return false;
	d2_stateid(entry->admission.client_id, &seqid, other);
	if (seqid != entry->admission.stateid_seqid ||
	    memcmp(other, entry->admission.stateid_other, sizeof(other)))
		return false;
	memcpy(auth.session, entry->admission.session, 16);
	memcpy(auth.principal.bytes, entry->admission.principal, 16);
	memcpy(auth.issuer.bytes, entry->admission.issuer, 16);
	auth.authority_epoch = entry->admission.authority_epoch;
	auth.fence_sequence = entry->admission.fence_sequence;
	auth.lease_epoch = entry->admission.lease_epoch;
	auth.writer = entry->admission.writer;
	auth.rights = entry->admission.rights;
	if (!d2_admission_find(r->store, entry->admission.client_id)) {
		d1_admission_id id =
			d1_fixture_admit_full(r->store->model, object, &auth);

		if (id.raw != entry->admission.client_id)
			return false;
	}
	return d2_admission_remember(r->store, entry->admission.client_id,
				     object, &auth);
}

static struct d1_objkey *d2_replay_object(struct d2_replay *r,
					  const uint8_t file_key[32])
{
	uint32_t i;

	for (i = 0; i < r->store->registered_count; i++)
		if (!memcmp(r->store->registered[i].file_key, file_key, 32))
			return &r->store->registered[i].object;
	return NULL;
}

static bool d2_replay_receipt(struct d2_replay *r,
			      const struct d1_objkey *object,
			      const struct d2_entry *e)
{
	struct d1_result result = { 0 };
	struct d1_opkey key = { 0 };

	memcpy(key.origin.bytes, e->key.session, D1_UUID_BYTES);
	key.sequence = ((uint64_t)e->key.slot << 32) | e->key.sequence;
	key.ordinal = e->key.compound_ordinal;
	result.key = key;
	result.index_epoch = e->index_generation;
	result.eof = e->extent_high_water;
	result.disposition = D1_COMPLETED;
	result.count = 1;
	result.entries[0].status = e->status;
	result.entries[0].txn_present = e->txn_id != 0;
	result.entries[0].txn.raw = e->txn_id;
	result.entries[0].version_present = e->result_visible_object_id != 0;
	result.entries[0].version.raw = e->result_visible_object_id;
	result.entries[0].postcond_present = e->postcond_present;
	result.entries[0].postcond.raw = e->postcond_id;
	result.entries[0].guard.never_written = e->result_guard_never_written;
	result.entries[0].guard.generation = e->result_guard_generation;
	result.entries[0].guard.writer = e->result_guard_writer;
	result.entries[0].owner.cohort.raw = e->owner_cohort;
	result.entries[0].owner.writer = e->owner_client_id;
	result.entries[0].owner.co_id = e->owner_co_id;
	result.entries[0].stability = e->stability;
	result.entries[0].activated = e->result_activated;
	result.entries[0].phase =
		e->transition <= D2_ROLLED_BACK ? e->transition : 0;
	memcpy(result.entries[0].verifier, e->result_verifier,
	       D1_VERIFIER_BYTES);
	result.entries[0].disposition = D1_COMPLETED;
	return (!e->payload_object_id ||
		d2_work_remember(r->store, e->txn_id,
				 e->result_visible_object_id,
				 e->admission.client_id)) &&
	       d2_receipt_remember(r->store, &object->export_uuid, &key,
				   e->key.request_digest, &result);
}

static void d2_replay_envelope_header(struct d2_replay *r,
				      const struct d2_wal_header *h,
				      const struct d2_entry *e,
				      const struct d1_objkey *object,
				      struct d1_envelope *env)
{
	env->object = *object;
	env->admission = d1_fixture_admission_handle(r->store->model,
						     e->admission.client_id);
	env->incarnation = h->ds_incarnation;
	memcpy(env->key.origin.bytes, e->key.session, D1_UUID_BYTES);
	env->key.sequence = ((uint64_t)e->key.slot << 32) | e->key.sequence;
	env->key.ordinal = e->key.compound_ordinal;
}

static bool d2_replay_lifecycle(struct d2_replay *r,
				const struct d2_wal_header *h,
				const struct d2_entry *e,
				const struct d1_objkey *object)
{
	struct d1_envelope env = { 0 };
	struct d1_result result;
	uint8_t digest[D1_DIGEST_BYTES];

	d2_replay_envelope_header(r, h, e, object, &env);
	env.op = e->transition == D2_FINALIZED ? D1_OP_FINALIZE_BATCH :
						 D1_OP_COMMIT_BATCH;
	env.body.lifecycle.range_begin = e->chunk_index;
	env.body.lifecycle.range_end = e->chunk_index + 1;
	env.body.lifecycle.count = 1;
	env.body.lifecycle.entries[0].index = e->chunk_index;
	env.body.lifecycle.entries[0].owner.cohort.raw = e->owner_cohort;
	env.body.lifecycle.entries[0].owner.writer = e->owner_client_id;
	env.body.lifecycle.entries[0].owner.co_id = e->owner_co_id;
	env.body.lifecycle.entries[0].txn =
		d1_fixture_txn_handle(r->store->model, e->txn_id);
	env.body.lifecycle.entries[0].predecessor_present =
		e->predecessor_present;
	env.body.lifecycle.entries[0].predecessor = d1_fixture_version_handle(
		r->store->model, e->predecessor_object_id);
	memcpy(env.body.lifecycle.prior_verifier, e->result_verifier,
	       D1_VERIFIER_BYTES);
	if (!d2_env_digest(&env, digest) ||
	    memcmp(digest, e->key.request_digest, D1_DIGEST_BYTES))
		return false;
	r->status = d1_store_apply(r->store->model, &env, &result);
	if (r->status != D1_OK || result.count != 1 ||
	    result.entries[0].status != e->status ||
	    result.entries[0].phase != e->transition ||
	    result.entries[0].txn.raw != e->txn_id ||
	    result.entries[0].version.raw != e->result_visible_object_id ||
	    result.entries[0].guard.generation != e->result_guard_generation ||
	    result.entries[0].guard.writer != e->result_guard_writer ||
	    result.entries[0].guard.never_written !=
		    e->result_guard_never_written ||
	    result.eof != e->extent_high_water)
		return false;
	return d2_replay_receipt(r, object, e);
}

static bool d2_replay_rollback(struct d2_replay *r,
			       const struct d2_wal_header *h,
			       const struct d2_entry *e,
			       const struct d1_objkey *object)
{
	struct d1_envelope env = { 0 };
	struct d1_result result;
	const typeof(r->store->work[0]) *work;
	const typeof(r->store->custodies[0]) *custody;
	d1_version_id visible = d1_version_none();
	uint8_t digest[D1_DIGEST_BYTES];

	d2_replay_envelope_header(r, h, e, object, &env);
	env.op = D1_OP_ROLLBACK_BATCH;
	env.body.rollback.range_begin = e->chunk_index;
	env.body.rollback.range_end = e->chunk_index + 1;
	env.body.rollback.count = 1;
	env.body.rollback.entries[0].index = e->chunk_index;
	env.body.rollback.entries[0].owner.cohort.raw = e->owner_cohort;
	env.body.rollback.entries[0].owner.writer = e->owner_client_id;
	env.body.rollback.entries[0].owner.co_id = e->owner_co_id;
	env.body.rollback.entries[0].txn =
		d1_fixture_txn_handle(r->store->model, e->txn_id);
	env.body.rollback.entries[0].visible_present = d1_store_visible(
		r->store->model, object, e->chunk_index, &visible);
	env.body.rollback.entries[0].visible = visible;
	env.body.rollback.entries[0].predecessor_present =
		e->predecessor_present;
	env.body.rollback.entries[0].predecessor = d1_fixture_version_handle(
		r->store->model, e->predecessor_object_id);
	work = d2_work_by_txn(r->store, e->txn_id);
	custody = work ? d2_custody_by_version(r->store, work->version_id) :
			 NULL;
	if (custody) {
		env.body.rollback.entries[0].custody_present = true;
		env.body.rollback.entries[0].custody =
			d1_fixture_custody_handle(r->store->model,
						  custody->custody_id);
	}
	if (!d2_env_digest(&env, digest) ||
	    memcmp(digest, e->key.request_digest, D1_DIGEST_BYTES))
		return false;
	r->status = d1_store_apply(r->store->model, &env, &result);
	if (r->status != D1_OK || result.count != 1 ||
	    result.entries[0].status != e->status ||
	    result.entries[0].phase != D2_ROLLED_BACK ||
	    result.entries[0].txn.raw != e->txn_id ||
	    result.entries[0].guard.generation != e->result_guard_generation ||
	    result.entries[0].guard.writer != e->result_guard_writer ||
	    result.entries[0].guard.never_written !=
		    e->result_guard_never_written ||
	    result.eof != e->extent_high_water)
		return false;
	return d2_replay_receipt(r, object, e);
}

static bool d2_replay_entry(struct d2_replay *r, const struct d2_wal_header *h,
			    const struct d2_entry *e)
{
	struct d2_payload_object object;
	struct d1_envelope env = { 0 };
	struct d1_result result;
	struct d1_objkey *key;
	struct d1_guard guard = { .never_written = true };
	uint8_t *allocation = NULL, *scratch = NULL;
	uint8_t digest[32];
	bool ok = false;

	if (e->transition != D2_PREPARED && e->transition != D2_FINALIZED &&
	    e->transition != D2_COMMITTED && e->transition != D2_ROLLED_BACK &&
	    e->transition != D2_REFUSED && e->transition != D2_ABORTED)
		return false;
	key = d2_replay_object(r, e->file_key);
	if (!key)
		return false;
	if (e->transition == D2_REFUSED || e->transition == D2_ABORTED)
		return !e->payload_object_id && d2_replay_receipt(r, key, e);
	if (!e->payload_object_id)
		return e->transition == D2_ROLLED_BACK ?
			       d2_replay_rollback(r, h, e, key) :
			       (e->transition == D2_FINALIZED ||
				e->transition == D2_COMMITTED) &&
				       d2_replay_lifecycle(r, h, e, key);
	if (e->status != D1_OK || !e->payload_object_id ||
	    e->payload_object_id <= r->highest_payload_id)
		return false;
	r->status = d2_files_payload_read(r->files, e->payload_object_offset,
					  &object, &allocation);
	if (r->status != D1_OK ||
	    object.payload_object_id != e->payload_object_id ||
	    object.content_len != e->payload_content_len)
		goto out;
	d2_replay_envelope_header(r, h, e, key, &env);
	env.op = D1_OP_WRITE_BATCH;
	env.body.write.count = 1;
	env.body.write.stability = e->stability;
	env.body.write.activate = e->result_activated;
	env.body.write.entries[0].index = e->chunk_index;
	env.body.write.entries[0].owner.cohort.raw = e->owner_cohort;
	env.body.write.entries[0].owner.writer = e->owner_client_id;
	env.body.write.entries[0].owner.co_id = e->owner_co_id;
	(void)d1_store_guard(r->store->model, key, e->chunk_index, &guard);
	env.body.write.entries[0].guard_check = true;
	env.body.write.entries[0].expected = guard;
	env.body.write.entries[0].payload = object.content;
	env.body.write.entries[0].payload_len = object.content_len;
	env.body.write.entries[0].checksum.alg = object.content_alg;
	env.body.write.entries[0].checksum.len = object.content_ck_len;
	memcpy(env.body.write.entries[0].checksum.digest, object.content_ck,
	       object.content_ck_len);
	scratch = malloc(D1_ENVELOPE_MAX);
	if (!scratch)
		goto out;
	if (!d1_envelope_digest(&env, scratch, D1_ENVELOPE_MAX, digest))
		goto out;
	if (memcmp(digest, e->key.request_digest, sizeof(digest))) {
		env.body.write.activate = !env.body.write.activate;
		if (!d1_envelope_digest(&env, scratch, D1_ENVELOPE_MAX,
					digest) ||
		    memcmp(digest, e->key.request_digest, sizeof(digest)))
			goto out;
	}
	r->status = d1_store_apply(r->store->model, &env, &result);
	if (r->status != D1_OK || result.count != 1 ||
	    result.entries[0].status != e->status ||
	    result.entries[0].txn.raw != e->txn_id ||
	    result.entries[0].version.raw != e->result_visible_object_id ||
	    result.entries[0].guard.generation != e->result_guard_generation ||
	    result.entries[0].guard.writer != e->result_guard_writer ||
	    result.entries[0].guard.never_written !=
		    e->result_guard_never_written ||
	    result.eof != e->extent_high_water)
		goto out;
	r->highest_payload_id = object.payload_object_id;
	ok = d2_replay_receipt(r, key, e);
out:
	free(scratch);
	free(allocation);
	return ok;
}

static uint32_t d2_model_next_incarnation(struct d2_store *s);

static bool d2_replay_start(struct d2_replay *r, const struct d2_wal_header *h)
{
	if (!r->last_incarnation) {
		r->last_incarnation = h->ds_incarnation;
		return h->ds_incarnation ==
		       d1_store_incarnation(r->store->model);
	}
	if (h->ds_incarnation != r->last_incarnation + 1 ||
	    d1_store_incarnation(r->store->model) != r->last_incarnation)
		return false;
	r->status = d2_model_next_incarnation(r->store);
	if (r->status != D1_OK ||
	    d1_store_incarnation(r->store->model) != h->ds_incarnation) {
		if (r->status == D1_OK)
			r->status = D1_IO;
		return false;
	}
	r->last_incarnation = h->ds_incarnation;
	return true;
}

static uint32_t d2_model_next_incarnation(struct d2_store *s)
{
	uint8_t *journal = NULL;
	struct d1_store *next = NULL;
	size_t journal_len = 0;
	uint32_t status;

	status = d1_store_journal_snapshot(s->model, &journal, &journal_len);
	if (status == D1_OK) {
		next = d1_store_open(&s->uuid, s->chunk_bytes,
				     s->max_file_bytes);
		status = next ? d1_store_reopen(next, journal, journal_len) :
				D1_NOSPC;
	}
	free(journal);
	if (status != D1_OK) {
		d1_store_free(next);
		return status;
	}
	d1_store_free(s->model);
	s->model = next;
	return status;
}

static bool d2_replay_record(const struct d2_wal_header *h,
			     const uint8_t *record, void *arg)
{
	struct d2_replay *r = arg;
	struct d2_control control;
	struct d2_entry e;

	if (h->family == D2_REC_START)
		return d2_replay_start(r, h);
	if (h->family == D2_REC_CONTROL) {
		if (!d2_control_decode(record, h->total_bytes, h, &control))
			return false;
		return d2_replay_registration(r, &control) &&
		       d2_replay_liveness(r, &control) &&
		       d2_replay_custody(r, &control);
	}
	if (h->family != D2_REC_ENTRY)
		return true;
	if (!d2_entry_decode(record, h->total_bytes, h, &e))
		return false;
	if (!d2_replay_admission(r, &e))
		return false;
	return d2_replay_entry(r, h, &e);
}

static void d2_store_key(struct d2_store *s, struct d2_key_block *key)
{
	uint64_t incarnation = d2_files_super(s->files)->ds_incarnation;
	uint32_t sequence = (uint32_t)s->synthetic_sequence++;

	memset(key, 0, sizeof(*key));
	key->sequence = sequence;
	key->operation_key[0] = (uint8_t)(incarnation >> 56);
	key->operation_key[1] = (uint8_t)(incarnation >> 48);
	key->operation_key[2] = (uint8_t)(incarnation >> 40);
	key->operation_key[3] = (uint8_t)(incarnation >> 32);
	key->operation_key[4] = (uint8_t)(incarnation >> 24);
	key->operation_key[5] = (uint8_t)(incarnation >> 16);
	key->operation_key[6] = (uint8_t)(incarnation >> 8);
	key->operation_key[7] = (uint8_t)incarnation;
	key->operation_key[12] = (uint8_t)(sequence >> 24);
	key->operation_key[13] = (uint8_t)(sequence >> 16);
	key->operation_key[14] = (uint8_t)(sequence >> 8);
	key->operation_key[15] = (uint8_t)sequence;
}

static uint32_t d2_register_file(struct d2_store *s,
				 const struct d1_objkey *object,
				 uint8_t file_key[32])
{
	struct d2_wal_header h = { 0 };
	struct d2_control control = { 0 };
	struct d1_cursor cursor;
	uint8_t handle[32], record[396], zero[D2_MAX_HANDLE_BYTES] = { 0 };
	size_t written;
	uint32_t i, status;

	memcpy(handle, object->export_uuid.bytes, 16);
	memcpy(handle + 16, object->object_uuid.bytes, 16);
	d2_file_key(handle, sizeof(handle), file_key);
	for (i = 0; i < s->registered_count; i++)
		if (d2_same_object(&s->registered[i].object, object)) {
			if (memcmp(s->registered[i].file_key, file_key, 32))
				return D1_IO;
			return D1_OK;
		}
	if (s->registered_count == D1_MAX_OBJECTS)
		return D1_NOSPC;
	h.family = D2_REC_CONTROL;
	memcpy(h.store_uuid, s->uuid.bytes, 16);
	memcpy(h.wal_uuid, d2_files_super(s->files)->wal_uuid, 16);
	h.lsn = d2_files_next_lsn(s->files);
	h.ds_incarnation = d2_files_super(s->files)->ds_incarnation;
	control.subtype = D2_CTL_FILE_REGISTER;
	control.transition = D2_COMMITTED;
	control.status = D1_OK;
	d2_store_key(s, &control.key);
	control.body_len = 184;
	d1_enc_init(&cursor, control.body, control.body_len);
	d1_enc_raw(&cursor, file_key, 32);
	d1_enc_u16(&cursor, 0);
	d1_enc_u16(&cursor, sizeof(handle));
	d1_enc_raw(&cursor, handle, sizeof(handle));
	d1_enc_raw(&cursor, zero, D2_MAX_HANDLE_BYTES - sizeof(handle));
	d1_enc_u32(&cursor, s->chunk_bytes);
	d1_enc_u64(&cursor, s->max_file_bytes / s->chunk_bytes);
	d1_enc_u64(&cursor, 0);
	if (!d1_cursor_ok(&cursor) ||
	    !d2_control_encode(&h, &control, record, sizeof(record), &written))
		return D1_INVALID;
	status = d2_files_wal_append(s->files, record, written);
	if (status != D1_OK)
		return status;
	i = s->registered_count++;
	s->registered[i].object = *object;
	memcpy(s->registered[i].file_key, file_key, 32);
	return D1_OK;
}

static struct d2_store *d2_store_alloc(const uint8_t uuid[16],
				       uint32_t chunk_bytes,
				       uint64_t max_file_bytes)
{
	struct d2_store *s = calloc(1, sizeof(*s));

	if (!s)
		return NULL;
	memcpy(s->uuid.bytes, uuid, 16);
	s->chunk_bytes = chunk_bytes;
	s->max_file_bytes = max_file_bytes;
	s->next_payload_seq = 1;
	s->synthetic_sequence = 1;
	if (pthread_mutex_init(&s->lock, NULL)) {
		free(s);
		return NULL;
	}
	s->model = d1_store_open(&s->uuid, chunk_bytes, max_file_bytes);
	if (!s->model) {
		pthread_mutex_destroy(&s->lock);
		free(s);
		return NULL;
	}
	return s;
}

static void d2_store_free(struct d2_store *s, bool crash)
{
	if (!s)
		return;
	if (s->files) {
		if (crash)
			d2_files_crash(s->files);
		else
			d2_files_close(s->files);
	}
	d1_store_free(s->model);
	pthread_mutex_destroy(&s->lock);
	free(s);
}

static bool d2_geometry(uint32_t chunk_bytes, uint64_t max_file_bytes)
{
	return chunk_bytes >= 4096 && chunk_bytes <= 1024u * 1024u &&
	       chunk_bytes % 4096 == 0 && max_file_bytes != 0 &&
	       max_file_bytes % chunk_bytes == 0 &&
	       max_file_bytes / chunk_bytes <= UINT32_MAX;
}

uint32_t d2_store_provision(int dirfd, const struct d2_store_config *config,
			    struct d2_binding *binding, struct d2_store **out)
{
	struct d2_files *files = NULL;
	struct d2_store *s;
	uint32_t status;

	if (!config || !binding || !out ||
	    !d2_geometry(config->chunk_bytes, config->max_file_bytes))
		return D1_INVALID;
	*out = NULL;
	status = d2_files_provision(dirfd, &config->files, binding, &files);
	if (status != D1_OK)
		return status;
	s = d2_store_alloc(binding->store_uuid, config->chunk_bytes,
			   config->max_file_bytes);
	if (!s) {
		d2_files_close(files);
		return D1_NOSPC;
	}
	s->files = files;
	status = d1_store_journal_enable(s->model);
	if (status != D1_OK) {
		d2_store_free(s, false);
		return status;
	}
	*out = s;
	return D1_OK;
}

uint32_t d2_store_rebind(int dirfd, const struct d2_store_rebind *config,
			 struct d2_binding *binding, struct d2_store **out)
{
	struct d2_scan_result scan;
	const struct d2_scan_result *first;
	struct d2_replay replay = { .status = D1_OK };
	struct d2_files *files = NULL;
	struct d2_store *s;
	uint32_t decision, status;

	if (!config || !binding || !out ||
	    !d2_geometry(config->chunk_bytes, config->max_file_bytes))
		return D1_INVALID;
	*out = NULL;
	status = d2_files_rebind(dirfd, &config->files, binding, &files);
	if (status != D1_OK)
		return status;
	s = d2_store_alloc(binding->store_uuid, config->chunk_bytes,
			   config->max_file_bytes);
	if (!s) {
		d2_files_close(files);
		return D1_NOSPC;
	}
	s->files = files;
	replay.files = files;
	replay.store = s;
	status = d1_store_journal_enable(s->model);
	if (status != D1_OK)
		goto fail;
	status = d2_files_scan(files, d2_replay_record, &replay, &scan, false);
	if (status != D1_OK || replay.status != D1_OK)
		goto fail;
	s->next_payload_seq =
		(replay.highest_payload_id & ((UINT64_C(1) << 40) - 1)) + 1;
	status = d2_model_next_incarnation(s);
	if (status != D1_OK)
		goto fail;
	first = d2_files_last_scan(files);
	decision = first && first->tail_class != D2_TAIL_CLEAN ?
			   D2_RD_TRUNCATED_TAIL :
			   D2_RD_CLEAN_SCAN;
	if (first && first->tail_class != D2_TAIL_CLEAN &&
	    first->tail_class != D2_TAIL_ZERO)
		decision |= D2_RD_TRUNCATED_NONZERO_TAIL;
	status = d2_files_start(files, decision,
				first ? first->truncated_bytes : 0,
				d2_files_payload_cursor(files), 0, 0, 0);
	if (status != D1_OK)
		goto fail;
	*out = s;
	return D1_OK;

fail:
	d2_store_free(s, false);
	return status == D1_OK ? D1_IO : status;
}

static void d2_key_from_env(const struct d1_envelope *env,
			    struct d2_key_block *key)
{
	memcpy(key->session, env->key.origin.bytes, 16);
	key->slot = (uint32_t)(env->key.sequence >> 32);
	key->sequence = (uint32_t)env->key.sequence;
	key->compound_ordinal = env->key.ordinal;
	d2_operation_key(key->session, key->slot, key->sequence,
			 key->compound_ordinal, key->operation_key);
}

static uint32_t d2_transition(const struct d1_envelope *env,
			      const struct d1_result *result)
{
	if (result->entries[0].status != D1_OK)
		return D2_REFUSED;
	if (env->op == D1_OP_UNLOCK)
		return D2_UNLOCKED;
	if (result->entries[0].phase >= D2_ADMITTED &&
	    result->entries[0].phase <= D2_ROLLED_BACK)
		return result->entries[0].phase;
	return D2_COMMITTED;
}

static uint64_t d2_entry_index(const struct d1_envelope *env)
{
	switch (env->op) {
	case D1_OP_WRITE_BATCH:
		return env->body.write.entries[0].index;
	case D1_OP_FINALIZE_BATCH:
	case D1_OP_COMMIT_BATCH:
		return env->body.lifecycle.entries[0].index;
	case D1_OP_ROLLBACK_BATCH:
		return env->body.rollback.entries[0].index;
	default:
		return 0;
	}
}

static uint32_t d2_persist_entry(struct d2_store *s,
				 const struct d1_envelope *env,
				 const struct d1_result *result,
				 uint64_t prior_eof)
{
	struct d2_payload_object object = { 0 };
	struct d2_wal_header h = { 0 };
	struct d2_entry entry = { 0 };
	uint8_t *scratch = NULL;
	uint8_t record[D2_ENTRY_RECORD_BYTES];
	uint8_t handle[32];
	uint64_t eof, payload_id = 0, offset = 0;
	uint32_t status;
	const struct d1_write_entry *write =
		env->op == D1_OP_WRITE_BATCH ? &env->body.write.entries[0] :
					       NULL;
	const struct d1_lifecycle_entry *life =
		env->op == D1_OP_FINALIZE_BATCH ||
				env->op == D1_OP_COMMIT_BATCH ?
			&env->body.lifecycle.entries[0] :
			NULL;
	const struct d1_rollback_entry *rollback =
		env->op == D1_OP_ROLLBACK_BATCH ?
			&env->body.rollback.entries[0] :
			NULL;
	bool has_payload = write && result->entries[0].status == D1_OK;

	if (has_payload && s->next_payload_seq >= (UINT64_C(1) << 40))
		return D1_NOSPC;
	if (has_payload) {
		payload_id = (d2_files_super(s->files)->ds_incarnation << 40) |
			     s->next_payload_seq;
		memcpy(object.store_uuid, s->uuid.bytes, 16);
		object.payload_object_id = payload_id;
		object.content = write->payload;
		object.content_len = write->payload_len;
		object.content_alg = write->checksum.alg;
		object.content_ck_len = write->checksum.len;
		memcpy(object.content_ck, write->checksum.digest,
		       write->checksum.len);
		status = d2_files_payload_append(s->files, &object, &offset);
		if (status != D1_OK)
			return status;
	}
	h.family = D2_REC_ENTRY;
	memcpy(h.store_uuid, s->uuid.bytes, 16);
	memcpy(h.wal_uuid, d2_files_super(s->files)->wal_uuid, 16);
	h.lsn = d2_files_next_lsn(s->files);
	h.ds_incarnation = d2_files_super(s->files)->ds_incarnation;
	entry.transition = d2_transition(env, result);
	entry.status = result->entries[0].status;
	entry.disposition = D1_COMPLETED;
	entry.stability = result->entries[0].stability ?
				  result->entries[0].stability :
				  D1_FILE_SYNC;
	entry.batch_count = 1;
	memcpy(handle, env->object.export_uuid.bytes, 16);
	memcpy(handle + 16, env->object.object_uuid.bytes, 16);
	d2_file_key(handle, sizeof(handle), entry.file_key);
	entry.chunk_index = d2_entry_index(env);
	d2_key_from_env(env, &entry.key);
	d2_admission_encode(s, env->admission.raw, &entry.admission);
	scratch = malloc(D1_ENVELOPE_MAX);
	if (!scratch)
		return D1_NOSPC;
	if (!d1_envelope_digest(env, scratch, D1_ENVELOPE_MAX,
				entry.key.request_digest)) {
		free(scratch);
		return D1_INVALID;
	}
	free(scratch);
	entry.txn_id = life	? life->txn.raw :
		       rollback ? rollback->txn.raw :
				  result->entries[0].txn.raw;
	entry.owner_cohort = result->entries[0].owner.cohort.raw;
	entry.owner_client_id = result->entries[0].owner.writer;
	entry.owner_co_id = result->entries[0].owner.co_id;
	entry.generation = result->entries[0].guard.generation;
	if (life) {
		entry.predecessor_present = life->predecessor_present;
		entry.predecessor_object_id = life->predecessor.raw;
	} else if (rollback) {
		entry.predecessor_present = rollback->predecessor_present;
		entry.predecessor_object_id = rollback->predecessor.raw;
	}
	entry.postcond_present = result->entries[0].postcond_present;
	entry.postcond_id = result->entries[0].postcond.raw;
	entry.result_guard_generation = result->entries[0].guard.generation;
	entry.result_guard_writer = result->entries[0].guard.writer;
	entry.result_guard_never_written =
		result->entries[0].guard.never_written;
	entry.result_activated = result->entries[0].activated;
	entry.result_visible_object_id = result->entries[0].version.raw;
	entry.payload_object_id = payload_id;
	entry.payload_object_offset = offset;
	entry.payload_content_len = has_payload ? write->payload_len : 0;
	eof = d1_store_eof(s->model, &env->object);
	entry.extent_high_water = eof;
	entry.extent_highest_index = eof ? (eof - 1) / s->chunk_bytes : 0;
	entry.extent_kind = eof > prior_eof ? D2_EXTENT_EXTEND :
			    eof < prior_eof ? D2_EXTENT_SHRINK :
					      D2_EXTENT_UNCHANGED;
	entry.index_generation = result->index_epoch;
	memcpy(entry.result_verifier, result->entries[0].verifier, 8);
	if (has_payload) {
		entry.result_effective_len = write->payload_len;
		entry.result_ck_alg = write->checksum.alg;
		entry.result_ck_len = write->checksum.len;
		memcpy(entry.result_ck, write->checksum.digest,
		       write->checksum.len);
	}
	if (!d2_entry_encode(&h, &entry, record)) {
		return D1_INVALID;
	}
	status = d2_files_wal_append(s->files, record, sizeof(record));
	if (status == D1_OK && has_payload)
		s->next_payload_seq++;
	return status;
}

uint32_t d2_store_apply(struct d2_store *s, const struct d1_envelope *env,
			struct d1_result *result)
{
	uint8_t *before = NULL, *after = NULL;
	uint8_t digest[D1_DIGEST_BYTES];
	uint8_t file_key[32];
	size_t before_len = 0, after_len = 0;
	uint64_t prior_eof;
	uint32_t status, persist;

	if (!s || !env || !result)
		return D1_INVALID;
	if (!d1_envelope_validate(env))
		return D1_INVALID;
	if (!d2_env_digest(env, digest))
		return D1_NOSPC;
	if ((env->op != D1_OP_WRITE_BATCH && env->op != D1_OP_FINALIZE_BATCH &&
	     env->op != D1_OP_COMMIT_BATCH &&
	     env->op != D1_OP_ROLLBACK_BATCH) ||
	    d1_envelope_member_count(env) != 1 ||
	    (env->op == D1_OP_ROLLBACK_BATCH &&
	     (env->body.rollback.range_begin !=
		      env->body.rollback.entries[0].index ||
	      env->body.rollback.range_end !=
		      env->body.rollback.entries[0].index + 1)) ||
	    ((env->op == D1_OP_FINALIZE_BATCH ||
	      env->op == D1_OP_COMMIT_BATCH) &&
	     (env->body.lifecycle.range_begin !=
		      env->body.lifecycle.entries[0].index ||
	      env->body.lifecycle.range_end !=
		      env->body.lifecycle.entries[0].index + 1))) {
		memset(result, 0, sizeof(*result));
		result->key = env->key;
		result->disposition = D1_UNRECORDED;
		result->count = 1;
		result->entries[0].status = D1_UNSUPPORTED;
		result->entries[0].disposition = D1_UNRECORDED;
		return D1_UNSUPPORTED;
	}
	pthread_mutex_lock(&s->lock);
	if (s->fenced) {
		pthread_mutex_unlock(&s->lock);
		return D1_IO;
	}
	if (!memcmp(env->key.origin.bytes, (uint8_t[16]){ 0 }, 16)) {
		pthread_mutex_unlock(&s->lock);
		return D1_INVALID;
	}
	if (env->op == D1_OP_ROLLBACK_BATCH && !d2_rollback_supported(s, env)) {
		memset(result, 0, sizeof(*result));
		result->key = env->key;
		result->disposition = D1_UNRECORDED;
		result->count = 1;
		result->entries[0].status = D1_UNSUPPORTED;
		result->entries[0].disposition = D1_UNRECORDED;
		pthread_mutex_unlock(&s->lock);
		return D1_UNSUPPORTED;
	}
	status = d2_register_file(s, &env->object, file_key);
	if (status != D1_OK) {
		if (status != D1_NOSPC)
			s->fenced = true;
		pthread_mutex_unlock(&s->lock);
		return status;
	}
	if (d2_admission_matches(s, env->admission.raw, &env->object) &&
	    d2_receipt_lookup(s, env, digest, result)) {
		pthread_mutex_unlock(&s->lock);
		return D1_OK;
	}
	prior_eof = d1_store_eof(s->model, &env->object);
	status = d1_store_journal_snapshot(s->model, &before, &before_len);
	if (status != D1_OK)
		goto out;
	status = d1_store_apply(s->model, env, result);
	if (d1_store_journal_snapshot(s->model, &after, &after_len) != D1_OK) {
		status = D1_NOSPC;
		goto out;
	}
	if (before_len == after_len &&
	    (!before_len || !memcmp(before, after, before_len)))
		goto out;
	persist = d2_persist_entry(s, env, result, prior_eof);
	if (persist != D1_OK) {
		s->fenced = true;
		status = persist;
		memset(result, 0, sizeof(*result));
		result->disposition = D1_UNRECORDED;
	} else if (!d2_receipt_remember(s, &env->object.export_uuid, &env->key,
					digest, result)) {
		s->fenced = true;
		status = D1_IO;
	} else if (env->op == D1_OP_WRITE_BATCH &&
		   !d2_work_remember(s, result->entries[0].txn.raw,
				     result->entries[0].version.raw,
				     env->admission.raw)) {
		s->fenced = true;
		status = D1_IO;
	}
out:
	free(before);
	free(after);
	pthread_mutex_unlock(&s->lock);
	return status;
}

d1_admission_id d2_store_admit_full(struct d2_store *s,
				    const struct d1_objkey *object,
				    const struct d1_fixture_authority *auth)
{
	d1_admission_id id = d1_admission_none();
	uint8_t file_key[32];
	uint32_t status;

	if (!s || !object || !auth)
		return id;
	pthread_mutex_lock(&s->lock);
	status = s->fenced ? D1_IO : d2_register_file(s, object, file_key);
	if (status != D1_OK && status != D1_NOSPC)
		s->fenced = true;
	if (!s->fenced)
		if (status == D1_OK)
			id = d1_fixture_admit_full(s->model, object, auth);
	if (d1_admission_live(id) &&
	    !d2_admission_remember(s, id.raw, object, auth)) {
		s->fenced = true;
		id = d1_admission_none();
	}
	pthread_mutex_unlock(&s->lock);
	return id;
}

d1_admission_id d2_store_admit(struct d2_store *s,
			       const struct d1_objkey *object, uint32_t writer,
			       uint32_t rights)
{
	struct d1_fixture_authority auth = { 0 };
	unsigned int i;

	for (i = 0; i < D1_UUID_BYTES; i++) {
		auth.session[i] = (uint8_t)(0x80u + writer + i);
		auth.issuer.bytes[i] = (uint8_t)(0xf0u + i);
		auth.principal.bytes[i] = (uint8_t)(writer + i);
	}
	auth.writer = writer;
	auth.rights = rights;
	return d2_store_admit_full(s, object, &auth);
}

static uint32_t d2_liveness_control(struct d2_store *s,
				    d1_admission_id admission, uint32_t subtype)
{
	const struct d1_fixture_authority *auth =
		d2_admission_find(s, admission.raw);
	struct d2_wal_header h = { 0 };
	struct d2_control control = { 0 };
	struct d1_cursor cursor;
	uint8_t record[320], other[12];
	size_t written;
	uint32_t seqid;

	if (!auth || !memcmp(auth->session, (uint8_t[16]){ 0 }, 16))
		return D1_INVALID;
	h.family = D2_REC_CONTROL;
	memcpy(h.store_uuid, s->uuid.bytes, 16);
	memcpy(h.wal_uuid, d2_files_super(s->files)->wal_uuid, 16);
	h.lsn = d2_files_next_lsn(s->files);
	h.ds_incarnation = d2_files_super(s->files)->ds_incarnation;
	control.subtype = subtype;
	control.transition = D2_COMMITTED;
	control.status = D1_OK;
	memcpy(control.key.session, auth->session, 16);
	if (h.lsn > UINT32_MAX)
		return D1_NOSPC;
	control.key.sequence = (uint32_t)h.lsn;
	d2_operation_key(control.key.session, control.key.slot,
			 control.key.sequence, control.key.compound_ordinal,
			 control.key.operation_key);
	memcpy(control.admission_issuer, auth->issuer.bytes, 16);
	control.admission_authority_epoch = auth->authority_epoch;
	control.admission_client_id = admission.raw;
	if (subtype == D2_CTL_REVOKE_STATEID) {
		control.body_len = 60;
		d2_stateid(admission.raw, &seqid, other);
		d1_enc_init(&cursor, control.body, control.body_len);
		d1_enc_u32(&cursor, seqid);
		d1_enc_raw(&cursor, other, sizeof(other));
		d1_enc_u32(&cursor, 1);
		d1_enc_raw(&cursor, auth->session, 16);
		d1_enc_u64(&cursor, admission.raw);
		d1_enc_raw(&cursor, auth->principal.bytes, 16);
	} else {
		control.body_len = 52;
		d1_enc_init(&cursor, control.body, control.body_len);
		d1_enc_raw(&cursor, auth->session, 16);
		d1_enc_u64(&cursor, admission.raw);
		d1_enc_raw(&cursor, auth->principal.bytes, 16);
		d1_enc_u64(&cursor, auth->lease_epoch);
		d1_enc_u32(&cursor, 1);
	}
	if (!d1_cursor_ok(&cursor))
		return D1_INVALID;
	d2_hash_domain("FFV2-D2B-CONTROL-v1", control.body, control.body_len,
		       control.key.request_digest);
	if (!d2_control_encode(&h, &control, record, sizeof(record), &written))
		return D1_INVALID;
	return d2_files_wal_append(s->files, record, written);
}

static uint32_t d2_custody_control(struct d2_store *s, d1_custody_id custody,
				   d1_version_id version, uint64_t admission_id)
{
	const struct d1_fixture_authority *auth =
		d2_admission_find(s, admission_id);
	struct d2_wal_header h = { 0 };
	struct d2_control control = { 0 };
	struct d1_cursor cursor;
	uint8_t record[256];
	size_t written;

	if (!auth || !memcmp(auth->session, (uint8_t[16]){ 0 }, 16))
		return D1_INVALID;
	h.family = D2_REC_CONTROL;
	memcpy(h.store_uuid, s->uuid.bytes, 16);
	memcpy(h.wal_uuid, d2_files_super(s->files)->wal_uuid, 16);
	h.lsn = d2_files_next_lsn(s->files);
	h.ds_incarnation = d2_files_super(s->files)->ds_incarnation;
	if (h.lsn > UINT32_MAX)
		return D1_NOSPC;
	control.subtype = D2_CTL_CUSTODY;
	control.transition = D2_COMMITTED;
	control.status = D1_OK;
	memcpy(control.key.session, auth->session, 16);
	control.key.sequence = (uint32_t)h.lsn;
	d2_operation_key(control.key.session, 0, control.key.sequence, 0,
			 control.key.operation_key);
	memcpy(control.admission_issuer, auth->issuer.bytes, 16);
	control.admission_authority_epoch = auth->authority_epoch;
	control.admission_client_id = admission_id;
	control.body_len = 36;
	d1_enc_init(&cursor, control.body, control.body_len);
	d1_enc_u64(&cursor, custody.raw);
	d1_enc_u64(&cursor, version.raw);
	d1_enc_u32(&cursor, 1);
	d1_enc_raw(&cursor, auth->principal.bytes, 16);
	if (!d1_cursor_ok(&cursor))
		return D1_INVALID;
	d2_hash_domain("FFV2-D2B-CONTROL-v1", control.body, control.body_len,
		       control.key.request_digest);
	if (!d2_control_encode(&h, &control, record, sizeof(record), &written))
		return D1_INVALID;
	return d2_files_wal_append(s->files, record, written);
}

void d2_store_revoke(struct d2_store *s, d1_admission_id admission)
{
	if (!s)
		return;
	pthread_mutex_lock(&s->lock);
	if (!s->fenced) {
		d1_fixture_revoke(s->model, admission);
		if (d2_liveness_control(s, admission, D2_CTL_REVOKE_STATEID) !=
		    D1_OK)
			s->fenced = true;
	}
	pthread_mutex_unlock(&s->lock);
}

void d2_store_expire(struct d2_store *s, d1_admission_id admission)
{
	if (!s)
		return;
	pthread_mutex_lock(&s->lock);
	if (!s->fenced) {
		d1_fixture_expire(s->model, admission);
		if (d2_liveness_control(s, admission, D2_CTL_LEASE_EXPIRE) !=
		    D1_OK)
			s->fenced = true;
	}
	pthread_mutex_unlock(&s->lock);
}

d1_custody_id d2_store_custody(struct d2_store *s, d1_version_id version)
{
	d1_custody_id id = d1_custody_none();
	const typeof(s->work[0]) *work;

	if (!s)
		return id;
	pthread_mutex_lock(&s->lock);
	work = d2_work_by_version(s, version.raw);
	if (!s->fenced && work)
		id = d1_fixture_custody(s->model, version);
	if (d1_custody_live(id) &&
	    (d2_custody_control(s, id, version, work->admission_id) != D1_OK ||
	     !d2_custody_remember(s, id.raw, version.raw,
				  work->admission_id))) {
		s->fenced = true;
		id = d1_custody_none();
	}
	pthread_mutex_unlock(&s->lock);
	return id;
}

void d2_store_certificate(struct d2_store *s,
			  const uint8_t certificate[D1_CERTIFICATE_BYTES])
{
	(void)certificate;
	if (!s)
		return;
	pthread_mutex_lock(&s->lock);
	if (!s->fenced)
		s->fenced = true;
	pthread_mutex_unlock(&s->lock);
}

d1_admission_id d2_store_admission_handle(struct d2_store *s, uint64_t raw)
{
	return s ? d1_fixture_admission_handle(s->model, raw) :
		   d1_admission_none();
}

d1_txn_id d2_store_txn_handle(struct d2_store *s, uint64_t raw)
{
	return s ? d1_fixture_txn_handle(s->model, raw) : d1_txn_none();
}

d1_version_id d2_store_version_handle(struct d2_store *s, uint64_t raw)
{
	return s ? d1_fixture_version_handle(s->model, raw) : d1_version_none();
}

void d2_store_verifier(struct d2_store *s, uint8_t verifier[8])
{
	if (!verifier)
		return;
	if (!s || s->fenced)
		memset(verifier, 0, 8);
	else
		d1_store_verifier(s->model, verifier);
}

uint64_t d2_store_incarnation(struct d2_store *s)
{
	return s && !s->fenced ? d1_store_incarnation(s->model) : 0;
}

void d2_store_fail_next_index(struct d2_store *s)
{
	if (s && !s->fenced)
		d1_fixture_fail_next_index(s->model);
}

void d2_store_set_io_hook(struct d2_store *s, d2_io_hook_fn hook, void *arg)
{
	if (s)
		d2_files_set_io_hook(s->files, hook, arg);
}

bool d2_store_visible(struct d2_store *s, const struct d1_objkey *object,
		      uint64_t index, d1_version_id *version)
{
	return s && !s->fenced &&
	       d1_store_visible(s->model, object, index, version);
}

bool d2_store_guard(struct d2_store *s, const struct d1_objkey *object,
		    uint64_t index, struct d1_guard *guard)
{
	return s && !s->fenced &&
	       d1_store_guard(s->model, object, index, guard);
}

uint64_t d2_store_eof(struct d2_store *s, const struct d1_objkey *object)
{
	return s && !s->fenced ? d1_store_eof(s->model, object) : 0;
}

uint64_t d2_store_wal_bytes(struct d2_store *s)
{
	return s && !s->fenced ? d2_files_wal_cursor(s->files) : 0;
}

uint32_t d2_store_view_open(struct d2_store *s, const struct d1_objkey *object,
			    d1_admission_id admission,
			    const struct d1_selection_spec *selection,
			    uint64_t begin, uint64_t end, struct d1_view **view)
{
	if (!s || s->fenced)
		return D1_IO;
	return d1_view_open(s->model, object, admission, selection, begin, end,
			    view);
}

void d2_store_crash(struct d2_store *s)
{
	d2_store_free(s, true);
}

uint32_t d2_store_close(struct d2_store *s)
{
	uint32_t status;

	if (!s)
		return D1_OK;
	status = d1_store_close(s->model);
	if (status != D1_OK)
		return status;
	d2_store_free(s, false);
	return D1_OK;
}
