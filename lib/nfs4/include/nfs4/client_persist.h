/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_NFS4_CLIENT_PERSIST_H
#define _REFFS_NFS4_CLIENT_PERSIST_H

#include <netinet/in.h>
#include <sys/types.h>

#include "nfsv42_xdr.h"
#include "reffs/server.h"
#include "nfs4/client.h"

/*
 * nfs4_client_alloc_or_find - the central EXCHANGE_ID allocation path.
 *
 * Given the wire parameters from an EXCHANGE_ID request, returns an
 * nfs4_client with a ref held by the caller (release with
 * client_put(nfs4_client_to_client(nc))).
 *
 * Implements RFC 8881 S18.35.4 Table 11 decision tree based on
 * (same_verifier, same_principal, confirmed).  When update is false
 * (non-update case), cases where principal or verifier differ cause
 * the old client to be expired and a new one allocated.  When update
 * is true (UPD_CONFIRMED_REC_A), additional validation produces
 * NFS4ERR_PERM or NFS4ERR_NOT_SAME via *out_status.
 *
 * Returns NULL on allocation failure, misconfiguration, or update
 * error.  When NULL is returned and *out_status != 0, the caller
 * should return that nfsstat4.  When *out_status == 0 and the return
 * is NULL, the caller should return NFS4ERR_SERVERFAULT.
 */
struct nfs4_client *
nfs4_client_alloc_or_find(struct server_state *ss, const client_owner4 *owner,
			  const struct nfs_impl_id4 *impl_id,
			  const verifier4 *verifier,
			  const struct sockaddr_in *sin, uid_t principal_uid,
			  bool update, nfsstat4 *out_status);

/*
 * nfs4_client_expire - expunge all state for nc and remove it from
 * the active incarnations file.  Called when a client restarts
 * (new verifier) or its lease expires.
 *
 * Removes nc from the in-memory client hash, drains its stateids,
 * removes its slot from client_incarnations, then drops the ref.
 * The caller must not use nc after this call.
 */
void nfs4_client_expire(struct server_state *ss, struct nfs4_client *nc);

#endif /* _REFFS_NFS4_CLIENT_PERSIST_H */
