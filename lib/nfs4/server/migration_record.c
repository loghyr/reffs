/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * In-flight proxy migration record table -- slice 6c-x.2.
 *
 * Two-index cds_lfht (by proxy_stateid.other and by inode) with
 * Rule 6 ref-counted entries.  Modeled on lib/nfs4/server/
 * trust_stateid.c; see the design doc revision section "RCU + Rule 6
 * discipline for migration_record table" for the dual-index dance.
 *
 * Slice 6c-x.2 ships the table primitives + lease-expiry reaper.
 * The phase transitions and per-instance delta application that
 * PROXY_DONE / PROXY_CANCEL drive land in slice 6c-x.3; the
 * LAYOUTGET view-build hook lands in slice 6c-x.4.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <urcu.h>
#include <urcu/rculfhash.h>
#include <urcu/ref.h>
#include <xxhash.h>

#include "nfsv42_xdr.h"
#include "reffs/log.h"
#include "reffs/rcu.h"
#include "reffs/server.h"
#include "reffs/time.h"
#include "nfs4/migration_record.h"

/* ------------------------------------------------------------------ */
/* Internal state                                                      */

static struct cds_lfht *migration_ht_stid; /* keyed by stateid.other */
static struct cds_lfht *migration_ht_ino; /* keyed by inode pointer */

/*
 * Reaper cadence: scan every 30 s and abandon records whose lease
 * has been silent for `silence_threshold_ns`.  Threshold computed
 * per-scan from the current server lease time (1.5x lease).
 */
#define MIGRATION_REAPER_SCAN_SEC 30

static pthread_t migration_reaper_thread;
static pthread_mutex_t migration_reaper_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t migration_reaper_cv = PTHREAD_COND_INITIALIZER;
static _Atomic bool migration_reaper_running;

/* ------------------------------------------------------------------ */
/* Hash + match callbacks                                              */

static unsigned long mr_hash_stid(const uint8_t *other)
{
	return (unsigned long)XXH3_64bits(other, NFS4_OTHER_SIZE);
}

static unsigned long mr_hash_ino(uint64_t ino)
{
	return (unsigned long)XXH3_64bits(&ino, sizeof(ino));
}

static int mr_match_stid(struct cds_lfht_node *node, const void *key)
{
	const struct migration_record *mr =
		caa_container_of(node, struct migration_record, mr_stid_node);
	return memcmp(mr->mr_stateid_other, key, NFS4_OTHER_SIZE) == 0;
}

static int mr_match_ino(struct cds_lfht_node *node, const void *key)
{
	const struct migration_record *mr =
		caa_container_of(node, struct migration_record, mr_ino_node);
	const uint64_t *ino = key;

	return mr->mr_ino == *ino;
}

/* ------------------------------------------------------------------ */
/* Lifecycle callbacks                                                 */

static void mr_rcu_free(struct rcu_head *head)
{
	struct migration_record *mr =
		caa_container_of(head, struct migration_record, mr_rcu);

	free(mr->mr_deltas);
	free(mr);
}

static void mr_release(struct urcu_ref *ref)
{
	struct migration_record *mr =
		caa_container_of(ref, struct migration_record, mr_ref);

	/*
	 * Rule 6: idempotent removal from BOTH indices before scheduling
	 * the RCU-deferred free.  cds_lfht_del returns negative for
	 * already-removed nodes -- safe by design (the explicit-unhash
	 * paths run del before put, so this release-callback hits the
	 * already-removed branch in the common case).
	 *
	 * Tables may be NULL when a find ref outlives migration_record_fini's
	 * drain (caller held a ref across the shutdown).  Guard.
	 */
	rcu_read_lock();
	if (migration_ht_stid)
		cds_lfht_del(migration_ht_stid, &mr->mr_stid_node);
	if (migration_ht_ino)
		cds_lfht_del(migration_ht_ino, &mr->mr_ino_node);
	rcu_read_unlock();

	call_rcu(&mr->mr_rcu, mr_rcu_free);
}

void migration_record_put(struct migration_record *mr)
{
	if (mr)
		urcu_ref_put(&mr->mr_ref, mr_release);
}

/* ------------------------------------------------------------------ */
/* Reaper                                                              */

void migration_record_reaper_scan(uint64_t max_silence_ns, uint64_t now_mono_ns)
{
	if (!migration_ht_stid)
		return;

	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;

	rcu_read_lock();
	cds_lfht_first(migration_ht_stid, &iter);
	while ((node = cds_lfht_iter_get_node(&iter)) != NULL) {
		struct migration_record *mr = caa_container_of(
			node, struct migration_record, mr_stid_node);

		/* Advance BEFORE put -- Rule 6 reaper pattern. */
		cds_lfht_next(migration_ht_stid, &iter);

		if (!urcu_ref_get_unless_zero(&mr->mr_ref))
			continue;

		uint64_t last = atomic_load_explicit(
			&mr->mr_last_progress_mono_ns, memory_order_acquire);

		if (last == 0 || now_mono_ns < last + max_silence_ns) {
			migration_record_put(mr);
			continue;
		}

		enum migration_phase phase = atomic_load_explicit(
			&mr->mr_phase, memory_order_acquire);

		if (phase == MIGRATION_PHASE_COMMITTED ||
		    phase == MIGRATION_PHASE_ABANDONED) {
			migration_record_put(mr);
			continue;
		}

		TRACE("migration_reaper: abandoning stateid other=%02x%02x... ino=%llu silence=%llums",
		      mr->mr_stateid_other[0], mr->mr_stateid_other[1],
		      (unsigned long long)mr->mr_ino,
		      (unsigned long long)((now_mono_ns - last) / 1000000ULL));

		/*
		 * Transition phase to ABANDONED first; then explicit unhash
		 * + drop creation ref destroys the entry.  The find ref we
		 * just took is dropped after the unhash so the iterator can
		 * safely continue (advance happened above already).
		 */
		atomic_store_explicit(&mr->mr_phase, MIGRATION_PHASE_ABANDONED,
				      memory_order_release);
		migration_record_unhash(mr);
		migration_record_put(mr); /* find ref */
		migration_record_put(mr); /* creation ref */
	}
	rcu_read_unlock();
}

static void *migration_reaper_thread_fn(void *arg __attribute__((unused)))
{
	rcu_register_thread();

	while (atomic_load_explicit(&migration_reaper_running,
				    memory_order_relaxed)) {
		struct timespec ts;

		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_sec += MIGRATION_REAPER_SCAN_SEC;

		pthread_mutex_lock(&migration_reaper_mtx);
		pthread_cond_timedwait(&migration_reaper_cv,
				       &migration_reaper_mtx, &ts);
		pthread_mutex_unlock(&migration_reaper_mtx);

		if (!atomic_load_explicit(&migration_reaper_running,
					  memory_order_relaxed))
			break;

		struct server_state *ss = server_state_find();
		uint32_t lease_sec = ss ? server_lease_time(ss) : 0;

		server_state_put(ss);

		if (lease_sec == 0)
			continue;

		/*
		 * Silence threshold = 1.5x lease (per design doc revision
		 * "Lease accounting + PS-crash reaper").  Integer math: the
		 * factor is encoded as "lease + lease/2" to avoid casting
		 * through doubles.
		 */
		uint64_t silence_ns = (uint64_t)lease_sec * 1000000000ULL +
				      (uint64_t)lease_sec * 500000000ULL;

		migration_record_reaper_scan(silence_ns, reffs_now_ns());
	}

	rcu_unregister_thread();
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */

int migration_record_init(void)
{
	migration_ht_stid = cds_lfht_new(16, 16, 0, CDS_LFHT_AUTO_RESIZE, NULL);
	if (!migration_ht_stid) {
		LOG("migration_record_init: cds_lfht_new (stid) failed");
		return -ENOMEM;
	}

	migration_ht_ino = cds_lfht_new(16, 16, 0, CDS_LFHT_AUTO_RESIZE, NULL);
	if (!migration_ht_ino) {
		LOG("migration_record_init: cds_lfht_new (ino) failed");
		cds_lfht_destroy(migration_ht_stid, NULL);
		migration_ht_stid = NULL;
		return -ENOMEM;
	}

	atomic_store_explicit(&migration_reaper_running, true,
			      memory_order_relaxed);
	int ret = pthread_create(&migration_reaper_thread, NULL,
				 migration_reaper_thread_fn, NULL);
	if (ret != 0) {
		LOG("migration_record_init: pthread_create failed: %d", ret);
		cds_lfht_destroy(migration_ht_stid, NULL);
		cds_lfht_destroy(migration_ht_ino, NULL);
		migration_ht_stid = NULL;
		migration_ht_ino = NULL;
		atomic_store_explicit(&migration_reaper_running, false,
				      memory_order_relaxed);
		return -ret;
	}

	return 0;
}

void migration_record_fini(void)
{
	if (!migration_ht_stid)
		return;

	/*
	 * Same wakeup discipline as trust_stateid_fini: hold the mutex
	 * across the running-flag flip + cond_signal so the reaper
	 * cannot miss the wakeup.
	 */
	pthread_mutex_lock(&migration_reaper_mtx);
	atomic_store_explicit(&migration_reaper_running, false,
			      memory_order_relaxed);
	pthread_cond_signal(&migration_reaper_cv);
	pthread_mutex_unlock(&migration_reaper_mtx);
	pthread_join(migration_reaper_thread, NULL);

	/*
	 * Drain: drop the creation ref of each entry (Rule 6).  Iterate
	 * the stid index (it's the canonical one); the release callback
	 * removes from both indices.
	 */
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;

	rcu_read_lock();
	cds_lfht_first(migration_ht_stid, &iter);
	while ((node = cds_lfht_iter_get_node(&iter)) != NULL) {
		struct migration_record *mr = caa_container_of(
			node, struct migration_record, mr_stid_node);

		cds_lfht_next(migration_ht_stid, &iter);
		migration_record_put(mr);
	}
	rcu_read_unlock();

	synchronize_rcu();
	cds_lfht_destroy(migration_ht_stid, NULL);
	cds_lfht_destroy(migration_ht_ino, NULL);
	migration_ht_stid = NULL;
	migration_ht_ino = NULL;
}

int migration_record_create(const stateid4 *stid, struct super_block *sb,
			    uint64_t ino, const char *owner_reg,
			    uint32_t owner_reg_len,
			    struct migration_instance_delta *deltas,
			    uint32_t ndeltas, uint64_t initial_progress_mono_ns,
			    struct migration_record **out_mr)
{
	if (!migration_ht_stid)
		return -EINVAL;
	if (!stid || !owner_reg || !out_mr)
		return -EINVAL;
	if (owner_reg_len == 0 || owner_reg_len > MIGRATION_OWNER_REG_MAX)
		return -EINVAL;
	if (ndeltas > 0 && !deltas)
		return -EINVAL;

	*out_mr = NULL;

	/*
	 * Per-inode invariant check: refuse a second migration on a
	 * file that already has an active record.  Slice 6c-y will
	 * also enforce this at the assignment-builder layer; this
	 * defensive check makes mis-callers explicit rather than
	 * silently overwriting the prior record.
	 */
	rcu_read_lock();
	struct cds_lfht_iter iter;

	cds_lfht_lookup(migration_ht_ino, mr_hash_ino(ino), mr_match_ino, &ino,
			&iter);
	if (cds_lfht_iter_get_node(&iter) != NULL) {
		rcu_read_unlock();
		return -EBUSY;
	}
	rcu_read_unlock();

	struct migration_record *mr = calloc(1, sizeof(*mr));

	if (!mr)
		return -ENOMEM;

	memcpy(mr->mr_stateid_other, stid->other, NFS4_OTHER_SIZE);
	mr->mr_ino = ino;
	mr->mr_sb = sb;

	memcpy(mr->mr_owner_reg, owner_reg, owner_reg_len);
	mr->mr_owner_reg_len = owner_reg_len;

	atomic_store_explicit(&mr->mr_phase, MIGRATION_PHASE_PENDING,
			      memory_order_relaxed);
	atomic_store_explicit(&mr->mr_last_progress_mono_ns,
			      initial_progress_mono_ns, memory_order_relaxed);

	mr->mr_ndeltas = ndeltas;
	mr->mr_deltas = deltas; /* takes ownership */

	urcu_ref_init(&mr->mr_ref);

	rcu_read_lock();
	cds_lfht_add(migration_ht_stid,
		     mr_hash_stid((const uint8_t *)stid->other),
		     &mr->mr_stid_node);
	cds_lfht_add(migration_ht_ino, mr_hash_ino(ino), &mr->mr_ino_node);
	rcu_read_unlock();

	*out_mr = mr;
	return 0;
}

int migration_record_renew(const stateid4 *stid, uint64_t now_mono_ns)
{
	struct migration_record *mr = migration_record_find_by_stateid(stid);

	if (!mr)
		return -ENOENT;

	atomic_store_explicit(&mr->mr_last_progress_mono_ns, now_mono_ns,
			      memory_order_release);
	migration_record_put(mr);
	return 0;
}

void migration_record_unhash(struct migration_record *mr)
{
	if (!mr || !migration_ht_stid)
		return;

	rcu_read_lock();
	cds_lfht_del(migration_ht_stid, &mr->mr_stid_node);
	cds_lfht_del(migration_ht_ino, &mr->mr_ino_node);
	rcu_read_unlock();
}

int migration_record_commit(struct migration_record *mr)
{
	if (!mr)
		return -EINVAL;

	enum migration_phase expected = MIGRATION_PHASE_PENDING;

	if (!atomic_compare_exchange_strong_explicit(
		    &mr->mr_phase, &expected, MIGRATION_PHASE_COMMITTED,
		    memory_order_acq_rel, memory_order_acquire)) {
		expected = MIGRATION_PHASE_IN_PROGRESS;
		if (!atomic_compare_exchange_strong_explicit(
			    &mr->mr_phase, &expected, MIGRATION_PHASE_COMMITTED,
			    memory_order_acq_rel, memory_order_acquire))
			return -EALREADY;
	}

	migration_record_unhash(mr);
	migration_record_put(mr); /* drop creation ref */
	return 0;
}

int migration_record_abandon(struct migration_record *mr)
{
	if (!mr)
		return -EINVAL;

	enum migration_phase expected = MIGRATION_PHASE_PENDING;

	if (!atomic_compare_exchange_strong_explicit(
		    &mr->mr_phase, &expected, MIGRATION_PHASE_ABANDONED,
		    memory_order_acq_rel, memory_order_acquire)) {
		expected = MIGRATION_PHASE_IN_PROGRESS;
		if (!atomic_compare_exchange_strong_explicit(
			    &mr->mr_phase, &expected, MIGRATION_PHASE_ABANDONED,
			    memory_order_acq_rel, memory_order_acquire))
			return -EALREADY;
	}

	migration_record_unhash(mr);
	migration_record_put(mr); /* drop creation ref */
	return 0;
}

struct migration_record *migration_record_find_by_stateid(const stateid4 *stid)
{
	if (!stid || !migration_ht_stid)
		return NULL;

	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;
	struct migration_record *mr = NULL;

	rcu_read_lock();
	cds_lfht_lookup(migration_ht_stid,
			mr_hash_stid((const uint8_t *)stid->other),
			mr_match_stid, stid->other, &iter);
	node = cds_lfht_iter_get_node(&iter);
	if (node) {
		struct migration_record *cand = caa_container_of(
			node, struct migration_record, mr_stid_node);

		if (urcu_ref_get_unless_zero(&cand->mr_ref))
			mr = cand;
	}
	rcu_read_unlock();
	return mr;
}

struct migration_record *migration_record_find_by_inode(uint64_t ino)
{
	if (!migration_ht_ino)
		return NULL;

	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;
	struct migration_record *mr = NULL;

	rcu_read_lock();
	cds_lfht_lookup(migration_ht_ino, mr_hash_ino(ino), mr_match_ino, &ino,
			&iter);
	node = cds_lfht_iter_get_node(&iter);
	if (node) {
		struct migration_record *cand = caa_container_of(
			node, struct migration_record, mr_ino_node);

		if (urcu_ref_get_unless_zero(&cand->mr_ref))
			mr = cand;
	}
	rcu_read_unlock();
	return mr;
}
