/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Per-listener write buffer table for PS Phase 4a.
 *
 * Each NFSv4 client (open stateid) accumulates WRITE bytes here per
 * upstream FH; ps_proxy_pipeline_commit (4a.2b) flushes the buffer
 * through ec_write_codec_with_file when the client COMMITs.
 *
 * See .claude/design/proxy-server-phase4a.md for the full quiesce +
 * Rule 6 lifecycle design.  This file ships the table machinery and
 * the quiesce primitives; the actual flush-on-COMMIT shim lives in
 * ps_proxy_ops.c (Phase 4a step 5, slice 4a.2b).
 */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <urcu.h>
#include <urcu/rculfhash.h>
#include <urcu/ref.h>
#include <xxhash.h>

#include "ps_state.h"
#include "ps_write_buffer.h"
#include "ps_write_buffer_internal.h"

/* ------------------------------------------------------------------ */
/* Test hook storage                                                   */
/* ------------------------------------------------------------------ */

_Atomic(void (*)(void)) ps_test_hook_pre_state_load = NULL;
_Atomic(void (*)(void)) ps_test_hook_in_codec_flush = NULL;
_Atomic(uint64_t (*)(void)) ps_test_hook_clock_now_ns = NULL;

/* ------------------------------------------------------------------ */
/* Hash + match                                                        */
/* ------------------------------------------------------------------ */

/*
 * Hash the lookup key: stateid_other (12 bytes) || upstream_fh.
 * Per-listener tables so we do NOT mix listener_id into the hash --
 * the listener disambiguator is the table-pointer itself.  Different
 * listeners hold different lfht instances.
 *
 * Stack-concatenate the key into a small fixed buffer so we can use
 * the stateless XXH64() entry point (the streaming XXH64_state_t API
 * isn't always exposed by the installed xxhash header on every
 * distro).  PS_MAX_FH_SIZE (128) plus PS_STATEID_OTHER_SIZE (12) is
 * 140 bytes -- comfortable stack frame.
 */
static unsigned long pwb_hash(const uint8_t *stateid_other, const uint8_t *fh,
			      uint32_t fh_len)
{
	uint8_t key[PS_STATEID_OTHER_SIZE + PS_MAX_FH_SIZE];

	memcpy(key, stateid_other, PS_STATEID_OTHER_SIZE);
	memcpy(key + PS_STATEID_OTHER_SIZE, fh, fh_len);
	return (unsigned long)XXH64(key, PS_STATEID_OTHER_SIZE + fh_len, 0);
}

struct pwb_match_arg {
	const uint8_t *stateid_other;
	const uint8_t *fh;
	uint32_t fh_len;
};

static int pwb_match(struct cds_lfht_node *node, const void *key)
{
	const struct ps_write_buffer *buf =
		caa_container_of(node, struct ps_write_buffer, pwb_ht_node);
	const struct pwb_match_arg *m = key;

	if (buf->pwb_upstream_fh_len != m->fh_len)
		return 0;
	if (memcmp(buf->pwb_stateid_other, m->stateid_other,
		   PS_STATEID_OTHER_SIZE) != 0)
		return 0;
	if (memcmp(buf->pwb_upstream_fh, m->fh, m->fh_len) != 0)
		return 0;
	return 1;
}

/* ------------------------------------------------------------------ */
/* Dirty-stripe table (Phase 4b)                                       */
/* ------------------------------------------------------------------ */

/*
 * The dirty hash table is keyed by stripe number (uint32_t).  We
 * use cds_lfht for size-flexibility (sparse vs dense workloads);
 * the per-buffer mutex (pwb_mutex) provides all the serialisation
 * the table needs.  rcu_read_lock is taken around the cds_lfht
 * traversals purely to satisfy liburcu's API contract.
 */
#define PS_DIRTY_HT_BUCKETS_INIT 16u

static unsigned long pds_hash(uint32_t stripe_no)
{
	return (unsigned long)stripe_no;
}

static int pds_match(struct cds_lfht_node *node, const void *key)
{
	const struct ps_dirty_stripe *ds =
		caa_container_of(node, struct ps_dirty_stripe, pds_ht_node);
	const uint32_t *want = key;

	return ds->pds_stripe_no == *want;
}

/* Small bit-array helpers on a uint8_t[] backing store. */
static void pds_set_bit(uint8_t *mask, uint32_t bit)
{
	mask[bit >> 3] |= (uint8_t)(1u << (bit & 7u));
}

static bool pds_test_bit(const uint8_t *mask, uint32_t bit)
{
	return (mask[bit >> 3] & (1u << (bit & 7u))) != 0;
}

static bool pds_all_bits_set(const uint8_t *mask, uint32_t nbits)
{
	uint32_t i;

	for (i = 0; i < nbits; i++) {
		if (!pds_test_bit(mask, i))
			return false;
	}
	return true;
}

/* ------------------------------------------------------------------ */
/* Release callbacks (Rule 6 lifecycle)                                */
/* ------------------------------------------------------------------ */

/*
 * Drain the dirty-stripe table for a buffer that is about to be
 * freed.  Caller must guarantee no other thread can touch the
 * table -- pwb_release is the call site, and at that point the
 * refcount is zero (no find/table ref outstanding) and the buffer
 * has just been removed from the per-listener table.
 *
 * Direct free of the dirty entries is safe: the dirty hash table
 * is only reachable via buf->pwb_dirty_ht, the buffer is now
 * unreachable, and pwb_mutex (held only by mark_dirty/lookup) is
 * not contended.  rcu_read_lock around the iterator is liburcu
 * API boilerplate.  We do this synchronously in pwb_release
 * rather than from the rcu callback so cds_lfht_destroy is never
 * called from a call_rcu context (rcu-violations.md Pattern 6
 * footgun -- some lfht destroy paths may eventually call
 * synchronize_rcu).
 */
static void pwb_dirty_table_drain(struct ps_write_buffer *buf)
{
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;

	if (!buf->pwb_dirty_ht)
		return;

	rcu_read_lock();
	cds_lfht_first(buf->pwb_dirty_ht, &iter);
	while ((node = cds_lfht_iter_get_node(&iter)) != NULL) {
		struct ps_dirty_stripe *ds = caa_container_of(
			node, struct ps_dirty_stripe, pds_ht_node);

		cds_lfht_next(buf->pwb_dirty_ht, &iter);
		cds_lfht_del(buf->pwb_dirty_ht, node);
		free(ds->pds_partial_mask);
		free(ds);
	}
	rcu_read_unlock();
	cds_lfht_destroy(buf->pwb_dirty_ht, NULL);
	buf->pwb_dirty_ht = NULL;
}

static void pwb_rcu_free(struct rcu_head *head)
{
	struct ps_write_buffer *buf =
		caa_container_of(head, struct ps_write_buffer, pwb_rcu_head);

	pthread_mutex_destroy(&buf->pwb_mutex);
	free(buf->pwb_data);
	free(buf);
}

/*
 * urcu_ref release: refcount reached zero.  cds_lfht_del is
 * idempotent (returns negative if the node is already unlinked) so
 * we ALWAYS del here -- defends against any caller path that drops
 * the final ref without explicitly del'ing first (the prior version
 * relied on caller discipline, which is a leak vector waiting for
 * the first programmer to forget).
 *
 * Requires pwb_ht to be set; buffers that were never inserted
 * (alloc-then-fail-before-insert) must not reach this release --
 * lookup_or_alloc destroys those directly.
 *
 * Dirty-stripe drain runs synchronously here (before the rcu
 * callback) so cds_lfht_destroy never runs from a call_rcu context.
 */
static void pwb_release(struct urcu_ref *ref)
{
	struct ps_write_buffer *buf =
		caa_container_of(ref, struct ps_write_buffer, pwb_ref);

	if (buf->pwb_ht)
		cds_lfht_del(buf->pwb_ht, &buf->pwb_ht_node);
	pwb_dirty_table_drain(buf);
	call_rcu(&buf->pwb_rcu_head, pwb_rcu_free);
}

/* ------------------------------------------------------------------ */
/* Table lifecycle (called from ps_state.c)                            */
/* ------------------------------------------------------------------ */

int ps_write_buffer_table_init(struct ps_listener_state *pls)
{
	int err;

	if (!pls)
		return -EINVAL;

	pls->pls_write_buffer_ht = cds_lfht_new(
		PS_WRITE_BUFFER_HASH_BUCKETS_INIT,
		PS_WRITE_BUFFER_HASH_BUCKETS_INIT,
		/* max_nr_buckets */ 0, CDS_LFHT_AUTO_RESIZE, NULL);
	if (!pls->pls_write_buffer_ht)
		return -ENOMEM;

	err = pthread_mutex_init(&pls->pls_drain_mutex, NULL);
	if (err != 0) {
		cds_lfht_destroy(pls->pls_write_buffer_ht, NULL);
		pls->pls_write_buffer_ht = NULL;
		return -err;
	}
	err = pthread_cond_init(&pls->pls_drain_cv, NULL);
	if (err != 0) {
		pthread_mutex_destroy(&pls->pls_drain_mutex);
		cds_lfht_destroy(pls->pls_write_buffer_ht, NULL);
		pls->pls_write_buffer_ht = NULL;
		return -err;
	}

	/*
	 * Init-before-publish: relaxed stores are sufficient because
	 * the caller (ps_state_register) does a release-store on
	 * ps_nlisteners after we return, which provides the publish
	 * edge for every field in this struct.
	 */
	atomic_store_explicit(&pls->pls_state, PS_LISTENER_RUNNING,
			      memory_order_relaxed);
	atomic_store_explicit(&pls->pls_boot_gen, 1, memory_order_relaxed);
	atomic_store_explicit(&pls->pls_active_buffer_refs, 0,
			      memory_order_relaxed);
	atomic_store_explicit(&pls->pls_cap_rejections_total, 0,
			      memory_order_relaxed);
	atomic_store_explicit(&pls->pls_fbig_rejections_total, 0,
			      memory_order_relaxed);
	atomic_store_explicit(&pls->pls_close_flush_timeouts_total, 0,
			      memory_order_relaxed);
	return 0;
}

void ps_write_buffer_table_destroy(struct ps_listener_state *pls)
{
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;

	if (!pls || !pls->pls_write_buffer_ht)
		return;

	/*
	 * Walk dropping table refs.  Caller (ps_listener_stop) already
	 * waited for pls_active_buffer_refs to reach zero, so no
	 * concurrent op holds a find ref -- the only ref on each entry
	 * is the table ref.
	 */
	rcu_read_lock();
	cds_lfht_first(pls->pls_write_buffer_ht, &iter);
	while ((node = cds_lfht_iter_get_node(&iter)) != NULL) {
		struct ps_write_buffer *buf = caa_container_of(
			node, struct ps_write_buffer, pwb_ht_node);

		/*
		 * Advance BEFORE put per patterns/rcu-violations.md
		 * Pattern 7: put may run the release callback which
		 * call_rcu's the free, and we cannot iterate past a
		 * removed-then-freed node.  cds_lfht_del is idempotent.
		 */
		cds_lfht_next(pls->pls_write_buffer_ht, &iter);
		cds_lfht_del(pls->pls_write_buffer_ht, node);
		urcu_ref_put(&buf->pwb_ref, pwb_release);
	}
	rcu_read_unlock();
	synchronize_rcu();
	cds_lfht_destroy(pls->pls_write_buffer_ht, NULL);
	pls->pls_write_buffer_ht = NULL;

	pthread_cond_destroy(&pls->pls_drain_cv);
	pthread_mutex_destroy(&pls->pls_drain_mutex);
}

/* ------------------------------------------------------------------ */
/* Quiesce protocol                                                    */
/* ------------------------------------------------------------------ */

bool ps_write_buffer_enter_quiesce_or_bail(struct ps_listener_state *pls)
{
	void (*hook)(void);

	if (!pls)
		return false;

	atomic_fetch_add_explicit(&pls->pls_active_buffer_refs, 1,
				  memory_order_acq_rel);

	/*
	 * Test hook: deterministic CPU pause between the fetch_add and
	 * the state load, so a TOCTOU test can transition pls_state to
	 * DRAINING during the gap and assert we observe DRAINING on
	 * the post-add load.  Production builds load NULL and skip.
	 */
	hook = atomic_load_explicit(&ps_test_hook_pre_state_load,
				    memory_order_relaxed);
	if (hook)
		hook();

	enum ps_listener_state_kind state =
		atomic_load_explicit(&pls->pls_state, memory_order_acquire);

	if (state != PS_LISTENER_RUNNING) {
		ps_write_buffer_leave_quiesce(pls);
		return false;
	}
	return true;
}

void ps_write_buffer_leave_quiesce(struct ps_listener_state *pls)
{
	uint64_t prev;

	if (!pls)
		return;

	prev = atomic_fetch_sub_explicit(&pls->pls_active_buffer_refs, 1,
					 memory_order_acq_rel);
	if (prev == 1) {
		/*
		 * Standard cv-predicate discipline: teardown loads the
		 * counter UNDER pls_drain_mutex; we MUST take the mutex
		 * around the broadcast to avoid a lost-wakeup race
		 * (teardown reads counter > 0, we fetch_sub to 0 +
		 * broadcast before teardown enters cond_wait -- without
		 * the mutex teardown would sleep forever).
		 */
		pthread_mutex_lock(&pls->pls_drain_mutex);
		pthread_cond_broadcast(&pls->pls_drain_cv);
		pthread_mutex_unlock(&pls->pls_drain_mutex);
	}
}

/* ------------------------------------------------------------------ */
/* Buffer lookup / alloc                                               */
/* ------------------------------------------------------------------ */

struct ps_write_buffer *ps_write_buffer_lookup_or_alloc(
	struct ps_listener_state *pls,
	const uint8_t stateid_other[PS_STATEID_OTHER_SIZE],
	const uint8_t *upstream_fh, uint32_t upstream_fh_len)
{
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;
	struct ps_write_buffer *buf = NULL;
	struct pwb_match_arg key = {
		.stateid_other = stateid_other,
		.fh = upstream_fh,
		.fh_len = upstream_fh_len,
	};
	unsigned long hash;

	if (!pls || !pls->pls_write_buffer_ht || !stateid_other ||
	    !upstream_fh || upstream_fh_len == 0 ||
	    upstream_fh_len > PS_MAX_FH_SIZE) {
		/*
		 * Caller passed enter_quiesce_or_bail before reaching
		 * us and holds the pls_active_buffer_refs reservation.
		 * Symmetric contract: every NULL-return path here
		 * releases that reservation so the caller never has
		 * to remember to leave_quiesce on failure.
		 *
		 * Reservation is per-op; if the caller bails without
		 * touching a buffer it must call leave_quiesce
		 * directly.  See ps_write_buffer.h.
		 */
		if (pls)
			ps_write_buffer_leave_quiesce(pls);
		return NULL;
	}

	hash = pwb_hash(stateid_other, upstream_fh, upstream_fh_len);

	/* Lookup first.  rcu_read_lock for the lfht traversal. */
	rcu_read_lock();
	cds_lfht_lookup(pls->pls_write_buffer_ht, hash, pwb_match, &key, &iter);
	node = cds_lfht_iter_get_node(&iter);
	if (node) {
		buf = caa_container_of(node, struct ps_write_buffer,
				       pwb_ht_node);
		if (!urcu_ref_get_unless_zero(&buf->pwb_ref))
			buf = NULL; /* losing race with teardown */
	}
	rcu_read_unlock();

	if (buf)
		return buf;

	/* Allocate + insert. */
	buf = calloc(1, sizeof(*buf));
	if (!buf) {
		ps_write_buffer_leave_quiesce(pls);
		return NULL;
	}

	memcpy(buf->pwb_stateid_other, stateid_other, PS_STATEID_OTHER_SIZE);
	memcpy(buf->pwb_upstream_fh, upstream_fh, upstream_fh_len);
	buf->pwb_upstream_fh_len = upstream_fh_len;
	buf->pwb_listener_id = pls->pls_listener_id;
	buf->pwb_listener_gen =
		atomic_load_explicit(&pls->pls_boot_gen, memory_order_acquire);
	buf->pwb_data = NULL;
	buf->pwb_capacity = 0;
	buf->pwb_high_water = 0;
	if (pthread_mutex_init(&buf->pwb_mutex, NULL) != 0) {
		free(buf);
		ps_write_buffer_leave_quiesce(pls);
		return NULL;
	}
	cds_lfht_node_init(&buf->pwb_ht_node);

	/*
	 * pwb_ht is set BEFORE the insert so pwb_release (the urcu_ref
	 * release callback) can call cds_lfht_del unconditionally on
	 * the eventual final put.  The lost-insert-race path below
	 * destroys our buffer directly without going through pwb_release,
	 * so the pwb_ht we set here is never observed in that case.
	 *
	 * urcu_ref_init starts at 1 (the table ref).  Bump for the
	 * per-op find ref the caller will use; refcount is now 2.
	 */
	buf->pwb_ht = pls->pls_write_buffer_ht;
	urcu_ref_init(&buf->pwb_ref);
	urcu_ref_get(&buf->pwb_ref);

	rcu_read_lock();
	/*
	 * cds_lfht_add_unique avoids a duplicate-insert race: if a
	 * concurrent thread inserted the same key just before us, we
	 * get back the existing node (with refcount we DID NOT take).
	 */
	struct cds_lfht_node *existing =
		cds_lfht_add_unique(pls->pls_write_buffer_ht, hash, pwb_match,
				    &key, &buf->pwb_ht_node);

	if (existing != &buf->pwb_ht_node) {
		/* Lost the insert race; existing node won. */
		struct ps_write_buffer *other = caa_container_of(
			existing, struct ps_write_buffer, pwb_ht_node);
		bool got = urcu_ref_get_unless_zero(&other->pwb_ref);

		rcu_read_unlock();
		/*
		 * Tear down our just-allocated buffer directly -- it
		 * was never inserted, so no cds_lfht_del needed and no
		 * RCU grace period required (no reader could have seen
		 * it).  Bypass pwb_release entirely.
		 */
		pthread_mutex_destroy(&buf->pwb_mutex);
		free(buf);

		if (!got) {
			/*
			 * Existing entry is also dying.  Release the
			 * caller's enter_quiesce reservation so the
			 * NULL contract is symmetric.
			 */
			ps_write_buffer_leave_quiesce(pls);
			return NULL;
		}
		return other;
	}
	rcu_read_unlock();

	return buf;
}

struct ps_write_buffer *
ps_write_buffer_lookup(struct ps_listener_state *pls,
		       const uint8_t stateid_other[PS_STATEID_OTHER_SIZE],
		       const uint8_t *upstream_fh, uint32_t upstream_fh_len)
{
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;
	struct ps_write_buffer *buf = NULL;
	struct pwb_match_arg key = {
		.stateid_other = stateid_other,
		.fh = upstream_fh,
		.fh_len = upstream_fh_len,
	};
	unsigned long hash;

	if (!pls || !pls->pls_write_buffer_ht || !stateid_other ||
	    !upstream_fh || upstream_fh_len == 0 ||
	    upstream_fh_len > PS_MAX_FH_SIZE) {
		if (pls)
			ps_write_buffer_leave_quiesce(pls);
		return NULL;
	}

	hash = pwb_hash(stateid_other, upstream_fh, upstream_fh_len);

	rcu_read_lock();
	cds_lfht_lookup(pls->pls_write_buffer_ht, hash, pwb_match, &key, &iter);
	node = cds_lfht_iter_get_node(&iter);
	if (node) {
		buf = caa_container_of(node, struct ps_write_buffer,
				       pwb_ht_node);
		if (!urcu_ref_get_unless_zero(&buf->pwb_ref))
			buf = NULL;
	}
	rcu_read_unlock();

	if (!buf)
		ps_write_buffer_leave_quiesce(pls);
	return buf;
}

struct ps_write_buffer *
ps_write_buffer_find_by_fh(struct ps_listener_state *pls,
			   const uint8_t *upstream_fh, uint32_t upstream_fh_len)
{
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;
	struct ps_write_buffer *found = NULL;

	if (!pls || !pls->pls_write_buffer_ht || !upstream_fh ||
	    upstream_fh_len == 0 || upstream_fh_len > PS_MAX_FH_SIZE) {
		if (pls)
			ps_write_buffer_leave_quiesce(pls);
		return NULL;
	}

	rcu_read_lock();
	cds_lfht_first(pls->pls_write_buffer_ht, &iter);
	while ((node = cds_lfht_iter_get_node(&iter)) != NULL) {
		struct ps_write_buffer *buf = caa_container_of(
			node, struct ps_write_buffer, pwb_ht_node);

		if (buf->pwb_upstream_fh_len == upstream_fh_len &&
		    memcmp(buf->pwb_upstream_fh, upstream_fh,
			   upstream_fh_len) == 0) {
			if (urcu_ref_get_unless_zero(&buf->pwb_ref)) {
				found = buf;
				break;
			}
			/*
			 * Lost race with teardown of this entry; keep
			 * scanning -- another writer's buffer for the
			 * same FH (Phase 4a documented limitation) may
			 * still be live.
			 */
		}
		cds_lfht_next(pls->pls_write_buffer_ht, &iter);
	}
	rcu_read_unlock();

	if (!found)
		ps_write_buffer_leave_quiesce(pls);
	return found;
}

void ps_write_buffer_release_find_ref(struct ps_write_buffer *buffer,
				      struct ps_listener_state *pls)
{
	if (!buffer)
		return;
	urcu_ref_put(&buffer->pwb_ref, pwb_release);
	ps_write_buffer_leave_quiesce(pls);
}

void ps_write_buffer_drop(struct ps_write_buffer *buffer,
			  struct ps_listener_state *pls)
{
	if (!buffer || !pls)
		return;

	/*
	 * Drop both refs: find (per-op) then table.  Release callback
	 * fires on the second put and does its own cds_lfht_del +
	 * call_rcu.  No explicit del here -- pwb_release owns that
	 * step.
	 */
	urcu_ref_put(&buffer->pwb_ref, pwb_release); /* find */
	urcu_ref_put(&buffer->pwb_ref, pwb_release); /* table */
	ps_write_buffer_leave_quiesce(pls);
}

/* ------------------------------------------------------------------ */
/* Dirty-bitmap public API (Phase 4b)                                  */
/* ------------------------------------------------------------------ */

int ps_write_buffer_set_geom(struct ps_write_buffer *buf,
			     const struct ps_write_buffer_geom *geom)
{
	uint64_t stripe;

	if (!buf || !geom)
		return -EINVAL;
	if (geom->pwbg_k == 0 || geom->pwbg_shard_size == 0)
		return -EINVAL;

	/* k * shard_size overflow check */
	stripe = (uint64_t)geom->pwbg_k * (uint64_t)geom->pwbg_shard_size;
	if (stripe > UINT32_MAX)
		return -EINVAL;

	if (buf->pwb_geom_set) {
		/* Idempotent: identical fields => no-op success. */
		if (buf->pwb_geom.pwbg_k == geom->pwbg_k &&
		    buf->pwb_geom.pwbg_m == geom->pwbg_m &&
		    buf->pwb_geom.pwbg_shard_size == geom->pwbg_shard_size)
			return 0;
		/*
		 * Different geometry after the buffer has been touched
		 * is a fatal mismatch; the caller (pipeline shim in
		 * slice 4b.2 onward) drops the buffer and replies
		 * NFS4ERR_STALE so the client rewrites under the new
		 * geometry.
		 */
		return -EINVAL;
	}

	buf->pwb_geom = *geom;
	buf->pwb_geom_set = true;
	return 0;
}

/*
 * Look up an existing dirty entry, or allocate one if absent.
 * Caller MUST hold buf->pwb_mutex.  Returns NULL on allocation
 * failure.
 *
 * The first dirty mark on a buffer allocates the dirty hash table
 * lazily; subsequent calls reuse it.  The hash table is destroyed
 * in pwb_rcu_free.
 */
static struct ps_dirty_stripe *pds_lookup_or_alloc(struct ps_write_buffer *buf,
						   uint32_t stripe_no)
{
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;
	struct ps_dirty_stripe *ds;
	uint32_t k;

	if (!buf->pwb_dirty_ht) {
		buf->pwb_dirty_ht = cds_lfht_new(PS_DIRTY_HT_BUCKETS_INIT,
						 PS_DIRTY_HT_BUCKETS_INIT,
						 /* max_nr_buckets */ 0,
						 CDS_LFHT_AUTO_RESIZE, NULL);
		if (!buf->pwb_dirty_ht)
			return NULL;
	}

	rcu_read_lock();
	cds_lfht_lookup(buf->pwb_dirty_ht, pds_hash(stripe_no), pds_match,
			&stripe_no, &iter);
	node = cds_lfht_iter_get_node(&iter);
	rcu_read_unlock();
	if (node)
		return caa_container_of(node, struct ps_dirty_stripe,
					pds_ht_node);

	ds = calloc(1, sizeof(*ds));
	if (!ds)
		return NULL;

	ds->pds_stripe_no = stripe_no;
	k = buf->pwb_geom.pwbg_k;
	ds->pds_partial_mask_bits = k;
	/*
	 * Allocate the partial mask up-front (tiny -- 1 byte for the
	 * typical k=4).  "Fully dirty" is signalled by promoting the
	 * pointer to NULL (free'd) once every bit becomes set.
	 * Allocating immediately removes the otherwise-ambiguous
	 * "NULL means uninitialised vs NULL means fully dirty" state
	 * a fresh entry would have.
	 */
	ds->pds_partial_mask = calloc(1, (k + 7u) / 8u);
	if (!ds->pds_partial_mask) {
		free(ds);
		return NULL;
	}
	cds_lfht_node_init(&ds->pds_ht_node);

	rcu_read_lock();
	/*
	 * cds_lfht_add_unique handles concurrent inserts -- though
	 * pwb_mutex serialises everything here, the API requires this
	 * shape and the cost is negligible.
	 */
	{
		struct cds_lfht_node *existing = cds_lfht_add_unique(
			buf->pwb_dirty_ht, pds_hash(stripe_no), pds_match,
			&stripe_no, &ds->pds_ht_node);

		if (existing != &ds->pds_ht_node) {
			rcu_read_unlock();
			free(ds->pds_partial_mask);
			free(ds);
			return caa_container_of(
				existing, struct ps_dirty_stripe, pds_ht_node);
		}
	}
	rcu_read_unlock();
	return ds;
}

int ps_write_buffer_mark_dirty(struct ps_write_buffer *buf, uint64_t offset,
			       uint32_t count)
{
	uint32_t stripe_size;
	uint32_t shard_size;
	uint32_t k;
	uint64_t end;
	uint32_t first_stripe;
	uint32_t last_stripe;
	uint32_t stripe_no;

	if (!buf || count == 0)
		return -EINVAL;
	if (!buf->pwb_geom_set)
		return -EINVAL;

	shard_size = buf->pwb_geom.pwbg_shard_size;
	k = buf->pwb_geom.pwbg_k;
	stripe_size = k * shard_size;

	end = offset + (uint64_t)count;
	first_stripe = (uint32_t)(offset / stripe_size);
	last_stripe = (uint32_t)((end - 1) / stripe_size);

	for (stripe_no = first_stripe; stripe_no <= last_stripe; stripe_no++) {
		struct ps_dirty_stripe *ds;
		uint64_t stripe_base = (uint64_t)stripe_no * stripe_size;
		uint64_t stripe_end = stripe_base + stripe_size;
		uint64_t hit_lo = offset > stripe_base ? offset : stripe_base;
		uint64_t hit_hi = end < stripe_end ? end : stripe_end;
		uint32_t first_shard;
		uint32_t last_shard;
		uint32_t shard;

		ds = pds_lookup_or_alloc(buf, stripe_no);
		if (!ds)
			return -ENOMEM;

		/*
		 * Stripe already fully dirty (every shard previously
		 * marked).  A further partial mark cannot demote it;
		 * skip the bit accounting entirely.
		 */
		if (!ds->pds_partial_mask)
			continue;

		first_shard = (uint32_t)((hit_lo - stripe_base) / shard_size);
		/*
		 * Inclusive last shard: hit_hi is one past the last
		 * written byte, so subtract 1 before dividing.
		 */
		last_shard =
			(uint32_t)((hit_hi - 1 - stripe_base) / shard_size);

		for (shard = first_shard; shard <= last_shard; shard++)
			pds_set_bit(ds->pds_partial_mask, shard);

		/*
		 * Promote to fully dirty (NULL mask, no RMW at flush)
		 * if this mark filled the last empty shard.
		 */
		if (pds_all_bits_set(ds->pds_partial_mask, k)) {
			free(ds->pds_partial_mask);
			ds->pds_partial_mask = NULL;
		}
	}

	return 0;
}

size_t ps_write_buffer_dirty_count(struct ps_write_buffer *buf)
{
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;
	size_t n = 0;

	if (!buf || !buf->pwb_dirty_ht)
		return 0;

	rcu_read_lock();
	cds_lfht_first(buf->pwb_dirty_ht, &iter);
	while ((node = cds_lfht_iter_get_node(&iter)) != NULL) {
		n++;
		cds_lfht_next(buf->pwb_dirty_ht, &iter);
	}
	rcu_read_unlock();
	return n;
}

struct ps_dirty_stripe *
ps_write_buffer_dirty_lookup(struct ps_write_buffer *buf, uint32_t stripe_no)
{
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;
	struct ps_dirty_stripe *ds = NULL;

	if (!buf || !buf->pwb_dirty_ht)
		return NULL;

	rcu_read_lock();
	cds_lfht_lookup(buf->pwb_dirty_ht, pds_hash(stripe_no), pds_match,
			&stripe_no, &iter);
	node = cds_lfht_iter_get_node(&iter);
	if (node)
		ds = caa_container_of(node, struct ps_dirty_stripe,
				      pds_ht_node);
	rcu_read_unlock();
	return ds;
}

bool ps_dirty_stripe_is_fully_dirty(const struct ps_dirty_stripe *ds)
{
	return ds && ds->pds_partial_mask == NULL;
}

bool ps_dirty_stripe_shard_is_dirty(const struct ps_dirty_stripe *ds,
				    uint32_t shard)
{
	if (!ds)
		return false;
	if (!ds->pds_partial_mask)
		return true; /* fully dirty */
	if (shard >= ds->pds_partial_mask_bits)
		return false;
	return pds_test_bit(ds->pds_partial_mask, shard);
}

/* ------------------------------------------------------------------ */
/* Whitebox helpers                                                    */
/* ------------------------------------------------------------------ */

size_t ps_write_buffer_table_count(struct ps_listener_state *pls)
{
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;
	size_t n = 0;

	if (!pls || !pls->pls_write_buffer_ht)
		return 0;

	rcu_read_lock();
	cds_lfht_first(pls->pls_write_buffer_ht, &iter);
	while ((node = cds_lfht_iter_get_node(&iter)) != NULL) {
		n++;
		cds_lfht_next(pls->pls_write_buffer_ht, &iter);
	}
	rcu_read_unlock();
	return n;
}
