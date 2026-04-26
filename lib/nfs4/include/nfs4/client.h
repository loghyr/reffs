/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_NFS4_CLIENT_H
#define _REFFS_NFS4_CLIENT_H

#include <stdatomic.h>

#include "reffs/rcu.h"
#include "nfsv42_xdr.h"
#include "reffs/client.h"
#include "reffs/nfs4_stats.h"
#include "reffs/settings.h"

struct server_state;

/*
 * nfs4_client - NFS4 identity wrapper around the fs-layer struct client.
 *
 * The fs layer owns lifetime (ref counting, hash table, RCU).
 * This layer owns only what is needed on the fast (stateid) path:
 * verifier, confirmed flag, address, and incarnation counter.
 *
 * What is NOT stored here:
 *
 *   co_ownerid  -- the ownerid-->slot mapping lives in the clients file
 *                 on disk.  EXCHANGE_ID is already doing IO; finding
 *                 an existing client scans that file for the ownerid,
 *                 recovers the slot, then calls nfs4_client_find() with
 *                 the clientid4 built from (slot, incarnation,
 *                 current boot_seq).  Fast paths (stateid operations)
 *                 use clientid4 --> client_find() and never touch ownerid.
 *
 *   nc_domain / nc_name -- written once to client_identity_record at
 *                 EXCHANGE_ID time; never needed in memory again.
 */
struct nfs4_client {
	struct sockaddr_in nc_sin;
	uint16_t nc_incarnation;
	verifier4 nc_verifier; /* EXCHANGE_ID verifier */
	bool nc_confirmed;
	bool nc_needs_reclaim; /* true until RECLAIM_COMPLETE received */
	bool nc_reclaim_done; /* true after first RECLAIM_COMPLETE */
	uint32_t nc_exchgid_flags; /* eia_flags from client's EXCHANGE_ID */
	uid_t nc_principal_uid; /* AUTH_SYS uid at EXCHANGE_ID time */
	/*
	 * Set true after a successful PROXY_REGISTRATION on a session
	 * owned by this client.  Grants one narrow privilege: namespace
	 * discovery ops (LOOKUP / LOOKUPP / PUTFH / PUTROOTFH / GETFH /
	 * SEQUENCE) bypass export-rule filtering on this client's
	 * sessions.  Every other op continues to authorize against the
	 * forwarded end-client credentials normally.  See
	 * .claude/design/proxy-server.md "Privilege model".
	 *
	 * Atomic (release on publication, acquire on read) because the
	 * slice 6b-iii squat-guard scans the client hashtable from one
	 * session while another session may be in the middle of
	 * publishing its own registration -- this flag is the publication
	 * marker for the adjacent nc_ps_principal / nc_ps_registration_id
	 * / nc_ps_lease_expire_ns fields.  Without release/acquire a
	 * scanner could observe nc_is_registered_ps == true while still
	 * reading uninitialised principal bytes.
	 */
	_Atomic bool nc_is_registered_ps;

	/*
	 * PROXY_REGISTRATION identity + lease (slice 6b-iii).  Only
	 * meaningful when nc_is_registered_ps == true.  Set at
	 * registration time so the squat-guard can scan for "another
	 * registered client with the same GSS principal"; lease is in
	 * CLOCK_MONOTONIC ns (dual-clock strategy in standards.md).
	 *
	 * nc_ps_lease_expire_ns is _Atomic because the renewal path
	 * writes it from one session while the squat-check reader runs
	 * on another session concurrently.
	 */
	char nc_ps_principal[REFFS_CONFIG_MAX_PRINCIPAL];
	char nc_ps_registration_id[PROXY_REGISTRATION_ID_MAX];
	uint32_t nc_ps_registration_id_len;
	_Atomic uint64_t nc_ps_lease_expire_ns;

	uint32_t nc_create_seq; /* expected csa_sequence for CREATE_SESSION */
	void *nc_create_reply; /* cached CREATE_SESSION XDR reply */
	uint32_t nc_create_reply_len;
	uint32_t nc_session_count; /* atomic: number of active sessions */
	uint64_t nc_last_renew_ns; /* atomic: CLOCK_MONOTONIC of last SEQUENCE */

	struct cds_list_head nc_lock_owners;
	pthread_mutex_t nc_lock_owners_mutex;

	/* Per-op NFS4 statistics -- client scope (ephemeral). */
	struct reffs_op_stats nc_op_stats[REFFS_NFS4_OP_MAX];

	/* Per-CB-op statistics -- indexed by CB op code. */
	struct reffs_cb_stats nc_cb_stats[REFFS_CB_OP_MAX];

	/* Layout error stats reported by this client. */
	struct reffs_layout_error_stats nc_layout_errors;

	struct client nc_client; /* fs-layer object -- keep last */
};

static inline struct nfs4_client *client_to_nfs4(struct client *client)
{
	return caa_container_of(client, struct nfs4_client, nc_client);
}

static inline struct client *nfs4_client_to_client(struct nfs4_client *nc)
{
	return &nc->nc_client;
}

/* ------------------------------------------------------------------ */
/* NFS4-aware alloc / find                                             */

/*
 * nfs4_client_alloc - allocate an nfs4_client.
 *
 * No impl_id, no owner copy: both are persistence-only and live on
 * disk.  The caller writes the identity record before calling us.
 *
 * Caller must client_put(nfs4_client_to_client(nc)) when done.
 */
struct nfs4_client *nfs4_client_alloc(const verifier4 *verifier,
				      const struct sockaddr_in *sin,
				      uint16_t incarnation,
				      clientid4 assigned_id,
				      uid_t principal_uid);

/*
 * nfs4_client_find - look up by clientid4.
 * Returns ref-bumped nfs4_client or NULL.  Caller must client_put().
 */
struct nfs4_client *nfs4_client_find(clientid4 clid);

struct nfs4_client *nfs4_client_get(struct nfs4_client *nc);
void nfs4_client_put(struct nfs4_client *nc);

/*
 * nfs4_client_find_other_registered_ps - scan the client hashtable
 * for an in-memory client (other than `self`) holding the
 * registered-PS privilege with the given GSS principal (slice
 * 6b-iii squat-guard).
 *
 * Returns ref-bumped match or NULL.  Caller drops ref via
 * nfs4_client_put().  Does NOT filter by lease expiry -- the caller
 * decides squat (still valid + different id) vs renewal (still
 * valid + same id) vs expired-treat-as-fresh (lease in past).
 */
struct nfs4_client *
nfs4_client_find_other_registered_ps(struct server_state *ss,
				     const struct nfs4_client *self,
				     const char *principal);

/*
 * nfs4_client_find_by_owner - find an in-memory client by ownerid.
 *
 * Scans the clients file for an ownerid match to recover the slot,
 * then looks up the active incarnation for that slot, builds the
 * clientid4 from (slot, incarnation, boot_seq), and calls
 * nfs4_client_find().
 *
 * Only called on the EXCHANGE_ID path -- disk IO is expected here.
 *
 * Returns ref-bumped nfs4_client or NULL (not found or I/O error).
 * Caller must client_put().
 */
/*
 * out_slot: if non-NULL, set to the slot found in the identity file for this
 * ownerid, or UINT32_MAX if the ownerid is unknown.  When the return value is
 * NULL but *out_slot != UINT32_MAX, the ownerid is known but the client has
 * not yet reconnected this boot (reclaiming client).
 */
struct nfs4_client *nfs4_client_find_by_owner(struct server_state *ss,
					      uint16_t boot_seq,
					      const client_owner4 *owner,
					      uint32_t *out_slot);

/* ------------------------------------------------------------------ */
/* clientid4 bit-packing                                               */

#define CLIENTID_SLOT_BITS 32
#define CLIENTID_INCARNATION_BITS 16
#define CLIENTID_BOOT_BITS 16

#define CLIENTID_SLOT_SHIFT 0
#define CLIENTID_INCARNATION_SHIFT CLIENTID_SLOT_BITS
#define CLIENTID_BOOT_SHIFT (CLIENTID_SLOT_BITS + CLIENTID_INCARNATION_BITS)

#define CLIENTID_SLOT_MASK ((1ULL << CLIENTID_SLOT_BITS) - 1)
#define CLIENTID_INCARNATION_MASK ((1ULL << CLIENTID_INCARNATION_BITS) - 1)
#define CLIENTID_BOOT_MASK ((1ULL << CLIENTID_BOOT_BITS) - 1)

static inline clientid4 clientid_make(uint32_t slot, uint16_t incarnation,
				      uint16_t boot_seq)
{
	return ((uint64_t)boot_seq << CLIENTID_BOOT_SHIFT) |
	       ((uint64_t)incarnation << CLIENTID_INCARNATION_SHIFT) |
	       ((uint64_t)slot << CLIENTID_SLOT_SHIFT);
}

static inline uint32_t clientid_slot(clientid4 clid)
{
	return (clid >> CLIENTID_SLOT_SHIFT) & CLIENTID_SLOT_MASK;
}

static inline uint16_t clientid_incarnation(clientid4 clid)
{
	return (clid >> CLIENTID_INCARNATION_SHIFT) & CLIENTID_INCARNATION_MASK;
}

static inline uint16_t clientid_boot_seq(clientid4 clid)
{
	return (clid >> CLIENTID_BOOT_SHIFT) & CLIENTID_BOOT_MASK;
}

#endif /* _REFFS_NFS4_CLIENT_H */
