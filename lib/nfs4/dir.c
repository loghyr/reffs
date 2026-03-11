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

void nfs4_op_lookup(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	LOOKUP4args *args = NFS4_OP_ARG_SETUP(c, ph, oplookup);
	LOOKUP4res *res = NFS4_OP_RES_SETUP(c, ph, oplookup);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_lookupp(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	LOOKUPP4res *res = NFS4_OP_RES_SETUP(c, ph, oplookupp);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)res);
}

void nfs4_op_readdir(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	READDIR4args *args = NFS4_OP_ARG_SETUP(c, ph, opreaddir);
	READDIR4res *res = NFS4_OP_RES_SETUP(c, ph, opreaddir);
	nfsstat4 *status = &res->status;
	READDIR4resok *resok = NFS4_OP_RESOK_SETUP(res, READDIR4res_u, resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_create(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	CREATE4args *args = NFS4_OP_ARG_SETUP(c, ph, opcreate);
	CREATE4res *res = NFS4_OP_RES_SETUP(c, ph, opcreate);
	nfsstat4 *status = &res->status;
	CREATE4resok *resok = NFS4_OP_RESOK_SETUP(res, CREATE4res_u, resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_remove(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	REMOVE4args *args = NFS4_OP_ARG_SETUP(c, ph, opremove);
	REMOVE4res *res = NFS4_OP_RES_SETUP(c, ph, opremove);
	nfsstat4 *status = &res->status;
	REMOVE4resok *resok = NFS4_OP_RESOK_SETUP(res, REMOVE4res_u, resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_rename(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	RENAME4args *args = NFS4_OP_ARG_SETUP(c, ph, oprename);
	RENAME4res *res = NFS4_OP_RES_SETUP(c, ph, oprename);
	nfsstat4 *status = &res->status;
	RENAME4resok *resok = NFS4_OP_RESOK_SETUP(res, RENAME4res_u, resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_link(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	LINK4args *args = NFS4_OP_ARG_SETUP(c, ph, oplink);
	LINK4res *res = NFS4_OP_RES_SETUP(c, ph, oplink);
	nfsstat4 *status = &res->status;
	LINK4resok *resok = NFS4_OP_RESOK_SETUP(res, LINK4res_u, resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_openattr(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	OPENATTR4args *args = NFS4_OP_ARG_SETUP(c, ph, opopenattr);
	OPENATTR4res *res = NFS4_OP_RES_SETUP(c, ph, opopenattr);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_readlink(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	READLINK4res *res = NFS4_OP_RES_SETUP(c, ph, opreadlink);
	nfsstat4 *status = &res->status;
	READLINK4resok *resok =
		NFS4_OP_RESOK_SETUP(res, READLINK4res_u, resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)res, (void *)resok);
}
