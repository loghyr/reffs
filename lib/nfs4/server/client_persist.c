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
#include "reffs/dstore.h"
#include "reffs/dstore_ops.h"
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
 * impl_id (domain + name) is consumed here and only here -- these are
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
	 * Tight-coupling: revoke all layout stateids for this client on
	 * every DS that supports TRUST_STATEID.  Best-effort -- expiry
	 * proceeds regardless of whether revocation succeeds.
	 *
	 * Use collect_all (not collect_available): drained dstores still
	 * carry live trust-stateid tables, so a client whose lease
	 * expires while it has stateids on a drained dstore must still
	 * have them revoked there.  The TRUST_STATEID RPC itself is
	 * gated on connectivity inside dstore_bulk_revoke_stateid.
	 *
	 * clientid4 == client->c_id (nfs4 layer stores clientid4 in c_id).
	 */
	{
		struct dstore *revoke_dstores[DSTORE_REVOKE_MAX];
		uint32_t nrevoke =
			dstore_collect_all(revoke_dstores, DSTORE_REVOKE_MAX);
		uint64_t clientid = client->c_id;

		for (uint32_t ri = 0; ri < nrevoke; ri++) {
			if (revoke_dstores[ri]->ds_tight_coupled &&
			    dstore_is_connected(revoke_dstores[ri]))
				dstore_bulk_revoke_stateid(revoke_dstores[ri],
							   clientid);
			dstore_put(revoke_dstores[ri]);
		}
	}

	/*
	 * Order matters -- see handoff invariants:
	 *   1. Remove from incarnations file -- crash after this but
	 *      before step 2 is safe; next boot skips recovery for slot.
	 *   2. Drain stateids.
	 *   3. Unhash and drop the client ref.
	 */
	if (ss->ss_persist_ops->client_incarnation_remove(ss->ss_persist_ctx,
							  slot))
		LOG("nfs4_client_expire: slot %u not in incarnations", slot);

	trace_fs_client(client, __func__, __LINE__);
	__atomic_fetch_or(&client->c_state, CLIENT_IS_EXPIRING,
			  __ATOMIC_RELEASE);
	/*
	 * Destroy remaining sessions on this client.  For RFC 8881
	 * S18.35.4 case 7 (replace_client), sessions are re-parented
	 * to the new client as zombies BEFORE expire is called, so
	 * nc_session_count is already 0 and this is a no-op.
	 */
	nfs4_session_destroy_for_client(ss, nc);
	client_remove_all_stateids(client);
	if (client_unhash(client))
		client_put(client);
	client_put(client);
}

/* ------------------------------------------------------------------ */
/* Alloc or find                                                       */

/*
 * Replace an existing client: expire the old one, allocate a new one
 * with bumped incarnation.  Used by RFC 8881 Table 11 cases where
 * the verifier or principal changed.
 */
static struct nfs4_client *replace_client(struct server_state *ss,
					  struct nfs4_client *old_nc,
					  const verifier4 *verifier,
					  const struct sockaddr_in *sin,
					  uid_t principal_uid)
{
	struct client_incarnation_record crc;
	uint32_t slot;
	uint16_t incarnation;
	clientid4 clid;
	struct nfs4_client *nc;

	slot = clientid_slot((clientid4)nfs4_client_to_client(old_nc)->c_id);
	incarnation = old_nc->nc_incarnation + 1;
	clid = clientid_make(slot, incarnation, server_boot_seq(ss));

	nc = nfs4_client_alloc(verifier, sin, incarnation, clid, principal_uid);
	if (!nc) {
		client_put(nfs4_client_to_client(old_nc));
		return NULL;
	}

	/*
	 * RFC 8881 S18.35.4 case 7: re-parent the old client's sessions
	 * to the new client as zombies.  The sessions remain valid for
	 * SEQUENCE until CREATE_SESSION confirms the new client and
	 * destroys the zombies.  After re-parenting, the old client has
	 * no sessions and can be safely expired.
	 */
	nfs4_session_reparent_for_replace(ss, old_nc, nc);
	nfs4_client_expire(ss, old_nc);

	make_incarnation_record(&crc, ss, slot, incarnation, verifier, sin);
	if (ss->ss_persist_ops->client_incarnation_add(ss->ss_persist_ctx,
						       &crc)) {
		client_put(nfs4_client_to_client(nc));
		return NULL;
	}

	return nc;
}

struct nfs4_client *
nfs4_client_alloc_or_find(struct server_state *ss, const client_owner4 *owner,
			  const struct nfs_impl_id4 *impl_id,
			  const verifier4 *verifier,
			  const struct sockaddr_in *sin, uid_t principal_uid,
			  bool update, nfsstat4 *out_status)
{
	struct nfs4_client *nc;
	struct client_identity_record cir;
	struct client_incarnation_record crc;
	uint32_t slot;
	uint16_t incarnation;
	clientid4 clid;

	*out_status = 0;

	/*
	 * Scan the clients file for this ownerid --> slot, then look up
	 * the active incarnation --> clientid4 --> in-memory client.
	 * Disk IO here is expected; EXCHANGE_ID is not a fast path.
	 */
	uint32_t prev_slot = UINT32_MAX;
	nc = nfs4_client_find_by_owner(ss, server_boot_seq(ss), owner,
				       &prev_slot);
	if (!nc) {
		/*
		 * RFC 8881 S18.35.4: update on a non-existent record
		 * is NFS4ERR_NOENT.
		 */
		if (update) {
			*out_status = NFS4ERR_NOENT;
			return NULL;
		}

		bool is_reclaiming = (prev_slot != UINT32_MAX);

		if (is_reclaiming) {
			/* ------------------------------------------ */
			/* Known ownerid -- first reconnect this boot. */
			/* Reuse the existing slot; no new identity    */
			/* record (the ownerid-->slot mapping persists). */

			slot = prev_slot;
			incarnation = 0;
			clid = clientid_make(slot, incarnation,
					     server_boot_seq(ss));

			/*
			 * Remove the stale previous-boot incarnation so
			 * nfs4_client_find_by_owner won't see two entries
			 * for the same slot after we add the new one.
			 */
			ss->ss_persist_ops->client_incarnation_remove(
				ss->ss_persist_ctx, slot);
		} else {
			/* ------------------------------------------ */
			/* New client -- never seen this ownerid.      */

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
			if (ss->ss_persist_ops->client_identity_append(
				    ss->ss_persist_ctx, &cir))
				return NULL;
		}

		nc = nfs4_client_alloc(verifier, sin, incarnation, clid,
				       principal_uid);
		if (!nc)
			return NULL;

		nc->nc_needs_reclaim = is_reclaiming;

		make_incarnation_record(&crc, ss, slot, incarnation, verifier,
					sin);
		if (ss->ss_persist_ops->client_incarnation_add(
			    ss->ss_persist_ctx, &crc)) {
			client_put(nfs4_client_to_client(nc));
			return NULL;
		}

		return nc;
	}

	/* ---------------------------------------------------------- */
	/* Known ownerid -- RFC 8881 S18.35.4 Table 11 decision tree.  */
	/*                                                             */
	/* The 3-bit key is (same_verifier, same_principal, confirmed).*/
	/* Only case 5 (confirmed + same verifier + same principal)    */
	/* returns the existing client.  All other non-update cases    */
	/* replace the old client with a new one (including case 1:    */
	/* unconfirmed + same verifier + same principal).              */

	bool same_verifier = verifier_eq(&nc->nc_verifier, verifier);
	bool same_principal = (nc->nc_principal_uid == principal_uid);
	bool confirmed = nc->nc_confirmed;

	if (update) {
		/*
		 * RFC 8881 S18.35.4 update path (UPD_CONFIRMED_REC_A).
		 * Only valid against a confirmed record with matching
		 * verifier and principal.
		 */
		if (!confirmed) {
			/* U1: unconfirmed + update --> NFS4ERR_NOENT */
			*out_status = NFS4ERR_NOENT;
			client_put(nfs4_client_to_client(nc));
			return NULL;
		}
		if (!same_principal) {
			/* U6/U8: different principal --> NFS4ERR_PERM */
			*out_status = NFS4ERR_PERM;
			client_put(nfs4_client_to_client(nc));
			return NULL;
		}
		if (!same_verifier) {
			/* U7: same principal, different verifier */
			*out_status = NFS4ERR_NOT_SAME;
			client_put(nfs4_client_to_client(nc));
			return NULL;
		}
		/* U5: confirmed, same verifier, same principal --> update */
		return nc;
	}

	/* Non-update path. */
	if (same_verifier && same_principal && confirmed) {
		/*
		 * Case 5 (confirmed, same verifier + principal): idempotent
		 * retry -- return the existing client record.
		 */
		return nc;
	}

	/*
	 * Cases 2,3,4 (unconfirmed) and 6,7,8 (confirmed): verifier
	 * or principal changed.  Expire the old client and allocate a
	 * new one.  This covers:
	 *   - same_verifier + diff principal (cases 2, 6)
	 *   - diff verifier + same principal (cases 3, 7)
	 *   - diff verifier + diff principal (cases 4, 8)
	 */
	return replace_client(ss, nc, verifier, sin, principal_uid);
}
