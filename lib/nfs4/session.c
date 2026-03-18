/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <stdint.h>
#include <string.h>
#include <netinet/in.h>

#include "nfsv42_xdr.h"
#include "nfsv42_names.h"
#include "reffs/log.h"
#include "reffs/rpc.h"
#include "reffs/server.h"
#include "reffs/client.h"
#include "nfs4/compound.h"
#include "nfs4/ops.h"
#include "nfs4/errors.h"
#include "nfs4/client.h"
#include "nfs4/client_persist.h"

void nfs4_op_exchange_id(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	EXCHANGE_ID4args *args = NFS4_OP_ARG_SETUP(c, ph, opexchange_id);
	EXCHANGE_ID4res *res = NFS4_OP_RES_SETUP(c, ph, opexchange_id);
	nfsstat4 *status = &res->eir_status;
	EXCHANGE_ID4resok *resok =
		NFS4_OP_RESOK_SETUP(res, EXCHANGE_ID4res_u, eir_resok4);

	u_int num_ops = ((COMPOUND4args *)(ph)->ph_args)->argarray.argarray_len;
	struct server_state *ss = NULL;
	struct nfs4_client *nc = NULL;
	struct nfs_impl_id4 *impl_id = NULL;
	struct sockaddr_in sin;

	if (c->c_curr_op == 0 && num_ops > 1) {
		*status = NFS4ERR_NOT_ONLY_OP;
		goto out;
	}

	ss = server_state_find();
	if (!ss) {
		*status = NFS4ERR_SERVERFAULT;
		goto out;
	}

	if (args->eia_client_impl_id.eia_client_impl_id_len > 0)
		impl_id = args->eia_client_impl_id.eia_client_impl_id_val;

	rpc_trans_get_sockaddr_in(c->c_rt, &sin);

	nc = nfs4_client_alloc_or_find(ss, &args->eia_clientowner, impl_id,
				       &args->eia_clientowner.co_verifier,
				       &sin);
	if (!nc) {
		*status = NFS4ERR_SERVERFAULT;
		goto out;
	}

	resok->eir_clientid = (clientid4)nfs4_client_to_client(nc)->c_id;
	resok->eir_sequenceid = 1;
	resok->eir_flags = EXCHGID4_FLAG_USE_NON_PNFS;
	if (nc->nc_confirmed)
		resok->eir_flags |= EXCHGID4_FLAG_CONFIRMED_R;

	if (args->eia_state_protect.spa_how != SP4_NONE) {
		*status = NFS4ERR_NOTSUPP;
		goto out;
	}
	resok->eir_state_protect.spr_how = SP4_NONE;

	/*
	 * Both so_major_id_val and eir_server_scope_val must be calloc()'d:
	 * XDR free will call free() on each pointer independently, so they
	 * need separate heap allocations even though they hold the same string.
	 * The source string lives in ss for the server's lifetime.
	 */
	{
		char *major_id = calloc(1, ss->ss_owner_id_len + 1);
		char *scope = calloc(1, ss->ss_owner_id_len + 1);

		if (!major_id || !scope) {
			free(major_id);
			free(scope);
			*status = NFS4ERR_SERVERFAULT;
			goto out;
		}
		memcpy(major_id, ss->ss_owner_id, ss->ss_owner_id_len);
		memcpy(scope, ss->ss_owner_id, ss->ss_owner_id_len);

		resok->eir_server_owner.so_minor_id = 0;
		resok->eir_server_owner.so_major_id.so_major_id_val = major_id;
		resok->eir_server_owner.so_major_id.so_major_id_len =
			ss->ss_owner_id_len;

		resok->eir_server_scope.eir_server_scope_val = scope;
		resok->eir_server_scope.eir_server_scope_len =
			ss->ss_owner_id_len;
	}

	resok->eir_server_impl_id.eir_server_impl_id_val = NULL;
	resok->eir_server_impl_id.eir_server_impl_id_len = 0;

	client_put(nfs4_client_to_client(nc));
	nc = NULL;
	*status = NFS4_OK;

out:
	server_state_put(ss);
	LOG("%s status=%s(%d)", __func__, nfs4_err_name(*status), *status);
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

	u_int num_ops = ((COMPOUND4args *)(ph)->ph_args)->argarray.argarray_len;

	if (c->c_curr_op == 0 && num_ops > 1) {
		*status = NFS4ERR_NOT_ONLY_OP;
		goto out;
	}

	*status = NFS4ERR_NOTSUPP;

out:
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

	u_int num_ops = ((COMPOUND4args *)(ph)->ph_args)->argarray.argarray_len;

	if (c->c_curr_op == 0 && num_ops > 1) {
		*status = NFS4ERR_NOT_ONLY_OP;
		goto out;
	}

	*status = NFS4ERR_NOTSUPP;

out:
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

	if (c->c_curr_op != 0) {
		*status = NFS4ERR_SEQUENCE_POS;
		goto out;
	}

	*status = NFS4ERR_NOTSUPP;

out:
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
