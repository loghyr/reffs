/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Data store (dstore) -- an NFSv3 data server export used by the MDS.
 *
 * Each dstore represents an address:path pair from the [[data_server]]
 * config.  At startup the MDS uses the MOUNT protocol to obtain the
 * root filehandle, then uses the stored CLIENT handle for NFSv3
 * control operations (CREATE, REMOVE, GETATTR).
 *
 * Dstores are RCU-protected and refcounted, stored in a global
 * cds_lfht keyed by ds_id.  Layouts reference dstores by ID.
 */

#ifndef _REFFS_DSTORE_H
#define _REFFS_DSTORE_H

#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include <rpc/rpc.h>

#include "reffs/nfs4_stats.h"
#include "reffs/rcu.h"
#include "reffs/settings.h"

#define DSTORE_MAX_FH 64 /* NFSv3 FHSIZE3 */
#define DSTORE_REVOKE_MAX 64 /* max DSes to scan for bulk revoke */

#define DSTORE_IS_HASHED (1ULL << 0)
#define DSTORE_IS_MOUNTED (1ULL << 1)
#define DSTORE_IS_RECONNECTING (1ULL << 2)

struct dstore_ops;
struct runway;

struct dstore {
	uint32_t ds_id; /* unique ID (from config) */

	/* Config (immutable after alloc) */
	char ds_address[REFFS_CONFIG_MAX_HOST]; /* hostname or IP from config */
	char ds_ip[INET_ADDRSTRLEN]; /* resolved dotted-decimal IP */
	char ds_path[REFFS_CONFIG_MAX_PATH];

	/* DS protocol: nfsv3 (flex files) or nfsv4 (file layouts). */
	enum reffs_ds_protocol ds_protocol;

	/* Ops vtable: local (VFS), nfsv3, or nfsv4. */
	const struct dstore_ops *ds_ops;

	/* Pre-created file pool (set by runway_create at startup). */
	struct runway *ds_runway;

	/* MOUNT result (valid when DSTORE_IS_MOUNTED is set) */
	uint8_t ds_root_fh[DSTORE_MAX_FH]; /* NFSv3 root filehandle */
	uint32_t ds_root_fh_len; /* actual FH length */

	/*
	 * NFSv3 client handle for control-plane ops (nfsv3 vtable).
	 * Protected by ds_clnt_mutex -- readers must hold the mutex
	 * while using the handle, reconnect holds it exclusively.
	 */
	CLIENT *ds_clnt;
	pthread_mutex_t ds_clnt_mutex;

	/*
	 * NFSv4.2 session for control-plane + InBand I/O (nfsv4 vtable).
	 * The MDS acts as a plain NFSv4 client (USE_NON_PNFS) to the DS.
	 * NULL when the dstore uses NFSv3 or local VFS.
	 */
	struct mds_session *ds_v4_session;

	/* RCU + refcount infrastructure */
	struct rcu_head ds_rcu;
	struct urcu_ref ds_ref;
	uint64_t ds_state; /* atomic flag word */
	struct cds_lfht_node ds_node; /* hash table node */

	/*
	 * Tight coupling (pNFS Flex Files v2): true if the DS responded to
	 * the capability probe (TRUST_STATEID with anonymous stateid) with
	 * NFS4ERR_INVAL, indicating it supports TRUST_STATEID / REVOKE_STATEID.
	 * Set once in dstore_alloc after the NFSv4 session is established.
	 * Read-only after that; no synchronization needed.
	 */
	bool ds_tight_coupled;

	/*
	 * Drain flag (mirror-lifecycle Slice B).  When true, LAYOUTGET /
	 * runway-pop excludes this dstore from new placements.  Existing
	 * instances on the dstore remain reachable until migrated off
	 * (slice E autopilot) or the dstore is destroyed.
	 *
	 * Declared as a separate _Atomic bool rather than a bit in
	 * ds_state because ds_state uses GCC __atomic_* builtins (a
	 * grandfathered field per .claude/standards.md "Two atomic APIs
	 * in use").  C11 _Atomic separate field is the right precedent
	 * for fresh state -- "Do not add new GCC-builtin atomic fields."
	 *
	 * NOT_NOW_BROWN_COW: persist across reffsd restarts (today the
	 * flag resets on boot).  See .claude/design/mirror-lifecycle.md
	 * "Drain persistence".
	 */
	_Atomic bool ds_drained;

	/*
	 * Cached count of (sb, inum) entries indexed against this dstore
	 * across all SBs (mirror-lifecycle Slice B'').  The persistent
	 * reverse index is the source of truth; this is a hot-path cache
	 * for DSTORE_INSTANCE_COUNT and (in slice G) DSTORE_DESTROY
	 * admission control.  Bumped/decremented in the same code path
	 * that adds/removes index entries.  Memory order is relaxed --
	 * no synchronization-with semantics required since the index is
	 * authoritative.  Rebuilt at server startup by walking SBs and
	 * summing dstore_index_count() per (sb, this dstore).
	 */
	_Atomic uint64_t ds_instance_count;

	/* Layout error stats reported by clients for this dstore. */
	struct reffs_layout_error_stats ds_layout_errors;
};

/* ------------------------------------------------------------------ */
/* Module lifecycle                                                    */

/*
 * dstore_init -- create the global dstore hash table.
 * Call once at startup before dstore_alloc.
 */
int dstore_init(void);

/*
 * dstore_fini -- drain and destroy the global hash table.
 * Call at shutdown after all dstore references are released.
 */
void dstore_fini(void);

/* ------------------------------------------------------------------ */
/* Lifetime                                                            */

/*
 * dstore_alloc -- allocate a dstore, mount it, and insert into the
 * global hash table.
 *
 * If mount is true, connect to the DS via MOUNT to obtain the root
 * FH.  If false, the dstore is inserted unmounted (for unit tests
 * or deferred mount).
 *
 * Returns a ref-bumped pointer on success (caller must dstore_put),
 * or NULL on failure (duplicate ID).
 */
struct dstore *dstore_alloc(uint32_t id, const char *address, const char *path,
			    enum reffs_ds_protocol protocol, bool mount);

/*
 * dstore_find -- look up by id.
 * Returns a ref-bumped pointer or NULL.  Caller must dstore_put().
 */
struct dstore *dstore_find(uint32_t id);

/*
 * ds_session_create -- establish an NFSv4.2 session to a DS.
 * Used for NFSv4 dstores (control plane + InBand I/O).
 * Sets ds->ds_v4_session and ds->ds_root_fh on success.
 */
int ds_session_create(struct dstore *ds);
void ds_session_destroy(struct dstore *ds);

/* Bump / drop ref.  NULL-safe. */
struct dstore *dstore_get(struct dstore *ds);
void dstore_put(struct dstore *ds);

/* Remove from hash table (idempotent). */
bool dstore_unhash(struct dstore *ds);

/*
 * dstore_probe_root_access -- verify MDS can CREATE with uid=0 on a DS.
 * Removes any stale .root_probe breadcrumb before the new probe.
 * Returns 0 (root confirmed), -EACCES (root squashed, LOG emitted),
 * or another negative errno for unexpected failures.
 *
 * Called from dstore_alloc() after a successful NFSv3 MOUNT.  Exposed
 * for unit testing -- production callers use dstore_alloc().
 */
int dstore_probe_root_access(struct dstore *ds);

/* ------------------------------------------------------------------ */
/* Connection management                                               */

/*
 * dstore_is_available -- true if the dstore is mounted, not
 * currently reconnecting, and not flagged for drain.  Lock-free
 * atomic check; used by LAYOUTGET / runway-pop to exclude dstores
 * that should not receive new placements.
 *
 * Drain semantics (mirror-lifecycle Slice B): a drained dstore is
 * unavailable for NEW placements; existing instances are still
 * reachable via the normal data path -- this helper governs only
 * placement, not I/O.
 */
static inline bool dstore_is_available(const struct dstore *ds)
{
	uint64_t s;

	__atomic_load(&ds->ds_state, &s, __ATOMIC_ACQUIRE);
	if (!(s & DSTORE_IS_MOUNTED) || (s & DSTORE_IS_RECONNECTING))
		return false;
	return !atomic_load_explicit(&ds->ds_drained, memory_order_acquire);
}

/*
 * dstore_is_connected -- true if the dstore is mounted and not
 * currently reconnecting.  Ignores the drain flag.
 *
 * Use this for connection-liveness checks (e.g. dstore_reconnect's
 * already-up short-circuit) and for any fan-out that must reach
 * existing instances on a drained dstore (e.g. lease-expiry
 * TRUST_STATEID bulk-revoke).  Use dstore_is_available() only for
 * placement decisions (LAYOUTGET, runway-pop).
 */
static inline bool dstore_is_connected(const struct dstore *ds)
{
	uint64_t s;

	__atomic_load(&ds->ds_state, &s, __ATOMIC_ACQUIRE);
	return (s & DSTORE_IS_MOUNTED) && !(s & DSTORE_IS_RECONNECTING);
}

/*
 * dstore_reconnect -- tear down the existing CLIENT handle and
 * re-MOUNT the data server.  Called when an NFSv3 RPC to this
 * dstore fails.
 *
 * Serialised by ds_clnt_mutex -- concurrent callers block until
 * the first one finishes.  Callers should retry their operation
 * after this returns.
 *
 * Returns 0 on success, negative errno on failure (dstore remains
 * in the table with DSTORE_IS_MOUNTED cleared so callers can
 * distinguish "never mounted" from "was mounted, now down").
 */
int dstore_reconnect(struct dstore *ds);

/*
 * dstore_collect_available -- gather refs to all mounted dstores.
 *
 * Fills out[] with ref-bumped dstore pointers (caller must put each).
 * max: size of out[].  Returns the number of dstores collected.
 */
uint32_t dstore_collect_available(struct dstore **out, uint32_t max);

/*
 * dstore_collect_all -- gather refs to every dstore, regardless of
 * mount / drain / reconnecting state.  Used by the DSTORE_LIST probe
 * op for the operator dashboard.  Caller drops each ref via
 * dstore_put().
 */
uint32_t dstore_collect_all(struct dstore **out, uint32_t max);

/* ------------------------------------------------------------------ */
/* Bulk operations                                                     */

/*
 * dstore_load_config -- create and mount all dstores from config.
 * Continues past individual mount failures (logs errors).
 * Returns 0 on success, negative errno if the hash table is not
 * initialized or no data servers are configured.
 */
int dstore_load_config(const struct reffs_config *cfg);

/*
 * dstore_unload_all -- unhash and release all dstores.
 */
void dstore_unload_all(void);

#endif /* _REFFS_DSTORE_H */
