/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "d1_codec.h"
#include "d1_digest.h"
#include "d2_store.h"

static unsigned int checks;
static unsigned int failures;

static void check(bool value, const char *name)
{
	checks++;
	if (!value) {
		failures++;
		fprintf(stderr, "not ok %u - %s\n", checks, name);
	}
}

static void fill(uint8_t *p, size_t len, uint8_t seed)
{
	size_t i;

	for (i = 0; i < len; i++)
		p[i] = (uint8_t)(seed + i);
}

static bool same_result(const struct d1_result *a, const struct d1_result *b)
{
	uint32_t i, j;

	if (memcmp(a->key.origin.bytes, b->key.origin.bytes, D1_UUID_BYTES) ||
	    a->key.sequence != b->key.sequence ||
	    a->key.ordinal != b->key.ordinal ||
	    a->index_epoch != b->index_epoch || a->eof != b->eof ||
	    a->disposition != b->disposition || a->count != b->count)
		return false;
	for (i = 0; i < a->count; i++) {
		const struct d1_entry_result *x = &a->entries[i];
		const struct d1_entry_result *y = &b->entries[i];

		if (x->status != y->status ||
		    x->version_present != y->version_present ||
		    x->version.raw != y->version.raw ||
		    x->txn_present != y->txn_present ||
		    x->txn.raw != y->txn.raw ||
		    x->cohort_present != y->cohort_present ||
		    x->cohort.raw != y->cohort.raw ||
		    x->member_txn_count != y->member_txn_count ||
		    x->postcond_present != y->postcond_present ||
		    x->postcond.raw != y->postcond.raw ||
		    x->episode_present != y->episode_present ||
		    x->episode.raw != y->episode.raw ||
		    x->member_present != y->member_present ||
		    x->member != y->member ||
		    x->guard.never_written != y->guard.never_written ||
		    x->guard.generation != y->guard.generation ||
		    x->guard.writer != y->guard.writer ||
		    x->owner.cohort.raw != y->owner.cohort.raw ||
		    x->owner.writer != y->owner.writer ||
		    x->owner.co_id != y->owner.co_id ||
		    x->stability != y->stability ||
		    x->activated != y->activated || x->phase != y->phase ||
		    memcmp(x->verifier, y->verifier, D1_VERIFIER_BYTES) ||
		    x->disposition != y->disposition)
			return false;
		for (j = 0; j < x->member_txn_count; j++)
			if (x->member_txn[j].raw != y->member_txn[j].raw)
				return false;
	}
	return true;
}

static void write_request(struct d1_envelope *env, uint64_t sequence,
			  uint64_t index, uint32_t co_id,
			  const struct d1_guard *guard, const uint8_t *payload,
			  uint32_t len)
{
	uint32_t crc = d1_crc32c(payload, len);

	env->key.sequence = sequence;
	memset(&env->body, 0, sizeof(env->body));
	env->body.write.count = 1;
	env->body.write.stability = D1_FILE_SYNC;
	env->body.write.activate = true;
	env->body.write.entries[0].index = index;
	env->body.write.entries[0].owner.cohort.raw = 1;
	env->body.write.entries[0].owner.writer = 17;
	env->body.write.entries[0].owner.co_id = co_id;
	env->body.write.entries[0].guard_check = true;
	env->body.write.entries[0].expected = *guard;
	env->body.write.entries[0].payload = payload;
	env->body.write.entries[0].payload_len = len;
	env->body.write.entries[0].checksum.alg = D1_CKSUM_CRC32C;
	env->body.write.entries[0].checksum.len = 4;
	env->body.write.entries[0].checksum.digest[0] = (uint8_t)(crc >> 24);
	env->body.write.entries[0].checksum.digest[1] = (uint8_t)(crc >> 16);
	env->body.write.entries[0].checksum.digest[2] = (uint8_t)(crc >> 8);
	env->body.write.entries[0].checksum.digest[3] = (uint8_t)crc;
}

static bool entry_has_admission(int dirfd, uint64_t wal_bytes,
				const struct d2_binding *binding, uint64_t id)
{
	struct d2_wal_header header;
	struct d2_entry entry;
	uint8_t *wal;
	uint8_t expected[12] = { 'T', 'A', 'T', 'E' };
	uint64_t at = 0;
	unsigned int i;
	int fd = -1;
	bool found = false;

	for (i = 0; i < 8; i++)
		expected[4 + i] = (uint8_t)(id >> (56 - 8 * i));
	wal = malloc((size_t)wal_bytes);
	if (!wal)
		return false;
	fd = openat(dirfd, "wal", O_RDONLY | O_CLOEXEC);
	if (fd < 0 ||
	    pread(fd, wal, (size_t)wal_bytes, 0) != (ssize_t)wal_bytes)
		goto out;
	while (at < wal_bytes &&
	       d2_wal_header_decode(wal + at, (size_t)(wal_bytes - at),
				    binding->store_uuid, binding->wal_uuid,
				    &header)) {
		if (header.family == D2_REC_ENTRY &&
		    d2_entry_decode(wal + at, header.total_bytes, &header,
				    &entry) &&
		    entry.admission.client_id == id &&
		    entry.admission.stateid_seqid == UINT32_C(0x44324253) &&
		    !memcmp(entry.admission.stateid_other, expected,
			    sizeof(expected)))
			found = true;
		at += header.total_bytes;
	}
out:
	if (fd >= 0)
		close(fd);
	free(wal);
	return found;
}

static bool retarget_refused_entry(int dirfd, uint64_t wal_bytes,
				   const struct d2_binding *binding,
				   uint64_t id, const uint8_t file_key[32])
{
	struct d2_wal_header header;
	struct d2_entry entry;
	uint8_t *record;
	uint64_t at = 0;
	int fd;
	bool changed = false;

	record = malloc(D2_MAX_RECORD_BYTES);
	if (!record)
		return false;
	fd = openat(dirfd, "wal", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		free(record);
		return false;
	}
	while (at < wal_bytes) {
		size_t want = wal_bytes - at > D2_MAX_RECORD_BYTES ?
				      D2_MAX_RECORD_BYTES :
				      (size_t)(wal_bytes - at);
		ssize_t got = pread(fd, record, want, at);

		if (got < (ssize_t)D2_WAL_HEADER_BYTES ||
		    !d2_wal_header_decode(record, (size_t)got,
					  binding->store_uuid,
					  binding->wal_uuid, &header) ||
		    header.total_bytes > wal_bytes - at)
			break;
		if (header.family == D2_REC_ENTRY &&
		    header.total_bytes == D2_ENTRY_RECORD_BYTES &&
		    d2_entry_decode(record, D2_ENTRY_RECORD_BYTES, &header,
				    &entry) &&
		    entry.transition == D2_REFUSED &&
		    entry.admission.client_id == id) {
			memcpy(entry.file_key, file_key,
			       sizeof(entry.file_key));
			if (!d2_entry_encode(&header, &entry, record) ||
			    pwrite(fd, record, D2_ENTRY_RECORD_BYTES, at) !=
				    (ssize_t)D2_ENTRY_RECORD_BYTES)
				break;
			changed = true;
			break;
		}
		at += header.total_bytes;
	}
	close(fd);
	free(record);
	return changed;
}

static bool authority_precedes_entry(int dirfd, uint64_t wal_bytes,
				     const struct d2_binding *binding,
				     uint64_t admission_id)
{
	struct d2_wal_header header;
	struct d2_control control;
	struct d2_entry entry;
	uint8_t *wal;
	uint64_t at = 0;
	int fd = -1;
	bool trust = false, authority = false, ok = false;

	wal = malloc((size_t)wal_bytes);
	if (!wal)
		return false;
	fd = openat(dirfd, "wal", O_RDONLY | O_CLOEXEC);
	if (fd < 0 ||
	    pread(fd, wal, (size_t)wal_bytes, 0) != (ssize_t)wal_bytes)
		goto out;
	while (at < wal_bytes &&
	       d2_wal_header_decode(wal + at, (size_t)(wal_bytes - at),
				    binding->store_uuid, binding->wal_uuid,
				    &header)) {
		if (header.family == D2_REC_CONTROL &&
		    d2_control_decode(wal + at, header.total_bytes, &header,
				      &control) &&
		    control.admission_client_id == admission_id) {
			if (control.subtype == D2_CTL_TRUST_STATEID)
				trust = !authority;
			else if (control.subtype == D2_CTL_AUTHORITY_ADMIT)
				authority = trust;
		} else if (header.family == D2_REC_ENTRY &&
			   d2_entry_decode(wal + at, header.total_bytes,
					   &header, &entry) &&
			   entry.admission.client_id == admission_id) {
			ok = trust && authority;
			break;
		}
		at += header.total_bytes;
	}
out:
	if (fd >= 0)
		close(fd);
	free(wal);
	return ok;
}

static bool entry_wal_semantics_valid(int dirfd, uint64_t wal_bytes,
				      const struct d2_binding *binding)
{
	struct d2_wal_header header;
	struct d2_entry entry;
	uint8_t *wal;
	uint64_t at = 0;
	uint32_t entries = 0;
	int fd = -1;
	bool ok = false;

	wal = malloc((size_t)wal_bytes);
	if (!wal)
		return false;
	fd = openat(dirfd, "wal", O_RDONLY | O_CLOEXEC);
	if (fd < 0 ||
	    pread(fd, wal, (size_t)wal_bytes, 0) != (ssize_t)wal_bytes)
		goto out;
	while (at < wal_bytes) {
		if (!d2_wal_header_decode(wal + at, (size_t)(wal_bytes - at),
					  binding->store_uuid,
					  binding->wal_uuid, &header))
			goto out;
		if (header.family == D2_REC_ENTRY) {
			if (!d2_entry_decode(wal + at, header.total_bytes,
					     &header, &entry) ||
			    entry.generation != entry.result_guard_generation ||
			    (!entry.predecessor_present &&
			     (entry.predecessor_object_id ||
			      entry.predecessor_generation)) ||
			    (!entry.postcond_present && entry.postcond_id))
				goto out;
			entries++;
		}
		at += header.total_bytes;
	}
	ok = entries != 0;
out:
	if (fd >= 0)
		close(fd);
	free(wal);
	return ok;
}

struct authority_race {
	struct d2_store *store;
	struct d1_envelope transition;
	struct d1_result transition_result;
	struct d1_uuid issuer;
	d1_admission_id actor;
	uint64_t epoch;
	atomic_bool go;
	atomic_bool first_done;
	int forced_order;
	uint32_t transition_status;
	uint32_t revoke_status;
};

static void *race_transition(void *arg)
{
	struct authority_race *race = arg;

	while (!atomic_load_explicit(&race->go, memory_order_acquire))
		sched_yield();
	if (race->forced_order == 2)
		while (!atomic_load_explicit(&race->first_done,
					     memory_order_acquire))
			sched_yield();
	race->transition_status = d2_store_apply(race->store, &race->transition,
						 &race->transition_result);
	if (race->forced_order == 1)
		atomic_store_explicit(&race->first_done, true,
				      memory_order_release);
	return NULL;
}

static void *race_revoke(void *arg)
{
	struct authority_race *race = arg;

	while (!atomic_load_explicit(&race->go, memory_order_acquire))
		sched_yield();
	if (race->forced_order == 1)
		while (!atomic_load_explicit(&race->first_done,
					     memory_order_acquire))
			sched_yield();
	race->revoke_status = d2_store_revoke_authority(
		race->store, race->actor, &race->issuer, race->epoch, 1);
	if (race->forced_order == 2)
		atomic_store_explicit(&race->first_done, true,
				      memory_order_release);
	return NULL;
}

static bool authority_race_log(int dirfd, uint64_t begin, uint64_t end,
			       const struct d2_binding *binding,
			       uint64_t txn_id, uint64_t epoch,
			       bool *revoke_first)
{
	struct d2_wal_header header;
	struct d2_control control;
	struct d2_entry entry;
	struct d1_cursor cursor;
	uint8_t *wal;
	uint8_t issuer[D1_UUID_BYTES];
	uint64_t at = begin, entry_lsn = 0, revoke_lsn = 0;
	uint64_t target_epoch;
	uint32_t entry_status = 0, entry_transition = 0;
	uint32_t reason;
	unsigned int entries = 0, revokes = 0;
	int fd = -1;
	bool ok = false;

	wal = malloc((size_t)end);
	if (!wal)
		return false;
	fd = openat(dirfd, "wal", O_RDONLY | O_CLOEXEC);
	if (fd < 0 || pread(fd, wal, (size_t)end, 0) != (ssize_t)end)
		goto out;
	while (at < end && d2_wal_header_decode(wal + at, (size_t)(end - at),
						binding->store_uuid,
						binding->wal_uuid, &header)) {
		if (header.family == D2_REC_CONTROL &&
		    d2_control_decode(wal + at, header.total_bytes, &header,
				      &control) &&
		    control.subtype == D2_CTL_AUTHORITY_REVOKE &&
		    control.body_len == 28) {
			d1_dec_init(&cursor, control.body, control.body_len);
			if (d1_dec_raw(&cursor, issuer, sizeof(issuer)) &&
			    d1_dec_u64(&cursor, &target_epoch) &&
			    d1_dec_u32(&cursor, &reason) &&
			    d1_dec_finished(&cursor) && target_epoch == epoch &&
			    reason) {
				revokes++;
				revoke_lsn = header.lsn;
			}
		} else if (header.family == D2_REC_ENTRY &&
			   d2_entry_decode(wal + at, header.total_bytes,
					   &header, &entry) &&
			   entry.txn_id == txn_id) {
			entries++;
			entry_lsn = header.lsn;
			entry_status = entry.status;
			entry_transition = entry.transition;
		}
		at += header.total_bytes;
	}
	if (at != end || entries != 1 || revokes != 1 ||
	    entry_lsn == revoke_lsn)
		goto out;
	*revoke_first = revoke_lsn < entry_lsn;
	ok = *revoke_first ?
		     entry_transition == D2_REFUSED &&
			     entry_status == D1_STALE_AUTH :
		     entry_transition == D2_FINALIZED && entry_status == D1_OK;
out:
	if (fd >= 0)
		close(fd);
	free(wal);
	return ok;
}

struct rebind_race {
	struct d2_store *store;
	struct d1_envelope transition;
	struct d1_envelope recovery;
	struct d1_result transition_result;
	struct d1_result recovery_result;
	atomic_bool go;
	atomic_bool first_done;
	int forced_order;
	uint32_t transition_status;
	uint32_t recovery_status;
};

static void *race_rebind_transition(void *arg)
{
	struct rebind_race *race = arg;

	while (!atomic_load_explicit(&race->go, memory_order_acquire))
		sched_yield();
	if (race->forced_order == 2)
		while (!atomic_load_explicit(&race->first_done,
					     memory_order_acquire))
			sched_yield();
	race->transition_status = d2_store_apply(race->store, &race->transition,
						 &race->transition_result);
	if (race->forced_order == 1)
		atomic_store_explicit(&race->first_done, true,
				      memory_order_release);
	return NULL;
}

static void *race_recovery_admit(void *arg)
{
	struct rebind_race *race = arg;

	while (!atomic_load_explicit(&race->go, memory_order_acquire))
		sched_yield();
	if (race->forced_order == 1)
		while (!atomic_load_explicit(&race->first_done,
					     memory_order_acquire))
			sched_yield();
	race->recovery_status = d2_store_apply(race->store, &race->recovery,
					       &race->recovery_result);
	if (race->forced_order == 2)
		atomic_store_explicit(&race->first_done, true,
				      memory_order_release);
	return NULL;
}

static bool rebind_race_log(int dirfd, uint64_t begin, uint64_t end,
			    const struct d2_binding *binding, uint64_t txn_id,
			    bool *rebind_first)
{
	struct d2_wal_header header;
	struct d2_control control;
	struct d2_entry entry;
	uint8_t *wal;
	uint64_t at = begin, entry_lsn = 0, rebind_lsn = 0;
	uint32_t entry_status = 0, entry_transition = 0;
	unsigned int entries = 0, rebinds = 0;
	int fd = -1;
	bool ok = false;

	wal = malloc((size_t)end);
	if (!wal)
		return false;
	fd = openat(dirfd, "wal", O_RDONLY | O_CLOEXEC);
	if (fd < 0 || pread(fd, wal, (size_t)end, 0) != (ssize_t)end)
		goto out;
	while (at < end && d2_wal_header_decode(wal + at, (size_t)(end - at),
						binding->store_uuid,
						binding->wal_uuid, &header)) {
		if (header.family == D2_REC_CONTROL &&
		    d2_control_decode(wal + at, header.total_bytes, &header,
				      &control) &&
		    control.subtype == D2_CTL_RECOVERY_ADMIT) {
			rebinds++;
			rebind_lsn = header.lsn;
		} else if (header.family == D2_REC_ENTRY &&
			   d2_entry_decode(wal + at, header.total_bytes,
					   &header, &entry) &&
			   entry.txn_id == txn_id) {
			entries++;
			entry_lsn = header.lsn;
			entry_status = entry.status;
			entry_transition = entry.transition;
		}
		at += header.total_bytes;
	}
	if (at != end || entries != 1 || rebinds != 1 ||
	    entry_lsn == rebind_lsn)
		goto out;
	*rebind_first = rebind_lsn < entry_lsn;
	ok = *rebind_first ?
		     entry_transition == D2_REFUSED &&
			     entry_status == D1_STALE_AUTH :
		     entry_transition == D2_FINALIZED && entry_status == D1_OK;
out:
	if (fd >= 0)
		close(fd);
	free(wal);
	return ok;
}

static bool repair_revoke_race_log(int dirfd, uint64_t begin, uint64_t end,
				   const struct d2_binding *binding,
				   uint64_t cohort_id, bool *revoke_first)
{
	struct d2_wal_header header;
	struct d2_control control;
	struct d2_cohort cohort;
	uint8_t *wal;
	uint64_t at = begin, cohort_lsn = 0, revoke_lsn = 0;
	uint32_t cohort_status = 0, cohort_transition = 0, cohort_flags = 0;
	unsigned int cohorts = 0, revokes = 0;
	int fd = -1;
	bool ok = false;

	wal = malloc((size_t)end);
	if (!wal)
		return false;
	fd = openat(dirfd, "wal", O_RDONLY | O_CLOEXEC);
	if (fd < 0 || pread(fd, wal, (size_t)end, 0) != (ssize_t)end)
		goto out;
	while (at < end && d2_wal_header_decode(wal + at, (size_t)(end - at),
						binding->store_uuid,
						binding->wal_uuid, &header)) {
		if (header.family == D2_REC_CONTROL &&
		    d2_control_decode(wal + at, header.total_bytes, &header,
				      &control) &&
		    control.subtype == D2_CTL_AUTHORITY_REVOKE) {
			revokes++;
			revoke_lsn = header.lsn;
		} else if (header.family == D2_REC_COHORT &&
			   d2_cohort_decode(wal + at, header.total_bytes,
					    &header, &cohort) &&
			   cohort.cohort_id == cohort_id) {
			cohorts++;
			cohort_lsn = header.lsn;
			cohort_status = cohort.status;
			cohort_transition = cohort.transition;
			cohort_flags = cohort.flags;
		}
		at += header.total_bytes;
	}
	if (at != end || cohorts != 1 || revokes != 1 ||
	    cohort_lsn == revoke_lsn)
		goto out;
	*revoke_first = revoke_lsn < cohort_lsn;
	ok = !(cohort_flags & 2u) &&
	     (*revoke_first ? cohort_transition == D2_REFUSED &&
				      cohort_status == D1_STALE_AUTH :
			      cohort_transition == D2_PREPARED &&
				      cohort_status == D1_OK);
out:
	if (fd >= 0)
		close(fd);
	free(wal);
	return ok;
}

static bool repair_failure_revoke_race_log(int dirfd, uint64_t begin,
					   uint64_t end,
					   const struct d2_binding *binding,
					   uint64_t cohort_id,
					   bool *revoke_first)
{
	struct d2_wal_header header;
	struct d2_control control;
	struct d2_cohort cohort;
	uint8_t *wal;
	uint64_t at = begin, cohort_lsn = 0, revoke_lsn = 0;
	uint32_t cohort_status = 0, cohort_transition = 0, cohort_flags = 0;
	unsigned int cohorts = 0, revokes = 0;
	int fd = -1;
	bool ok = false;

	wal = malloc((size_t)end);
	if (!wal)
		return false;
	fd = openat(dirfd, "wal", O_RDONLY | O_CLOEXEC);
	if (fd < 0 || pread(fd, wal, (size_t)end, 0) != (ssize_t)end)
		goto out;
	while (at < end && d2_wal_header_decode(wal + at, (size_t)(end - at),
						binding->store_uuid,
						binding->wal_uuid, &header)) {
		if (header.family == D2_REC_CONTROL &&
		    d2_control_decode(wal + at, header.total_bytes, &header,
				      &control) &&
		    control.subtype == D2_CTL_AUTHORITY_REVOKE) {
			revokes++;
			revoke_lsn = header.lsn;
		} else if (header.family == D2_REC_COHORT &&
			   d2_cohort_decode(wal + at, header.total_bytes,
					    &header, &cohort) &&
			   cohort.cohort_id == cohort_id) {
			cohorts++;
			cohort_lsn = header.lsn;
			cohort_status = cohort.status;
			cohort_transition = cohort.transition;
			cohort_flags = cohort.flags;
		}
		at += header.total_bytes;
	}
	if (at != end || cohorts != 1 || revokes != 1 ||
	    cohort_lsn == revoke_lsn)
		goto out;
	*revoke_first = revoke_lsn < cohort_lsn;
	ok = *revoke_first ?
		     !(cohort_flags & 2u) && cohort_transition == D2_REFUSED &&
			     cohort_status == D1_STALE_AUTH :
		     (cohort_flags & 2u) && cohort_transition == D2_ABORTED &&
			     cohort_status == D1_CHECKSUM;
out:
	if (fd >= 0)
		close(fd);
	free(wal);
	return ok;
}

static bool postcond_revoke_race_log(int dirfd, uint64_t begin, uint64_t end,
				     const struct d2_binding *binding,
				     uint64_t txn_id, bool *revoke_first)
{
	struct d2_wal_header header;
	struct d2_control control;
	struct d2_entry entry;
	uint8_t *wal;
	uint64_t at = begin, entry_lsn = 0, postcond_lsn = 0, revoke_lsn = 0;
	uint32_t entry_status = 0, entry_transition = 0;
	unsigned int entries = 0, postconds = 0, revokes = 0;
	int fd = -1;
	bool ok = false;

	wal = malloc((size_t)end);
	if (!wal)
		return false;
	fd = openat(dirfd, "wal", O_RDONLY | O_CLOEXEC);
	if (fd < 0 || pread(fd, wal, (size_t)end, 0) != (ssize_t)end)
		goto out;
	while (at < end && d2_wal_header_decode(wal + at, (size_t)(end - at),
						binding->store_uuid,
						binding->wal_uuid, &header)) {
		if (header.family == D2_REC_CONTROL &&
		    d2_control_decode(wal + at, header.total_bytes, &header,
				      &control)) {
			if (control.subtype == D2_CTL_AUTHORITY_REVOKE) {
				revokes++;
				revoke_lsn = header.lsn;
			} else if (control.subtype == D2_CTL_POSTCOND) {
				postconds++;
				postcond_lsn = header.lsn;
			}
		} else if (header.family == D2_REC_ENTRY &&
			   d2_entry_decode(wal + at, header.total_bytes,
					   &header, &entry) &&
			   entry.txn_id == txn_id) {
			entries++;
			entry_lsn = header.lsn;
			entry_status = entry.status;
			entry_transition = entry.transition;
		}
		at += header.total_bytes;
	}
	if (at != end || entries != 1 || revokes != 1 ||
	    entry_lsn == revoke_lsn)
		goto out;
	*revoke_first = revoke_lsn < entry_lsn;
	ok = *revoke_first ? postconds == 0 && entry_transition == D2_REFUSED &&
				     entry_status == D1_STALE_AUTH :
			     postconds == 1 && postcond_lsn + 1 == entry_lsn &&
				     entry_lsn < revoke_lsn &&
				     entry_transition == D2_REFUSED &&
				     entry_status == D1_NO_PREDECESSOR;
out:
	if (fd >= 0)
		close(fd);
	free(wal);
	return ok;
}

static bool repair_rebind_race_log(int dirfd, uint64_t begin, uint64_t end,
				   const struct d2_binding *binding,
				   uint64_t cohort_id, uint32_t terminal,
				   uint32_t recovery_refusal,
				   bool *rebind_first)
{
	struct d2_wal_header header;
	struct d2_control control;
	struct d2_cohort cohort;
	uint8_t *wal;
	uint64_t at = begin, cohort_lsn = 0, rebind_lsn = 0;
	uint32_t cohort_status = 0, cohort_transition = 0, cohort_flags = 0;
	uint32_t rebind_status = 0, rebind_transition = 0;
	unsigned int cohorts = 0, rebinds = 0;
	int fd = -1;
	bool ok = false;

	wal = malloc((size_t)end);
	if (!wal)
		return false;
	fd = openat(dirfd, "wal", O_RDONLY | O_CLOEXEC);
	if (fd < 0 || pread(fd, wal, (size_t)end, 0) != (ssize_t)end)
		goto out;
	while (at < end && d2_wal_header_decode(wal + at, (size_t)(end - at),
						binding->store_uuid,
						binding->wal_uuid, &header)) {
		if (header.family == D2_REC_CONTROL &&
		    d2_control_decode(wal + at, header.total_bytes, &header,
				      &control) &&
		    control.subtype == D2_CTL_RECOVERY_ADMIT) {
			rebinds++;
			rebind_lsn = header.lsn;
			rebind_status = control.status;
			rebind_transition = control.transition;
		} else if (header.family == D2_REC_COHORT &&
			   d2_cohort_decode(wal + at, header.total_bytes,
					    &header, &cohort) &&
			   cohort.cohort_id == cohort_id) {
			cohorts++;
			cohort_lsn = header.lsn;
			cohort_status = cohort.status;
			cohort_transition = cohort.transition;
			cohort_flags = cohort.flags;
		}
		at += header.total_bytes;
	}
	if (at != end || cohorts != 1 || rebinds != 1 ||
	    cohort_lsn == rebind_lsn)
		goto out;
	*rebind_first = rebind_lsn < cohort_lsn;
	ok = !(cohort_flags & 2u) &&
	     (*rebind_first ?
		      rebind_transition == D2_COMMITTED &&
			      rebind_status == D1_OK &&
			      cohort_transition == D2_REFUSED &&
			      cohort_status == D1_STALE_AUTH :
		      cohort_transition == terminal && cohort_status == D1_OK &&
			      rebind_transition == D2_REFUSED &&
			      rebind_status == recovery_refusal);
out:
	if (fd >= 0)
		close(fd);
	free(wal);
	return ok;
}

static bool
admitted_error_repair(struct d2_store *store, const struct d1_objkey *object,
		      d1_admission_id admission, uint32_t writer,
		      uint64_t index, uint8_t origin_seed,
		      const uint8_t *payload, uint32_t payload_len,
		      struct d1_envelope *prepare, d1_repair_id *cohort_out,
		      d1_custody_id *custody_out, d1_version_id *version_out,
		      d1_episode_id *episode_out)
{
	struct d1_envelope env = { 0 };
	struct d1_result result;
	struct d1_guard guard = { .never_written = true };
	d1_custody_id custody;
	d1_episode_id episode;
	d1_txn_id txn;
	d1_version_id version;

	env.object = *object;
	env.admission = admission;
	env.incarnation = d2_store_incarnation(store);
	fill(env.key.origin.bytes, sizeof(env.key.origin.bytes), origin_seed);
	env.op = D1_OP_WRITE_BATCH;
	write_request(&env, 1, index, 400 + (uint32_t)index, &guard, payload,
		      payload_len);
	env.body.write.entries[0].owner.writer = writer;
	if (d2_store_apply(store, &env, &result) != D1_OK ||
	    result.entries[0].status != D1_OK)
		return false;
	version = result.entries[0].version;
	if (version_out)
		*version_out = version;
	custody = d2_store_custody(store, version);
	if (!d1_custody_live(custody))
		return false;
	if (custody_out)
		*custody_out = custody;
	memset(&env.body, 0, sizeof(env.body));
	env.key.sequence = 2;
	env.op = D1_OP_MARK_ERROR;
	env.body.repair.range_begin = index;
	env.body.repair.range_end = index + 1;
	env.body.repair.count = 1;
	env.body.repair.entries[0].index = index;
	env.body.repair.entries[0].owner.cohort.raw = 2;
	env.body.repair.entries[0].owner.writer = writer;
	env.body.repair.entries[0].owner.co_id = 500 + (uint32_t)index;
	env.body.repair.entries[0].custody_present = true;
	env.body.repair.entries[0].custody = custody;
	env.body.repair.entries[0].successor_present = true;
	env.body.repair.entries[0].successor = version;
	if (d2_store_apply(store, &env, &result) != D1_OK ||
	    result.entries[0].status != D1_OK ||
	    !result.entries[0].episode_present)
		return false;
	episode = result.entries[0].episode;
	if (episode_out)
		*episode_out = episode;
	memset(&env.body, 0, sizeof(env.body));
	env.key.sequence = 3;
	env.op = D1_OP_BEGIN_REPAIR;
	env.body.repair.range_begin = index;
	env.body.repair.range_end = index + 1;
	env.body.repair.count = 1;
	env.body.repair.episode_present = true;
	env.body.repair.episode = episode;
	env.body.repair.entries[0].index = index;
	env.body.repair.entries[0].owner.cohort.raw = 3;
	env.body.repair.entries[0].owner.writer = writer;
	env.body.repair.entries[0].owner.co_id = 600 + (uint32_t)index;
	env.body.repair.entries[0].mode = D1_REPAIR_ERROR;
	env.body.repair.entries[0].custody_present = true;
	env.body.repair.entries[0].custody = custody;
	env.body.repair.entries[0].successor_present = true;
	env.body.repair.entries[0].successor = version;
	if (d2_store_apply(store, &env, &result) != D1_OK ||
	    result.entries[0].status != D1_OK ||
	    result.entries[0].phase != D2_ADMITTED ||
	    !result.entries[0].cohort_present ||
	    result.entries[0].member_txn_count != 1)
		return false;
	*cohort_out = result.entries[0].cohort;
	txn = result.entries[0].member_txn[0];
	*prepare = env;
	memset(&prepare->body, 0, sizeof(prepare->body));
	prepare->key.sequence = 4;
	prepare->op = D1_OP_PREPARE_REPAIR;
	prepare->body.repair.range_begin = index;
	prepare->body.repair.range_end = index + 1;
	prepare->body.repair.count = 1;
	prepare->body.repair.cohort_present = true;
	prepare->body.repair.cohort = *cohort_out;
	prepare->body.repair.entries[0].index = index;
	prepare->body.repair.entries[0].owner.cohort.raw = 3;
	prepare->body.repair.entries[0].owner.writer = writer;
	prepare->body.repair.entries[0].owner.co_id = 600 + (uint32_t)index;
	prepare->body.repair.entries[0].txn_present = true;
	prepare->body.repair.entries[0].txn = txn;
	prepare->body.repair.entries[0].payload_present = true;
	prepare->body.repair.entries[0].payload = payload;
	prepare->body.repair.entries[0].payload_len = payload_len;
	return d1_checksum_compute(D1_CKSUM_CRC32C, payload, payload_len,
				   &prepare->body.repair.entries[0].checksum);
}

static bool committed_error_repair(struct d2_store *store,
				   const struct d1_objkey *object,
				   d1_admission_id actor,
				   d1_admission_id admission, uint32_t writer,
				   uint64_t index, uint8_t origin_seed,
				   const uint8_t *payload, uint32_t payload_len,
				   struct d1_envelope *unlock,
				   d1_repair_id *cohort_out, d1_txn_id *txn_out)
{
	struct d1_envelope env;
	struct d1_result result;
	d1_custody_id custody;
	d1_episode_id episode;
	d1_version_id predecessor;
	struct d1_opkey certificate_key = { 0 };
	uint8_t certificate[32];

	if (!admitted_error_repair(store, object, admission, writer, index,
				   origin_seed, payload, payload_len, &env,
				   cohort_out, &custody, &predecessor,
				   &episode))
		return false;
	*txn_out = env.body.repair.entries[0].txn;
	if (d2_store_apply(store, &env, &result) != D1_OK ||
	    result.entries[0].status != D1_OK ||
	    result.entries[0].phase != D2_PREPARED)
		return false;
	memset(&env.body, 0, sizeof(env.body));
	env.key.sequence = 5;
	env.op = D1_OP_FINALIZE_REPAIR;
	env.body.repair.range_begin = index;
	env.body.repair.range_end = index + 1;
	env.body.repair.count = 1;
	env.body.repair.cohort_present = true;
	env.body.repair.cohort = *cohort_out;
	env.body.repair.verifier_present = true;
	d2_store_verifier(store, env.body.repair.prior_verifier);
	env.body.repair.entries[0].index = index;
	env.body.repair.entries[0].owner.cohort.raw = 3;
	env.body.repair.entries[0].owner.writer = writer;
	env.body.repair.entries[0].owner.co_id = 600 + (uint32_t)index;
	env.body.repair.entries[0].custody_present = true;
	env.body.repair.entries[0].custody = custody;
	env.body.repair.entries[0].txn_present = true;
	env.body.repair.entries[0].txn = *txn_out;
	env.body.repair.entries[0].predecessor_present = true;
	env.body.repair.entries[0].predecessor = predecessor;
	if (d2_store_apply(store, &env, &result) != D1_OK ||
	    result.entries[0].status != D1_OK ||
	    result.entries[0].phase != D2_FINALIZED)
		return false;
	env.key.sequence = 6;
	env.op = D1_OP_COMMIT_REPAIR;
	d2_store_verifier(store, env.body.repair.prior_verifier);
	if (d2_store_apply(store, &env, &result) != D1_OK ||
	    result.entries[0].status != D1_OK ||
	    result.entries[0].phase != D2_COMMITTED)
		return false;
	fill(certificate, sizeof(certificate), (uint8_t)(origin_seed + 1));
	fill(certificate_key.origin.bytes, D1_UUID_BYTES,
	     (uint8_t)(origin_seed + 2));
	certificate_key.sequence = 1;
	if (d2_store_certificate(store, actor, &certificate_key, episode,
				 *cohort_out, certificate) != D1_OK)
		return false;
	memset(&env.body, 0, sizeof(env.body));
	env.key.sequence = 7;
	env.op = D1_OP_CLEAR_ERROR;
	env.body.repair.range_begin = index;
	env.body.repair.range_end = index + 1;
	env.body.repair.count = 1;
	env.body.repair.cohort_present = true;
	env.body.repair.cohort = *cohort_out;
	env.body.repair.episode_present = true;
	env.body.repair.episode = episode;
	env.body.repair.certificate_present = true;
	memcpy(env.body.repair.certificate, certificate, sizeof(certificate));
	env.body.repair.entries[0].index = index;
	env.body.repair.entries[0].owner.cohort.raw = 3;
	env.body.repair.entries[0].owner.writer = writer;
	env.body.repair.entries[0].owner.co_id = 600 + (uint32_t)index;
	env.body.repair.entries[0].custody_present = true;
	env.body.repair.entries[0].custody = custody;
	if (d2_store_apply(store, &env, &result) != D1_OK ||
	    result.entries[0].status != D1_OK)
		return false;
	*unlock = env;
	memset(&unlock->body, 0, sizeof(unlock->body));
	unlock->key.sequence = 8;
	unlock->op = D1_OP_UNLOCK;
	unlock->body.repair.range_begin = index;
	unlock->body.repair.range_end = index + 1;
	unlock->body.repair.count = 1;
	unlock->body.repair.cohort_present = true;
	unlock->body.repair.cohort = *cohort_out;
	unlock->body.repair.entries[0].index = index;
	unlock->body.repair.entries[0].owner.cohort.raw = 3;
	unlock->body.repair.entries[0].owner.writer = writer;
	unlock->body.repair.entries[0].owner.co_id = 600 + (uint32_t)index;
	unlock->body.repair.entries[0].custody_present = true;
	unlock->body.repair.entries[0].custody = custody;
	return true;
}

int main(void)
{
	const char *root = getenv("D2_TEST_ROOT");
	struct d2_store_config config = { 0 };
	struct d2_store_rebind reopen = { 0 };
	struct d2_binding binding = { 0 };
	struct d2_store *store = NULL;
	struct d1_objkey object = { 0 };
	struct d1_envelope env = { 0 };
	struct d1_envelope committed_rollback;
	struct d1_envelope conflict;
	struct d1_envelope aborted_prepare;
	struct d1_envelope expired;
	struct d1_envelope error_unlock;
	struct d1_envelope nopre;
	struct d1_envelope marked;
	struct d1_envelope repair_unlock;
	struct d1_envelope second_error_unlock;
	struct d1_envelope stale_incarnation;
	struct d1_envelope recovery;
	struct d1_envelope retired_exact;
	struct d1_envelope refused;
	struct d1_envelope unbound_refused;
	struct d1_envelope refused_clear;
	struct d1_envelope refused_mark;
	struct d1_envelope refused_repair;
	struct d1_envelope unsupported;
	struct d1_result result = { 0 };
	struct d1_result durable_result = { 0 };
	struct d1_result refused_result = { 0 };
	struct d1_result damaged_bound_result = { 0 };
	struct d1_result damaged_unbound_result = { 0 };
	struct d1_result nopre_result = { 0 }, unlock_result = { 0 };
	struct d1_result refused_recovery_result = { 0 }, marked_result = { 0 };
	struct d1_result refused_mark_result = { 0 },
			 refused_repair_result = { 0 };
	struct d1_result aborted_result = { 0 }, refused_clear_result = { 0 };
	struct d1_result error_unlock_result = { 0 };
	struct d1_opkey certificate_key = { 0 };
	struct d1_opkey stale_certificate_key = { 0 };
	struct d1_opkey bare_certificate_key = { 0 };
	d1_admission_id admission;
	d1_admission_id control_admission, fresh_admission, next_control,
		next_fresh;
	d1_admission_id stale_certificate_actor = { 0 };
	d1_admission_id bare_certificate_actor = { 0 };
	d1_admission_id second_error_admission = { 0 };
	d1_custody_id custody, custody2;
	d1_episode_id episode;
	d1_postcond_id postcond;
	d1_repair_id repair;
	d1_repair_id second_error_repair;
	d1_txn_id repair_txn, repair_txn2, second_error_txn, staged_txn;
	d1_version_id broken, broken2, predecessor, successor, visible;
	struct d2_wal_header wal_header;
	struct d2_control control;
	struct d1_selection_spec selection = { .selection =
						       D1_SELECT_ORDINARY };
	struct d1_guard guard = { .never_written = true };
	struct d1_view *view = NULL;
	uint8_t certificate[D1_CERTIFICATE_BYTES];
	uint8_t second_certificate[D1_CERTIFICATE_BYTES];
	uint8_t payload[4096], replacement[4096], readback[4096], token[32];
	uint8_t verifier[D1_VERIFIER_BYTES];
	uint8_t registration[396];
	uint32_t read_len;
	uint64_t wal_bytes, promise_base;
	int dirfd;

	if (!root)
		return 77;
	dirfd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dirfd < 0)
		return 77;
	unlinkat(dirfd, "super", 0);
	unlinkat(dirfd, "wal", 0);
	unlinkat(dirfd, "payload", 0);
	fill(config.files.export_uuid, 16, 0x20);
	fill(token, sizeof(token), 0x40);
	config.files.binding_token = token;
	config.files.binding_token_len = sizeof(token);
	config.files.capacity_wal_bytes = D2_MIN_WAL_BYTES;
	config.files.capacity_payload_bytes = 64u * 1024u * 1024u;
	config.chunk_bytes = sizeof(payload);
	config.max_file_bytes = 64u * sizeof(payload);
	check(d2_store_provision(dirfd, &config, &binding, &store) == D1_OK,
	      "provision fixed files");
	if (!store)
		goto done;
	fill(object.export_uuid.bytes, 16, 0x20);
	fill(object.object_uuid.bytes, 16, 0x60);
	admission = d2_store_admit(store, &object, 17,
				   D1_RIGHT_READ | D1_RIGHT_WRITE |
					   D1_RIGHT_SINGLE_WRITER);
	check(d1_admission_live(admission), "durable admission");
	{
		int wal_fd = openat(dirfd, "wal", O_RDONLY | O_CLOEXEC);
		ssize_t got = wal_fd < 0 ? -1 :
					   pread(wal_fd, registration,
						 sizeof(registration),
						 D2_START_RECORD_BYTES);

		if (wal_fd >= 0)
			close(wal_fd);
		check(got == (ssize_t)sizeof(registration) &&
			      d2_wal_header_decode(
				      registration, sizeof(registration),
				      binding.store_uuid, binding.wal_uuid,
				      &wal_header) &&
			      d2_control_decode(registration,
						sizeof(registration),
						&wal_header, &control) &&
			      control.subtype == D2_CTL_FILE_REGISTER,
		      "file registration precedes object records");
	}
	fill(payload, sizeof(payload), 0x80);
	env.object = object;
	env.admission = admission;
	env.incarnation = d2_store_incarnation(store);
	fill(env.key.origin.bytes, 16, 0xa0);
	env.op = D1_OP_WRITE_BATCH;
	write_request(&env, 1, 0, 1, &guard, payload, sizeof(payload));
	check(d2_store_apply(store, &env, &result) == D1_OK,
	      "payload then WAL apply");
	{
		uint8_t persisted[sizeof(payload)];
		int payload_fd = openat(dirfd, "payload", O_RDONLY | O_CLOEXEC);

		check(payload_fd >= 0 &&
			      pread(payload_fd, persisted, sizeof(persisted),
				    D2_PAYLOAD_ALIGN +
					    D2_PAYLOAD_HEADER_BYTES) ==
				      (ssize_t)sizeof(persisted) &&
			      !memcmp(persisted, payload, sizeof(persisted)),
		      "payload object contains application bytes");
		if (payload_fd >= 0)
			close(payload_fd);
	}
	check(entry_has_admission(dirfd, d2_store_wal_bytes(store), &binding,
				  admission.raw),
	      "ENTRY carries injective admission surrogate");
	check(authority_precedes_entry(dirfd, d2_store_wal_bytes(store),
				       &binding, admission.raw),
	      "trust and covering authority precede admitted work");
	check(result.entries[0].status == D1_OK &&
		      result.entries[0].disposition == D1_COMPLETED,
	      "exact result recorded");
	check(d2_store_visible(store, &object, 0, &visible),
	      "write is visible");
	check(d2_store_eof(store, &object) == sizeof(payload),
	      "visible EOF is derived");
	check(d2_store_view_open(store, &object, admission, &selection, 0,
				 sizeof(payload), &view) == D1_OK,
	      "read view pins visible payload");
	check(d2_store_guard(store, &object, 0, &guard), "visible guard reads");
	fill(replacement, sizeof(replacement), 0x90);
	write_request(&env, 2, 0, 2, &guard, replacement, sizeof(replacement));
	check(d2_store_apply(store, &env, &result) == D1_OK,
	      "replacement commits");
	check(d1_view_read(view, 0, readback, sizeof(readback), &read_len) ==
			      D1_OK &&
		      read_len == sizeof(readback) &&
		      !memcmp(readback, payload, sizeof(payload)),
	      "pin retains predecessor across replacement");
	d1_view_close(view);
	view = NULL;
	wal_bytes = d2_store_wal_bytes(store);
	check(d2_store_apply(store, &env, &result) == D1_OK &&
		      d2_store_wal_bytes(store) == wal_bytes,
	      "exact replay appends nothing");
	conflict = env;
	conflict.body.write.activate = !conflict.body.write.activate;
	check(d2_store_apply(store, &conflict, &result) == D1_OK &&
		      result.entries[0].status == D1_REPLAY_CONFLICT &&
		      d2_store_wal_bytes(store) == wal_bytes,
	      "changed request under an operation key is refused");
	stale_incarnation = env;
	stale_incarnation.key.sequence = 98;
	stale_incarnation.incarnation++;
	check(d2_store_apply(store, &stale_incarnation, &result) == D1_OK &&
		      result.entries[0].status == D1_STALE_AUTH,
	      "wrong incarnation is durably refused");
	wal_bytes = d2_store_wal_bytes(store);
	memset(env.key.origin.bytes, 0, sizeof(env.key.origin.bytes));
	env.key.sequence = 97;
	check(d2_store_apply(store, &env, &result) == D1_INVALID &&
		      d2_store_wal_bytes(store) == wal_bytes,
	      "zero operation-key origin is rejected before recording");
	env = conflict;
	env.body.write.activate = !env.body.write.activate;
	unsupported = env;
	unsupported.key.sequence = 99;
	unsupported.op = D1_OP_LEASE_REAP;
	memset(&unsupported.body, 0, sizeof(unsupported.body));
	unsupported.body.control.count = 1;
	unsupported.body.control.txns[0] = d2_store_txn_handle(store, 1);
	unsupported.body.control.old_admission = admission;
	check(d2_store_apply(store, &unsupported, &result) == D1_UNSUPPORTED &&
		      result.disposition == D1_UNRECORDED &&
		      d2_store_wal_bytes(store) == wal_bytes,
	      "unimplemented control is explicitly unsupported");
	refused = env;
	write_request(&refused, 40, 0, 40,
		      &(struct d1_guard){ .never_written = true }, payload,
		      sizeof(payload));
	check(d2_store_apply(store, &refused, &refused_result) == D1_OK &&
		      refused_result.entries[0].status == D1_GUARDED,
	      "guard refusal is recorded without changing state");
	guard = (struct d1_guard){ .never_written = true };
	write_request(&env, 3, 5, 3, &guard, payload, sizeof(payload));
	check(d2_store_apply(store, &env, &result) == D1_OK &&
		      d2_store_eof(store, &object) == 6u * sizeof(payload),
	      "sparse high chunk extends EOF");
	write_request(&env, 4, 3, 4, &guard, payload, sizeof(payload));
	check(d2_store_apply(store, &env, &result) == D1_OK,
	      "interior chunk commits before publication fault control");
	write_request(&env, 5, 6, 5, &guard, payload, sizeof(payload));
	d2_store_fail_next_index(store);
	wal_bytes = d2_store_wal_bytes(store);
	check(d2_store_apply(store, &env, &result) == D1_OK &&
		      d2_store_wal_bytes(store) > wal_bytes &&
		      d2_store_overlay_active(store) &&
		      d2_store_visible(store, &object, 6, &visible) &&
		      visible.raw == result.entries[0].version.raw &&
		      !d2_store_materialized(store, &object, 6, &predecessor) &&
		      d2_store_eof(store, &object) == 7u * sizeof(payload),
	      "post-WAL index failure serves the overlay and advances EOF");
	durable_result = result;
	wal_bytes = d2_store_wal_bytes(store);
	check(d2_store_apply(store, &env, &result) == D1_OK &&
		      same_result(&result, &durable_result) &&
		      d2_store_wal_bytes(store) == wal_bytes,
	      "post-WAL index failure retry returns the exact receipt once");
	d2_store_crash(store);
	store = NULL;
	memcpy(reopen.files.expected_store_uuid, binding.store_uuid, 16);
	memcpy(reopen.files.expected_export_uuid, binding.export_uuid, 16);
	reopen.files.expected_root_ino = binding.root_ino;
	reopen.files.binding_token = token;
	reopen.files.binding_token_len = sizeof(token);
	reopen.chunk_bytes = config.chunk_bytes;
	reopen.max_file_bytes = config.max_file_bytes;
	check(d2_store_rebind(dirfd, &reopen, &binding, &store) == D1_OK,
	      "fresh process rebinds fixed files");
	check(store && d2_store_visible(store, &object, 0, &visible),
	      "replay reconstructs visible version");
	check(store && d2_store_eof(store, &object) == 7u * sizeof(payload),
	      "replay reconstructs EOF");
	check(store && d2_store_visible(store, &object, 3, &visible) &&
		      d2_store_visible(store, &object, 5, &visible) &&
		      d2_store_visible(store, &object, 6, &visible),
	      "replay reconstructs sparse visible map");
	if (store) {
		wal_bytes = d2_store_wal_bytes(store);
		refused.admission =
			d2_store_admission_handle(store, refused.admission.raw);
		check(d2_store_apply(store, &refused, &result) == D1_OK &&
			      same_result(&result, &refused_result) &&
			      d2_store_wal_bytes(store) == wal_bytes,
		      "restart returns exact refused receipt");
		conflict.admission = d2_store_admission_handle(
			store, conflict.admission.raw);
		check(d2_store_apply(store, &conflict, &result) == D1_OK &&
			      result.entries[0].status == D1_REPLAY_CONFLICT &&
			      d2_store_wal_bytes(store) == wal_bytes,
		      "restart preserves request-digest conflict");
		stale_incarnation.admission = d2_store_admission_handle(
			store, stale_incarnation.admission.raw);
		check(d2_store_apply(store, &stale_incarnation, &result) ==
				      D1_OK &&
			      result.entries[0].status == D1_STALE_AUTH &&
			      d2_store_wal_bytes(store) == wal_bytes,
		      "restart returns wrong-incarnation refusal");
	}
	if (store) {
		env.admission = d2_store_admission_handle(store, admission.raw);
		wal_bytes = d2_store_wal_bytes(store);
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      same_result(&result, &durable_result) &&
			      d2_store_wal_bytes(store) == wal_bytes,
		      "restart returns exact durable receipt once");
	}
	if (store) {
		check(d2_store_visible(store, &object, 3, &predecessor) &&
			      d2_store_guard(store, &object, 3, &guard),
		      "committed rollback captures predecessor");
		admission = d2_store_admit(store, &object, 17,
					   D1_RIGHT_READ | D1_RIGHT_WRITE |
						   D1_RIGHT_REPAIR |
						   D1_RIGHT_SINGLE_WRITER);
		env.admission = admission;
		env.incarnation = d2_store_incarnation(store);
		env.op = D1_OP_WRITE_BATCH;
		write_request(&env, 20, 3, 20, &guard, replacement, 137);
		env.body.write.activate = false;
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_OK,
		      "committed rollback stages replacement");
		staged_txn = result.entries[0].txn;
		successor = result.entries[0].version;
		memset(&env.body, 0, sizeof(env.body));
		env.key.sequence = 21;
		env.op = D1_OP_FINALIZE_BATCH;
		env.body.lifecycle.range_begin = 3;
		env.body.lifecycle.range_end = 4;
		env.body.lifecycle.count = 1;
		env.body.lifecycle.entries[0].index = 3;
		env.body.lifecycle.entries[0].owner.cohort.raw = 1;
		env.body.lifecycle.entries[0].owner.writer = 17;
		env.body.lifecycle.entries[0].owner.co_id = 20;
		env.body.lifecycle.entries[0].txn = staged_txn;
		env.body.lifecycle.entries[0].predecessor_present = true;
		env.body.lifecycle.entries[0].predecessor = predecessor;
		d2_store_verifier(store, verifier);
		memcpy(env.body.lifecycle.prior_verifier, verifier,
		       sizeof(verifier));
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].phase == D2_FINALIZED,
		      "replacement finalizes before custody rollback");
		env.key.sequence = 22;
		env.op = D1_OP_COMMIT_BATCH;
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].phase == D2_COMMITTED &&
			      d2_store_eof(store, &object) ==
				      7u * sizeof(payload) &&
			      d2_store_visible(store, &object, 6, &visible),
		      "short interior replacement preserves higher extent");
		custody = d2_store_custody(store, successor);
		check(d1_custody_live(custody), "custody issue is durable");
		memset(&env.body, 0, sizeof(env.body));
		env.key.sequence = 23;
		env.op = D1_OP_ROLLBACK_BATCH;
		env.body.rollback.range_begin = 3;
		env.body.rollback.range_end = 4;
		env.body.rollback.count = 1;
		env.body.rollback.entries[0].index = 3;
		env.body.rollback.entries[0].owner.cohort.raw = 1;
		env.body.rollback.entries[0].owner.writer = 17;
		env.body.rollback.entries[0].owner.co_id = 20;
		env.body.rollback.entries[0].txn = staged_txn;
		env.body.rollback.entries[0].visible_present = true;
		env.body.rollback.entries[0].visible = successor;
		env.body.rollback.entries[0].predecessor_present = true;
		env.body.rollback.entries[0].predecessor = predecessor;
		env.body.rollback.entries[0].custody_present = true;
		env.body.rollback.entries[0].custody = custody;
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].phase == D2_ROLLED_BACK &&
			      d2_store_visible(store, &object, 3, &visible) &&
			      visible.raw == predecessor.raw &&
			      d2_store_visible(store, &object, 6, &visible) &&
			      d2_store_eof(store, &object) ==
				      7u * sizeof(payload),
		      "interior rollback preserves unrelated higher extent");
		committed_rollback = env;
		d2_store_crash(store);
		store = NULL;
		check(d2_store_rebind(dirfd, &reopen, &binding, &store) ==
				      D1_OK &&
			      d2_store_visible(store, &object, 3, &visible) &&
			      visible.raw == predecessor.raw,
		      "restart replays custody rollback");
		if (store) {
			committed_rollback.admission = d2_store_admission_handle(
				store, committed_rollback.admission.raw);
			wal_bytes = d2_store_wal_bytes(store);
			check(d2_store_apply(store, &committed_rollback,
					     &result) == D1_OK &&
				      result.entries[0].phase ==
					      D2_ROLLED_BACK &&
				      d2_store_wal_bytes(store) == wal_bytes,
			      "restart returns committed rollback receipt");
		}
	}
	if (store) {
		admission = d2_store_admit(store, &object, 17,
					   D1_RIGHT_READ | D1_RIGHT_WRITE |
						   D1_RIGHT_SINGLE_WRITER);
		memset(&env, 0, sizeof(env));
		env.object = object;
		env.admission = admission;
		env.incarnation = d2_store_incarnation(store);
		fill(env.key.origin.bytes, 16, 0xb4);
		env.op = D1_OP_WRITE_BATCH;
		guard = (struct d1_guard){ .never_written = true };
		write_request(&env, 1, 13, 41, &guard, payload,
			      sizeof(payload));
		env.body.write.activate = false;
		promise_base = d2_store_wal_promised(store);
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].phase == D2_PREPARED &&
			      d2_store_wal_promised(store) ==
				      promise_base + 2u * D2_ENTRY_RECORD_BYTES,
		      "lease reap fixture leaves prepared work");
		d2_store_expire(store, admission);
		d2_store_expire(store, admission);
		d2_store_revoke(store, admission);
		wal_bytes = d2_store_wal_bytes(store);
		d2_store_crash(store);
		store = NULL;
		check(d2_store_rebind(dirfd, &reopen, &binding, &store) ==
				      D1_OK &&
			      d2_store_wal_bytes(store) ==
				      wal_bytes + D2_START_RECORD_BYTES +
					      264u &&
			      d2_store_wal_promised(store) == promise_base &&
			      !d2_store_visible(store, &object, 13, &visible),
		      "restart reaps only explicitly expired prepared work");
		if (store) {
			wal_bytes = d2_store_wal_bytes(store);
			d2_store_crash(store);
			store = NULL;
			check(d2_store_rebind(dirfd, &reopen, &binding,
					      &store) == D1_OK &&
				      d2_store_wal_bytes(store) ==
					      wal_bytes + D2_START_RECORD_BYTES,
			      "reap record rescans without repeating sweep");
		}
	}
	if (store) {
		admission = d2_store_admit(store, &object, 17,
					   D1_RIGHT_READ | D1_RIGHT_WRITE |
						   D1_RIGHT_SINGLE_WRITER);
		env.admission = admission;
		env.incarnation = d2_store_incarnation(store);
		env.op = D1_OP_WRITE_BATCH;
		guard = (struct d1_guard){ .never_written = true };
		write_request(&env, 30, 20, 30, &guard, payload,
			      sizeof(payload));
		env.body.write.activate = false;
		promise_base = d2_store_wal_promised(store);
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].phase == D2_PREPARED &&
			      d2_store_wal_promised(store) ==
				      promise_base + 2u * D2_ENTRY_RECORD_BYTES,
		      "prepared work reserves finalize and commit");
		staged_txn = result.entries[0].txn;
		d2_store_crash(store);
		store = NULL;
		check(d2_store_rebind(dirfd, &reopen, &binding, &store) ==
				      D1_OK &&
			      d2_store_wal_promised(store) ==
				      promise_base +
					      2u * D2_ENTRY_RECORD_BYTES &&
			      d2_store_recovery_allowance(store) ==
				      D2_RESTART_SWEEP,
		      "restart reconstructs prepared completion reserve");
	}
	if (store) {
		control_admission =
			d2_store_admit(store, &object, 17, D1_RIGHT_CONTROL);
		check(d2_store_recovery_allowance(store) == D2_RESTART_SWEEP,
		      "acting control admission uses ordinary capacity");
		fresh_admission =
			d2_store_admit(store, &object, 17,
				       D1_RIGHT_READ | D1_RIGHT_WRITE |
					       D1_RIGHT_SINGLE_WRITER);
		check(d2_store_recovery_allowance(store) ==
			      D2_RESTART_SWEEP - 552u,
		      "returning admission spends trust and vouch allowance");
		memset(&recovery, 0, sizeof(recovery));
		recovery.object = object;
		recovery.admission = control_admission;
		recovery.incarnation = d2_store_incarnation(store);
		fill(recovery.key.origin.bytes, 16, 0xc0);
		recovery.key.sequence = 1;
		recovery.op = D1_OP_RECOVERY_ADMIT;
		recovery.body.control.count = 1;
		recovery.body.control.txns[0] =
			d2_store_txn_handle(store, staged_txn.raw);
		recovery.body.control.old_admission =
			d2_store_admission_handle(store, admission.raw);
		recovery.body.control.new_admission_present = true;
		recovery.body.control.new_admission = fresh_admission;
		recovery.body.control.read_epoch_present = true;
		recovery.body.control.read_epoch = 0;
		check(d2_store_apply(store, &recovery, &result) == D1_OK &&
			      result.entries[0].status == D1_OK &&
			      d2_store_recovery_allowance(store) ==
				      D2_RESTART_SWEEP - 808u,
		      "recovery admission rebinds prepared work");
		d2_store_crash(store);
		store = NULL;
		check(d2_store_rebind(dirfd, &reopen, &binding, &store) ==
			      D1_OK,
		      "recovery admission replays from portable controls");
	}
	if (store) {
		recovery.admission =
			d2_store_admission_handle(store, control_admission.raw);
		recovery.body.control.txns[0] =
			d2_store_txn_handle(store, staged_txn.raw);
		recovery.body.control.old_admission =
			d2_store_admission_handle(store, admission.raw);
		recovery.body.control.new_admission =
			d2_store_admission_handle(store, fresh_admission.raw);
		wal_bytes = d2_store_wal_bytes(store);
		check(d2_store_apply(store, &recovery, &result) == D1_OK &&
			      result.entries[0].status == D1_OK &&
			      d2_store_wal_bytes(store) == wal_bytes,
		      "restart returns exact recovery admission receipt");
		next_control =
			d2_store_admit(store, &object, 17, D1_RIGHT_CONTROL);
		next_fresh = d2_store_admit(store, &object, 17,
					    D1_RIGHT_READ | D1_RIGHT_WRITE |
						    D1_RIGHT_SINGLE_WRITER);
		recovery.admission = next_control;
		recovery.incarnation = d2_store_incarnation(store);
		recovery.key.sequence = 2;
		recovery.body.control.old_admission =
			d2_store_admission_handle(store, fresh_admission.raw);
		recovery.body.control.new_admission = next_fresh;
		check(d2_store_apply(store, &recovery, &result) == D1_OK &&
			      result.entries[0].status == D1_OK,
		      "later recovery uses the transaction's current admission");
		memset(&env, 0, sizeof(env));
		env.object = object;
		env.admission = next_fresh;
		env.incarnation = d2_store_incarnation(store);
		fill(env.key.origin.bytes, 16, 0xd0);
		env.key.sequence = 1;
		env.op = D1_OP_FINALIZE_BATCH;
		env.body.lifecycle.range_begin = 20;
		env.body.lifecycle.range_end = 21;
		env.body.lifecycle.count = 1;
		env.body.lifecycle.entries[0].index = 20;
		env.body.lifecycle.entries[0].owner.cohort.raw = 1;
		env.body.lifecycle.entries[0].owner.writer = 17;
		env.body.lifecycle.entries[0].owner.co_id = 30;
		env.body.lifecycle.entries[0].txn =
			d2_store_txn_handle(store, staged_txn.raw);
		d2_store_verifier(store, verifier);
		memcpy(env.body.lifecycle.prior_verifier, verifier,
		       sizeof(verifier));
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].phase == D2_FINALIZED &&
			      d2_store_wal_promised(store) ==
				      promise_base + D2_ENTRY_RECORD_BYTES,
		      "finalize consumes one ordinary reserve record");
		recovery.key.sequence = 3;
		recovery.body.control.old_admission =
			d2_store_admission_handle(store, admission.raw);
		check(d2_store_apply(store, &recovery, &result) == D1_OK &&
			      result.entries[0].status == D1_OWNER_CONFLICT,
		      "recovery refusal is recorded without rebinding work");
		refused_recovery_result = result;
		admission = d2_store_admit(store, &object, 17,
					   D1_RIGHT_READ | D1_RIGHT_WRITE |
						   D1_RIGHT_REPAIR |
						   D1_RIGHT_SINGLE_WRITER);
		memset(&env, 0, sizeof(env));
		env.object = object;
		env.admission = admission;
		env.incarnation = d2_store_incarnation(store);
		fill(env.key.origin.bytes, 16, 0xe0);
		env.op = D1_OP_WRITE_BATCH;
		guard = (struct d1_guard){ .never_written = true };
		write_request(&env, 1, 22, 31, &guard, payload,
			      sizeof(payload));
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_OK,
		      "postcondition fixture publishes a successor");
		staged_txn = result.entries[0].txn;
		successor = result.entries[0].version;
		custody = d2_store_custody(store, successor);
		memset(&env.body, 0, sizeof(env.body));
		env.key.sequence = 2;
		env.op = D1_OP_ROLLBACK_BATCH;
		env.body.rollback.range_begin = 22;
		env.body.rollback.range_end = 23;
		env.body.rollback.count = 1;
		env.body.rollback.entries[0].index = 22;
		env.body.rollback.entries[0].owner.cohort.raw = 1;
		env.body.rollback.entries[0].owner.writer = 17;
		env.body.rollback.entries[0].owner.co_id = 31;
		env.body.rollback.entries[0].txn = staged_txn;
		env.body.rollback.entries[0].visible_present = true;
		env.body.rollback.entries[0].visible = successor;
		env.body.rollback.entries[0].custody_present = true;
		env.body.rollback.entries[0].custody = custody;
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_NO_PREDECESSOR &&
			      result.entries[0].postcond_present,
		      "no-predecessor rollback persists its postcondition group");
		postcond = result.entries[0].postcond;
		nopre = env;
		nopre_result = result;

		memset(&env.body, 0, sizeof(env.body));
		env.key.sequence = 3;
		env.op = D1_OP_BEGIN_REPAIR;
		env.body.repair.range_begin = 22;
		env.body.repair.range_end = 23;
		env.body.repair.count = 1;
		env.body.repair.entries[0].index = 22;
		env.body.repair.entries[0].owner.cohort.raw = 2;
		env.body.repair.entries[0].owner.writer = 17;
		env.body.repair.entries[0].owner.co_id = 32;
		env.body.repair.entries[0].mode = D1_REPAIR_NOPRE;
		env.body.repair.entries[0].custody_present = true;
		env.body.repair.entries[0].custody = custody;
		env.body.repair.entries[0].postcond_present = true;
		env.body.repair.entries[0].postcond = postcond;
		env.body.repair.entries[0].successor_present = true;
		env.body.repair.entries[0].successor = successor;
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_OK &&
			      result.entries[0].phase == D2_ADMITTED &&
			      result.entries[0].cohort_present &&
			      result.entries[0].member_txn_count == 1,
		      "no-predecessor repair admission persists");
		repair = result.entries[0].cohort;
		repair_txn = result.entries[0].member_txn[0];

		memset(&env.body, 0, sizeof(env.body));
		env.key.sequence = 4;
		env.op = D1_OP_PREPARE_REPAIR;
		env.body.repair.range_begin = 22;
		env.body.repair.range_end = 23;
		env.body.repair.count = 1;
		env.body.repair.cohort_present = true;
		env.body.repair.cohort = repair;
		env.body.repair.entries[0].index = 22;
		env.body.repair.entries[0].owner.cohort.raw = 2;
		env.body.repair.entries[0].owner.writer = 17;
		env.body.repair.entries[0].owner.co_id = 32;
		env.body.repair.entries[0].txn_present = true;
		env.body.repair.entries[0].txn = repair_txn;
		env.body.repair.entries[0].payload_present = true;
		env.body.repair.entries[0].payload = replacement;
		env.body.repair.entries[0].payload_len = sizeof(replacement);
		d1_checksum_compute(D1_CKSUM_CRC32C, replacement,
				    sizeof(replacement),
				    &env.body.repair.entries[0].checksum);
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_OK &&
			      result.entries[0].phase == D2_PREPARED,
		      "no-predecessor repair payload persists");

		memset(&env.body, 0, sizeof(env.body));
		env.key.sequence = 5;
		env.op = D1_OP_FINALIZE_REPAIR;
		env.body.repair.range_begin = 22;
		env.body.repair.range_end = 23;
		env.body.repair.count = 1;
		env.body.repair.cohort_present = true;
		env.body.repair.cohort = repair;
		env.body.repair.verifier_present = true;
		d2_store_verifier(store, env.body.repair.prior_verifier);
		env.body.repair.entries[0].index = 22;
		env.body.repair.entries[0].owner.cohort.raw = 2;
		env.body.repair.entries[0].owner.writer = 17;
		env.body.repair.entries[0].owner.co_id = 32;
		env.body.repair.entries[0].custody_present = true;
		env.body.repair.entries[0].custody = custody;
		env.body.repair.entries[0].txn_present = true;
		env.body.repair.entries[0].txn = repair_txn;
		env.body.repair.entries[0].predecessor_present = true;
		env.body.repair.entries[0].predecessor = successor;
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_OK &&
			      result.entries[0].phase == D2_FINALIZED,
		      "no-predecessor repair finalize persists");

		env.key.sequence = 6;
		env.op = D1_OP_COMMIT_REPAIR;
		d2_store_verifier(store, env.body.repair.prior_verifier);
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_OK &&
			      result.entries[0].phase == D2_COMMITTED &&
			      d2_store_visible(store, &object, 22, &visible) &&
			      visible.raw != successor.raw,
		      "no-predecessor repair commit persists replacement");
		d2_store_expire(store, admission);
		d2_store_crash(store);
		store = NULL;
		check(d2_store_rebind(dirfd, &reopen, &binding, &store) ==
			      D1_OK,
		      "committed repair reopens with its lock held");
		if (!store)
			goto done;
		control_admission =
			d2_store_admit(store, &object, 17, D1_RIGHT_CONTROL);
		fresh_admission = d2_store_admit(
			store, &object, 17,
			D1_RIGHT_READ | D1_RIGHT_WRITE | D1_RIGHT_REPAIR |
				D1_RIGHT_SINGLE_WRITER);
		memset(&env, 0, sizeof(env));
		env.object = object;
		env.admission = control_admission;
		env.incarnation = d2_store_incarnation(store);
		fill(env.key.origin.bytes, 16, 0xe8);
		env.key.sequence = 1;
		env.op = D1_OP_RECOVERY_ADMIT;
		env.body.control.count = 1;
		env.body.control.txns[0] =
			d2_store_txn_handle(store, repair_txn.raw);
		env.body.control.old_admission =
			d2_store_admission_handle(store, admission.raw);
		env.body.control.new_admission_present = true;
		env.body.control.new_admission = fresh_admission;
		env.body.control.read_epoch_present = true;
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_OK,
		      "restart rebinds committed repair member");
		admission = fresh_admission;

		memset(&env, 0, sizeof(env));
		env.object = object;
		env.admission = admission;
		env.incarnation = d2_store_incarnation(store);
		fill(env.key.origin.bytes, 16, 0xe0);
		env.key.sequence = 7;
		env.op = D1_OP_UNLOCK;
		env.body.repair.range_begin = 22;
		env.body.repair.range_end = 23;
		env.body.repair.count = 1;
		env.body.repair.cohort_present = true;
		env.body.repair.cohort =
			d2_store_repair_handle(store, repair.raw);
		env.body.repair.entries[0].index = 22;
		env.body.repair.entries[0].owner.cohort.raw = 2;
		env.body.repair.entries[0].owner.writer = 17;
		env.body.repair.entries[0].owner.co_id = 32;
		env.body.repair.entries[0].custody_present = true;
		env.body.repair.entries[0].custody =
			d2_store_custody_handle(store, custody.raw);
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_OK,
		      "no-predecessor repair unlock persists");
		repair_unlock = env;
		unlock_result = result;
	}
	if (store) {
		admission = d2_store_admit(store, &object, 17,
					   D1_RIGHT_READ | D1_RIGHT_WRITE |
						   D1_RIGHT_SINGLE_WRITER);
		guard = (struct d1_guard){ .never_written = true };
		env.admission = admission;
		env.incarnation = d2_store_incarnation(store);
		env.op = D1_OP_WRITE_BATCH;
		write_request(&env, 8, 6, 5, &guard, replacement,
			      sizeof(replacement));
		check(d2_store_apply(store, &env, &result) == D1_OK,
		      "post-restart write uses the next incarnation");
		guard = (struct d1_guard){ .never_written = true };
		write_request(&env, 9, 7, 6, &guard, payload, sizeof(payload));
		env.body.write.activate = false;
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].phase == D2_PREPARED,
		      "staged write persists as PREPARED");
		staged_txn = result.entries[0].txn;
		memset(&env.body, 0, sizeof(env.body));
		env.key.sequence = 10;
		env.op = D1_OP_FINALIZE_BATCH;
		env.body.lifecycle.range_begin = 7;
		env.body.lifecycle.range_end = 8;
		env.body.lifecycle.count = 1;
		env.body.lifecycle.entries[0].index = 7;
		env.body.lifecycle.entries[0].owner.cohort.raw = 1;
		env.body.lifecycle.entries[0].owner.writer = 17;
		env.body.lifecycle.entries[0].owner.co_id = 6;
		env.body.lifecycle.entries[0].txn = staged_txn;
		d2_store_verifier(store, verifier);
		memcpy(env.body.lifecycle.prior_verifier, verifier,
		       sizeof(verifier));
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].phase == D2_FINALIZED,
		      "single member finalize persists without payload");
		env.key.sequence = 11;
		env.op = D1_OP_COMMIT_BATCH;
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].phase == D2_COMMITTED &&
			      d2_store_visible(store, &object, 7, &visible),
		      "single member commit publishes staged payload");
		guard = (struct d1_guard){ .never_written = true };
		env.op = D1_OP_WRITE_BATCH;
		write_request(&env, 12, 8, 7, &guard, replacement,
			      sizeof(replacement));
		env.body.write.activate = false;
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].phase == D2_PREPARED,
		      "rollback fixture stages private work");
		staged_txn = result.entries[0].txn;
		memset(&env.body, 0, sizeof(env.body));
		env.key.sequence = 13;
		env.op = D1_OP_ROLLBACK_BATCH;
		env.body.rollback.range_begin = 8;
		env.body.rollback.range_end = 9;
		env.body.rollback.count = 1;
		env.body.rollback.entries[0].index = 8;
		env.body.rollback.entries[0].owner.cohort.raw = 1;
		env.body.rollback.entries[0].owner.writer = 17;
		env.body.rollback.entries[0].owner.co_id = 7;
		env.body.rollback.entries[0].txn = staged_txn;
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].phase == D2_ROLLED_BACK &&
			      !d2_store_visible(store, &object, 8, &visible),
		      "private rollback persists without payload");
		d2_store_crash(store);
		store = NULL;
		check(d2_store_rebind(dirfd, &reopen, &binding, &store) ==
			      D1_OK,
		      "second restart rebinds both incarnations");
		check(store && d2_store_visible(store, &object, 6, &visible),
		      "second restart publishes post-restart write");
		check(store && d2_store_visible(store, &object, 7, &visible),
		      "second restart replays finalize and commit");
		if (store) {
			check(d2_store_visible(store, &object, 22, &visible) &&
				      visible.raw != successor.raw,
			      "second restart replays no-predecessor repair");
			check(!d2_store_visible(store, &object, 8, &visible),
			      "second restart replays private rollback");
			nopre.admission = d2_store_admission_handle(
				store, nopre.admission.raw);
			nopre.body.rollback.entries[0].txn = d2_store_txn_handle(
				store, nopre.body.rollback.entries[0].txn.raw);
			nopre.body.rollback.entries[0]
				.visible = d2_store_version_handle(
				store,
				nopre.body.rollback.entries[0].visible.raw);
			wal_bytes = d2_store_wal_bytes(store);
			check(d2_store_apply(store, &nopre, &result) == D1_OK &&
				      same_result(&result, &nopre_result) &&
				      d2_store_wal_bytes(store) == wal_bytes,
			      "restart returns exact no-predecessor receipt");
			repair_unlock.admission = d2_store_admission_handle(
				store, repair_unlock.admission.raw);
			repair_unlock.body.repair.cohort =
				d2_store_repair_handle(
					store,
					repair_unlock.body.repair.cohort.raw);
			repair_unlock.body.repair.entries[0].custody =
				d2_store_custody_handle(
					store,
					repair_unlock.body.repair.entries[0]
						.custody.raw);
			wal_bytes = d2_store_wal_bytes(store);
			check(d2_store_apply(store, &repair_unlock, &result) ==
					      D1_OK &&
				      same_result(&result, &unlock_result) &&
				      d2_store_wal_bytes(store) == wal_bytes,
			      "restart returns exact repair unlock receipt");
			recovery.admission = d2_store_admission_handle(
				store, recovery.admission.raw);
			recovery.body.control.txns[0] = d2_store_txn_handle(
				store, recovery.body.control.txns[0].raw);
			recovery.body.control
				.old_admission = d2_store_admission_handle(
				store, recovery.body.control.old_admission.raw);
			recovery.body.control
				.new_admission = d2_store_admission_handle(
				store, recovery.body.control.new_admission.raw);
			wal_bytes = d2_store_wal_bytes(store);
			check(d2_store_apply(store, &recovery, &result) ==
					      D1_OK &&
				      same_result(&result,
						  &refused_recovery_result) &&
				      d2_store_wal_bytes(store) == wal_bytes,
			      "restart returns exact refused recovery receipt");
			env.admission =
				d2_store_admission_handle(store, admission.raw);
			wal_bytes = d2_store_wal_bytes(store);
			check(d2_store_apply(store, &env, &result) == D1_OK &&
				      result.entries[0].phase ==
					      D2_ROLLED_BACK &&
				      d2_store_wal_bytes(store) == wal_bytes,
			      "restart returns exact rollback receipt");
		}
	}
	if (store) {
		admission = d2_store_admit(store, &object, 17,
					   D1_RIGHT_READ | D1_RIGHT_WRITE |
						   D1_RIGHT_SINGLE_WRITER);
		env.admission = admission;
		env.incarnation = d2_store_incarnation(store);
		env.op = D1_OP_WRITE_BATCH;
		guard = (struct d1_guard){ .never_written = true };
		write_request(&env, 14, 9, 8, &guard, payload, sizeof(payload));
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_OK,
		      "expiry fixture records its admission");
		d2_store_expire(store, admission);
		write_request(&env, 15, 10, 9, &guard, payload,
			      sizeof(payload));
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_STALE_AUTH,
		      "durable lease expiry refuses later work");
		expired = env;
		admission = d2_store_admit(store, &object, 17,
					   D1_RIGHT_READ | D1_RIGHT_WRITE |
						   D1_RIGHT_SINGLE_WRITER);
		env.admission = admission;
		write_request(&env, 16, 11, 10, &guard, replacement,
			      sizeof(replacement));
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_OK,
		      "revocation fixture records its admission");
		d2_store_revoke(store, admission);
		d2_store_revoke(store, admission);
		d2_store_expire(store, admission);
		write_request(&env, 17, 12, 11, &guard, replacement,
			      sizeof(replacement));
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_STALE_AUTH,
		      "durable stateid revocation refuses later work");
		wal_bytes = d2_store_wal_bytes(store);
		d2_store_revoke(store, (d1_admission_id){ .raw = UINT64_MAX });
		d2_store_expire(store, (d1_admission_id){ .raw = UINT64_MAX });
		check(d2_store_wal_bytes(store) == wal_bytes,
		      "unknown liveness subjects are no-ops");
		d2_store_crash(store);
		store = NULL;
		check(d2_store_rebind(dirfd, &reopen, &binding, &store) ==
			      D1_OK,
		      "repeated and combined liveness controls replay");
		if (store) {
			env.admission =
				d2_store_admission_handle(store, admission.raw);
			wal_bytes = d2_store_wal_bytes(store);
			check(d2_store_apply(store, &env, &result) == D1_OK &&
				      result.entries[0].status ==
					      D1_STALE_AUTH &&
				      d2_store_wal_bytes(store) == wal_bytes,
			      "restart returns receipt after stateid revocation");
			expired.admission = d2_store_admission_handle(
				store, expired.admission.raw);
			check(d2_store_apply(store, &expired, &result) ==
					      D1_OK &&
				      result.entries[0].status ==
					      D1_STALE_AUTH &&
				      d2_store_wal_bytes(store) == wal_bytes,
			      "restart returns receipt after lease expiry");
		}
	}
	if (store) {
		admission = d2_store_admit(store, &object, 17,
					   D1_RIGHT_READ | D1_RIGHT_WRITE |
						   D1_RIGHT_REPAIR |
						   D1_RIGHT_SINGLE_WRITER);
		env.admission = admission;
		env.incarnation = d2_store_incarnation(store);
		env.op = D1_OP_WRITE_BATCH;
		guard = (struct d1_guard){ .never_written = true };
		write_request(&env, 18, 23, 33, &guard, payload,
			      sizeof(payload));
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_OK,
		      "error episode fixture publishes a version");
		broken = result.entries[0].version;
		custody = d2_store_custody(store, broken);
		check(d1_custody_live(custody),
		      "error episode fixture persists custody");
		guard = (struct d1_guard){ .never_written = true };
		write_request(&env, 19, 24, 34, &guard, replacement,
			      sizeof(replacement));
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_OK,
		      "second error fixture publishes a version");
		broken2 = result.entries[0].version;
		custody2 = d2_store_custody(store, broken2);
		check(d1_custody_live(custody2),
		      "second error fixture persists custody");
		memset(&env.body, 0, sizeof(env.body));
		env.key.sequence = 20;
		env.op = D1_OP_MARK_ERROR;
		env.body.repair.range_begin = 23;
		env.body.repair.range_end = 25;
		env.body.repair.count = 2;
		env.body.repair.entries[0].index = 23;
		env.body.repair.entries[0].owner.cohort.raw = 3;
		env.body.repair.entries[0].owner.writer = 17;
		env.body.repair.entries[0].owner.co_id = 34;
		env.body.repair.entries[0].custody_present = true;
		env.body.repair.entries[0].custody = custody;
		env.body.repair.entries[0].successor_present = true;
		env.body.repair.entries[0].successor = broken;
		env.body.repair.entries[1].index = 24;
		env.body.repair.entries[1].owner.cohort.raw = 3;
		env.body.repair.entries[1].owner.writer = 17;
		env.body.repair.entries[1].owner.co_id = 35;
		env.body.repair.entries[1].custody_present = true;
		env.body.repair.entries[1].custody = custody2;
		env.body.repair.entries[1].successor_present = true;
		env.body.repair.entries[1].successor = broken2;
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_OK &&
			      result.entries[0].episode_present,
		      "error episode mark persists");
		episode = result.entries[0].episode;
		marked = env;
		marked_result = result;
		d2_store_crash(store);
		store = NULL;
		check(d2_store_rebind(dirfd, &reopen, &binding, &store) ==
			      D1_OK,
		      "error episode mark replays after restart");
		if (store) {
			marked.admission = d2_store_admission_handle(
				store, marked.admission.raw);
			marked.body.repair.entries[0]
				.custody = d2_store_custody_handle(
				store,
				marked.body.repair.entries[0].custody.raw);
			marked.body.repair.entries[0]
				.successor = d2_store_version_handle(
				store,
				marked.body.repair.entries[0].successor.raw);
			marked.body.repair.entries[1]
				.custody = d2_store_custody_handle(
				store,
				marked.body.repair.entries[1].custody.raw);
			marked.body.repair.entries[1]
				.successor = d2_store_version_handle(
				store,
				marked.body.repair.entries[1].successor.raw);
			wal_bytes = d2_store_wal_bytes(store);
			check(d2_store_apply(store, &marked, &result) ==
					      D1_OK &&
				      same_result(&result, &marked_result) &&
				      d2_store_wal_bytes(store) == wal_bytes,
			      "restart returns exact episode-mark receipt");

			admission =
				d2_store_admit(store, &object, 17,
					       D1_RIGHT_READ | D1_RIGHT_WRITE |
						       D1_RIGHT_REPAIR |
						       D1_RIGHT_SINGLE_WRITER);
			episode = d2_store_episode_handle(store, episode.raw);
			custody = marked.body.repair.entries[0].custody;
			broken = marked.body.repair.entries[0].successor;
			custody2 = marked.body.repair.entries[1].custody;
			broken2 = marked.body.repair.entries[1].successor;
			refused_mark = marked;
			refused_mark.admission = admission;
			refused_mark.incarnation = d2_store_incarnation(store);
			fill(refused_mark.key.origin.bytes, 16, 0xf1);
			refused_mark.key.sequence = 1;
			check(d2_store_apply(store, &refused_mark, &result) ==
					      D1_OK &&
				      result.entries[0].status == D1_BAD_PHASE,
			      "repeated episode mark refusal persists");
			refused_mark_result = result;
			memset(&env, 0, sizeof(env));
			env.object = object;
			env.admission = admission;
			env.incarnation = d2_store_incarnation(store);
			fill(env.key.origin.bytes, 16, 0xe0);
			env.key.sequence = 21;
			env.op = D1_OP_BEGIN_REPAIR;
			env.body.repair.range_begin = 23;
			env.body.repair.range_end = 25;
			env.body.repair.count = 2;
			env.body.repair.episode_present = true;
			env.body.repair.episode = episode;
			env.body.repair.entries[0].index = 23;
			env.body.repair.entries[0].owner.cohort.raw = 4;
			env.body.repair.entries[0].owner.writer = 17;
			env.body.repair.entries[0].owner.co_id = 35;
			env.body.repair.entries[0].mode = D1_REPAIR_ERROR;
			env.body.repair.entries[0].custody_present = true;
			env.body.repair.entries[0].custody = custody;
			env.body.repair.entries[0].successor_present = true;
			env.body.repair.entries[0].successor = broken;
			env.body.repair.entries[1].index = 24;
			env.body.repair.entries[1].owner.cohort.raw = 4;
			env.body.repair.entries[1].owner.writer = 17;
			env.body.repair.entries[1].owner.co_id = 36;
			env.body.repair.entries[1].mode = D1_REPAIR_ERROR;
			env.body.repair.entries[1].custody_present = true;
			env.body.repair.entries[1].custody = custody2;
			env.body.repair.entries[1].successor_present = true;
			env.body.repair.entries[1].successor = broken2;
			env.body.repair.entries[0].owner.writer = 18;
			env.body.repair.entries[1].owner.writer = 18;
			check(d2_store_apply(store, &env, &result) == D1_OK &&
				      result.entries[0].status == D1_STALE_AUTH,
			      "repair writer mismatch refusal persists");
			refused_repair = env;
			refused_repair_result = result;
			env.key.sequence = 22;
			env.body.repair.entries[0].owner.writer = 17;
			env.body.repair.entries[1].owner.writer = 17;
			check(d2_store_apply(store, &env, &result) == D1_OK &&
				      result.entries[0].status == D1_OK &&
				      result.entries[0].phase == D2_ADMITTED,
			      "error repair admission persists");
			repair = result.entries[0].cohort;
			repair_txn = result.entries[0].member_txn[0];
			repair_txn2 = result.entries[0].member_txn[1];
			memset(&env.body, 0, sizeof(env.body));
			env.key.sequence = 23;
			env.op = D1_OP_PREPARE_REPAIR;
			env.body.repair.range_begin = 23;
			env.body.repair.range_end = 25;
			env.body.repair.count = 2;
			env.body.repair.cohort_present = true;
			env.body.repair.cohort = repair;
			env.body.repair.entries[0].index = 23;
			env.body.repair.entries[0].owner.cohort.raw = 4;
			env.body.repair.entries[0].owner.writer = 17;
			env.body.repair.entries[0].owner.co_id = 35;
			env.body.repair.entries[0].txn_present = true;
			env.body.repair.entries[0].txn = repair_txn;
			env.body.repair.entries[0].payload_present = true;
			env.body.repair.entries[0].payload = replacement;
			env.body.repair.entries[0].payload_len =
				sizeof(replacement);
			d1_checksum_compute(
				D1_CKSUM_CRC32C, replacement,
				sizeof(replacement),
				&env.body.repair.entries[0].checksum);
			env.body.repair.entries[0].checksum.digest[0] ^= 0xff;
			env.body.repair.entries[1].index = 24;
			env.body.repair.entries[1].owner.cohort.raw = 4;
			env.body.repair.entries[1].owner.writer = 17;
			env.body.repair.entries[1].owner.co_id = 36;
			env.body.repair.entries[1].txn_present = true;
			env.body.repair.entries[1].txn = repair_txn2;
			env.body.repair.entries[1].payload_present = true;
			env.body.repair.entries[1].payload = payload;
			env.body.repair.entries[1].payload_len =
				sizeof(payload);
			d1_checksum_compute(
				D1_CKSUM_CRC32C, payload, sizeof(payload),
				&env.body.repair.entries[1].checksum);
			check(d2_store_apply(store, &env, &result) == D1_OK &&
				      result.entries[0].status == D1_CHECKSUM &&
				      result.entries[0].phase == D2_ABORTED,
			      "failed two-member prepare aborts atomically");
			aborted_prepare = env;
			aborted_result = result;
			d2_store_crash(store);
			store = NULL;
			check(d2_store_rebind(dirfd, &reopen, &binding,
					      &store) == D1_OK,
			      "kept repair abort replays before reuse");
			if (!store)
				goto done;
			admission =
				d2_store_admission_handle(store, admission.raw);
			episode = d2_store_episode_handle(store, episode.raw);
			custody = d2_store_custody_handle(store, custody.raw);
			custody2 = d2_store_custody_handle(store, custody2.raw);
			broken = d2_store_version_handle(store, broken.raw);
			broken2 = d2_store_version_handle(store, broken2.raw);
			aborted_prepare.admission = admission;
			aborted_prepare.body.repair.cohort =
				d2_store_repair_handle(
					store,
					aborted_prepare.body.repair.cohort.raw);
			aborted_prepare.body.repair.entries[0]
				.txn = d2_store_txn_handle(
				store,
				aborted_prepare.body.repair.entries[0].txn.raw);
			aborted_prepare.body.repair.entries[1]
				.txn = d2_store_txn_handle(
				store,
				aborted_prepare.body.repair.entries[1].txn.raw);
			wal_bytes = d2_store_wal_bytes(store);
			check(d2_store_apply(store, &aborted_prepare,
					     &result) == D1_OK &&
				      same_result(&result, &aborted_result) &&
				      d2_store_wal_bytes(store) == wal_bytes,
			      "restart returns kept abort before new cohort");
			admission =
				d2_store_admit(store, &object, 17,
					       D1_RIGHT_READ | D1_RIGHT_WRITE |
						       D1_RIGHT_REPAIR |
						       D1_RIGHT_SINGLE_WRITER);

			memset(&env.body, 0, sizeof(env.body));
			env.admission = admission;
			env.incarnation = d2_store_incarnation(store);
			promise_base = d2_store_wal_promised(store);
			env.key.sequence = 24;
			env.op = D1_OP_BEGIN_REPAIR;
			env.body.repair.range_begin = 23;
			env.body.repair.range_end = 25;
			env.body.repair.count = 2;
			env.body.repair.episode_present = true;
			env.body.repair.episode = episode;
			env.body.repair.entries[0].index = 23;
			env.body.repair.entries[0].owner.cohort.raw = 5;
			env.body.repair.entries[0].owner.writer = 17;
			env.body.repair.entries[0].owner.co_id = 37;
			env.body.repair.entries[0].mode = D1_REPAIR_ERROR;
			env.body.repair.entries[0].custody_present = true;
			env.body.repair.entries[0].custody = custody;
			env.body.repair.entries[0].successor_present = true;
			env.body.repair.entries[0].successor = broken;
			env.body.repair.entries[1].index = 24;
			env.body.repair.entries[1].owner.cohort.raw = 5;
			env.body.repair.entries[1].owner.writer = 17;
			env.body.repair.entries[1].owner.co_id = 38;
			env.body.repair.entries[1].mode = D1_REPAIR_ERROR;
			env.body.repair.entries[1].custody_present = true;
			env.body.repair.entries[1].custody = custody2;
			env.body.repair.entries[1].successor_present = true;
			env.body.repair.entries[1].successor = broken2;
			check(d2_store_apply(store, &env, &result) == D1_OK &&
				      result.entries[0].status == D1_OK &&
				      result.entries[0].phase == D2_ADMITTED &&
				      d2_store_wal_promised(store) ==
					      promise_base + 4464,
			      "two-member repair reopens after abort");
			repair = result.entries[0].cohort;
			repair_txn = result.entries[0].member_txn[0];
			repair_txn2 = result.entries[0].member_txn[1];

			memset(&env.body, 0, sizeof(env.body));
			env.key.sequence = 25;
			env.op = D1_OP_PREPARE_REPAIR;
			env.body.repair.range_begin = 23;
			env.body.repair.range_end = 25;
			env.body.repair.count = 2;
			env.body.repair.cohort_present = true;
			env.body.repair.cohort = repair;
			env.body.repair.entries[0].index = 23;
			env.body.repair.entries[0].owner.cohort.raw = 5;
			env.body.repair.entries[0].owner.writer = 17;
			env.body.repair.entries[0].owner.co_id = 37;
			env.body.repair.entries[0].txn_present = true;
			env.body.repair.entries[0].txn = repair_txn;
			env.body.repair.entries[0].payload_present = true;
			env.body.repair.entries[0].payload = replacement;
			env.body.repair.entries[0].payload_len =
				sizeof(replacement);
			d1_checksum_compute(
				D1_CKSUM_CRC32C, replacement,
				sizeof(replacement),
				&env.body.repair.entries[0].checksum);
			env.body.repair.entries[1].index = 24;
			env.body.repair.entries[1].owner.cohort.raw = 5;
			env.body.repair.entries[1].owner.writer = 17;
			env.body.repair.entries[1].owner.co_id = 38;
			env.body.repair.entries[1].txn_present = true;
			env.body.repair.entries[1].txn = repair_txn2;
			env.body.repair.entries[1].payload_present = true;
			env.body.repair.entries[1].payload = payload;
			env.body.repair.entries[1].payload_len =
				sizeof(payload);
			d1_checksum_compute(
				D1_CKSUM_CRC32C, payload, sizeof(payload),
				&env.body.repair.entries[1].checksum);
			check(d2_store_apply(store, &env, &result) == D1_OK &&
				      result.entries[0].status == D1_OK &&
				      result.entries[0].phase == D2_PREPARED &&
				      d2_store_wal_promised(store) ==
					      promise_base + 3644,
			      "error repair payload persists");

			memset(&env.body, 0, sizeof(env.body));
			env.key.sequence = 26;
			env.op = D1_OP_FINALIZE_REPAIR;
			env.body.repair.range_begin = 23;
			env.body.repair.range_end = 25;
			env.body.repair.count = 2;
			env.body.repair.cohort_present = true;
			env.body.repair.cohort = repair;
			env.body.repair.verifier_present = true;
			d2_store_verifier(store,
					  env.body.repair.prior_verifier);
			env.body.repair.entries[0].index = 23;
			env.body.repair.entries[0].owner.cohort.raw = 5;
			env.body.repair.entries[0].owner.writer = 17;
			env.body.repair.entries[0].owner.co_id = 37;
			env.body.repair.entries[0].custody_present = true;
			env.body.repair.entries[0].custody = custody;
			env.body.repair.entries[0].txn_present = true;
			env.body.repair.entries[0].txn = repair_txn;
			env.body.repair.entries[0].predecessor_present = true;
			env.body.repair.entries[0].predecessor = broken;
			env.body.repair.entries[1].index = 24;
			env.body.repair.entries[1].owner.cohort.raw = 5;
			env.body.repair.entries[1].owner.writer = 17;
			env.body.repair.entries[1].owner.co_id = 38;
			env.body.repair.entries[1].custody_present = true;
			env.body.repair.entries[1].custody = custody2;
			env.body.repair.entries[1].txn_present = true;
			env.body.repair.entries[1].txn = repair_txn2;
			env.body.repair.entries[1].predecessor_present = true;
			env.body.repair.entries[1].predecessor = broken2;
			check(d2_store_apply(store, &env, &result) == D1_OK &&
				      result.entries[0].status == D1_OK &&
				      result.entries[0].phase == D2_FINALIZED &&
				      d2_store_wal_promised(store) ==
					      promise_base + 2824,
			      "error repair finalize persists");
			env.key.sequence = 27;
			env.op = D1_OP_COMMIT_REPAIR;
			d2_store_verifier(store,
					  env.body.repair.prior_verifier);
			check(d2_store_apply(store, &env, &result) == D1_OK &&
				      result.entries[0].status == D1_OK &&
				      result.entries[0].phase == D2_COMMITTED &&
				      d2_store_visible(store, &object, 23,
						       &visible) &&
				      visible.raw != broken.raw &&
				      d2_store_visible(store, &object, 24,
						       &visible) &&
				      visible.raw != broken2.raw &&
				      d2_store_wal_promised(store) ==
					      promise_base + 2004,
			      "error repair commit persists replacement");
			d2_store_expire(store, admission);
			d2_store_crash(store);
			store = NULL;
			check(d2_store_rebind(dirfd, &reopen, &binding,
					      &store) == D1_OK,
			      "two-member committed repair reopens locked");
			if (!store)
				goto done;
			repair = d2_store_repair_handle(store, repair.raw);
			episode = d2_store_episode_handle(store, episode.raw);
			custody = d2_store_custody_handle(store, custody.raw);
			custody2 = d2_store_custody_handle(store, custody2.raw);
			control_admission = d2_store_admit(store, &object, 17,
							   D1_RIGHT_CONTROL);
			fresh_admission =
				d2_store_admit(store, &object, 17,
					       D1_RIGHT_READ | D1_RIGHT_WRITE |
						       D1_RIGHT_REPAIR |
						       D1_RIGHT_SINGLE_WRITER);
			memset(&env, 0, sizeof(env));
			env.object = object;
			env.admission = control_admission;
			env.incarnation = d2_store_incarnation(store);
			fill(env.key.origin.bytes, 16, 0xe9);
			env.key.sequence = 1;
			env.op = D1_OP_RECOVERY_ADMIT;
			env.body.control.count = 2;
			env.body.control.txns[0] =
				d2_store_txn_handle(store, repair_txn.raw);
			env.body.control.txns[1] =
				d2_store_txn_handle(store, repair_txn2.raw);
			env.body.control.old_admission =
				d2_store_admission_handle(store, admission.raw);
			env.body.control.new_admission_present = true;
			env.body.control.new_admission = fresh_admission;
			env.body.control.read_epoch_present = true;
			check(d2_store_apply(store, &env, &result) == D1_OK &&
				      result.entries[0].status == D1_OK,
			      "restart atomically rebinds repair vector");
			admission = fresh_admission;
			stale_certificate_actor = d2_store_admit(
				store, &object, 18, D1_RIGHT_CONTROL);
			fill(stale_certificate_key.origin.bytes, D1_UUID_BYTES,
			     0xcb);
			stale_certificate_key.sequence = 1;
			d2_store_revoke(store, stale_certificate_actor);
			fill(certificate, sizeof(certificate), 0xc0);
			wal_bytes = d2_store_wal_bytes(store);
			check(d2_store_certificate(
				      store, stale_certificate_actor,
				      &stale_certificate_key, episode, repair,
				      certificate) == D1_STALE_AUTH &&
				      d2_store_wal_bytes(store) > wal_bytes,
			      "revoked MDS certificate refusal persists");
			wal_bytes = d2_store_wal_bytes(store);
			check(d2_store_certificate(
				      store, stale_certificate_actor,
				      &stale_certificate_key, episode, repair,
				      certificate) == D1_STALE_AUTH &&
				      d2_store_wal_bytes(store) == wal_bytes,
			      "revoked MDS certificate retry is exact");
			fill(certificate_key.origin.bytes, D1_UUID_BYTES, 0xca);
			certificate_key.sequence = 1;
			check(d2_store_certificate(store, control_admission,
						   &certificate_key, episode,
						   repair,
						   certificate) == D1_OK &&
				      d2_store_wal_promised(store) ==
					      promise_base + 1736,
			      "completion certificate install persists");
			wal_bytes = d2_store_wal_bytes(store);
			check(d2_store_certificate(store, control_admission,
						   &certificate_key, episode,
						   repair,
						   certificate) == D1_OK &&
				      d2_store_wal_bytes(store) == wal_bytes,
			      "completion certificate retry is exact");
			fill(second_certificate, sizeof(second_certificate),
			     0xd6);
			second_error_admission =
				d2_store_admit(store, &object, 18,
					       D1_RIGHT_READ | D1_RIGHT_WRITE |
						       D1_RIGHT_REPAIR |
						       D1_RIGHT_SINGLE_WRITER);
			check(committed_error_repair(
				      store, &object, control_admission,
				      second_error_admission, 18, 25, 0xd5,
				      replacement, sizeof(replacement),
				      &second_error_unlock,
				      &second_error_repair, &second_error_txn),
			      "second certificate subject completes independently");
			check(d2_store_apply(store, &second_error_unlock,
					     &result) == D1_OK &&
				      result.entries[0].status == D1_OK,
			      "second certificate subject unlocks independently");
			{
				struct d1_fixture_authority bare = { 0 };

				fill(bare.issuer.bytes, D1_UUID_BYTES, 0xcc);
				fill(bare.principal.bytes, D1_UUID_BYTES, 0xcd);
				fill(bare.session, D1_UUID_BYTES, 0xce);
				bare.authority_epoch = 1;
				bare.fence_sequence = 1;
				bare.lease_epoch = 1;
				bare.writer = 19;
				bare.rights = D1_RIGHT_CONTROL;
				bare_certificate_actor = d2_store_admit_bare(
					store, &object, &bare);
			}
			fill(bare_certificate_key.origin.bytes, D1_UUID_BYTES,
			     0xcf);
			bare_certificate_key.sequence = 1;
			wal_bytes = d2_store_wal_bytes(store);
			check(d1_admission_live(bare_certificate_actor) &&
				      d2_store_certificate(
					      store, bare_certificate_actor,
					      &bare_certificate_key, episode,
					      repair,
					      certificate) == D1_STALE_AUTH &&
				      d2_store_wal_bytes(store) == wal_bytes,
			      "bare actor certificate refusal is not recorded");
			memset(&env.body, 0, sizeof(env.body));
			env.admission = admission;
			env.key.sequence = 28;
			env.op = D1_OP_CLEAR_ERROR;
			env.body.repair.range_begin = 23;
			env.body.repair.range_end = 25;
			env.body.repair.count = 2;
			env.body.repair.cohort_present = true;
			env.body.repair.cohort = repair;
			env.body.repair.episode_present = true;
			env.body.repair.episode = episode;
			env.body.repair.certificate_present = true;
			memcpy(env.body.repair.certificate, certificate,
			       sizeof(certificate));
			env.body.repair.entries[0].index = 23;
			env.body.repair.entries[0].owner.cohort.raw = 5;
			env.body.repair.entries[0].owner.writer = 17;
			env.body.repair.entries[0].owner.co_id = 37;
			env.body.repair.entries[0].custody_present = true;
			env.body.repair.entries[0].custody = custody;
			env.body.repair.entries[1].index = 24;
			env.body.repair.entries[1].owner.cohort.raw = 5;
			env.body.repair.entries[1].owner.writer = 17;
			env.body.repair.entries[1].owner.co_id = 38;
			env.body.repair.entries[1].custody_present = true;
			env.body.repair.entries[1].custody = custody2;
			memcpy(env.body.repair.certificate, second_certificate,
			       sizeof(second_certificate));
			check(d2_store_apply(store, &env, &result) == D1_OK &&
				      result.entries[0].status == D1_STALE_AUTH,
			      "cross-cohort completion certificate is refused");
			refused_clear = env;
			refused_clear_result = result;
			env.key.sequence = 29;
			memcpy(env.body.repair.certificate, certificate,
			       sizeof(certificate));
			check(d2_store_apply(store, &env, &result) == D1_OK &&
				      result.entries[0].status == D1_OK &&
				      d2_store_wal_promised(store) ==
					      promise_base + 820,
			      "completion certificate clears error episode");
			memset(&env.body, 0, sizeof(env.body));
			env.key.sequence = 30;
			env.op = D1_OP_UNLOCK;
			env.body.repair.range_begin = 23;
			env.body.repair.range_end = 25;
			env.body.repair.count = 2;
			env.body.repair.cohort_present = true;
			env.body.repair.cohort = repair;
			env.body.repair.entries[0].index = 23;
			env.body.repair.entries[0].owner.cohort.raw = 5;
			env.body.repair.entries[0].owner.writer = 17;
			env.body.repair.entries[0].owner.co_id = 37;
			env.body.repair.entries[0].custody_present = true;
			env.body.repair.entries[0].custody = custody;
			env.body.repair.entries[1].index = 24;
			env.body.repair.entries[1].owner.cohort.raw = 5;
			env.body.repair.entries[1].owner.writer = 17;
			env.body.repair.entries[1].owner.co_id = 38;
			env.body.repair.entries[1].custody_present = true;
			env.body.repair.entries[1].custody = custody2;
			check(d2_store_apply(store, &env, &result) == D1_OK &&
				      result.entries[0].status == D1_OK &&
				      d2_store_wal_promised(store) ==
					      promise_base,
			      "cleared error repair unlock persists");
			error_unlock = env;
			error_unlock_result = result;
			check(entry_wal_semantics_valid(
				      dirfd, d2_store_wal_bytes(store),
				      &binding),
			      "pre-restart ENTRY WAL obeys frozen semantics");
			d2_store_crash(store);
			store = NULL;
			check(d2_store_rebind(dirfd, &reopen, &binding,
					      &store) == D1_OK &&
				      d2_store_wal_promised(store) ==
					      promise_base &&
				      d2_store_visible(store, &object, 23,
						       &visible) &&
				      visible.raw != broken.raw &&
				      d2_store_visible(store, &object, 24,
						       &visible) &&
				      visible.raw != broken2.raw,
			      "completed error repair replays after restart");
			if (store) {
				bare_certificate_actor =
					d2_store_admission_handle(
						store,
						bare_certificate_actor.raw);
				wal_bytes = d2_store_wal_bytes(store);
				check(d2_store_certificate(
					      store, bare_certificate_actor,
					      &bare_certificate_key,
					      d2_store_episode_handle(
						      store, episode.raw),
					      d2_store_repair_handle(
						      store, repair.raw),
					      certificate) == D1_STALE_AUTH &&
					      d2_store_wal_bytes(store) ==
						      wal_bytes,
				      "bare actor refusal stays unrecorded after restart");
				stale_certificate_actor =
					d2_store_admission_handle(
						store,
						stale_certificate_actor.raw);
				wal_bytes = d2_store_wal_bytes(store);
				check(d2_store_certificate(
					      store, stale_certificate_actor,
					      &stale_certificate_key,
					      d2_store_episode_handle(
						      store, episode.raw),
					      d2_store_repair_handle(
						      store, repair.raw),
					      certificate) == D1_STALE_AUTH &&
					      d2_store_wal_bytes(store) ==
						      wal_bytes,
				      "restart returns revoked certificate receipt");
				error_unlock.admission =
					d2_store_admission_handle(
						store,
						error_unlock.admission.raw);
				error_unlock.body.repair
					.cohort = d2_store_repair_handle(
					store,
					error_unlock.body.repair.cohort.raw);
				error_unlock.body.repair.entries[0].custody =
					d2_store_custody_handle(
						store, error_unlock.body.repair
							       .entries[0]
							       .custody.raw);
				error_unlock.body.repair.entries[1].custody =
					d2_store_custody_handle(
						store, error_unlock.body.repair
							       .entries[1]
							       .custody.raw);
				wal_bytes = d2_store_wal_bytes(store);
				check(d2_store_apply(store, &error_unlock,
						     &result) == D1_OK &&
					      same_result(
						      &result,
						      &error_unlock_result) &&
					      d2_store_wal_bytes(store) ==
						      wal_bytes,
				      "restart returns exact error unlock receipt");
				refused_clear.admission =
					d2_store_admission_handle(
						store,
						refused_clear.admission.raw);
				refused_clear.body.repair
					.cohort = d2_store_repair_handle(
					store,
					refused_clear.body.repair.cohort.raw);
				refused_clear.body.repair
					.episode = d2_store_episode_handle(
					store,
					refused_clear.body.repair.episode.raw);
				refused_clear.body.repair.entries[0].custody =
					d2_store_custody_handle(
						store, refused_clear.body.repair
							       .entries[0]
							       .custody.raw);
				refused_clear.body.repair.entries[1].custody =
					d2_store_custody_handle(
						store, refused_clear.body.repair
							       .entries[1]
							       .custody.raw);
				wal_bytes = d2_store_wal_bytes(store);
				check(d2_store_apply(store, &refused_clear,
						     &result) == D1_OK &&
					      same_result(
						      &result,
						      &refused_clear_result) &&
					      d2_store_wal_bytes(store) ==
						      wal_bytes,
				      "restart returns refused clear receipt");
				refused_repair.admission =
					d2_store_admission_handle(
						store,
						refused_repair.admission.raw);
				refused_repair.body.repair
					.episode = d2_store_episode_handle(
					store,
					refused_repair.body.repair.episode.raw);
				refused_repair.body.repair.entries[0].custody =
					d2_store_custody_handle(
						store,
						refused_repair.body.repair
							.entries[0]
							.custody.raw);
				refused_repair.body.repair.entries[0]
					.successor = d2_store_version_handle(
					store,
					refused_repair.body.repair.entries[0]
						.successor.raw);
				refused_repair.body.repair.entries[1].custody =
					d2_store_custody_handle(
						store,
						refused_repair.body.repair
							.entries[1]
							.custody.raw);
				refused_repair.body.repair.entries[1]
					.successor = d2_store_version_handle(
					store,
					refused_repair.body.repair.entries[1]
						.successor.raw);
				wal_bytes = d2_store_wal_bytes(store);
				check(d2_store_apply(store, &refused_repair,
						     &result) == D1_OK &&
					      same_result(
						      &result,
						      &refused_repair_result) &&
					      d2_store_wal_bytes(store) ==
						      wal_bytes,
				      "restart returns refused repair receipt");
				refused_mark.admission =
					d2_store_admission_handle(
						store,
						refused_mark.admission.raw);
				refused_mark.body.repair.entries[0].custody =
					d2_store_custody_handle(
						store, refused_mark.body.repair
							       .entries[0]
							       .custody.raw);
				refused_mark.body.repair.entries[0].successor =
					d2_store_version_handle(
						store, refused_mark.body.repair
							       .entries[0]
							       .successor.raw);
				refused_mark.body.repair.entries[1].custody =
					d2_store_custody_handle(
						store, refused_mark.body.repair
							       .entries[1]
							       .custody.raw);
				refused_mark.body.repair.entries[1].successor =
					d2_store_version_handle(
						store, refused_mark.body.repair
							       .entries[1]
							       .successor.raw);
				wal_bytes = d2_store_wal_bytes(store);
				check(d2_store_apply(store, &refused_mark,
						     &result) == D1_OK &&
					      same_result(
						      &result,
						      &refused_mark_result) &&
					      d2_store_wal_bytes(store) ==
						      wal_bytes,
				      "restart returns refused mark receipt");
				aborted_prepare.admission =
					d2_store_admission_handle(
						store,
						aborted_prepare.admission.raw);
				aborted_prepare.body.repair
					.cohort = d2_store_repair_handle(
					store,
					aborted_prepare.body.repair.cohort.raw);
				aborted_prepare.body.repair.entries[0].txn =
					d2_store_txn_handle(
						store,
						aborted_prepare.body.repair
							.entries[0]
							.txn.raw);
				aborted_prepare.body.repair.entries[1].txn =
					d2_store_txn_handle(
						store,
						aborted_prepare.body.repair
							.entries[1]
							.txn.raw);
				wal_bytes = d2_store_wal_bytes(store);
				check(d2_store_apply(store, &aborted_prepare,
						     &result) == D1_OK &&
					      same_result(&result,
							  &aborted_result) &&
					      d2_store_wal_bytes(store) ==
						      wal_bytes,
				      "restart returns kept-abort receipt");
			}
		}
	}
	check(store && entry_wal_semantics_valid(
			       dirfd, d2_store_wal_bytes(store), &binding),
	      "oracle ENTRY WAL obeys frozen generation and presence cells");
	if (store) {
		uint8_t byte;
		int payload_fd;

		check(d2_store_close(store) == D1_OK,
		      "close before payload corruption");
		store = NULL;
		payload_fd = openat(dirfd, "payload", O_RDWR | O_CLOEXEC);
		check(payload_fd >= 0 &&
			      pread(payload_fd, &byte, 1,
				    D2_PAYLOAD_ALIGN +
					    D2_PAYLOAD_HEADER_BYTES) == 1 &&
			      (++byte,
			       pwrite(payload_fd, &byte, 1,
				      D2_PAYLOAD_ALIGN +
					      D2_PAYLOAD_HEADER_BYTES) == 1) &&
			      fdatasync(payload_fd) == 0,
		      "corrupt a referenced payload object");
		if (payload_fd >= 0)
			close(payload_fd);
		check(d2_store_rebind(dirfd, &reopen, &binding, &store) ==
			      D1_OK,
		      "damaged superseded payload does not block restart");
		if (store) {
			uint32_t open_status;

			admission = d2_store_admit(store, &object, 17,
						   D1_RIGHT_READ);
			open_status = d2_store_view_open(store, &object,
							 admission, &selection,
							 0, sizeof(payload),
							 &view);
			check(open_status == D1_CHECKSUM && !view,
			      "damaged visible payload returns checksum after restart");
			if (view) {
				d1_view_close(view);
				view = NULL;
			}
			d2_store_close(store);
			store = NULL;
		}
	}
	unlinkat(dirfd, "super", 0);
	unlinkat(dirfd, "wal", 0);
	unlinkat(dirfd, "payload", 0);
	memset(&binding, 0, sizeof(binding));
	check(d2_store_provision(dirfd, &config, &binding, &store) == D1_OK,
	      "provision pending-corruption fixture");
	if (store) {
		uint8_t byte;
		int payload_fd;

		memcpy(reopen.files.expected_store_uuid, binding.store_uuid,
		       16);
		memcpy(reopen.files.expected_export_uuid, binding.export_uuid,
		       16);
		reopen.files.expected_root_ino = binding.root_ino;
		admission = d2_store_admit(store, &object, 17,
					   D1_RIGHT_READ | D1_RIGHT_WRITE |
						   D1_RIGHT_SINGLE_WRITER);
		memset(&env, 0, sizeof(env));
		env.object = object;
		env.admission = admission;
		env.incarnation = d2_store_incarnation(store);
		fill(env.key.origin.bytes, 16, 0xb7);
		env.op = D1_OP_WRITE_BATCH;
		guard = (struct d1_guard){ .never_written = true };
		write_request(&env, 1, 0, 1, &guard, payload, sizeof(payload));
		env.body.write.activate = false;
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].phase == D2_PREPARED,
		      "pending-corruption fixture stages prepared chunk");
		staged_txn = result.entries[0].txn;
		write_request(&env, 2, 1, 2, &guard, replacement,
			      sizeof(replacement));
		env.body.write.activate = false;
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].phase == D2_PREPARED,
		      "pending-corruption fixture stages finalized chunk");
		repair_txn = result.entries[0].txn;
		memset(&env.body, 0, sizeof(env.body));
		env.key.sequence = 3;
		env.op = D1_OP_FINALIZE_BATCH;
		env.body.lifecycle.range_begin = 1;
		env.body.lifecycle.range_end = 2;
		env.body.lifecycle.count = 1;
		env.body.lifecycle.entries[0].index = 1;
		env.body.lifecycle.entries[0].owner.cohort.raw = 1;
		env.body.lifecycle.entries[0].owner.writer = 17;
		env.body.lifecycle.entries[0].owner.co_id = 2;
		env.body.lifecycle.entries[0].txn = repair_txn;
		d2_store_verifier(store, env.body.lifecycle.prior_verifier);
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].phase == D2_FINALIZED,
		      "pending-corruption fixture finalizes second chunk");
		check(d2_store_close(store) == D1_OK,
		      "close pending-corruption fixture");
		store = NULL;
		payload_fd = openat(dirfd, "payload", O_RDWR | O_CLOEXEC);
		check(payload_fd >= 0 &&
			      pread(payload_fd, &byte, 1,
				    D2_PAYLOAD_ALIGN +
					    D2_PAYLOAD_HEADER_BYTES) == 1 &&
			      (++byte,
			       pwrite(payload_fd, &byte, 1,
				      D2_PAYLOAD_ALIGN +
					      D2_PAYLOAD_HEADER_BYTES) == 1) &&
			      pread(payload_fd, &byte, 1,
				    3u * D2_PAYLOAD_ALIGN +
					    D2_PAYLOAD_HEADER_BYTES) == 1 &&
			      (++byte,
			       pwrite(payload_fd, &byte, 1,
				      3u * D2_PAYLOAD_ALIGN +
					      D2_PAYLOAD_HEADER_BYTES) == 1) &&
			      fdatasync(payload_fd) == 0,
		      "damage prepared and finalized payload content");
		if (payload_fd >= 0)
			close(payload_fd);
		check(d2_store_rebind(dirfd, &reopen, &binding, &store) ==
			      D1_OK,
		      "damaged pending payloads remain recoverable");
		if (store) {
			control_admission = d2_store_admit(store, &object, 17,
							   D1_RIGHT_CONTROL);
			fresh_admission =
				d2_store_admit(store, &object, 17,
					       D1_RIGHT_READ | D1_RIGHT_WRITE |
						       D1_RIGHT_SINGLE_WRITER);
			memset(&recovery, 0, sizeof(recovery));
			recovery.object = object;
			recovery.admission = control_admission;
			recovery.incarnation = d2_store_incarnation(store);
			fill(recovery.key.origin.bytes, 16, 0xb8);
			recovery.key.sequence = 1;
			recovery.op = D1_OP_RECOVERY_ADMIT;
			recovery.body.control.count = 2;
			recovery.body.control.txns[0] =
				d2_store_txn_handle(store, staged_txn.raw);
			recovery.body.control.txns[1] =
				d2_store_txn_handle(store, repair_txn.raw);
			recovery.body.control.old_admission =
				d2_store_admission_handle(store, admission.raw);
			recovery.body.control.new_admission_present = true;
			recovery.body.control.new_admission = fresh_admission;
			recovery.body.control.read_epoch_present = true;
			check(d2_store_apply(store, &recovery, &result) ==
					      D1_OK &&
				      result.entries[0].status == D1_OK,
			      "re-admit transactions with damaged payloads");
			memset(&env, 0, sizeof(env));
			env.object = object;
			env.admission = fresh_admission;
			env.incarnation = d2_store_incarnation(store);
			fill(env.key.origin.bytes, 16, 0xb9);
			env.key.sequence = 1;
			env.op = D1_OP_FINALIZE_BATCH;
			env.body.lifecycle.range_begin = 0;
			env.body.lifecycle.range_end = 1;
			env.body.lifecycle.count = 1;
			env.body.lifecycle.entries[0].index = 0;
			env.body.lifecycle.entries[0].owner.cohort.raw = 1;
			env.body.lifecycle.entries[0].owner.writer = 17;
			env.body.lifecycle.entries[0].owner.co_id = 1;
			env.body.lifecycle.entries[0].txn =
				d2_store_txn_handle(store, staged_txn.raw);
			d2_store_verifier(store,
					  env.body.lifecycle.prior_verifier);
			check(d2_store_apply(store, &env, &result) == D1_OK &&
				      result.entries[0].status == D1_CHECKSUM &&
				      result.entries[0].txn_present &&
				      result.entries[0].phase == D2_PREPARED &&
				      result.entries[0].disposition ==
					      D1_COMPLETED,
			      "bound damaged payload refuses finalize reproducibly");
			refused = env;
			damaged_bound_result = result;
			wal_bytes = d2_store_wal_bytes(store);
			check(d2_store_apply(store, &refused, &result) ==
					      D1_OK &&
				      same_result(&result,
						  &damaged_bound_result) &&
				      d2_store_wal_bytes(store) == wal_bytes,
			      "bound checksum refusal replays exactly");
			env.key.sequence = 2;
			env.op = D1_OP_COMMIT_BATCH;
			env.body.lifecycle.range_begin = 1;
			env.body.lifecycle.range_end = 2;
			env.body.lifecycle.entries[0].index = 1;
			env.body.lifecycle.entries[0].owner.co_id = 2;
			env.body.lifecycle.entries[0].owner.writer = 99;
			env.body.lifecycle.entries[0].txn =
				d2_store_txn_handle(store, repair_txn.raw);
			check(d2_store_apply(store, &env, &result) == D1_OK &&
				      result.entries[0].status == D1_CHECKSUM &&
				      !result.entries[0].txn_present &&
				      result.entries[0].phase == 0 &&
				      result.entries[0].disposition ==
					      D1_COMPLETED,
			      "unbound damaged payload omits unreplayable phase");
			unbound_refused = env;
			damaged_unbound_result = result;
			d2_store_crash(store);
			store = NULL;
			check(d2_store_rebind(dirfd, &reopen, &binding,
					      &store) == D1_OK,
			      "checksum publication refusals survive restart");
			if (store) {
				refused.admission = d2_store_admission_handle(
					store, refused.admission.raw);
				refused.body.lifecycle.entries[0].txn =
					d2_store_txn_handle(
						store, refused.body.lifecycle
							       .entries[0]
							       .txn.raw);
				wal_bytes = d2_store_wal_bytes(store);
				check(d2_store_apply(store, &refused,
						     &result) == D1_OK &&
					      same_result(
						      &result,
						      &damaged_bound_result) &&
					      d2_store_wal_bytes(store) ==
						      wal_bytes,
				      "restarted bound damage refusal replays exactly");
				unbound_refused.admission =
					d2_store_admission_handle(
						store,
						unbound_refused.admission.raw);
				unbound_refused.body.lifecycle.entries[0].txn =
					d2_store_txn_handle(
						store,
						unbound_refused.body.lifecycle
							.entries[0]
							.txn.raw);
				wal_bytes = d2_store_wal_bytes(store);
				check(d2_store_apply(store, &unbound_refused,
						     &result) == D1_OK &&
					      same_result(
						      &result,
						      &damaged_unbound_result) &&
					      d2_store_wal_bytes(store) ==
						      wal_bytes,
				      "restarted unbound damage refusal replays exactly");
				d2_store_close(store);
				store = NULL;
			}
		}
	}
	unlinkat(dirfd, "super", 0);
	unlinkat(dirfd, "wal", 0);
	unlinkat(dirfd, "payload", 0);
	memset(&binding, 0, sizeof(binding));
	check(d2_store_provision(dirfd, &config, &binding, &store) == D1_OK,
	      "provision visible-corruption fixture");
	if (store) {
		uint8_t byte;
		int payload_fd;

		memcpy(reopen.files.expected_store_uuid, binding.store_uuid,
		       16);
		memcpy(reopen.files.expected_export_uuid, binding.export_uuid,
		       16);
		reopen.files.expected_root_ino = binding.root_ino;
		admission = d2_store_admit(store, &object, 17,
					   D1_RIGHT_READ | D1_RIGHT_WRITE |
						   D1_RIGHT_SINGLE_WRITER);
		memset(&env, 0, sizeof(env));
		env.object = object;
		env.admission = admission;
		env.incarnation = d2_store_incarnation(store);
		fill(env.key.origin.bytes, 16, 0xb6);
		env.op = D1_OP_WRITE_BATCH;
		guard = (struct d1_guard){ .never_written = true };
		write_request(&env, 1, 0, 1, &guard, payload, sizeof(payload));
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_OK,
		      "visible-corruption fixture writes target chunk");
		retired_exact = env;
		write_request(&env, 2, 1, 2, &guard, replacement,
			      sizeof(replacement));
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_OK,
		      "visible-corruption fixture writes healthy chunk");
		write_request(&env, 3, 2, 3, &guard, payload, sizeof(payload));
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_OK &&
			      d2_store_guard(store, &object, 2, &guard) &&
			      d2_store_visible(store, &object, 2, &predecessor),
		      "visible-corruption fixture writes superseded chunk");
		write_request(&env, 4, 2, 4, &guard, replacement,
			      sizeof(replacement));
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_OK,
		      "visible-corruption fixture stages replacement chunk");
		staged_txn = result.entries[0].txn;
		memset(&env.body, 0, sizeof(env.body));
		env.key.sequence = 5;
		env.op = D1_OP_FINALIZE_BATCH;
		env.body.lifecycle.range_begin = 2;
		env.body.lifecycle.range_end = 3;
		env.body.lifecycle.count = 1;
		env.body.lifecycle.entries[0].index = 2;
		env.body.lifecycle.entries[0].owner.cohort.raw = 1;
		env.body.lifecycle.entries[0].owner.writer = 17;
		env.body.lifecycle.entries[0].owner.co_id = 4;
		env.body.lifecycle.entries[0].txn = staged_txn;
		env.body.lifecycle.entries[0].predecessor_present = true;
		env.body.lifecycle.entries[0].predecessor = predecessor;
		d2_store_verifier(store, env.body.lifecycle.prior_verifier);
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].phase == D2_FINALIZED,
		      "visible-corruption fixture finalizes replacement chunk");
		env.key.sequence = 6;
		env.op = D1_OP_COMMIT_BATCH;
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].phase == D2_COMMITTED,
		      "visible-corruption fixture commits replacement chunk");
		check(d2_store_close(store) == D1_OK,
		      "close visible-corruption fixture");
		store = NULL;
		payload_fd = openat(dirfd, "payload", O_RDWR | O_CLOEXEC);
		check(payload_fd >= 0 &&
			      pread(payload_fd, &byte, 1,
				    D2_PAYLOAD_ALIGN +
					    D2_PAYLOAD_HEADER_BYTES) == 1 &&
			      (++byte,
			       pwrite(payload_fd, &byte, 1,
				      D2_PAYLOAD_ALIGN +
					      D2_PAYLOAD_HEADER_BYTES) == 1) &&
			      pread(payload_fd, &byte, 1,
				    5u * D2_PAYLOAD_ALIGN +
					    D2_PAYLOAD_HEADER_BYTES) == 1 &&
			      (++byte,
			       pwrite(payload_fd, &byte, 1,
				      5u * D2_PAYLOAD_ALIGN +
					      D2_PAYLOAD_HEADER_BYTES) == 1) &&
			      fdatasync(payload_fd) == 0,
		      "damage visible and superseded payload content");
		if (payload_fd >= 0)
			close(payload_fd);
		check(d2_store_rebind(dirfd, &reopen, &binding, &store) ==
			      D1_OK,
		      "visible payload damage does not block restart");
		if (store) {
			uint32_t damaged_status;

			admission = d2_store_admit(store, &object, 17,
						   D1_RIGHT_READ);
			damaged_status = d2_store_view_open(
				store, &object, admission, &selection, 0,
				sizeof(payload), &view);
			check(damaged_status == D1_CHECKSUM && !view,
			      "damaged visible chunk returns checksum");
			check(d2_store_view_open(store, &object, admission,
						 &selection, sizeof(payload),
						 2u * sizeof(payload),
						 &view) == D1_OK &&
				      d1_view_read(view, sizeof(payload),
						   readback, sizeof(readback),
						   &read_len) == D1_OK &&
				      read_len == sizeof(readback) &&
				      !memcmp(readback, replacement,
					      sizeof(replacement)),
			      "unrelated visible chunk remains readable");
			if (view) {
				d1_view_close(view);
				view = NULL;
			}
			{
				uint32_t open_status, read_status = D1_INVALID;

				open_status = d2_store_view_open(
					store, &object, admission, &selection,
					2u * sizeof(payload),
					3u * sizeof(payload), &view);
				if (open_status == D1_OK)
					read_status = d1_view_read(
						view, 2u * sizeof(payload),
						readback, sizeof(readback),
						&read_len);
				check(open_status == D1_OK &&
					      read_status == D1_OK &&
					      read_len == sizeof(readback) &&
					      !memcmp(readback, replacement,
						      sizeof(replacement)),
				      "damaged superseded object has no visible consequence");
			}
			if (view) {
				d1_view_close(view);
				view = NULL;
			}
			wal_bytes = d2_store_wal_bytes(store);
			retired_exact.admission = d2_store_admission_handle(
				store, retired_exact.admission.raw);
			check(d2_store_apply(store, &retired_exact, &result) ==
					      D1_OK &&
				      result.entries[0].status == D1_OK &&
				      d2_store_wal_bytes(store) == wal_bytes,
			      "damaged write still returns exact receipt");
			d2_store_close(store);
			store = NULL;
		}
	}
	unlinkat(dirfd, "super", 0);
	unlinkat(dirfd, "wal", 0);
	unlinkat(dirfd, "payload", 0);
	config.files.capacity_payload_bytes = 2u * D2_PAYLOAD_ALIGN;
	memset(&binding, 0, sizeof(binding));
	check(d2_store_provision(dirfd, &config, &binding, &store) == D1_OK,
	      "provision payload-capacity fixture");
	if (store) {
		admission = d2_store_admit(store, &object, 17,
					   D1_RIGHT_READ | D1_RIGHT_WRITE |
						   D1_RIGHT_SINGLE_WRITER);
		memset(&env, 0, sizeof(env));
		env.object = object;
		env.admission = admission;
		env.incarnation = d2_store_incarnation(store);
		fill(env.key.origin.bytes, 16, 0xb8);
		env.op = D1_OP_WRITE_BATCH;
		guard = (struct d1_guard){ .never_written = true };
		write_request(&env, 1, 0, 1, &guard, payload, sizeof(payload));
		wal_bytes = d2_store_wal_bytes(store);
		check(d2_store_apply(store, &env, &result) == D1_NOSPC &&
			      result.disposition == D1_UNRECORDED &&
			      result.entries[0].disposition == D1_UNRECORDED &&
			      d2_store_wal_bytes(store) == wal_bytes &&
			      !d2_store_visible(store, &object, 0, &visible),
		      "payload capacity refuses before model mutation");
		write_request(&env, 2, 0, 2, &guard, payload, 1);
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_OK &&
			      d2_store_visible(store, &object, 0, &visible),
		      "store remains usable after capacity refusal");
		guard = (struct d1_guard){ .never_written = true };
		write_request(&env, 3, 0, 3, &guard, payload, sizeof(payload));
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_GUARDED,
		      "semantic refusal precedes payload capacity");
		check(d2_store_guard(store, &object, 0, &guard),
		      "capacity fixture reads committed guard");
		write_request(&env, 4, 0, 4, &guard, payload, sizeof(payload));
		check(d2_store_apply(store, &env, &result) == D1_NOSPC &&
			      result.disposition == D1_UNRECORDED &&
			      d2_store_visible(store, &object, 0, &visible),
		      "valid replacement is refused when payload is full");
	}
	if (store) {
		d2_store_close(store);
		store = NULL;
	}
	unlinkat(dirfd, "super", 0);
	unlinkat(dirfd, "wal", 0);
	unlinkat(dirfd, "payload", 0);
	config.files.capacity_payload_bytes = 64u * 1024u * 1024u;
	memset(&binding, 0, sizeof(binding));
	check(d2_store_provision(dirfd, &config, &binding, &store) == D1_OK,
	      "provision retirement fixture");
	if (store) {
		uint64_t retired_incarnation;
		d1_admission_id retired_control;
		struct d1_opkey retired_certificate_key = { 0 };
		uint8_t retired_certificate[D1_CERTIFICATE_BYTES];

		memcpy(reopen.files.expected_store_uuid, binding.store_uuid,
		       16);
		memcpy(reopen.files.expected_export_uuid, binding.export_uuid,
		       16);
		reopen.files.expected_root_ino = binding.root_ino;
		admission = d2_store_admit(store, &object, 17,
					   D1_RIGHT_READ | D1_RIGHT_WRITE |
						   D1_RIGHT_SINGLE_WRITER);
		retired_control =
			d2_store_admit(store, &object, 19, D1_RIGHT_CONTROL);
		memset(&env, 0, sizeof(env));
		env.object = object;
		env.admission = admission;
		env.incarnation = d2_store_incarnation(store);
		fill(env.key.origin.bytes, 16, 0xba);
		env.op = D1_OP_WRITE_BATCH;
		guard = (struct d1_guard){ .never_written = true };
		write_request(&env, 1, 0, 1, &guard, payload, sizeof(payload));
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_OK,
		      "retirement fixture records exact receipt");
		retired_exact = env;
		wal_bytes = d2_store_wal_bytes(store);
		check(d2_store_tombstone_file(store, &object, 1) == D1_OK &&
			      d2_store_wal_bytes(store) == wal_bytes + 248u,
		      "file tombstone publishes its terminal record");
		wal_bytes = d2_store_wal_bytes(store);
		write_request(&env, 2, 1, 2, &guard, payload, sizeof(payload));
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_BAD_PHASE &&
			      result.disposition == D1_UNRECORDED &&
			      d2_store_wal_bytes(store) == wal_bytes,
		      "tombstoned file rejects later mutation");
		d2_store_crash(store);
		store = NULL;
		check(d2_store_rebind(dirfd, &reopen, &binding, &store) ==
			      D1_OK,
		      "file tombstone replays on restart");
		if (!store)
			goto done;
		wal_bytes = d2_store_wal_bytes(store);
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_BAD_PHASE &&
			      d2_store_wal_bytes(store) == wal_bytes,
		      "replayed file tombstone remains terminal");
		retired_incarnation = d2_store_incarnation(store);
		wal_bytes = d2_store_wal_bytes(store);
		check(d2_store_retire(store, 1) == D1_OK &&
			      d2_store_wal_bytes(store) == wal_bytes + 224u,
		      "export tombstone is the final record");
		wal_bytes = d2_store_wal_bytes(store);
		fill(retired_certificate_key.origin.bytes, D1_UUID_BYTES, 0xd0);
		retired_certificate_key.sequence = 1;
		fill(retired_certificate, sizeof(retired_certificate), 0xd1);
		check(d2_store_certificate(
			      store, retired_control, &retired_certificate_key,
			      (d1_episode_id){ .raw = 1 },
			      (d1_repair_id){ .raw = 1 },
			      retired_certificate) == D1_BAD_PHASE &&
			      d2_store_trust_admission(store, retired_control,
						       admission) ==
				      D1_BAD_PHASE &&
			      d2_store_admit_authority(store, retired_control,
						       &admission,
						       1) == D1_BAD_PHASE &&
			      d2_store_wal_bytes(store) == wal_bytes,
		      "retired controls refuse without appending records");
		write_request(&env, 3, 2, 3, &guard, payload, sizeof(payload));
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_BAD_PHASE &&
			      result.disposition == D1_UNRECORDED &&
			      d2_store_wal_bytes(store) == wal_bytes,
		      "retired store rejects mutation without a receipt");
		d2_store_crash(store);
		store = NULL;
		check(d2_store_rebind(dirfd, &reopen, &binding, &store) ==
				      D1_OK &&
			      d2_store_wal_bytes(store) == wal_bytes &&
			      d2_store_incarnation(store) ==
				      retired_incarnation,
		      "retired restart appends no START");
		if (store) {
			retired_control = d2_store_admission_handle(
				store, retired_control.raw);
			admission =
				d2_store_admission_handle(store, admission.raw);
			check(d2_store_certificate(store, retired_control,
						   &retired_certificate_key,
						   (d1_episode_id){ .raw = 1 },
						   (d1_repair_id){ .raw = 1 },
						   retired_certificate) ==
					      D1_BAD_PHASE &&
				      d2_store_trust_admission(
					      store, retired_control,
					      admission) == D1_BAD_PHASE &&
				      d2_store_admit_authority(
					      store, retired_control,
					      &admission, 1) == D1_BAD_PHASE &&
				      d2_store_wal_bytes(store) == wal_bytes,
			      "retired controls remain unrecorded after restart");
			retired_exact.admission = d2_store_admission_handle(
				store, retired_exact.admission.raw);
			check(d2_store_apply(store, &retired_exact, &result) ==
					      D1_OK &&
				      result.entries[0].status == D1_OK &&
				      d2_store_wal_bytes(store) == wal_bytes,
			      "retired store returns exact durable receipt");
			d2_store_crash(store);
			store = NULL;
			check(d2_store_rebind(dirfd, &reopen, &binding,
					      &store) == D1_OK &&
				      d2_store_wal_bytes(store) == wal_bytes &&
				      d2_store_incarnation(store) ==
					      retired_incarnation,
			      "persisted retired restart writes no WAL");
		}
	}
	if (store) {
		d2_store_close(store);
		store = NULL;
	}
	unlinkat(dirfd, "super", 0);
	unlinkat(dirfd, "wal", 0);
	unlinkat(dirfd, "payload", 0);
	memset(&binding, 0, sizeof(binding));
	check(d2_store_provision(dirfd, &config, &binding, &store) == D1_OK,
	      "provision retired-super disagreement fixture");
	if (store) {
		struct d2_files *files = NULL;

		memcpy(reopen.files.expected_store_uuid, binding.store_uuid,
		       16);
		memcpy(reopen.files.expected_export_uuid, binding.export_uuid,
		       16);
		reopen.files.expected_root_ino = binding.root_ino;
		d2_store_crash(store);
		store = NULL;
		check(d2_files_rebind(dirfd, &reopen.files, &binding, &files) ==
				      D1_OK &&
			      d2_files_super_update(files, D2_SB_RETIRED) ==
				      D1_OK,
		      "forge retired superblock without tombstone");
		if (files)
			d2_files_close(files);
		check(d2_store_rebind(dirfd, &reopen, &binding, &store) ==
				      D1_IO &&
			      !store,
		      "retired superblock disagreement fences");
	}
	unlinkat(dirfd, "super", 0);
	unlinkat(dirfd, "wal", 0);
	unlinkat(dirfd, "payload", 0);
	memset(&binding, 0, sizeof(binding));
	check(d2_store_provision(dirfd, &config, &binding, &store) == D1_OK,
	      "provision batched-authority fixture");
	if (store) {
		struct d1_fixture_authority mds = { 0 }, first = { 0 },
					    second = { 0 }, third = { 0 };
		struct d1_objkey second_object = object;
		struct d1_envelope authority_refused;
		d1_admission_id mds_id, first_id, second_id, third_id;
		d1_admission_id beneficiaries[2], repeated[2];
		d1_version_id first_version;

		fill(mds.issuer.bytes, sizeof(mds.issuer.bytes), 0x31);
		fill(mds.principal.bytes, sizeof(mds.principal.bytes), 0x41);
		fill(mds.session, sizeof(mds.session), 0x51);
		mds.writer = 90;
		mds.rights = D1_RIGHT_CONTROL;
		mds.authority_epoch = 7;
		first = mds;
		fill(first.principal.bytes, sizeof(first.principal.bytes),
		     0x61);
		fill(first.session, sizeof(first.session), 0x71);
		first.writer = 91;
		first.rights = D1_RIGHT_READ | D1_RIGHT_WRITE |
			       D1_RIGHT_SINGLE_WRITER;
		first.lease_epoch = 11;
		first.fence_sequence = 21;
		second = first;
		fill(second.principal.bytes, sizeof(second.principal.bytes),
		     0x81);
		fill(second.session, sizeof(second.session), 0x91);
		second.writer = 92;
		second.lease_epoch = 12;
		second.fence_sequence = 22;
		fill(second_object.object_uuid.bytes,
		     sizeof(second_object.object_uuid.bytes), 0x70);
		mds_id = d2_store_admit_full(store, &object, &mds);
		first_id = d2_store_admit_bare(store, &object, &first);
		second_id = d2_store_admit_bare(store, &second_object, &second);
		check(d1_admission_live(mds_id) &&
			      d1_admission_live(first_id) &&
			      d1_admission_live(second_id) &&
			      mds_id.raw != first_id.raw &&
			      mds_id.raw != second_id.raw,
		      "distinct MDS and beneficiary admissions are issued");
		memset(&env, 0, sizeof(env));
		env.object = object;
		env.admission = first_id;
		env.incarnation = d2_store_incarnation(store);
		env.op = D1_OP_WRITE_BATCH;
		fill(env.key.origin.bytes, sizeof(env.key.origin.bytes), 0xcc);
		guard = (struct d1_guard){ .never_written = true };
		write_request(&env, 1, 0, 50, &guard, payload, sizeof(payload));
		env.body.write.entries[0].owner.writer = first.writer;
		wal_bytes = d2_store_wal_bytes(store);
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_STALE_AUTH &&
			      result.disposition == D1_UNRECORDED &&
			      d2_store_wal_bytes(store) == wal_bytes,
		      "beneficiary cannot mutate before authority is recorded");
		check(d2_store_trust_admission(store, mds_id, first_id) ==
				      D1_OK &&
			      d2_store_trust_admission(store, mds_id,
						       second_id) == D1_OK,
		      "MDS records beneficiary trust before authority");
		wal_bytes = d2_store_wal_bytes(store);
		check(d2_store_trust_admission(store, mds_id, first_id) ==
				      D1_STALE_AUTH &&
			      d2_store_wal_bytes(store) == wal_bytes,
		      "second trust request cannot retarget a stateid");
		repeated[0] = first_id;
		repeated[1] = first_id;
		wal_bytes = d2_store_wal_bytes(store);
		check(d2_store_admit_authority(store, mds_id, repeated, 2) ==
				      D1_INVALID &&
			      d2_store_wal_bytes(store) == wal_bytes,
		      "duplicate authority vector is rejected before append");
		beneficiaries[0] = first_id;
		beneficiaries[1] = second_id;
		check(d2_store_admit_authority(store, mds_id, beneficiaries,
					       2) == D1_OK,
		      "one MDS authority record covers two beneficiaries");
		env.admission = first_id;
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_OK,
		      "first beneficiary mutates after batched authority");
		first_version = result.entries[0].version;
		env.admission = second_id;
		env.object = second_object;
		write_request(&env, 3, 1, 51, &guard, replacement,
			      sizeof(replacement));
		env.body.write.entries[0].owner.writer = second.writer;
		wal_bytes = d2_store_wal_bytes(store);
		env.body.write.entries[0].expected.never_written = false;
		env.body.write.entries[0].expected.generation = 1;
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_GUARDED &&
			      d2_store_wal_bytes(store) ==
				      wal_bytes + D2_ENTRY_RECORD_BYTES,
		      "refused entry does not bind admission to object");
		env.body.write.entries[0].expected = guard;
		write_request(&env, 2, 1, 51, &guard, replacement,
			      sizeof(replacement));
		env.body.write.entries[0].owner.writer = second.writer;
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_OK,
		      "second beneficiary mutates after batched authority");
		check(d2_store_revoke_authority(store, mds_id, &mds.issuer, 7,
						1) == D1_OK,
		      "MDS revokes the admitted authority epoch");
		third = first;
		fill(third.principal.bytes, sizeof(third.principal.bytes),
		     0xa1);
		fill(third.session, sizeof(third.session), 0xb1);
		third.writer = 93;
		third_id = d2_store_admit_bare(store, &object, &third);
		wal_bytes = d2_store_wal_bytes(store);
		check(d1_admission_live(third_id) &&
			      d2_store_trust_admission(store, mds_id,
						       third_id) ==
				      D1_STALE_AUTH &&
			      d2_store_admit_authority(store, mds_id, &third_id,
						       1) == D1_STALE_AUTH &&
			      !d1_custody_live(
				      d2_store_custody(store, first_version)) &&
			      d2_store_wal_bytes(store) == wal_bytes,
		      "revoked authority cannot issue trust, authority or custody");
		wal_bytes = d2_store_wal_bytes(store);
		env.admission = first_id;
		env.object = object;
		write_request(&env, 4, 2, 52, &guard, payload, sizeof(payload));
		env.body.write.entries[0].owner.writer = first.writer;
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_STALE_AUTH &&
			      d2_store_wal_bytes(store) ==
				      wal_bytes + D2_ENTRY_RECORD_BYTES,
		      "revoked authority records a stale mutation receipt");
		authority_refused = env;
		{
			uint8_t first_handle[32];
			uint8_t first_file_key[32];

			memcpy(first_handle, object.export_uuid.bytes, 16);
			memcpy(first_handle + 16, object.object_uuid.bytes, 16);
			d2_file_key(first_handle, sizeof(first_handle),
				    first_file_key);
			check(retarget_refused_entry(
				      dirfd, d2_store_wal_bytes(store),
				      &binding, second_id.raw, first_file_key),
			      "refused entry is retargeted for replay coverage");
		}
		memcpy(reopen.files.expected_store_uuid, binding.store_uuid,
		       16);
		memcpy(reopen.files.expected_export_uuid, binding.export_uuid,
		       16);
		reopen.files.expected_root_ino = binding.root_ino;
		d2_store_crash(store);
		store = NULL;
		check(d2_store_rebind(dirfd, &reopen, &binding, &store) ==
			      D1_OK,
		      "batched authority admissions replay");
		check(store && d2_store_visible(store, &object, 0, &visible) &&
			      d2_store_visible(store, &second_object, 1,
					       &visible),
		      "pending admissions bind to their used objects");
		if (store) {
			authority_refused.admission = d2_store_admission_handle(
				store, authority_refused.admission.raw);
			wal_bytes = d2_store_wal_bytes(store);
			check(d2_store_apply(store, &authority_refused,
					     &result) == D1_OK &&
				      result.entries[0].status ==
					      D1_STALE_AUTH &&
				      d2_store_wal_bytes(store) == wal_bytes,
			      "revoked authority receipt replays after restart");
		}
	}
	if (store) {
		check(d2_store_close(store) == D1_OK,
		      "close batched-authority fixture");
		store = NULL;
	}
	unlinkat(dirfd, "super", 0);
	unlinkat(dirfd, "wal", 0);
	unlinkat(dirfd, "payload", 0);
	memset(&binding, 0, sizeof(binding));
	check(d2_store_provision(dirfd, &config, &binding, &store) == D1_OK,
	      "provision reverse authority-order fixture");
	if (store) {
		struct d1_fixture_authority mds = { 0 }, first = { 0 },
					    second = { 0 };
		struct d1_objkey second_object = object;
		d1_admission_id mds_id, first_id, second_id;
		d1_admission_id beneficiaries[2];

		fill(mds.issuer.bytes, sizeof(mds.issuer.bytes), 0x32);
		fill(mds.principal.bytes, sizeof(mds.principal.bytes), 0x42);
		fill(mds.session, sizeof(mds.session), 0x52);
		mds.writer = 94;
		mds.rights = D1_RIGHT_CONTROL;
		mds.authority_epoch = 8;
		first = mds;
		fill(first.principal.bytes, sizeof(first.principal.bytes),
		     0x62);
		fill(first.session, sizeof(first.session), 0x72);
		first.writer = 95;
		first.rights = D1_RIGHT_READ | D1_RIGHT_WRITE |
			       D1_RIGHT_SINGLE_WRITER;
		first.lease_epoch = 13;
		first.fence_sequence = 23;
		second = first;
		fill(second.principal.bytes, sizeof(second.principal.bytes),
		     0x82);
		fill(second.session, sizeof(second.session), 0x92);
		second.writer = 96;
		second.lease_epoch = 14;
		second.fence_sequence = 24;
		fill(second_object.object_uuid.bytes,
		     sizeof(second_object.object_uuid.bytes), 0x71);
		mds_id = d2_store_admit_full(store, &object, &mds);
		first_id = d2_store_admit_bare(store, &object, &first);
		second_id = d2_store_admit_bare(store, &second_object, &second);
		beneficiaries[0] = first_id;
		beneficiaries[1] = second_id;
		check(d1_admission_live(mds_id) &&
			      d1_admission_live(first_id) &&
			      d1_admission_live(second_id) &&
			      first_id.raw < second_id.raw &&
			      d2_store_trust_admission(store, mds_id,
						       first_id) == D1_OK &&
			      d2_store_trust_admission(store, mds_id,
						       second_id) == D1_OK &&
			      d2_store_admit_authority(
				      store, mds_id, beneficiaries, 2) == D1_OK,
		      "two ordered admissions receive authority");
		memset(&env, 0, sizeof(env));
		env.object = second_object;
		env.admission = second_id;
		env.incarnation = d2_store_incarnation(store);
		env.op = D1_OP_WRITE_BATCH;
		fill(env.key.origin.bytes, sizeof(env.key.origin.bytes), 0xcd);
		write_request(&env, 1, 1, 54, &guard, replacement,
			      sizeof(replacement));
		env.body.write.entries[0].owner.writer = second.writer;
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_OK,
		      "higher admission mutates its object first");
		env.object = object;
		env.admission = first_id;
		write_request(&env, 2, 0, 53, &guard, payload, sizeof(payload));
		env.body.write.entries[0].owner.writer = first.writer;
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_OK,
		      "lower admission mutates its object second");
		memcpy(reopen.files.expected_store_uuid, binding.store_uuid,
		       16);
		memcpy(reopen.files.expected_export_uuid, binding.export_uuid,
		       16);
		reopen.files.expected_root_ino = binding.root_ino;
		d2_store_crash(store);
		store = NULL;
		check(d2_store_rebind(dirfd, &reopen, &binding, &store) ==
			      D1_OK,
		      "reverse first-use authority admissions replay");
		check(store && d2_store_visible(store, &object, 0, &visible) &&
			      d2_store_visible(store, &second_object, 1,
					       &visible),
		      "reverse first-use admissions retain their objects");
	}
	if (store) {
		check(d2_store_close(store) == D1_OK,
		      "close reverse authority-order fixture");
		store = NULL;
	}
	unlinkat(dirfd, "super", 0);
	unlinkat(dirfd, "wal", 0);
	unlinkat(dirfd, "payload", 0);
	memset(&binding, 0, sizeof(binding));
	check(d2_store_provision(dirfd, &config, &binding, &store) == D1_OK,
	      "provision authority-transition race fixture");
	if (store) {
		enum { AUTHORITY_RACE_RUNS = 8 };
		struct d1_envelope retries[AUTHORITY_RACE_RUNS];
		uint64_t txn_ids[AUTHORITY_RACE_RUNS];
		uint64_t admission_ids[AUTHORITY_RACE_RUNS];
		bool revoke_first[AUTHORITY_RACE_RUNS] = { 0 };
		unsigned int i;
		bool races_ok = true, retries_ok = true;

		for (i = 0; i < AUTHORITY_RACE_RUNS; i++) {
			struct d1_fixture_authority mds = { 0 }, client = { 0 };
			struct authority_race race = { .store = store };
			struct d1_result prepared;
			d1_admission_id mds_id, client_id;
			d1_txn_id txn;
			pthread_t transition_thread, revoke_thread;
			uint64_t begin, end;
			uint32_t prepare_status;
			int transition_created, revoke_created;
			bool log_ok, run_ok;

			fill(mds.issuer.bytes, sizeof(mds.issuer.bytes),
			     (uint8_t)(0xa0 + i));
			fill(mds.principal.bytes, sizeof(mds.principal.bytes),
			     (uint8_t)(0x20 + i));
			fill(mds.session, sizeof(mds.session),
			     (uint8_t)(0x40 + i));
			mds.writer = 100 + i;
			mds.rights = D1_RIGHT_CONTROL;
			mds.authority_epoch = 100 + i;
			client = mds;
			fill(client.principal.bytes,
			     sizeof(client.principal.bytes),
			     (uint8_t)(0x60 + i));
			fill(client.session, sizeof(client.session),
			     (uint8_t)(0x80 + i));
			client.writer = 200 + i;
			client.rights = D1_RIGHT_READ | D1_RIGHT_WRITE |
					D1_RIGHT_SINGLE_WRITER;
			client.lease_epoch = 200 + i;
			client.fence_sequence = 300 + i;
			mds_id = d2_store_admit_full(store, &object, &mds);
			client_id =
				d2_store_admit_bare(store, &object, &client);
			if (!d1_admission_live(mds_id) ||
			    !d1_admission_live(client_id) ||
			    d2_store_trust_admission(store, mds_id,
						     client_id) != D1_OK ||
			    d2_store_admit_authority(store, mds_id, &client_id,
						     1) != D1_OK) {
				races_ok = false;
				break;
			}
			memset(&env, 0, sizeof(env));
			env.object = object;
			env.admission = client_id;
			env.incarnation = d2_store_incarnation(store);
			fill(env.key.origin.bytes, sizeof(env.key.origin.bytes),
			     (uint8_t)(0xc0 + i));
			env.op = D1_OP_WRITE_BATCH;
			guard = (struct d1_guard){ .never_written = true };
			write_request(&env, 1, 32 + i, 100 + i, &guard, payload,
				      sizeof(payload));
			env.body.write.activate = false;
			env.body.write.entries[0].owner.writer = client.writer;
			prepare_status = d2_store_apply(store, &env, &prepared);
			if (prepare_status != D1_OK ||
			    prepared.entries[0].status != D1_OK ||
			    prepared.entries[0].phase != D2_PREPARED) {
				races_ok = false;
				break;
			}
			txn = prepared.entries[0].txn;
			race.actor = mds_id;
			race.issuer = mds.issuer;
			race.epoch = mds.authority_epoch;
			race.transition.object = object;
			race.transition.admission = client_id;
			race.transition.incarnation = env.incarnation;
			race.transition.key = env.key;
			race.transition.key.sequence = 2;
			race.transition.op = D1_OP_FINALIZE_BATCH;
			race.transition.body.lifecycle.range_begin = 32 + i;
			race.transition.body.lifecycle.range_end = 33 + i;
			race.transition.body.lifecycle.count = 1;
			race.transition.body.lifecycle.entries[0].index =
				32 + i;
			race.transition.body.lifecycle.entries[0].owner =
				env.body.write.entries[0].owner;
			race.transition.body.lifecycle.entries[0].txn = txn;
			d2_store_verifier(
				store,
				race.transition.body.lifecycle.prior_verifier);
			atomic_init(&race.go, false);
			atomic_init(&race.first_done, false);
			race.forced_order = i == 0 ? 1 : i == 1 ? 2 : 0;
			begin = d2_store_wal_bytes(store);
			transition_created =
				pthread_create(&transition_thread, NULL,
					       race_transition, &race);
			revoke_created =
				transition_created ?
					-1 :
					pthread_create(&revoke_thread, NULL,
						       race_revoke, &race);
			atomic_store_explicit(&race.go, true,
					      memory_order_release);
			if (!transition_created)
				pthread_join(transition_thread, NULL);
			if (!revoke_created)
				pthread_join(revoke_thread, NULL);
			end = d2_store_wal_bytes(store);
			log_ok = authority_race_log(dirfd, begin, end, &binding,
						    txn.raw, race.epoch,
						    &revoke_first[i]);
			run_ok = !transition_created && !revoke_created &&
				 race.transition_status == D1_OK &&
				 race.revoke_status == D1_OK && log_ok &&
				 (revoke_first[i] ?
					  race.transition_result.entries[0]
								  .status ==
							  D1_STALE_AUTH &&
						  race.transition_result
								  .entries[0]
								  .phase == 0 :
					  race.transition_result.entries[0]
								  .status ==
							  D1_OK &&
						  race.transition_result
								  .entries[0]
								  .phase ==
							  D2_FINALIZED);
			{
				uint32_t phase;
				uint64_t admission_id;

				run_ok = run_ok &&
					 d2_store_txn_state(store, txn.raw,
							    &phase,
							    &admission_id) &&
					 phase == (revoke_first[i] ?
							   D2_PREPARED :
							   D2_FINALIZED) &&
					 admission_id == client_id.raw;
			}
			races_ok = races_ok && run_ok;
			retries[i] = race.transition;
			txn_ids[i] = txn.raw;
			admission_ids[i] = client_id.raw;
		}
		check(races_ok,
		      "authority revoke serializes with prepared finalize");
		memcpy(reopen.files.expected_store_uuid, binding.store_uuid,
		       16);
		memcpy(reopen.files.expected_export_uuid, binding.export_uuid,
		       16);
		reopen.files.expected_root_ino = binding.root_ino;
		d2_store_crash(store);
		store = NULL;
		check(d2_store_rebind(dirfd, &reopen, &binding, &store) ==
			      D1_OK,
		      "authority-transition race log replays");
		if (store && races_ok) {
			wal_bytes = d2_store_wal_bytes(store);
			for (i = 0; i < AUTHORITY_RACE_RUNS; i++) {
				uint32_t phase, retry_status;
				uint64_t admission_id;

				retries[i].admission = d2_store_admission_handle(
					store, retries[i].admission.raw);
				retry_status = d2_store_apply(
					store, &retries[i], &result);

				if (retry_status != D1_OK ||
				    result.entries[0].status !=
					    (revoke_first[i] ? D1_STALE_AUTH :
							       D1_OK) ||
				    result.entries[0].phase !=
					    (revoke_first[i] ? 0 :
							       D2_FINALIZED) ||
				    !d2_store_txn_state(store, txn_ids[i],
							&phase,
							&admission_id) ||
				    phase != (revoke_first[i] ? D2_PREPARED :
								D2_FINALIZED) ||
				    admission_id != admission_ids[i] ||
				    d2_store_wal_bytes(store) != wal_bytes)
					retries_ok = false;
			}
		}
		check(store && retries_ok,
		      "authority-transition race receipts replay exactly");
	}
	if (store) {
		check(d2_store_close(store) == D1_OK,
		      "close authority-transition race fixture");
		store = NULL;
	}
	unlinkat(dirfd, "super", 0);
	unlinkat(dirfd, "wal", 0);
	unlinkat(dirfd, "payload", 0);
	memset(&binding, 0, sizeof(binding));
	check(d2_store_provision(dirfd, &config, &binding, &store) == D1_OK,
	      "provision recovery-rebind race fixture");
	if (store) {
		enum { REBIND_RACE_RUNS = 6 };
		struct d1_envelope transitions[REBIND_RACE_RUNS];
		struct d1_envelope recoveries[REBIND_RACE_RUNS];
		struct d1_owner owners[REBIND_RACE_RUNS];
		d1_admission_id old_admissions[REBIND_RACE_RUNS];
		d1_admission_id new_admissions[REBIND_RACE_RUNS];
		uint64_t txn_ids[REBIND_RACE_RUNS];
		bool rebind_first[REBIND_RACE_RUNS] = { 0 };
		d1_admission_id recovery_actor;
		unsigned int i;
		bool setup_ok = true, races_ok = true, replay_ok = true;

		for (i = 0; i < REBIND_RACE_RUNS; i++) {
			struct d1_result prepared;

			old_admissions[i] =
				d2_store_admit(store, &object, 40 + i,
					       D1_RIGHT_READ | D1_RIGHT_WRITE |
						       D1_RIGHT_SINGLE_WRITER);
			memset(&env, 0, sizeof(env));
			env.object = object;
			env.admission = old_admissions[i];
			env.incarnation = d2_store_incarnation(store);
			fill(env.key.origin.bytes, sizeof(env.key.origin.bytes),
			     (uint8_t)(0x20 + i));
			env.op = D1_OP_WRITE_BATCH;
			guard = (struct d1_guard){ .never_written = true };
			write_request(&env, 1, 48 + i, 200 + i, &guard, payload,
				      sizeof(payload));
			env.body.write.activate = false;
			env.body.write.entries[0].owner.writer = 40 + i;
			setup_ok = setup_ok &&
				   d1_admission_live(old_admissions[i]) &&
				   d2_store_apply(store, &env, &prepared) ==
					   D1_OK &&
				   prepared.entries[0].status == D1_OK &&
				   prepared.entries[0].phase == D2_PREPARED;
			if (!setup_ok)
				break;
			txn_ids[i] = prepared.entries[0].txn.raw;
			owners[i] = env.body.write.entries[0].owner;
		}
		check(setup_ok,
		      "prepare transactions for recovery-rebind races");
		if (store && setup_ok) {
			recovery_actor = d2_store_admit(store, &object, 70,
							D1_RIGHT_CONTROL);
			for (i = 0; i < REBIND_RACE_RUNS; i++) {
				struct rebind_race race = { .store = store };
				pthread_t transition_thread, recovery_thread;
				uint32_t phase = 0;
				uint64_t admission_id = 0, begin, end;
				int transition_created, recovery_created;
				bool log_ok, run_ok;

				old_admissions[i] = d2_store_admission_handle(
					store, old_admissions[i].raw);
				new_admissions[i] = d2_store_admit(
					store, &object, 40 + i,
					D1_RIGHT_READ | D1_RIGHT_WRITE |
						D1_RIGHT_SINGLE_WRITER);
				memset(&race.transition, 0,
				       sizeof(race.transition));
				race.transition.object = object;
				race.transition.admission = old_admissions[i];
				race.transition.incarnation =
					d2_store_incarnation(store);
				fill(race.transition.key.origin.bytes,
				     sizeof(race.transition.key.origin.bytes),
				     (uint8_t)(0x50 + i));
				race.transition.key.sequence = 1;
				race.transition.op = D1_OP_FINALIZE_BATCH;
				race.transition.body.lifecycle.range_begin =
					48 + i;
				race.transition.body.lifecycle.range_end =
					49 + i;
				race.transition.body.lifecycle.count = 1;
				race.transition.body.lifecycle.entries[0].index =
					48 + i;
				race.transition.body.lifecycle.entries[0].owner =
					owners[i];
				race.transition.body.lifecycle.entries[0].txn =
					d2_store_txn_handle(store, txn_ids[i]);
				d2_store_verifier(store,
						  race.transition.body.lifecycle
							  .prior_verifier);
				memset(&race.recovery, 0,
				       sizeof(race.recovery));
				race.recovery.object = object;
				race.recovery.admission = recovery_actor;
				race.recovery.incarnation =
					d2_store_incarnation(store);
				fill(race.recovery.key.origin.bytes,
				     sizeof(race.recovery.key.origin.bytes),
				     (uint8_t)(0x70 + i));
				race.recovery.key.sequence = 1;
				race.recovery.op = D1_OP_RECOVERY_ADMIT;
				race.recovery.body.control.count = 1;
				race.recovery.body.control.txns[0] =
					d2_store_txn_handle(store, txn_ids[i]);
				race.recovery.body.control.old_admission =
					old_admissions[i];
				race.recovery.body.control
					.new_admission_present = true;
				race.recovery.body.control.new_admission =
					new_admissions[i];
				race.recovery.body.control.read_epoch_present =
					true;
				atomic_init(&race.go, false);
				atomic_init(&race.first_done, false);
				race.forced_order = i == 0 ? 1 : i == 1 ? 2 : 0;
				begin = d2_store_wal_bytes(store);
				transition_created = pthread_create(
					&transition_thread, NULL,
					race_rebind_transition, &race);
				recovery_created =
					transition_created ?
						-1 :
						pthread_create(
							&recovery_thread, NULL,
							race_recovery_admit,
							&race);
				atomic_store_explicit(&race.go, true,
						      memory_order_release);
				if (!transition_created)
					pthread_join(transition_thread, NULL);
				if (!recovery_created)
					pthread_join(recovery_thread, NULL);
				end = d2_store_wal_bytes(store);
				log_ok = rebind_race_log(dirfd, begin, end,
							 &binding, txn_ids[i],
							 &rebind_first[i]);
				run_ok = !transition_created &&
					 !recovery_created &&
					 d1_admission_live(new_admissions[i]) &&
					 race.transition_status == D1_OK &&
					 race.recovery_status == D1_OK &&
					 race.recovery_result.entries[0]
							 .status == D1_OK &&
					 log_ok &&
					 race.transition_result.entries[0]
							 .status ==
						 (rebind_first[i] ?
							  D1_STALE_AUTH :
							  D1_OK) &&
					 race.transition_result.entries[0]
							 .phase ==
						 (rebind_first[i] ?
							  0 :
							  D2_FINALIZED) &&
					 d2_store_txn_state(store, txn_ids[i],
							    &phase,
							    &admission_id) &&
					 phase == (rebind_first[i] ?
							   D2_PREPARED :
							   D2_FINALIZED) &&
					 admission_id == new_admissions[i].raw;
				races_ok = races_ok && run_ok;
				transitions[i] = race.transition;
				recoveries[i] = race.recovery;
			}
		}
		check(store && races_ok,
		      "recovery admission serializes with prepared finalize");
		if (store) {
			memcpy(reopen.files.expected_store_uuid,
			       binding.store_uuid, 16);
			memcpy(reopen.files.expected_export_uuid,
			       binding.export_uuid, 16);
			reopen.files.expected_root_ino = binding.root_ino;
			d2_store_crash(store);
			store = NULL;
		}
		check(races_ok && d2_store_rebind(dirfd, &reopen, &binding,
						  &store) == D1_OK,
		      "recovery-rebind race log replays");
		if (store && races_ok) {
			wal_bytes = d2_store_wal_bytes(store);
			for (i = 0; i < REBIND_RACE_RUNS; i++) {
				uint32_t phase;
				uint64_t admission_id;

				transitions[i].admission =
					d2_store_admission_handle(
						store,
						transitions[i].admission.raw);
				recoveries[i].admission =
					d2_store_admission_handle(
						store,
						recoveries[i].admission.raw);
				recoveries[i].body.control.old_admission =
					d2_store_admission_handle(
						store,
						recoveries[i]
							.body.control
							.old_admission.raw);
				recoveries[i].body.control.new_admission =
					d2_store_admission_handle(
						store,
						recoveries[i]
							.body.control
							.new_admission.raw);
				if (d2_store_apply(store, &transitions[i],
						   &result) != D1_OK ||
				    result.entries[0].status !=
					    (rebind_first[i] ? D1_STALE_AUTH :
							       D1_OK) ||
				    result.entries[0].phase !=
					    (rebind_first[i] ? 0 :
							       D2_FINALIZED) ||
				    d2_store_apply(store, &recoveries[i],
						   &result) != D1_OK ||
				    result.entries[0].status != D1_OK ||
				    !d2_store_txn_state(store, txn_ids[i],
							&phase,
							&admission_id) ||
				    phase != (rebind_first[i] ? D2_PREPARED :
								D2_FINALIZED) ||
				    admission_id != new_admissions[i].raw ||
				    d2_store_wal_bytes(store) != wal_bytes)
					replay_ok = false;
			}
		}
		check(store && replay_ok,
		      "recovery-rebind race receipts replay exactly");
	}
	if (store) {
		check(d2_store_close(store) == D1_OK,
		      "close recovery-rebind race fixture");
		store = NULL;
	}
	unlinkat(dirfd, "super", 0);
	unlinkat(dirfd, "wal", 0);
	unlinkat(dirfd, "payload", 0);
	memset(&binding, 0, sizeof(binding));
	check(d2_store_provision(dirfd, &config, &binding, &store) == D1_OK,
	      "provision lost-revoke headroom fixture");
	if (store) {
		struct d1_fixture_authority mds = { 0 }, client = { 0 };
		d1_admission_id mds_id, client_id;
		d1_txn_id txn;
		uint32_t phase, fill_status = D1_OK;
		uint64_t admission_id;

		fill(mds.issuer.bytes, sizeof(mds.issuer.bytes), 0x25);
		fill(mds.principal.bytes, sizeof(mds.principal.bytes), 0x35);
		fill(mds.session, sizeof(mds.session), 0x45);
		mds.writer = 80;
		mds.rights = D1_RIGHT_CONTROL;
		mds.authority_epoch = 800;
		client = mds;
		fill(client.principal.bytes, sizeof(client.principal.bytes),
		     0x55);
		fill(client.session, sizeof(client.session), 0x65);
		client.writer = 81;
		client.rights = D1_RIGHT_READ | D1_RIGHT_WRITE |
				D1_RIGHT_SINGLE_WRITER;
		client.lease_epoch = 801;
		client.fence_sequence = 802;
		mds_id = d2_store_admit_full(store, &object, &mds);
		client_id = d2_store_admit_bare(store, &object, &client);
		check(d1_admission_live(mds_id) &&
			      d1_admission_live(client_id) &&
			      d2_store_trust_admission(store, mds_id,
						       client_id) == D1_OK &&
			      d2_store_admit_authority(store, mds_id,
						       &client_id, 1) == D1_OK,
		      "admit lost-revoke authority and beneficiary");
		memset(&env, 0, sizeof(env));
		env.object = object;
		env.admission = client_id;
		env.incarnation = d2_store_incarnation(store);
		fill(env.key.origin.bytes, sizeof(env.key.origin.bytes), 0x75);
		env.op = D1_OP_WRITE_BATCH;
		guard = (struct d1_guard){ .never_written = true };
		write_request(&env, 1, 60, 300, &guard, payload,
			      sizeof(payload));
		env.body.write.activate = false;
		env.body.write.entries[0].owner.writer = client.writer;
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_OK &&
			      result.entries[0].phase == D2_PREPARED,
		      "prepare work before lost authority revoke");
		txn = result.entries[0].txn;
		while (fill_status == D1_OK)
			fill_status = d2_store_admit_authority(store, mds_id,
							       NULL, 0);
		check(fill_status == D1_NOSPC,
		      "drain ordinary WAL headroom with valid controls");
		wal_bytes = d2_store_wal_bytes(store);
		check(d2_store_revoke_authority(store, mds_id, &mds.issuer, 800,
						1) == D1_NOSPC &&
			      d2_store_wal_bytes(store) == wal_bytes,
		      "unrecorded authority revoke still publishes restriction");
		memset(&env.body, 0, sizeof(env.body));
		env.key.sequence = 2;
		env.op = D1_OP_FINALIZE_BATCH;
		env.body.lifecycle.range_begin = 60;
		env.body.lifecycle.range_end = 61;
		env.body.lifecycle.count = 1;
		env.body.lifecycle.entries[0].index = 60;
		env.body.lifecycle.entries[0].owner.cohort.raw = 1;
		env.body.lifecycle.entries[0].owner.writer = client.writer;
		env.body.lifecycle.entries[0].owner.co_id = 300;
		env.body.lifecycle.entries[0].txn = txn;
		d2_store_verifier(store, env.body.lifecycle.prior_verifier);
		check(d2_store_apply(store, &env, &result) == D1_OK &&
			      result.entries[0].status == D1_STALE_AUTH &&
			      result.disposition == D1_UNRECORDED &&
			      result.entries[0].disposition == D1_UNRECORDED &&
			      d2_store_wal_bytes(store) == wal_bytes &&
			      d2_store_txn_state(store, txn.raw, &phase,
						 &admission_id) &&
			      phase == D2_PREPARED &&
			      admission_id == client_id.raw,
		      "forward-funded stale finalize leaves prepared work");
		memcpy(reopen.files.expected_store_uuid, binding.store_uuid,
		       16);
		memcpy(reopen.files.expected_export_uuid, binding.export_uuid,
		       16);
		reopen.files.expected_root_ino = binding.root_ino;
		d2_store_crash(store);
		store = NULL;
		check(d2_store_rebind(dirfd, &reopen, &binding, &store) ==
				      D1_OK &&
			      d2_store_txn_state(store, txn.raw, &phase,
						 &admission_id) &&
			      phase == D2_PREPARED &&
			      admission_id == client_id.raw,
		      "lost revoke restarts with prepared work fenced by incarnation");
	}
	if (store) {
		check(d2_store_close(store) == D1_OK,
		      "close lost-revoke headroom fixture");
		store = NULL;
	}
	unlinkat(dirfd, "super", 0);
	unlinkat(dirfd, "wal", 0);
	unlinkat(dirfd, "payload", 0);
	memset(&binding, 0, sizeof(binding));
	check(d2_store_provision(dirfd, &config, &binding, &store) == D1_OK,
	      "provision repair-revoke race fixture");
	if (store) {
		enum { REPAIR_REVOKE_RUNS = 4 };
		struct d1_envelope retries[REPAIR_REVOKE_RUNS];
		d1_repair_id cohorts[REPAIR_REVOKE_RUNS];
		bool revoke_first[REPAIR_REVOKE_RUNS] = { 0 };
		uint32_t expected_phase[REPAIR_REVOKE_RUNS] = { 0 };
		unsigned int i;
		bool races_ok = true, replay_ok = true;

		for (i = 0; i < REPAIR_REVOKE_RUNS; i++) {
			struct d1_fixture_authority mds = { 0 }, client = { 0 };
			struct authority_race race = { .store = store };
			d1_admission_id mds_id, client_id;
			pthread_t prepare_thread, revoke_thread;
			uint32_t phase, members;
			uint64_t begin, end;
			int prepare_created, revoke_created;
			bool run_ok;

			fill(mds.issuer.bytes, sizeof(mds.issuer.bytes),
			     (uint8_t)(0x90 + i));
			fill(mds.principal.bytes, sizeof(mds.principal.bytes),
			     (uint8_t)(0x30 + i));
			fill(mds.session, sizeof(mds.session),
			     (uint8_t)(0x50 + i));
			mds.writer = 90 + i;
			mds.rights = D1_RIGHT_CONTROL;
			mds.authority_epoch = 900 + i;
			client = mds;
			fill(client.principal.bytes,
			     sizeof(client.principal.bytes),
			     (uint8_t)(0x60 + i));
			fill(client.session, sizeof(client.session),
			     (uint8_t)(0x70 + i));
			client.writer = 100 + i;
			client.rights = D1_RIGHT_READ | D1_RIGHT_WRITE |
					D1_RIGHT_REPAIR |
					D1_RIGHT_SINGLE_WRITER;
			client.lease_epoch = 910 + i;
			client.fence_sequence = 920 + i;
			mds_id = d2_store_admit_full(store, &object, &mds);
			client_id =
				d2_store_admit_bare(store, &object, &client);
			if (!d1_admission_live(mds_id) ||
			    !d1_admission_live(client_id) ||
			    d2_store_trust_admission(store, mds_id,
						     client_id) != D1_OK ||
			    d2_store_admit_authority(store, mds_id, &client_id,
						     1) != D1_OK ||
			    !admitted_error_repair(
				    store, &object, client_id, client.writer, i,
				    (uint8_t)(0xa0 + i), payload,
				    sizeof(payload), &race.transition,
				    &cohorts[i], NULL, NULL, NULL)) {
				races_ok = false;
				break;
			}
			race.actor = mds_id;
			race.issuer = mds.issuer;
			race.epoch = mds.authority_epoch;
			atomic_init(&race.go, false);
			atomic_init(&race.first_done, false);
			race.forced_order = i == 0 ? 1 : i == 1 ? 2 : 0;
			begin = d2_store_wal_bytes(store);
			prepare_created = pthread_create(
				&prepare_thread, NULL, race_transition, &race);
			revoke_created =
				prepare_created ?
					-1 :
					pthread_create(&revoke_thread, NULL,
						       race_revoke, &race);
			atomic_store_explicit(&race.go, true,
					      memory_order_release);
			if (!prepare_created)
				pthread_join(prepare_thread, NULL);
			if (!revoke_created)
				pthread_join(revoke_thread, NULL);
			end = d2_store_wal_bytes(store);
			expected_phase[i] =
				race.transition_result.entries[0].phase;
			run_ok = !prepare_created && !revoke_created &&
				 race.transition_status == D1_OK &&
				 race.revoke_status == D1_OK &&
				 repair_revoke_race_log(dirfd, begin, end,
							&binding,
							cohorts[i].raw,
							&revoke_first[i]) &&
				 race.transition_result.entries[0].status ==
					 (revoke_first[i] ? D1_STALE_AUTH :
							    D1_OK) &&
				 d2_store_repair_state(store, cohorts[i].raw,
						       &phase, &members) &&
				 phase == (revoke_first[i] ? D2_ADMITTED :
							     D2_PREPARED) &&
				 members == 1;
			races_ok = races_ok && run_ok;
			retries[i] = race.transition;
		}
		check(races_ok,
		      "authority revoke serializes with admitted repair prepare");
		memcpy(reopen.files.expected_store_uuid, binding.store_uuid,
		       16);
		memcpy(reopen.files.expected_export_uuid, binding.export_uuid,
		       16);
		reopen.files.expected_root_ino = binding.root_ino;
		d2_store_crash(store);
		store = NULL;
		check(races_ok && d2_store_rebind(dirfd, &reopen, &binding,
						  &store) == D1_OK,
		      "repair-revoke race log replays");
		if (store && races_ok) {
			wal_bytes = d2_store_wal_bytes(store);
			for (i = 0; i < REPAIR_REVOKE_RUNS; i++) {
				uint32_t phase = 0, members = 0, retry_status;

				retries[i].admission = d2_store_admission_handle(
					store, retries[i].admission.raw);
				retries[i].body.repair.cohort =
					d2_store_repair_handle(
						store,
						retries[i]
							.body.repair.cohort.raw);
				retries[i].body.repair.entries[0].txn =
					d2_store_txn_handle(store,
							    retries[i]
								    .body.repair
								    .entries[0]
								    .txn.raw);
				retry_status = d2_store_apply(
					store, &retries[i], &result);
				if (retry_status != D1_OK ||
				    result.entries[0].status !=
					    (revoke_first[i] ? D1_STALE_AUTH :
							       D1_OK) ||
				    result.entries[0].phase !=
					    expected_phase[i] ||
				    !d2_store_repair_state(store,
							   cohorts[i].raw,
							   &phase, &members) ||
				    phase != (revoke_first[i] ? D2_ADMITTED :
								D2_PREPARED) ||
				    members != 1 ||
				    d2_store_wal_bytes(store) != wal_bytes)
					replay_ok = false;
			}
		}
		check(store && replay_ok,
		      "repair-revoke race receipts replay exactly");
	}
	if (store) {
		check(d2_store_close(store) == D1_OK,
		      "close repair-revoke race fixture");
		store = NULL;
	}
	unlinkat(dirfd, "super", 0);
	unlinkat(dirfd, "wal", 0);
	unlinkat(dirfd, "payload", 0);
	memset(&binding, 0, sizeof(binding));
	check(d2_store_provision(dirfd, &config, &binding, &store) == D1_OK,
	      "provision failed-prepare revoke race fixture");
	if (store) {
		enum { FAILURE_REVOKE_RUNS = 4 };
		struct d1_envelope retries[FAILURE_REVOKE_RUNS];
		d1_repair_id cohorts[FAILURE_REVOKE_RUNS];
		bool revoke_first[FAILURE_REVOKE_RUNS] = { 0 };
		unsigned int i;
		bool races_ok = true, replay_ok = true;

		for (i = 0; i < FAILURE_REVOKE_RUNS; i++) {
			struct d1_fixture_authority mds = { 0 }, client = { 0 };
			struct authority_race race = { .store = store };
			d1_admission_id mds_id, client_id;
			pthread_t prepare_thread, revoke_thread;
			uint32_t phase, members;
			uint64_t begin, end;
			int prepare_created, revoke_created;
			bool run_ok;

			fill(mds.issuer.bytes, sizeof(mds.issuer.bytes),
			     (uint8_t)(0xb0 + i));
			fill(mds.principal.bytes, sizeof(mds.principal.bytes),
			     (uint8_t)(0x40 + i));
			fill(mds.session, sizeof(mds.session),
			     (uint8_t)(0x50 + i));
			mds.writer = 140 + i;
			mds.rights = D1_RIGHT_CONTROL;
			mds.authority_epoch = 1000 + i;
			client = mds;
			fill(client.principal.bytes,
			     sizeof(client.principal.bytes),
			     (uint8_t)(0x60 + i));
			fill(client.session, sizeof(client.session),
			     (uint8_t)(0x70 + i));
			client.writer = 150 + i;
			client.rights = D1_RIGHT_READ | D1_RIGHT_WRITE |
					D1_RIGHT_REPAIR |
					D1_RIGHT_SINGLE_WRITER;
			client.lease_epoch = 1010 + i;
			client.fence_sequence = 1020 + i;
			mds_id = d2_store_admit_full(store, &object, &mds);
			client_id =
				d2_store_admit_bare(store, &object, &client);
			if (!d1_admission_live(mds_id) ||
			    !d1_admission_live(client_id) ||
			    d2_store_trust_admission(store, mds_id,
						     client_id) != D1_OK ||
			    d2_store_admit_authority(store, mds_id, &client_id,
						     1) != D1_OK ||
			    !admitted_error_repair(
				    store, &object, client_id, client.writer,
				    16 + i, (uint8_t)(0xc0 + i), payload,
				    sizeof(payload), &race.transition,
				    &cohorts[i], NULL, NULL, NULL)) {
				races_ok = false;
				break;
			}
			race.transition.body.repair.entries[0]
				.checksum.digest[0] ^= 0xff;
			race.actor = mds_id;
			race.issuer = mds.issuer;
			race.epoch = mds.authority_epoch;
			atomic_init(&race.go, false);
			atomic_init(&race.first_done, false);
			race.forced_order = i == 0 ? 1 : i == 1 ? 2 : 0;
			begin = d2_store_wal_bytes(store);
			prepare_created = pthread_create(
				&prepare_thread, NULL, race_transition, &race);
			revoke_created =
				prepare_created ?
					-1 :
					pthread_create(&revoke_thread, NULL,
						       race_revoke, &race);
			atomic_store_explicit(&race.go, true,
					      memory_order_release);
			if (!prepare_created)
				pthread_join(prepare_thread, NULL);
			if (!revoke_created)
				pthread_join(revoke_thread, NULL);
			end = d2_store_wal_bytes(store);
			run_ok = !prepare_created && !revoke_created &&
				 race.transition_status == D1_OK &&
				 race.revoke_status == D1_OK &&
				 repair_failure_revoke_race_log(
					 dirfd, begin, end, &binding,
					 cohorts[i].raw, &revoke_first[i]) &&
				 race.transition_result.entries[0].status ==
					 (revoke_first[i] ? D1_STALE_AUTH :
							    D1_CHECKSUM) &&
				 race.transition_result.entries[0].phase ==
					 (revoke_first[i] ? 0 : D2_ABORTED) &&
				 d2_store_repair_state(store, cohorts[i].raw,
						       &phase, &members) &&
				 phase == (revoke_first[i] ? D2_ADMITTED :
							     D2_ABORTED) &&
				 members == 1;
			races_ok = races_ok && run_ok;
			retries[i] = race.transition;
		}
		check(races_ok,
		      "kept prepare failure serializes with authority revoke");
		memcpy(reopen.files.expected_store_uuid, binding.store_uuid,
		       16);
		memcpy(reopen.files.expected_export_uuid, binding.export_uuid,
		       16);
		reopen.files.expected_root_ino = binding.root_ino;
		d2_store_crash(store);
		store = NULL;
		check(races_ok && d2_store_rebind(dirfd, &reopen, &binding,
						  &store) == D1_OK,
		      "failed-prepare revoke race log replays");
		if (store && races_ok) {
			wal_bytes = d2_store_wal_bytes(store);
			for (i = 0; i < FAILURE_REVOKE_RUNS; i++) {
				uint32_t phase = 0, members = 0;

				retries[i].admission = d2_store_admission_handle(
					store, retries[i].admission.raw);
				retries[i].body.repair.cohort =
					d2_store_repair_handle(
						store,
						retries[i]
							.body.repair.cohort.raw);
				retries[i].body.repair.entries[0].txn =
					d2_store_txn_handle(store,
							    retries[i]
								    .body.repair
								    .entries[0]
								    .txn.raw);
				if (d2_store_apply(store, &retries[i],
						   &result) != D1_OK ||
				    result.entries[0].status !=
					    (revoke_first[i] ? D1_STALE_AUTH :
							       D1_CHECKSUM) ||
				    result.entries[0].phase !=
					    (revoke_first[i] ? 0 :
							       D2_ABORTED) ||
				    !d2_store_repair_state(store,
							   cohorts[i].raw,
							   &phase, &members) ||
				    phase != (revoke_first[i] ? D2_ADMITTED :
								D2_ABORTED) ||
				    members != 1 ||
				    d2_store_wal_bytes(store) != wal_bytes)
					replay_ok = false;
			}
		}
		check(store && replay_ok,
		      "failed-prepare revoke receipts replay exactly");
	}
	if (store) {
		check(d2_store_close(store) == D1_OK,
		      "close failed-prepare revoke race fixture");
		store = NULL;
	}
	unlinkat(dirfd, "super", 0);
	unlinkat(dirfd, "wal", 0);
	unlinkat(dirfd, "payload", 0);
	memset(&binding, 0, sizeof(binding));
	check(d2_store_provision(dirfd, &config, &binding, &store) == D1_OK,
	      "provision postcondition revoke race fixture");
	if (store) {
		enum { POSTCOND_REVOKE_RUNS = 4 };
		struct d1_envelope retries[POSTCOND_REVOKE_RUNS];
		d1_version_id successors[POSTCOND_REVOKE_RUNS];
		uint64_t postconds[POSTCOND_REVOKE_RUNS] = { 0 };
		bool revoke_first[POSTCOND_REVOKE_RUNS] = { 0 };
		unsigned int i;
		bool races_ok = true, replay_ok = true;

		for (i = 0; i < POSTCOND_REVOKE_RUNS; i++) {
			struct d1_fixture_authority mds = { 0 }, client = { 0 };
			struct authority_race race = { .store = store };
			struct d1_result written;
			d1_admission_id mds_id, client_id;
			d1_custody_id issued;
			pthread_t rollback_thread, revoke_thread;
			uint64_t begin, end;
			int rollback_created, revoke_created;
			bool run_ok;

			fill(mds.issuer.bytes, sizeof(mds.issuer.bytes),
			     (uint8_t)(0xd0 + i));
			fill(mds.principal.bytes, sizeof(mds.principal.bytes),
			     (uint8_t)(0x20 + i));
			fill(mds.session, sizeof(mds.session),
			     (uint8_t)(0x30 + i));
			mds.writer = 160 + i;
			mds.rights = D1_RIGHT_CONTROL;
			mds.authority_epoch = 1100 + i;
			client = mds;
			fill(client.principal.bytes,
			     sizeof(client.principal.bytes),
			     (uint8_t)(0x40 + i));
			fill(client.session, sizeof(client.session),
			     (uint8_t)(0x50 + i));
			client.writer = 170 + i;
			client.rights = D1_RIGHT_READ | D1_RIGHT_WRITE |
					D1_RIGHT_REPAIR |
					D1_RIGHT_SINGLE_WRITER;
			client.lease_epoch = 1110 + i;
			client.fence_sequence = 1120 + i;
			mds_id = d2_store_admit_full(store, &object, &mds);
			client_id =
				d2_store_admit_bare(store, &object, &client);
			if (!d1_admission_live(mds_id) ||
			    !d1_admission_live(client_id) ||
			    d2_store_trust_admission(store, mds_id,
						     client_id) != D1_OK ||
			    d2_store_admit_authority(store, mds_id, &client_id,
						     1) != D1_OK) {
				races_ok = false;
				break;
			}
			memset(&env, 0, sizeof(env));
			env.object = object;
			env.admission = client_id;
			env.incarnation = d2_store_incarnation(store);
			fill(env.key.origin.bytes, sizeof(env.key.origin.bytes),
			     (uint8_t)(0xe0 + i));
			env.op = D1_OP_WRITE_BATCH;
			guard = (struct d1_guard){ .never_written = true };
			write_request(&env, 1, 24 + i, 700 + i, &guard, payload,
				      sizeof(payload));
			env.body.write.entries[0].owner.writer = client.writer;
			if (d2_store_apply(store, &env, &written) != D1_OK ||
			    written.entries[0].status != D1_OK ||
			    !written.entries[0].version_present) {
				races_ok = false;
				break;
			}
			successors[i] = written.entries[0].version;
			issued = d2_store_custody(store, successors[i]);
			if (!d1_custody_live(issued)) {
				races_ok = false;
				break;
			}
			race.transition = env;
			memset(&race.transition.body, 0,
			       sizeof(race.transition.body));
			race.transition.key.sequence = 2;
			race.transition.op = D1_OP_ROLLBACK_BATCH;
			race.transition.body.rollback.range_begin = 24 + i;
			race.transition.body.rollback.range_end = 25 + i;
			race.transition.body.rollback.count = 1;
			race.transition.body.rollback.entries[0].index = 24 + i;
			race.transition.body.rollback.entries[0].owner =
				env.body.write.entries[0].owner;
			race.transition.body.rollback.entries[0].txn =
				written.entries[0].txn;
			race.transition.body.rollback.entries[0]
				.visible_present = true;
			race.transition.body.rollback.entries[0].visible =
				successors[i];
			race.transition.body.rollback.entries[0]
				.custody_present = true;
			race.transition.body.rollback.entries[0].custody =
				issued;
			race.actor = mds_id;
			race.issuer = mds.issuer;
			race.epoch = mds.authority_epoch;
			atomic_init(&race.go, false);
			atomic_init(&race.first_done, false);
			race.forced_order = i == 0 ? 1 : i == 1 ? 2 : 0;
			begin = d2_store_wal_bytes(store);
			rollback_created = pthread_create(
				&rollback_thread, NULL, race_transition, &race);
			revoke_created =
				rollback_created ?
					-1 :
					pthread_create(&revoke_thread, NULL,
						       race_revoke, &race);
			atomic_store_explicit(&race.go, true,
					      memory_order_release);
			if (!rollback_created)
				pthread_join(rollback_thread, NULL);
			if (!revoke_created)
				pthread_join(revoke_thread, NULL);
			end = d2_store_wal_bytes(store);
			run_ok = !rollback_created && !revoke_created &&
				 race.transition_status == D1_OK &&
				 race.revoke_status == D1_OK &&
				 postcond_revoke_race_log(
					 dirfd, begin, end, &binding,
					 written.entries[0].txn.raw,
					 &revoke_first[i]) &&
				 race.transition_result.entries[0].status ==
					 (revoke_first[i] ?
						  D1_STALE_AUTH :
						  D1_NO_PREDECESSOR) &&
				 race.transition_result.entries[0]
						 .postcond_present ==
					 !revoke_first[i];
			if (!revoke_first[i])
				postconds[i] = race.transition_result.entries[0]
						       .postcond.raw;
			races_ok = races_ok && run_ok;
			retries[i] = race.transition;
		}
		check(races_ok,
		      "postcondition rollback serializes with authority revoke");
		memcpy(reopen.files.expected_store_uuid, binding.store_uuid,
		       16);
		memcpy(reopen.files.expected_export_uuid, binding.export_uuid,
		       16);
		reopen.files.expected_root_ino = binding.root_ino;
		d2_store_crash(store);
		store = NULL;
		check(races_ok && d2_store_rebind(dirfd, &reopen, &binding,
						  &store) == D1_OK,
		      "postcondition revoke race log replays");
		if (store && races_ok) {
			wal_bytes = d2_store_wal_bytes(store);
			for (i = 0; i < POSTCOND_REVOKE_RUNS; i++) {
				d1_version_id restored_version;
				uint64_t restored_index;
				bool consumed;

				retries[i].admission = d2_store_admission_handle(
					store, retries[i].admission.raw);
				retries[i].body.rollback.entries[0].txn =
					d2_store_txn_handle(
						store, retries[i]
							       .body.rollback
							       .entries[0]
							       .txn.raw);
				retries[i].body.rollback.entries[0].visible =
					d2_store_version_handle(
						store, successors[i].raw);
				retries[i].body.rollback.entries[0].custody =
					d2_store_custody_handle(
						store, retries[i]
							       .body.rollback
							       .entries[0]
							       .custody.raw);
				if (d2_store_apply(store, &retries[i],
						   &result) != D1_OK ||
				    result.entries[0].status !=
					    (revoke_first[i] ?
						     D1_STALE_AUTH :
						     D1_NO_PREDECESSOR) ||
				    result.entries[0].postcond_present !=
					    !revoke_first[i] ||
				    (!revoke_first[i] &&
				     (result.entries[0].postcond.raw !=
					      postconds[i] ||
				      !d2_store_postcond(store, postconds[i],
							 &restored_index,
							 &restored_version,
							 &consumed) ||
				      restored_index != 24 + i ||
				      restored_version.raw !=
					      successors[i].raw ||
				      consumed)) ||
				    d2_store_wal_bytes(store) != wal_bytes)
					replay_ok = false;
			}
		}
		check(store && replay_ok,
		      "postcondition revoke receipts replay exactly");
	}
	if (store) {
		check(d2_store_close(store) == D1_OK,
		      "close postcondition revoke race fixture");
		store = NULL;
	}
	unlinkat(dirfd, "super", 0);
	unlinkat(dirfd, "wal", 0);
	unlinkat(dirfd, "payload", 0);
	memset(&binding, 0, sizeof(binding));
	check(d2_store_provision(dirfd, &config, &binding, &store) == D1_OK,
	      "provision repair-rebind abort race fixture");
	if (store) {
		enum { REPAIR_REBIND_RUNS = 4 };
		struct d1_envelope aborts[REPAIR_REBIND_RUNS];
		struct d1_envelope recoveries[REPAIR_REBIND_RUNS];
		d1_repair_id cohorts[REPAIR_REBIND_RUNS];
		d1_admission_id old_admissions[REPAIR_REBIND_RUNS];
		d1_admission_id new_admissions[REPAIR_REBIND_RUNS];
		uint64_t txn_ids[REPAIR_REBIND_RUNS];
		bool rebind_first[REPAIR_REBIND_RUNS] = { 0 };
		d1_admission_id recovery_actor;
		unsigned int i;
		bool races_ok = true, replay_ok = true;

		recovery_actor =
			d2_store_admit(store, &object, 120, D1_RIGHT_CONTROL);
		for (i = 0; i < REPAIR_REBIND_RUNS; i++) {
			struct d1_envelope prepare;
			struct rebind_race race = { .store = store };
			d1_custody_id custody;
			pthread_t abort_thread, recovery_thread;
			uint32_t repair_phase = 0, txn_phase = 0, members = 0;
			uint64_t admission_id = 0, begin, end;
			int abort_created, recovery_created;
			bool log_ok, repair_ok, run_ok, txn_ok;

			old_admissions[i] =
				d2_store_admit(store, &object, 130 + i,
					       D1_RIGHT_READ | D1_RIGHT_WRITE |
						       D1_RIGHT_REPAIR |
						       D1_RIGHT_SINGLE_WRITER);
			if (!d1_admission_live(old_admissions[i]) ||
			    !admitted_error_repair(
				    store, &object, old_admissions[i], 130 + i,
				    8 + i, (uint8_t)(0xb0 + i), payload,
				    sizeof(payload), &prepare, &cohorts[i],
				    &custody, NULL, NULL)) {
				races_ok = false;
				break;
			}
			txn_ids[i] = prepare.body.repair.entries[0].txn.raw;
			new_admissions[i] =
				d2_store_admit(store, &object, 130 + i,
					       D1_RIGHT_READ | D1_RIGHT_WRITE |
						       D1_RIGHT_REPAIR |
						       D1_RIGHT_SINGLE_WRITER);
			memset(&race.transition, 0, sizeof(race.transition));
			race.transition.object = object;
			race.transition.admission = old_admissions[i];
			race.transition.incarnation =
				d2_store_incarnation(store);
			fill(race.transition.key.origin.bytes,
			     sizeof(race.transition.key.origin.bytes),
			     (uint8_t)(0xd0 + i));
			race.transition.key.sequence = 1;
			race.transition.op = D1_OP_ABORT_REPAIR;
			race.transition.body.repair.range_begin = 8 + i;
			race.transition.body.repair.range_end = 9 + i;
			race.transition.body.repair.count = 1;
			race.transition.body.repair.cohort_present = true;
			race.transition.body.repair.cohort = cohorts[i];
			race.transition.body.repair.phase_present = true;
			race.transition.body.repair.phase = D2_ADMITTED;
			race.transition.body.repair.entries[0].index = 8 + i;
			race.transition.body.repair.entries[0].owner =
				prepare.body.repair.entries[0].owner;
			race.transition.body.repair.entries[0].custody_present =
				true;
			race.transition.body.repair.entries[0].custody =
				custody;
			memset(&race.recovery, 0, sizeof(race.recovery));
			race.recovery.object = object;
			race.recovery.admission = recovery_actor;
			race.recovery.incarnation = d2_store_incarnation(store);
			fill(race.recovery.key.origin.bytes,
			     sizeof(race.recovery.key.origin.bytes),
			     (uint8_t)(0xe0 + i));
			race.recovery.key.sequence = 1;
			race.recovery.op = D1_OP_RECOVERY_ADMIT;
			race.recovery.body.control.count = 1;
			race.recovery.body.control.txns[0] =
				d2_store_txn_handle(store, txn_ids[i]);
			race.recovery.body.control.old_admission =
				old_admissions[i];
			race.recovery.body.control.new_admission_present = true;
			race.recovery.body.control.new_admission =
				new_admissions[i];
			race.recovery.body.control.read_epoch_present = true;
			atomic_init(&race.go, false);
			atomic_init(&race.first_done, false);
			race.forced_order = i == 0 ? 1 : i == 1 ? 2 : 0;
			begin = d2_store_wal_bytes(store);
			abort_created = pthread_create(&abort_thread, NULL,
						       race_rebind_transition,
						       &race);
			recovery_created =
				abort_created ?
					-1 :
					pthread_create(&recovery_thread, NULL,
						       race_recovery_admit,
						       &race);
			atomic_store_explicit(&race.go, true,
					      memory_order_release);
			if (!abort_created)
				pthread_join(abort_thread, NULL);
			if (!recovery_created)
				pthread_join(recovery_thread, NULL);
			end = d2_store_wal_bytes(store);
			log_ok = repair_rebind_race_log(
				dirfd, begin, end, &binding, cohorts[i].raw,
				D2_ABORTED, D1_INVALID, &rebind_first[i]);
			repair_ok = d2_store_repair_state(
				store, cohorts[i].raw, &repair_phase, &members);
			txn_ok = d2_store_txn_state(store, txn_ids[i],
						    &txn_phase, &admission_id);
			run_ok = !abort_created && !recovery_created &&
				 race.transition_status == D1_OK &&
				 race.recovery_status == D1_OK && log_ok &&
				 race.transition_result.entries[0].status ==
					 (rebind_first[i] ? D1_STALE_AUTH :
							    D1_OK) &&
				 race.recovery_result.entries[0].status ==
					 (rebind_first[i] ? D1_OK :
							    D1_INVALID) &&
				 repair_ok &&
				 repair_phase == (rebind_first[i] ?
							  D2_ADMITTED :
							  D2_ABORTED) &&
				 members == 1 &&
				 (!rebind_first[i] ||
				  (txn_ok && txn_phase == D2_ADMITTED &&
				   admission_id == new_admissions[i].raw));
			races_ok = races_ok && run_ok;
			aborts[i] = race.transition;
			recoveries[i] = race.recovery;
		}
		check(races_ok,
		      "repair abort serializes with recovery admission");
		memcpy(reopen.files.expected_store_uuid, binding.store_uuid,
		       16);
		memcpy(reopen.files.expected_export_uuid, binding.export_uuid,
		       16);
		reopen.files.expected_root_ino = binding.root_ino;
		d2_store_crash(store);
		store = NULL;
		check(races_ok && d2_store_rebind(dirfd, &reopen, &binding,
						  &store) == D1_OK,
		      "repair abort-rebind race log replays");
		if (store && races_ok) {
			wal_bytes = d2_store_wal_bytes(store);
			for (i = 0; i < REPAIR_REBIND_RUNS; i++) {
				uint32_t phase, members;
				uint64_t admission_id;

				aborts[i].admission = d2_store_admission_handle(
					store, aborts[i].admission.raw);
				aborts[i].body.repair.cohort =
					d2_store_repair_handle(
						store,
						aborts[i].body.repair.cohort.raw);
				aborts[i].body.repair.entries[0].custody =
					d2_store_custody_handle(
						store,
						aborts[i]
							.body.repair.entries[0]
							.custody.raw);
				recoveries[i].admission =
					d2_store_admission_handle(
						store,
						recoveries[i].admission.raw);
				recoveries[i]
					.body.control
					.txns[0] = d2_store_txn_handle(
					store,
					recoveries[i].body.control.txns[0].raw);
				recoveries[i].body.control.old_admission =
					d2_store_admission_handle(
						store,
						recoveries[i]
							.body.control
							.old_admission.raw);
				recoveries[i].body.control.new_admission =
					d2_store_admission_handle(
						store,
						recoveries[i]
							.body.control
							.new_admission.raw);
				if (d2_store_apply(store, &aborts[i],
						   &result) != D1_OK ||
				    result.entries[0].status !=
					    (rebind_first[i] ? D1_STALE_AUTH :
							       D1_OK) ||
				    d2_store_apply(store, &recoveries[i],
						   &result) != D1_OK ||
				    result.entries[0].status !=
					    (rebind_first[i] ? D1_OK :
							       D1_INVALID) ||
				    !d2_store_repair_state(store,
							   cohorts[i].raw,
							   &phase, &members) ||
				    phase != (rebind_first[i] ? D2_ADMITTED :
								D2_ABORTED) ||
				    members != 1 ||
				    (rebind_first[i] &&
				     (!d2_store_txn_state(store, txn_ids[i],
							  &phase,
							  &admission_id) ||
				      phase != D2_ADMITTED ||
				      admission_id != new_admissions[i].raw)) ||
				    d2_store_wal_bytes(store) != wal_bytes)
					replay_ok = false;
			}
		}
		check(store && replay_ok,
		      "repair abort-rebind race receipts replay exactly");
	}
	if (store) {
		check(d2_store_close(store) == D1_OK,
		      "close repair abort-rebind race fixture");
		store = NULL;
	}
	unlinkat(dirfd, "super", 0);
	unlinkat(dirfd, "wal", 0);
	unlinkat(dirfd, "payload", 0);
	memset(&binding, 0, sizeof(binding));
	check(d2_store_provision(dirfd, &config, &binding, &store) == D1_OK,
	      "provision repair unlock-rebind race fixture");
	if (store) {
		enum { UNLOCK_REBIND_RUNS = 4 };
		struct d1_envelope unlocks[UNLOCK_REBIND_RUNS];
		struct d1_envelope recoveries[UNLOCK_REBIND_RUNS];
		d1_repair_id cohorts[UNLOCK_REBIND_RUNS];
		d1_admission_id new_admissions[UNLOCK_REBIND_RUNS];
		uint64_t txn_ids[UNLOCK_REBIND_RUNS];
		bool rebind_first[UNLOCK_REBIND_RUNS] = { 0 };
		d1_admission_id recovery_actor;
		unsigned int i;
		bool races_ok = true, replay_ok = true;

		recovery_actor =
			d2_store_admit(store, &object, 180, D1_RIGHT_CONTROL);
		for (i = 0; i < UNLOCK_REBIND_RUNS; i++) {
			struct rebind_race race = { .store = store };
			d1_admission_id old_admission;
			d1_txn_id txn;
			pthread_t unlock_thread, recovery_thread;
			uint32_t repair_phase = 0, txn_phase = 0, members = 0;
			uint64_t admission_id = 0, begin, end;
			int unlock_created, recovery_created;
			bool log_ok, repair_ok, run_ok, txn_ok;

			old_admission =
				d2_store_admit(store, &object, 190 + i,
					       D1_RIGHT_READ | D1_RIGHT_WRITE |
						       D1_RIGHT_REPAIR |
						       D1_RIGHT_SINGLE_WRITER);
			if (!d1_admission_live(old_admission) ||
			    !committed_error_repair(
				    store, &object, recovery_actor,
				    old_admission, 190 + i, 32 + i,
				    (uint8_t)(0x90 + i), payload,
				    sizeof(payload), &race.transition,
				    &cohorts[i], &txn)) {
				races_ok = false;
				break;
			}
			txn_ids[i] = txn.raw;
			new_admissions[i] =
				d2_store_admit(store, &object, 190 + i,
					       D1_RIGHT_READ | D1_RIGHT_WRITE |
						       D1_RIGHT_REPAIR |
						       D1_RIGHT_SINGLE_WRITER);
			memset(&race.recovery, 0, sizeof(race.recovery));
			race.recovery.object = object;
			race.recovery.admission = recovery_actor;
			race.recovery.incarnation = d2_store_incarnation(store);
			fill(race.recovery.key.origin.bytes,
			     sizeof(race.recovery.key.origin.bytes),
			     (uint8_t)(0xa0 + i));
			race.recovery.key.sequence = 1;
			race.recovery.op = D1_OP_RECOVERY_ADMIT;
			race.recovery.body.control.count = 1;
			race.recovery.body.control.txns[0] = txn;
			race.recovery.body.control.old_admission =
				old_admission;
			race.recovery.body.control.new_admission_present = true;
			race.recovery.body.control.new_admission =
				new_admissions[i];
			race.recovery.body.control.read_epoch_present = true;
			atomic_init(&race.go, false);
			atomic_init(&race.first_done, false);
			race.forced_order = i == 0 ? 1 : i == 1 ? 2 : 0;
			begin = d2_store_wal_bytes(store);
			unlock_created = pthread_create(&unlock_thread, NULL,
							race_rebind_transition,
							&race);
			recovery_created =
				unlock_created ?
					-1 :
					pthread_create(&recovery_thread, NULL,
						       race_recovery_admit,
						       &race);
			atomic_store_explicit(&race.go, true,
					      memory_order_release);
			if (!unlock_created)
				pthread_join(unlock_thread, NULL);
			if (!recovery_created)
				pthread_join(recovery_thread, NULL);
			end = d2_store_wal_bytes(store);
			log_ok = repair_rebind_race_log(
				dirfd, begin, end, &binding, cohorts[i].raw,
				D2_UNLOCKED, D1_BAD_PHASE, &rebind_first[i]);
			repair_ok = d2_store_repair_state(
				store, cohorts[i].raw, &repair_phase, &members);
			txn_ok = d2_store_txn_state(store, txn_ids[i],
						    &txn_phase, &admission_id);
			run_ok =
				!unlock_created && !recovery_created &&
				race.transition_status == D1_OK &&
				race.recovery_status == D1_OK && log_ok &&
				race.transition_result.entries[0].status ==
					(rebind_first[i] ? D1_STALE_AUTH :
							   D1_OK) &&
				race.recovery_result.entries[0].status ==
					(rebind_first[i] ? D1_OK :
							   D1_BAD_PHASE) &&
				repair_ok && repair_phase == D2_COMMITTED &&
				members == 1 &&
				(rebind_first[i] ?
					 txn_ok && txn_phase == D2_COMMITTED &&
						 admission_id ==
							 new_admissions[i].raw :
					 !txn_ok);
			races_ok = races_ok && run_ok;
			unlocks[i] = race.transition;
			recoveries[i] = race.recovery;
		}
		check(races_ok,
		      "repair unlock serializes with recovery admission");
		memcpy(reopen.files.expected_store_uuid, binding.store_uuid,
		       16);
		memcpy(reopen.files.expected_export_uuid, binding.export_uuid,
		       16);
		reopen.files.expected_root_ino = binding.root_ino;
		d2_store_crash(store);
		store = NULL;
		check(races_ok && d2_store_rebind(dirfd, &reopen, &binding,
						  &store) == D1_OK,
		      "repair unlock-rebind race log replays");
		if (store && races_ok) {
			wal_bytes = d2_store_wal_bytes(store);
			for (i = 0; i < UNLOCK_REBIND_RUNS; i++) {
				uint32_t phase, members;
				uint64_t admission_id;

				unlocks[i].admission = d2_store_admission_handle(
					store, unlocks[i].admission.raw);
				unlocks[i].body.repair.cohort =
					d2_store_repair_handle(
						store,
						unlocks[i]
							.body.repair.cohort.raw);
				unlocks[i].body.repair.entries[0].custody =
					d2_store_custody_handle(
						store,
						unlocks[i]
							.body.repair.entries[0]
							.custody.raw);
				recoveries[i].admission =
					d2_store_admission_handle(
						store,
						recoveries[i].admission.raw);
				recoveries[i]
					.body.control
					.txns[0] = d2_store_txn_handle(
					store,
					recoveries[i].body.control.txns[0].raw);
				recoveries[i].body.control.old_admission =
					d2_store_admission_handle(
						store,
						recoveries[i]
							.body.control
							.old_admission.raw);
				recoveries[i].body.control.new_admission =
					d2_store_admission_handle(
						store,
						recoveries[i]
							.body.control
							.new_admission.raw);
				if (d2_store_apply(store, &unlocks[i],
						   &result) != D1_OK ||
				    result.entries[0].status !=
					    (rebind_first[i] ? D1_STALE_AUTH :
							       D1_OK) ||
				    d2_store_apply(store, &recoveries[i],
						   &result) != D1_OK ||
				    result.entries[0].status !=
					    (rebind_first[i] ? D1_OK :
							       D1_BAD_PHASE) ||
				    !d2_store_repair_state(store,
							   cohorts[i].raw,
							   &phase, &members) ||
				    phase != D2_COMMITTED || members != 1 ||
				    (rebind_first[i] ?
					     (!d2_store_txn_state(
						      store, txn_ids[i], &phase,
						      &admission_id) ||
					      phase != D2_COMMITTED ||
					      admission_id !=
						      new_admissions[i].raw) :
					     d2_store_txn_state(
						     store, txn_ids[i], &phase,
						     &admission_id)) ||
				    d2_store_wal_bytes(store) != wal_bytes)
					replay_ok = false;
			}
		}
		check(store && replay_ok,
		      "repair unlock-rebind race receipts replay exactly");
	}
	if (store) {
		d2_store_close(store);
		store = NULL;
	}
	unlinkat(dirfd, "super", 0);
	unlinkat(dirfd, "wal", 0);
	unlinkat(dirfd, "payload", 0);
	memset(&binding, 0, sizeof(binding));
	config.files.capacity_wal_bytes = 8u * 1024u * 1024u;
	config.max_file_bytes = 128u * sizeof(payload);
	reopen.max_file_bytes = config.max_file_bytes;
	check(d2_store_provision(dirfd, &config, &binding, &store) == D1_OK,
	      "provision live-transaction bound fixture");
	if (store) {
		d1_txn_id survivors[D2_MAX_LIVE_TXNS];
		bool admitted_all = true, replayed_all = true;
		unsigned int i;

		admission = d2_store_admit(store, &object, 17,
					   D1_RIGHT_READ | D1_RIGHT_WRITE |
						   D1_RIGHT_SINGLE_WRITER);
		check(d1_admission_live(admission),
		      "live-transaction bound admission is issued");
		memset(&env, 0, sizeof(env));
		env.object = object;
		env.admission = admission;
		env.incarnation = d2_store_incarnation(store);
		fill(env.key.origin.bytes, D1_UUID_BYTES, 0xdc);
		env.op = D1_OP_WRITE_BATCH;
		guard = (struct d1_guard){ .never_written = true };
		for (i = 0; i < D2_MAX_LIVE_TXNS; i++) {
			uint32_t apply_status;

			write_request(&env, i + 1, i, 700 + i, &guard, payload,
				      sizeof(payload));
			env.body.write.activate = false;
			env.body.write.entries[0].owner.writer = 17;
			apply_status = d2_store_apply(store, &env, &result);
			if (apply_status != D1_OK ||
			    result.entries[0].status != D1_OK ||
			    result.entries[0].phase != D2_PREPARED) {
				admitted_all = false;
				break;
			}
			survivors[i] = result.entries[0].txn;
		}
		write_request(&env, D2_MAX_LIVE_TXNS + 1, D2_MAX_LIVE_TXNS, 800,
			      &guard, replacement, sizeof(replacement));
		env.body.write.activate = false;
		env.body.write.entries[0].owner.writer = 17;
		{
			uint32_t bound_status =
				d2_store_apply(store, &env, &result);
			check(admitted_all && bound_status == D1_NOSPC &&
				      result.disposition == D1_UNRECORDED,
			      "sixty-fifth live transaction is refused at admission");
		}
		memcpy(reopen.files.expected_store_uuid, binding.store_uuid,
		       16);
		memcpy(reopen.files.expected_export_uuid, binding.export_uuid,
		       16);
		reopen.files.expected_root_ino = binding.root_ino;
		d2_store_crash(store);
		store = NULL;
		check(d2_store_rebind(dirfd, &reopen, &binding, &store) ==
				      D1_OK &&
			      d2_store_recovery_allowance(store) ==
				      D2_RESTART_SWEEP,
		      "restart sweep funds sixty-four surviving transactions");
		for (i = 0; store && i < D2_MAX_LIVE_TXNS; i++) {
			uint32_t phase;
			uint64_t admission_id;

			if (!d2_store_txn_state(store, survivors[i].raw, &phase,
						&admission_id) ||
			    phase != D2_PREPARED) {
				replayed_all = false;
			}
		}
		check(store && replayed_all,
		      "all sixty-four funded transactions survive restart");
	}

done:
	if (store)
		d2_store_close(store);
	close(dirfd);
	printf("D2 STORE: %u checks, %u failures\n", checks, failures);
	return failures != 0;
}
