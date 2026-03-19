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
#include <errno.h>

#include <urcu/compiler.h>
#include <urcu/rculfhash.h>

#include "nfsv42_xdr.h"
#include "reffs/log.h"
#include "reffs/rcu.h"
#include "reffs/client.h"
#include "reffs/client_persist.h"
#include "nfs4/trace/nfs4.h"
#include "nfs4/client.h"

/* ------------------------------------------------------------------ */
/* RCU / release callbacks                                             */

static void nfs4_client_free_rcu(struct rcu_head *rcu)
{
	struct client *client = caa_container_of(rcu, struct client, c_rcu);
	struct nfs4_client *nc = client_to_nfs4(client);

	if (client->c_stateids)
		cds_lfht_destroy(client->c_stateids, NULL);

	free(nc);
}

static void nfs4_client_release(struct urcu_ref *ref)
{
	struct client *client = caa_container_of(ref, struct client, c_ref);

	call_rcu(&client->c_rcu, nfs4_client_free_rcu);
}

struct nfs4_client *nfs4_client_get(struct nfs4_client *nc)
{
	if (!nc)
		return NULL;

	return client_get(&nc->nc_client) ? nc : NULL;
}

void nfs4_client_put(struct nfs4_client *nc)
{
	if (nc)
		client_put(&nc->nc_client);
}

/* ------------------------------------------------------------------ */
/* Alloc                                                               */

struct nfs4_client *nfs4_client_alloc(const verifier4 *verifier,
				      const struct sockaddr_in *sin,
				      uint16_t incarnation,
				      clientid4 assigned_id)
{
	struct nfs4_client *nc;
	int ret;

	nc = calloc(1, sizeof(*nc));
	if (!nc)
		return NULL;

	memcpy(&nc->nc_verifier, verifier, sizeof(*verifier));
	memcpy(&nc->nc_sin, sin, sizeof(*sin));
	nc->nc_incarnation = incarnation;
	nc->nc_confirmed = false;

	ret = client_assign(&nc->nc_client, (uint64_t)assigned_id,
			    nfs4_client_free_rcu, nfs4_client_release);
	if (ret) {
		free(nc);
		return NULL;
	}

	return nc;
}

/* ------------------------------------------------------------------ */
/* Find by clientid4                                                   */

struct nfs4_client *nfs4_client_find(clientid4 clid)
{
	struct client *client = client_find((uint64_t)clid);

	if (!client)
		return NULL;

	return client_to_nfs4(client);
}

/* ------------------------------------------------------------------ */
/* Find by owner (EXCHANGE_ID path — disk IO acceptable)              */

struct find_owner_cb_arg {
	const client_owner4 *owner;
	uint32_t slot; /* UINT32_MAX until matched */
};

static int find_owner_cb(const struct client_identity_record *cir, void *arg)
{
	struct find_owner_cb_arg *a = arg;

	if (cir->cir_ownerid_len != a->owner->co_ownerid.co_ownerid_len)
		return 0;
	if (memcmp(cir->cir_ownerid, a->owner->co_ownerid.co_ownerid_val,
		   cir->cir_ownerid_len) != 0)
		return 0;

	a->slot = cir->cir_slot;
	return 1; /* stop iteration */
}

struct nfs4_client *nfs4_client_find_by_owner(const char *state_dir,
					      uint16_t boot_seq,
					      const client_owner4 *owner,
					      uint32_t *out_slot)
{
	struct find_owner_cb_arg cb_arg;
	struct client_incarnation_record incs[CLIENT_INCARNATION_MAX];
	size_t nincs = 0;
	int ret;
	struct nfs4_client *nc = NULL;

	if (out_slot)
		*out_slot = UINT32_MAX;

	cb_arg.owner = owner;
	cb_arg.slot = UINT32_MAX;

	ret = client_identity_load(state_dir, find_owner_cb, &cb_arg);
	if (ret < 0)
		return NULL;
	if (cb_arg.slot == UINT32_MAX)
		return NULL;

	if (out_slot)
		*out_slot = cb_arg.slot;

	ret = client_incarnation_load(state_dir, incs, CLIENT_INCARNATION_MAX,
				      &nincs);
	if (ret < 0)
		return NULL;

	for (size_t i = 0; i < nincs; i++) {
		if (incs[i].crc_slot != cb_arg.slot)
			continue;

		clientid4 clid = clientid_make(
			cb_arg.slot, incs[i].crc_incarnation, boot_seq);
		nc = nfs4_client_find(clid);
		break;
	}

	return nc;
}
