/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Data store (dstore) — an NFSv3 data server export used by the MDS.
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
#include <stdbool.h>
#include <stdint.h>

#include <rpc/rpc.h>

#include "reffs/rcu.h"
#include "reffs/settings.h"

#define DSTORE_MAX_FH 64 /* NFSv3 FHSIZE3 */

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

	/* Ops vtable: nfsv3 (remote) or local (same server). */
	const struct dstore_ops *ds_ops;

	/* Pre-created file pool (set by runway_create at startup). */
	struct runway *ds_runway;

	/* MOUNT result (valid when DSTORE_IS_MOUNTED is set) */
	uint8_t ds_root_fh[DSTORE_MAX_FH]; /* NFSv3 root filehandle */
	uint32_t ds_root_fh_len; /* actual FH length */

	/*
	 * NFSv3 client handle for control-plane ops.
	 * Protected by ds_clnt_mutex — readers must hold the mutex
	 * while using the handle, reconnect holds it exclusively.
	 */
	CLIENT *ds_clnt;
	pthread_mutex_t ds_clnt_mutex;

	/* RCU + refcount infrastructure */
	struct rcu_head ds_rcu;
	struct urcu_ref ds_ref;
	uint64_t ds_state; /* atomic flag word */
	struct cds_lfht_node ds_node; /* hash table node */
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
			    bool mount);

/*
 * dstore_find -- look up by id.
 * Returns a ref-bumped pointer or NULL.  Caller must dstore_put().
 */
struct dstore *dstore_find(uint32_t id);

/* Bump / drop ref.  NULL-safe. */
struct dstore *dstore_get(struct dstore *ds);
void dstore_put(struct dstore *ds);

/* Remove from hash table (idempotent). */
bool dstore_unhash(struct dstore *ds);

/* ------------------------------------------------------------------ */
/* Connection management                                               */

/*
 * dstore_is_available -- true if the dstore is mounted and not
 * currently reconnecting.  Lock-free atomic check.
 */
static inline bool dstore_is_available(const struct dstore *ds)
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
 * Serialised by ds_clnt_mutex — concurrent callers block until
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
