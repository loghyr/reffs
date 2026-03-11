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

void nfs4_op_layoutget(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	LAYOUTGET4args *args = NFS4_OP_ARG_SETUP(c, ph, oplayoutget);
	LAYOUTGET4res *res = NFS4_OP_RES_SETUP(c, ph, oplayoutget);
	nfsstat4 *status = &res->logr_status;
	LAYOUTGET4resok *resok =
		NFS4_OP_RESOK_SETUP(res, LAYOUTGET4res_u, logr_resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_layoutcommit(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	LAYOUTCOMMIT4args *args = NFS4_OP_ARG_SETUP(c, ph, oplayoutcommit);
	LAYOUTCOMMIT4res *res = NFS4_OP_RES_SETUP(c, ph, oplayoutcommit);
	nfsstat4 *status = &res->locr_status;
	LAYOUTCOMMIT4resok *resok =
		NFS4_OP_RESOK_SETUP(res, LAYOUTCOMMIT4res_u, locr_resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_layoutreturn(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	LAYOUTRETURN4args *args = NFS4_OP_ARG_SETUP(c, ph, oplayoutreturn);
	LAYOUTRETURN4res *res = NFS4_OP_RES_SETUP(c, ph, oplayoutreturn);
	nfsstat4 *status = &res->lorr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_getdeviceinfo(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	GETDEVICEINFO4args *args = NFS4_OP_ARG_SETUP(c, ph, opgetdeviceinfo);
	GETDEVICEINFO4res *res = NFS4_OP_RES_SETUP(c, ph, opgetdeviceinfo);
	nfsstat4 *status = &res->gdir_status;
	GETDEVICEINFO4resok *resok =
		NFS4_OP_RESOK_SETUP(res, GETDEVICEINFO4res_u, gdir_resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_getdevicelist(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	GETDEVICELIST4args *args = NFS4_OP_ARG_SETUP(c, ph, opgetdevicelist);
	GETDEVICELIST4res *res = NFS4_OP_RES_SETUP(c, ph, opgetdevicelist);
	nfsstat4 *status = &res->gdlr_status;
	GETDEVICELIST4resok *resok =
		NFS4_OP_RESOK_SETUP(res, GETDEVICELIST4res_u, gdlr_resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_layouterror(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	LAYOUTERROR4args *args = NFS4_OP_ARG_SETUP(c, ph, oplayouterror);
	LAYOUTERROR4res *res = NFS4_OP_RES_SETUP(c, ph, oplayouterror);
	nfsstat4 *status = &res->ler_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_layoutstats(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	LAYOUTSTATS4args *args = NFS4_OP_ARG_SETUP(c, ph, oplayoutstats);
	LAYOUTSTATS4res *res = NFS4_OP_RES_SETUP(c, ph, oplayoutstats);
	nfsstat4 *status = &res->lsr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}
