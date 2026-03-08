/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
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

void nfs4_op_getattr(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	GETATTR4args *args = NFS4_OP_ARG_SETUP(c, ph, opgetattr);
	GETATTR4res *res = NFS4_OP_RES_SETUP(c, ph, opgetattr);
	nfsstat4 *status = &res->status;
	GETATTR4resok *resok = NFS4_OP_RESOK_SETUP(res, GETATTR4res_u, resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_setattr(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	SETATTR4args *args = NFS4_OP_ARG_SETUP(c, ph, opsetattr);
	SETATTR4res *res = NFS4_OP_RES_SETUP(c, ph, opsetattr);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_verify(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	VERIFY4args *args = NFS4_OP_ARG_SETUP(c, ph, opverify);
	VERIFY4res *res = NFS4_OP_RES_SETUP(c, ph, opverify);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_nverify(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	NVERIFY4args *args = NFS4_OP_ARG_SETUP(c, ph, opnverify);
	NVERIFY4res *res = NFS4_OP_RES_SETUP(c, ph, opnverify);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_access(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	ACCESS4args *args = NFS4_OP_ARG_SETUP(c, ph, opaccess);
	ACCESS4res *res = NFS4_OP_RES_SETUP(c, ph, opaccess);
	nfsstat4 *status = &res->status;
	ACCESS4resok *resok = NFS4_OP_RESOK_SETUP(res, ACCESS4res_u, resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_access_mask(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	ACCESS_MASK4args *args = NFS4_OP_ARG_SETUP(c, ph, opaccess_mask);
	ACCESS_MASK4res *res = NFS4_OP_RES_SETUP(c, ph, opaccess_mask);
	nfsstat4 *status = &res->amr_status;
	ACCESS_MASK4resok *resok =
		NFS4_OP_RESOK_SETUP(res, ACCESS_MASK4res_u, amr_resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}
