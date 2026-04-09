/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * DS trust table -- pNFS flexfiles v2 tight coupling.
 *
 * Implements the in-memory trust table used by TRUST_STATEID,
 * REVOKE_STATEID, and BULK_REVOKE_STATEID.  CHUNK_WRITE and
 * CHUNK_READ validate against this table when the server operates
 * as a tightly-coupled DS.
 *
 * Ref-counting follows Rule 6 (patterns/ref-counting.md).
 * Hash table: cds_lfht, keyed by XXH3_64bits(te_other, 12).
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include <urcu.h>
#include <urcu/rculfhash.h>
#include <urcu/ref.h>
#include <xxhash.h>

#include "nfsv42_xdr.h"
#include "reffs/log.h"
#include "reffs/rcu.h"
#include "reffs/server.h"
#include "reffs/time.h"
#include "nfs4/trust_stateid.h"

/* ------------------------------------------------------------------ */
/* Internal state                                                      */

static struct cds_lfht *trust_ht;

/* Expiry reaper period: scan every 60 seconds. */
#define TRUST_REAPER_SCAN_SEC 60

static pthread_t trust_reaper_thread;
static pthread_mutex_t trust_reaper_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t trust_reaper_cv = PTHREAD_COND_INITIALIZER;
static _Atomic bool trust_reaper_running;

/* ------------------------------------------------------------------ */
/* Hash and match                                                      */

static unsigned long trust_hash(const uint8_t *other)
{
	return (unsigned long)XXH3_64bits(other, NFS4_OTHER_SIZE);
}

static int trust_match(struct cds_lfht_node *node, const void *key)
{
	const struct trust_entry *te =
		caa_container_of(node, struct trust_entry, te_ht_node);
	return memcmp(te->te_other, key, NFS4_OTHER_SIZE) == 0;
}

/* ------------------------------------------------------------------ */
/* Lifecycle callbacks                                                 */

static void trust_entry_rcu_free(struct rcu_head *head)
{
	struct trust_entry *te =
		caa_container_of(head, struct trust_entry, te_rcu);
	free(te);
}

static void trust_entry_release(struct urcu_ref *ref)
{
	struct trust_entry *te =
		caa_container_of(ref, struct trust_entry, te_ref);

	/*
	 * Rule 6: remove from hash table (idempotent if already removed)
	 * before scheduling the RCU-deferred free.
	 *
	 * trust_ht may be NULL if trust_table_fini() ran and a caller
	 * held a find ref across the drain.  Guard to avoid the NULL
	 * dereference; cds_lfht_del is idempotent for already-removed
	 * nodes, so fini's drain already removed the entry.
	 */
	rcu_read_lock();
	if (trust_ht)
		cds_lfht_del(trust_ht, &te->te_ht_node);
	rcu_read_unlock();

	call_rcu(&te->te_rcu, trust_entry_rcu_free);
}

void trust_entry_put(struct trust_entry *te)
{
	if (te)
		urcu_ref_put(&te->te_ref, trust_entry_release);
}

void trust_stateid_renewal_scan(uint32_t lease_sec)
{
	if (!trust_ht)
		return;

	uint64_t now = reffs_now_ns();

	/*
	 * Renewal threshold: renew when remaining lifetime < lease_sec / 2.
	 * With lease_sec=0 the threshold is 0, so no entry qualifies.
	 */
	uint64_t threshold_ns = (uint64_t)lease_sec * 1000000000ULL / 2;
	uint64_t new_lifetime_ns = (uint64_t)lease_sec * 1000000000ULL;

	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;

	rcu_read_lock();
	cds_lfht_first(trust_ht, &iter);
	while ((node = cds_lfht_iter_get_node(&iter)) != NULL) {
		struct trust_entry *te =
			caa_container_of(node, struct trust_entry, te_ht_node);

		/* Advance before any put -- Rule 6 reaper pattern. */
		cds_lfht_next(trust_ht, &iter);

		if (!urcu_ref_get_unless_zero(&te->te_ref))
			continue;

		uint64_t exp = atomic_load_explicit(&te->te_expire_ns,
						    memory_order_acquire);

		/*
		 * Skip entries that are not set, already expired (the expiry
		 * pass handles those), or still far enough away.
		 */
		if (exp == 0 || exp <= now || exp - now >= threshold_ns) {
			trust_entry_put(te);
			continue;
		}

		/*
		 * Entry is nearing expiry: extend.  We do not check client
		 * liveness here; the lease reaper calls bulk_revoke when a
		 * client expires, which removes all of that client's entries.
		 * The window between client expiry and bulk_revoke is at most
		 * one scan interval -- a briefly stale entry that gets one
		 * extra renewal is harmless.
		 *
		 * NOT_NOW_BROWN_COW: in the multi-machine (MDS != DS) case,
		 * renewal should be driven by the MDS re-issuing TRUST_STATEID
		 * before expiry (design/trust-stateid.md Step 2.8).  This
		 * DS-side extension is correct only for combined mode where
		 * the MDS and DS share a process.
		 */
		uint64_t new_exp = now + new_lifetime_ns;
		atomic_store_explicit(&te->te_expire_ns, new_exp,
				      memory_order_release);

		TRACE("trust_renewal: extended stateid other=%02x%02x... +%us",
		      te->te_other[0], te->te_other[1], lease_sec);

		trust_entry_put(te);
	}
	rcu_read_unlock();
}

/* ------------------------------------------------------------------ */
/* Reaper thread                                                       */

static void *trust_reaper_thread_fn(void *arg __attribute__((unused)))
{
	rcu_register_thread();

	while (atomic_load_explicit(&trust_reaper_running,
				    memory_order_relaxed)) {
		struct timespec ts;

		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_sec += TRUST_REAPER_SCAN_SEC;

		pthread_mutex_lock(&trust_reaper_mtx);
		pthread_cond_timedwait(&trust_reaper_cv, &trust_reaper_mtx,
				       &ts);
		pthread_mutex_unlock(&trust_reaper_mtx);

		if (!atomic_load_explicit(&trust_reaper_running,
					  memory_order_relaxed))
			break;

		if (!trust_ht)
			continue;

		uint64_t now = reffs_now_ns();
		struct cds_lfht_iter iter;
		struct cds_lfht_node *node;

		/*
		 * Advance iterator BEFORE put (Rule 6 reaper pattern):
		 * put() may invoke the release callback synchronously, which
		 * calls cds_lfht_del, which would corrupt the iterator state.
		 */
		rcu_read_lock();
		cds_lfht_first(trust_ht, &iter);
		while ((node = cds_lfht_iter_get_node(&iter)) != NULL) {
			struct trust_entry *te = caa_container_of(
				node, struct trust_entry, te_ht_node);

			cds_lfht_next(trust_ht, &iter);

			if (!urcu_ref_get_unless_zero(&te->te_ref))
				continue;

			uint64_t exp = atomic_load_explicit(
				&te->te_expire_ns, memory_order_acquire);

			if (exp == 0 || now < exp) {
				trust_entry_put(te);
				continue;
			}

			TRACE("trust_reaper: expiring stateid other=%02x%02x...",
			      te->te_other[0], te->te_other[1]);

			/* Drop find ref + creation ref to destroy the entry. */
			trust_entry_put(te);
			trust_entry_put(te);
		}
		rcu_read_unlock();

		/*
		 * Renewal pass: extend entries nearing expiry.  Uses the
		 * server's configured lease time; if server_state is not
		 * available (startup or shutdown race), skip this cycle.
		 */
		struct server_state *ss = server_state_find();
		if (ss) {
			uint32_t lease_sec = server_lease_time(ss);

			server_state_put(ss);
			trust_stateid_renewal_scan(lease_sec);
		}
	}

	rcu_unregister_thread();
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */

int trust_stateid_init(void)
{
	trust_ht = cds_lfht_new(16, 16, 0, CDS_LFHT_AUTO_RESIZE, NULL);
	if (!trust_ht) {
		LOG("trust_stateid_init: cds_lfht_new failed");
		return -1;
	}

	atomic_store_explicit(&trust_reaper_running, true,
			      memory_order_relaxed);
	int ret = pthread_create(&trust_reaper_thread, NULL,
				 trust_reaper_thread_fn, NULL);
	if (ret != 0) {
		LOG("trust_stateid_init: pthread_create failed: %d", ret);
		cds_lfht_destroy(trust_ht, NULL);
		trust_ht = NULL;
		return -ret;
	}

	return 0;
}

void trust_stateid_fini(void)
{
	if (!trust_ht)
		return;

	atomic_store_explicit(&trust_reaper_running, false,
			      memory_order_relaxed);
	pthread_cond_signal(&trust_reaper_cv);
	pthread_join(trust_reaper_thread, NULL);

	/*
	 * Drain: drop the creation ref of each entry (Rule 6).
	 * The reaper has been joined so no new find refs are being taken.
	 * RPC processing must have stopped (rpc_program_handler_put) before
	 * this point, so the only outstanding refs are creation refs -- one
	 * put per entry is sufficient.  Any find ref taken before the RPC
	 * handler was torn down and still outstanding will call
	 * trust_entry_release, which now guards on trust_ht != NULL.
	 *
	 * Advance the iterator before put because put may trigger
	 * trust_entry_release, which calls cds_lfht_del + call_rcu.
	 */
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;

	rcu_read_lock();
	cds_lfht_first(trust_ht, &iter);
	while ((node = cds_lfht_iter_get_node(&iter)) != NULL) {
		struct trust_entry *te =
			caa_container_of(node, struct trust_entry, te_ht_node);

		cds_lfht_next(trust_ht, &iter);
		trust_entry_put(te);
	}
	rcu_read_unlock();

	synchronize_rcu();
	cds_lfht_destroy(trust_ht, NULL);
	trust_ht = NULL;
}

int trust_stateid_register(const stateid4 *stateid, uint64_t ino,
			   clientid4 clientid, layoutiomode4 iomode,
			   uint64_t expire_mono_ns, const char *principal)
{
	if (!trust_ht)
		return -EINVAL;

	unsigned long hash = trust_hash((const uint8_t *)stateid->other);

	/*
	 * Check for existing entry first (idempotent MDS retry).
	 * If found, update in-place (expiry + flags).
	 */
	struct cds_lfht_iter iter;
	struct cds_lfht_node *found;

	rcu_read_lock();
	cds_lfht_lookup(trust_ht, hash, trust_match, stateid->other, &iter);
	found = cds_lfht_iter_get_node(&iter);

	if (found) {
		struct trust_entry *te =
			caa_container_of(found, struct trust_entry, te_ht_node);

		if (urcu_ref_get_unless_zero(&te->te_ref)) {
			atomic_store_explicit(&te->te_expire_ns, expire_mono_ns,
					      memory_order_relaxed);
			atomic_store_explicit(&te->te_flags, TRUST_ACTIVE,
					      memory_order_release);
			/*
			 * NOT_NOW_BROWN_COW: te_iomode is a plain field
			 * updated here while the entry is live in the
			 * hash table.  If CHUNK_WRITE ever enforces
			 * read-only constraints via te_iomode, convert
			 * this field to _Atomic layoutiomode4 and use
			 * atomic_store_explicit(memory_order_release).
			 */
			te->te_iomode = iomode;
			rcu_read_unlock();
			trust_entry_put(te);
			return 0;
		}
		/* Entry is dying; fall through to allocate a replacement. */
	}
	rcu_read_unlock();

	struct trust_entry *te = calloc(1, sizeof(*te));

	if (!te)
		return -ENOMEM;

	memcpy(te->te_other, stateid->other, NFS4_OTHER_SIZE);
	te->te_ino = ino;
	te->te_clientid = clientid;
	te->te_iomode = iomode;
	atomic_store_explicit(&te->te_expire_ns, expire_mono_ns,
			      memory_order_relaxed);
	atomic_store_explicit(&te->te_flags, TRUST_ACTIVE,
			      memory_order_release);

	if (principal && *principal) {
		strncpy(te->te_principal, principal, TRUST_PRINCIPAL_MAX - 1);
		te->te_principal[TRUST_PRINCIPAL_MAX - 1] = '\0';
	}

	urcu_ref_init(&te->te_ref);

	rcu_read_lock();
	cds_lfht_add(trust_ht, hash, &te->te_ht_node);
	rcu_read_unlock();

	return 0;
}

void trust_stateid_revoke(const stateid4 *stateid)
{
	if (!trust_ht)
		return;

	unsigned long hash = trust_hash((const uint8_t *)stateid->other);
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;

	rcu_read_lock();
	cds_lfht_lookup(trust_ht, hash, trust_match, stateid->other, &iter);
	node = cds_lfht_iter_get_node(&iter);

	if (node) {
		struct trust_entry *te =
			caa_container_of(node, struct trust_entry, te_ht_node);

		if (urcu_ref_get_unless_zero(&te->te_ref)) {
			/*
			 * Explicit removal: unhash now so it is no longer
			 * findable; drop find ref + creation ref.
			 */
			cds_lfht_del(trust_ht, &te->te_ht_node);
			rcu_read_unlock();
			trust_entry_put(te); /* find ref */
			trust_entry_put(te); /* creation ref */
			return;
		}
	}
	rcu_read_unlock();
}

void trust_stateid_bulk_revoke(clientid4 clientid)
{
	if (!trust_ht)
		return;

	bool clear_all = (clientid == 0);
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;

	/*
	 * We must advance the iterator before calling put() (Rule 6).
	 * Collect matching entries' find refs inside rcu_read_lock, then
	 * drop creation refs outside.
	 *
	 * Because we need to advance before put and cds_lfht_del invalidates
	 * the current position, the correct pattern is:
	 *   1. get_node + advance in a single pass under rcu_read_lock
	 *   2. for entries to revoke: del + put outside lock
	 *
	 * We use a batch array to collect entries, then process them after
	 * dropping the read lock.  Batch size 64 is sufficient for any
	 * reasonable number of stateids per client.
	 */
#define BULK_BATCH 64
	struct trust_entry *batch[BULK_BATCH];
	int n;

restart:
	n = 0;

	rcu_read_lock();
	cds_lfht_first(trust_ht, &iter);
	while ((node = cds_lfht_iter_get_node(&iter)) != NULL) {
		struct trust_entry *te =
			caa_container_of(node, struct trust_entry, te_ht_node);

		cds_lfht_next(trust_ht, &iter);

		if (!clear_all && te->te_clientid != clientid)
			continue;

		if (!urcu_ref_get_unless_zero(&te->te_ref))
			continue;

		batch[n++] = te;

		if (n == BULK_BATCH) {
			/*
			 * Batch full; process before continuing so we don't
			 * need an unbounded array.
			 */
			for (int i = 0; i < n; i++) {
				cds_lfht_del(trust_ht, &batch[i]->te_ht_node);
			}
			rcu_read_unlock();
			for (int i = 0; i < n; i++) {
				trust_entry_put(batch[i]); /* find ref */
				trust_entry_put(batch[i]); /* creation ref */
			}
			goto restart;
		}
	}

	for (int i = 0; i < n; i++)
		cds_lfht_del(trust_ht, &batch[i]->te_ht_node);
	rcu_read_unlock();

	for (int i = 0; i < n; i++) {
		trust_entry_put(batch[i]); /* find ref */
		trust_entry_put(batch[i]); /* creation ref */
	}
#undef BULK_BATCH
}

struct trust_entry *trust_stateid_find(const stateid4 *stateid)
{
	unsigned long hash = trust_hash((const uint8_t *)stateid->other);
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;
	struct trust_entry *te = NULL;

	if (!trust_ht)
		return NULL;

	rcu_read_lock();
	cds_lfht_lookup(trust_ht, hash, trust_match, stateid->other, &iter);
	node = cds_lfht_iter_get_node(&iter);

	if (node) {
		struct trust_entry *tmp =
			caa_container_of(node, struct trust_entry, te_ht_node);

		if (urcu_ref_get_unless_zero(&tmp->te_ref))
			te = tmp;
	}
	rcu_read_unlock();

	return te;
}

uint64_t trust_stateid_convert_expire(const nfstime4 *expire,
				      uint64_t now_wall_ns,
				      uint64_t now_mono_ns)
{
	/* RFC 8881: nseconds must be < 1e9 */
	if (expire->nseconds >= 1000000000u)
		return 0;

	/*
	 * Reject negative seconds (seconds = -1 would wrap to a ~292-year
	 * deadline when cast to uint64_t, bypassing the reaper).  Also
	 * reject seconds = 0 (the Unix epoch) as degenerate -- any real
	 * lease expiry is well above the epoch.
	 */
	if (expire->seconds <= 0)
		return 0;

	uint64_t expire_wall_ns =
		(uint64_t)expire->seconds * 1000000000ULL + expire->nseconds;

	/*
	 * Expiry in the past is invalid: the MDS must have sent a
	 * stale or bogus deadline.  Return 0 so the op handler rejects
	 * with NFS4ERR_INVAL rather than registering an already-expired
	 * entry that CHUNK ops would immediately reject.
	 */
	if (expire_wall_ns <= now_wall_ns)
		return 0;

	uint64_t remaining_ns = expire_wall_ns - now_wall_ns;

	/* Overflow guard: cap at ~292 years. */
	if (remaining_ns > (uint64_t)UINT64_MAX - now_mono_ns)
		return UINT64_MAX;

	return now_mono_ns + remaining_ns;
}
