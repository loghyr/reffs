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
#include <stdbool.h>
#include <netinet/in.h>

#include "nfsv42_xdr.h"
#include "reffs/log.h"
#include "reffs/rcu.h"
#include "reffs/network.h"
#include "reffs/client.h"
#include "reffs/client_persist.h"
#include "reffs/server.h"
#include "nfs4/trace/nfs4.h"
#include "reffs/trace/fs.h"
#include "nfs4/client.h"
#include "nfs4/client_persist.h"

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */

static bool verifier_eq(const verifier4 *a, const verifier4 *b)
{
	return memcmp(a, b, NFS4_VERIFIER_SIZE) == 0;
}

static bool addr_eq(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
	return a->sin_addr.s_addr == b->sin_addr.s_addr &&
	       a->sin_port == b->sin_port;
}

/*
 * Build a client_incarnation_record from current server state and wire
 * parameters.
 */
static void make_incarnation_record(struct client_incarnation_record *crc,
				    const struct server_state *ss,
				    uint32_t slot, uint16_t incarnation,
				    const verifier4 *verifier,
				    const struct sockaddr_in *sin)
{
	memset(crc, 0, sizeof(*crc));
	crc->crc_magic = CLIENT_INCARNATION_MAGIC;
	crc->crc_slot = slot;
	crc->crc_boot_seq = server_boot_seq(ss);
	crc->crc_incarnation = incarnation;
	memcpy(crc->crc_verifier, verifier, NFS4_VERIFIER_SIZE);
	sockaddr_in_to_full_str(sin, crc->crc_addr, sizeof(crc->crc_addr));
}

/*
 * Build a client_identity_record from wire parameters.
 *
 * impl_id (domain + name) is consumed here and only here — these are
 * persistence fields that must not be stored on struct nfs4_client.
 */
static void make_identity_record(struct client_identity_record *cir,
				 uint32_t slot, const client_owner4 *owner,
				 const struct nfs_impl_id4 *impl_id)
{
	uint16_t oid_len;

	memset(cir, 0, sizeof(*cir));
	cir->cir_magic = CLIENT_IDENTITY_MAGIC;
	cir->cir_slot = slot;

	oid_len = (uint16_t)owner->co_ownerid.co_ownerid_len;
	if (oid_len > CLIENT_OWNERID_MAX)
		oid_len = CLIENT_OWNERID_MAX;
	cir->cir_ownerid_len = oid_len;
	memcpy(cir->cir_ownerid, owner->co_ownerid.co_ownerid_val, oid_len);

	if (impl_id) {
		if (impl_id->nii_domain.utf8string_val) {
			uint32_t len = impl_id->nii_domain.utf8string_len;
			if (len > sizeof(cir->cir_domain) - 1)
				len = sizeof(cir->cir_domain) - 1;
			memcpy(cir->cir_domain,
			       impl_id->nii_domain.utf8string_val, len);
		}
		if (impl_id->nii_name.utf8string_val) {
			uint32_t len = impl_id->nii_name.utf8string_len;
			if (len > sizeof(cir->cir_name) - 1)
				len = sizeof(cir->cir_name) - 1;
			memcpy(cir->cir_name, impl_id->nii_name.utf8string_val,
			       len);
		}
	}
}

/* ------------------------------------------------------------------ */
/* Expire                                                              */

void nfs4_client_expire(struct server_state *ss, struct nfs4_client *nc)
{
	struct client *client = nfs4_client_to_client(nc);
	uint32_t slot = (uint32_t)client->c_id;

	/*
	 * Order matters — see handoff invariants:
	 *   1. Remove from incarnations file — crash after this but
	 *      before step 2 is safe; next boot skips recovery for slot.
	 *   2. Drain stateids.
	 *   3. Unhash and drop the client ref.
	 */
	if (client_incarnation_remove(ss->ss_state_dir, slot))
		LOG("nfs4_client_expire: slot %u not in incarnations", slot);

	trace_fs_client(client, __func__, __LINE__);
	__atomic_fetch_or(&client->c_state, CLIENT_IS_EXPIRING,
			  __ATOMIC_RELEASE);
	client_remove_all_stateids(client);
	if (client_unhash(client))
		client_put(client);
	client_put(client);
}

/* ------------------------------------------------------------------ */
/* Alloc or find                                                       */

struct nfs4_client *
nfs4_client_alloc_or_find(struct server_state *ss, const client_owner4 *owner,
			  const struct nfs_impl_id4 *impl_id,
			  const verifier4 *verifier,
			  const struct sockaddr_in *sin)
{
	struct nfs4_client *nc;
	struct client_identity_record cir;
	struct client_incarnation_record crc;
	uint32_t slot;
	uint16_t incarnation;
	clientid4 clid;
	char addr_existing[REFFS_ADDR_LEN];
	char addr_new[REFFS_ADDR_LEN];

	/*
	 * Scan the clients file for this ownerid → slot, then look up
	 * the active incarnation → clientid4 → in-memory client.
	 * Disk IO here is expected; EXCHANGE_ID is not a fast path.
	 */
	uint32_t prev_slot = UINT32_MAX;
	nc = nfs4_client_find_by_owner(ss->ss_state_dir, server_boot_seq(ss),
				       owner, &prev_slot);
	if (!nc) {
		bool is_reclaiming = (prev_slot != UINT32_MAX);

		if (is_reclaiming) {
			/* ------------------------------------------ */
			/* Known ownerid — first reconnect this boot. */
			/* Reuse the existing slot; no new identity    */
			/* record (the ownerid→slot mapping persists). */

			slot = prev_slot;
			incarnation = 0;
			clid = clientid_make(slot, incarnation,
					     server_boot_seq(ss));

			/*
			 * Remove the stale previous-boot incarnation so
			 * nfs4_client_find_by_owner won't see two entries
			 * for the same slot after we add the new one.
			 */
			client_incarnation_remove(ss->ss_state_dir, slot);
		} else {
			/* ------------------------------------------ */
			/* New client — never seen this ownerid.      */

			slot = server_alloc_client_slot(ss);
			if (slot == UINT32_MAX)
				return NULL;

			incarnation = 0;
			clid = clientid_make(slot, incarnation,
					     server_boot_seq(ss));

			/*
			 * Write identity record first (ownerid + domain +
			 * name to disk).  This is the only place impl_id
			 * is consumed.
			 */
			make_identity_record(&cir, slot, owner, impl_id);
			if (client_identity_append(ss->ss_state_dir, &cir))
				return NULL;
		}

		nc = nfs4_client_alloc(verifier, sin, incarnation, clid);
		if (!nc)
			return NULL;

		nc->nc_needs_reclaim = is_reclaiming;

		make_incarnation_record(&crc, ss, slot, incarnation, verifier,
					sin);
		if (client_incarnation_add(ss->ss_state_dir, &crc)) {
			client_put(nfs4_client_to_client(nc));
			return NULL;
		}

		return nc;
	}

	/* ---------------------------------------------------------- */
	/* Known ownerid — walk the decision tree.                    */

	bool same_verifier = verifier_eq(&nc->nc_verifier, verifier);
	bool same_addr = addr_eq(&nc->nc_sin, sin);

	if (same_verifier && same_addr) {
		/* Idempotent retry — return existing client. */
		return nc;
	}

	if (same_verifier && !same_addr) {
		/*
		 * Same verifier, different address — multi-homed client.
		 * Return the existing client unchanged.
		 */
		LOG("nfs4_client_alloc_or_find: slot %u multi-homed "
		    "(new addr differs, verifier matches)",
		    (uint32_t)nfs4_client_to_client(nc)->c_id);
		return nc;
	}

	if (!same_verifier && !same_addr) {
		/*
		 * Different verifier AND different address — two distinct
		 * clients claim the same co_ownerid.  Refuse; caller
		 * returns NFS4ERR_CLID_INUSE.
		 */
		sockaddr_in_to_full_str(&nc->nc_sin, addr_existing,
					sizeof(addr_existing));
		sockaddr_in_to_full_str(sin, addr_new, sizeof(addr_new));
		LOG("nfs4_client_alloc_or_find: co_ownerid collision "
		    "— existing client at %s, new request from %s "
		    "— possible misconfiguration",
		    addr_existing, addr_new);
		client_put(nfs4_client_to_client(nc));
		return NULL;
	}

	/* !same_verifier && same_addr — client restarted. */

	slot = (uint32_t)nfs4_client_to_client(nc)->c_id;
	incarnation = nc->nc_incarnation + 1;
	clid = clientid_make(slot, incarnation, server_boot_seq(ss));

	/*
	 * Expire the old client — removes from incarnations file,
	 * drains stateids, drops ref.  nc is invalid after this.
	 */
	nfs4_client_expire(ss, nc);
	nc = NULL;

	nc = nfs4_client_alloc(verifier, sin, incarnation, clid);
	if (!nc)
		return NULL;

	make_incarnation_record(&crc, ss, slot, incarnation, verifier, sin);
	if (client_incarnation_add(ss->ss_state_dir, &crc)) {
		client_put(nfs4_client_to_client(nc));
		return NULL;
	}

	/*
	 * No new identity record on restart: the ownerid→slot mapping
	 * in the clients file is permanent for this slot.
	 */
	return nc;
}
