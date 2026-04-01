/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Lease reaper thread.
 *
 * Periodically scans the client hash table and expires clients whose
 * last SEQUENCE timestamp exceeds the lease timeout.  RFC 5661 §8.3
 * requires that the server expire client state when the client has not
 * renewed its lease within the lease period.
 *
 * The thread follows the cb_timeout.c pattern: an atomic running flag
 * checked each iteration, with pthread_join for clean shutdown.
 */

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

/*
 * Scan interval: check once per 30 seconds.  Lease periods are
 * typically 90 seconds, so a 30-second scan catches expired clients
 * within one-third of a lease period.
 */
#define LEASE_SCAN_INTERVAL_SEC 30

/* Expire after 1.5x the lease time with no renewal. */
#define LEASE_EXPIRE_FACTOR_NUM 3
#define LEASE_EXPIRE_FACTOR_DEN 2

/* ------------------------------------------------------------------ */
/* Global state                                                        */
/* ------------------------------------------------------------------ */

static pthread_t lease_reaper_thread;
static _Atomic uint32_t lease_reaper_running;
static pthread_mutex_t lease_reaper_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t lease_reaper_cv = PTHREAD_COND_INITIALIZER;

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
		pthread_cond_timedwait(&lease_reaper_cv, &lease_reaper_mtx,
				       &ts);
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
				 * (RFC 8881 §18.35.4); confirmed clients
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
	atomic_store_explicit(&lease_reaper_running, 0, memory_order_relaxed);
	pthread_cond_signal(&lease_reaper_cv);
	pthread_join(lease_reaper_thread, NULL);
}
