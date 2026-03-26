/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <pthread.h>

#include "nfsv42_xdr.h"
#include "nfsv42_names.h"
#include "reffs/log.h"
#include "reffs/rpc.h"
#include "reffs/inode.h"
#include "reffs/lock.h"
#include "reffs/stateid.h"
#include "nfs4/compound.h"
#include "nfs4/ops.h"
#include "nfs4/errors.h"
#include "nfs4/stateid.h"
#include "nfs4/client.h"

uint32_t nfs4_op_delegpurge(struct compound *compound)
{
	DELEGPURGE4res *res = NFS4_OP_RES_SETUP(compound, opdelegpurge);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}

uint32_t nfs4_op_delegreturn(struct compound *compound)
{
	DELEGRETURN4args *args = NFS4_OP_ARG_SETUP(compound, opdelegreturn);
	DELEGRETURN4res *res = NFS4_OP_RES_SETUP(compound, opdelegreturn);
	nfsstat4 *status = &res->status;

	if (network_file_handle_empty(&compound->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		return 0;
	}

	if (stateid4_is_special(&args->deleg_stateid)) {
		*status = NFS4ERR_BAD_STATEID;
		return 0;
	}

	uint32_t seqid, id, type, cookie;
	unpack_stateid4(&args->deleg_stateid, &seqid, &id, &type, &cookie);

	if (type != Delegation_Stateid) {
		*status = NFS4ERR_BAD_STATEID;
		return 0;
	}

	struct stateid *stid = stateid_find(compound->c_inode, id);
	if (!stid || stid->s_tag != Delegation_Stateid ||
	    stid->s_cookie != cookie) {
		stateid_put(stid);
		*status = NFS4ERR_BAD_STATEID;
		return 0;
	}

	if (compound->c_nfs4_client &&
	    stid->s_client != nfs4_client_to_client(compound->c_nfs4_client)) {
		stateid_put(stid);
		*status = NFS4ERR_BAD_STATEID;
		return 0;
	}

	uint32_t cur_seqid = __atomic_load_n(&stid->s_seqid, __ATOMIC_RELAXED);
	if (seqid != 0) {
		if (seqid < cur_seqid) {
			stateid_put(stid);
			*status = NFS4ERR_OLD_STATEID;
			return 0;
		}
		if (seqid > cur_seqid) {
			stateid_put(stid);
			*status = NFS4ERR_BAD_STATEID;
			return 0;
		}
	}

	struct delegation_stateid *ds = stid_to_delegation(stid);

	/*
	 * RFC 9754 OPEN XOR: if this delegation subsumed an open, clean
	 * up the internal open_stateid (share removal + unhash + free)
	 * now, since the client will not send a separate CLOSE.
	 */
	if (ds->ds_open) {
		struct open_stateid *os = ds->ds_open;
		ds->ds_open = NULL;
		pthread_mutex_lock(&compound->c_inode->i_lock_mutex);
		reffs_share_remove(compound->c_inode, &os->os_owner, NULL);
		pthread_mutex_unlock(&compound->c_inode->i_lock_mutex);
		stateid_inode_unhash(&os->os_stid);
		stateid_client_unhash(&os->os_stid);
		stateid_put(&os->os_stid); /* state ref → freed via RCU */
	}

	/* Unhash and free the delegation stateid. */
	stateid_inode_unhash(stid);
	stateid_client_unhash(stid);

	if (compound->c_curr_stid == stid) {
		stateid_put(stid); /* put c_curr_stid ref */
		compound->c_curr_stid = NULL;
	}

	stateid_put(stid); /* find ref */
	stateid_put(stid); /* state ref → freed */

	return 0;
}

uint32_t nfs4_op_get_dir_delegation(struct compound *compound)
{
	GET_DIR_DELEGATION4res *res =
		NFS4_OP_RES_SETUP(compound, opget_dir_delegation);
	nfsstat4 *status = &res->gddr_status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}

uint32_t nfs4_op_want_delegation(struct compound *compound)
{
	WANT_DELEGATION4res *res =
		NFS4_OP_RES_SETUP(compound, opwant_delegation);
	nfsstat4 *status = &res->wdr_status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}
