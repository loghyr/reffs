/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include "nfsv42_xdr.h"
#include "nfsv42_names.h"
#include "reffs/log.h"
#include "reffs/rpc.h"
#include "reffs/inode.h"
#include "reffs/stateid.h"
#include "nfs4/compound.h"
#include "nfs4/ops.h"
#include "nfs4/errors.h"
#include "nfs4/stateid.h"
#include "nfs4/client.h"

void nfs4_op_delegpurge(struct compound *c)
{
	DELEGPURGE4args *args = NFS4_OP_ARG_SETUP(c, opdelegpurge);
	DELEGPURGE4res *res = NFS4_OP_RES_SETUP(c, opdelegpurge);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_delegreturn(struct compound *c)
{
	DELEGRETURN4args *args = NFS4_OP_ARG_SETUP(c, opdelegreturn);
	DELEGRETURN4res *res = NFS4_OP_RES_SETUP(c, opdelegreturn);
	nfsstat4 *status = &res->status;

	if (network_file_handle_empty(&c->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		goto out;
	}

	if (stateid4_is_special(&args->deleg_stateid)) {
		*status = NFS4ERR_BAD_STATEID;
		goto out;
	}

	uint32_t seqid, id, type, cookie;
	unpack_stateid4(&args->deleg_stateid, &seqid, &id, &type, &cookie);

	if (type != Delegation_Stateid) {
		*status = NFS4ERR_BAD_STATEID;
		goto out;
	}

	struct stateid *stid = stateid_find(c->c_inode, id);
	if (!stid || stid->s_tag != Delegation_Stateid ||
	    stid->s_cookie != cookie) {
		stateid_put(stid);
		*status = NFS4ERR_BAD_STATEID;
		goto out;
	}

	if (c->c_nfs4_client &&
	    stid->s_client != nfs4_client_to_client(c->c_nfs4_client)) {
		stateid_put(stid);
		*status = NFS4ERR_BAD_STATEID;
		goto out;
	}

	/* Unhash and free the delegation stateid. */
	stateid_inode_unhash(stid);
	stateid_client_unhash(stid);

	if (c->c_curr_stid == stid) {
		stateid_put(stid); /* put c_curr_stid ref */
		c->c_curr_stid = NULL;
	}

	stateid_put(stid); /* find ref */
	stateid_put(stid); /* state ref → freed */

	*status = NFS4_OK;

out:
	LOG("%s status=%s(%d)", __func__, nfs4_err_name(*status), *status);
}

void nfs4_op_get_dir_delegation(struct compound *c)
{
	GET_DIR_DELEGATION4args *args =
		NFS4_OP_ARG_SETUP(c, opget_dir_delegation);
	GET_DIR_DELEGATION4res *res =
		NFS4_OP_RES_SETUP(c, opget_dir_delegation);
	nfsstat4 *status = &res->gddr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_want_delegation(struct compound *c)
{
	WANT_DELEGATION4args *args = NFS4_OP_ARG_SETUP(c, opwant_delegation);
	WANT_DELEGATION4res *res = NFS4_OP_RES_SETUP(c, opwant_delegation);
	nfsstat4 *status = &res->wdr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}
