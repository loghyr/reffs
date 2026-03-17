/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_NFS4_CLIENT_PERSIST_H
#define _REFFS_NFS4_CLIENT_PERSIST_H

#include <netinet/in.h>

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
 * Decision tree:
 *
 *   ownerid not seen before:
 *     → append to clients file, add to client_incarnations,
 *       return new nfs4_client (incarnation 0)
 *
 *   ownerid known, same verifier, same addr:
 *     → idempotent retry, return existing client
 *
 *   ownerid known, same verifier, diff addr:
 *     → multi-homed client, return existing client
 *
 *   ownerid known, diff verifier, same addr:
 *     → client restarted: expire old client, remove from
 *       incarnations, allocate new with bumped incarnation
 *
 *   ownerid known, diff verifier, diff addr:
 *     → misconfiguration: LOG both addresses, return NULL
 *       (caller should return NFS4ERR_CLID_INUSE)
 *
 * Returns NULL on allocation failure or misconfiguration.
 */
struct nfs4_client *
nfs4_client_alloc_or_find(struct server_state *ss, const client_owner4 *owner,
			  const struct nfs_impl_id4 *impl_id,
			  const verifier4 *verifier,
			  const struct sockaddr_in *sin);

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
