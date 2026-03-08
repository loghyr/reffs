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

void nfs4_op_getfh(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	GETFH4res *res = NFS4_OP_RES_SETUP(c, ph, opgetfh);
	nfsstat4 *status = &res->status;
	GETFH4resok *resok = NFS4_OP_RESOK_SETUP(res, GETFH4res_u, resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)res, (void *)resok);
}

void nfs4_op_putfh(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	PUTFH4args *args = NFS4_OP_ARG_SETUP(c, ph, opputfh);
	PUTFH4res *res = NFS4_OP_RES_SETUP(c, ph, opputfh);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_putpubfh(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	PUTPUBFH4res *res = NFS4_OP_RES_SETUP(c, ph, opputpubfh);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)res);
}

void nfs4_op_putrootfh(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	PUTROOTFH4res *res = NFS4_OP_RES_SETUP(c, ph, opputrootfh);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)res);
}

void nfs4_op_restorefh(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	RESTOREFH4res *res = NFS4_OP_RES_SETUP(c, ph, oprestorefh);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)res);
}

void nfs4_op_savefh(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	SAVEFH4res *res = NFS4_OP_RES_SETUP(c, ph, opsavefh);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)res);
}
