/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_NFS4_CLIENT_H
#define _REFFS_NFS4_CLIENT_H

#include <urcu/compiler.h> /* caa_container_of */

#include "nfsv42_xdr.h"
#include "reffs/client.h"

/*
 * nfs4_client - NFS4 identity wrapper around the fs-layer struct client.
 *
 * The fs layer owns lifetime (ref counting, hash table, RCU).
 * This layer owns the NFS wire identity (owner, verifier, confirmed state).
 *
 * Mirrors the stateid / nfs4_stateid split: the embedded struct client
 * is the anchor; caa_container_of recovers the nfs4_client from it.
 */
struct nfs4_client {
	client_owner4 nc_owner; /* long-hand client identity */
	verifier4 nc_verifier; /* EXCHANGE_ID / SETCLIENTID verifier */
	bool nc_confirmed;
	struct client nc_client; /* fs-layer object; must be last or
	                              * after all NFS fields to keep
	                              * caa_container_of unambiguous */
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
 * nfs4_client_alloc - allocate an nfs4_client for the given owner.
 * Derives the uint64_t id from the owner (server assigns clientid4).
 * Caller must client_put(nfs4_client_to_client(nc)) when done.
 */
struct nfs4_client *nfs4_client_alloc(client_owner4 *owner, verifier4 *verifier,
				      clientid4 assigned_id);

/*
 * nfs4_client_find - look up by clientid4.
 * Returns ref-bumped nfs4_client or NULL.  Caller must client_put().
 */
struct nfs4_client *nfs4_client_find(clientid4 clid);

/*
 * nfs4_client_find_by_owner - look up by client_owner4.
 * Linear scan of confirmed clients; only used on EXCHANGE_ID path.
 */
struct nfs4_client *nfs4_client_find_by_owner(client_owner4 *owner);

/*
 * Assign the clientid
 */
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
