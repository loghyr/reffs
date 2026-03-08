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
#include "nfs4_internal.h"
#include "ops.h"
#include "errors.h"

void nfs4_op_exchange_id(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	EXCHANGE_ID4args *args = NFS4_OP_ARG_SETUP(c, ph, opexchange_id);
	EXCHANGE_ID4res *res = NFS4_OP_RES_SETUP(c, ph, opexchange_id);
	nfsstat4 *status = &res->eir_status;
	EXCHANGE_ID4resok *resok =
		NFS4_OP_RESOK_SETUP(res, EXCHANGE_ID4res_u, eir_resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_create_session(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	CREATE_SESSION4args *args = NFS4_OP_ARG_SETUP(c, ph, opcreate_session);
	CREATE_SESSION4res *res = NFS4_OP_RES_SETUP(c, ph, opcreate_session);
	nfsstat4 *status = &res->csr_status;
	CREATE_SESSION4resok *resok =
		NFS4_OP_RESOK_SETUP(res, CREATE_SESSION4res_u, csr_resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_destroy_session(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	DESTROY_SESSION4res *res = NFS4_OP_RES_SETUP(c, ph, opdestroy_session);
	nfsstat4 *status = &res->dsr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)res);
}

void nfs4_op_sequence(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	SEQUENCE4args *args = NFS4_OP_ARG_SETUP(c, ph, opsequence);
	SEQUENCE4res *res = NFS4_OP_RES_SETUP(c, ph, opsequence);
	nfsstat4 *status = &res->sr_status;
	SEQUENCE4resok *resok =
		NFS4_OP_RESOK_SETUP(res, SEQUENCE4res_u, sr_resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_renew(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	RENEW4res *res = NFS4_OP_RES_SETUP(c, ph, oprenew);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)res);
}

void nfs4_op_setclientid(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	SETCLIENTID4args *args = NFS4_OP_ARG_SETUP(c, ph, opsetclientid);
	SETCLIENTID4res *res = NFS4_OP_RES_SETUP(c, ph, opsetclientid);
	nfsstat4 *status = &res->status;
	SETCLIENTID4resok *resok =
		NFS4_OP_RESOK_SETUP(res, SETCLIENTID4res_u, resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_setclientid_confirm(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	SETCLIENTID_CONFIRM4res *res =
		NFS4_OP_RES_SETUP(c, ph, opsetclientid_confirm);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)res);
}

void nfs4_op_destroy_clientid(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	DESTROY_CLIENTID4res *res =
		NFS4_OP_RES_SETUP(c, ph, opdestroy_clientid);
	nfsstat4 *status = &res->dcr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)res);
}

void nfs4_op_reclaim_complete(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	RECLAIM_COMPLETE4args *args =
		NFS4_OP_ARG_SETUP(c, ph, opreclaim_complete);
	RECLAIM_COMPLETE4res *res =
		NFS4_OP_RES_SETUP(c, ph, opreclaim_complete);
	nfsstat4 *status = &res->rcr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_bind_conn_to_session(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	BIND_CONN_TO_SESSION4args *args =
		NFS4_OP_ARG_SETUP(c, ph, opbind_conn_to_session);
	BIND_CONN_TO_SESSION4res *res =
		NFS4_OP_RES_SETUP(c, ph, opbind_conn_to_session);
	nfsstat4 *status = &res->bctsr_status;
	BIND_CONN_TO_SESSION4resok *resok = NFS4_OP_RESOK_SETUP(
		res, BIND_CONN_TO_SESSION4res_u, bctsr_resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_backchannel_ctl(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	BACKCHANNEL_CTL4args *args =
		NFS4_OP_ARG_SETUP(c, ph, opbackchannel_ctl);
	BACKCHANNEL_CTL4res *res = NFS4_OP_RES_SETUP(c, ph, opbackchannel_ctl);
	nfsstat4 *status = &res->bcr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_set_ssv(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	SET_SSV4args *args = NFS4_OP_ARG_SETUP(c, ph, opset_ssv);
	SET_SSV4res *res = NFS4_OP_RES_SETUP(c, ph, opset_ssv);
	nfsstat4 *status = &res->ssr_status;
	SET_SSV4resok *resok =
		NFS4_OP_RESOK_SETUP(res, SET_SSV4res_u, ssr_resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}
