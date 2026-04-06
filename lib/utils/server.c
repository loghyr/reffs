/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

#include "reffs/cmp.h"
#include "reffs/rcu.h"
#include "reffs/log.h"
#include "reffs/server_persist.h"
#include "reffs/server.h"
#include "reffs/client_persist.h"

#define DEFAULT_LEASE_TIME 45U /* seconds, per RFC 8881 s2.10.6 */
#define DEFAULT_GRACE_TIME 45U

static _Atomic(struct server_state *) current_server_state;

/* ------------------------------------------------------------------ */
/* State machine                                                       */

const char *server_lifecycle_name(enum server_lifecycle lc)
{
	switch (lc) {
	case SERVER_BOOTING:
		return "booting";
	case SERVER_GRACE_STARTED:
		return "grace_started";
	case SERVER_IN_GRACE:
		return "in_grace";
	case SERVER_GRACE_ENDED:
		return "grace_ended";
	case SERVER_SHUTTING_DOWN:
		return "shutting_down";
	default:
		return "unknown";
	}
}

static void server_lifecycle_set(struct server_state *ss,
				 enum server_lifecycle next)
{
	TRACE("server lifecycle: %s -> %s",
	      server_lifecycle_name(atomic_load_explicit(&ss->ss_lifecycle,
							 memory_order_relaxed)),
	      server_lifecycle_name(next));

	atomic_store_explicit(&ss->ss_lifecycle, next, memory_order_release);
}

/* ------------------------------------------------------------------ */
/* Ref counting                                                        */

static void server_state_free(struct urcu_ref *ref)
{
	struct server_state *ss =
		caa_container_of(ref, struct server_state, ss_ref);

	if (ss->ss_incarnations)
		cds_lfht_destroy(ss->ss_incarnations, NULL);

	if (ss->ss_session_ht)
		cds_lfht_destroy(ss->ss_session_ht, NULL);

	if (ss->ss_client_ht)
		cds_lfht_destroy(ss->ss_client_ht, NULL);

	if (ss->ss_persist_ops && ss->ss_persist_ops->fini)
		ss->ss_persist_ops->fini(ss->ss_persist_ctx);
	ss->ss_persist_ctx = NULL;

	free(ss->ss_owner_id);
	free(ss->ss_state_dir);
	free(ss);
}

struct server_state *server_state_get(struct server_state *ss)
{
	if (!ss)
		return NULL;

	if (server_shutting_down(ss))
		return NULL;

	if (!urcu_ref_get_unless_zero(&ss->ss_ref))
		return NULL;

	/*
         * Transition GRACE_STARTED -> IN_GRACE on first successful
         * get from the protocol layer -- grace period is now open.
         */
	if (atomic_load_explicit(&ss->ss_lifecycle, memory_order_acquire) ==
	    SERVER_GRACE_STARTED)
		server_lifecycle_set(ss, SERVER_IN_GRACE);

	return ss;
}

void server_state_put(struct server_state *ss)
{
	if (!ss)
		return;

	urcu_ref_put(&ss->ss_ref, server_state_free);
}

struct server_state *server_state_find(void)
{
	struct server_state *ss = atomic_load_explicit(&current_server_state,
						       memory_order_acquire);
	return server_state_get(ss);
}

/* ------------------------------------------------------------------ */
/* Grace period                                                        */

/*
 * grace_timer_thread -- fires after 2 * ss_grace_time seconds and ends grace.
 *
 * Clients are given twice the grace period to complete reclaim.  This will
 * eventually be driven by a config value; for now it is hard-coded as 2x.
 *
 * Sleeps in 1-second increments so it wakes promptly on shutdown.
 * Does NOT hold an extra ref on ss; server_state_fini() joins the thread
 * before dropping the final ref, so ss is guaranteed live for the
 * thread's lifetime.
 */
static void *grace_timer_thread(void *arg)
{
	struct server_state *ss = arg;
	struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
	uint32_t grace_deadline = 2 * ss->ss_grace_time;

	for (uint32_t elapsed = 0; elapsed < grace_deadline; elapsed++) {
		clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);

		if (server_shutting_down(ss))
			return NULL;

		/* Early exit if all clients already reclaimed. */
		enum server_lifecycle lc = atomic_load_explicit(
			&ss->ss_lifecycle, memory_order_acquire);
		if (lc >= SERVER_GRACE_ENDED)
			return NULL;
	}

	server_grace_end(ss);
	return NULL;
}

void server_grace_start(struct server_state *ss)
{
	enum server_lifecycle lc =
		atomic_load_explicit(&ss->ss_lifecycle, memory_order_acquire);

	if (lc != SERVER_BOOTING) {
		LOG("server_grace_start: unexpected state %s",
		    server_lifecycle_name(lc));
		return;
	}

	clock_gettime(CLOCK_MONOTONIC, &ss->ss_grace_start);
	server_lifecycle_set(ss, SERVER_GRACE_STARTED);

	if (pthread_create(&ss->ss_grace_thread, NULL, grace_timer_thread,
			   ss) != 0) {
		LOG("server_grace_start: failed to create grace timer thread; "
		    "grace will not expire automatically");
		ss->ss_grace_thread = 0;
	}
}

void server_grace_end(struct server_state *ss)
{
	enum server_lifecycle lc =
		atomic_load_explicit(&ss->ss_lifecycle, memory_order_acquire);

	if (lc != SERVER_IN_GRACE && lc != SERVER_GRACE_STARTED) {
		LOG("server_grace_end: unexpected state %s",
		    server_lifecycle_name(lc));
		return;
	}

	server_lifecycle_set(ss, SERVER_GRACE_ENDED);
}

void server_reclaim_complete(struct server_state *ss)
{
	uint32_t prev =
		__atomic_fetch_sub(&ss->ss_unreclaimed, 1, __ATOMIC_ACQ_REL);
	if (prev == 1)
		server_grace_end(ss);
}

/* ------------------------------------------------------------------ */
/* Client slot allocation                                              */

uint32_t server_alloc_client_slot(struct server_state *ss)
{
	uint32_t slot;

	slot = __atomic_fetch_add(&ss->ss_persist.sps_slot_next, 1,
				  __ATOMIC_RELAXED);
	if (slot == UINT32_MAX) {
		/* Wrap -- 4 billion clients is unreachable but handle it. */
		LOG("server_alloc_client_slot: slot overflow");
		return UINT32_MAX;
	}

	/*
         * Persist the incremented slot_next before returning so the
         * slot survives a crash between alloc and client confirmation.
         */
	if (ss->ss_persist_ops->server_state_save(ss->ss_persist_ctx,
						  &ss->ss_persist)) {
		LOG("server_alloc_client_slot: failed to persist slot %u",
		    slot);
		/* Roll back and signal error. */
		__atomic_fetch_sub(&ss->ss_persist.sps_slot_next, 1,
				   __ATOMIC_RELAXED);
		return UINT32_MAX;
	}

	return slot;
}

/* ------------------------------------------------------------------ */
/* Init / fini                                                         */

struct server_state *
server_state_init(const char *state_path, int port,
		  enum reffs_text_case case_mode,
		  enum reffs_storage_type storage_type
		  __attribute__((unused)) /* used only with HAVE_ROCKSDB */
)
{
	struct server_state *ss;
	uint8_t prev_clean_shutdown;
	int ret;

	ss = calloc(1, sizeof(*ss));
	if (!ss)
		return NULL;

	if (state_path) {
		ss->ss_state_dir = strdup(state_path);
		if (!ss->ss_state_dir) {
			free(ss);
			return NULL;
		}
	}

	ss->ss_port = port;
	ss->ss_case = case_mode;
	reffs_case_set(case_mode);

	/*
	 * Set up persistence dispatch based on configured backend type.
	 * RocksDB --> namespace DB; everything else --> flatfile.
	 */
#ifdef HAVE_ROCKSDB
	if (storage_type == REFFS_STORAGE_ROCKSDB && state_path) {
		ret = rocksdb_namespace_init(state_path, &ss->ss_persist_ops,
					     &ss->ss_persist_ctx);
		if (ret) {
			LOG("server_state_init: rocksdb namespace init failed: %d",
			    ret);
			goto err_path;
		}
	} else
#endif
	{
		ss->ss_persist_ops = flatfile_persist_ops_get();
		if (state_path) {
			ss->ss_persist_ctx = strdup(state_path);
			if (!ss->ss_persist_ctx)
				goto err_path;
		}
	}

	/* Load or create persistent state. */
	ret = ss->ss_persist_ops->server_state_load(ss->ss_persist_ctx,
						    &ss->ss_persist);
	if (ret == -ENOENT) {
		/* Fresh start -- initialise defaults. */
		ss->ss_persist.sps_magic = REFFS_SERVER_STATE_MAGIC;
		ss->ss_persist.sps_version = REFFS_SERVER_STATE_VERSION;
		ss->ss_persist.sps_boot_seq = 0;
		ss->ss_persist.sps_clean_shutdown = 1;
		ss->ss_persist.sps_slot_next = 1; /* 0 is sentinel */
		ss->ss_persist.sps_lease_time = DEFAULT_LEASE_TIME;
		uuid_generate(ss->ss_persist.sps_uuid);
		TRACE("server_state_init: fresh start at %s", state_path);
	} else if (ret) {
		LOG("server_state_init: failed to load state from %s: %d",
		    state_path, ret);
		goto err_path;
	} else {
		TRACE("server_state_init: loaded state from %s "
		      "(boot_seq=%u slot_next=%u clean=%u)",
		      state_path, ss->ss_persist.sps_boot_seq,
		      ss->ss_persist.sps_slot_next,
		      ss->ss_persist.sps_clean_shutdown);
	}

	/* Increment boot_seq and mark dirty before touching any state. */
	prev_clean_shutdown = ss->ss_persist.sps_clean_shutdown;
	ss->ss_persist.sps_boot_seq++;
	ss->ss_persist.sps_clean_shutdown = 0;
	ret = ss->ss_persist_ops->server_state_save(ss->ss_persist_ctx,
						    &ss->ss_persist);
	if (ret) {
		LOG("server_state_init: failed to save state: %d", ret);
		goto err_path;
	}

	uuid_copy(ss->ss_uuid, ss->ss_persist.sps_uuid);

	/* Build the stable server owner string used in EXCHANGE_ID replies. */
	{
		char buf[14 + 36 + 1 + HOST_NAME_MAX + 1];
		char uuid_str[37];
		char hostname[HOST_NAME_MAX + 1];
		int n;

		uuid_unparse(ss->ss_persist.sps_uuid, uuid_str);
		if (gethostname(hostname, sizeof(hostname)) != 0)
			hostname[0] = '\0';
		hostname[HOST_NAME_MAX] = '\0';

		n = snprintf(buf, sizeof(buf), "Reffs NFSv4.2 %s/%s", uuid_str,
			     hostname);
		ss->ss_owner_id_len = (n > 0 && (size_t)n < sizeof(buf)) ?
					      (size_t)n :
					      sizeof(buf) - 1;
		ss->ss_owner_id = strndup(buf, ss->ss_owner_id_len);
		if (!ss->ss_owner_id) {
			LOG("server_state_init: failed to build owner id");
			goto err_path;
		}
	}

	/* Determine grace period. */
	ss->ss_grace_time = ss->ss_persist.sps_lease_time ?
				    ss->ss_persist.sps_lease_time :
				    DEFAULT_GRACE_TIME;

	/* Set up runtime subsystems. */
	ss->ss_client_ht = cds_lfht_new(8, 8, 0, CDS_LFHT_AUTO_RESIZE, NULL);
	if (!ss->ss_client_ht) {
		LOG("server_state_init: failed to allocate client hash table");
		goto err_path;
	}

	ss->ss_session_ht = cds_lfht_new(8, 8, 0, CDS_LFHT_AUTO_RESIZE, NULL);
	if (!ss->ss_session_ht) {
		LOG("server_state_init: failed to allocate session hash table");
		goto err_path;
	}

	/*
         * NOT_NOW_BROWN_COW: allocate ss_incarnations hash table.
         */

	urcu_ref_init(&ss->ss_ref);
	server_lifecycle_set(ss, SERVER_BOOTING);

	/*
         * Decide whether grace is needed:
         *   - dirty shutdown (crash or kill) always requires grace
         *   - clean shutdown with prior clients requires grace so they
         *     can reclaim state
         *   - fresh start (slot_next == 1) needs no grace
         */
	if (!prev_clean_shutdown || ss->ss_persist.sps_slot_next > 1) {
		/*
		 * Count previous-boot clients that will need to reclaim.
		 * Grace ends early when this counter reaches zero via
		 * server_reclaim_complete(), or after ss_grace_time seconds
		 * via the grace timer thread.
		 */
		struct client_incarnation_record incs[CLIENT_INCARNATION_MAX];
		size_t nincs = 0;

		if (ss->ss_persist_ops->client_incarnation_load(
			    ss->ss_persist_ctx, incs, CLIENT_INCARNATION_MAX,
			    &nincs) < 0)
			nincs = 0;

		if (nincs == 0) {
			/*
			 * No incarnation records -- no clients can reclaim.
			 * Skip grace entirely; server_reclaim_complete() would
			 * never fire, so the counter-based end trigger can't
			 * work.  The timer-based path could work, but a 90-
			 * second stall on every dirty-shutdown-with-no-clients
			 * boot is surprising.  Go straight to GRACE_ENDED.
			 */
			TRACE("server_state_init: no incarnation records, "
			      "skipping grace");
			server_lifecycle_set(ss, SERVER_GRACE_ENDED);
		} else {
			__atomic_store_n(&ss->ss_unreclaimed, (uint32_t)nincs,
					 __ATOMIC_RELAXED);
			server_grace_start(ss);
		}
	} else {
		TRACE("server_state_init: skipping grace period (fresh/clean start)");
		server_lifecycle_set(ss, SERVER_GRACE_ENDED);
	}

	TRACE("server_state_init: boot_seq=%u lifecycle=%s",
	      ss->ss_persist.sps_boot_seq,
	      server_lifecycle_name(atomic_load_explicit(
		      &ss->ss_lifecycle, memory_order_relaxed)));

	atomic_store_explicit(&current_server_state, ss, memory_order_release);
	return ss;

err_path:
	if (ss->ss_session_ht) {
		ret = cds_lfht_destroy(ss->ss_session_ht, NULL);
		if (ret < 0)
			LOG("Could not delete session hash table: %m");
		ss->ss_session_ht = NULL;
	}

	if (ss->ss_client_ht) {
		ret = cds_lfht_destroy(ss->ss_client_ht, NULL);
		if (ret < 0) {
			LOG("Could not delete a hash table: %m");
		}
		ss->ss_client_ht = NULL;
	}

	if (ss->ss_persist_ops && ss->ss_persist_ops->fini)
		ss->ss_persist_ops->fini(ss->ss_persist_ctx);
	ss->ss_persist_ctx = NULL;

	free(ss->ss_state_dir);
	free(ss);
	return NULL;
}

void server_state_fini(struct server_state *ss)
{
	int ret;

	if (!ss)
		return;

	server_lifecycle_set(ss, SERVER_SHUTTING_DOWN);

	atomic_store_explicit(&current_server_state, NULL,
			      memory_order_release);

	/*
	 * Wake and join the grace timer thread before touching any other
	 * state.  The SHUTTING_DOWN lifecycle causes the thread to exit
	 * promptly on its next 1-second wakeup.
	 */
	if (ss->ss_grace_thread) {
		pthread_join(ss->ss_grace_thread, NULL);
		ss->ss_grace_thread = 0;
	}

	/*
         * Write the clean-shutdown flag now, before releasing the
         * final ref.  If we crash between here and server_state_free(),
         * the next boot will still see a dirty shutdown -- that is
         * intentional and conservative.
         */
	ss->ss_persist.sps_clean_shutdown = 1;
	if (ss->ss_persist_ops->server_state_save(ss->ss_persist_ctx,
						  &ss->ss_persist))
		LOG("server_state_fini: failed to save clean shutdown flag");

	if (ss->ss_session_ht) {
		ret = cds_lfht_destroy(ss->ss_session_ht, NULL);
		if (ret < 0)
			LOG("Could not delete session hash table: %m");
		ss->ss_session_ht = NULL;
	}

	if (ss->ss_client_ht) {
		ret = cds_lfht_destroy(ss->ss_client_ht, NULL);
		if (ret < 0) {
			LOG("Could not delete a hash table: %m");
		}
		ss->ss_client_ht = NULL;
	}

	/*
         * Drop the initial ref taken in server_state_init().  If protocol
         * handlers still hold refs, server_state_free() will be called
         * when the last one drops.  server_state_get() returning NULL
         * for SHUTTING_DOWN prevents new refs from being acquired.
         */
	server_state_put(ss);
}

void server_state_persist_quick(struct server_state *ss)
{
	if (!ss)
		return;

	/*
	 * Mark SHUTTING_DOWN so server_state_get() returns NULL and
	 * no new compounds start.
	 */
	server_lifecycle_set(ss, SERVER_SHUTTING_DOWN);
	atomic_store_explicit(&current_server_state, NULL,
			      memory_order_release);

	/*
	 * Persist the clean-shutdown flag.  On restart, recovery will
	 * see this and skip the grace period if no client state was
	 * dirty.
	 */
	ss->ss_persist.sps_clean_shutdown = 1;
	if (ss->ss_persist_ops->server_state_save(ss->ss_persist_ctx,
						  &ss->ss_persist))
		LOG("persist_quick: failed to save server state");

	/*
	 * Close the namespace database so RocksDB flushes its WAL and
	 * releases the LOCK file.  Without this, SIGKILL after quick
	 * shutdown leaves stale LOCK files that slow the next startup.
	 */
	if (ss->ss_persist_ops->fini)
		ss->ss_persist_ops->fini(ss->ss_persist_ctx);
	ss->ss_persist_ctx = NULL;

	TRACE("Quick shutdown: server state persisted");
}
