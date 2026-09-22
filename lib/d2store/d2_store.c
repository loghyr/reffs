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
	bool fenced;
};

struct d2_replay {
	struct d2_files *files;
	struct d2_store *store;
	uint8_t *snapshot;
	size_t snapshot_len;
	uint64_t highest_payload_id;
	uint32_t status;
};

static bool d2_same_object(const struct d1_objkey *a, const struct d1_objkey *b)
{
	return !memcmp(a, b, sizeof(*a));
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
	return d2_admission_remember(r->store, entry->admission.client_id,
				     object, &auth);
}

static bool d2_replay_record(const struct d2_wal_header *h,
			     const uint8_t *record, void *arg)
{
	struct d2_replay *r = arg;
	struct d2_control control;
	struct d2_entry e;
	struct d2_payload_object object;
	uint8_t *allocation = NULL;

	if (h->family == D2_REC_CONTROL) {
		if (!d2_control_decode(record, h->total_bytes, h, &control))
			return false;
		return d2_replay_registration(r, &control);
	}
	if (h->family != D2_REC_ENTRY)
		return true;
	if (!d2_entry_decode(record, h->total_bytes, h, &e))
		return false;
	if (!d2_replay_admission(r, &e))
		return false;
	if (!e.payload_object_id ||
	    e.payload_object_id <= r->highest_payload_id)
		return true;
	r->status = d2_files_payload_read(r->files, e.payload_object_offset,
					  &object, &allocation);
	if (r->status != D1_OK ||
	    object.payload_object_id != e.payload_object_id ||
	    object.content_len != e.payload_content_len) {
		free(allocation);
		return false;
	}
	free(r->snapshot);
	r->snapshot = malloc(object.content_len ? object.content_len : 1);
	if (!r->snapshot) {
		free(allocation);
		r->status = D1_NOSPC;
		return false;
	}
	memcpy(r->snapshot, object.content, object.content_len);
	r->snapshot_len = object.content_len;
	r->highest_payload_id = object.payload_object_id;
	free(allocation);
	return true;
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
	status = d2_files_scan(files, d2_replay_record, &replay, &scan, false);
	if (status != D1_OK || replay.status != D1_OK)
		goto fail;
	if (replay.snapshot) {
		status = d1_store_reopen(s->model, replay.snapshot,
					 replay.snapshot_len);
		s->next_payload_seq = (replay.highest_payload_id &
				       ((UINT64_C(1) << 40) - 1)) +
				      1;
	} else {
		status = d1_store_journal_enable(s->model);
	}
	free(replay.snapshot);
	replay.snapshot = NULL;
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
	free(replay.snapshot);
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

static uint32_t d2_persist_snapshot(struct d2_store *s,
				    const struct d1_envelope *env,
				    const struct d1_result *result,
				    uint64_t prior_eof)
{
	struct d2_payload_object object = { 0 };
	struct d2_wal_header h = { 0 };
	struct d2_entry entry = { 0 };
	uint8_t *snapshot = NULL;
	uint8_t *scratch = NULL;
	uint8_t record[D2_ENTRY_RECORD_BYTES];
	uint8_t handle[32];
	size_t snapshot_len;
	uint64_t eof, payload_id, offset;
	uint32_t status;

	status = d1_store_journal_snapshot(s->model, &snapshot, &snapshot_len);
	if (status != D1_OK)
		return status;
	if (snapshot_len > UINT32_MAX ||
	    s->next_payload_seq >= (UINT64_C(1) << 40)) {
		free(snapshot);
		return D1_NOSPC;
	}
	payload_id = (d2_files_super(s->files)->ds_incarnation << 40) |
		     s->next_payload_seq;
	memcpy(object.store_uuid, s->uuid.bytes, 16);
	object.payload_object_id = payload_id;
	object.content = snapshot;
	object.content_len = (uint32_t)snapshot_len;
	object.content_alg = 0;
	status = d2_files_payload_append(s->files, &object, &offset);
	if (status != D1_OK) {
		free(snapshot);
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
	if (!scratch) {
		free(scratch);
		free(snapshot);
		return D1_NOSPC;
	}
	if (!d1_envelope_digest(env, scratch, D1_ENVELOPE_MAX,
				entry.key.request_digest))
		d2_hash_domain("FFV2-D2B-SNAPSHOT-v1", snapshot, snapshot_len,
			       entry.key.request_digest);
	free(scratch);
	entry.txn_id = result->entries[0].txn.raw;
	entry.owner_cohort = result->entries[0].owner.cohort.raw;
	entry.owner_client_id = result->entries[0].owner.writer;
	entry.owner_co_id = result->entries[0].owner.co_id;
	entry.generation = result->entries[0].guard.generation;
	entry.result_guard_generation = result->entries[0].guard.generation;
	entry.result_guard_writer = result->entries[0].guard.writer;
	entry.result_guard_never_written =
		result->entries[0].guard.never_written;
	entry.result_activated = result->entries[0].activated;
	entry.result_visible_object_id = result->entries[0].version.raw;
	entry.payload_object_id = payload_id;
	entry.payload_object_offset = offset;
	entry.payload_content_len = (uint32_t)snapshot_len;
	eof = d1_store_eof(s->model, &env->object);
	entry.extent_high_water = eof;
	entry.extent_highest_index = eof ? (eof - 1) / s->chunk_bytes : 0;
	entry.extent_kind = eof > prior_eof ? D2_EXTENT_EXTEND :
			    eof < prior_eof ? D2_EXTENT_SHRINK :
					      D2_EXTENT_UNCHANGED;
	entry.index_generation = result->index_epoch;
	memcpy(entry.result_verifier, result->entries[0].verifier, 8);
	if (!d2_entry_encode(&h, &entry, record)) {
		free(snapshot);
		return D1_INVALID;
	}
	status = d2_files_wal_append(s->files, record, sizeof(record));
	free(snapshot);
	if (status == D1_OK)
		s->next_payload_seq++;
	return status;
}

uint32_t d2_store_apply(struct d2_store *s, const struct d1_envelope *env,
			struct d1_result *result)
{
	uint8_t *before = NULL, *after = NULL;
	uint8_t file_key[32];
	size_t before_len = 0, after_len = 0;
	uint64_t prior_eof;
	uint32_t status, persist;

	if (!s || !env || !result)
		return D1_INVALID;
	if (!d1_envelope_validate(env))
		return D1_INVALID;
	if (env->op >= D1_OP_MARK_ERROR || d1_envelope_member_count(env) != 1) {
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
	status = d2_register_file(s, &env->object, file_key);
	if (status != D1_OK) {
		if (status != D1_NOSPC)
			s->fenced = true;
		pthread_mutex_unlock(&s->lock);
		return status;
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
	persist = d2_persist_snapshot(s, env, result, prior_eof);
	if (persist != D1_OK) {
		s->fenced = true;
		status = persist;
		memset(result, 0, sizeof(*result));
		result->disposition = D1_UNRECORDED;
	}
out:
	free(before);
	free(after);
	pthread_mutex_unlock(&s->lock);
	return status;
}

/* Fixture controls are persisted by D1 and fenced if D2 cannot claim them. */
static uint32_t d2_fixture_checkpoint(struct d2_store *s)
{
	struct d1_envelope env = { 0 };
	struct d1_result result = { 0 };

	memcpy(env.key.origin.bytes, s->uuid.bytes, 16);
	env.key.sequence = s->synthetic_sequence++;
	env.op = D1_OP_LEASE_REAP;
	result.count = 1;
	result.disposition = D1_COMPLETED;
	result.entries[0].status = D1_OK;
	result.entries[0].disposition = D1_COMPLETED;
	result.entries[0].stability = D1_FILE_SYNC;
	return d2_persist_snapshot(s, &env, &result, 0);
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
	if (d1_admission_live(id) && d2_fixture_checkpoint(s) != D1_OK) {
		s->fenced = true;
		id = d1_admission_none();
	}
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
		auth.issuer.bytes[i] = (uint8_t)(0xf0u + i);
		auth.principal.bytes[i] = (uint8_t)(writer + i);
	}
	auth.writer = writer;
	auth.rights = rights;
	return d2_store_admit_full(s, object, &auth);
}

void d2_store_revoke(struct d2_store *s, d1_admission_id admission)
{
	if (!s)
		return;
	pthread_mutex_lock(&s->lock);
	if (!s->fenced) {
		d1_fixture_revoke(s->model, admission);
		if (d2_fixture_checkpoint(s) != D1_OK)
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
		if (d2_fixture_checkpoint(s) != D1_OK)
			s->fenced = true;
	}
	pthread_mutex_unlock(&s->lock);
}

d1_custody_id d2_store_custody(struct d2_store *s, d1_version_id version)
{
	d1_custody_id id = d1_custody_none();

	if (!s)
		return id;
	pthread_mutex_lock(&s->lock);
	if (!s->fenced)
		id = d1_fixture_custody(s->model, version);
	if (d1_custody_live(id) && d2_fixture_checkpoint(s) != D1_OK) {
		s->fenced = true;
		id = d1_custody_none();
	}
	pthread_mutex_unlock(&s->lock);
	return id;
}

void d2_store_certificate(struct d2_store *s,
			  const uint8_t certificate[D1_CERTIFICATE_BYTES])
{
	if (!s)
		return;
	pthread_mutex_lock(&s->lock);
	if (!s->fenced) {
		d1_fixture_certificate(s->model, certificate);
		if (d2_fixture_checkpoint(s) != D1_OK)
			s->fenced = true;
	}
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
