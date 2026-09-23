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
#include "d1_digest.h"
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
	uint64_t wal_promised;
	uint64_t recovery_allowance;
	bool recovery_spend;
	bool retired;
	uint32_t tombstone_count;
	uint8_t tombstones[D1_MAX_OBJECTS][32];
	uint32_t registered_count;
	uint32_t damaged_count;
	struct {
		uint64_t version_id;
		struct d1_checksum checksum;
	} damaged[D1_MAX_VERSIONS];
	struct {
		struct d1_objkey object;
		uint8_t file_key[32];
	} registered[D1_MAX_OBJECTS];
	struct {
		bool used;
		bool object_known;
		bool model_bound;
		bool trust_seen;
		bool authority_seen;
		bool revoked;
		bool expired;
		uint64_t recovery_txn_id;
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
		struct d1_objkey object;
		uint64_t index;
		struct d1_owner owner;
		uint32_t phase;
		uint64_t payload_bytes;
		bool repair;
		uint64_t cohort_id;
		bool recovery_eligible;
		bool recovery_trusted;
		bool recovery_vouched;
		bool recovery_admitted;
		bool predecessor_present;
		uint64_t predecessor_id;
	} work[D1_MAX_TXNS];
	struct {
		bool used;
		uint64_t custody_id;
		uint64_t version_id;
		uint64_t admission_id;
	} custodies[D1_MAX_CUSTODY];
	struct {
		bool used;
		uint64_t id;
		uint64_t admission_id;
		uint32_t phase;
		bool episode_present;
		uint64_t episode_id;
		bool certificate_installed;
		bool episode_cleared;
		uint32_t count;
		struct {
			uint32_t mode;
			struct d1_objkey object;
			uint64_t index;
			uint64_t txn_id;
			struct d1_owner owner;
			uint64_t custody_id;
			bool postcond_present;
			uint64_t postcond_id;
			uint64_t successor_id;
			bool predecessor_present;
			uint64_t predecessor_id;
			uint32_t predecessor_generation;
			uint64_t payload_id;
			uint64_t payload_offset;
			uint32_t payload_len;
			uint32_t payload_ck_alg;
			uint32_t payload_ck_len;
			uint8_t payload_ck[sizeof(
				((struct d1_checksum *)0)->digest)];
		} members[D1_BATCH_ENTRIES_MAX];
	} cohorts[D1_MAX_REPAIRS];
	struct {
		bool used;
		uint64_t id;
		uint64_t admission_id;
		struct d1_objkey object;
		uint32_t count;
		struct {
			uint64_t index;
			uint64_t version_id;
			uint64_t custody_id;
		} items[D1_BATCH_ENTRIES_MAX];
	} episodes[D1_MAX_EPISODES];
	struct {
		bool used;
		uint64_t episode_id;
		uint64_t cohort_id;
		uint8_t value[D1_CERTIFICATE_BYTES];
	} certificates[D1_MAX_REPAIRS];
	bool fenced;
};

struct d2_replay {
	struct d2_files *files;
	struct d2_store *store;
	uint64_t highest_payload_id;
	uint64_t last_incarnation;
	uint32_t status;
	bool postcond_pending;
	uint64_t postcond_id;
	uint8_t postcond_file_key[32];
	uint64_t postcond_chunk_index;
	uint64_t postcond_version_id;
	uint64_t postcond_custody_id;
};

static struct d1_objkey *d2_replay_object(struct d2_replay *r,
					  const uint8_t file_key[32]);

static bool d2_same_object(const struct d1_objkey *a, const struct d1_objkey *b)
{
	return !memcmp(a, b, sizeof(*a));
}

static bool d2_file_tombstoned(const struct d2_store *s,
			       const uint8_t file_key[32])
{
	uint32_t i;

	for (i = 0; i < s->tombstone_count; i++)
		if (!memcmp(s->tombstones[i], file_key, 32))
			return true;
	return false;
}

static bool d2_object_tombstoned(const struct d2_store *s,
				 const struct d1_objkey *object)
{
	uint8_t handle[32], file_key[32];

	memcpy(handle, object->export_uuid.bytes, 16);
	memcpy(handle + 16, object->object_uuid.bytes, 16);
	d2_file_key(handle, sizeof(handle), file_key);
	return d2_file_tombstoned(s, file_key);
}

static bool d2_tombstone_remember(struct d2_store *s,
				  const uint8_t file_key[32])
{
	if (d2_file_tombstoned(s, file_key) ||
	    s->tombstone_count == D1_MAX_OBJECTS)
		return false;
	memcpy(s->tombstones[s->tombstone_count++], file_key, 32);
	return true;
}

static bool d2_same_key(const struct d1_opkey *a, const struct d1_opkey *b)
{
	return !memcmp(a->origin.bytes, b->origin.bytes, D1_UUID_BYTES) &&
	       a->sequence == b->sequence && a->ordinal == b->ordinal;
}

static uint32_t d2_wal_append_floor(struct d2_store *s,
				    const uint8_t *record, size_t len,
				    uint64_t promised);
static uint32_t d2_wal_append_group_floor(struct d2_store *s,
					  const uint8_t *records, size_t len,
					  uint64_t promised);

static uint32_t d2_wal_append(struct d2_store *s, const uint8_t *record,
			      size_t len)
{
	return d2_wal_append_floor(s, record, len, s->wal_promised);
}

static uint32_t d2_wal_append_floor(struct d2_store *s,
				    const uint8_t *record, size_t len,
				    uint64_t promised)
{
	uint64_t allowance = s->recovery_allowance;
	uint64_t floor;
	uint32_t status;

	if (s->recovery_spend) {
		if (len > allowance)
			return D1_NOSPC;
		allowance -= len;
	}
	if (allowance > UINT64_MAX - promised)
		return D1_NOSPC;
	floor = promised + allowance;
	status = d2_files_wal_append_floor(s->files, record, len, floor);
	if (status == D1_OK && s->recovery_spend)
		s->recovery_allowance = allowance;
	return status;
}

static uint32_t d2_wal_append_group_floor(struct d2_store *s,
					  const uint8_t *records, size_t len,
					  uint64_t promised)
{
	uint64_t allowance = s->recovery_allowance;
	uint64_t floor;
	uint32_t status;

	if (s->recovery_spend) {
		if (len > allowance)
			return D1_NOSPC;
		allowance -= len;
	}
	if (allowance > UINT64_MAX - promised)
		return D1_NOSPC;
	floor = promised + allowance;
	status = d2_files_wal_append_group_floor(s->files, records, len, floor);
	if (status == D1_OK && s->recovery_spend)
		s->recovery_allowance = allowance;
	return status;
}

static uint64_t d2_cohort_promise(uint32_t phase, size_t record_bytes,
				  bool episode_present, bool installed,
				  bool cleared)
{
	uint64_t records;

	switch (phase) {
	case D2_ADMITTED:
		records = 4;
		break;
	case D2_PREPARED:
		records = 3;
		break;
	case D2_FINALIZED:
		records = 2;
		break;
	case D2_COMMITTED:
		records = 1;
		break;
	default:
		return 0;
	}
	return records * record_bytes +
	       (episode_present && !installed ? 268u : 0u) +
	       (episode_present && !cleared ? 916u : 0u);
}

static uint64_t d2_work_promise(uint32_t phase)
{
	switch (phase) {
	case D2_PREPARED:
		return 2u * D2_ENTRY_RECORD_BYTES;
	case D2_FINALIZED:
		return D2_ENTRY_RECORD_BYTES;
	default:
		return 0;
	}
}

static bool
d2_work_recoverable(const typeof(((struct d2_store *)0)->work[0]) *work)
{
	if (work->repair)
		return work->phase >= D2_ADMITTED && work->phase <= D2_COMMITTED;
	return d2_work_promise(work->phase) != 0;
}

static bool d2_rebuild_promises(struct d2_store *s)
{
	uint64_t total = 0, add;
	uint32_t i;

	for (i = 0; i < D1_MAX_REPAIRS; i++) {
		if (!s->cohorts[i].used)
			continue;
		add = d2_cohort_promise(s->cohorts[i].phase,
					332u + 244u * s->cohorts[i].count,
					s->cohorts[i].episode_present,
					s->cohorts[i].certificate_installed,
					s->cohorts[i].episode_cleared);
		if (add > UINT64_MAX - total)
			return false;
		total += add;
	}
	for (i = 0; i < D1_MAX_TXNS; i++) {
		if (!s->work[i].used || s->work[i].repair)
			continue;
		add = d2_work_promise(s->work[i].phase);
		if (add > UINT64_MAX - total)
			return false;
		total += add;
	}
	s->wal_promised = total;
	return true;
}

static typeof(((struct d2_store *)0)->admissions[0]) *
d2_admission_slot(struct d2_store *s, uint64_t id);

static bool d2_rebuild_payload_accounting(const struct d2_store *s,
					  uint64_t *outstanding,
					  uint64_t *staged)
{
	uint64_t total = 0;
	uint32_t i;

	for (i = 0; i < D1_MAX_TXNS; i++) {
		if (!s->work[i].used || s->work[i].repair ||
		    !d2_work_promise(s->work[i].phase))
			continue;
		if (s->work[i].payload_bytes > UINT64_MAX - total)
			return false;
		total += s->work[i].payload_bytes;
	}
	*outstanding = 0;
	*staged = total;
	return true;
}

static void d2_open_recovery_window(struct d2_store *s, uint64_t allowance)
{
	uint32_t i;
	bool any = false;

	for (i = 0; i < D1_MAX_TXNS; i++) {
		typeof(s->work[0]) *work = &s->work[i];
		typeof(s->admissions[0]) *admission;

		work->recovery_eligible = false;
		work->recovery_trusted = false;
		work->recovery_vouched = false;
		work->recovery_admitted = false;
		if (!work->used || !d2_work_recoverable(work))
			continue;
		any = true;
		admission = d2_admission_slot(s, work->admission_id);
		if (!admission || (admission->expired && !work->repair))
			continue;
		work->recovery_eligible = true;
	}
	s->recovery_allowance = any ? allowance : 0;
}

static void d2_close_recovery_window_if_complete(struct d2_store *s)
{
	uint32_t i;

	for (i = 0; i < D1_MAX_TXNS; i++)
		if (s->work[i].used && s->work[i].recovery_eligible &&
		    !s->work[i].recovery_admitted)
			return;
	s->recovery_allowance = 0;
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
			     uint64_t version_id, uint64_t admission_id,
			     const struct d1_objkey *object, uint64_t index,
			     const struct d1_owner *owner,
			     uint32_t phase, uint64_t payload_bytes,
			     bool predecessor_present,
			     uint64_t predecessor_id)
{
	uint32_t i;

	if (!txn_id || !version_id || !admission_id)
		return true;
	for (i = 0; i < D1_MAX_TXNS; i++) {
		if (s->work[i].used && s->work[i].txn_id == txn_id)
			return s->work[i].version_id == version_id &&
			       s->work[i].admission_id == admission_id &&
			       s->work[i].phase == phase &&
			       s->work[i].payload_bytes == payload_bytes &&
			       s->work[i].index == index &&
			       !memcmp(&s->work[i].owner, owner, sizeof(*owner)) &&
			       s->work[i].predecessor_present ==
				       predecessor_present &&
			       s->work[i].predecessor_id == predecessor_id &&
			       !memcmp(&s->work[i].object, object,
				       sizeof(*object));
		if (!s->work[i].used) {
			s->work[i].used = true;
			s->work[i].txn_id = txn_id;
			s->work[i].version_id = version_id;
			s->work[i].admission_id = admission_id;
			s->work[i].object = *object;
			s->work[i].index = index;
			s->work[i].owner = *owner;
			s->work[i].phase = phase;
			s->work[i].payload_bytes = payload_bytes;
			s->work[i].predecessor_present = predecessor_present;
			s->work[i].predecessor_id = predecessor_id;
			return true;
		}
	}
	return false;
}

static bool d2_work_rebind(struct d2_store *s, uint64_t txn_id,
			   uint64_t old_admission, uint64_t new_admission)
{
	uint32_t i;

	for (i = 0; i < D1_MAX_TXNS; i++)
		if (s->work[i].used && s->work[i].txn_id == txn_id) {
			uint32_t j;

			if (s->work[i].admission_id != old_admission)
				return false;
			if (s->work[i].repair) {
				for (j = 0; j < D1_MAX_REPAIRS; j++)
					if (s->cohorts[j].used &&
					    s->cohorts[j].id == s->work[i].cohort_id)
						break;
				if (j == D1_MAX_REPAIRS ||
				    (s->cohorts[j].admission_id != old_admission &&
				     s->cohorts[j].admission_id != new_admission))
					return false;
				if (s->cohorts[j].admission_id == old_admission)
					s->cohorts[j].admission_id = new_admission;
			}
			s->work[i].admission_id = new_admission;
			return true;
		}
	return false;
}

static bool d2_recovery_rebind(struct d2_store *s,
			       const struct d1_envelope *env)
{
	uint32_t i;

	for (i = 0; i < env->body.control.count; i++)
		if (!d2_work_rebind(s, env->body.control.txns[i].raw,
				    env->body.control.old_admission.raw,
				    env->body.control.new_admission.raw))
			return false;
	return true;
}

static bool d2_repair_work_remember(struct d2_store *s, uint64_t txn_id,
				    uint64_t admission_id,
				    const struct d1_objkey *object,
				    uint64_t index,
				    const struct d1_owner *owner,
				    uint64_t cohort_id, uint32_t phase)
{
	uint32_t i;

	if (!txn_id || !admission_id || !cohort_id)
		return false;
	for (i = 0; i < D1_MAX_TXNS; i++) {
		if (s->work[i].used && s->work[i].txn_id == txn_id)
			return s->work[i].repair &&
			       s->work[i].cohort_id == cohort_id;
		if (!s->work[i].used) {
			memset(&s->work[i], 0, sizeof(s->work[i]));
			s->work[i].used = true;
			s->work[i].repair = true;
			s->work[i].cohort_id = cohort_id;
			s->work[i].txn_id = txn_id;
			s->work[i].admission_id = admission_id;
			s->work[i].object = *object;
			s->work[i].index = index;
			s->work[i].owner = *owner;
			s->work[i].phase = phase;
			return true;
		}
	}
	return false;
}

static bool d2_repair_work_set_phase(struct d2_store *s, uint64_t cohort_id,
				     uint32_t phase)
{
	uint32_t i;
	bool found = false;

	for (i = 0; i < D1_MAX_TXNS; i++)
		if (s->work[i].used && s->work[i].repair &&
		    s->work[i].cohort_id == cohort_id) {
			s->work[i].phase = phase;
			found = true;
		}
	return found;
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

static bool d2_version_damaged(const struct d2_store *s, uint64_t version_id)
{
	uint32_t i;

	for (i = 0; i < s->damaged_count; i++)
		if (s->damaged[i].version_id == version_id)
			return true;
	return false;
}

static bool d2_work_set_phase(struct d2_store *s, uint64_t txn_id,
			      uint32_t phase)
{
	uint32_t i;

	for (i = 0; i < D1_MAX_TXNS; i++)
		if (s->work[i].used && s->work[i].txn_id == txn_id) {
			s->work[i].phase = phase;
			return true;
		}
	return false;
}

static bool d2_work_set_version(struct d2_store *s, uint64_t txn_id,
				uint64_t version_id)
{
	uint32_t i;

	for (i = 0; i < D1_MAX_TXNS; i++)
		if (s->work[i].used && s->work[i].txn_id == txn_id) {
			s->work[i].version_id = version_id;
			return true;
		}
	return false;
}

static const struct d1_fixture_authority *
d2_admission_find(const struct d2_store *s, uint64_t id);

static bool d2_recovery_matches(const struct d2_store *s,
				const typeof(((struct d2_store *)0)->work[0]) *work,
				const struct d1_objkey *object,
				const struct d1_fixture_authority *fresh)
{
	const struct d1_fixture_authority *old;

	if (!work->used || !work->recovery_eligible ||
	    !(fresh->rights & (D1_RIGHT_READ | D1_RIGHT_WRITE)) ||
	    (object && !d2_same_object(&work->object, object)))
		return false;
	old = d2_admission_find(s, work->admission_id);
	return old && old->writer == fresh->writer &&
	       !memcmp(old->issuer.bytes, fresh->issuer.bytes, D1_UUID_BYTES) &&
	       !memcmp(old->principal.bytes, fresh->principal.bytes,
		       D1_UUID_BYTES);
}

static typeof(((struct d2_store *)0)->work[0]) *
d2_recovery_candidate(struct d2_store *s, const struct d1_objkey *object,
			      const struct d1_fixture_authority *fresh)
{
	typeof(s->work[0]) *candidate = NULL;
	uint32_t i;

	for (i = 0; i < D1_MAX_TXNS; i++) {
		if (s->work[i].recovery_trusted ||
		    s->work[i].recovery_vouched ||
		    !d2_recovery_matches(s, &s->work[i], object, fresh))
			continue;
		if (!candidate || s->work[i].txn_id < candidate->txn_id)
			candidate = &s->work[i];
	}
	return candidate;
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

static typeof(((struct d2_store *)0)->cohorts[0]) *
d2_cohort_find(struct d2_store *s, uint64_t id)
{
	uint32_t i;

	for (i = 0; i < D1_MAX_REPAIRS; i++)
		if (s->cohorts[i].used && s->cohorts[i].id == id)
			return &s->cohorts[i];
	return NULL;
}

static typeof(((struct d2_store *)0)->cohorts[0]) *
d2_cohort_spare(struct d2_store *s)
{
	uint32_t i;

	for (i = 0; i < D1_MAX_REPAIRS; i++)
		if (!s->cohorts[i].used)
			return &s->cohorts[i];
	return NULL;
}

static void d2_episode_uuid(uint64_t id, uint8_t uuid[16])
{
	static const uint8_t prefix[8] = { 'D', '2', 'B', 'E',
					   'P', 'I', 'S', 'O' };
	unsigned int i;

	memcpy(uuid, prefix, sizeof(prefix));
	for (i = 0; i < 8; i++)
		uuid[8 + i] = (uint8_t)(id >> (56 - 8 * i));
}

static bool d2_episode_id(const uint8_t uuid[16], uint64_t *id)
{
	static const uint8_t prefix[8] = { 'D', '2', 'B', 'E',
					   'P', 'I', 'S', 'O' };
	unsigned int i;

	if (memcmp(uuid, prefix, sizeof(prefix)))
		return false;
	*id = 0;
	for (i = 0; i < 8; i++)
		*id = (*id << 8) | uuid[8 + i];
	return *id != 0;
}

static typeof(((struct d2_store *)0)->episodes[0]) *
d2_episode_find(struct d2_store *s, uint64_t id)
{
	uint32_t i;

	for (i = 0; i < D1_MAX_EPISODES; i++)
		if (s->episodes[i].used && s->episodes[i].id == id)
			return &s->episodes[i];
	return NULL;
}

static bool d2_episode_remember(struct d2_store *s,
				const struct d1_envelope *env,
				const struct d1_result *result)
{
	typeof(s->episodes[0]) *episode = NULL;
	uint32_t i;

	if (!result->entries[0].episode_present)
		return false;
	if (d2_episode_find(s, result->entries[0].episode.raw))
		return true;
	for (i = 0; i < D1_MAX_EPISODES; i++)
		if (!s->episodes[i].used) {
			episode = &s->episodes[i];
			break;
		}
	if (!episode)
		return false;
	memset(episode, 0, sizeof(*episode));
	episode->used = true;
	episode->id = result->entries[0].episode.raw;
	episode->admission_id = env->admission.raw;
	episode->object = env->object;
	episode->count = env->body.repair.count;
	for (i = 0; i < episode->count; i++) {
		episode->items[i].index = env->body.repair.entries[i].index;
		episode->items[i].version_id =
			env->body.repair.entries[i].successor.raw;
		episode->items[i].custody_id =
			env->body.repair.entries[i].custody.raw;
	}
	return true;
}

static typeof(((struct d2_store *)0)->certificates[0]) *
d2_certificate_find(struct d2_store *s, uint64_t episode_id,
			    uint64_t cohort_id)
{
	uint32_t i;

	for (i = 0; i < D1_MAX_REPAIRS; i++)
		if (s->certificates[i].used &&
		    s->certificates[i].episode_id == episode_id &&
		    s->certificates[i].cohort_id == cohort_id)
			return &s->certificates[i];
	return NULL;
}

static bool d2_certificate_remember(
	struct d2_store *s, uint64_t episode_id, uint64_t cohort_id,
	const uint8_t certificate[D1_CERTIFICATE_BYTES])
{
	uint32_t i;

	if (d2_certificate_find(s, episode_id, cohort_id))
		return false;
	for (i = 0; i < D1_MAX_REPAIRS; i++)
		if (!s->certificates[i].used) {
			s->certificates[i].used = true;
			s->certificates[i].episode_id = episode_id;
			s->certificates[i].cohort_id = cohort_id;
			memcpy(s->certificates[i].value, certificate,
			       D1_CERTIFICATE_BYTES);
			return true;
		}
	return false;
}

static bool d2_cohort_remember_begin(struct d2_store *s,
				     const struct d1_envelope *env,
				     const struct d1_result *result)
{
	typeof(s->cohorts[0]) *cohort;
	uint32_t i;

	if (!result->entries[0].cohort_present ||
	    result->entries[0].member_txn_count != env->body.repair.count)
		return false;
	cohort = d2_cohort_find(s, result->entries[0].cohort.raw);
	if (cohort)
		return true;
	cohort = d2_cohort_spare(s);
	if (!cohort)
		return false;
	memset(cohort, 0, sizeof(*cohort));
	cohort->used = true;
	cohort->id = result->entries[0].cohort.raw;
	cohort->admission_id = env->admission.raw;
	cohort->phase = D2_ADMITTED;
	cohort->episode_present = env->body.repair.episode_present;
	cohort->episode_id = env->body.repair.episode.raw;
	cohort->count = env->body.repair.count;
	for (i = 0; i < cohort->count; i++) {
		const struct d1_repair_entry *entry =
			&env->body.repair.entries[i];

		cohort->members[i].mode = entry->mode;
		cohort->members[i].object = env->object;
		cohort->members[i].index = entry->index;
		cohort->members[i].txn_id =
			result->entries[0].member_txn[i].raw;
		cohort->members[i].owner = entry->owner;
		cohort->members[i].custody_id = entry->custody.raw;
		cohort->members[i].postcond_present = entry->postcond_present;
		cohort->members[i].postcond_id = entry->postcond.raw;
		cohort->members[i].successor_id = entry->successor.raw;
		cohort->members[i].predecessor_present =
			entry->predecessor_present;
		cohort->members[i].predecessor_id = entry->predecessor.raw;
		if (!d2_repair_work_remember(s, cohort->members[i].txn_id,
					     cohort->admission_id,
					     &cohort->members[i].object,
					     cohort->members[i].index,
					     &cohort->members[i].owner,
					     cohort->id, D2_ADMITTED))
			return false;
	}
	return true;
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

static typeof(((struct d2_store *)0)->admissions[0]) *
d2_admission_slot(struct d2_store *s, uint64_t id)
{
	uint32_t i;

	for (i = 0; i < D1_MAX_ADMISSIONS; i++)
		if (s->admissions[i].used && s->admissions[i].id == id)
			return &s->admissions[i];
	return NULL;
}

static bool d2_admission_matches(const struct d2_store *s, uint64_t id,
				 const struct d1_objkey *object)
{
	uint32_t i;

	for (i = 0; i < D1_MAX_ADMISSIONS; i++)
		if (s->admissions[i].used && s->admissions[i].id == id)
			return s->admissions[i].object_known &&
			       d2_same_object(&s->admissions[i].object, object);
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
		if (s->admissions[i].used && s->admissions[i].id == id) {
			if (memcmp(&s->admissions[i].auth, auth,
				   sizeof(*auth)) ||
			    (s->admissions[i].object_known &&
			     !d2_same_object(&s->admissions[i].object, object)))
				return false;
			s->admissions[i].object = *object;
			s->admissions[i].object_known = true;
			s->admissions[i].model_bound = true;
			return true;
		}
		if (!s->admissions[i].used) {
			s->admissions[i].used = true;
			s->admissions[i].object_known = true;
			s->admissions[i].model_bound = true;
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

static bool d2_replay_trust(struct d2_replay *r,
			    const struct d2_wal_header *h,
			    const struct d2_control *control)
{
	typeof(r->store->admissions[0]) *actor, *slot;
	struct d1_fixture_authority auth = { 0 };
	struct d1_cursor cursor;
	uint8_t body_other[12], other[12], session[16], principal[16];
	uint64_t lease_epoch, fence_sequence, beneficiary_id;
	uint32_t seqid, expected_seqid, iomode, writer, rights;

	if (control->subtype != D2_CTL_TRUST_STATEID)
		return true;
	d1_dec_init(&cursor, control->body, control->body_len);
	if (!control->admission_client_id ||
	    control->transition != D2_COMMITTED ||
	    control->status != D1_OK || !d1_dec_u32(&cursor, &seqid) ||
	    !d1_dec_raw(&cursor, body_other, sizeof(body_other)) ||
	    !d1_dec_u64(&cursor, &lease_epoch) ||
	    !d1_dec_u64(&cursor, &fence_sequence) ||
	    !d1_dec_u32(&cursor, &iomode) ||
	    !d1_dec_raw(&cursor, session, sizeof(session)) ||
	    !d1_dec_u64(&cursor, &beneficiary_id) ||
	    !d1_dec_raw(&cursor, principal, sizeof(principal)) ||
	    !d1_dec_u32(&cursor, &writer) || !d1_dec_u32(&cursor, &rights) ||
	    !d1_dec_finished(&cursor) || !beneficiary_id ||
	    iomode != (rights & (D1_RIGHT_READ | D1_RIGHT_WRITE)))
		return false;
	d2_stateid(beneficiary_id, &expected_seqid, other);
	if (seqid != expected_seqid || memcmp(body_other, other, sizeof(other)))
		return false;
	memcpy(auth.session, session, sizeof(auth.session));
	memcpy(auth.principal.bytes, principal, sizeof(auth.principal.bytes));
	memcpy(auth.issuer.bytes, control->admission_issuer,
	       sizeof(auth.issuer.bytes));
	auth.authority_epoch = control->admission_authority_epoch;
	auth.fence_sequence = fence_sequence;
	auth.lease_epoch = lease_epoch;
	auth.writer = writer;
	auth.rights = rights;
	actor = d2_admission_slot(r->store, control->admission_client_id);
	if (control->admission_client_id != beneficiary_id &&
	    (!actor || memcmp(actor->auth.issuer.bytes, auth.issuer.bytes,
			      D1_UUID_BYTES) ||
	     actor->auth.authority_epoch != auth.authority_epoch))
		return false;
	slot = d2_admission_slot(r->store, beneficiary_id);
	if (slot)
		return false;
	for (uint32_t i = 0; i < D1_MAX_ADMISSIONS; i++) {
		typeof(r->store->work[0]) *candidate = NULL;

		slot = &r->store->admissions[i];
		if (slot->used)
			continue;
		slot->used = true;
		slot->id = beneficiary_id;
		slot->auth = auth;
		slot->trust_seen = true;
		candidate = d2_recovery_candidate(r->store, NULL, &auth);
		if (candidate) {
			if (h->total_bytes > r->store->recovery_allowance)
				return false;
			r->store->recovery_allowance -= h->total_bytes;
			candidate->recovery_trusted = true;
			slot->recovery_txn_id = candidate->txn_id;
		}
		return true;
	}
	return false;
}

static bool d2_replay_authority(struct d2_replay *r,
				const struct d2_wal_header *h,
				const struct d2_control *control)
{
	typeof(r->store->admissions[0]) *actor,
		*beneficiary[D2_MAX_AUTHORITY_STATEIDS];
	typeof(r->store->work[0]) *candidate[D2_MAX_AUTHORITY_STATEIDS];
	struct d1_cursor cursor;
	uint8_t issuer[16], other[12], expected_other[12], previous[16] = { 0 },
		stateid[16];
	uint64_t epoch;
	uint32_t count, i, j, seqid, expected_seqid;
	bool funded;

	if (control->subtype != D2_CTL_AUTHORITY_ADMIT)
		return true;
	actor = d2_admission_slot(r->store, control->admission_client_id);
	d1_dec_init(&cursor, control->body, control->body_len);
	if (!actor ||
	    control->transition != D2_COMMITTED || control->status != D1_OK ||
	    !d1_dec_raw(&cursor, issuer, sizeof(issuer)) ||
	    !d1_dec_u64(&cursor, &epoch) || !d1_dec_u32(&cursor, &count) ||
	    count > D2_MAX_AUTHORITY_STATEIDS ||
	    memcmp(issuer, control->admission_issuer, sizeof(issuer)) ||
	    memcmp(issuer, actor->auth.issuer.bytes, sizeof(issuer)) ||
	    control->admission_authority_epoch != actor->auth.authority_epoch)
		return false;
	funded = count != 0;
	for (i = 0; i < count; i++) {
		beneficiary[i] = NULL;
		candidate[i] = NULL;
		if (!d1_dec_u32(&cursor, &seqid) ||
		    !d1_dec_raw(&cursor, other, sizeof(other)))
			return false;
		stateid[0] = (uint8_t)(seqid >> 24);
		stateid[1] = (uint8_t)(seqid >> 16);
		stateid[2] = (uint8_t)(seqid >> 8);
		stateid[3] = (uint8_t)seqid;
		memcpy(stateid + 4, other, sizeof(other));
		if (i && memcmp(previous, stateid, sizeof(stateid)) >= 0)
			return false;
		memcpy(previous, stateid, sizeof(previous));
		for (j = 0; j < D1_MAX_ADMISSIONS; j++) {
			typeof(r->store->admissions[0]) *slot =
				&r->store->admissions[j];

			if (!slot->used)
				continue;
			d2_stateid(slot->id, &expected_seqid, expected_other);
			if (seqid == expected_seqid &&
			    !memcmp(other, expected_other, sizeof(other))) {
				beneficiary[i] = slot;
				break;
			}
		}
		if (!beneficiary[i] || beneficiary[i]->authority_seen)
			return false;
		candidate[i] = (typeof(r->store->work[0]) *)d2_work_by_txn(
			r->store, beneficiary[i]->recovery_txn_id);
		if (!candidate[i] || !candidate[i]->recovery_trusted ||
		    candidate[i]->recovery_vouched)
			funded = false;
	}
	if (!d1_dec_finished(&cursor))
		return false;
	if (funded) {
		if (h->total_bytes > r->store->recovery_allowance)
			return false;
		r->store->recovery_allowance -= h->total_bytes;
	}
	for (i = 0; i < count; i++) {
		beneficiary[i]->authority_seen = true;
		memcpy(beneficiary[i]->auth.issuer.bytes, issuer, sizeof(issuer));
		beneficiary[i]->auth.authority_epoch = epoch;
		if (funded)
			candidate[i]->recovery_vouched = true;
	}
	return true;
}

static bool d2_replay_liveness(struct d2_replay *r,
			       const struct d2_control *control)
{
	typeof(r->store->admissions[0]) *slot;
	const struct d1_fixture_authority *auth;
	struct d1_cursor cursor;
	uint8_t issuer[16], session[16], principal[16], other[12];
	uint64_t client_id, epoch, lease_epoch;
	uint32_t reason, seqid, expected_seqid;

	if (control->subtype != D2_CTL_AUTHORITY_REVOKE &&
	    control->subtype != D2_CTL_REVOKE_STATEID &&
	    control->subtype != D2_CTL_LEASE_EXPIRE)
		return true;
	client_id = control->admission_client_id;
	slot = d2_admission_slot(r->store, client_id);
	auth = d2_admission_find(r->store, client_id);
	if (!slot || !auth || slot->revoked || slot->expired ||
	    control->transition != D2_COMMITTED ||
	    control->status != D1_OK ||
	    memcmp(control->key.session, auth->session, sizeof(auth->session)) ||
	    memcmp(control->admission_issuer, auth->issuer.bytes, 16) ||
	    control->admission_authority_epoch != auth->authority_epoch)
		return false;
	d1_dec_init(&cursor, control->body, control->body_len);
	if (control->subtype == D2_CTL_AUTHORITY_REVOKE) {
		if (!d1_dec_raw(&cursor, issuer, sizeof(issuer)) ||
		    !d1_dec_u64(&cursor, &epoch) ||
		    !d1_dec_u32(&cursor, &reason) ||
		    !d1_dec_finished(&cursor) || !epoch || !reason)
			return false;
		for (uint32_t i = 0; i < D1_MAX_ADMISSIONS; i++) {
			slot = &r->store->admissions[i];
			if (!slot->used || slot->auth.authority_epoch != epoch ||
			    memcmp(slot->auth.issuer.bytes, issuer, sizeof(issuer)))
				continue;
			slot->revoked = true;
			if (slot->model_bound)
				d1_fixture_revoke(
					r->store->model,
					d1_fixture_admission_handle(
						r->store->model, slot->id));
		}
		return true;
	} else if (control->subtype == D2_CTL_REVOKE_STATEID) {
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
		slot = d2_admission_slot(r->store, client_id);
		if (!slot)
			return false;
		slot->revoked = true;
		if (slot->model_bound)
			d1_fixture_revoke(
				r->store->model,
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
		slot = d2_admission_slot(r->store, client_id);
		if (!slot)
			return false;
		slot->expired = true;
		if (slot->model_bound)
			d1_fixture_expire(
				r->store->model,
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
	if (!work || work->admission_id != control->admission_client_id ||
	    d2_object_tombstoned(r->store, &work->object))
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

static bool d2_replay_reap(struct d2_replay *r,
			   const struct d2_wal_header *h,
			   const struct d2_control *control)
{
	struct d1_envelope env = { 0 };
	struct d1_result result;
	struct d1_cursor cursor;
	const typeof(r->store->admissions[0]) *admission;
	const typeof(r->store->work[0]) *work, *items[D2_MAX_COUNTED_ENTRIES];
	uint8_t file_key[32];
	uint64_t txn_id, index;
	uint32_t base, count, i, n;

	if (control->subtype != D2_CTL_LEASE_REAP)
		return true;
	admission = d2_admission_slot(r->store,
				       control->admission_client_id);
	if (!admission || !admission->used || !admission->expired ||
	    control->transition != D2_COMMITTED || control->status != D1_OK ||
	    memcmp(control->admission_issuer, admission->auth.issuer.bytes,
		   D1_UUID_BYTES) ||
	    control->admission_authority_epoch !=
		    admission->auth.authority_epoch)
		return false;
	d1_dec_init(&cursor, control->body, control->body_len);
	if (!d1_dec_u32(&cursor, &count) || !count ||
	    count > D2_MAX_COUNTED_ENTRIES)
		return false;
	for (i = 0; i < count; i++) {
		if (!d1_dec_u64(&cursor, &txn_id) ||
		    !d1_dec_raw(&cursor, file_key, sizeof(file_key)) ||
		    !d1_dec_u64(&cursor, &index))
			return false;
		work = d2_work_by_txn(r->store, txn_id);
		if (!work || work->admission_id != admission->id ||
		    work->index != index ||
		    (work->phase != D2_PREPARED &&
		     work->phase != D2_FINALIZED))
			return false;
		items[i] = work;
		if (!i) {
			env.object = work->object;
		} else if (!d2_same_object(&env.object, &work->object)) {
			return false;
		}
		if (d2_file_tombstoned(r->store, file_key) ||
		    !d2_replay_object(r, file_key) ||
		    !d2_same_object(d2_replay_object(r, file_key),
				    &work->object))
			return false;
	}
	if (!d1_dec_finished(&cursor))
		return false;
	env.admission = d1_fixture_admission_handle(r->store->model,
						    admission->id);
	env.incarnation = h->ds_incarnation;
	env.op = D1_OP_LEASE_REAP;
	env.body.control.old_admission = env.admission;
	for (base = 0; base < count; base += D1_BATCH_ENTRIES_MAX) {
		n = count - base;
		if (n > D1_BATCH_ENTRIES_MAX)
			n = D1_BATCH_ENTRIES_MAX;
		env.key.sequence = (h->ds_incarnation << 32) |
				   control->key.sequence;
		env.key.ordinal = base / D1_BATCH_ENTRIES_MAX;
		env.body.control.count = n;
		for (i = 0; i < n; i++)
			env.body.control.txns[i] = d1_fixture_txn_handle(
				r->store->model, items[base + i]->txn_id);
		r->status = d1_store_apply(r->store->model, &env, &result);
		if (r->status != D1_OK || result.count != 1 ||
		    result.entries[0].status != D1_OK ||
		    result.entries[0].phase != D2_ROLLED_BACK)
			return false;
	}
	for (i = 0; i < count; i++)
		if (!d2_work_set_phase(r->store, items[i]->txn_id,
				       D2_ROLLED_BACK))
			return false;
	if (h->total_bytes > r->store->recovery_allowance)
		return false;
	r->store->recovery_allowance -= h->total_bytes;
	return d2_rebuild_promises(r->store);
}

static bool d2_replay_postcond(struct d2_replay *r,
			       const struct d2_control *control)
{
	const typeof(r->store->custodies[0]) *custody;
	const typeof(r->store->work[0]) *work;
	struct d1_envelope env = { 0 };
	struct d1_result result;
	struct d1_cursor cursor;
	struct d1_objkey *object = NULL;
	uint8_t file_key[32];
	uint64_t id, index, version_id, custody_id;
	uint32_t kind, i;
	bool predecessor_retained, predecessor_released = false;
	bool registered = false;

	if (control->subtype != D2_CTL_POSTCOND)
		return true;
	if (r->postcond_pending || control->transition != D2_COMMITTED ||
	    control->status != D1_OK)
		return false;
	d1_dec_init(&cursor, control->body, control->body_len);
	if (!d1_dec_u64(&cursor, &id) ||
	    !d1_dec_raw(&cursor, file_key, sizeof(file_key)) ||
	    !d1_dec_u64(&cursor, &index) || !d1_dec_u64(&cursor, &version_id) ||
	    !d1_dec_u64(&cursor, &custody_id) || !d1_dec_u32(&cursor, &kind) ||
	    !d1_dec_finished(&cursor) || !id || kind < 1 || kind > 3)
		return false;
	for (i = 0; i < r->store->registered_count; i++)
		if (!memcmp(r->store->registered[i].file_key, file_key,
			    sizeof(file_key))) {
			registered = true;
			object = &r->store->registered[i].object;
			break;
		}
	custody = d2_custody_by_version(r->store, version_id);
	work = d2_work_by_version(r->store, version_id);
	if (!registered || d2_file_tombstoned(r->store, file_key) ||
	    !custody || custody->custody_id != custody_id ||
	    !work || !d2_same_object(&work->object, object))
		return false;
	predecessor_retained = work->predecessor_present &&
		d1_fixture_version_retained(
			r->store->model,
			d1_fixture_version_handle(r->store->model,
						  work->predecessor_id),
			&predecessor_released);
	if ((!work->predecessor_present && kind != 1) ||
	    (work->predecessor_present && !predecessor_retained && kind != 2) ||
	    (predecessor_retained && (!predecessor_released || kind != 3)))
		return false;
	env.object = *object;
	env.admission = d1_fixture_admission_handle(r->store->model,
						     work->admission_id);
	env.incarnation = d1_store_incarnation(r->store->model);
	env.key.sequence = id;
	env.op = D1_OP_ROLLBACK_BATCH;
	env.body.rollback.range_begin = index;
	env.body.rollback.range_end = index + 1;
	env.body.rollback.count = 1;
	env.body.rollback.entries[0].index = index;
	env.body.rollback.entries[0].owner = work->owner;
	env.body.rollback.entries[0].txn =
		d1_fixture_txn_handle(r->store->model, work->txn_id);
	env.body.rollback.entries[0].visible_present = true;
	env.body.rollback.entries[0].visible =
		d1_fixture_version_handle(r->store->model, version_id);
	env.body.rollback.entries[0].predecessor_present =
		work->predecessor_present;
	env.body.rollback.entries[0].predecessor =
		d1_fixture_version_handle(r->store->model, work->predecessor_id);
	env.body.rollback.entries[0].custody_present = true;
	env.body.rollback.entries[0].custody =
		d1_fixture_custody_handle(r->store->model, custody_id);
	r->status = d1_store_apply(r->store->model, &env, &result);
	if (r->status != D1_OK || result.count != 1 ||
	    result.entries[0].status != D1_NO_PREDECESSOR ||
	    !result.entries[0].postcond_present ||
	    result.entries[0].postcond.raw != id ||
	    result.entries[0].version.raw != version_id)
		return false;
	r->postcond_pending = true;
	r->postcond_id = id;
	memcpy(r->postcond_file_key, file_key, sizeof(file_key));
	r->postcond_chunk_index = index;
	r->postcond_version_id = version_id;
	r->postcond_custody_id = custody_id;
	return true;
}

static bool d2_bind_pending_admissions(struct d2_replay *r,
				       const struct d1_objkey *object);

static bool d2_replay_admission(struct d2_replay *r,
				const struct d2_entry *entry)
{
	typeof(r->store->admissions[0]) *slot;
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
	if (!d2_bind_pending_admissions(r, object))
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
	slot = d2_admission_slot(r->store, entry->admission.client_id);
	if (!slot || !slot->authority_seen ||
	    memcmp(&slot->auth, &auth, sizeof(auth)))
		return false;
	if (!slot->object_known) {
		d1_admission_id id =
			d1_fixture_admit_full(r->store->model, object, &auth);

		if (id.raw != entry->admission.client_id)
			return false;
		slot->model_bound = true;
	} else if (!slot->model_bound ||
		   !d2_same_object(&slot->object, object)) {
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
				 e->admission.client_id, object, e->chunk_index,
				 &result.entries[0].owner,
				 e->transition,
				 d2_payload_object_bytes(
					 e->payload_content_len),
				 e->predecessor_present,
				 e->predecessor_object_id)) &&
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
	struct d1_result result = { 0 };
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
	if (!d2_work_set_phase(r->store, e->txn_id, e->transition))
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
	    result.entries[0].phase !=
		    (e->status == D1_NO_PREDECESSOR ? D2_COMMITTED :
						      D2_ROLLED_BACK) ||
	    result.entries[0].txn.raw != e->txn_id ||
	    result.entries[0].postcond_present != e->postcond_present ||
	    result.entries[0].postcond.raw != e->postcond_id ||
	    result.entries[0].guard.generation != e->result_guard_generation ||
	    result.entries[0].guard.writer != e->result_guard_writer ||
	    result.entries[0].guard.never_written !=
		    e->result_guard_never_written ||
	    result.eof != e->extent_high_water)
		return false;
	if (!d2_work_set_phase(r->store, e->txn_id, D2_ROLLED_BACK))
		return false;
	return d2_replay_receipt(r, object, e);
}

static bool d2_replay_entry(struct d2_replay *r, const struct d2_wal_header *h,
			    const struct d2_entry *e)
{
	struct d2_payload_object object;
	struct d1_checksum recorded_checksum;
	struct d1_envelope env = { 0 };
	struct d1_result result;
	struct d1_objkey *key;
	struct d1_guard guard = { .never_written = true };
	uint8_t *allocation = NULL, *scratch = NULL;
	uint8_t digest[32];
	bool content_ok, ok = false;

	if (e->transition != D2_PREPARED && e->transition != D2_FINALIZED &&
	    e->transition != D2_COMMITTED && e->transition != D2_ROLLED_BACK &&
	    e->transition != D2_REFUSED && e->transition != D2_ABORTED)
		return false;
	key = d2_replay_object(r, e->file_key);
	if (!key)
		return false;
	if (e->transition == D2_REFUSED &&
	    e->status == D1_NO_PREDECESSOR && e->postcond_present)
		return !e->payload_object_id &&
		       d2_replay_receipt(r, key, e);
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
	r->status = d2_files_payload_read_content(r->files,
					  e->payload_object_offset, &object,
					  &allocation, &content_ok);
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
	recorded_checksum = env.body.write.entries[0].checksum;
	if (!content_ok &&
	    !d1_checksum_compute(recorded_checksum.alg, object.content,
				 object.content_len,
				 &env.body.write.entries[0].checksum))
		goto out;
	scratch = malloc(D1_ENVELOPE_MAX);
	if (!scratch)
		goto out;
	if (!d1_envelope_digest(&env, scratch, D1_ENVELOPE_MAX, digest))
		goto out;
	if (content_ok && memcmp(digest, e->key.request_digest, sizeof(digest))) {
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
	if (!content_ok) {
		if (r->store->damaged_count >= D1_MAX_VERSIONS)
			goto out;
		r->store->damaged[r->store->damaged_count].version_id =
			e->result_visible_object_id;
		r->store->damaged[r->store->damaged_count].checksum =
			recorded_checksum;
		r->store->damaged_count++;
	}
	r->highest_payload_id = object.payload_object_id;
	ok = d2_replay_receipt(r, key, e);
out:
	free(scratch);
	free(allocation);
	return ok;
}

static uint32_t d2_model_next_incarnation(struct d2_store *s);
static void d2_store_key(struct d2_store *s, struct d2_key_block *key);

static uint64_t d2_restart_reap_bytes(const struct d2_store *s)
{
	uint64_t total = 0;
	uint32_t a, i, count;

	for (a = 0; a < D1_MAX_ADMISSIONS; a++) {
		if (!s->admissions[a].used || !s->admissions[a].expired)
			continue;
		count = 0;
		for (i = 0; i < D1_MAX_TXNS; i++)
			if (s->work[i].used && !s->work[i].repair &&
			    s->work[i].admission_id == s->admissions[a].id &&
			    (s->work[i].phase == D2_PREPARED ||
			     s->work[i].phase == D2_FINALIZED))
				count++;
		if (count > D2_MAX_COUNTED_ENTRIES)
			return UINT64_MAX;
		if (count) {
			uint64_t add = 216u + 48u * count;

			if (add > UINT64_MAX - total)
				return UINT64_MAX;
			total += add;
		}
	}
	return total;
}

static uint32_t d2_restart_reap(struct d2_store *s)
{
	struct d1_envelope env;
	struct d1_result result;
	struct d2_wal_header h;
	struct d2_control control;
	struct d1_cursor cursor;
	typeof(s->work[0]) *items[D2_MAX_COUNTED_ENTRIES], *swap;
	const typeof(s->admissions[0]) *admission;
	uint8_t handle[32], file_key[32], record[D2_MAX_RECORD_BYTES];
	uint64_t released, promised;
	size_t written;
	uint32_t a, base, count, i, j, n, status;

	for (a = 0; a < D1_MAX_ADMISSIONS; a++) {
		admission = &s->admissions[a];
		if (!admission->used || !admission->expired)
			continue;
		count = 0;
		for (i = 0; i < D1_MAX_TXNS; i++) {
			if (!s->work[i].used || s->work[i].repair ||
			    s->work[i].admission_id != admission->id ||
			    (s->work[i].phase != D2_PREPARED &&
			     s->work[i].phase != D2_FINALIZED))
				continue;
			if (count == D2_MAX_COUNTED_ENTRIES)
				return D1_NOSPC;
			items[count++] = &s->work[i];
		}
		if (!count)
			continue;
		for (i = 1; i < count; i++)
			for (j = i; j && items[j - 1]->txn_id > items[j]->txn_id;
			     j--) {
				swap = items[j - 1];
				items[j - 1] = items[j];
				items[j] = swap;
			}
		memset(&env, 0, sizeof(env));
		memset(&control, 0, sizeof(control));
		memset(&h, 0, sizeof(h));
		env.object = items[0]->object;
		env.admission = d1_fixture_admission_handle(s->model,
							 admission->id);
		env.incarnation = d1_store_incarnation(s->model);
		env.op = D1_OP_LEASE_REAP;
		env.body.control.old_admission = env.admission;
		h.family = D2_REC_CONTROL;
		memcpy(h.store_uuid, s->uuid.bytes, D1_UUID_BYTES);
		memcpy(h.wal_uuid, d2_files_super(s->files)->wal_uuid,
		       D1_UUID_BYTES);
		h.lsn = d2_files_next_lsn(s->files);
		h.ds_incarnation = d2_files_super(s->files)->ds_incarnation;
		control.subtype = D2_CTL_LEASE_REAP;
		control.transition = D2_COMMITTED;
		control.status = D1_OK;
		d2_store_key(s, &control.key);
		memcpy(control.admission_issuer, admission->auth.issuer.bytes,
		       D1_UUID_BYTES);
		control.admission_authority_epoch =
			admission->auth.authority_epoch;
		control.admission_client_id = admission->id;
		control.body_len = 4u + 48u * count;
		d1_enc_init(&cursor, control.body, control.body_len);
		d1_enc_u32(&cursor, count);
		released = 0;
		for (i = 0; i < count; i++) {
			if (!d2_same_object(&env.object, &items[i]->object))
				return D1_INVALID;
			memcpy(handle, items[i]->object.export_uuid.bytes,
			       D1_UUID_BYTES);
			memcpy(handle + D1_UUID_BYTES,
			       items[i]->object.object_uuid.bytes, D1_UUID_BYTES);
			d2_file_key(handle, sizeof(handle), file_key);
			d1_enc_u64(&cursor, items[i]->txn_id);
			d1_enc_raw(&cursor, file_key, sizeof(file_key));
			d1_enc_u64(&cursor, items[i]->index);
			released += d2_work_promise(items[i]->phase);
		}
		if (!d1_cursor_ok(&cursor) || released > s->wal_promised ||
		    !d2_control_encode(&h, &control, record, sizeof(record),
				       &written))
			return D1_IO;
		for (base = 0; base < count; base += D1_BATCH_ENTRIES_MAX) {
			n = count - base;
			if (n > D1_BATCH_ENTRIES_MAX)
				n = D1_BATCH_ENTRIES_MAX;
			env.key.sequence = (h.ds_incarnation << 32) |
					   control.key.sequence;
			env.key.ordinal = base / D1_BATCH_ENTRIES_MAX;
			env.body.control.count = n;
			for (i = 0; i < n; i++)
				env.body.control.txns[i] =
					d1_fixture_txn_handle(
						s->model,
						items[base + i]->txn_id);
			status = d1_store_apply(s->model, &env, &result);
			if (status != D1_OK || result.count != 1 ||
			    result.entries[0].status != D1_OK)
				return status == D1_OK ? D1_IO : status;
		}
		promised = s->wal_promised - released;
		status = d2_wal_append_floor(s, record, written, promised);
		if (status != D1_OK)
			return status;
		s->wal_promised = promised;
		for (i = 0; i < count; i++)
			items[i]->phase = D2_ROLLED_BACK;
	}
	return D1_OK;
}

static bool d2_replay_start(struct d2_replay *r, const struct d2_wal_header *h,
			    const uint8_t *record)
{
	struct d2_start start;
	uint64_t outstanding, staged;

	if (!d2_start_decode(record, h->total_bytes, h, &start) ||
	    !d2_rebuild_promises(r->store) ||
	    !d2_rebuild_payload_accounting(r->store, &outstanding, &staged) ||
	    start.live_txn_wal_reserved != r->store->wal_promised ||
	    start.live_txn_payload_outstanding != outstanding ||
	    start.live_txn_payload_staged != staged)
		return false;
	d2_open_recovery_window(r->store, D2_RESTART_SWEEP);
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

static uint32_t d2_model_apply_damage(struct d2_store *s)
{
	uint32_t i;

	for (i = 0; i < s->damaged_count; i++)
		if (!d1_fixture_version_damage(
			    s->model,
			    d1_fixture_version_handle(
				    s->model, s->damaged[i].version_id),
			    &s->damaged[i].checksum))
			return D1_IO;
	return D1_OK;
}

static uint32_t d2_model_restore(struct d2_store *s, const uint8_t *journal,
				 size_t journal_len)
{
	struct d1_store *restored;
	uint32_t status;

	restored = d1_store_open(&s->uuid, s->chunk_bytes, s->max_file_bytes);
	if (!restored)
		return D1_NOSPC;
	status = d1_fixture_restore_journal(restored, journal, journal_len);
	if (status != D1_OK) {
		d1_store_free(restored);
		return status;
	}
	d1_store_free(s->model);
	s->model = restored;
	return D1_OK;
}

static bool d2_bind_pending_admissions(struct d2_replay *r,
				       const struct d1_objkey *object)
{
	typeof(r->store->admissions[0]) *next;
	d1_admission_id id;
	uint32_t i;

	for (;;) {
		next = NULL;
		for (i = 0; i < D1_MAX_ADMISSIONS; i++) {
			typeof(r->store->admissions[0]) *slot =
				&r->store->admissions[i];

			if (!slot->used || !slot->authority_seen ||
			    slot->model_bound)
				continue;
			if (!next || slot->id < next->id)
				next = slot;
		}
		if (!next)
			return true;
		id = d1_fixture_admit_full(r->store->model, object,
					   &next->auth);
		if (id.raw != next->id)
			return false;
		next->object = *object;
		next->object_known = true;
		next->model_bound = true;
		if (next->revoked)
			d1_fixture_revoke(r->store->model, id);
		if (next->expired)
			d1_fixture_expire(r->store->model, id);
	}
}

static bool d2_recovery_fundable(const struct d2_store *s,
				 const struct d1_envelope *env,
				 uint64_t record_bytes);

static bool d2_replay_recovery(struct d2_replay *r,
			       const struct d2_wal_header *h,
			       const struct d2_control *control)
{
	typeof(r->store->admissions[0]) *fresh;
	typeof(r->store->work[0]) *work[D1_BATCH_ENTRIES_MAX];
	struct d1_envelope env = { 0 };
	struct d1_result result;
	struct d1_cursor cursor;
	uint8_t digest[D1_DIGEST_BYTES], other[12];
	uint64_t lease_epoch, read_epoch = 0;
	uint32_t count, seqid, expected_seqid, i;
	bool funded, matched = false;

	if (control->subtype != D2_CTL_RECOVERY_ADMIT)
		return true;
	fresh = d2_admission_slot(r->store, control->admission_client_id);
	d2_stateid(control->admission_client_id, &expected_seqid, digest);
	d1_dec_init(&cursor, control->body, control->body_len);
	if (!fresh || !fresh->authority_seen ||
	    !d1_dec_u32(&cursor, &count) || !count ||
	    count > D1_BATCH_ENTRIES_MAX)
		return false;
	for (i = 0; i < count; i++) {
		uint64_t txn_id, entry_read_epoch;

		if (!d1_dec_u64(&cursor, &txn_id) ||
		    !d1_dec_u32(&cursor, &seqid) ||
		    !d1_dec_raw(&cursor, other, sizeof(other)) ||
		    !d1_dec_u64(&cursor, &lease_epoch) ||
		    !d1_dec_u64(&cursor, &entry_read_epoch) ||
		    seqid != expected_seqid ||
		    memcmp(other, digest, sizeof(other)) ||
		    lease_epoch != fresh->auth.lease_epoch)
			return false;
		work[i] = (typeof(r->store->work[0]) *)d2_work_by_txn(
			r->store, txn_id);
		if (!work[i] ||
		    d2_object_tombstoned(r->store, &work[i]->object) ||
		    (i && txn_id <= work[i - 1]->txn_id) ||
		    (i && !d2_same_object(&work[0]->object, &work[i]->object)) ||
		    (i && work[0]->admission_id != work[i]->admission_id) ||
		    (i && read_epoch != entry_read_epoch))
			return false;
		read_epoch = entry_read_epoch;
	}
	if (!d1_dec_finished(&cursor) ||
	    memcmp(control->admission_issuer, fresh->auth.issuer.bytes,
		   D1_UUID_BYTES) ||
	    control->admission_authority_epoch != fresh->auth.authority_epoch ||
	    !d2_bind_pending_admissions(r, &work[0]->object))
		return false;
	env.object = work[0]->object;
	env.incarnation = h->ds_incarnation;
	memcpy(env.key.origin.bytes, control->key.session, D1_UUID_BYTES);
	env.key.sequence = ((uint64_t)control->key.slot << 32) |
			   control->key.sequence;
	env.key.ordinal = control->key.compound_ordinal;
	env.op = D1_OP_RECOVERY_ADMIT;
	env.body.control.count = count;
	for (i = 0; i < count; i++)
		env.body.control.txns[i] = d1_fixture_txn_handle(
			r->store->model, work[i]->txn_id);
	env.body.control.old_admission = d1_fixture_admission_handle(
		r->store->model, work[0]->admission_id);
	env.body.control.new_admission_present = true;
	env.body.control.new_admission = d1_fixture_admission_handle(
		r->store->model, control->admission_client_id);
	env.body.control.read_epoch_present = true;
	env.body.control.read_epoch = read_epoch;
	if (control->transition == D2_REFUSED) {
		memset(&result, 0, sizeof(result));
		result.key = env.key;
		result.count = 1;
		result.disposition = D1_COMPLETED;
		result.entries[0].status = control->status;
		result.entries[0].disposition = D1_COMPLETED;
		return d2_receipt_remember(r->store,
					   &work[0]->object.export_uuid,
					   &env.key,
					   control->key.request_digest,
					   &result);
	}
	for (i = 0; i < D1_MAX_ADMISSIONS; i++) {
		typeof(r->store->admissions[0]) *slot =
			&r->store->admissions[i];

		if (!slot->used || !slot->model_bound || !slot->object_known ||
		    !d2_same_object(&slot->object, &work[0]->object))
			continue;
		env.admission =
			d1_fixture_admission_handle(r->store->model, slot->id);
		if (d2_env_digest(&env, digest) &&
		    !memcmp(digest, control->key.request_digest,
			    D1_DIGEST_BYTES)) {
			matched = true;
			break;
		}
	}
	if (!matched)
		return false;
	r->status = d1_store_apply(r->store->model, &env, &result);
	if (r->status != D1_OK || result.count != 1 ||
	    result.entries[0].status != control->status ||
	    ((control->transition == D2_COMMITTED) !=
	     (control->status == D1_OK)))
		return false;
	if (control->status == D1_OK &&
	    !d2_recovery_rebind(r->store, &env))
		return false;
	funded = control->status == D1_OK &&
		 d2_recovery_fundable(r->store, &env, h->total_bytes);
	if (funded) {
		if (h->total_bytes > r->store->recovery_allowance)
			return false;
		r->store->recovery_allowance -= h->total_bytes;
		for (i = 0; i < count; i++)
			work[i]->recovery_admitted = true;
		d2_close_recovery_window_if_complete(r->store);
	}
	return d2_receipt_remember(r->store, &work[0]->object.export_uuid,
				   &env.key, control->key.request_digest,
				   &result);
}

static bool d2_replay_episode_mark(struct d2_replay *r,
				   const struct d2_wal_header *h,
				   const struct d2_control *control)
{
	struct d2_entry admission_entry = { 0 };
	struct d1_envelope env = { 0 };
	struct d1_result result = { 0 };
	struct d1_cursor cursor;
	struct d1_objkey *object;
	const struct d1_fixture_authority *auth;
	uint8_t episode_uuid[16], file_key[32];
	uint64_t episode_id, index, version_id, custody_id;
	uint32_t count, i;
	bool committed;

	if (control->subtype != D2_CTL_EPISODE_MARK)
		return true;
	committed = control->transition == D2_COMMITTED &&
		    control->status == D1_OK;
	if (!committed && (control->transition != D2_REFUSED ||
			   control->status == D1_OK))
		return false;
	d1_dec_init(&cursor, control->body, control->body_len);
	if (!d1_dec_raw(&cursor, episode_uuid, sizeof(episode_uuid)) ||
	    !d1_dec_raw(&cursor, file_key, sizeof(file_key)) ||
	    !d1_dec_u64(&cursor, &env.body.repair.range_begin) ||
	    !d1_dec_u64(&cursor, &env.body.repair.range_end) ||
	    !d1_dec_u32(&cursor, &count) || count == 0 ||
	    count > D1_BATCH_ENTRIES_MAX ||
	    !d2_episode_id(episode_uuid, &episode_id))
		return false;
	object = d2_replay_object(r, file_key);
	if (!object || d2_file_tombstoned(r->store, file_key) ||
	    (committed && d2_episode_find(r->store, episode_id)))
		return false;
	auth = d2_admission_find(r->store, control->admission_client_id);
	if (!auth ||
	    memcmp(control->admission_issuer, auth->issuer.bytes,
		   D1_UUID_BYTES) ||
	    control->admission_authority_epoch != auth->authority_epoch)
		return false;
	admission_entry.admission.client_id = control->admission_client_id;
	d2_admission_encode(r->store, control->admission_client_id,
			    &admission_entry.admission);
	memcpy(admission_entry.file_key, file_key, sizeof(file_key));
	if (!d2_replay_admission(r, &admission_entry))
		return false;
	env.object = *object;
	env.admission = d1_fixture_admission_handle(
		r->store->model, control->admission_client_id);
	env.incarnation = h->ds_incarnation;
	memcpy(env.key.origin.bytes, control->key.session, D1_UUID_BYTES);
	env.key.sequence = ((uint64_t)control->key.slot << 32) |
			   control->key.sequence;
	env.key.ordinal = control->key.compound_ordinal;
	env.op = D1_OP_MARK_ERROR;
	env.body.repair.count = count;
	for (i = 0; i < count; i++) {
		struct d1_repair_entry *entry = &env.body.repair.entries[i];
		bool predecessor_present;
		d1_version_id version, predecessor;

		if (!d1_dec_u64(&cursor, &index) ||
		    !d1_dec_u64(&cursor, &version_id) ||
		    !d1_dec_u64(&cursor, &custody_id))
			return false;
		entry->index = index;
		entry->owner.cohort.raw = episode_id;
		entry->owner.writer = auth->writer;
		entry->owner.co_id = i + 1;
		entry->custody_present = true;
		entry->custody = d1_fixture_custody_handle(r->store->model,
							 custody_id);
		entry->successor_present = true;
		entry->successor = version = d1_fixture_version_handle(
			r->store->model, version_id);
		if (committed &&
		    !d1_fixture_version_predecessor(r->store->model, version,
						     &predecessor_present,
						     &predecessor))
			return false;
		if (committed) {
			entry->predecessor_present = predecessor_present;
			entry->predecessor = predecessor;
		}
	}
	if (!d1_dec_finished(&cursor))
		return false;
	if (!committed) {
		memset(&result, 0, sizeof(result));
		result.key = env.key;
		result.count = 1;
		result.disposition = D1_COMPLETED;
		result.eof = d1_store_eof(r->store->model, &env.object);
		result.entries[0].status = control->status;
		result.entries[0].disposition = D1_COMPLETED;
		result.entries[0].stability = D1_FILE_SYNC;
		d1_store_verifier(r->store->model,
				  result.entries[0].verifier);
		return d2_receipt_remember(r->store,
			&env.object.export_uuid, &env.key,
			control->key.request_digest, &result);
	}
	r->status = d1_store_apply(r->store->model, &env, &result);
	if (r->status != D1_OK || result.count != 1 ||
	    result.entries[0].status != D1_OK ||
	    !result.entries[0].episode_present ||
	    result.entries[0].episode.raw != episode_id ||
	    !d2_episode_remember(r->store, &env, &result))
		return false;
	return d2_receipt_remember(r->store, &env.object.export_uuid, &env.key,
				   control->key.request_digest, &result);
}

static bool d2_replay_certificate(struct d2_replay *r,
				  const struct d2_control *control)
{
	typeof(r->store->cohorts[0]) *cohort;
	const struct d1_fixture_authority *auth;
	struct d1_cursor cursor;
	uint8_t episode_uuid[D1_UUID_BYTES];
	uint8_t certificate[D1_CERTIFICATE_BYTES];
	uint64_t episode_id, cohort_id;

	if (control->subtype != D2_CTL_CERTIFICATE_INSTALL)
		return true;
	if (control->transition != D2_COMMITTED || control->status != D1_OK)
		return false;
	d1_dec_init(&cursor, control->body, control->body_len);
	if (!d1_dec_raw(&cursor, episode_uuid, sizeof(episode_uuid)) ||
	    !d1_dec_u64(&cursor, &cohort_id) ||
	    !d1_dec_raw(&cursor, certificate, sizeof(certificate)) ||
	    !d1_dec_finished(&cursor) ||
	    !d2_episode_id(episode_uuid, &episode_id) ||
	    d2_certificate_find(r->store, episode_id, cohort_id))
		return false;
	cohort = d2_cohort_find(r->store, cohort_id);
	if (!cohort || !cohort->count ||
	    d2_object_tombstoned(r->store, &cohort->members[0].object) ||
	    cohort->phase != D2_COMMITTED ||
	    !cohort->episode_present || cohort->episode_id != episode_id ||
	    cohort->admission_id != control->admission_client_id)
		return false;
	auth = d2_admission_find(r->store, cohort->admission_id);
	if (!auth ||
	    memcmp(control->admission_issuer, auth->issuer.bytes,
		   D1_UUID_BYTES) ||
	    control->admission_authority_epoch != auth->authority_epoch)
		return false;
	d1_fixture_certificate(r->store->model, certificate);
	if (!d2_certificate_remember(r->store, episode_id, cohort_id,
				    certificate))
		return false;
	cohort->certificate_installed = true;
	return true;
}

static bool d2_replay_episode_clear(struct d2_replay *r,
				    const struct d2_wal_header *h,
				    const struct d2_control *control)
{
	typeof(r->store->cohorts[0]) *cohort;
	typeof(r->store->certificates[0]) *installed;
	struct d2_entry admission_entry = { 0 };
	struct d1_envelope env = { 0 };
	struct d1_result result = { 0 };
	struct d1_cursor cursor;
	const struct d1_fixture_authority *auth;
	uint8_t episode_uuid[D1_UUID_BYTES], file_key[32];
	uint8_t certificate[D1_CERTIFICATE_BYTES];
	uint64_t episode_id, cohort_id, index;
	uint32_t reason, count, i;
	bool committed;

	if (control->subtype != D2_CTL_EPISODE_CLEAR)
		return true;
	committed = control->transition == D2_COMMITTED &&
		    control->status == D1_OK;
	if (!committed && (control->transition != D2_REFUSED ||
			   control->status == D1_OK))
		return false;
	d1_dec_init(&cursor, control->body, control->body_len);
	if (!d1_dec_raw(&cursor, episode_uuid, sizeof(episode_uuid)) ||
	    !d1_dec_u64(&cursor, &cohort_id) ||
	    !d1_dec_u32(&cursor, &reason) || !reason ||
	    !d1_dec_u32(&cursor, &count) || count == 0 ||
	    count > D1_BATCH_ENTRIES_MAX ||
	    !d1_dec_raw(&cursor, certificate, sizeof(certificate)) ||
	    !d2_episode_id(episode_uuid, &episode_id))
		return false;
	cohort = d2_cohort_find(r->store, cohort_id);
	installed = d2_certificate_find(r->store, episode_id, cohort_id);
	if (committed &&
	    (!cohort || cohort->phase != D2_COMMITTED ||
	     !cohort->episode_present || cohort->episode_id != episode_id ||
	     cohort->count != count || !installed ||
	     memcmp(installed->value, certificate, sizeof(certificate))))
		return false;
	auth = d2_admission_find(r->store, control->admission_client_id);
	if (!auth ||
	    memcmp(control->admission_issuer, auth->issuer.bytes,
		   D1_UUID_BYTES) ||
	    control->admission_authority_epoch != auth->authority_epoch)
		return false;
	env.body.repair.range_begin = UINT64_MAX;
	for (i = 0; i < count; i++) {
		struct d1_repair_entry *entry = &env.body.repair.entries[i];
		const typeof(cohort->members[0]) *member =
			cohort && i < cohort->count ? &cohort->members[i] : NULL;
		struct d1_objkey *object;

		if (!d1_dec_raw(&cursor, file_key, sizeof(file_key)) ||
		    !d1_dec_u64(&cursor, &index) ||
		    (committed && (!member || index != member->index)))
			return false;
		object = d2_replay_object(r, file_key);
		if (!object || d2_file_tombstoned(r->store, file_key) ||
		    (committed && !d2_same_object(object, &member->object)) ||
		    (i && !d2_same_object(object, &env.object)))
			return false;
		if (!i)
			env.object = *object;
		if (index < env.body.repair.range_begin)
			env.body.repair.range_begin = index;
		if (index + 1 > env.body.repair.range_end)
			env.body.repair.range_end = index + 1;
		entry->index = index;
		if (member)
			entry->owner = member->owner;
		else {
			entry->owner.cohort.raw = cohort_id;
			entry->owner.writer = auth->writer;
			entry->owner.co_id = i + 1;
		}
		entry->custody_present = true;
		entry->custody = d1_fixture_custody_handle(
			r->store->model, member ? member->custody_id : 1);
	}
	if (!d1_dec_finished(&cursor))
		return false;
	d2_admission_encode(r->store, control->admission_client_id,
			    &admission_entry.admission);
	memcpy(admission_entry.file_key, file_key, sizeof(file_key));
	if (!d2_replay_admission(r, &admission_entry))
		return false;
	env.admission = d1_fixture_admission_handle(
		r->store->model, control->admission_client_id);
	env.incarnation = h->ds_incarnation;
	memcpy(env.key.origin.bytes, control->key.session, D1_UUID_BYTES);
	env.key.sequence = ((uint64_t)control->key.slot << 32) |
			   control->key.sequence;
	env.key.ordinal = control->key.compound_ordinal;
	env.op = D1_OP_CLEAR_ERROR;
	env.body.repair.count = count;
	env.body.repair.cohort_present = true;
	env.body.repair.cohort = d1_fixture_repair_handle(r->store->model,
							 cohort_id);
	env.body.repair.episode_present = true;
	env.body.repair.episode = d1_fixture_episode_handle(r->store->model,
							 episode_id);
	env.body.repair.certificate_present = true;
	memcpy(env.body.repair.certificate, certificate, sizeof(certificate));
	r->status = d1_store_apply(r->store->model, &env, &result);
	if (r->status != D1_OK || result.count != 1 ||
	    result.entries[0].status != control->status)
		return false;
	if (committed)
		cohort->episode_cleared = true;
	return d2_receipt_remember(r->store, &env.object.export_uuid, &env.key,
				   control->key.request_digest, &result);
}

static bool d2_replay_cohort_refusal(struct d2_replay *r,
				     const struct d2_cohort *disk)
{
	struct d2_entry admission_entry = { 0 };
	struct d1_result result = { 0 };
	struct d1_opkey key = { 0 };
	struct d1_objkey *object = NULL;
	typeof(r->store->cohorts[0]) *cohort;
	uint32_t i;

	if (disk->transition != D2_REFUSED || disk->status == D1_OK ||
	    disk->member_count == 0)
		return false;
	for (i = 0; i < disk->member_count; i++) {
		struct d1_objkey *member =
			d2_replay_object(r, disk->members[i].file_key);

		if (!member || (object && !d2_same_object(object, member)) ||
		    disk->members[i].payload_object_id ||
		    disk->members[i].staged)
			return false;
		object = member;
	}
	if (!object)
		return false;
	admission_entry.admission = disk->admission;
	memcpy(admission_entry.file_key, disk->members[0].file_key,
	       sizeof(admission_entry.file_key));
	if (!d2_replay_admission(r, &admission_entry))
		return false;
	memcpy(key.origin.bytes, disk->key.session, D1_UUID_BYTES);
	key.sequence = ((uint64_t)disk->key.slot << 32) |
		       disk->key.sequence;
	key.ordinal = disk->key.compound_ordinal;
	result.key = key;
	result.count = 1;
	result.disposition = disk->disposition;
	result.index_epoch = disk->index_generation;
	result.entries[0].status = disk->status;
	result.entries[0].disposition = disk->disposition;
	result.entries[0].stability = D1_FILE_SYNC;
	memcpy(result.entries[0].verifier, disk->result_verifier,
	       D1_VERIFIER_BYTES);
	cohort = d2_cohort_find(r->store, disk->cohort_id);
	if (cohort) {
		result.entries[0].cohort_present = true;
		result.entries[0].cohort = d1_fixture_repair_handle(
			r->store->model, cohort->id);
		result.entries[0].phase = cohort->phase;
	}
	for (i = 0; i < disk->member_count; i++)
		if (disk->members[i].member_status == disk->status) {
			result.entries[0].member_present = true;
			result.entries[0].member = i;
			result.entries[0].guard.never_written =
				disk->members[i].result_guard_never_written;
			result.entries[0].guard.generation =
				disk->members[i].result_guard_generation;
			result.entries[0].guard.writer =
				disk->members[i].result_guard_writer;
			break;
		}
	result.eof = disk->members[0].extent_high_water;
	return d2_receipt_remember(r->store, &object->export_uuid, &key,
				   disk->key.request_digest, &result);
}

static bool d2_replay_cohort(struct d2_replay *r, const struct d2_wal_header *h,
			     const struct d2_cohort *disk)
{
	typeof(r->store->cohorts[0]) *saved = NULL;
	struct d1_envelope env = { 0 };
	struct d1_result result;
	struct d2_entry admission_entry = { 0 };
	struct d2_payload_object payloads[D1_BATCH_ENTRIES_MAX];
	struct d1_checksum recorded_checksum[D1_BATCH_ENTRIES_MAX];
	uint8_t *allocations[D1_BATCH_ENTRIES_MAX] = { 0 };
	uint8_t digest[D1_DIGEST_BYTES];
	struct d1_objkey *first_object;
	uint64_t prior_eof;
	uint32_t expected_extent, i, op;
	bool content_ok[D1_BATCH_ENTRIES_MAX] = { 0 };
	bool damaged = false, kept_abort, ok = false;

	if (disk->transition == D2_REFUSED)
		return d2_replay_cohort_refusal(r, disk);
	kept_abort = disk->transition == D2_ABORTED &&
		     disk->status != D1_OK && (disk->flags & 2u);
	if ((!kept_abort && disk->status != D1_OK) ||
	    disk->member_count == 0)
		return false;
	if (disk->transition == D2_ADMITTED)
		op = D1_OP_BEGIN_REPAIR;
	else if (disk->transition == D2_PREPARED)
		op = D1_OP_PREPARE_REPAIR;
	else if (disk->transition == D2_FINALIZED)
		op = D1_OP_FINALIZE_REPAIR;
	else if (disk->transition == D2_COMMITTED)
		op = D1_OP_COMMIT_REPAIR;
	else if (disk->transition == D2_ABORTED)
		op = D1_OP_ABORT_REPAIR;
	else if (disk->transition == D2_UNLOCKED)
		op = D1_OP_UNLOCK;
	else
		return false;
	admission_entry.admission = disk->admission;
	memcpy(admission_entry.file_key, disk->members[0].file_key,
	       sizeof(admission_entry.file_key));
	if (!d2_replay_admission(r, &admission_entry))
		return false;
	first_object = d2_replay_object(r, disk->members[0].file_key);
	if (!first_object)
		return false;
	env.object = *first_object;
	prior_eof = d1_store_eof(r->store->model, &env.object);
	env.admission = d1_fixture_admission_handle(r->store->model,
						    disk->admission.client_id);
	env.incarnation = h->ds_incarnation;
	memcpy(env.key.origin.bytes, disk->key.session, D1_UUID_BYTES);
	env.key.sequence = ((uint64_t)disk->key.slot << 32) |
			   disk->key.sequence;
	env.key.ordinal = disk->key.compound_ordinal;
	env.op = op;
	env.body.repair.count = disk->member_count;
	env.body.repair.range_begin = UINT64_MAX;
	if (op == D1_OP_BEGIN_REPAIR && (disk->flags & 1u)) {
		uint64_t episode_id;

		if (!d2_episode_id(disk->episode_uuid, &episode_id) ||
		    !d2_episode_find(r->store, episode_id))
			goto out;
		env.body.repair.episode_present = true;
		env.body.repair.episode = d1_fixture_episode_handle(
			r->store->model, episode_id);
	}
	if (op != D1_OP_BEGIN_REPAIR) {
		saved = d2_cohort_find(r->store, disk->cohort_id);
		if (!saved || saved->count != disk->member_count)
			goto out;
		env.body.repair.cohort_present = true;
		env.body.repair.cohort = d1_fixture_repair_handle(
			r->store->model, disk->cohort_id);
	}
	for (i = 0; i < disk->member_count; i++) {
		const struct d2_cohort_member *member = &disk->members[i];
		struct d1_repair_entry *entry = &env.body.repair.entries[i];
		struct d1_objkey *object =
			d2_replay_object(r, member->file_key);

		if (!object || !d2_same_object(object, &env.object))
			goto out;
		if (member->chunk_index < env.body.repair.range_begin)
			env.body.repair.range_begin = member->chunk_index;
		if (member->chunk_index + 1 > env.body.repair.range_end)
			env.body.repair.range_end = member->chunk_index + 1;
		entry->index = member->chunk_index;
		entry->owner.cohort.raw = member->owner_cohort;
		entry->owner.writer = member->owner_client_id;
		entry->owner.co_id = member->owner_co_id;
		if (op == D1_OP_BEGIN_REPAIR) {
			entry->mode = member->repair_mode;
			entry->custody_present = true;
			entry->custody = d1_fixture_custody_handle(
				r->store->model, member->custody_id);
			entry->postcond_present = member->postcond_present;
			entry->postcond = d1_fixture_postcond_handle(
				r->store->model, member->postcond_id);
			entry->successor_present = true;
			entry->successor = d1_fixture_version_handle(
				r->store->model, member->successor_object_id);
			entry->predecessor_present =
				member->predecessor_present;
			entry->predecessor = d1_fixture_version_handle(
				r->store->model, member->predecessor_object_id);
		} else if (op == D1_OP_PREPARE_REPAIR) {
			if (!member->payload_object_id ||
			    member->payload_object_id <= r->highest_payload_id)
				goto out;
			r->status = d2_files_payload_read_content(
				r->files, member->payload_object_offset,
				&payloads[i], &allocations[i], &content_ok[i]);
			if (r->status != D1_OK ||
			    payloads[i].payload_object_id !=
				    member->payload_object_id ||
			    payloads[i].content_len !=
				    member->payload_content_len)
				goto out;
			r->highest_payload_id = member->payload_object_id;
			entry->txn_present = true;
			entry->txn = d1_fixture_txn_handle(
				r->store->model, member->member_txn_id);
			entry->payload_present = true;
			entry->payload = payloads[i].content;
			entry->payload_len = payloads[i].content_len;
			entry->checksum.alg = payloads[i].content_alg;
			entry->checksum.len = payloads[i].content_ck_len;
			memcpy(entry->checksum.digest, payloads[i].content_ck,
			       payloads[i].content_ck_len);
			recorded_checksum[i] = entry->checksum;
			if (!content_ok[i]) {
				damaged = true;
				if (!d1_checksum_compute(
					    recorded_checksum[i].alg,
					    payloads[i].content,
					    payloads[i].content_len,
					    &entry->checksum))
					goto out;
			}
		} else {
			entry->custody_present = true;
			entry->custody = d1_fixture_custody_handle(
				r->store->model, member->custody_id);
			if (op == D1_OP_FINALIZE_REPAIR ||
			    op == D1_OP_COMMIT_REPAIR) {
				entry->txn_present = true;
				entry->txn = d1_fixture_txn_handle(
					r->store->model, member->member_txn_id);
				entry->predecessor_present = true;
				entry->predecessor = d1_fixture_version_handle(
					r->store->model,
					member->successor_object_id);
			}
		}
	}
	if (op == D1_OP_ABORT_REPAIR) {
		env.body.repair.phase_present = true;
		env.body.repair.phase = saved->phase;
	}
	if (op == D1_OP_FINALIZE_REPAIR || op == D1_OP_COMMIT_REPAIR) {
		env.body.repair.verifier_present = true;
		d1_store_verifier(r->store->model,
				  env.body.repair.prior_verifier);
	}
	if (!kept_abort && !damaged &&
	    (!d2_env_digest(&env, digest) ||
	     memcmp(digest, disk->key.request_digest, D1_DIGEST_BYTES)))
		goto out;
	r->status = d1_store_apply(r->store->model, &env, &result);
	if (r->status != D1_OK || result.count != 1 ||
	    result.entries[0].status != D1_OK ||
	    memcmp(result.entries[0].verifier, disk->result_verifier,
		   D1_VERIFIER_BYTES))
		goto out;
	if (op == D1_OP_PREPARE_REPAIR)
		for (i = 0; i < disk->member_count; i++) {
			d1_txn_id txn;
			d1_version_id version;

			if (!d1_fixture_repair_member(
				    r->store->model,
				    d1_fixture_repair_handle(r->store->model,
						     disk->cohort_id),
				    i, &txn, &version) ||
			    txn.raw != disk->members[i].member_txn_id ||
			    !version.raw ||
			    !d2_work_set_version(r->store, txn.raw, version.raw))
				goto out;
			if (!content_ok[i]) {
				if (r->store->damaged_count >= D1_MAX_VERSIONS)
					goto out;
				r->store->damaged[r->store->damaged_count]
					.version_id = version.raw;
				r->store->damaged[r->store->damaged_count]
					.checksum = recorded_checksum[i];
				r->store->damaged_count++;
			}
		}
	expected_extent = result.eof > prior_eof ? D2_EXTENT_EXTEND :
			  result.eof < prior_eof ? D2_EXTENT_SHRINK :
						 D2_EXTENT_UNCHANGED;
	for (i = 0; i < disk->member_count; i++)
		if (disk->members[i].extent_kind != expected_extent ||
		    disk->members[i].extent_high_water != result.eof ||
		    disk->members[i].extent_highest_index !=
			    (result.eof ? (result.eof - 1) /
						 r->store->chunk_bytes :
					  0))
			goto out;
	if (op == D1_OP_BEGIN_REPAIR &&
	    (!result.entries[0].cohort_present ||
	     result.entries[0].cohort.raw != disk->cohort_id ||
	     result.entries[0].member_txn_count != disk->member_count))
		goto out;
	if (op != D1_OP_BEGIN_REPAIR && op != D1_OP_UNLOCK &&
	    result.entries[0].phase != disk->transition)
		goto out;
	if (kept_abort) {
		result.entries[0].status = disk->status;
		result.entries[0].disposition = disk->disposition;
		for (i = 0; i < disk->member_count; i++)
			if (disk->members[i].member_status == disk->status) {
				result.entries[0].member_present = true;
				result.entries[0].member = i;
				result.entries[0].guard.never_written =
					disk->members[i]
						.result_guard_never_written;
				result.entries[0].guard.generation =
					disk->members[i]
						.result_guard_generation;
				result.entries[0].guard.writer =
					disk->members[i].result_guard_writer;
				break;
			}
	}
	if (op == D1_OP_BEGIN_REPAIR) {
		if (result.entries[0].cohort.raw != disk->cohort_id ||
		    !d2_cohort_remember_begin(r->store, &env, &result))
			goto out;
		saved = d2_cohort_find(r->store, disk->cohort_id);
	} else if (op == D1_OP_PREPARE_REPAIR) {
		for (i = 0; i < disk->member_count; i++) {
			saved->members[i].payload_id =
				disk->members[i].payload_object_id;
			saved->members[i].payload_offset =
				disk->members[i].payload_object_offset;
			saved->members[i].payload_len =
				disk->members[i].payload_content_len;
			saved->members[i].payload_ck_alg =
				disk->members[i].result_ck_alg;
			saved->members[i].payload_ck_len =
				disk->members[i].result_ck_len;
			memcpy(saved->members[i].payload_ck,
			       disk->members[i].result_ck,
			       disk->members[i].result_ck_len);
		}
	}
	if (op != D1_OP_UNLOCK) {
		saved->phase = disk->transition;
		if (!d2_repair_work_set_phase(r->store, saved->id,
					      disk->transition))
			goto out;
	} else if (!d2_repair_work_set_phase(r->store, saved->id,
					       D2_UNLOCKED))
		goto out;
	ok = d2_receipt_remember(r->store, &env.object.export_uuid, &env.key,
				 disk->key.request_digest, &result);
out:
	for (i = 0; i < D1_BATCH_ENTRIES_MAX; i++)
		free(allocations[i]);
	return ok;
}

static bool d2_replay_record(const struct d2_wal_header *h,
			     const uint8_t *record, void *arg)
{
	struct d2_replay *r = arg;
	struct d2_control control;
	struct d2_cohort cohort;
	struct d2_entry e;
	const typeof(r->store->work[0]) *paired_work;
	bool paired, replayed;

	if (r->store->retired)
		return false;
	if (r->postcond_pending && h->family == D2_REC_START) {
		r->postcond_pending = false;
	} else if (r->postcond_pending && h->family != D2_REC_ENTRY) {
		return false;
	}
	if (h->family == D2_REC_START)
		return d2_replay_start(r, h, record);
	if (h->family == D2_REC_CONTROL) {
		struct d1_cursor cursor;
		uint8_t file_key[32];
		uint64_t final_lsn;
		uint32_t reason;

		if (!d2_control_decode(record, h->total_bytes, h, &control))
			return false;
		if (control.subtype == D2_CTL_FILE_TOMBSTONE) {
			d1_dec_init(&cursor, control.body, control.body_len);
			if (control.transition != D2_COMMITTED ||
			    control.status != D1_OK ||
			    memcmp(control.admission_issuer,
				   (uint8_t[D1_UUID_BYTES]){ 0 }, D1_UUID_BYTES) ||
			    control.admission_authority_epoch ||
			    control.admission_client_id ||
			    !d1_dec_raw(&cursor, file_key, sizeof(file_key)) ||
			    !d1_dec_u32(&cursor, &reason) || !reason ||
			    !d1_dec_finished(&cursor) ||
			    !d2_tombstone_remember(r->store, file_key))
				return false;
			return true;
		}
		if (control.subtype == D2_CTL_FILE_REGISTER &&
		    d2_file_tombstoned(r->store, control.body))
			return false;
		if (control.subtype == D2_CTL_EXPORT_TOMBSTONE) {
			d1_dec_init(&cursor, control.body, control.body_len);
			if (control.transition != D2_COMMITTED ||
			    control.status != D1_OK ||
			    memcmp(control.admission_issuer,
				   (uint8_t[D1_UUID_BYTES]){ 0 }, D1_UUID_BYTES) ||
			    control.admission_authority_epoch ||
			    control.admission_client_id ||
			    !d1_dec_u32(&cursor, &reason) || !reason ||
			    !d1_dec_u64(&cursor, &final_lsn) ||
			    !d1_dec_finished(&cursor) || final_lsn != h->lsn)
				return false;
			r->store->retired = true;
			return true;
		}
		return d2_replay_registration(r, &control) &&
		       d2_replay_trust(r, h, &control) &&
		       d2_replay_authority(r, h, &control) &&
		       d2_replay_recovery(r, h, &control) &&
		       d2_replay_episode_mark(r, h, &control) &&
		       d2_replay_certificate(r, &control) &&
		       d2_replay_episode_clear(r, h, &control) &&
		       d2_replay_liveness(r, &control) &&
		       d2_replay_reap(r, h, &control) &&
		       d2_replay_custody(r, &control) &&
		       d2_replay_postcond(r, &control);
	}
	if (h->family == D2_REC_COHORT) {
		uint32_t i;

		if (!d2_cohort_decode(record, h->total_bytes, h, &cohort))
			return false;
		for (i = 0; i < cohort.member_count; i++)
			if (d2_file_tombstoned(r->store,
					       cohort.members[i].file_key))
				return false;
		return d2_replay_cohort(r, h, &cohort);
	}
	if (h->family != D2_REC_ENTRY)
		return true;
	if (!d2_entry_decode(record, h->total_bytes, h, &e))
		return false;
	if (d2_file_tombstoned(r->store, e.file_key))
		return false;
	paired = e.status == D1_NO_PREDECESSOR && e.postcond_present &&
		 e.postcond_id != 0;
	paired_work = paired ? d2_work_by_txn(r->store, e.txn_id) : NULL;
	if (paired != r->postcond_pending ||
	    (paired &&
	     (!paired_work || e.postcond_id != r->postcond_id ||
	      memcmp(e.file_key, r->postcond_file_key, sizeof(e.file_key)) ||
	      e.chunk_index != r->postcond_chunk_index ||
	      paired_work->version_id != r->postcond_version_id)))
		return false;
	if (!d2_replay_admission(r, &e))
		return false;
	replayed = d2_replay_entry(r, h, &e);
	if (replayed && paired)
		r->postcond_pending = false;
	return replayed;
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
	status = d2_wal_append(s, record, written);
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
	uint64_t payload_outstanding, payload_staged, reap_bytes, required;
	uint32_t decision, status;

	if (!config || !binding || !out ||
	    !d2_geometry(config->chunk_bytes, config->max_file_bytes))
		return D1_INVALID;
	*out = NULL;
	status = d2_files_rebind(dirfd, &config->files, binding, &files);
	if (status != D1_OK)
		return status;
	if (d2_files_super(files)->state != D2_SB_RETIRED) {
		status = d2_files_super_update(files, D2_SB_NEEDS_RECOVERY);
		if (status != D1_OK) {
			d2_files_close(files);
			return status;
		}
	}
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
	if (status != D1_OK || replay.status != D1_OK) {
		if (status != D1_NOSPC && replay.status != D1_NOSPC)
			(void)d2_files_super_update(files, D2_SB_FENCED);
		goto fail;
	}
	if (s->retired) {
		if (d2_files_super(files)->state != D2_SB_RETIRED) {
			status = d2_files_super_update(files, D2_SB_RETIRED);
			if (status != D1_OK)
				goto fail;
		}
		*out = s;
		return D1_OK;
	}
	if (d2_files_super(files)->state == D2_SB_RETIRED) {
		status = D1_IO;
		goto fail;
	}
	if (!d2_rebuild_promises(s) ||
	    !d2_rebuild_payload_accounting(s, &payload_outstanding,
					  &payload_staged)) {
		status = D1_IO;
		goto fail;
	}
	s->next_payload_seq =
		(replay.highest_payload_id & ((UINT64_C(1) << 40) - 1)) + 1;
	reap_bytes = d2_restart_reap_bytes(s);
	if (s->wal_promised > UINT64_MAX - D2_START_RECORD_BYTES ||
	    reap_bytes > UINT64_MAX - D2_START_RECORD_BYTES - s->wal_promised) {
		status = D1_NOSPC;
		goto fail;
	}
	required = D2_START_RECORD_BYTES + reap_bytes + s->wal_promised;
	if (d2_files_wal_cursor(files) >
		    d2_files_super(files)->capacity_wal_bytes ||
	    required > d2_files_super(files)->capacity_wal_bytes -
			       d2_files_wal_cursor(files)) {
		status = D1_NOSPC;
		goto fail;
	}
	s->recovery_allowance = 0;
	status = d2_model_next_incarnation(s);
	if (status != D1_OK)
		goto fail;
	status = d2_model_apply_damage(s);
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
				d2_files_payload_cursor(files), s->wal_promised,
				payload_outstanding, payload_staged);
	if (status != D1_OK)
		goto fail;
	status = d2_restart_reap(s);
	if (status != D1_OK)
		goto fail;
	d2_open_recovery_window(s, D2_RESTART_SWEEP - reap_bytes);
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

static bool d2_wal_has_room(const struct d2_store *s, uint64_t add)
{
	const struct d2_superblock *super = d2_files_super(s->files);
	uint64_t headroom, restarts, wal_need = s->wal_promised;

	if (s->recovery_allowance > UINT64_MAX - wal_need)
		return false;
	wal_need += s->recovery_allowance;
	if (add > UINT64_MAX - wal_need)
		return false;
	wal_need += add;
	restarts = super->ds_incarnation ? super->ds_incarnation - 1 : 0;
	if (restarts > D2_MIN_RESTARTS)
		restarts = D2_MIN_RESTARTS;
	headroom = (D2_MIN_RESTARTS - restarts) * D2_RESTART_HEADROOM +
		   (D2_RECOVERY_HEADROOM -
		    D2_MIN_RESTARTS * D2_RESTART_HEADROOM);
	if (headroom > UINT64_MAX - wal_need)
		return false;
	wal_need += headroom;
	if (d2_files_wal_cursor(s->files) > super->capacity_wal_bytes ||
	    wal_need > super->capacity_wal_bytes -
			       d2_files_wal_cursor(s->files))
		return false;
	return true;
}

static bool d2_recovery_fundable(const struct d2_store *s,
				 const struct d1_envelope *env,
				 uint64_t record_bytes)
{
	const typeof(s->work[0]) *work;
	uint32_t i;

	if (!env->body.control.count ||
	    env->body.control.count > D1_BATCH_ENTRIES_MAX ||
	    record_bytes > s->recovery_allowance)
		return false;
	for (i = 0; i < env->body.control.count; i++) {
		work = d2_work_by_txn(s, env->body.control.txns[i].raw);
		if (!work || !work->recovery_eligible || work->recovery_admitted)
			return false;
	}
	return true;
}

static uint32_t d2_preflight_ordinary(struct d2_store *s,
				      const struct d1_envelope *env,
				      const struct d1_result *probe)
{
	const struct d2_superblock *super = d2_files_super(s->files);
	const typeof(s->work[0]) *work = NULL;
	const typeof(s->cohorts[0]) *cohort = NULL;
	uint64_t payload_need = 0, add = 0;
	uint64_t record_bytes;
	uint32_t i;

	switch (env->op) {
	case D1_OP_WRITE_BATCH:
		add = D2_ENTRY_RECORD_BYTES;
		if (probe->entries[0].status != D1_OK)
			break;
		payload_need = d2_payload_object_bytes(
			env->body.write.entries[0].payload_len);
		if (!env->body.write.activate)
			add += 2u * D2_ENTRY_RECORD_BYTES;
		break;
	case D1_OP_FINALIZE_BATCH:
	case D1_OP_COMMIT_BATCH:
		if (probe->entries[0].status != D1_OK)
			add = D2_ENTRY_RECORD_BYTES;
		break;
	case D1_OP_ROLLBACK_BATCH:
		work = d2_work_by_txn(
			s, env->body.rollback.entries[0].txn.raw);
		if (probe->entries[0].status != D1_OK &&
		    probe->entries[0].status != D1_NO_PREDECESSOR)
			add = D2_ENTRY_RECORD_BYTES;
		else if (!work || !d2_work_promise(work->phase))
			add = D2_POSTCOND_RECORD_BYTES + D2_ENTRY_RECORD_BYTES;
		break;
	case D1_OP_RECOVERY_ADMIT:
		record_bytes = 216u + 40u * env->body.control.count;
		if (probe->entries[0].status != D1_OK ||
		    !d2_recovery_fundable(s, env, record_bytes))
			add = record_bytes;
		break;
	case D1_OP_MARK_ERROR:
		add = D2_POSTCOND_RECORD_BYTES + 24u * env->body.repair.count;
		break;
	case D1_OP_CLEAR_ERROR:
		record_bytes = 276u + 40u * env->body.repair.count;
		if (probe->entries[0].status != D1_OK) {
			add = record_bytes;
			break;
		}
		cohort = d2_cohort_find(s, env->body.repair.cohort.raw);
		if (!cohort || d2_cohort_promise(
				       cohort->phase,
				       332u + 244u * cohort->count,
				       cohort->episode_present,
				       cohort->certificate_installed,
				       cohort->episode_cleared) < record_bytes)
			add = record_bytes;
		break;
	case D1_OP_BEGIN_REPAIR:
		record_bytes = 332u + 244u * env->body.repair.count;
		add = record_bytes;
		if (probe->entries[0].status == D1_OK)
			add += d2_cohort_promise(
				D2_ADMITTED, record_bytes,
				env->body.repair.episode_present, false, false);
		break;
	case D1_OP_PREPARE_REPAIR:
		for (i = 0; probe->entries[0].status == D1_OK &&
			    i < env->body.repair.count; i++)
			if (env->body.repair.entries[i].payload_present) {
				uint64_t extent = d2_payload_object_bytes(
					env->body.repair.entries[i].payload_len);

				if (extent > UINT64_MAX - payload_need)
					return D1_NOSPC;
				payload_need += extent;
			}
		/* fall through */
	case D1_OP_FINALIZE_REPAIR:
	case D1_OP_COMMIT_REPAIR:
	case D1_OP_ABORT_REPAIR:
	case D1_OP_UNLOCK:
		if (probe->entries[0].status != D1_OK &&
		    probe->entries[0].phase != D2_ABORTED) {
			add = 332u + 244u * env->body.repair.count;
			break;
		}
		cohort = d2_cohort_find(s, env->body.repair.cohort.raw);
		if (!cohort)
			add = 332u + 244u * env->body.repair.count;
		break;
	default:
		return D1_OK;
	}
	if (!d2_wal_has_room(s, add) ||
	    d2_files_payload_cursor(s->files) > super->capacity_payload_bytes ||
	    payload_need > super->capacity_payload_bytes -
				   d2_files_payload_cursor(s->files))
		return D1_NOSPC;
	return D1_OK;
}

static uint32_t d2_unrecorded_nospc(const struct d1_envelope *env,
				    struct d1_result *result)
{
	memset(result, 0, sizeof(*result));
	result->key = env->key;
	result->disposition = D1_UNRECORDED;
	result->count = 1;
	result->entries[0].status = D1_NOSPC;
	result->entries[0].stability = D1_FILE_SYNC;
	result->entries[0].disposition = D1_UNRECORDED;
	return D1_NOSPC;
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

static uint32_t d2_encode_postcond(struct d2_store *s,
				   const struct d1_envelope *env,
				   const struct d1_result *result,
				   uint8_t record[D2_POSTCOND_RECORD_BYTES],
				   size_t *written)
{
	const struct d1_rollback_entry *rollback =
		&env->body.rollback.entries[0];
	struct d2_wal_header h = { 0 };
	struct d2_control control = { 0 };
	struct d1_cursor cursor;
	uint8_t handle[32], file_key[32];

	if (env->op != D1_OP_ROLLBACK_BATCH ||
	    result->entries[0].status != D1_NO_PREDECESSOR ||
	    !result->entries[0].postcond_present ||
	    !rollback->visible_present || !rollback->custody_present)
		return D1_OK;
	h.family = D2_REC_CONTROL;
	memcpy(h.store_uuid, s->uuid.bytes, D1_UUID_BYTES);
	memcpy(h.wal_uuid, d2_files_super(s->files)->wal_uuid, D1_UUID_BYTES);
	h.lsn = d2_files_next_lsn(s->files);
	h.ds_incarnation = d2_files_super(s->files)->ds_incarnation;
	control.subtype = D2_CTL_POSTCOND;
	control.transition = D2_COMMITTED;
	control.status = D1_OK;
	d2_store_key(s, &control.key);
	memcpy(handle, env->object.export_uuid.bytes, D1_UUID_BYTES);
	memcpy(handle + D1_UUID_BYTES, env->object.object_uuid.bytes,
	       D1_UUID_BYTES);
	d2_file_key(handle, sizeof(handle), file_key);
	control.body_len = 68;
	d1_enc_init(&cursor, control.body, control.body_len);
	d1_enc_u64(&cursor, result->entries[0].postcond.raw);
	d1_enc_raw(&cursor, file_key, sizeof(file_key));
	d1_enc_u64(&cursor, rollback->index);
	d1_enc_u64(&cursor, rollback->visible.raw);
	d1_enc_u64(&cursor, rollback->custody.raw);
	d1_enc_u32(&cursor, rollback->predecessor_present ? 2 : 1);
	if (!d1_cursor_ok(&cursor))
		return D1_INVALID;
	if (!d2_control_encode(&h, &control, record,
			       D2_POSTCOND_RECORD_BYTES, written))
		return D1_INVALID;
	return D1_OK;
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
	uint8_t group[D2_POSTCOND_RECORD_BYTES + D2_ENTRY_RECORD_BYTES];
	uint8_t record[D2_ENTRY_RECORD_BYTES];
	uint8_t handle[32];
	uint64_t eof, payload_id = 0, offset = 0;
	uint64_t old_promise = 0, new_promise, promised;
	size_t postcond_written = 0;
	uint32_t status;
	const typeof(s->work[0]) *saved;
	bool predecessor_present = false;
	d1_version_id predecessor = d1_version_none();
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
	saved = d2_work_by_txn(s, entry.txn_id);
	if (saved)
		old_promise = d2_work_promise(saved->phase);
	new_promise = result->entries[0].status == D1_OK ||
			      result->entries[0].status == D1_NO_PREDECESSOR ?
			      d2_work_promise(entry.transition) : old_promise;
	if (old_promise > s->wal_promised ||
	    new_promise > UINT64_MAX - (s->wal_promised - old_promise))
		return D1_IO;
	promised = s->wal_promised - old_promise + new_promise;
	entry.owner_cohort = result->entries[0].owner.cohort.raw;
	entry.owner_client_id = result->entries[0].owner.writer;
	entry.owner_co_id = result->entries[0].owner.co_id;
	entry.generation = result->entries[0].guard.generation;
	if (write && result->entries[0].version_present) {
		if (!d1_fixture_version_predecessor(
			    s->model, result->entries[0].version,
			    &predecessor_present, &predecessor))
			return D1_IO;
		entry.predecessor_present = predecessor_present;
		entry.predecessor_object_id = predecessor.raw;
	} else if (life) {
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
	if (env->op == D1_OP_ROLLBACK_BATCH &&
	    result->entries[0].status == D1_NO_PREDECESSOR &&
	    result->entries[0].postcond_present && rollback->visible_present &&
	    rollback->custody_present) {
		status = d2_encode_postcond(s, env, result, group,
					    &postcond_written);
		if (status != D1_OK)
			return status;
		h.lsn = d2_files_next_lsn(s->files) + 1;
		if (!d2_entry_encode(&h, &entry, record))
			return D1_INVALID;
		memcpy(group + postcond_written, record, sizeof(record));
		status = d2_wal_append_group_floor(
			s, group, postcond_written + sizeof(record), promised);
	} else {
		status = d2_entry_encode(&h, &entry, record) ?
				 d2_wal_append_floor(s, record, sizeof(record),
						     promised) :
				 D1_INVALID;
	}
	if (status == D1_OK) {
		s->wal_promised = promised;
		if (has_payload)
			s->next_payload_seq++;
	}
	return status;
}

static uint32_t d2_persist_recovery(struct d2_store *s,
				    const struct d1_envelope *env,
				    const struct d1_result *result)
{
	typeof(s->work[0]) *work[D1_BATCH_ENTRIES_MAX];
	const struct d1_fixture_authority *fresh =
		d2_admission_find(s, env->body.control.new_admission.raw);
	struct d2_wal_header h = { 0 };
	struct d2_control control = { 0 };
	struct d1_cursor cursor;
	uint8_t record[D2_MAX_RECORD_BYTES], other[12];
	size_t written;
	uint32_t i, seqid, status;
	bool funded;

	if (!fresh || !env->body.control.count ||
	    env->body.control.count > D1_BATCH_ENTRIES_MAX)
		return D1_INVALID;
	for (i = 0; i < env->body.control.count; i++)
		work[i] = (typeof(s->work[0]) *)d2_work_by_txn(
			s, env->body.control.txns[i].raw);
	h.family = D2_REC_CONTROL;
	memcpy(h.store_uuid, s->uuid.bytes, D1_UUID_BYTES);
	memcpy(h.wal_uuid, d2_files_super(s->files)->wal_uuid, D1_UUID_BYTES);
	h.lsn = d2_files_next_lsn(s->files);
	h.ds_incarnation = d2_files_super(s->files)->ds_incarnation;
	control.subtype = D2_CTL_RECOVERY_ADMIT;
	control.transition = result->entries[0].status == D1_OK ? D2_COMMITTED :
								  D2_REFUSED;
	control.status = result->entries[0].status;
	d2_key_from_env(env, &control.key);
	if (!d2_env_digest(env, control.key.request_digest))
		return D1_NOSPC;
	memcpy(control.admission_issuer, fresh->issuer.bytes, D1_UUID_BYTES);
	control.admission_authority_epoch = fresh->authority_epoch;
	control.admission_client_id = env->body.control.new_admission.raw;
	control.body_len = 4u + 40u * env->body.control.count;
	d2_stateid(control.admission_client_id, &seqid, other);
	d1_enc_init(&cursor, control.body, control.body_len);
	d1_enc_u32(&cursor, env->body.control.count);
	for (i = 0; i < env->body.control.count; i++) {
		d1_enc_u64(&cursor, env->body.control.txns[i].raw);
		d1_enc_u32(&cursor, seqid);
		d1_enc_raw(&cursor, other, sizeof(other));
		d1_enc_u64(&cursor, fresh->lease_epoch);
		d1_enc_u64(&cursor, env->body.control.read_epoch);
	}
	if (!d1_cursor_ok(&cursor) ||
	    !d2_control_encode(&h, &control, record, sizeof(record), &written))
		return D1_INVALID;
	funded = result->entries[0].status == D1_OK &&
		 d2_recovery_fundable(s, env, written);
	s->recovery_spend = funded;
	status = d2_wal_append(s, record, written);
	s->recovery_spend = false;
	if (status == D1_OK && funded)
		for (i = 0; i < env->body.control.count; i++)
			work[i]->recovery_admitted = true;
	if (status == D1_OK && funded)
		d2_close_recovery_window_if_complete(s);
	return status;
}

static uint32_t d2_persist_episode_mark(struct d2_store *s,
					const struct d1_envelope *env,
					const struct d1_result *result)
{
	const struct d1_fixture_authority *auth =
		d2_admission_find(s, env->admission.raw);
	struct d2_wal_header h = { 0 };
	struct d2_control control = { 0 };
	struct d1_cursor cursor;
	uint8_t handle[32], file_key[32], record[D2_MAX_RECORD_BYTES];
	uint64_t episode_id;
	uint32_t i, status;
	size_t written;

	if (!auth || env->op != D1_OP_MARK_ERROR ||
	    env->body.repair.count == 0 ||
	    env->body.repair.count > D2_MAX_BATCH_ENTRIES)
		return D1_UNSUPPORTED;
	h.family = D2_REC_CONTROL;
	memcpy(h.store_uuid, s->uuid.bytes, D1_UUID_BYTES);
	memcpy(h.wal_uuid, d2_files_super(s->files)->wal_uuid, D1_UUID_BYTES);
	h.lsn = d2_files_next_lsn(s->files);
	h.ds_incarnation = d2_files_super(s->files)->ds_incarnation;
	control.subtype = D2_CTL_EPISODE_MARK;
	control.transition = result->entries[0].status == D1_OK ?
				     D2_COMMITTED : D2_REFUSED;
	control.status = result->entries[0].status;
	d2_key_from_env(env, &control.key);
	if (!d2_env_digest(env, control.key.request_digest))
		return D1_NOSPC;
	memcpy(control.admission_issuer, auth->issuer.bytes, D1_UUID_BYTES);
	control.admission_authority_epoch = auth->authority_epoch;
	control.admission_client_id = env->admission.raw;
	memcpy(handle, env->object.export_uuid.bytes, D1_UUID_BYTES);
	memcpy(handle + D1_UUID_BYTES, env->object.object_uuid.bytes,
	       D1_UUID_BYTES);
	d2_file_key(handle, sizeof(handle), file_key);
	control.body_len = 68 + 24 * env->body.repair.count;
	d1_enc_init(&cursor, control.body, control.body_len);
	episode_id = result->entries[0].episode_present ?
			     result->entries[0].episode.raw :
			     env->key.sequence | (UINT64_C(1) << 63);
	d2_episode_uuid(episode_id, handle);
	d1_enc_raw(&cursor, handle, D1_UUID_BYTES);
	d1_enc_raw(&cursor, file_key, sizeof(file_key));
	d1_enc_u64(&cursor, env->body.repair.range_begin);
	d1_enc_u64(&cursor, env->body.repair.range_end);
	d1_enc_u32(&cursor, env->body.repair.count);
	for (i = 0; i < env->body.repair.count; i++) {
		const struct d1_repair_entry *entry =
			&env->body.repair.entries[i];

		d1_enc_u64(&cursor, entry->index);
		d1_enc_u64(&cursor, entry->successor.raw);
		d1_enc_u64(&cursor, entry->custody.raw);
	}
	if (!d1_cursor_ok(&cursor) ||
	    !d2_control_encode(&h, &control, record, sizeof(record), &written))
		return D1_INVALID;
	status = d2_wal_append(s, record, written);
	if (status == D1_OK && result->entries[0].status == D1_OK &&
	    !d2_episode_remember(s, env, result))
		return D1_IO;
	return status;
}

static uint32_t d2_persist_episode_clear(struct d2_store *s,
					 const struct d1_envelope *env,
					 const struct d1_result *result)
{
	const struct d1_fixture_authority *auth =
		d2_admission_find(s, env->admission.raw);
	struct d2_wal_header h = { 0 };
	struct d2_control control = { 0 };
	struct d1_cursor cursor;
	uint8_t episode_uuid[D1_UUID_BYTES], handle[32], file_key[32];
	uint8_t record[D2_MAX_RECORD_BYTES];
	uint32_t i, status;
	typeof(s->cohorts[0]) *cohort;
	uint64_t promised;
	size_t written;

	if (!auth || env->op != D1_OP_CLEAR_ERROR ||
	    !env->body.repair.episode_present ||
	    !env->body.repair.cohort_present ||
	    env->body.repair.count == 0 ||
	    env->body.repair.count > D2_MAX_BATCH_ENTRIES)
		return D1_UNSUPPORTED;
	h.family = D2_REC_CONTROL;
	memcpy(h.store_uuid, s->uuid.bytes, D1_UUID_BYTES);
	memcpy(h.wal_uuid, d2_files_super(s->files)->wal_uuid, D1_UUID_BYTES);
	h.lsn = d2_files_next_lsn(s->files);
	h.ds_incarnation = d2_files_super(s->files)->ds_incarnation;
	control.subtype = D2_CTL_EPISODE_CLEAR;
	control.transition = result->entries[0].status == D1_OK ?
				     D2_COMMITTED : D2_REFUSED;
	control.status = result->entries[0].status;
	d2_key_from_env(env, &control.key);
	if (!d2_env_digest(env, control.key.request_digest))
		return D1_NOSPC;
	memcpy(control.admission_issuer, auth->issuer.bytes, D1_UUID_BYTES);
	control.admission_authority_epoch = auth->authority_epoch;
	control.admission_client_id = env->admission.raw;
	control.body_len = 64 + 40 * env->body.repair.count;
	d1_enc_init(&cursor, control.body, control.body_len);
	d2_episode_uuid(env->body.repair.episode.raw, episode_uuid);
	d1_enc_raw(&cursor, episode_uuid, sizeof(episode_uuid));
	d1_enc_u64(&cursor, env->body.repair.cohort.raw);
	d1_enc_u32(&cursor, 1);
	d1_enc_u32(&cursor, env->body.repair.count);
	d1_enc_raw(&cursor, env->body.repair.certificate,
		   D1_CERTIFICATE_BYTES);
	for (i = 0; i < env->body.repair.count; i++) {
		memcpy(handle, env->object.export_uuid.bytes,
		       D1_UUID_BYTES);
		memcpy(handle + D1_UUID_BYTES, env->object.object_uuid.bytes,
		       D1_UUID_BYTES);
		d2_file_key(handle, sizeof(handle), file_key);
		d1_enc_raw(&cursor, file_key, sizeof(file_key));
		d1_enc_u64(&cursor, env->body.repair.entries[i].index);
	}
	if (!d1_cursor_ok(&cursor) ||
	    !d2_control_encode(&h, &control, record, sizeof(record), &written))
		return D1_INVALID;
	cohort = d2_cohort_find(s, env->body.repair.cohort.raw);
	if (result->entries[0].status != D1_OK)
		return d2_wal_append(s, record, written);
	if (!cohort || s->wal_promised < 916u)
		return D1_IO;
	promised = s->wal_promised - 916u;
	status = d2_wal_append_floor(s, record, written, promised);
	if (status == D1_OK) {
		s->wal_promised = promised;
		cohort->episode_cleared = true;
	}
	return status;
}

static bool d2_repair_operation(uint32_t op)
{
	return op == D1_OP_BEGIN_REPAIR || op == D1_OP_PREPARE_REPAIR ||
	       op == D1_OP_FINALIZE_REPAIR || op == D1_OP_COMMIT_REPAIR ||
	       op == D1_OP_ABORT_REPAIR || op == D1_OP_UNLOCK;
}

static uint32_t d2_persist_cohort(struct d2_store *s,
				  const struct d1_envelope *env,
				  const struct d1_result *result,
				  uint64_t prior_eof)
{
	typeof(s->cohorts[0]) *saved = NULL;
	struct d2_payload_object payload = { 0 };
	struct d2_wal_header h = { 0 };
	struct d2_cohort disk = { 0 };
	const struct d1_repair_batch *repair = &env->body.repair;
	uint8_t record[D2_MAX_RECORD_BYTES], handle[32];
	uint64_t eof, payload_ids[D1_BATCH_ENTRIES_MAX] = { 0 };
	uint64_t payload_offsets[D1_BATCH_ENTRIES_MAX] = { 0 };
	uint64_t old_promise = 0, new_promise, promised;
	uint32_t i, payload_count = 0, status;
	size_t written;
	bool kept_abort, refused;

	if (!d2_repair_operation(env->op) || repair->count == 0 ||
	    repair->count > D2_MAX_BATCH_ENTRIES)
		return D1_UNSUPPORTED;
	refused = result->entries[0].status != D1_OK;
	kept_abort = refused && result->entries[0].phase == D2_ABORTED;
	if (env->op != D1_OP_BEGIN_REPAIR) {
		saved = d2_cohort_find(s, repair->cohort.raw);
		if ((!refused || kept_abort) &&
		    (!saved || saved->count != repair->count))
			return D1_INVALID;
	}
	if (env->op == D1_OP_PREPARE_REPAIR && !refused) {
		for (i = 0; i < repair->count; i++) {
			const struct d1_repair_entry *entry =
				&repair->entries[i];

			if (!entry->payload_present ||
			    s->next_payload_seq + payload_count >=
				    (UINT64_C(1) << 40))
				return D1_NOSPC;
			payload_ids[i] =
				(d2_files_super(s->files)->ds_incarnation
				 << 40) |
				(s->next_payload_seq + payload_count++);
			memset(&payload, 0, sizeof(payload));
			memcpy(payload.store_uuid, s->uuid.bytes,
			       D1_UUID_BYTES);
			payload.payload_object_id = payload_ids[i];
			payload.content = entry->payload;
			payload.content_len = entry->payload_len;
			payload.content_alg = entry->checksum.alg;
			payload.content_ck_len = entry->checksum.len;
			memcpy(payload.content_ck, entry->checksum.digest,
			       entry->checksum.len);
			status = d2_files_payload_append(s->files, &payload,
							 &payload_offsets[i]);
			if (status != D1_OK)
				return status;
		}
	}
	h.family = D2_REC_COHORT;
	memcpy(h.store_uuid, s->uuid.bytes, D1_UUID_BYTES);
	memcpy(h.wal_uuid, d2_files_super(s->files)->wal_uuid, D1_UUID_BYTES);
	h.lsn = d2_files_next_lsn(s->files);
	h.ds_incarnation = d2_files_super(s->files)->ds_incarnation;
	disk.transition = kept_abort ? D2_ABORTED :
			  refused ? D2_REFUSED :
			  env->op == D1_OP_UNLOCK ? D2_UNLOCKED :
						       result->entries[0].phase;
	disk.status = result->entries[0].status;
	if (kept_abort)
		disk.flags |= 2u;
	disk.disposition = D1_COMPLETED;
	disk.cohort_id = env->op == D1_OP_BEGIN_REPAIR && !refused ?
				 result->entries[0].cohort.raw :
				 repair->cohort_present ? repair->cohort.raw : 0;
	if (((!refused || kept_abort) && saved && saved->episode_present) ||
	    repair->episode_present) {
		disk.flags |= 1u;
		d2_episode_uuid((!refused || kept_abort) && saved ?
						       saved->episode_id :
						       repair->episode.raw,
				disk.episode_uuid);
	}
	d2_key_from_env(env, &disk.key);
	if (!d2_env_digest(env, disk.key.request_digest))
		return D1_NOSPC;
	d2_admission_encode(s, env->admission.raw, &disk.admission);
	disk.index_generation = result->index_epoch;
	memcpy(disk.result_verifier, result->entries[0].verifier,
	       D1_VERIFIER_BYTES);
	disk.member_count = repair->count;
	eof = d1_store_eof(s->model, &env->object);
	for (i = 0; i < disk.member_count; i++) {
		const struct d1_repair_entry *request = &repair->entries[i];
		struct d2_cohort_member *member = &disk.members[i];
		const typeof(s->cohorts[0].members[0]) *prior =
			(!refused || kept_abort) && saved ? &saved->members[i] :
							       NULL;
		struct d1_guard guard;
		d1_version_id visible = d1_version_none();
		const struct d1_objkey *object = prior ? &prior->object :
							 &env->object;

		member->repair_mode = prior ? prior->mode : request->mode;
		memcpy(handle, object->export_uuid.bytes, D1_UUID_BYTES);
		memcpy(handle + D1_UUID_BYTES, object->object_uuid.bytes,
		       D1_UUID_BYTES);
		d2_file_key(handle, sizeof(handle), member->file_key);
		member->chunk_index = prior ? prior->index : request->index;
		member->member_txn_id = prior ? prior->txn_id :
					request->txn_present ? request->txn.raw :
				result->entries[0].member_txn[i].raw;
		member->owner_cohort = prior ? prior->owner.cohort.raw :
					       request->owner.cohort.raw;
		member->owner_client_id = prior ? prior->owner.writer :
						  request->owner.writer;
		member->owner_co_id = prior ? prior->owner.co_id :
					      request->owner.co_id;
		member->custody_id = prior ? prior->custody_id :
					     request->custody.raw;
		member->postcond_present = prior ? prior->postcond_present :
						   request->postcond_present;
		member->postcond_id = prior ? prior->postcond_id :
					      request->postcond.raw;
		member->successor_object_id = prior ? prior->successor_id :
						      request->successor.raw;
		member->predecessor_present =
			prior ? prior->predecessor_present :
				request->predecessor_present;
		member->predecessor_object_id =
			prior ? prior->predecessor_id :
				request->predecessor.raw;
		member->predecessor_generation =
			prior ? prior->predecessor_generation : 0;
		member->payload_object_id =
			refused || env->op == D1_OP_ABORT_REPAIR ? 0 :
					    payload_ids[i] ? payload_ids[i] :
					    prior	   ? prior->payload_id :
							     0;
		member->payload_object_offset =
			refused || env->op == D1_OP_ABORT_REPAIR ?
							0 :
					      payload_offsets[i] ?
							payload_offsets[i] :
						prior ? prior->payload_offset :
							0;
		member->payload_content_len =
			refused || env->op == D1_OP_ABORT_REPAIR ? 0 :
					      request->payload_present ?
						      request->payload_len :
					      prior ? prior->payload_len :
						      0;
		member->staged = !refused &&
				 (env->op == D1_OP_PREPARE_REPAIR ||
				 (prior && prior->payload_id &&
				  env->op != D1_OP_COMMIT_REPAIR &&
				  env->op != D1_OP_ABORT_REPAIR &&
				  env->op != D1_OP_UNLOCK));
		member->member_status =
			result->entries[0].member_present &&
				 result->entries[0].member == i ?
					result->entries[0].status : D1_OK;
		member->extent_high_water = eof;
		member->extent_highest_index =
			member->extent_high_water ? (member->extent_high_water -
						     1) / s->chunk_bytes :
						    0;
		member->extent_kind = eof > prior_eof ? D2_EXTENT_EXTEND :
				      eof < prior_eof ? D2_EXTENT_SHRINK :
							D2_EXTENT_UNCHANGED;
		if (d1_store_visible(s->model, object, member->chunk_index,
				     &visible))
			member->result_visible_object_id = visible.raw;
		if (d1_store_guard(s->model, object, member->chunk_index,
				   &guard)) {
			member->result_guard_never_written =
				guard.never_written;
			member->result_guard_generation = guard.generation;
			member->result_guard_writer = guard.writer;
		}
		if (!refused && request->payload_present) {
			member->result_effective_len = request->payload_len;
			member->result_ck_alg = request->checksum.alg;
			member->result_ck_len = request->checksum.len;
			memcpy(member->result_ck, request->checksum.digest,
			       request->checksum.len);
		} else if (!refused && prior && prior->payload_id) {
			member->result_effective_len = prior->payload_len;
			member->result_ck_alg = prior->payload_ck_alg;
			member->result_ck_len = prior->payload_ck_len;
			memcpy(member->result_ck, prior->payload_ck,
			       prior->payload_ck_len);
		}
	}
	if (!d2_cohort_encode(&h, &disk, record, sizeof(record), &written))
		return D1_INVALID;
	if (saved)
		old_promise = d2_cohort_promise(saved->phase, written,
						saved->episode_present,
						saved->certificate_installed,
						saved->episode_cleared);
	new_promise = refused && !kept_abort ? old_promise :
		      d2_cohort_promise(disk.transition, written,
					disk.flags & 1u,
					saved && saved->certificate_installed,
					saved && saved->episode_cleared);
	if (old_promise > s->wal_promised ||
	    new_promise > UINT64_MAX - (s->wal_promised - old_promise))
		return D1_IO;
	promised = s->wal_promised - old_promise + new_promise;
	status = d2_wal_append_floor(s, record, written, promised);
	if (status != D1_OK)
		return status;
	s->wal_promised = promised;
	s->next_payload_seq += payload_count;
	if (!refused && env->op == D1_OP_BEGIN_REPAIR &&
	    !d2_cohort_remember_begin(s, env, result))
		return D1_IO;
	if (!refused && env->op == D1_OP_PREPARE_REPAIR) {
		for (i = 0; i < repair->count; i++) {
			d1_txn_id txn;
			d1_version_id version;

			saved->members[i].payload_id = payload_ids[i];
			saved->members[i].payload_offset = payload_offsets[i];
			saved->members[i].payload_len =
				repair->entries[i].payload_len;
			saved->members[i].payload_ck_alg =
				repair->entries[i].checksum.alg;
			saved->members[i].payload_ck_len =
				repair->entries[i].checksum.len;
			memcpy(saved->members[i].payload_ck,
			       repair->entries[i].checksum.digest,
			       repair->entries[i].checksum.len);
			if (!d1_fixture_repair_member(s->model,
						      repair->cohort, i, &txn,
						      &version) ||
			    !version.raw ||
			    !d2_work_set_version(s, txn.raw, version.raw))
				return D1_IO;
		}
	}
	if ((!refused || kept_abort) && saved && env->op != D1_OP_UNLOCK) {
		saved->phase = disk.transition;
		if (!d2_repair_work_set_phase(s, saved->id, disk.transition))
			return D1_IO;
	} else if (!refused && saved && env->op == D1_OP_UNLOCK &&
		   !d2_repair_work_set_phase(s, saved->id, D2_UNLOCKED))
		return D1_IO;
	if (saved && (env->op == D1_OP_ABORT_REPAIR || kept_abort))
		for (i = 0; i < saved->count; i++) {
			saved->members[i].payload_id = 0;
			saved->members[i].payload_offset = 0;
			saved->members[i].payload_len = 0;
			saved->members[i].payload_ck_alg = 0;
			saved->members[i].payload_ck_len = 0;
			memset(saved->members[i].payload_ck, 0,
			       sizeof(saved->members[i].payload_ck));
		}
	return D1_OK;
}

uint32_t d2_store_apply(struct d2_store *s, const struct d1_envelope *env,
			struct d1_result *result)
{
	const typeof(s->work[0]) *damaged_work = NULL;
	typeof(s->cohorts[0]) *damaged_cohort = NULL;
	struct d1_result probe;
	uint8_t *before = NULL, *after = NULL;
	uint8_t digest[D1_DIGEST_BYTES];
	uint8_t file_key[32];
	size_t before_len = 0, after_len = 0;
	uint64_t prior_eof;
	uint32_t damaged_member, status, persist;
	bool model_changed = false, predecessor_present = false;
	d1_version_id predecessor = d1_version_none();

	if (!s || !env || !result)
		return D1_INVALID;
	if (!d1_envelope_validate(env))
		return D1_INVALID;
	if (!d2_env_digest(env, digest))
		return D1_NOSPC;
	if ((env->op != D1_OP_WRITE_BATCH && env->op != D1_OP_FINALIZE_BATCH &&
	     env->op != D1_OP_COMMIT_BATCH && env->op != D1_OP_ROLLBACK_BATCH &&
	     env->op != D1_OP_RECOVERY_ADMIT && env->op != D1_OP_MARK_ERROR &&
	     env->op != D1_OP_CLEAR_ERROR &&
	     !d2_repair_operation(env->op)) ||
	    (!d2_repair_operation(env->op) && env->op != D1_OP_MARK_ERROR &&
	     env->op != D1_OP_CLEAR_ERROR &&
	     env->op != D1_OP_RECOVERY_ADMIT &&
	     d1_envelope_member_count(env) != 1) ||
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
	if (d2_admission_matches(s, env->admission.raw, &env->object) &&
	    d2_receipt_lookup(s, env, digest, result)) {
		pthread_mutex_unlock(&s->lock);
		return D1_OK;
	}
	if (s->retired || d2_object_tombstoned(s, &env->object)) {
		memset(result, 0, sizeof(*result));
		result->key = env->key;
		result->disposition = D1_UNRECORDED;
		result->count = 1;
		result->entries[0].status = D1_BAD_PHASE;
		result->entries[0].disposition = D1_UNRECORDED;
		pthread_mutex_unlock(&s->lock);
		return D1_OK;
	}
	{
		typeof(s->admissions[0]) *admission =
			d2_admission_slot(s, env->admission.raw);

		if (admission && !admission->authority_seen) {
			memset(result, 0, sizeof(*result));
			result->key = env->key;
			result->disposition = D1_UNRECORDED;
			result->count = 1;
			result->entries[0].status = D1_STALE_AUTH;
			result->entries[0].disposition = D1_UNRECORDED;
			pthread_mutex_unlock(&s->lock);
			return D1_OK;
		}
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
	status = d1_store_probe(s->model, env, &probe);
	if (status != D1_OK) {
		if (status == D1_NOSPC)
			status = d2_unrecorded_nospc(env, result);
		pthread_mutex_unlock(&s->lock);
		return status;
	}
	status = d2_preflight_ordinary(s, env, &probe);
	if (status == D1_NOSPC) {
		status = d2_unrecorded_nospc(env, result);
		pthread_mutex_unlock(&s->lock);
		return status;
	}
	prior_eof = d1_store_eof(s->model, &env->object);
	if (env->op == D1_OP_FINALIZE_BATCH ||
	    env->op == D1_OP_COMMIT_BATCH) {
		damaged_work = d2_work_by_txn(
			s, env->body.lifecycle.entries[0].txn.raw);
		if (damaged_work &&
		    d2_version_damaged(s, damaged_work->version_id)) {
			*result = probe;
			if (env->op == D1_OP_COMMIT_BATCH && result->index_epoch)
				result->index_epoch--;
			result->eof = prior_eof;
			result->disposition = D1_COMPLETED;
			result->entries[0].status = D1_CHECKSUM;
			result->entries[0].disposition = D1_COMPLETED;
			result->entries[0].phase = damaged_work->phase;
			status = D1_OK;
			persist = d2_persist_entry(s, env, result, prior_eof);
			goto recorded;
		}
	}
	if (env->op == D1_OP_FINALIZE_REPAIR ||
	    env->op == D1_OP_COMMIT_REPAIR) {
		damaged_cohort = d2_cohort_find(s, env->body.repair.cohort.raw);
		for (damaged_member = 0;
		     damaged_cohort &&
		     damaged_member < env->body.repair.count;
		     damaged_member++) {
			damaged_work = d2_work_by_txn(
				s, env->body.repair.entries[damaged_member].txn.raw);
			if (damaged_work &&
			    d2_version_damaged(s, damaged_work->version_id))
				break;
		}
		if (damaged_cohort &&
		    damaged_member < env->body.repair.count) {
			*result = probe;
			if (env->op == D1_OP_COMMIT_REPAIR &&
			    result->index_epoch)
				result->index_epoch--;
			result->eof = prior_eof;
			result->disposition = D1_COMPLETED;
			result->entries[0].status = D1_CHECKSUM;
			result->entries[0].disposition = D1_COMPLETED;
			result->entries[0].phase = damaged_cohort->phase;
			result->entries[0].member_present = true;
			result->entries[0].member = damaged_member;
			if (!d1_store_guard(s->model, &damaged_work->object,
					    damaged_work->index,
					    &result->entries[0].guard)) {
				status = D1_IO;
				goto out;
			}
			status = D1_OK;
			persist = d2_persist_cohort(s, env, result, prior_eof);
			goto recorded;
		}
	}
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
	model_changed = true;
	if (env->op == D1_OP_RECOVERY_ADMIT)
		persist = d2_persist_recovery(s, env, result);
	else if (env->op == D1_OP_MARK_ERROR)
		persist = d2_persist_episode_mark(s, env, result);
	else if (env->op == D1_OP_CLEAR_ERROR)
		persist = d2_persist_episode_clear(s, env, result);
	else if (d2_repair_operation(env->op))
		persist = d2_persist_cohort(s, env, result, prior_eof);
	else
		persist = d2_persist_entry(s, env, result, prior_eof);
recorded:
	if (persist != D1_OK) {
		if (model_changed &&
		    d2_model_restore(s, before, before_len) != D1_OK)
			persist = D1_IO;
		s->fenced = true;
		status = persist;
		memset(result, 0, sizeof(*result));
		result->disposition = D1_UNRECORDED;
	} else if (!d2_receipt_remember(s, &env->object.export_uuid, &env->key,
					digest, result)) {
		s->fenced = true;
		status = D1_IO;
	} else if (env->op == D1_OP_WRITE_BATCH &&
		   result->entries[0].version_present &&
		   !d1_fixture_version_predecessor(
			   s->model, result->entries[0].version,
			   &predecessor_present, &predecessor)) {
		s->fenced = true;
		status = D1_IO;
	} else if (env->op == D1_OP_WRITE_BATCH &&
		   result->entries[0].version_present &&
		   !d2_work_remember(s, result->entries[0].txn.raw,
				     result->entries[0].version.raw,
				     env->admission.raw, &env->object,
				     env->body.write.entries[0].index,
				     &result->entries[0].owner,
				     result->entries[0].phase,
				     d2_payload_object_bytes(
					     env->body.write.entries[0].payload_len),
				     predecessor_present, predecessor.raw)) {
		s->fenced = true;
		status = D1_IO;
	} else if ((result->entries[0].status == D1_OK ||
		    (env->op == D1_OP_ROLLBACK_BATCH &&
		     result->entries[0].status == D1_NO_PREDECESSOR)) &&
		   (env->op == D1_OP_FINALIZE_BATCH ||
		    env->op == D1_OP_COMMIT_BATCH ||
		    env->op == D1_OP_ROLLBACK_BATCH) &&
		   result->entries[0].txn_present &&
		   !d2_work_set_phase(s, result->entries[0].txn.raw,
				      result->entries[0].phase)) {
		s->fenced = true;
		status = D1_IO;
	} else if (env->op == D1_OP_RECOVERY_ADMIT &&
		   result->entries[0].status == D1_OK &&
		   !d2_recovery_rebind(s, env)) {
		s->fenced = true;
		status = D1_IO;
	}
out:
	free(before);
	free(after);
	pthread_mutex_unlock(&s->lock);
	return status;
}

static uint32_t d2_client_control(struct d2_store *s, uint32_t subtype,
				  uint64_t admission_id,
				  const struct d1_fixture_authority *auth,
				  const uint8_t *body, uint32_t body_len)
{
	struct d2_wal_header h = { 0 };
	struct d2_control control = { 0 };
	uint8_t record[D2_MAX_RECORD_BYTES];
	size_t written;

	if (!auth || !body ||
	    !memcmp(auth->session, (uint8_t[D1_UUID_BYTES]){ 0 },
		    D1_UUID_BYTES))
		return D1_INVALID;
	h.family = D2_REC_CONTROL;
	memcpy(h.store_uuid, s->uuid.bytes, D1_UUID_BYTES);
	memcpy(h.wal_uuid, d2_files_super(s->files)->wal_uuid, D1_UUID_BYTES);
	h.lsn = d2_files_next_lsn(s->files);
	h.ds_incarnation = d2_files_super(s->files)->ds_incarnation;
	if (h.lsn > UINT32_MAX)
		return D1_NOSPC;
	control.subtype = subtype;
	control.transition = D2_COMMITTED;
	control.status = D1_OK;
	memcpy(control.key.session, auth->session, D1_UUID_BYTES);
	control.key.sequence = (uint32_t)h.lsn;
	d2_operation_key(control.key.session, 0, control.key.sequence, 0,
			 control.key.operation_key);
	memcpy(control.admission_issuer, auth->issuer.bytes, D1_UUID_BYTES);
	control.admission_authority_epoch = auth->authority_epoch;
	control.admission_client_id = admission_id;
	control.body_len = body_len;
	if (body_len > sizeof(control.body))
		return D1_INVALID;
	memcpy(control.body, body, body_len);
	d2_hash_domain("FFV2-D2B-CONTROL-v1", body, body_len,
		       control.key.request_digest);
	if (!d2_control_encode(&h, &control, record, sizeof(record), &written))
		return D1_INVALID;
	return d2_wal_append(s, record, written);
}

static uint32_t d2_admission_controls(struct d2_store *s, d1_admission_id id,
				      const struct d1_fixture_authority *auth)
{
	struct d1_cursor cursor;
	uint8_t body[84], other[12];
	uint32_t seqid, status;

	d2_stateid(id.raw, &seqid, other);
	d1_enc_init(&cursor, body, sizeof(body));
	d1_enc_u32(&cursor, seqid);
	d1_enc_raw(&cursor, other, sizeof(other));
	d1_enc_u64(&cursor, auth->lease_epoch);
	d1_enc_u64(&cursor, auth->fence_sequence);
	d1_enc_u32(&cursor, auth->rights & (D1_RIGHT_READ | D1_RIGHT_WRITE));
	d1_enc_raw(&cursor, auth->session, sizeof(auth->session));
	d1_enc_u64(&cursor, id.raw);
	d1_enc_raw(&cursor, auth->principal.bytes,
		   sizeof(auth->principal.bytes));
	d1_enc_u32(&cursor, auth->writer);
	d1_enc_u32(&cursor, auth->rights);
	if (!d1_cursor_ok(&cursor))
		return D1_INVALID;
	status = d2_client_control(s, D2_CTL_TRUST_STATEID, id.raw, auth, body,
				   sizeof(body));
	if (status != D1_OK)
		return status;
	d1_enc_init(&cursor, body, 44);
	d1_enc_raw(&cursor, auth->issuer.bytes, sizeof(auth->issuer.bytes));
	d1_enc_u64(&cursor, auth->authority_epoch);
	d1_enc_u32(&cursor, 1);
	d1_enc_u32(&cursor, seqid);
	d1_enc_raw(&cursor, other, sizeof(other));
	if (!d1_cursor_ok(&cursor))
		return D1_INVALID;
	return d2_client_control(s, D2_CTL_AUTHORITY_ADMIT, id.raw, auth, body,
				 44);
}

static d1_admission_id
d2_admit_model(struct d2_store *s, const struct d1_objkey *object,
	       const struct d1_fixture_authority *auth)
{
	d1_admission_id id = d1_admission_none();
	uint8_t file_key[32];
	uint32_t i, status;
	bool registered = false;

	for (i = 0; i < s->registered_count; i++)
		if (d2_same_object(&s->registered[i].object, object)) {
			registered = true;
			break;
		}
	if (!d2_wal_has_room(s, registered ? 0u : 396u))
		return id;
	status = d2_register_file(s, object, file_key);
	if (status != D1_OK) {
		if (status != D1_NOSPC)
			s->fenced = true;
		return id;
	}
	id = d1_fixture_admit_full(s->model, object, auth);
	if (d1_admission_live(id) &&
	    !d2_admission_remember(s, id.raw, object, auth)) {
		s->fenced = true;
		return d1_admission_none();
	}
	return id;
}

d1_admission_id d2_store_admit_bare(
	struct d2_store *s, const struct d1_objkey *object,
	const struct d1_fixture_authority *auth)
{
	d1_admission_id id = d1_admission_none();

	if (!s || !object || !auth ||
	    !memcmp(auth->session, (uint8_t[D1_UUID_BYTES]){ 0 },
		    D1_UUID_BYTES))
		return id;
	pthread_mutex_lock(&s->lock);
	if (!s->fenced && !s->retired && !d2_object_tombstoned(s, object))
		id = d2_admit_model(s, object, auth);
	pthread_mutex_unlock(&s->lock);
	return id;
}

uint32_t d2_store_trust_admission(struct d2_store *s, d1_admission_id actor,
				  d1_admission_id beneficiary)
{
	const struct d1_fixture_authority *actor_auth, *beneficiary_auth;
	typeof(s->work[0]) *candidate;
	uint8_t body[84], other[12];
	struct d1_cursor cursor;
	uint32_t seqid, status = D1_INVALID;
	bool funded = false;

	if (!s || !d1_admission_live(actor) ||
	    !d1_admission_live(beneficiary))
		return D1_INVALID;
	pthread_mutex_lock(&s->lock);
	actor_auth = d2_admission_find(s, actor.raw);
	beneficiary_auth = d2_admission_find(s, beneficiary.raw);
	if (s->fenced) {
		status = D1_IO;
		goto out;
	}
	if (!actor_auth || !beneficiary_auth ||
	    memcmp(actor_auth->issuer.bytes, beneficiary_auth->issuer.bytes,
		   D1_UUID_BYTES) ||
	    actor_auth->authority_epoch != beneficiary_auth->authority_epoch)
		goto out;
	if (d2_admission_slot(s, beneficiary.raw)->trust_seen) {
		status = D1_STALE_AUTH;
		goto out;
	}
	candidate = d2_recovery_candidate(s, NULL, beneficiary_auth);
	funded = candidate && s->recovery_allowance >= 296u;
	if (!funded && !d2_wal_has_room(s, 296u)) {
		status = D1_NOSPC;
		goto out;
	}
	d2_stateid(beneficiary.raw, &seqid, other);
	d1_enc_init(&cursor, body, sizeof(body));
	d1_enc_u32(&cursor, seqid);
	d1_enc_raw(&cursor, other, sizeof(other));
	d1_enc_u64(&cursor, beneficiary_auth->lease_epoch);
	d1_enc_u64(&cursor, beneficiary_auth->fence_sequence);
	d1_enc_u32(&cursor, beneficiary_auth->rights &
				   (D1_RIGHT_READ | D1_RIGHT_WRITE));
	d1_enc_raw(&cursor, beneficiary_auth->session, D1_UUID_BYTES);
	d1_enc_u64(&cursor, beneficiary.raw);
	d1_enc_raw(&cursor, beneficiary_auth->principal.bytes, D1_UUID_BYTES);
	d1_enc_u32(&cursor, beneficiary_auth->writer);
	d1_enc_u32(&cursor, beneficiary_auth->rights);
	if (!d1_cursor_ok(&cursor))
		goto out;
	s->recovery_spend = funded;
	status = d2_client_control(s, D2_CTL_TRUST_STATEID, actor.raw,
				   actor_auth, body, sizeof(body));
	s->recovery_spend = false;
	if (status == D1_OK && funded) {
		candidate->recovery_trusted = true;
		d2_admission_slot(s, beneficiary.raw)->recovery_txn_id =
			candidate->txn_id;
	}
	if (status == D1_OK)
		d2_admission_slot(s, beneficiary.raw)->trust_seen = true;
out:
	pthread_mutex_unlock(&s->lock);
	return status;
}

uint32_t d2_store_admit_authority(struct d2_store *s,
				  d1_admission_id actor,
				  const d1_admission_id *beneficiaries,
				  uint32_t count)
{
	const struct d1_fixture_authority *actor_auth;
	const struct d1_fixture_authority *beneficiary_auth;
	typeof(s->work[0]) *candidate[D2_MAX_AUTHORITY_STATEIDS] = { 0 };
	struct d1_cursor cursor;
	uint8_t body[28 + 16 * D2_MAX_AUTHORITY_STATEIDS];
	uint8_t previous[16] = { 0 }, stateid[16];
	uint32_t i, seqid, status = D1_INVALID;
	bool funded = count != 0;

	if (!s || !d1_admission_live(actor) ||
	    (count && !beneficiaries) || count > D2_MAX_AUTHORITY_STATEIDS)
		return D1_INVALID;
	pthread_mutex_lock(&s->lock);
	actor_auth = d2_admission_find(s, actor.raw);
	if (s->fenced) {
		status = D1_IO;
		goto out;
	}
	if (!actor_auth)
		goto out;
	d1_enc_init(&cursor, body, 28u + 16u * count);
	d1_enc_raw(&cursor, actor_auth->issuer.bytes, D1_UUID_BYTES);
	d1_enc_u64(&cursor, actor_auth->authority_epoch);
	d1_enc_u32(&cursor, count);
	for (i = 0; i < count; i++) {
		typeof(s->admissions[0]) *slot;

		if (!d1_admission_live(beneficiaries[i]))
			goto out;
		slot = d2_admission_slot(s, beneficiaries[i].raw);
		beneficiary_auth = d2_admission_find(s, beneficiaries[i].raw);
		if (!slot || !beneficiary_auth || !slot->trust_seen ||
		    slot->authority_seen ||
		    memcmp(actor_auth->issuer.bytes,
			   beneficiary_auth->issuer.bytes, D1_UUID_BYTES) ||
		    actor_auth->authority_epoch !=
			    beneficiary_auth->authority_epoch)
			goto out;
		d2_stateid(beneficiaries[i].raw, &seqid, stateid + 4);
		stateid[0] = (uint8_t)(seqid >> 24);
		stateid[1] = (uint8_t)(seqid >> 16);
		stateid[2] = (uint8_t)(seqid >> 8);
		stateid[3] = (uint8_t)seqid;
		if (i && memcmp(previous, stateid, sizeof(stateid)) >= 0)
			goto out;
		memcpy(previous, stateid, sizeof(previous));
		d1_enc_u32(&cursor, seqid);
		d1_enc_raw(&cursor, stateid + 4, 12);
		candidate[i] = (typeof(s->work[0]) *)d2_work_by_txn(
			s, slot->recovery_txn_id);
		if (!candidate[i] || !candidate[i]->recovery_trusted ||
		    candidate[i]->recovery_vouched)
			funded = false;
	}
	if (!d1_cursor_ok(&cursor))
		goto out;
	if (funded) {
		uint64_t bytes = 240u + 16u * count;

		if (s->recovery_allowance < bytes) {
			status = D1_NOSPC;
			goto out;
		}
	} else if (!d2_wal_has_room(s, 240u + 16u * count)) {
		status = D1_NOSPC;
		goto out;
	}
	s->recovery_spend = funded;
	status = d2_client_control(s, D2_CTL_AUTHORITY_ADMIT, actor.raw,
				   actor_auth, body, 28u + 16u * count);
	s->recovery_spend = false;
	if (status == D1_OK)
		for (i = 0; i < count; i++) {
			d2_admission_slot(s, beneficiaries[i].raw)->authority_seen =
				true;
			if (funded)
				candidate[i]->recovery_vouched = true;
		}
out:
	pthread_mutex_unlock(&s->lock);
	return status;
}

d1_admission_id d2_store_admit_full(struct d2_store *s,
				    const struct d1_objkey *object,
				    const struct d1_fixture_authority *auth)
{
	d1_admission_id id = d1_admission_none();
	typeof(s->work[0]) *candidate = NULL;
	uint8_t file_key[32];
	uint32_t i, status;
	bool funded = false, registered = false;

	if (!s || !object || !auth ||
	    !memcmp(auth->session, (uint8_t[D1_UUID_BYTES]){ 0 },
		    D1_UUID_BYTES))
		return id;
	pthread_mutex_lock(&s->lock);
	if (s->retired || d2_object_tombstoned(s, object)) {
		pthread_mutex_unlock(&s->lock);
		return id;
	}
	for (i = 0; i < s->registered_count; i++)
		if (d2_same_object(&s->registered[i].object, object)) {
			registered = true;
			break;
		}
	if (!s->fenced && registered) {
		candidate = d2_recovery_candidate(s, object, auth);
		funded = candidate && s->recovery_allowance >= 296u + 256u;
	}
	if (!s->fenced && !funded &&
	    !d2_wal_has_room(s, 296u + 256u + (registered ? 0u : 396u))) {
		pthread_mutex_unlock(&s->lock);
		return id;
	}
	status = s->fenced ? D1_IO : d2_register_file(s, object, file_key);
	if (status != D1_OK && status != D1_NOSPC)
		s->fenced = true;
	if (!s->fenced)
		if (status == D1_OK)
			id = d1_fixture_admit_full(s->model, object, auth);
	s->recovery_spend = funded;
	if (d1_admission_live(id) &&
	    d2_admission_controls(s, id, auth) != D1_OK) {
		s->recovery_spend = false;
		s->fenced = true;
		id = d1_admission_none();
	} else if (d1_admission_live(id) &&
		   !d2_admission_remember(s, id.raw, object, auth)) {
		s->fenced = true;
		id = d1_admission_none();
	} else if (d1_admission_live(id)) {
		d2_admission_slot(s, id.raw)->trust_seen = true;
		d2_admission_slot(s, id.raw)->authority_seen = true;
		if (funded) {
			candidate->recovery_trusted = true;
			candidate->recovery_vouched = true;
			d2_admission_slot(s, id.raw)->recovery_txn_id =
				candidate->txn_id;
		}
	}
	s->recovery_spend = false;
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
	return d2_wal_append(s, record, written);
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
	return d2_wal_append(s, record, written);
}

static uint32_t d2_certificate_control(
	struct d2_store *s, const typeof(s->cohorts[0]) *cohort,
	const uint8_t certificate[D1_CERTIFICATE_BYTES])
{
	const struct d1_fixture_authority *auth =
		d2_admission_find(s, cohort->admission_id);
	struct d2_wal_header h = { 0 };
	struct d2_control control = { 0 };
	struct d1_cursor cursor;
	uint8_t episode_uuid[D1_UUID_BYTES], record[D2_MAX_RECORD_BYTES];
	size_t written;
	uint64_t promised;

	if (!auth)
		return D1_STALE_AUTH;
	h.family = D2_REC_CONTROL;
	memcpy(h.store_uuid, s->uuid.bytes, D1_UUID_BYTES);
	memcpy(h.wal_uuid, d2_files_super(s->files)->wal_uuid, D1_UUID_BYTES);
	h.lsn = d2_files_next_lsn(s->files);
	h.ds_incarnation = d2_files_super(s->files)->ds_incarnation;
	if (h.lsn > UINT32_MAX)
		return D1_NOSPC;
	control.subtype = D2_CTL_CERTIFICATE_INSTALL;
	control.transition = D2_COMMITTED;
	control.status = D1_OK;
	memcpy(control.key.session, auth->session, D1_UUID_BYTES);
	control.key.sequence = (uint32_t)h.lsn;
	d2_operation_key(control.key.session, 0, control.key.sequence, 0,
			 control.key.operation_key);
	memcpy(control.admission_issuer, auth->issuer.bytes, D1_UUID_BYTES);
	control.admission_authority_epoch = auth->authority_epoch;
	control.admission_client_id = cohort->admission_id;
	control.body_len = 56;
	d1_enc_init(&cursor, control.body, control.body_len);
	d2_episode_uuid(cohort->episode_id, episode_uuid);
	d1_enc_raw(&cursor, episode_uuid, sizeof(episode_uuid));
	d1_enc_u64(&cursor, cohort->id);
	d1_enc_raw(&cursor, certificate, D1_CERTIFICATE_BYTES);
	if (!d1_cursor_ok(&cursor))
		return D1_INVALID;
	d2_hash_domain("FFV2-D2B-CONTROL-v1", control.body,
		       control.body_len, control.key.request_digest);
	if (!d2_control_encode(&h, &control, record, sizeof(record), &written))
		return D1_INVALID;
	if (s->wal_promised < written)
		return D1_IO;
	promised = s->wal_promised - written;
	if (d2_wal_append_floor(s, record, written, promised) != D1_OK)
		return D1_NOSPC;
	s->wal_promised = promised;
	return D1_OK;
}

void d2_store_revoke(struct d2_store *s, d1_admission_id admission)
{
	uint32_t status;
	typeof(s->admissions[0]) *slot;

	if (!s)
		return;
	pthread_mutex_lock(&s->lock);
	if (!s->fenced && !s->retired) {
		slot = d2_admission_slot(s, admission.raw);
		d1_fixture_revoke(s->model, admission);
		if (slot)
			slot->revoked = true;
		status = d2_liveness_control(s, admission,
					     D2_CTL_REVOKE_STATEID);
		if (status != D1_OK && status != D1_NOSPC)
			s->fenced = true;
	}
	pthread_mutex_unlock(&s->lock);
}

uint32_t d2_store_revoke_authority(struct d2_store *s,
				   d1_admission_id actor,
				   const struct d1_uuid *issuer,
				   uint64_t authority_epoch, uint32_t reason)
{
	const struct d1_fixture_authority *actor_auth;
	typeof(s->admissions[0]) *actor_slot;
	struct d1_cursor cursor;
	uint8_t body[28];
	uint32_t i, status = D1_INVALID;

	if (!s || !issuer || !d1_admission_live(actor) || !authority_epoch ||
	    !reason)
		return D1_INVALID;
	pthread_mutex_lock(&s->lock);
	actor_slot = d2_admission_slot(s, actor.raw);
	actor_auth = d2_admission_find(s, actor.raw);
	if (s->fenced)
		status = D1_IO;
	else if (s->retired)
		status = D1_BAD_PHASE;
	else if (!actor_slot || !actor_auth || actor_slot->revoked ||
		 actor_slot->expired)
		status = D1_STALE_AUTH;
	else {
		for (i = 0; i < D1_MAX_ADMISSIONS; i++) {
			typeof(s->admissions[0]) *slot = &s->admissions[i];

			if (!slot->used ||
			    slot->auth.authority_epoch != authority_epoch ||
			    memcmp(slot->auth.issuer.bytes, issuer->bytes,
				   D1_UUID_BYTES))
				continue;
			d1_fixture_revoke(
				s->model,
				d1_fixture_admission_handle(s->model, slot->id));
			slot->revoked = true;
		}
		d1_enc_init(&cursor, body, sizeof(body));
		d1_enc_raw(&cursor, issuer->bytes, D1_UUID_BYTES);
		d1_enc_u64(&cursor, authority_epoch);
		d1_enc_u32(&cursor, reason);
		status = d1_cursor_ok(&cursor) ?
				 d2_client_control(s, D2_CTL_AUTHORITY_REVOKE,
						   actor.raw, actor_auth, body,
						   sizeof(body)) :
				 D1_INVALID;
		if (status != D1_OK && status != D1_NOSPC)
			s->fenced = true;
	}
	pthread_mutex_unlock(&s->lock);
	return status;
}

void d2_store_expire(struct d2_store *s, d1_admission_id admission)
{
	uint32_t status;
	typeof(s->admissions[0]) *slot;

	if (!s)
		return;
	pthread_mutex_lock(&s->lock);
	if (!s->fenced && !s->retired) {
		slot = d2_admission_slot(s, admission.raw);
		d1_fixture_expire(s->model, admission);
		if (slot)
			slot->expired = true;
		status = d2_liveness_control(s, admission, D2_CTL_LEASE_EXPIRE);
		if (status != D1_OK && status != D1_NOSPC)
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
	if (!s->fenced && !s->retired && work &&
	    !d2_object_tombstoned(s, &work->object) &&
	    d2_wal_has_room(s, 248u))
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

uint32_t d2_store_certificate(
	struct d2_store *s, d1_episode_id episode, d1_repair_id repair,
	const uint8_t certificate[D1_CERTIFICATE_BYTES])
{
	typeof(s->cohorts[0]) *cohort;
	uint32_t status = D1_INVALID;

	if (!s || !certificate ||
	    !memcmp(certificate, (uint8_t[D1_CERTIFICATE_BYTES]){ 0 },
		    D1_CERTIFICATE_BYTES))
		return D1_INVALID;
	pthread_mutex_lock(&s->lock);
	cohort = d2_cohort_find(s, repair.raw);
	if (s->fenced)
		status = D1_IO;
	else if (s->retired)
		status = D1_BAD_PHASE;
	else if (cohort && cohort->count &&
		 d2_object_tombstoned(s, &cohort->members[0].object))
		status = D1_BAD_PHASE;
	else if (!cohort || cohort->phase != D2_COMMITTED ||
		 !cohort->episode_present || cohort->episode_id != episode.raw)
		status = D1_STALE_AUTH;
	else if (d2_certificate_find(s, episode.raw, repair.raw))
		status = D1_BAD_PHASE;
	else {
		status = d2_certificate_control(s, cohort, certificate);
		if (status == D1_OK) {
			d1_fixture_certificate(s->model, certificate);
			if (!d2_certificate_remember(s, episode.raw, repair.raw,
						     certificate))
				status = D1_IO;
			else
				cohort->certificate_installed = true;
		}
		if (status != D1_OK && status != D1_NOSPC)
			s->fenced = true;
	}
	pthread_mutex_unlock(&s->lock);
	return status;
}

uint32_t d2_store_retire(struct d2_store *s, uint32_t reason)
{
	struct d2_wal_header h = { 0 };
	struct d2_control control = { 0 };
	struct d1_cursor cursor;
	uint8_t record[224];
	size_t written;
	uint32_t status;

	if (!s || !reason)
		return D1_INVALID;
	pthread_mutex_lock(&s->lock);
	if (s->fenced) {
		status = D1_IO;
		goto out;
	}
	if (s->retired) {
		status = D1_BAD_PHASE;
		goto out;
	}
	/* The restriction publishes before this operation claims WAL bytes. */
	s->retired = true;
	h.family = D2_REC_CONTROL;
	memcpy(h.store_uuid, s->uuid.bytes, D1_UUID_BYTES);
	memcpy(h.wal_uuid, d2_files_super(s->files)->wal_uuid, D1_UUID_BYTES);
	h.lsn = d2_files_next_lsn(s->files);
	h.ds_incarnation = d2_files_super(s->files)->ds_incarnation;
	control.subtype = D2_CTL_EXPORT_TOMBSTONE;
	control.transition = D2_COMMITTED;
	control.status = D1_OK;
	d2_store_key(s, &control.key);
	control.body_len = 12;
	d1_enc_init(&cursor, control.body, control.body_len);
	d1_enc_u32(&cursor, reason);
	d1_enc_u64(&cursor, h.lsn);
	if (!d1_cursor_ok(&cursor) ||
	    !d2_control_encode(&h, &control, record, sizeof(record), &written)) {
		status = D1_INVALID;
		goto out;
	}
	status = d2_wal_append(s, record, written);
	if (status != D1_OK && status != D1_NOSPC)
		s->fenced = true;
out:
	pthread_mutex_unlock(&s->lock);
	return status;
}

uint32_t d2_store_tombstone_file(struct d2_store *s,
				 const struct d1_objkey *object, uint32_t reason)
{
	struct d2_wal_header h = { 0 };
	struct d2_control control = { 0 };
	struct d1_cursor cursor;
	uint8_t handle[32], file_key[32], record[248];
	size_t written;
	uint32_t i, status;
	bool registered = false;

	if (!s || !object || !reason)
		return D1_INVALID;
	memcpy(handle, object->export_uuid.bytes, 16);
	memcpy(handle + 16, object->object_uuid.bytes, 16);
	d2_file_key(handle, sizeof(handle), file_key);
	pthread_mutex_lock(&s->lock);
	if (s->fenced) {
		status = D1_IO;
		goto out;
	}
	if (s->retired || d2_file_tombstoned(s, file_key)) {
		status = D1_BAD_PHASE;
		goto out;
	}
	for (i = 0; i < s->registered_count; i++)
		if (!memcmp(s->registered[i].file_key, file_key, 32)) {
			registered = true;
			break;
		}
	if (!registered) {
		status = D1_INVALID;
		goto out;
	}
	if (!d2_tombstone_remember(s, file_key)) {
		status = D1_BAD_PHASE;
		goto out;
	}
	h.family = D2_REC_CONTROL;
	memcpy(h.store_uuid, s->uuid.bytes, 16);
	memcpy(h.wal_uuid, d2_files_super(s->files)->wal_uuid, 16);
	h.lsn = d2_files_next_lsn(s->files);
	h.ds_incarnation = d2_files_super(s->files)->ds_incarnation;
	control.subtype = D2_CTL_FILE_TOMBSTONE;
	control.transition = D2_COMMITTED;
	control.status = D1_OK;
	d2_store_key(s, &control.key);
	control.body_len = 36;
	d1_enc_init(&cursor, control.body, control.body_len);
	d1_enc_raw(&cursor, file_key, sizeof(file_key));
	d1_enc_u32(&cursor, reason);
	if (!d1_cursor_ok(&cursor) ||
	    !d2_control_encode(&h, &control, record, sizeof(record), &written)) {
		status = D1_INVALID;
		goto out;
	}
	status = d2_wal_append(s, record, written);
	if (status != D1_OK && status != D1_NOSPC)
		s->fenced = true;
out:
	pthread_mutex_unlock(&s->lock);
	return status;
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

d1_custody_id d2_store_custody_handle(struct d2_store *s, uint64_t raw)
{
	return s ? d1_fixture_custody_handle(s->model, raw) : d1_custody_none();
}

d1_repair_id d2_store_repair_handle(struct d2_store *s, uint64_t raw)
{
	return s ? d1_fixture_repair_handle(s->model, raw) : d1_repair_none();
}

d1_episode_id d2_store_episode_handle(struct d2_store *s, uint64_t raw)
{
	return s ? d1_fixture_episode_handle(s->model, raw) : d1_episode_none();
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

void d2_store_fail_next_wal_write(struct d2_store *s)
{
	if (s && !s->fenced)
		d2_files_fail_next_wal_write(s->files);
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

bool d2_store_postcond(struct d2_store *s, uint64_t raw, uint64_t *index,
		       d1_version_id *successor, bool *consumed)
{
	return s && !s->fenced && index && successor && consumed &&
	       d1_fixture_postcond(s->model,
				   d1_fixture_postcond_handle(s->model, raw),
				   index, successor, consumed);
}

bool d2_store_txn_state(struct d2_store *s, uint64_t raw, uint32_t *phase,
			uint64_t *admission_id)
{
	const typeof(s->work[0]) *work;
	d1_version_id version;
	uint32_t model_phase;
	bool found = false;

	if (!s || !phase || !admission_id)
		return false;
	pthread_mutex_lock(&s->lock);
	work = d2_work_by_txn(s, raw);
	if (work &&
	    d1_fixture_txn_state(s->model,
				 d1_fixture_txn_handle(s->model, raw),
				 &model_phase, &version) &&
	    model_phase == work->phase && version.raw == work->version_id) {
		*phase = work->phase;
		*admission_id = work->admission_id;
		found = true;
	}
	pthread_mutex_unlock(&s->lock);
	return found;
}

bool d2_store_repair_state(struct d2_store *s, uint64_t raw, uint32_t *phase,
			   uint32_t *member_count)
{
	typeof(s->cohorts[0]) *cohort;
	uint32_t model_phase, model_count;

	if (!s || !phase || !member_count)
		return false;
	pthread_mutex_lock(&s->lock);
	cohort = d2_cohort_find(s, raw);
	if (cohort &&
	    d1_fixture_repair_state(
		    s->model, d1_fixture_repair_handle(s->model, raw),
		    &model_phase, &model_count) &&
	    model_phase == cohort->phase && model_count == cohort->count) {
		*phase = cohort->phase;
		*member_count = cohort->count;
	} else {
		cohort = NULL;
	}
	pthread_mutex_unlock(&s->lock);
	return cohort != NULL;
}

uint64_t d2_store_eof(struct d2_store *s, const struct d1_objkey *object)
{
	return s && !s->fenced ? d1_store_eof(s->model, object) : 0;
}

uint64_t d2_store_wal_bytes(struct d2_store *s)
{
	return s && !s->fenced ? d2_files_wal_cursor(s->files) : 0;
}

uint64_t d2_store_wal_promised(struct d2_store *s)
{
	return s && !s->fenced ? s->wal_promised : 0;
}

uint64_t d2_store_recovery_allowance(struct d2_store *s)
{
	return s && !s->fenced ? s->recovery_allowance : 0;
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
