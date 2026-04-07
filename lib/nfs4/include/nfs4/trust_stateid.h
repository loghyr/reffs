/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * DS trust table -- flexfiles v2 tight coupling.
 *
 * The MDS registers layout stateids in this table via TRUST_STATEID.
 * CHUNK_WRITE and CHUNK_READ validate the client's stateid against the
 * table before allowing I/O.
 *
 * The table is a global cds_lfht keyed by the 12-byte stateid.other
 * field.  Rule 6 (ref-counting.md) governs the entry lifecycle.
 *
 * Flags (te_flags):
 *   TRUST_ACTIVE   -- stateid is fully registered, I/O allowed
 *   TRUST_PENDING  -- MDS re-registered after crash; DS returns
 *                     NFS4ERR_DELAY until the MDS confirms (see
 *                     BULK_REVOKE then re-register flow)
 */

#ifndef _REFFS_NFS4_TRUST_STATEID_H
#define _REFFS_NFS4_TRUST_STATEID_H

#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>

#include <urcu/rculfhash.h>
#include <urcu/ref.h>
#include <urcu/rculist.h>

#include "nfsv42_xdr.h"

/* Maximum length of the principal string stored in a trust entry. */
#define TRUST_PRINCIPAL_MAX 256

/* te_flags bits */
#define TRUST_ACTIVE  (1u << 0) /* stateid accepted for I/O */
#define TRUST_PENDING (1u << 1) /* pending revalidation (MDS reboot) */

struct trust_entry {
	struct cds_lfht_node te_ht_node;               /* MUST be first */
	struct rcu_head      te_rcu;                   /* for call_rcu */
	struct urcu_ref      te_ref;

	uint8_t   te_other[NFS4_OTHER_SIZE];   /* stateid.other -- hash key */
	uint64_t  te_ino;                       /* inode (from current FH) */
	clientid4 te_clientid;                  /* client that holds layout */
	layoutiomode4 te_iomode;               /* LAYOUTIOMODE4_READ or _RW */

	/*
	 * Expiry in CLOCK_MONOTONIC nanoseconds.
	 * _Atomic so the lease-reaper can renew without locking.
	 */
	_Atomic uint64_t te_expire_ns;

	_Atomic uint32_t te_flags; /* TRUST_ACTIVE | TRUST_PENDING */

	/*
	 * Principal string from tsa_principal (UTF-8).  Written once at
	 * registration time; read-only thereafter.  Empty string = no
	 * principal constraint (AUTH_SYS from a trusted MDS).
	 */
	char te_principal[TRUST_PRINCIPAL_MAX];
};

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */

/*
 * trust_stateid_init -- allocate the global hash table and start the
 * expiry reaper thread.  Called at server startup.
 */
int trust_stateid_init(void);

/*
 * trust_stateid_fini -- drain the hash table, stop the reaper, and
 * free all resources.  Called at server shutdown.
 */
void trust_stateid_fini(void);

/* ------------------------------------------------------------------ */
/* Mutation                                                            */

/*
 * trust_stateid_register -- insert or update a trust entry.
 *
 * If an entry for the same stateid.other already exists, updates its
 * expiry, iomode, and flags (idempotent for MDS retries).  Otherwise
 * allocates a new entry.
 *
 * ino: inode number from compound->c_inode at TRUST_STATEID time.
 * expire_mono_ns: absolute CLOCK_MONOTONIC expiry in nanoseconds.
 * principal: UTF-8 caller principal (empty string for AUTH_SYS MDS).
 *
 * Returns 0 on success, -ENOMEM on allocation failure.
 */
int trust_stateid_register(const stateid4 *stateid, uint64_t ino,
			   clientid4 clientid, layoutiomode4 iomode,
			   uint64_t expire_mono_ns,
			   const char *principal);

/*
 * trust_stateid_revoke -- remove the entry for this stateid.other.
 * No-op if not found.
 */
void trust_stateid_revoke(const stateid4 *stateid);

/*
 * trust_stateid_bulk_revoke -- remove all entries for clientid.
 * If clientid is all-zeros, clears the entire table.
 */
void trust_stateid_bulk_revoke(clientid4 clientid);

/* ------------------------------------------------------------------ */
/* Lookup                                                              */

/*
 * trust_stateid_find -- return a ref-bumped trust_entry for stateid,
 * or NULL if not found.
 *
 * The caller must call trust_entry_put() when done.
 * Expired entries ARE returned; the caller must check te_expire_ns.
 */
struct trust_entry *trust_stateid_find(const stateid4 *stateid);

/*
 * trust_entry_put -- drop a reference to a trust entry.
 * If the refcount reaches zero the entry is freed via RCU.
 */
void trust_entry_put(struct trust_entry *te);

/* ------------------------------------------------------------------ */
/* Helpers                                                             */

/*
 * trust_stateid_convert_expire -- convert a wire nfstime4 expiry
 * (wall clock) to an absolute CLOCK_MONOTONIC nanosecond value.
 *
 * The caller passes the current wall-clock and monotonic readings
 * (taken together) so the conversion is accurate even if clocks drift
 * between the reading and this call.
 *
 * Returns the monotonic deadline, or 0 on invalid/overflow input.
 */
uint64_t trust_stateid_convert_expire(const nfstime4 *expire,
				      uint64_t now_wall_ns,
				      uint64_t now_mono_ns);

#endif /* _REFFS_NFS4_TRUST_STATEID_H */
