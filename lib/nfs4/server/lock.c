/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "nfsv42_xdr.h"
#include "nfsv42_names.h"
#include "reffs/log.h"
#include "reffs/rpc.h"
#include "reffs/inode.h"
#include "reffs/lock.h"
#include "reffs/server.h"
#include "nfs4/compound.h"
#include "nfs4/ops.h"
#include "nfs4/errors.h"
#include "nfs4/stateid.h"
#include "nfs4/client.h"

/* ------------------------------------------------------------------ */
/* Lock Owner Management                                               */

static void nfs4_lock_owner_release(struct urcu_ref *ref)
{
	struct nfs4_lock_owner __attribute__((unused)) *lo =
		caa_container_of(ref, struct nfs4_lock_owner, lo_base.lo_ref);

	/*
	 * Note: We don't remove from nc_lock_owners here because the client
	 * might still be active and we want to reuse the owner.
	 * The owner is freed when the client is freed, or explicitly via
	 * RELEASE_LOCKOWNER.
	 * Actually, for NFSv4, lock owners are often kept until explicitly
	 * released.
	 */
}

static bool nfs4_lock_owner_match(struct reffs_lock_owner *lo_base, void *arg)
{
	struct nfs4_lock_owner *lo =
		caa_container_of(lo_base, struct nfs4_lock_owner, lo_base);
	lock_owner4 *owner = arg;

	if (lo->lo_clientid != owner->clientid)
		return false;
	if (lo->lo_owner.n_len != owner->owner.owner_len)
		return false;
	return memcmp(lo->lo_owner.n_bytes, owner->owner.owner_val,
		      owner->owner.owner_len) == 0;
}

static struct nfs4_lock_owner *nfs4_get_lock_owner(struct nfs4_client *nc,
						   lock_owner4 *owner)
{
	struct nfs4_lock_owner *lo;

	pthread_mutex_lock(&nc->nc_lock_owners_mutex);
	cds_list_for_each_entry(lo, &nc->nc_lock_owners, lo_base.lo_list) {
		if (nfs4_lock_owner_match(&lo->lo_base, owner)) {
			lock_owner_get(&lo->lo_base);
			pthread_mutex_unlock(&nc->nc_lock_owners_mutex);
			return lo;
		}
	}

	lo = calloc(1, sizeof(*lo));
	if (lo) {
		urcu_ref_init(&lo->lo_base.lo_ref);
		lo->lo_base.lo_release = nfs4_lock_owner_release;
		lo->lo_base.lo_match = nfs4_lock_owner_match;
		lo->lo_clientid = owner->clientid;
		lo->lo_owner.n_len = owner->owner.owner_len;
		lo->lo_owner.n_bytes = malloc(owner->owner.owner_len);
		if (!lo->lo_owner.n_bytes) {
			free(lo);
			pthread_mutex_unlock(&nc->nc_lock_owners_mutex);
			return NULL;
		}
		memcpy(lo->lo_owner.n_bytes, owner->owner.owner_val,
		       owner->owner.owner_len);
		cds_list_add(&lo->lo_base.lo_list, &nc->nc_lock_owners);
	}
	pthread_mutex_unlock(&nc->nc_lock_owners_mutex);
	return lo;
}

/* ------------------------------------------------------------------ */
/* Operation Handlers                                                  */

uint32_t nfs4_op_lock(struct compound *compound)
{
	LOCK4args *args = NFS4_OP_ARG_SETUP(compound, oplock);
	LOCK4res *res = NFS4_OP_RES_SETUP(compound, oplock);
	nfsstat4 *status = &res->status;
	LOCK4resok *resok = NFS4_OP_RESOK_SETUP(res, LOCK4res_u, resok4);
	struct lock_stateid *ls = NULL;
	struct open_stateid *os = NULL;
	struct nfs4_lock_owner *lo = NULL;
	struct reffs_lock *lock = NULL;
	struct reffs_lock *conflict = NULL;
	int ret;

	if (network_file_handle_empty(&compound->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		return 0;
	}

	if (server_in_grace(compound->c_server_state) && !args->reclaim) {
		*status = NFS4ERR_GRACE;
		return 0;
	}

	if (args->locker.new_lock_owner) {
		open_to_lock_owner4 *oto = &args->locker.locker4_u.open_owner;

		/* Resolve open_stateid — may be current stateid. */
		const stateid4 *lock_wire = &oto->open_stateid;
		stateid4 lock_resolved;

		if (stateid4_is_current(lock_wire)) {
			if (!compound->c_curr_stid) {
				*status = NFS4ERR_BAD_STATEID;
				return 0;
			}
			pack_stateid4(&lock_resolved, compound->c_curr_stid);
			lock_wire = &lock_resolved;
		}

		uint32_t seqid, id, type, cookie;
		unpack_stateid4(lock_wire, &seqid, &id, &type, &cookie);
		if (type != Open_Stateid) {
			*status = NFS4ERR_BAD_STATEID;
			return 0;
		}
		struct stateid *stid = stateid_find(compound->c_inode, id);
		if (!stid || stid->s_tag != Open_Stateid ||
		    stid->s_cookie != cookie) {
			stateid_put(stid);
			*status = NFS4ERR_BAD_STATEID;
			return 0;
		}
		os = stid_to_open(stid);

		/* Find or create lock_owner */
		lo = nfs4_get_lock_owner(compound->c_nfs4_client,
					 &oto->lock_owner);
		if (!lo) {
			stateid_put(stid);
			*status = NFS4ERR_DELAY;
			return 0;
		}

		/* Allocate new lock_stateid */
		ls = lock_stateid_alloc(
			compound->c_inode,
			nfs4_client_to_client(compound->c_nfs4_client));
		if (!ls) {
			lock_owner_put(&lo->lo_base);
			stateid_put(stid);
			*status = NFS4ERR_DELAY;
			return 0;
		}
		ls->ls_owner = lo; /* Ref already held from get_lock_owner */
		ls->ls_open = os; /* Ref already held from stateid_find */
		__atomic_fetch_add(&ls->ls_stid.s_seqid, 1, __ATOMIC_SEQ_CST);
	} else {
		exist_lock_owner4 *elo = &args->locker.locker4_u.lock_owner;

		/* Resolve lock_stateid */
		uint32_t seqid, id, type, cookie;
		unpack_stateid4(&elo->lock_stateid, &seqid, &id, &type,
				&cookie);
		if (type != Lock_Stateid) {
			*status = NFS4ERR_BAD_STATEID;
			return 0;
		}
		struct stateid *stid = stateid_find(compound->c_inode, id);
		if (!stid || stid->s_tag != Lock_Stateid ||
		    stid->s_cookie != cookie) {
			stateid_put(stid);
			*status = NFS4ERR_BAD_STATEID;
			return 0;
		}
		ls = stid_to_lock(stid);
		lo = ls->ls_owner;
		lock_owner_get(&lo->lo_base);

		/*
		 * RFC 8881 §8.2.2: in NFSv4.1+, lock_seqid is always 0
		 * and MUST be ignored by the server.  Do not validate it.
		 */
	}

	/* Perform locking */
	bool exclusive =
		(args->locktype == WRITE_LT || args->locktype == WRITEW_LT);

	pthread_mutex_lock(&compound->c_inode->i_lock_mutex);

	conflict = reffs_lock_find_conflict(compound->c_inode, args->offset,
					    args->length, exclusive,
					    &lo->lo_base, &args->locker);
	if (conflict) {
		res->LOCK4res_u.denied.offset = conflict->l_offset;
		res->LOCK4res_u.denied.length = conflict->l_len;
		res->LOCK4res_u.denied.locktype =
			conflict->l_exclusive ? WRITE_LT : READ_LT;
		/* Encode conflict owner - simplified for now */
		struct nfs4_lock_owner *clo = caa_container_of(
			conflict->l_owner, struct nfs4_lock_owner, lo_base);
		res->LOCK4res_u.denied.owner.clientid = clo->lo_clientid;
		res->LOCK4res_u.denied.owner.owner.owner_len =
			clo->lo_owner.n_len;
		res->LOCK4res_u.denied.owner.owner.owner_val =
			malloc(clo->lo_owner.n_len);
		if (res->LOCK4res_u.denied.owner.owner.owner_val) {
			memcpy(res->LOCK4res_u.denied.owner.owner.owner_val,
			       clo->lo_owner.n_bytes, clo->lo_owner.n_len);
		}

		pthread_mutex_unlock(&compound->c_inode->i_lock_mutex);
		if (args->locker.new_lock_owner) {
			stateid_inode_unhash(&ls->ls_stid);
			stateid_client_unhash(&ls->ls_stid);
			stateid_put(&ls->ls_stid);
		} else {
			lock_owner_put(&lo->lo_base);
			stateid_put(&ls->ls_stid);
		}
		*status = NFS4ERR_DENIED;
		return 0;
	}

	lock = calloc(1, sizeof(*lock));
	if (!lock) {
		pthread_mutex_unlock(&compound->c_inode->i_lock_mutex);
		if (args->locker.new_lock_owner) {
			stateid_inode_unhash(&ls->ls_stid);
			stateid_client_unhash(&ls->ls_stid);
			stateid_put(&ls->ls_stid);
		} else {
			lock_owner_put(&lo->lo_base);
			stateid_put(&ls->ls_stid);
		}
		*status = NFS4ERR_DELAY;
		return 0;
	}

	if (args->locker.new_lock_owner)
		lock_owner_get(&lo->lo_base);
	lock->l_owner = &lo->lo_base;
	lock->l_offset = args->offset;
	lock->l_len = args->length;
	lock->l_exclusive = exclusive;
	lock->l_inode = inode_active_get(compound->c_inode);

	ret = reffs_lock_add(compound->c_inode, lock, NULL);
	pthread_mutex_unlock(&compound->c_inode->i_lock_mutex);

	if (ret) {
		reffs_lock_free(lock);
		if (args->locker.new_lock_owner) {
			stateid_inode_unhash(&ls->ls_stid);
			stateid_client_unhash(&ls->ls_stid);
			stateid_put(&ls->ls_stid);
		} else {
			lock_owner_put(&lo->lo_base);
			stateid_put(&ls->ls_stid);
		}
		*status = NFS4ERR_DENIED;
		return 0;
	}

	if (!args->locker.new_lock_owner) {
		__atomic_fetch_add(&ls->ls_stid.s_seqid, 1, __ATOMIC_SEQ_CST);
	}

	pack_stateid4(&resok->lock_stateid, &ls->ls_stid);

	/* Update current stateid in compound */
	stateid_put(compound->c_curr_stid);
	compound->c_curr_stid = stateid_get(&ls->ls_stid);

	if (!args->locker.new_lock_owner) {
		urcu_ref_put(
			&lo->lo_base.lo_ref,
			lo->lo_base.lo_release); /* Drop ref from line 183 */
		stateid_put(&ls->ls_stid); /* Drop find ref */
	}

	return 0;
}

uint32_t nfs4_op_lockt(struct compound *compound)
{
	LOCKT4args *args = NFS4_OP_ARG_SETUP(compound, oplockt);
	LOCKT4res *res = NFS4_OP_RES_SETUP(compound, oplockt);
	nfsstat4 *status = &res->status;
	struct reffs_lock *conflict = NULL;

	if (network_file_handle_empty(&compound->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		return 0;
	}

	if (server_in_grace(compound->c_server_state)) {
		*status = NFS4ERR_GRACE;
		return 0;
	}

	bool exclusive =
		(args->locktype == WRITE_LT || args->locktype == WRITEW_LT);

	pthread_mutex_lock(&compound->c_inode->i_lock_mutex);
	conflict = reffs_lock_find_conflict(compound->c_inode, args->offset,
					    args->length, exclusive, NULL,
					    &args->owner);
	if (conflict) {
		res->LOCKT4res_u.denied.offset = conflict->l_offset;
		res->LOCKT4res_u.denied.length = conflict->l_len;
		res->LOCKT4res_u.denied.locktype =
			conflict->l_exclusive ? WRITE_LT : READ_LT;
		struct nfs4_lock_owner *clo = caa_container_of(
			conflict->l_owner, struct nfs4_lock_owner, lo_base);
		res->LOCKT4res_u.denied.owner.clientid = clo->lo_clientid;
		res->LOCKT4res_u.denied.owner.owner.owner_len =
			clo->lo_owner.n_len;
		res->LOCKT4res_u.denied.owner.owner.owner_val =
			malloc(clo->lo_owner.n_len);
		if (res->LOCKT4res_u.denied.owner.owner.owner_val) {
			memcpy(res->LOCKT4res_u.denied.owner.owner.owner_val,
			       clo->lo_owner.n_bytes, clo->lo_owner.n_len);
		}
		pthread_mutex_unlock(&compound->c_inode->i_lock_mutex);
		*status = NFS4ERR_DENIED;
		return 0;
	}
	pthread_mutex_unlock(&compound->c_inode->i_lock_mutex);

	return 0;
}

uint32_t nfs4_op_locku(struct compound *compound)
{
	LOCKU4args *args = NFS4_OP_ARG_SETUP(compound, oplocku);
	LOCKU4res *res = NFS4_OP_RES_SETUP(compound, oplocku);
	nfsstat4 *status = &res->status;

	if (network_file_handle_empty(&compound->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		return 0;
	}

	/* Resolve current stateid if used. */
	const stateid4 *wire_stid = &args->lock_stateid;
	stateid4 resolved_stid;

	if (stateid4_is_current(wire_stid)) {
		if (!compound->c_curr_stid) {
			*status = NFS4ERR_BAD_STATEID;
			return 0;
		}
		pack_stateid4(&resolved_stid, compound->c_curr_stid);
		wire_stid = &resolved_stid;
	}

	uint32_t seqid, id, type, cookie;
	unpack_stateid4(wire_stid, &seqid, &id, &type, &cookie);
	if (type != Lock_Stateid) {
		*status = NFS4ERR_BAD_STATEID;
		return 0;
	}
	struct stateid *stid = stateid_find(compound->c_inode, id);
	if (!stid || stid->s_tag != Lock_Stateid || stid->s_cookie != cookie) {
		stateid_put(stid);
		*status = NFS4ERR_BAD_STATEID;
		return 0;
	}
	struct lock_stateid *ls = stid_to_lock(stid);

	/* Verify stateid seqid (args->seqid is the lock-owner seqid, which
	 * NFSv4.1 clients always set to zero per RFC 5661 §8.2.2) */
	uint32_t cur_seqid = __atomic_load_n(&stid->s_seqid, __ATOMIC_RELAXED);
	if (seqid != 0 && seqid != cur_seqid) {
		stateid_put(stid);
		*status = NFS4ERR_BAD_SEQID;
		return 0;
	}

	pthread_mutex_lock(&compound->c_inode->i_lock_mutex);
	reffs_lock_remove(compound->c_inode, args->offset, args->length,
			  &ls->ls_owner->lo_base, NULL);
	pthread_mutex_unlock(&compound->c_inode->i_lock_mutex);

	__atomic_fetch_add(&stid->s_seqid, 1, __ATOMIC_SEQ_CST);
	pack_stateid4(&res->LOCKU4res_u.lock_stateid, stid);

	stateid_put(stid);

	return 0;
}

uint32_t nfs4_op_free_stateid(struct compound *compound)
{
	FREE_STATEID4args *args = NFS4_OP_ARG_SETUP(compound, opfree_stateid);
	FREE_STATEID4res *res = NFS4_OP_RES_SETUP(compound, opfree_stateid);
	nfsstat4 *status = &res->fsr_status;

	/* Resolve current stateid if used. */
	const stateid4 *wire = &args->fsa_stateid;
	stateid4 resolved;

	if (stateid4_is_current(wire)) {
		if (!compound->c_curr_stid) {
			*status = NFS4ERR_BAD_STATEID;
			return 0;
		}
		pack_stateid4(&resolved, compound->c_curr_stid);
		wire = &resolved;
	}

	uint32_t seqid, id, type, cookie;
	unpack_stateid4(wire, &seqid, &id, &type, &cookie);

	struct stateid *stid = stateid_find(compound->c_inode, id);
	if (!stid || stid->s_tag != type || stid->s_cookie != cookie) {
		stateid_put(stid);
		*status = NFS4ERR_BAD_STATEID;
		return 0;
	}

	/*
	 * RFC 8881 §18.38.3: FREE_STATEID on an open stateid while
	 * the share reservation is still active returns LOCKS_HELD.
	 * The client must CLOSE the file before freeing the stateid.
	 */
	if (type == Open_Stateid) {
		stateid_put(stid);
		*status = NFS4ERR_LOCKS_HELD;
		return 0;
	}

	/*
	 * Unhash atomically — if another FREE_STATEID already unhashed
	 * this stateid, bail out to prevent refcount underflow.
	 */
	if (!stateid_inode_unhash(stid)) {
		stateid_put(stid); /* drop the find ref */
		*status = NFS4ERR_BAD_STATEID;
		return 0;
	}
	stateid_client_unhash(stid);

	/* Drop the "state ref" */
	stateid_put(stid);
	/* Drop the "find ref" */
	stateid_put(stid);

	return 0;
}

uint32_t nfs4_op_release_lockowner(struct compound *compound)
{
	RELEASE_LOCKOWNER4args *args =
		NFS4_OP_ARG_SETUP(compound, oprelease_lockowner);
	RELEASE_LOCKOWNER4res *res =
		NFS4_OP_RES_SETUP(compound, oprelease_lockowner);
	nfsstat4 *status __attribute__((unused)) = &res->status;

	struct nfs4_client *nc = compound->c_nfs4_client;
	struct nfs4_lock_owner *lo, *tmp;

	pthread_mutex_lock(&nc->nc_lock_owners_mutex);
	cds_list_for_each_entry_safe(lo, tmp, &nc->nc_lock_owners,
				     lo_base.lo_list) {
		if (nfs4_lock_owner_match(&lo->lo_base, args)) {
			/*
			 * NOT_NOW_BROWN_COW: RFC 8881 §18.22.3 requires
			 * NFS4ERR_LOCKS_HELD if any locks are still held.
			 * Checking requires iterating all inodes for locks
			 * owned by this owner.  For now, unconditionally
			 * release.
			 */
			cds_list_del(&lo->lo_base.lo_list);
			lock_owner_put(&lo->lo_base);
			pthread_mutex_unlock(&nc->nc_lock_owners_mutex);
			return 0;
		}
	}
	pthread_mutex_unlock(&nc->nc_lock_owners_mutex);

	return 0;
}

uint32_t nfs4_op_test_stateid(struct compound *compound)
{
	TEST_STATEID4args *args = NFS4_OP_ARG_SETUP(compound, optest_stateid);
	TEST_STATEID4res *res = NFS4_OP_RES_SETUP(compound, optest_stateid);
	nfsstat4 *status = &res->tsr_status;

	/*
	 * NOT_NOW_BROWN_COW: properly validate each stateid by looking
	 * it up in the per-inode hash table.  Requires either a global
	 * stateid index or iterating all inodes.  For now, return OK
	 * for all stateids — this is incorrect per RFC 8881 §18.48
	 * but functional for non-adversarial clients.
	 */
	res->TEST_STATEID4res_u.tsr_resok4.tsr_status_codes
		.tsr_status_codes_len = args->ts_stateids.ts_stateids_len;
	res->TEST_STATEID4res_u.tsr_resok4.tsr_status_codes
		.tsr_status_codes_val =
		calloc(res->TEST_STATEID4res_u.tsr_resok4.tsr_status_codes
			       .tsr_status_codes_len,
		       sizeof(nfsstat4));
	if (!res->TEST_STATEID4res_u.tsr_resok4.tsr_status_codes
		     .tsr_status_codes_val) {
		*status = NFS4ERR_DELAY;
		return 0;
	}

	for (uint32_t i = 0; i < res->TEST_STATEID4res_u.tsr_resok4
					 .tsr_status_codes.tsr_status_codes_len;
	     i++) {
		res->TEST_STATEID4res_u.tsr_resok4.tsr_status_codes
			.tsr_status_codes_val[i] = NFS4_OK;
	}

	return 0;
}
