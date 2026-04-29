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
#include "reffs/client.h"
#include "reffs/filehandle.h"
#include "reffs/inode.h"
#include "reffs/log.h"
#include "reffs/rcu.h"
#include "reffs/runway.h"
#include "reffs/server.h"
#include "reffs/super_block.h"
#include "reffs/time.h"
#include "nfs4/cb.h"
#include "nfs4/client.h"
#include "nfs4/migration_record.h"
#include "nfs4/session.h"
#include "nfs4/stateid.h"

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

/*
 * Slice 6c-zz persistence helpers (defined later in this file).
 * Forward-declared here because migration_record_create and
 * migration_record_unhash call them.
 */
static void migration_record_save_one(const struct migration_record *mr);
static void migration_record_remove_one(const uint8_t *stateid_other);

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
	mr->mr_sb_id = sb ? sb->sb_id : 0;

	memcpy(mr->mr_owner_reg, owner_reg, owner_reg_len);
	mr->mr_owner_reg_len = owner_reg_len;

	atomic_store_explicit(&mr->mr_seqid, stid->seqid, memory_order_relaxed);
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

	/*
	 * Slice 6c-zz: persist the freshly-hashed record so a future
	 * MDS restart can reload it.  Save AFTER the hash insert so a
	 * crash during persist still leaves the in-memory state
	 * consistent (the record is live and its proxy_stateid is the
	 * same; the next save attempt -- e.g. on commit -- supersedes
	 * the partial state).  No-op when persistence is unattached.
	 */
	migration_record_save_one(mr);

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

	/*
	 * Slice 6c-zz: remove from disk too.  Symmetric with the
	 * save in migration_record_create.  Idempotent at the backend
	 * (already-removed -> 0).
	 */
	migration_record_remove_one(mr->mr_stateid_other);
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

int migration_apply_deltas_to_segment(const struct layout_segment *base_seg,
				      uint32_t base_seg_index,
				      const struct migration_record *mr,
				      struct layout_segment *out_view)
{
	if (!base_seg || !mr || !out_view)
		return -EINVAL;

	/*
	 * Worst-case view size: every base instance survives + every
	 * INCOMING delta in this segment is appended.  Walk the deltas
	 * once to count the INCOMING entries; allocate accordingly.
	 */
	uint32_t incoming = 0;

	for (uint32_t i = 0; i < mr->mr_ndeltas; i++) {
		const struct migration_instance_delta *d = &mr->mr_deltas[i];

		if (d->mid_seg_index == base_seg_index &&
		    d->mid_state == MIGRATION_INSTANCE_INCOMING)
			incoming++;
	}

	uint32_t cap = base_seg->ls_nfiles + incoming;

	/*
	 * Copy scalar fields from the base segment.  ls_files / ls_nfiles
	 * are populated below.
	 */
	*out_view = *base_seg;
	out_view->ls_files = NULL;
	out_view->ls_nfiles = 0;

	if (cap == 0)
		return 0; /* empty layout, no allocation needed */

	out_view->ls_files = calloc(cap, sizeof(*out_view->ls_files));
	if (!out_view->ls_files)
		return -ENOMEM;

	uint32_t out_n = 0;

	/*
	 * Phase 1: copy non-DRAINING base entries.  Skip any base
	 * position covered by a DRAINING delta (omit-and-replace).
	 * INTERPOSED is forward-compat per design-doc invariant 3 --
	 * no slice-6c-x autopilot path emits INTERPOSED, so the
	 * lookup below will not find one in this slice; if a future
	 * slice adds them, we keep the base entry (the PS shadow is
	 * invisible to the LAYOUTGET view by definition).
	 */
	for (uint32_t i = 0; i < base_seg->ls_nfiles; i++) {
		bool draining = false;

		for (uint32_t k = 0; k < mr->mr_ndeltas; k++) {
			const struct migration_instance_delta *d =
				&mr->mr_deltas[k];

			if (d->mid_seg_index == base_seg_index &&
			    d->mid_instance_index == i &&
			    d->mid_state == MIGRATION_INSTANCE_DRAINING) {
				draining = true;
				break;
			}
		}
		if (draining)
			continue;
		out_view->ls_files[out_n++] = base_seg->ls_files[i];
	}

	/*
	 * Phase 2: append INCOMING entries.  Each INCOMING delta
	 * carries a fully-populated layout_data_file in
	 * mid_replacement_file (built by the record's creator).
	 */
	for (uint32_t k = 0; k < mr->mr_ndeltas; k++) {
		const struct migration_instance_delta *d = &mr->mr_deltas[k];

		if (d->mid_seg_index == base_seg_index &&
		    d->mid_state == MIGRATION_INSTANCE_INCOMING)
			out_view->ls_files[out_n++] = d->mid_replacement_file;
	}

	out_view->ls_nfiles = out_n;
	return 0;
}

void migration_release_view(struct layout_segment *view)
{
	if (!view)
		return;
	free(view->ls_files);
	view->ls_files = NULL;
	view->ls_nfiles = 0;
}

/* ------------------------------------------------------------------ */
/* Slice 6c-zz: persistence + reload                                   */

/*
 * Compile-time guards: the persistent header
 * lib/include/reffs/migration_persist.h hardcodes the constants it
 * shares with the in-memory side (NFS4_OTHER_SIZE / MIGRATION_OWNER_REG_MAX
 * / RUNWAY_MAX_FH) so the backends layer does not have to pull in
 * lib/nfs4 headers (one-way dependency rule).  Drift between the two
 * copies would silently corrupt the on-disk format on reload, so
 * pin them here -- this is the only .c file that includes both
 * sides and can check.
 */
_Static_assert(MR_PERSIST_NFS4_OTHER_SIZE == NFS4_OTHER_SIZE,
	       "migration_persist.h NFS4_OTHER_SIZE drift");
_Static_assert(MR_PERSIST_OWNER_REG_MAX == MIGRATION_OWNER_REG_MAX,
	       "migration_persist.h owner_reg max drift");
_Static_assert(MR_PERSIST_FH_MAX == RUNWAY_MAX_FH,
	       "migration_persist.h FH max drift vs runway");

int migration_record_to_persistent(const struct migration_record *mr,
				   struct migration_record_persistent *out)
{
	if (!mr || !out)
		return -EINVAL;
	if (mr->mr_ndeltas > MR_PERSIST_MAX_DELTAS)
		return -E2BIG;

	memset(out, 0, sizeof(*out));
	memcpy(out->mrp_stateid_other, mr->mr_stateid_other,
	       MR_PERSIST_NFS4_OTHER_SIZE);
	out->mrp_seqid =
		atomic_load_explicit(&mr->mr_seqid, memory_order_relaxed);
	/*
	 * Use mr_sb_id (set at register time and preserved across
	 * the persistence reload path) rather than dereferencing
	 * mr_sb here.  After a reload mr_sb is NULL but mr_sb_id
	 * still carries the original sb identity.
	 */
	out->mrp_sb_id = mr->mr_sb_id;
	out->mrp_ino = mr->mr_ino;
	out->mrp_owner_reg_len = mr->mr_owner_reg_len;
	if (mr->mr_owner_reg_len > 0)
		memcpy(out->mrp_owner_reg, mr->mr_owner_reg,
		       mr->mr_owner_reg_len);
	out->mrp_ndeltas = mr->mr_ndeltas;
	for (uint32_t i = 0; i < mr->mr_ndeltas; i++) {
		const struct migration_instance_delta *src = &mr->mr_deltas[i];
		struct mr_persist_instance_delta *dst = &out->mrp_deltas[i];

		dst->pmid_seg_index = src->mid_seg_index;
		dst->pmid_instance_index = src->mid_instance_index;
		dst->pmid_state = (uint32_t)src->mid_state;
		dst->pmid_replacement_delta_idx =
			src->mid_replacement_delta_idx;
		dst->pmid_replacement_file.pldf_dstore_id =
			src->mid_replacement_file.ldf_dstore_id;
		dst->pmid_replacement_file.pldf_fh_len =
			src->mid_replacement_file.ldf_fh_len;
		if (src->mid_replacement_file.ldf_fh_len > 0)
			memcpy(dst->pmid_replacement_file.pldf_fh,
			       src->mid_replacement_file.ldf_fh,
			       src->mid_replacement_file.ldf_fh_len);
	}
	return 0;
}

int migration_record_from_persistent(
	const struct migration_record_persistent *mrp)
{
	if (!mrp)
		return -EINVAL;
	if (mrp->mrp_owner_reg_len == 0 ||
	    mrp->mrp_owner_reg_len > MIGRATION_OWNER_REG_MAX)
		return -EINVAL;
	if (mrp->mrp_ndeltas > MR_PERSIST_MAX_DELTAS)
		return -EINVAL;

	struct migration_instance_delta *deltas = NULL;

	if (mrp->mrp_ndeltas > 0) {
		deltas = calloc(mrp->mrp_ndeltas, sizeof(*deltas));
		if (!deltas)
			return -ENOMEM;
		for (uint32_t i = 0; i < mrp->mrp_ndeltas; i++) {
			const struct mr_persist_instance_delta *src =
				&mrp->mrp_deltas[i];
			struct migration_instance_delta *dst = &deltas[i];

			dst->mid_seg_index = src->pmid_seg_index;
			dst->mid_instance_index = src->pmid_instance_index;
			dst->mid_state =
				(enum migration_instance_state)src->pmid_state;
			dst->mid_replacement_delta_idx =
				src->pmid_replacement_delta_idx;
			dst->mid_replacement_file.ldf_dstore_id =
				src->pmid_replacement_file.pldf_dstore_id;
			dst->mid_replacement_file.ldf_fh_len =
				src->pmid_replacement_file.pldf_fh_len;
			if (src->pmid_replacement_file.pldf_fh_len > 0)
				memcpy(dst->mid_replacement_file.ldf_fh,
				       src->pmid_replacement_file.pldf_fh,
				       src->pmid_replacement_file.pldf_fh_len);
		}
	}

	stateid4 stid;

	memset(&stid, 0, sizeof(stid));
	stid.seqid = mrp->mrp_seqid;
	memcpy(stid.other, mrp->mrp_stateid_other, MR_PERSIST_NFS4_OTHER_SIZE);

	struct migration_record *mr = NULL;
	/*
	 * sb is NULL on the reload path -- the in-memory super_block
	 * pointer cannot be reconstructed from disk.  PROXY_DONE /
	 * PROXY_CANCEL handlers compare on (sb_id, ino) when
	 * c_curr_sb is NULL anyway (slice 6c-x.3 priority-rule
	 * step 5), so a NULL sb is benign at the auth layer.  A
	 * future refinement could re-resolve sb_id -> super_block
	 * via super_block_find_for_listener after sb_registry_load
	 * runs.
	 */
	int ret = migration_record_create(&stid, NULL, mrp->mrp_ino,
					  (const char *)mrp->mrp_owner_reg,
					  mrp->mrp_owner_reg_len, deltas,
					  mrp->mrp_ndeltas, reffs_now_ns(),
					  &mr);

	if (ret < 0) {
		free(deltas);
		return ret;
	}
	/*
	 * Patch in the persisted sb_id so PROXY_DONE / PROXY_CANCEL
	 * lookups still see the right sb identity even though the
	 * in-memory super_block * is NULL on the reload path.  Slice
	 * 6c-zz reviewer note W2.
	 */
	mr->mr_sb_id = mrp->mrp_sb_id;

	/* deltas[] ownership transferred into mr by create. */
	return 0;
}

/*
 * Persistence hook: callable from the persist_ops backend if the
 * caller wired one in.  These two functions are NULL when the
 * server runs without persistence (RAM backend); record save /
 * remove become no-ops in that case.
 */
static const struct persist_ops *mr_persist_ops;
static void *mr_persist_ctx;

void migration_record_persist_attach(const struct persist_ops *ops, void *ctx)
{
	mr_persist_ops = ops;
	mr_persist_ctx = ctx;
}

static void migration_record_save_one(const struct migration_record *mr)
{
	int ret;

	if (!mr_persist_ops || !mr_persist_ops->migration_record_save)
		return;

	struct migration_record_persistent buf;

	if (migration_record_to_persistent(mr, &buf) < 0)
		return;

	ret = mr_persist_ops->migration_record_save(mr_persist_ctx, &buf);
	if (ret < 0) {
		/*
		 * Persist failure is not fatal -- the record is live in
		 * the in-memory tables, the next save attempt (commit,
		 * lease renewal) supersedes it -- but the operator should
		 * see it: ENOSPC at this point means migrations issued
		 * after this point will not survive an MDS restart.
		 */
		LOG("migration_record: persist save failed (errno %d); "
		    "in-memory state still consistent",
		    -ret);
	}
}

static void migration_record_remove_one(const uint8_t *stateid_other)
{
	int ret;

	if (!mr_persist_ops || !mr_persist_ops->migration_record_remove)
		return;

	ret = mr_persist_ops->migration_record_remove(mr_persist_ctx,
						      stateid_other);
	if (ret < 0) {
		LOG("migration_record: persist remove failed (errno %d); "
		    "stale on-disk record will be reaped on next save or load",
		    -ret);
	}
}

struct mr_load_ctx {
	int loaded;
};

static int mr_load_cb(const struct migration_record_persistent *mrp, void *arg)
{
	struct mr_load_ctx *lc = arg;
	int ret = migration_record_from_persistent(mrp);

	if (ret == 0)
		lc->loaded++;
	/*
	 * Per-record errors are non-fatal: log and skip so a single
	 * malformed record doesn't block the rest of the table from
	 * loading.  -EBUSY in particular means the inode already had a
	 * live record before reload, which shouldn't happen at init but
	 * isn't a load failure.
	 */
	if (ret < 0 && ret != -EBUSY)
		LOG("migration_record_load: record skipped: %d", ret);
	return 0;
}

int migration_record_load_persisted(const struct persist_ops *ops, void *ctx)
{
	if (!ops || !ops->migration_record_load)
		return 0;
	struct mr_load_ctx lc = { .loaded = 0 };
	int ret = ops->migration_record_load(ctx, mr_load_cb, &lc);

	if (ret < 0)
		return ret;
	return lc.loaded;
}

unsigned int migration_recall_layouts(struct inode *inode,
				      struct client *exclude_client,
				      struct server_state *ss)
{
	if (!inode || !inode->i_stateids)
		return 0;

	/*
	 * The fh used in CB_LAYOUTRECALL is the standard MDS file
	 * handle: { sb_id, ino }.  Build once and reuse for every
	 * recall in the loop -- the layout_recall body copies the
	 * bytes during XDR encode.
	 */
	struct network_file_handle cb_nfh = { 0 };

	cb_nfh.nfh_ino = inode->i_ino;
	cb_nfh.nfh_sb = inode->i_sb ? inode->i_sb->sb_id : 0;
	nfs_fh4 cb_fh4 = {
		.nfs_fh4_len = sizeof(cb_nfh),
		.nfs_fh4_val = (char *)&cb_nfh,
	};

	unsigned int queued = 0;
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;

	rcu_read_lock();
	cds_lfht_for_each(inode->i_stateids, &iter, node)
	{
		struct stateid *stid =
			caa_container_of(node, struct stateid, s_inode_node);

		if (stid->s_tag != Layout_Stateid)
			continue;
		if (!stid->s_client || stid->s_client == exclude_client)
			continue;
		if (!stateid_get(stid))
			continue;

		struct nfs4_client *nc = client_to_nfs4(stid->s_client);
		struct nfs4_session *sess =
			ss ? nfs4_session_find_for_client(ss, nc) : NULL;

		if (sess) {
			stateid4 wire_stid;

			pack_stateid4(&wire_stid, stid);
			(void)nfs4_cb_layoutrecall_fnf(
				sess, LAYOUT4_FLEX_FILES_V2,
				LAYOUTIOMODE4_ANY, /* changed=any */
				1 /* clora_changed=true */, &cb_fh4,
				0 /* offset */, NFS4_UINT64_MAX /* length */,
				&wire_stid);
			nfs4_session_put(sess);
			queued++;
		}
		stateid_put(stid);
	}
	rcu_read_unlock();
	return queued;
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
