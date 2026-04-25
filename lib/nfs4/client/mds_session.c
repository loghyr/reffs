/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * MDS session establishment for the EC demo client.
 *
 * Connects to the MDS via libtirpc, issues EXCHANGE_ID to get a
 * clientid, then CREATE_SESSION to get a sessionid and initial
 * slot state.  Teardown sends DESTROY_SESSION + DESTROY_CLIENTID.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <rpc/rpc.h>

/*
 * GSS-RPC support requires BOTH:
 *   - libgssapi_krb5 (HAVE_GSSAPI_KRB5) for the GSSAPI itself, and
 *   - <rpc/auth_gss.h> (HAVE_RPC_AUTH_GSS_H) for the SunRPC/GSS
 *     integration layer (shipped with libtirpc on Linux; absent on
 *     FreeBSD base, whose gssrpc equivalent uses a different API).
 * Both must be present, or the gated mds_session_create_sec block
 * below returns -ENOSYS.
 */
#if defined(HAVE_GSSAPI_KRB5) && defined(HAVE_RPC_AUTH_GSS_H)
#define REFFS_HAVE_GSS_RPC 1
#endif

#ifdef REFFS_HAVE_GSS_RPC
#include <rpc/auth_gss.h>
#include <gssapi/gssapi.h>
#include <gssapi/gssapi_krb5.h>

/* krb5 OID: 1.2.840.113554.1.2.2 */
static gss_OID_desc krb5oid_desc = {
	9, (void *)"\x2a\x86\x48\x86\xf7\x12\x01\x02\x02"
};
#endif

#include "nfsv42_xdr.h"
#include "ec_client.h"

/* ------------------------------------------------------------------ */
/* EXCHANGE_ID                                                         */
/* ------------------------------------------------------------------ */

static int mds_exchange_id(struct mds_session *ms)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	EXCHANGE_ID4args *args;
	int ret;

	ret = mds_compound_init(&mc, 1, "exchange_id");
	if (ret)
		return ret;

	slot = mds_compound_add_op(&mc, OP_EXCHANGE_ID);
	if (!slot) {
		mds_compound_fini(&mc);
		return -ENOSPC;
	}

	args = &slot->nfs_argop4_u.opexchange_id;

	args->eia_clientowner.co_ownerid.co_ownerid_val = ms->ms_owner;
	args->eia_clientowner.co_ownerid.co_ownerid_len = strlen(ms->ms_owner);
	memset(&args->eia_clientowner.co_verifier, 0,
	       sizeof(args->eia_clientowner.co_verifier));

	/* Random verifier -- new incarnation each time. */
	uint32_t pid = (uint32_t)getpid();

	memcpy(&args->eia_clientowner.co_verifier, &pid, sizeof(pid));

	args->eia_flags = EXCHGID4_FLAG_USE_PNFS_MDS;
	args->eia_state_protect.spa_how = SP4_NONE;
	args->eia_client_impl_id.eia_client_impl_id_len = 0;
	args->eia_client_impl_id.eia_client_impl_id_val = NULL;

	/*
	 * EXCHANGE_ID is a solo-op compound (no SEQUENCE), so we send
	 * it manually rather than through the session path.
	 */
	struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
	enum clnt_stat rpc_stat;

	memset(&mc.mc_res, 0, sizeof(mc.mc_res));
	rpc_stat = clnt_call(ms->ms_clnt, NFSPROC4_COMPOUND,
			     (xdrproc_t)xdr_COMPOUND4args, (caddr_t)&mc.mc_args,
			     (xdrproc_t)xdr_COMPOUND4res, (caddr_t)&mc.mc_res,
			     tv);
	if (rpc_stat != RPC_SUCCESS) {
		mds_compound_fini(&mc);
		return -EIO;
	}

	if (mc.mc_res.status != NFS4_OK ||
	    mc.mc_res.resarray.resarray_len < 1) {
		mds_compound_fini(&mc);
		return -EREMOTEIO;
	}

	nfs_resop4 *res_slot = &mc.mc_res.resarray.resarray_val[0];
	EXCHANGE_ID4res *eid_res = &res_slot->nfs_resop4_u.opexchange_id;

	if (eid_res->eir_status != NFS4_OK) {
		mds_compound_fini(&mc);
		return -EREMOTEIO;
	}

	EXCHANGE_ID4resok *resok = &eid_res->EXCHANGE_ID4res_u.eir_resok4;

	ms->ms_clientid = resok->eir_clientid;
	ms->ms_create_seq = resok->eir_sequenceid;

	mds_compound_fini(&mc);
	return 0;
}

/* ------------------------------------------------------------------ */
/* CREATE_SESSION                                                      */
/* ------------------------------------------------------------------ */

static int mds_create_session(struct mds_session *ms)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	CREATE_SESSION4args *args;
	int ret;

	ret = mds_compound_init(&mc, 1, "create_session");
	if (ret)
		return ret;

	slot = mds_compound_add_op(&mc, OP_CREATE_SESSION);
	if (!slot) {
		mds_compound_fini(&mc);
		return -ENOSPC;
	}

	args = &slot->nfs_argop4_u.opcreate_session;
	args->csa_clientid = ms->ms_clientid;
	args->csa_sequence = ms->ms_create_seq;
	args->csa_flags = 0;

	/* Fore channel: modest values for a demo client. */
	args->csa_fore_chan_attrs.ca_headerpadsize = 0;
	args->csa_fore_chan_attrs.ca_maxrequestsize = 1048576;
	args->csa_fore_chan_attrs.ca_maxresponsesize = 1048576;
	args->csa_fore_chan_attrs.ca_maxresponsesize_cached = 4096;
	args->csa_fore_chan_attrs.ca_maxoperations = 16;
	args->csa_fore_chan_attrs.ca_maxrequests = 1;
	args->csa_fore_chan_attrs.ca_rdma_ird.ca_rdma_ird_len = 0;
	args->csa_fore_chan_attrs.ca_rdma_ird.ca_rdma_ird_val = NULL;

	/* No back channel needed. */
	args->csa_back_chan_attrs.ca_headerpadsize = 0;
	args->csa_back_chan_attrs.ca_maxrequestsize = 4096;
	args->csa_back_chan_attrs.ca_maxresponsesize = 4096;
	args->csa_back_chan_attrs.ca_maxresponsesize_cached = 0;
	args->csa_back_chan_attrs.ca_maxoperations = 2;
	args->csa_back_chan_attrs.ca_maxrequests = 1;
	args->csa_back_chan_attrs.ca_rdma_ird.ca_rdma_ird_len = 0;
	args->csa_back_chan_attrs.ca_rdma_ird.ca_rdma_ird_val = NULL;

	args->csa_cb_program = 0;
	args->csa_sec_parms.csa_sec_parms_len = 0;
	args->csa_sec_parms.csa_sec_parms_val = NULL;

	/* Solo-op compound, no SEQUENCE. */
	struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
	enum clnt_stat rpc_stat;

	memset(&mc.mc_res, 0, sizeof(mc.mc_res));
	rpc_stat = clnt_call(ms->ms_clnt, NFSPROC4_COMPOUND,
			     (xdrproc_t)xdr_COMPOUND4args, (caddr_t)&mc.mc_args,
			     (xdrproc_t)xdr_COMPOUND4res, (caddr_t)&mc.mc_res,
			     tv);
	if (rpc_stat != RPC_SUCCESS) {
		mds_compound_fini(&mc);
		return -EIO;
	}

	if (mc.mc_res.status != NFS4_OK ||
	    mc.mc_res.resarray.resarray_len < 1) {
		mds_compound_fini(&mc);
		return -EREMOTEIO;
	}

	nfs_resop4 *res_slot = &mc.mc_res.resarray.resarray_val[0];
	CREATE_SESSION4res *cs_res = &res_slot->nfs_resop4_u.opcreate_session;

	if (cs_res->csr_status != NFS4_OK) {
		mds_compound_fini(&mc);
		return -EREMOTEIO;
	}

	CREATE_SESSION4resok *resok = &cs_res->CREATE_SESSION4res_u.csr_resok4;

	memcpy(ms->ms_sessionid, resok->csr_sessionid, sizeof(sessionid4));
	ms->ms_slot_seqid = 1; /* first request on slot 0 */
	ms->ms_maxrequestsize = resok->csr_fore_chan_attrs.ca_maxrequestsize;

	mds_compound_fini(&mc);
	return 0;
}

/* ------------------------------------------------------------------ */
/* RECLAIM_COMPLETE                                                    */
/* ------------------------------------------------------------------ */

static void mds_reclaim_complete(struct mds_session *ms)
{
	struct mds_compound mc;
	nfs_argop4 *slot;

	if (mds_compound_init(&mc, 2, "reclaim_complete"))
		return;

	if (mds_compound_add_sequence(&mc, ms))
		goto out;

	slot = mds_compound_add_op(&mc, OP_RECLAIM_COMPLETE);
	if (!slot)
		goto out;

	RECLAIM_COMPLETE4args *args = &slot->nfs_argop4_u.opreclaim_complete;
	args->rca_one_fs = FALSE;

	mds_compound_send(&mc, ms);
out:
	mds_compound_fini(&mc);
}

/* ------------------------------------------------------------------ */
/* DESTROY_SESSION                                                     */
/* ------------------------------------------------------------------ */

static void mds_destroy_session(struct mds_session *ms)
{
	struct mds_compound mc;
	nfs_argop4 *slot;

	if (mds_compound_init(&mc, 2, "destroy_session"))
		return;

	if (mds_compound_add_sequence(&mc, ms))
		goto out;

	slot = mds_compound_add_op(&mc, OP_DESTROY_SESSION);
	if (!slot)
		goto out;

	DESTROY_SESSION4args *args = &slot->nfs_argop4_u.opdestroy_session;
	memcpy(args->dsa_sessionid, ms->ms_sessionid, sizeof(sessionid4));

	mds_compound_send(&mc, ms);
out:
	mds_compound_fini(&mc);
}

/* ------------------------------------------------------------------ */
/* DESTROY_CLIENTID                                                    */
/* ------------------------------------------------------------------ */

static void mds_destroy_clientid(struct mds_session *ms)
{
	struct mds_compound mc;
	nfs_argop4 *slot;

	if (mds_compound_init(&mc, 1, "destroy_clientid"))
		return;

	slot = mds_compound_add_op(&mc, OP_DESTROY_CLIENTID);
	if (!slot)
		goto out;

	DESTROY_CLIENTID4args *args = &slot->nfs_argop4_u.opdestroy_clientid;
	args->dca_clientid = ms->ms_clientid;

	/* Solo-op, no SEQUENCE. */
	struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };

	memset(&mc.mc_res, 0, sizeof(mc.mc_res));
	clnt_call(ms->ms_clnt, NFSPROC4_COMPOUND, (xdrproc_t)xdr_COMPOUND4args,
		  (caddr_t)&mc.mc_args, (xdrproc_t)xdr_COMPOUND4res,
		  (caddr_t)&mc.mc_res, tv);
out:
	mds_compound_fini(&mc);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void mds_session_set_owner(struct mds_session *ms, const char *id)
{
	char hostname[128];

	if (gethostname(hostname, sizeof(hostname)) < 0)
		snprintf(hostname, sizeof(hostname), "ec_demo");

	if (id && id[0] != '\0')
		snprintf(ms->ms_owner, sizeof(ms->ms_owner), "%s:%s", hostname,
			 id);
	else
		snprintf(ms->ms_owner, sizeof(ms->ms_owner), "%s:%u", hostname,
			 (unsigned)getpid());
}

int mds_session_create(struct mds_session *ms, const char *host)
{
	int ret;

	/*
	 * Preserve ms_owner if the caller set it before create.
	 * Otherwise generate a default from hostname:PID.
	 */
	char saved_owner[256];

	memcpy(saved_owner, ms->ms_owner, sizeof(saved_owner));
	memset(ms, 0, sizeof(*ms));
	memcpy(ms->ms_owner, saved_owner, sizeof(ms->ms_owner));

	if (ms->ms_owner[0] == '\0')
		mds_session_set_owner(ms, NULL);

	if (pthread_mutex_init(&ms->ms_call_mutex, NULL) != 0)
		return -ENOMEM;

	ms->ms_clnt = clnt_create(host, NFS4_PROGRAM, NFS_V4, "tcp");
	if (!ms->ms_clnt) {
		pthread_mutex_destroy(&ms->ms_call_mutex);
		return -ECONNREFUSED;
	}

	/* AUTH_SYS is required by most exports; clnt_create defaults
	 * to AUTH_NONE which the server rejects with NFS4ERR_WRONGSEC.
	 * Remember the default auth on ms_auth_default so
	 * mds_compound_send_with_auth can restore it after a per-
	 * compound override.
	 */
	{
		AUTH *auth = authunix_create_default();

		if (auth) {
			auth_destroy(ms->ms_clnt->cl_auth);
			ms->ms_clnt->cl_auth = auth;
			ms->ms_auth_default = auth;
		}
	}

	ret = mds_exchange_id(ms);
	if (ret)
		goto err;

	ret = mds_create_session(ms);
	if (ret)
		goto err;

	/* Best-effort: tell the server we have no state to reclaim. */
	mds_reclaim_complete(ms);

	return 0;

err:
	if (ms->ms_clnt)
		clnt_destroy(ms->ms_clnt);
	pthread_mutex_destroy(&ms->ms_call_mutex);
	memset(ms, 0, sizeof(*ms));
	return ret;
}

int mds_session_create_sec(struct mds_session *ms, const char *host,
			   enum ec_sec_flavor sec)
{
#ifdef REFFS_HAVE_GSS_RPC
	if (sec == EC_SEC_SYS)
		return mds_session_create(ms, host);

	int ret;
	char saved_owner[256];

	memcpy(saved_owner, ms->ms_owner, sizeof(saved_owner));
	memset(ms, 0, sizeof(*ms));
	memcpy(ms->ms_owner, saved_owner, sizeof(ms->ms_owner));

	if (ms->ms_owner[0] == '\0')
		mds_session_set_owner(ms, NULL);

	if (pthread_mutex_init(&ms->ms_call_mutex, NULL) != 0)
		return -ENOMEM;

	ms->ms_clnt = clnt_create(host, NFS4_PROGRAM, NFS_V4, "tcp");
	if (!ms->ms_clnt) {
		pthread_mutex_destroy(&ms->ms_call_mutex);
		return -ECONNREFUSED;
	}

	/*
	 * RPCSEC_GSS via libtirpc's authgss_create_default.
	 * service_name is "nfs@host" -- the server's service principal.
	 */
	char service[512];

	snprintf(service, sizeof(service), "nfs@%s", host);

	rpc_gss_svc_t gss_svc;

	switch (sec) {
	case EC_SEC_KRB5:
		gss_svc = RPCSEC_GSS_SVC_NONE;
		break;
	case EC_SEC_KRB5I:
		gss_svc = RPCSEC_GSS_SVC_INTEGRITY;
		break;
	case EC_SEC_KRB5P:
		gss_svc = RPCSEC_GSS_SVC_PRIVACY;
		break;
	default:
		clnt_destroy(ms->ms_clnt);
		return -EINVAL;
	}

	struct rpc_gss_sec gss_sec = {
		.mech = &krb5oid_desc,
		.qop = GSS_C_QOP_DEFAULT,
		.svc = gss_svc,
		.cred = GSS_C_NO_CREDENTIAL,
		.req_flags = GSS_C_MUTUAL_FLAG,
	};

	AUTH *auth = authgss_create_default(ms->ms_clnt, service, &gss_sec);

	if (!auth) {
		fprintf(stderr, "ec_demo: authgss_create_default failed\n");
		clnt_destroy(ms->ms_clnt);
		pthread_mutex_destroy(&ms->ms_call_mutex);
		memset(ms, 0, sizeof(*ms));
		return -EACCES;
	}

	auth_destroy(ms->ms_clnt->cl_auth);
	ms->ms_clnt->cl_auth = auth;
	/*
	 * Record the default auth so send_with_auth's restore path
	 * works the same way as the AUTH_SYS session.  GSS sessions
	 * never take the AUTH_SYS-override path today (PS's proxy
	 * forwarders are AUTH_SYS only per slice 2e-iv-c scope), but
	 * keeping the bookkeeping uniform avoids a branch in the send
	 * helper.
	 */
	ms->ms_auth_default = auth;

	ret = mds_exchange_id(ms);
	if (ret)
		goto err;

	ret = mds_create_session(ms);
	if (ret)
		goto err;

	mds_reclaim_complete(ms);
	return 0;

err:
	if (ms->ms_clnt)
		clnt_destroy(ms->ms_clnt);
	pthread_mutex_destroy(&ms->ms_call_mutex);
	memset(ms, 0, sizeof(*ms));
	return ret;
#else
	(void)sec;
	return mds_session_create(ms, host);
#endif
}

void mds_session_destroy(struct mds_session *ms)
{
	if (!ms->ms_clnt)
		return;

	mds_destroy_session(ms);
	mds_destroy_clientid(ms);
	clnt_destroy(ms->ms_clnt);
	/*
	 * clnt_destroy calls auth_destroy on cl_auth; the default auth
	 * we stored in ms_auth_default was the same pointer, so don't
	 * double-destroy.  Just forget it.
	 */
	ms->ms_auth_default = NULL;
	pthread_mutex_destroy(&ms->ms_call_mutex);
	memset(ms, 0, sizeof(*ms));
}
