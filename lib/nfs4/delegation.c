/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include "nfsv42_xdr.h"
#include "reffs/log.h"
#include "reffs/rpc.h"
#include "compound.h"
#include "ops.h"
#include "errors.h"

void nfs4_op_delegpurge(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	DELEGPURGE4args *args = NFS4_OP_ARG_SETUP(c, ph, opdelegpurge);
	DELEGPURGE4res *res = NFS4_OP_RES_SETUP(c, ph, opdelegpurge);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_delegreturn(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	DELEGRETURN4args *args = NFS4_OP_ARG_SETUP(c, ph, opdelegreturn);
	DELEGRETURN4res *res = NFS4_OP_RES_SETUP(c, ph, opdelegreturn);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_get_dir_delegation(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	GET_DIR_DELEGATION4args *args =
		NFS4_OP_ARG_SETUP(c, ph, opget_dir_delegation);
	GET_DIR_DELEGATION4res *res =
		NFS4_OP_RES_SETUP(c, ph, opget_dir_delegation);
	nfsstat4 *status = &res->gddr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_want_delegation(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	WANT_DELEGATION4args *args =
		NFS4_OP_ARG_SETUP(c, ph, opwant_delegation);
	WANT_DELEGATION4res *res = NFS4_OP_RES_SETUP(c, ph, opwant_delegation);
	nfsstat4 *status = &res->wdr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}
