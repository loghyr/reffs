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

void nfs4_op_getxattr(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	GETXATTR4args *args = NFS4_OP_ARG_SETUP(c, ph, opgetxattr);
	GETXATTR4res *res = NFS4_OP_RES_SETUP(c, ph, opgetxattr);
	nfsstat4 *status = &res->gxr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_setxattr(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	SETXATTR4args *args = NFS4_OP_ARG_SETUP(c, ph, opsetxattr);
	SETXATTR4res *res = NFS4_OP_RES_SETUP(c, ph, opsetxattr);
	nfsstat4 *status = &res->sxr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_listxattrs(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	LISTXATTRS4args *args = NFS4_OP_ARG_SETUP(c, ph, oplistxattrs);
	LISTXATTRS4res *res = NFS4_OP_RES_SETUP(c, ph, oplistxattrs);
	nfsstat4 *status = &res->lxr_status;
	LISTXATTRS4resok *resok =
		NFS4_OP_RESOK_SETUP(res, LISTXATTRS4res_u, lxr_value);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_removexattr(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	REMOVEXATTR4args *args = NFS4_OP_ARG_SETUP(c, ph, opremovexattr);
	REMOVEXATTR4res *res = NFS4_OP_RES_SETUP(c, ph, opremovexattr);
	nfsstat4 *status = &res->rxr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}
