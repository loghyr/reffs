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
#include "compound.h"
#include "ops.h"
#include "errors.h"

void nfs4_op_copy(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	COPY4args *args = NFS4_OP_ARG_SETUP(c, ph, opcopy);
	COPY4res *res = NFS4_OP_RES_SETUP(c, ph, opcopy);
	nfsstat4 *status = &res->cr_status;
	COPY4resok *resok = NFS4_OP_RESOK_SETUP(res, COPY4res_u, cr_resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_copy_notify(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	COPY_NOTIFY4args *args = NFS4_OP_ARG_SETUP(c, ph, opoffload_notify);
	COPY_NOTIFY4res *res = NFS4_OP_RES_SETUP(c, ph, opcopy_notify);
	nfsstat4 *status = &res->cnr_status;
	COPY_NOTIFY4resok *resok =
		NFS4_OP_RESOK_SETUP(res, COPY_NOTIFY4res_u, resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_clone(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	CLONE4args *args = NFS4_OP_ARG_SETUP(c, ph, opclone);
	CLONE4res *res = NFS4_OP_RES_SETUP(c, ph, opclone);
	nfsstat4 *status = &res->cl_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}
void nfs4_op_offload_cancel(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	OFFLOAD_CANCEL4res *res = NFS4_OP_RES_SETUP(c, ph, opoffload_cancel);
	nfsstat4 *status = &res->ocr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)res);
}

void nfs4_op_offload_status(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	OFFLOAD_STATUS4res *res = NFS4_OP_RES_SETUP(c, ph, opoffload_status);
	nfsstat4 *status = &res->osr_status;
	OFFLOAD_STATUS4resok *resok =
		NFS4_OP_RESOK_SETUP(res, OFFLOAD_STATUS4res_u, osr_resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)res, (void *)resok);
}
