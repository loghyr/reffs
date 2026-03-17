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
/* RCU / release callbacks                                             */

static void nfs4_client_free_rcu(struct rcu_head *rcu)
{
	struct client *c = caa_container_of(rcu, struct client, c_rcu);
	struct nfs4_client *nc = client_to_nfs4(c);

	if (c->c_stateids)
		cds_lfht_destroy(c->c_stateids, NULL);

	free(nc);
}

static void nfs4_client_release(struct urcu_ref *ref)
{
	struct client *c = caa_container_of(ref, struct client, c_ref);

	call_rcu(&c->c_rcu, nfs4_client_free_rcu);
}

/* ------------------------------------------------------------------ */
/* Alloc                                                               */

struct nfs4_client *nfs4_client_alloc(const client_owner4 *owner,
				      const verifier4 *verifier,
				      const struct nfs_impl_id4 *impl_id,
				      const struct sockaddr_in *sin,
				      uint16_t incarnation,
				      clientid4 assigned_id)
{
	struct nfs4_client *nc;
	int ret;

	nc = calloc(1, sizeof(*nc));
	if (!nc)
		return NULL;

	memcpy(&nc->nc_owner, owner, sizeof(*owner));
	memcpy(&nc->nc_verifier, verifier, sizeof(*verifier));
	memcpy(&nc->nc_sin, sin, sizeof(*sin));
	nc->nc_incarnation = incarnation;
	nc->nc_confirmed = false;

	if (impl_id) {
		if (impl_id->nii_domain.utf8string_val) {
			nc->nc_domain =
				strdup(impl_id->nii_domain.utf8string_val);
			if (!nc->nc_domain)
				goto err_free;
		}
		if (impl_id->nii_name.utf8string_val) {
			nc->nc_name = strdup(impl_id->nii_name.utf8string_val);
			if (!nc->nc_name)
				goto err_free;
		}
	}

	ret = client_assign(&nc->nc_client, (uint64_t)assigned_id,
			    nfs4_client_free_rcu, nfs4_client_release);
	if (ret)
		goto err_free;

	return nc;

err_free:
	free(nc->nc_name);
	free(nc->nc_domain);
	free(nc);
	return NULL;
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
struct nfs4_client *nfs4_client_find_by_owner(const client_owner4 *owner)
{
	/* NOT_NOW_BROWN_COW: implement owner-keyed lookup */
	(void)owner;
	return NULL;
}
