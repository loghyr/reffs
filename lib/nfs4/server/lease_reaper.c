/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Lease reaper thread.
 *
 * Periodically scans the client hash table and expires clients whose
 * last SEQUENCE timestamp exceeds the lease timeout.  RFC 5661 S8.3
 * requires that the server expire client state when the client has not
 * renewed its lease within the lease period.
 *
 * The thread follows the cb_timeout.c pattern: an atomic running flag
 * checked each iteration, with pthread_join for clean shutdown.
 */

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

#include <urcu/rculfhash.h>

#include "nfsv42_xdr.h"
#include "reffs/log.h"
#include "reffs/rcu.h"
#include "reffs/client.h"
#include "reffs/server.h"
#include "reffs/time.h"
#include "nfs4/client.h"
#include "nfs4/client_persist.h"
#include "nfs4/lease_reaper.h"
#include "nfs4/session.h"

/*
 * Scan interval: check once per second.  The same loop now sweeps
 * both client lease expiry (slow change, 1.5x lease typically 135s
 * away) and probe-session expiry (issue #64, 2s after creation),
 * so the interval is bounded by the faster of the two.  The client
 * scan is a cheap rcu hash walk on an idle namespace -- doing it
 * 30x more often than before adds negligible load.
 */
#define LEASE_SCAN_INTERVAL_SEC 1

/* Expire after 1.5x the lease time with no renewal. */
#define LEASE_EXPIRE_FACTOR_NUM 3
#define LEASE_EXPIRE_FACTOR_DEN 2

/*
 * Probe-session reaping (issue #64): a session that received
 * CREATE_SESSION but has never been observed in SEQUENCE traffic
 * is almost certainly a Linux NFS-client trunking-probe leftover
 * (nfs4_pnfs_ds_connect emits 2x CREATE_SESSION per DS but only
 * 1x DESTROY_SESSION).  After PROBE_REAP_AGE_NS of inactivity we
 * unhash it so DESTROY_CLIENTID can pass the nc_session_count
 * gate without triggering the kernel's 10x/1Hz retry storm.
 *
 * 2 seconds was chosen to bound the bench-cell penalty: the
 * kernel begins DESTROY_CLIENTID retries ~2s after the probe is
 * created, so the probe must be reaped before the 3rd retry.
 * Legitimate slow clients still get SEQUENCE within 2s of
 * CREATE_SESSION in any realistic deployment; if not, they would
 * fail their own session-establishment timeout long before the
 * server-side reaper fires.
 */
#define PROBE_REAP_AGE_NS (2ULL * 1000000000ULL)

/* ------------------------------------------------------------------ */
/* Global state                                                        */
/* ------------------------------------------------------------------ */

static pthread_t lease_reaper_thread;
static _Atomic uint32_t lease_reaper_running;
static pthread_mutex_t lease_reaper_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t lease_reaper_cv = PTHREAD_COND_INITIALIZER;

/* ------------------------------------------------------------------ */
/* Probe-session sweep                                                 */
/* ------------------------------------------------------------------ */

unsigned int lease_reaper_sweep_probe_sessions(struct server_state *ss,
					       uint64_t now_ns)
{
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;
	unsigned int reaped = 0;

	if (!ss || !ss->ss_session_ht)
		return 0;

	rcu_read_lock();
	cds_lfht_first(ss->ss_session_ht, &iter);
	while ((node = cds_lfht_iter_get_node(&iter)) != NULL) {
		struct nfs4_session *ns =
			caa_container_of(node, struct nfs4_session, ns_node);
		uint64_t create = ns->ns_create_ns;
		uint64_t last = atomic_load_explicit(&ns->ns_last_seq_ns,
						     memory_order_acquire);

		/*
		 * Advance the iterator BEFORE any unhash (patterns/
		 * rcu-violations.md Pattern 7 / ref-counting.md Rule 6):
		 * nfs4_session_unhash drops the table's ref which may
		 * fire the release callback synchronously, invalidating
		 * the current node.
		 */
		cds_lfht_next(ss->ss_session_ht, &iter);

		/* Used session -- last differs from create after any
		 * SEQUENCE has bumped it.  Leave alone. */
		if (last != create)
			continue;
		/* Young session -- give it more time to actually be used
		 * before treating it as a probe.  Also guards against
		 * pathological clocks where now_ns < create. */
		if (create == 0 || now_ns <= create ||
		    now_ns - create < PROBE_REAP_AGE_NS)
			continue;

		TRACE("lease_reaper: reaping probe session age=%" PRIu64 "ms",
		      (uint64_t)((now_ns - create) / 1000000ULL));
		nfs4_session_unhash(ss, ns);
		reaped++;
	}
	rcu_read_unlock();

	return reaped;
}

/* ------------------------------------------------------------------ */
/* Thread function                                                     */
/* ------------------------------------------------------------------ */

static void *lease_reaper_thread_fn(void *arg __attribute__((unused)))
{
	rcu_register_thread();

	while (atomic_load_explicit(&lease_reaper_running,
				    memory_order_relaxed)) {
		struct timespec ts;

		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_sec += LEASE_SCAN_INTERVAL_SEC;

		pthread_mutex_lock(&lease_reaper_mtx);
		/* Loop to handle spurious wakeups: recompute the deadline and
		 * retry the wait until the full interval has elapsed. */
		while (atomic_load_explicit(&lease_reaper_running,
					    memory_order_relaxed)) {
			struct timespec now;
			int rc = pthread_cond_timedwait(&lease_reaper_cv,
							&lease_reaper_mtx, &ts);
			if (rc == ETIMEDOUT)
				break;
			clock_gettime(CLOCK_REALTIME, &now);
			if (now.tv_sec >= ts.tv_sec)
				break;
		}
		pthread_mutex_unlock(&lease_reaper_mtx);

		if (!atomic_load_explicit(&lease_reaper_running,
					  memory_order_relaxed))
			break;

		struct server_state *ss = server_state_find();

		if (!ss)
			continue;

		/* Don't expire clients during grace. */
		if (server_in_grace(ss)) {
			server_state_put(ss);
			continue;
		}

		if (!ss->ss_client_ht) {
			server_state_put(ss);
			continue;
		}

		uint64_t now = reffs_now_ns();
		uint32_t lease_sec = server_lease_time(ss);
		uint64_t expire_ns = (uint64_t)lease_sec *
				     LEASE_EXPIRE_FACTOR_NUM * 1000000000ULL /
				     LEASE_EXPIRE_FACTOR_DEN;

		/* Probe-session sweep (issue #64).  See helper for the
		 * detection rule.  Cheap when the table is empty; the
		 * 1-second tick is bounded by this sweep, not the
		 * client-lease sweep below. */
		if (ss->ss_session_ht)
			lease_reaper_sweep_probe_sessions(ss, now);

		struct cds_lfht_iter iter;
		struct cds_lfht_node *node;
		bool expired_one;

		/*
		 * Restart-after-expire loop: nfs4_client_expire drops
		 * the RCU read lock and modifies the hash table, so
		 * restart iteration from the beginning after each expiry.
		 */
		do {
			expired_one = false;
			rcu_read_lock();
			cds_lfht_first(ss->ss_client_ht, &iter);
			while ((node = cds_lfht_iter_get_node(&iter)) != NULL) {
				struct client *client = caa_container_of(
					node, struct client, c_node);
				struct nfs4_client *nc = client_to_nfs4(client);
				uint64_t last =
					__atomic_load_n(&nc->nc_last_renew_ns,
							__ATOMIC_ACQUIRE);

				/*
				 * Unconfirmed clients expire after 1x lease
				 * (RFC 8881 S18.35.4); confirmed clients
				 * get 1.5x lease as breathing room.
				 */
				uint64_t client_expire =
					nc->nc_confirmed ?
						expire_ns :
						(uint64_t)lease_sec *
							1000000000ULL;

				if (last == 0 || now <= last ||
				    now - last < client_expire) {
					cds_lfht_next(ss->ss_client_ht, &iter);
					continue;
				}

				TRACE("lease_reaper: expiring client "
				      "slot=%" PRIu64 " idle=%" PRIu64 "s",
				      client->c_id,
				      (uint64_t)((now - last) / 1000000000ULL));

				if (!client_get(client)) {
					cds_lfht_next(ss->ss_client_ht, &iter);
					continue;
				}
				rcu_read_unlock();

				nfs4_client_expire(ss, nc);
				expired_one = true;
				break;
			}
			if (!expired_one)
				rcu_read_unlock();
		} while (expired_one);

		server_state_put(ss);
	}

	rcu_unregister_thread();
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int lease_reaper_init(void)
{
	atomic_store_explicit(&lease_reaper_running, 1, memory_order_relaxed);

	return pthread_create(&lease_reaper_thread, NULL,
			      lease_reaper_thread_fn, NULL);
}

void lease_reaper_fini(void)
{
	/*
	 * Hold the mutex while clearing the flag and signalling so the
	 * wakeup is never lost: if the thread is between its running-flag
	 * check and pthread_cond_timedwait, it still holds the mutex, so
	 * our lock blocks until the thread has entered the wait -- and our
	 * signal is then guaranteed to reach it.  Without the lock the
	 * signal fires into the void and the thread sleeps for the full
	 * LEASE_SCAN_INTERVAL_SEC (30 s), causing intermittent test timeouts.
	 */
	pthread_mutex_lock(&lease_reaper_mtx);
	atomic_store_explicit(&lease_reaper_running, 0, memory_order_relaxed);
	pthread_cond_signal(&lease_reaper_cv);
	pthread_mutex_unlock(&lease_reaper_mtx);
	pthread_join(lease_reaper_thread, NULL);
}
