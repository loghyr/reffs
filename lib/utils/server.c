/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#include <time.h>

#include "reffs/rcu.h"
#include "reffs/log.h"
#include "reffs/server_persist.h"
#include "reffs/server.h"

#define DEFAULT_LEASE_TIME 90U /* seconds, per RFC 8881 s2.10.6 */
#define DEFAULT_GRACE_TIME 90U

static struct server_state *current_server_state;

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
	LOG("server lifecycle: %s -> %s",
	    server_lifecycle_name(ss->ss_lifecycle),
	    server_lifecycle_name(next));

	__atomic_store_n(&ss->ss_lifecycle, next, __ATOMIC_RELEASE);
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
         * get from the protocol layer — grace period is now open.
         */
	if (__atomic_load_n(&ss->ss_lifecycle, __ATOMIC_ACQUIRE) ==
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
	struct server_state *ss =
		__atomic_load_n(&current_server_state, __ATOMIC_ACQUIRE);
	return server_state_get(ss);
}

/* ------------------------------------------------------------------ */
/* Grace period                                                        */

void server_grace_start(struct server_state *ss)
{
	enum server_lifecycle lc =
		__atomic_load_n(&ss->ss_lifecycle, __ATOMIC_ACQUIRE);

	if (lc != SERVER_BOOTING) {
		LOG("server_grace_start: unexpected state %s",
		    server_lifecycle_name(lc));
		return;
	}

	clock_gettime(CLOCK_MONOTONIC, &ss->ss_grace_start);
	server_lifecycle_set(ss, SERVER_GRACE_STARTED);
}

void server_grace_end(struct server_state *ss)
{
	enum server_lifecycle lc =
		__atomic_load_n(&ss->ss_lifecycle, __ATOMIC_ACQUIRE);

	if (lc != SERVER_IN_GRACE && lc != SERVER_GRACE_STARTED) {
		LOG("server_grace_end: unexpected state %s",
		    server_lifecycle_name(lc));
		return;
	}

	server_lifecycle_set(ss, SERVER_GRACE_ENDED);
}

/* ------------------------------------------------------------------ */
/* Client slot allocation                                              */

uint32_t server_alloc_client_slot(struct server_state *ss)
{
	uint32_t slot;

	slot = __atomic_fetch_add(&ss->ss_persist.sps_slot_next, 1,
				  __ATOMIC_RELAXED);
	if (slot == UINT32_MAX) {
		/* Wrap — 4 billion clients is unreachable but handle it. */
		LOG("server_alloc_client_slot: slot overflow");
		return UINT32_MAX;
	}

	/*
         * Persist the incremented slot_next before returning so the
         * slot survives a crash between alloc and client confirmation.
         */
	if (server_persist_save(ss->ss_state_dir, &ss->ss_persist)) {
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

struct server_state *server_state_init(const char *state_path, int port)
{
	struct server_state *ss;
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

	/* Load or create persistent state. */
	ret = server_persist_load(state_path, &ss->ss_persist);
	if (ret == -ENOENT) {
		/* Fresh start — initialise defaults. */
		ss->ss_persist.sps_magic = REFFS_SERVER_STATE_MAGIC;
		ss->ss_persist.sps_version = REFFS_SERVER_STATE_VERSION;
		ss->ss_persist.sps_boot_seq = 0;
		ss->ss_persist.sps_clean_shutdown = 1;
		ss->ss_persist.sps_slot_next = 1; /* 0 is sentinel */
		ss->ss_persist.sps_lease_time = DEFAULT_LEASE_TIME;
		uuid_generate(ss->ss_persist.sps_uuid);
		LOG("server_state_init: fresh start at %s", state_path);
	} else if (ret) {
		LOG("server_state_init: failed to load state from %s: %d",
		    state_path, ret);
		goto err_path;
	} else {
		LOG("server_state_init: loaded state from %s "
		    "(boot_seq=%u slot_next=%u clean=%u)",
		    state_path, ss->ss_persist.sps_boot_seq,
		    ss->ss_persist.sps_slot_next,
		    ss->ss_persist.sps_clean_shutdown);
	}

	/* Increment boot_seq and mark dirty before touching any state. */
	ss->ss_persist.sps_boot_seq++;
	ss->ss_persist.sps_clean_shutdown = 0;
	ret = server_persist_save(state_path, &ss->ss_persist);
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
	if (!ss->ss_persist.sps_clean_shutdown ||
	    ss->ss_persist.sps_slot_next > 1) {
		server_grace_start(ss);
	} else {
		LOG("server_state_init: skipping grace period (fresh/clean start)");
		server_lifecycle_set(ss, SERVER_GRACE_ENDED);
	}

	LOG("server_state_init: boot_seq=%u lifecycle=%s",
	    ss->ss_persist.sps_boot_seq,
	    server_lifecycle_name(ss->ss_lifecycle));

	__atomic_store_n(&current_server_state, ss, __ATOMIC_RELEASE);
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

	__atomic_store_n(&current_server_state, NULL, __ATOMIC_RELEASE);

	/*
         * Write the clean-shutdown flag now, before releasing the
         * final ref.  If we crash between here and server_state_free(),
         * the next boot will still see a dirty shutdown — that is
         * intentional and conservative.
         */
	ss->ss_persist.sps_clean_shutdown = 1;
	if (server_persist_save(ss->ss_state_dir, &ss->ss_persist))
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
