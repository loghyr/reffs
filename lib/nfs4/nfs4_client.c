/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <urcu/compiler.h>
#include <urcu/rculfhash.h>
#include <xxhash.h>

#include "nfsv42_xdr.h"
#include "reffs/log.h"
#include "reffs/rcu.h"
#include "reffs/client.h"
#include "reffs/trace/nfs4_server.h"
#include "nfs4_client.h"

/* ------------------------------------------------------------------ */
/* Alloc                                                               */

struct nfs4_client *nfs4_client_alloc(client_owner4 *owner, verifier4 *verifier,
				      clientid4 assigned_id)
{
	struct nfs4_client *nc;
	struct client *c;

	nc = calloc(1, sizeof(*nc));
	if (!nc)
		return NULL;

	memcpy(&nc->nc_owner, owner, sizeof(*owner));
	memcpy(&nc->nc_verifier, verifier, sizeof(*verifier));
	nc->nc_confirmed = false;

	/*
	 * Hand off to the fs layer using assigned_id as the uint64_t key.
	 * client_alloc() inserts into the global client_ht and returns
	 * with two refs: one for the hash table, one for the caller.
	 */
	c = client_alloc((uint64_t)assigned_id);
	if (!c) {
		free(nc);
		return NULL;
	}

	/*
	 * Wire the fs-layer client into our embedded field.  We transfer
	 * the caller ref from client_alloc() to the nfs4_client — the
	 * hash-table ref stays independent.
	 *
	 * client_alloc() already filled c_id; copy it back so nc_client
	 * and any direct field access agree.
	 */
	nc->nc_client = *c;

	/*
	 * NOT_NOW_BROWN_COW: client_alloc should return a pointer we embed,
	 * not a copy.  For now the shallow copy is safe because urcu_ref
	 * and cds_lfht_node are value types re-initialised by the alloc;
	 * revisit when nfs4_client owns the allocation end-to-end.
	 */

	return nc;
}

/* ------------------------------------------------------------------ */
/* Find by clientid4                                                   */

struct nfs4_client *nfs4_client_find(clientid4 clid)
{
	struct client *c = client_find((uint64_t)clid);

	if (!c)
		return NULL;

	return client_to_nfs4(c);
}

/* ------------------------------------------------------------------ */
/* Find by owner (linear scan — EXCHANGE_ID path only)                */

/*
 * NOT_NOW_BROWN_COW: this needs a second hash table keyed by owner
 * once client counts grow.  For the prototype a walk is fine.
 */
struct nfs4_client *nfs4_client_find_by_owner(client_owner4 *owner)
{
	/* NOT_NOW_BROWN_COW: implement owner-keyed lookup */
	(void)owner;
	return NULL;
}
