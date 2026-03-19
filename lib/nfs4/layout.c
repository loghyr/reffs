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
#include "nfs4/compound.h"
#include "nfs4/ops.h"
#include "nfs4/errors.h"

void nfs4_op_layoutget(struct compound *c)
{
	LAYOUTGET4args *args = NFS4_OP_ARG_SETUP(c, oplayoutget);
	LAYOUTGET4res *res = NFS4_OP_RES_SETUP(c, oplayoutget);
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
	LAYOUTCOMMIT4args *args = NFS4_OP_ARG_SETUP(c, oplayoutcommit);
	LAYOUTCOMMIT4res *res = NFS4_OP_RES_SETUP(c, oplayoutcommit);
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
	LAYOUTRETURN4args *args = NFS4_OP_ARG_SETUP(c, oplayoutreturn);
	LAYOUTRETURN4res *res = NFS4_OP_RES_SETUP(c, oplayoutreturn);
	nfsstat4 *status = &res->lorr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_getdeviceinfo(struct compound *c)
{
	GETDEVICEINFO4args *args = NFS4_OP_ARG_SETUP(c, opgetdeviceinfo);
	GETDEVICEINFO4res *res = NFS4_OP_RES_SETUP(c, opgetdeviceinfo);
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
	GETDEVICELIST4args *args = NFS4_OP_ARG_SETUP(c, opgetdevicelist);
	GETDEVICELIST4res *res = NFS4_OP_RES_SETUP(c, opgetdevicelist);
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
	LAYOUTERROR4args *args = NFS4_OP_ARG_SETUP(c, oplayouterror);
	LAYOUTERROR4res *res = NFS4_OP_RES_SETUP(c, oplayouterror);
	nfsstat4 *status = &res->ler_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_layoutstats(struct compound *c)
{
	LAYOUTSTATS4args *args = NFS4_OP_ARG_SETUP(c, oplayoutstats);
	LAYOUTSTATS4res *res = NFS4_OP_RES_SETUP(c, oplayoutstats);
	nfsstat4 *status = &res->lsr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}
