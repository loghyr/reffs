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
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <rpc/rpc.h>

#include "host_port.h"

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

#include <openssl/ssl.h>

#include "nfsv42_xdr.h"
#include "reffs/filehandle.h"
#include "reffs/settings.h"
#include "reffs/tls_client.h"
#include "ec_client.h"
#include "mds_tls_xprt.h"

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

	/*
	 * EXCHGID4 flag is caller-controllable via ms_exchgid_flags
	 * so the same client lib serves both the PS-MDS path
	 * (USE_NON_PNFS -- a regular NFSv4 client; required by
	 * PROXY_REGISTRATION at lib/nfs4/server/proxy_registration.c)
	 * and the future MDS-to-DS dstore path (USE_PNFS_MDS --
	 * required by trust_stateid_ops.c which gates TRUST_STATEID
	 * acceptance on the flag).  Default zero preserves the
	 * pre-#140 USE_NON_PNFS behaviour for every existing caller
	 * of mds_session_create.  Tracked as task #140 reviewer
	 * follow-up #1.
	 */
	args->eia_flags = ms->ms_exchgid_flags ? ms->ms_exchgid_flags :
						 EXCHGID4_FLAG_USE_NON_PNFS;
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

	/*
	 * Fore channel sizes.  Must HEADROOM the largest expected
	 * data payload + RPC/NFS overhead -- a 1 MiB WRITE compound
	 * runs SEQUENCE + PUTFH + WRITE(1MiB) which overshoots a
	 * literal 1 MiB cap and the MDS rejects with
	 * NFS4ERR_REQ_TOO_BIG.  Ask for 4 MiB; the server clamps to
	 * its own NFS4_SESSION_MAX_REQUEST_SIZE (1 MiB + 64 KiB =
	 * ~1.06 MiB) which is enough for a 1 MiB data payload plus
	 * the sub-kilobyte compound prefix.
	 */
	args->csa_fore_chan_attrs.ca_headerpadsize = 0;
	args->csa_fore_chan_attrs.ca_maxrequestsize = 4U * 1024U * 1024U;
	args->csa_fore_chan_attrs.ca_maxresponsesize = 4U * 1024U * 1024U;
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
/* PROXY_REGISTRATION (slice plan-A.iii)                               */
/* ------------------------------------------------------------------ */

/*
 * Map a PROXY_REGISTRATION nfsstat4 -- whether at the COMPOUND level
 * (mc_res.status) or the per-op level (prrr_status) -- to an errno.
 *
 * Both levels surface the same error class because PROXY_REGISTRATION
 * is the last (and only failure-bearing) op in our compound: per RFC
 * 8881 S15.2, the COMPOUND status equals the status of the op that
 * failed, so a NFS4ERR_PERM at the op level shows up as a
 * NFS4ERR_PERM mc_res.status as well.  The same mapping is correct
 * for both.
 *
 * Return -EIO for unmapped statuses; caller diagnostics name the
 * specific value.
 */
int proxy_reg_nfsstat_to_errno(nfsstat4 status)
{
	switch (status) {
	case NFS4_OK:
		return 0;
	case NFS4ERR_PERM:
		return -EPERM;
	case NFS4ERR_DELAY:
		return -EAGAIN;
	case NFS4ERR_INVAL:
		return -EINVAL;
	case NFS4ERR_NOTSUPP:
	case NFS4ERR_OP_ILLEGAL:
		return -ENOSYS;
	case NFS4ERR_BADXDR:
		return -EBADMSG;
	case NFS4ERR_OP_NOT_IN_SESSION:
		return -EPROTO;
	default:
		return -EIO;
	}
}

int mds_session_send_proxy_registration(struct mds_session *ms,
					const uint8_t *registration_id,
					uint32_t registration_id_len)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	int ret = -EIO;
	int sret;

	if (!ms || (registration_id_len > 0 && !registration_id))
		return -EINVAL;
	if (registration_id_len > PROXY_REGISTRATION_ID_MAX)
		return -EINVAL;

	if (mds_compound_init(&mc, 2, "proxy_registration"))
		return -ENOMEM;

	if (mds_compound_add_sequence(&mc, ms))
		goto out;

	slot = mds_compound_add_op(&mc, OP_PROXY_REGISTRATION);
	if (!slot)
		goto out;

	PROXY_REGISTRATION4args *args =
		&slot->nfs_argop4_u.opproxy_registration;
	args->prr_flags = 0;
	args->prr_registration_id.prr_registration_id_len = registration_id_len;
	args->prr_registration_id.prr_registration_id_val =
		(char *)registration_id; /* libtirpc XDR borrows */

	/*
	 * mds_compound_send returns -EREMOTEIO when the COMPOUND replied
	 * with a non-OK status.  That is information we care about, not
	 * a transport failure -- treat it as success-of-the-call so the
	 * COMPOUND-level mapping below can run.  Any OTHER negative
	 * return (RPC stat != RPC_SUCCESS, encoder failure) IS a real
	 * transport failure and should propagate as -EIO.
	 */
	sret = mds_compound_send(&mc, ms);
	if (sret && sret != -EREMOTEIO)
		goto out;

	/*
	 * COMPOUND-level status carries the failure when an op short-
	 * circuited.  Map it through the same nfsstat4 helper so the PS
	 * caller sees -EPERM / -ENOSYS / -EAGAIN / etc. rather than the
	 * historical default of -EIO.  fflush stderr because our process
	 * runs long-lived; without an explicit flush, line-buffered
	 * stderr does not appear in a redirected log file.
	 */
	if (mc.mc_res.status != NFS4_OK) {
		ret = proxy_reg_nfsstat_to_errno(mc.mc_res.status);
		fprintf(stderr,
			"proxy_registration: COMPOUND status %u (errno %d)\n",
			mc.mc_res.status, -ret);
		fflush(stderr);
		goto out;
	}
	if (mc.mc_res.resarray.resarray_len < 2) {
		fprintf(stderr, "proxy_registration: short resarray (%u)\n",
			mc.mc_res.resarray.resarray_len);
		fflush(stderr);
		goto out;
	}
	nfsstat4 op_status =
		mc.mc_res.resarray.resarray_val[1]
			.nfs_resop4_u.opproxy_registration.prrr_status;
	ret = proxy_reg_nfsstat_to_errno(op_status);
	if (ret == -EIO && op_status != NFS4_OK) {
		fprintf(stderr,
			"proxy_registration: MDS returned nfsstat4 %u\n",
			op_status);
		fflush(stderr);
	}

out:
	mds_compound_fini(&mc);
	return ret;
}

/* ------------------------------------------------------------------ */
/* PROXY_PROGRESS / PROXY_DONE / PROXY_CANCEL (slice 6c-z)            */
/* ------------------------------------------------------------------ */

/*
 * Map a proxy_done / cancel nfsstat4 into the PS-side errno.
 * Mirrors the priority-ordered authorization rule on the MDS:
 *   PERM           -> -EPERM   (not registered, owner mismatch)
 *   STALE_STATEID  -> -ESTALE  (stateid from prior boot)
 *   BAD_STATEID    -> -EBADF   (no record / file mismatch)
 *   OLD_STATEID    -> -EAGAIN  (seqid out of order)
 *   INVAL          -> -EINVAL  (reserved-flag bit set, etc.)
 *   NOTSUPP        -> -ENOSYS  (server doesn't speak this op)
 *   OK             ->  0
 *   default        -> -EIO
 */
static int proxy_op_nfsstat_to_errno(nfsstat4 status)
{
	switch (status) {
	case NFS4_OK:
		return 0;
	case NFS4ERR_PERM:
		return -EPERM;
	case NFS4ERR_STALE_STATEID:
		return -ESTALE;
	case NFS4ERR_BAD_STATEID:
		return -EBADF;
	case NFS4ERR_OLD_STATEID:
		return -EAGAIN;
	case NFS4ERR_INVAL:
		return -EINVAL;
	case NFS4ERR_NOTSUPP:
		return -ENOSYS;
	default:
		return -EIO;
	}
}

int mds_session_send_proxy_progress(
	struct mds_session *ms, struct ps_progress_assignment *out_assignments,
	uint32_t max_assignments, uint32_t *lease_remaining_sec)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	int ret = -EIO;
	int sret;

	if (!ms)
		return -EINVAL;

	if (mds_compound_init(&mc, 2, "proxy_progress"))
		return -ENOMEM;
	if (mds_compound_add_sequence(&mc, ms))
		goto out;

	slot = mds_compound_add_op(&mc, OP_PROXY_PROGRESS);
	if (!slot)
		goto out;
	slot->nfs_argop4_u.opproxy_progress.ppa_flags = 0;

	sret = mds_compound_send(&mc, ms);
	if (sret && sret != -EREMOTEIO) {
		ret = sret;
		goto out;
	}

	if (mc.mc_res.status != NFS4_OK) {
		ret = proxy_op_nfsstat_to_errno(mc.mc_res.status);
		goto out;
	}
	if (mc.mc_res.resarray.resarray_len < 2) {
		ret = -EREMOTEIO;
		goto out;
	}

	PROXY_PROGRESS4res *res = &mc.mc_res.resarray.resarray_val[1]
					   .nfs_resop4_u.opproxy_progress;

	if (res->ppr_status != NFS4_OK) {
		ret = proxy_op_nfsstat_to_errno(res->ppr_status);
		goto out;
	}

	PROXY_PROGRESS4resok *resok = &res->PROXY_PROGRESS4res_u.ppr_resok;

	if (lease_remaining_sec)
		*lease_remaining_sec = resok->ppr_lease_remaining_sec;

	uint32_t n = resok->ppr_assignments.ppr_assignments_len;

	if (n > max_assignments)
		n = max_assignments;
	for (uint32_t i = 0; i < n; i++) {
		const proxy_assignment4 *a =
			&resok->ppr_assignments.ppr_assignments_val[i];
		out_assignments[i].pa_kind = (uint32_t)a->pa_kind;
		out_assignments[i].pa_stateid = a->pa_stateid;
		out_assignments[i].pa_source_dstore_id = a->pa_source_dstore_id;
		out_assignments[i].pa_target_dstore_id = a->pa_target_dstore_id;
		/*
		 * pa_file_fh is the network_file_handle bytes the MDS
		 * marshalled at slice-6c-y reply-build time.  Decode
		 * back to (sb_id, ino) so the PS-side OPEN+LAYOUTGET
		 * has the file identity in hand.
		 */
		out_assignments[i].pa_sb_id = 0;
		out_assignments[i].pa_ino = 0;
		if (a->pa_file_fh.nfs_fh4_len ==
		    sizeof(struct network_file_handle)) {
			struct network_file_handle nfh;

			memcpy(&nfh, a->pa_file_fh.nfs_fh4_val, sizeof(nfh));
			out_assignments[i].pa_sb_id = nfh.nfh_sb;
			out_assignments[i].pa_ino = nfh.nfh_ino;
		}
	}
	ret = (int)n;

out:
	mds_compound_fini(&mc);
	return ret;
}

int mds_session_send_proxy_done(struct mds_session *ms,
				const stateid4 *pd_stateid, nfsstat4 pd_status)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	int ret = -EIO;
	int sret;

	if (!ms || !pd_stateid)
		return -EINVAL;

	if (mds_compound_init(&mc, 2, "proxy_done"))
		return -ENOMEM;
	if (mds_compound_add_sequence(&mc, ms))
		goto out;

	slot = mds_compound_add_op(&mc, OP_PROXY_DONE);
	if (!slot)
		goto out;
	slot->nfs_argop4_u.opproxy_done.pd_stateid = *pd_stateid;
	slot->nfs_argop4_u.opproxy_done.pd_status = pd_status;

	sret = mds_compound_send(&mc, ms);
	if (sret && sret != -EREMOTEIO) {
		ret = sret;
		goto out;
	}
	if (mc.mc_res.status != NFS4_OK) {
		ret = proxy_op_nfsstat_to_errno(mc.mc_res.status);
		goto out;
	}
	if (mc.mc_res.resarray.resarray_len < 2) {
		ret = -EREMOTEIO;
		goto out;
	}
	ret = proxy_op_nfsstat_to_errno(
		mc.mc_res.resarray.resarray_val[1]
			.nfs_resop4_u.opproxy_done.pdr_status);

out:
	mds_compound_fini(&mc);
	return ret;
}

int mds_session_send_proxy_cancel(struct mds_session *ms,
				  const stateid4 *pc_stateid)
{
	struct mds_compound mc;
	nfs_argop4 *slot;
	int ret = -EIO;
	int sret;

	if (!ms || !pc_stateid)
		return -EINVAL;

	if (mds_compound_init(&mc, 2, "proxy_cancel"))
		return -ENOMEM;
	if (mds_compound_add_sequence(&mc, ms))
		goto out;

	slot = mds_compound_add_op(&mc, OP_PROXY_CANCEL);
	if (!slot)
		goto out;
	slot->nfs_argop4_u.opproxy_cancel.pc_stateid = *pc_stateid;

	sret = mds_compound_send(&mc, ms);
	if (sret && sret != -EREMOTEIO) {
		ret = sret;
		goto out;
	}
	if (mc.mc_res.status != NFS4_OK) {
		ret = proxy_op_nfsstat_to_errno(mc.mc_res.status);
		goto out;
	}
	if (mc.mc_res.resarray.resarray_len < 2) {
		ret = -EREMOTEIO;
		goto out;
	}
	ret = proxy_op_nfsstat_to_errno(
		mc.mc_res.resarray.resarray_val[1]
			.nfs_resop4_u.opproxy_cancel.pcr_status);

out:
	mds_compound_fini(&mc);
	return ret;
}

int ps_migration_step(struct mds_session *ms, ps_assignment_handler handler,
		      void *ctx, uint32_t *lease_remaining_sec)
{
	if (!ms)
		return -EINVAL;

	/*
	 * Cap matches the MDS's PROXY_PROGRESS_MAX_BATCH (8 in slice
	 * 6c-y).  The MDS won't send more, so a fixed-size stack
	 * buffer is sufficient and avoids any allocation in the hot
	 * polling path.
	 */
	struct ps_progress_assignment items[8];
	uint32_t lease = 0;
	int got = mds_session_send_proxy_progress(
		ms, items, sizeof(items) / sizeof(items[0]), &lease);

	if (lease_remaining_sec)
		*lease_remaining_sec = lease;
	if (got < 0)
		return got;

	int processed = 0;

	for (int i = 0; i < got; i++) {
		if (handler) {
			int hret = handler(ms, &items[i], ctx);

			if (hret < 0)
				return hret;
		}
		processed++;
	}
	return processed;
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
		snprintf(hostname, sizeof(hostname), "reffs-mds-client");

	if (id && id[0] != '\0')
		snprintf(ms->ms_owner, sizeof(ms->ms_owner), "%s:%s", hostname,
			 id);
	else
		snprintf(ms->ms_owner, sizeof(ms->ms_owner), "%s:%u", hostname,
			 (unsigned)getpid());
}

/*
 * Open an NFSv4 client handle to `host`.  If `host` contains a colon
 * (e.g. "reffs-ps-a:2049"), the part after the colon is parsed as a
 * TCP port number and the connection is made directly to that port
 * via clnttcp_create -- bypassing portmap.  This is required for PS
 * deployments where the proxy listener and the native listener live
 * on different ports and the standard NFS port (2049) hosts the
 * proxy listener that does not register with rpcbind.  Without a
 * colon the host is passed straight to clnt_create which uses
 * portmap as before.
 *
 * Returns a CLIENT * on success or NULL on failure.  Caller frees
 * via clnt_destroy.
 */
static CLIENT *mds_session_clnt_open(const char *host)
{
	if (!host)
		return NULL;

	char host_buf[256];
	int port = 0;

	if (mds_parse_host_port(host, host_buf, sizeof(host_buf), &port) < 0)
		return NULL;

	/*
	 * No explicit port -> hand the bare host string to libtirpc's
	 * portmap-driven path.  This is the only call site that still
	 * goes through portmap; the explicit-port path below bypasses
	 * it for PS deployments where the proxy listener does not
	 * register with rpcbind.
	 */
	if (port == 0)
		return clnt_create(host_buf, NFS4_PROGRAM, NFS_V4, "tcp");

	struct addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM,
	};
	struct addrinfo *res = NULL;

	if (getaddrinfo(host_buf, NULL, &hints, &res) != 0 || !res)
		return NULL;

	struct sockaddr_in sin = *(struct sockaddr_in *)res->ai_addr;

	freeaddrinfo(res);
	sin.sin_port = htons((uint16_t)port);

	int fd = socket(AF_INET, SOCK_STREAM, 0);

	if (fd < 0)
		return NULL;
	if (connect(fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
		close(fd);
		return NULL;
	}

	CLIENT *clnt = clnttcp_create(&sin, NFS4_PROGRAM, NFS_V4, &fd, 0, 0);

	if (!clnt) {
		close(fd);
		return NULL;
	}
	/* clnttcp_create takes ownership of fd via the xprt; freed by clnt_destroy. */
	return clnt;
}

int mds_session_create(struct mds_session *ms, const char *host)
{
	int ret;

	/*
	 * Preserve caller-set fields across the memset-zero pattern.
	 * ms_owner: optional client-owner override, else default to
	 * hostname:PID.  ms_exchgid_flags: caller picks USE_NON_PNFS
	 * (PS-MDS path) vs USE_PNFS_MDS (MDS-to-DS path); zero
	 * defaults to USE_NON_PNFS in mds_exchange_id.
	 */
	char saved_owner[256];
	uint32_t saved_exchgid = ms->ms_exchgid_flags;

	memcpy(saved_owner, ms->ms_owner, sizeof(saved_owner));
	memset(ms, 0, sizeof(*ms));
	memcpy(ms->ms_owner, saved_owner, sizeof(ms->ms_owner));
	ms->ms_exchgid_flags = saved_exchgid;

	if (ms->ms_owner[0] == '\0')
		mds_session_set_owner(ms, NULL);

	if (pthread_mutex_init(&ms->ms_call_mutex, NULL) != 0)
		return -ENOMEM;

	ms->ms_clnt = mds_session_clnt_open(host);
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

/*
 * TLS-protected variant of mds_session_clnt_open.  Brings up an
 * mTLS connection (via tls_starttls or tls_direct_connect) and
 * wraps the SSL+fd in a libtirpc CLIENT* via mds_tls_xprt_create.
 *
 * On success returns a CLIENT* and stores the owning SSL_CTX into
 * *ctx_out -- caller (mds_session_create_tls) attaches it to
 * ms->ms_tls_ctx so mds_session_destroy frees it after
 * clnt_destroy.  On NULL return all transient resources are
 * released here.
 */
static CLIENT *
mds_session_clnt_open_tls(const char *host, uint16_t port, const char *tls_cert,
			  const char *tls_key, const char *tls_ca, int tls_mode,
			  bool insecure_no_verify, SSL_CTX **ctx_out)
{
	struct tls_client_config cfg = {
		.cert_path = (tls_cert && tls_cert[0] != '\0') ? tls_cert :
								 NULL,
		.key_path = (tls_key && tls_key[0] != '\0') ? tls_key : NULL,
		.ca_path = (tls_ca && tls_ca[0] != '\0') ? tls_ca : NULL,
		/*
		 * Bind verification to the connect target so a CA-signed
		 * cert for some other host cannot MITM us.  X509_VERIFY_PARAM
		 * accepts both DNS names and IP literals; "host" here is
		 * whatever the operator put in the [[proxy_mds]] address
		 * field, which is the same value the certificate must have
		 * issued for.
		 */
		.hostname = host,
		/*
		 * Server-cert verification is on by default; an empty
		 * tls_ca path disables verification only when the
		 * operator explicitly set tls_insecure_no_verify=true.
		 * The config parser rejects the unset case at startup
		 * so a forgotten tls_ca line cannot silently downgrade.
		 */
		.no_verify = insecure_no_verify ? 1 : 0,
	};
	SSL_CTX *ctx = NULL;
	SSL *ssl = NULL;
	int fd = -1;

	*ctx_out = NULL;

	fd = tls_tcp_connect(host, (int)port);
	if (fd < 0)
		return NULL;

	ctx = tls_client_ctx_create(&cfg);
	if (!ctx)
		goto err;

	if (tls_mode == REFFS_PROXY_TLS_DIRECT)
		ssl = tls_direct_connect(fd, ctx, 0);
	else
		ssl = tls_starttls(fd, ctx, 0);
	if (!ssl)
		goto err;

	CLIENT *clnt = mds_tls_xprt_create(fd, ssl, NFS4_PROGRAM, NFS_V4);

	if (!clnt) {
		SSL_free(ssl);
		goto err;
	}
	/*
	 * The XPRT now owns fd + ssl.  Hand the SSL_CTX out so the
	 * session lifetime owns it; the CTX MUST outlive every SSL
	 * spawned from it (per OpenSSL contract).  mds_session_destroy
	 * frees the CTX after clnt_destroy completes.
	 */
	*ctx_out = ctx;
	return clnt;
err:
	if (ssl)
		SSL_free(ssl);
	if (ctx)
		SSL_CTX_free(ctx);
	if (fd >= 0)
		close(fd);
	return NULL;
}

int mds_session_create_tls(struct mds_session *ms, const char *host,
			   uint16_t port, const char *tls_cert,
			   const char *tls_key, const char *tls_ca,
			   int tls_mode, bool tls_insecure_no_verify)
{
	int ret;

	/*
	 * No-cert config falls back to plain TCP -- a config carrying
	 * empty cert paths is the operator saying "this listener is
	 * pre-TLS"; refusing here would force every dev / smoke topology
	 * to add a cert to the [[proxy_mds]] block.
	 */
	if (!tls_cert || tls_cert[0] == '\0' || !tls_key ||
	    tls_key[0] == '\0') {
		/*
		 * Bare host (no ":port") for the default NFS port lets
		 * mds_session_create skip its colon-splitting parser
		 * entirely, which is the IPv4-only path today.  For a
		 * non-default port we still have to encode "host:port",
		 * which is why this branch is IPv4-only -- IPv6 literals
		 * contain colons and the parser strrchr's the last one.
		 * Fixing the IPv6 case requires bracketing in
		 * mds_session_clnt_open too and is tracked as
		 * NOT_NOW_BROWN_COW; the deploy/sanity stack uses IPv4
		 * addresses so this is not a regression for #139.
		 */
		if (port == 0 || port == 2049)
			return mds_session_create(ms, host);

		/* host:port = max 256 + 1 + 5 + NUL */
		char hostport[REFFS_CONFIG_MAX_HOST + 8];
		int n = snprintf(hostport, sizeof(hostport), "%s:%u", host,
				 (unsigned)port);

		if (n < 0 || (size_t)n >= sizeof(hostport))
			return -EINVAL;
		return mds_session_create(ms, hostport);
	}

	char saved_owner[256];

	memcpy(saved_owner, ms->ms_owner, sizeof(saved_owner));
	memset(ms, 0, sizeof(*ms));
	memcpy(ms->ms_owner, saved_owner, sizeof(ms->ms_owner));

	if (ms->ms_owner[0] == '\0')
		mds_session_set_owner(ms, NULL);

	if (pthread_mutex_init(&ms->ms_call_mutex, NULL) != 0)
		return -ENOMEM;

	SSL_CTX *tls_ctx = NULL;

	ms->ms_clnt = mds_session_clnt_open_tls(host, port, tls_cert, tls_key,
						tls_ca, tls_mode,
						tls_insecure_no_verify,
						&tls_ctx);
	if (!ms->ms_clnt) {
		pthread_mutex_destroy(&ms->ms_call_mutex);
		return -ECONNREFUSED;
	}
	ms->ms_tls_ctx = tls_ctx;

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
	mds_reclaim_complete(ms);
	return 0;

err:
	if (ms->ms_clnt)
		clnt_destroy(ms->ms_clnt);
	if (ms->ms_tls_ctx) {
		SSL_CTX_free(ms->ms_tls_ctx);
		ms->ms_tls_ctx = NULL;
	}
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

	ms->ms_clnt = mds_session_clnt_open(host);
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
		fprintf(stderr,
			"mds_session_create_sec: authgss_create_default failed\n");
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
	/*
	 * SSL_CTX must outlive every SSL spawned from it (OpenSSL
	 * contract).  The custom XPRT freed its SSL inside
	 * clnt_destroy above; freeing the CTX now is safe.  No-op
	 * for plain-TCP sessions where ms_tls_ctx stays NULL.
	 */
	if (ms->ms_tls_ctx) {
		SSL_CTX_free(ms->ms_tls_ctx);
		ms->ms_tls_ctx = NULL;
	}
	pthread_mutex_destroy(&ms->ms_call_mutex);
	memset(ms, 0, sizeof(*ms));
}
