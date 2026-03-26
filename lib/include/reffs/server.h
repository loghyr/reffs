/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_SERVER_H
#define _REFFS_SERVER_H

#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#include <urcu/ref.h>
#include <urcu/rculfhash.h>
#include <uuid/uuid.h>

#include "reffs/cmp.h"
#include "reffs/nfs4_stats.h"
#include "reffs/server_persist.h"
#include "reffs/settings.h"

/*
 * Server lifecycle state machine.
 *
 *   BOOTING
 *      |
 *      | server_state_init() completes, clients had state or dirty shutdown
 *      v
 *   GRACE_STARTED  (transient — grace period timer armed)
 *      |
 *      | first server_state_get() by protocol layer
 *      v
 *   IN_GRACE       (reclaim window open)
 *      |
 *      | grace timer expires  OR  all clients reclaimed
 *      v
 *   GRACE_ENDED    (normal operation)
 *      |
 *      | server_state_fini() called
 *      v
 *   SHUTTING_DOWN
 *
 * If there was no prior client state (fresh start or clean shutdown
 * with no clients), the machine skips directly:
 *   BOOTING -> GRACE_ENDED
 *
 * server_state_get() returns NULL when state == SHUTTING_DOWN.
 */
enum server_lifecycle {
	SERVER_BOOTING = 0,
	SERVER_GRACE_STARTED = 1,
	SERVER_IN_GRACE = 2,
	SERVER_GRACE_ENDED = 3,
	SERVER_SHUTTING_DOWN = 4,
};

struct server_state {
	/* Persisted fields — loaded at boot, saved on clean shutdown. */
	struct server_persistent_state ss_persist;
	char *ss_state_dir;

	/* State machine */
	enum server_lifecycle ss_lifecycle;

	/* Grace period */
	struct timespec ss_grace_start;
	uint32_t ss_grace_time; /* seconds */
	uint32_t ss_unreclaimed; /* atomic: previous-boot clients not yet RECLAIM_COMPLETE'd */
	pthread_t ss_grace_thread; /* timer thread; 0 if not running */

	/*
         * Per-instance subsystem state — moved here from file-scope
         * statics in client.c so MDS and DS instances on the same
         * machine remain independent.
         */
	struct cds_lfht *ss_client_ht;
	uint64_t ss_client_mod_state;

	struct cds_lfht *ss_session_ht;

	/*
         * Per-boot incarnation table: slot -> uint16_t counter.
         * Looked up on EXCHANGE_ID reconnect to bump incarnation field
         * of the new clientid.
         * NOT_NOW_BROWN_COW: implement as cds_lfht keyed by slot.
         */
	struct cds_lfht *ss_incarnations;

	/* Ref count — protocol layer holds a ref while serving RPCs. */
	struct urcu_ref ss_ref;

	/* Unique server id */
	uuid_t ss_uuid;

	/*
	 * Human-readable server owner string used in EXCHANGE_ID responses:
	 * "Reffs NFSv4.2 <uuid>/<hostname>"
	 * Built once in server_state_init(), freed in server_state_free().
	 */
	char *ss_owner_id;
	size_t ss_owner_id_len;

	int ss_port;

	/* EXCHGID4_FLAG_USE_* — set at startup from the configured role. */
	uint32_t ss_exchgid_flags;

	/* Case sensitivity mode for this server instance. */
	enum reffs_text_case ss_case;

	/* Config values copied at boot — not persisted. */
	uint32_t ss_fence_uid_min;
	uint32_t ss_fence_uid_max;
	unsigned int ss_layout_width;

	/* NFSv4 owner string domain (e.g., "EXAMPLE.COM"). */
	char ss_nfs4_domain[256];

	/* Export security flavors — copied from first export at boot. */
	enum reffs_auth_flavor ss_flavors[REFFS_CONFIG_MAX_FLAVORS];
	unsigned int ss_nflavors;

	/* Per-op NFS4 statistics — global scope. */
	struct reffs_op_stats ss_nfs4_op_stats[REFFS_NFS4_OP_MAX];

	/* Aggregate backend I/O statistics across all superblocks. */
	struct reffs_backend_stats ss_backend_stats;

	/* Aggregate layout error stats (from LAYOUTERROR reports). */
	struct reffs_layout_error_stats ss_layout_errors;
};

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */

/*
 * server_state_init - load or create persistent state at path, increment
 * boot_seq, mark dirty, determine grace period, arm grace timer.
 *
 * case_mode controls whether directory lookups are case-sensitive or
 * case-insensitive for this server instance.
 *
 * Returns an allocated server_state on success (caller holds the
 * initial ref), NULL on failure.
 */
struct server_state *server_state_init(const char *state_path, int port,
				       enum reffs_text_case case_mode);

/*
 * server_state_fini - begin shutdown.  Transitions to SHUTTING_DOWN so that
 * subsequent server_state_get() calls return NULL.  Blocks until the
 * ref count reaches zero (all protocol handlers have released), then
 * writes the clean-shutdown flag and frees resources.
 */
void server_state_fini(struct server_state *ss);

/* ------------------------------------------------------------------ */
/* Ref counting                                                        */

/*
 * server_state_get - bump ref and return ss, or NULL if shutting down.
 * Protocol handlers call this at RPC dispatch entry; if NULL is
 * returned the handler must return NFS4ERR_SERVERFAULT / a mount error.
 */
struct server_state *server_state_get(struct server_state *ss);

/*
 * server_state_put - release ref.  If this is the last ref and the
 * server is SHUTTING_DOWN, triggers the final cleanup.
 */
void server_state_put(struct server_state *ss);

/* ------------------------------------------------------------------ */
/* State machine transitions                                           */

/*
 * server_grace_start - transition BOOTING -> GRACE_STARTED -> IN_GRACE.
 * Called by reffs_ns_init() after subsystems are ready to accept
 * reclaim.  No-op if grace is not needed (fresh / clean start).
 */
void server_grace_start(struct server_state *ss);

/*
 * server_grace_end - transition IN_GRACE -> GRACE_ENDED.
 * Called by grace timer expiry or when all clients have reclaimed.
 */
void server_grace_end(struct server_state *ss);

/*
 * server_reclaim_complete - called when a client sends RECLAIM_COMPLETE.
 * Decrements ss_unreclaimed; calls server_grace_end() when it reaches zero.
 */
void server_reclaim_complete(struct server_state *ss);

/*
 * server_state_find - locate the current server instance without a
 * pointer.  Returns a ref-bumped pointer or NULL if no server has been
 * initialised or shutdown is in progress.  Caller must
 * server_state_put() the result.
 */
struct server_state *server_state_find(void);

/* ------------------------------------------------------------------ */
/* Accessors for protocol layer                                        */

static inline bool server_in_grace(const struct server_state *ss)
{
	return ss->ss_lifecycle == SERVER_IN_GRACE ||
	       ss->ss_lifecycle == SERVER_GRACE_STARTED;
}

/*
 * nfs4_check_grace - return true if the server is currently in grace.
 * Self-contained: acquires and releases the server_state ref internally.
 */
static inline bool nfs4_check_grace(void)
{
	struct server_state *ss = server_state_find();
	bool in_grace = ss && server_in_grace(ss);

	server_state_put(ss);
	return in_grace;
}

static inline bool server_shutting_down(const struct server_state *ss)
{
	return ss->ss_lifecycle == SERVER_SHUTTING_DOWN;
}

static inline uint16_t server_boot_seq(const struct server_state *ss)
{
	return ss->ss_persist.sps_boot_seq;
}

static inline uint32_t server_lease_time(const struct server_state *ss)
{
	return ss->ss_persist.sps_lease_time;
}

/*
 * server_alloc_client_slot - atomically claim the next slot number.
 * Persists sps_slot_next before returning so the slot survives reboot.
 * Returns the allocated slot, or UINT32_MAX on overflow or I/O error.
 */
uint32_t server_alloc_client_slot(struct server_state *ss);

/*
 * server_lifecycle_name - human-readable state name for logging.
 */
const char *server_lifecycle_name(enum server_lifecycle lc);

#endif /* _REFFS_SERVER_H */
