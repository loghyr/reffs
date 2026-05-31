/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * COMPOUND builder for the EC demo client.
 *
 * Assembles an NFSv4.2 COMPOUND4args, sends it via clnt_call, and
 * provides access to the COMPOUND4res.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rpc/rpc.h>

#include "nfsv42_xdr.h"
#include "nfsv42_names.h"
#include "ec_client.h"

#define MDS_RPC_TIMEOUT_SEC 30

/*
 * Symbolic name for an enum auth_stat value (RFC 5531 sec 8.2).
 *
 * Used when clnt_call returns RPC_AUTHERROR -- without decoding,
 * the central log surfaces only the integer sub-code, which
 * collapses several distinct on-wire failure modes
 * (rejected creds, GSS context problem, GSS cred problem, ...)
 * into one indistinguishable line.  The "Auth Bogus Credentials
 * (seal broken)" failure that motivates the krb5 multi-mount
 * stress harness lives in the AUTH_REJECTEDCRED /
 * RPCSEC_GSS_CREDPROBLEM / RPCSEC_GSS_CTXPROBLEM neighbourhood;
 * naming it on the wire is what lets the operator confirm the
 * load shape is hitting the right path.
 *
 * Names match RFC 5531 sec 8.2 and the libtirpc auth_stat enum.
 * Unknown values produce "AUTH_STAT_?".
 */
static const char *auth_stat_name(enum auth_stat s)
{
	switch (s) {
	case AUTH_OK:
		return "AUTH_OK";
	case AUTH_BADCRED:
		return "AUTH_BADCRED";
	case AUTH_REJECTEDCRED:
		return "AUTH_REJECTEDCRED";
	case AUTH_BADVERF:
		return "AUTH_BADVERF";
	case AUTH_REJECTEDVERF:
		return "AUTH_REJECTEDVERF";
	case AUTH_TOOWEAK:
		return "AUTH_TOOWEAK";
	case AUTH_INVALIDRESP:
		return "AUTH_INVALIDRESP";
	case AUTH_FAILED:
		return "AUTH_FAILED";
	case AUTH_KERB_GENERIC:
		return "AUTH_KERB_GENERIC";
	case AUTH_TIMEEXPIRE:
		return "AUTH_TIMEEXPIRE";
	case AUTH_TKT_FILE:
		return "AUTH_TKT_FILE";
	case AUTH_DECODE:
		return "AUTH_DECODE";
	case AUTH_NET_ADDR:
		return "AUTH_NET_ADDR";
	case RPCSEC_GSS_CREDPROBLEM:
		return "RPCSEC_GSS_CREDPROBLEM";
	case RPCSEC_GSS_CTXPROBLEM:
		return "RPCSEC_GSS_CTXPROBLEM";
	default:
		return "AUTH_STAT_?";
	}
}

/* ------------------------------------------------------------------ */
/* Init / fini                                                         */
/* ------------------------------------------------------------------ */

int mds_compound_init(struct mds_compound *mc, uint32_t max_ops,
		      const char *tag)
{
	memset(mc, 0, sizeof(*mc));

	mc->mc_args.argarray.argarray_val = calloc(max_ops, sizeof(nfs_argop4));
	if (!mc->mc_args.argarray.argarray_val)
		return -ENOMEM;

	mc->mc_args.minorversion = 2;
	mc->mc_args.tag.utf8string_val = (char *)tag;
	mc->mc_args.tag.utf8string_len = tag ? strlen(tag) : 0;
	mc->mc_max_ops = max_ops;
	mc->mc_count = 0;

	return 0;
}

void mds_compound_fini(struct mds_compound *mc)
{
	xdr_free((xdrproc_t)xdr_COMPOUND4res, (caddr_t)&mc->mc_res);
	free(mc->mc_args.argarray.argarray_val);
	memset(mc, 0, sizeof(*mc));
}

/* ------------------------------------------------------------------ */
/* Add ops                                                             */
/* ------------------------------------------------------------------ */

nfs_argop4 *mds_compound_add_op(struct mds_compound *mc, nfs_opnum4 op)
{
	nfs_argop4 *slot;

	if (mc->mc_count >= mc->mc_max_ops)
		return NULL;

	slot = &mc->mc_args.argarray.argarray_val[mc->mc_count];
	memset(slot, 0, sizeof(*slot));
	slot->argop = op;
	mc->mc_count++;
	mc->mc_args.argarray.argarray_len = mc->mc_count;

	return slot;
}

int mds_compound_add_sequence(struct mds_compound *mc, struct mds_session *ms)
{
	nfs_argop4 *slot = mds_compound_add_op(mc, OP_SEQUENCE);

	if (!slot)
		return -ENOSPC;

	SEQUENCE4args *args = &slot->nfs_argop4_u.opsequence;

	/*
	 * sa_sessionid and sa_sequenceid are placeholders -- they are
	 * (re)written inside ms_call_mutex by mds_compound_send_with_auth
	 * just before clnt_call.  Single-slot session serialization
	 * requires seqids to be assigned in send order, so we cannot
	 * read ms_slot_seqid here at compound-build time without a
	 * lock; concurrent workers would read identical values and
	 * issue duplicate seqids -> NFS4ERR_SEQ_MISORDERED at the server.
	 */
	memcpy(args->sa_sessionid, ms->ms_sessionid, sizeof(sessionid4));
	args->sa_sequenceid = 0;
	args->sa_slotid = 0;
	args->sa_highest_slotid = 0;
	args->sa_cachethis = false;

	return 0;
}

/* ------------------------------------------------------------------ */
/* Send                                                                */
/* ------------------------------------------------------------------ */

int mds_compound_send(struct mds_compound *mc, struct mds_session *ms)
{
	return mds_compound_send_with_auth(mc, ms, NULL);
}

__attribute__((weak)) int
mds_compound_send_with_auth(struct mds_compound *mc, struct mds_session *ms,
			    const struct authunix_parms *creds)
{
	struct timeval tv = { .tv_sec = MDS_RPC_TIMEOUT_SEC, .tv_usec = 0 };
	enum clnt_stat rpc_stat;
	AUTH *override_auth = NULL;
	int ret = 0;

	memset(&mc->mc_res, 0, sizeof(mc->mc_res));

	if (creds) {
		/*
		 * authunix_create takes non-const machname / gids
		 * pointers in the standard TIRPC prototype; cast away
		 * the const to match.  The call copies the bytes into
		 * its own xdr_opaque so the caller's memory does not
		 * need to outlive the AUTH.
		 */
		override_auth = authunix_create(
			creds->aup_machname ? creds->aup_machname : (char *)"",
			creds->aup_uid, creds->aup_gid, (int)creds->aup_len,
			creds->aup_gids);
		if (!override_auth)
			return -ENOMEM;
	}

	pthread_mutex_lock(&ms->ms_call_mutex);

	/*
	 * Single-slot serialization: assign sa_sequenceid INSIDE the
	 * lock so concurrent worker threads see consecutive values.
	 * Patches op[0] (SEQUENCE) sessionid+seqid; harmless if the
	 * caller built a compound that does not start with SEQUENCE.
	 */
	SEQUENCE4args *seq = NULL;

	if (mc->mc_count > 0 &&
	    mc->mc_args.argarray.argarray_val[0].argop == OP_SEQUENCE) {
		seq = &mc->mc_args.argarray.argarray_val[0]
			       .nfs_argop4_u.opsequence;
		memcpy(seq->sa_sessionid, ms->ms_sessionid, sizeof(sessionid4));
		seq->sa_sequenceid = ms->ms_slot_seqid;
	}

	AUTH *saved_auth = NULL;

	if (override_auth) {
		saved_auth = ms->ms_clnt->cl_auth;
		ms->ms_clnt->cl_auth = override_auth;
	}

	rpc_stat = clnt_call(ms->ms_clnt, NFSPROC4_COMPOUND,
			     (xdrproc_t)xdr_COMPOUND4args,
			     (caddr_t)&mc->mc_args, (xdrproc_t)xdr_COMPOUND4res,
			     (caddr_t)&mc->mc_res, tv);

	if (override_auth) {
		ms->ms_clnt->cl_auth = saved_auth;
	}

	/*
	 * Bump slot seqid only if SEQUENCE itself succeeded.  RFC 8881
	 * S18.46.3: the server does not advance its slot when SEQUENCE
	 * fails (e.g. NFS4ERR_SEQ_MISORDERED, BADSESSION).  Bumping on
	 * RPC_SUCCESS regardless of SEQUENCE status drifts the local
	 * seqid ahead of the server and produces persistent
	 * SEQ_MISORDERED on every subsequent call.  Increment under
	 * the lock to keep assignment+increment atomic.
	 */
	if (rpc_stat == RPC_SUCCESS && seq != NULL &&
	    mc->mc_res.resarray.resarray_len > 0 &&
	    mc->mc_res.resarray.resarray_val[0].resop == OP_SEQUENCE &&
	    mc->mc_res.resarray.resarray_val[0]
			    .nfs_resop4_u.opsequence.sr_status == NFS4_OK)
		ms->ms_slot_seqid++;

	pthread_mutex_unlock(&ms->ms_call_mutex);

	if (override_auth)
		auth_destroy(override_auth);

	if (rpc_stat != RPC_SUCCESS) {
		/*
		 * Stage 4 INV-6 dig: surface the TIRPC failure mode.  The
		 * caller (renewal_tick_one, layout/IO compound issuers)
		 * only logs `errno=I/O` from the -EIO we return here, which
		 * collapses every distinct TIRPC failure -- timeout,
		 * unreachable peer, malformed reply, auth error -- to one
		 * indistinguishable line.  Track 2 runs 5/6/7 each lost a
		 * PS->MDS session at "errno=I/O sr_status=0" with no MDS-
		 * side log surface; the actual rpc_stat / re_status (from
		 * clnt_geterr) is the missing attribution.
		 */
		struct rpc_err rerr;
		clnt_geterr(ms->ms_clnt, &rerr);
		/*
		 * For RPC_AUTHERROR, re_errno and re_why alias the same
		 * union member -- re_why holds the enum auth_stat (RFC 5531
		 * sec 8.2).  Decode it symbolically; otherwise stick with
		 * the numeric re_errno (it is the related system errno for
		 * non-AUTH RPC failures).  The auth_stat surfacing is what
		 * lets the operator distinguish AUTH_REJECTEDCRED (rejected
		 * credential) from RPCSEC_GSS_CTXPROBLEM (context broken)
		 * from RPCSEC_GSS_CREDPROBLEM (credential expired) on the
		 * wire -- the customer "seal broken" symptom that motivates
		 * the krb5 multi-mount stress harness lives in that group.
		 */
		if (rpc_stat == RPC_AUTHERROR) {
			fprintf(stderr,
				"mds_compound_send: clnt_call returned rpc_stat=%d (%s) "
				"re_status=%d auth_stat=%s(%d) tag=%.*s\n",
				(int)rpc_stat, clnt_sperrno(rpc_stat),
				(int)rerr.re_status,
				auth_stat_name(rerr.re_why), (int)rerr.re_why,
				(int)mc->mc_args.tag.utf8string_len,
				mc->mc_args.tag.utf8string_val ?
					mc->mc_args.tag.utf8string_val :
					"");
			ret = -EACCES;
		} else {
			fprintf(stderr,
				"mds_compound_send: clnt_call returned rpc_stat=%d (%s) "
				"re_status=%d re_errno=%d tag=%.*s\n",
				(int)rpc_stat, clnt_sperrno(rpc_stat),
				(int)rerr.re_status, rerr.re_errno,
				(int)mc->mc_args.tag.utf8string_len,
				mc->mc_args.tag.utf8string_val ?
					mc->mc_args.tag.utf8string_val :
					"");
			ret = -EIO;
		}
		goto out;
	}

	if (mc->mc_res.status != NFS4_OK) {
		/*
		 * Surface which op failed and the symbolic NFS4ERR_* name
		 * before collapsing every COMPOUND-level non-OK to -121
		 * (-EREMOTEIO).  Without this, callers (mds_file_open,
		 * mds_layoutget, mds_write, ...) only see a generic -121
		 * with no attribution -- and the QA reproducer for the
		 * krb5 multi-mount stress lived in that fog window for
		 * a while.
		 *
		 * NFSv4 stops on first error per RFC 8881 sec 2.10.6.4,
		 * so the failing op is the last entry in resarray.  When
		 * resarray_len == 0 (no op processed -- server rejected
		 * before dispatch, e.g. NFS4ERR_MINOR_VERS_MISMATCH), we
		 * print "(no op)" instead of dereferencing.
		 */
		nfs_opnum4 failing_op = 0;
		const char *op_name = "(no op)";
		uint32_t nres = mc->mc_res.resarray.resarray_len;

		if (nres > 0) {
			failing_op =
				mc->mc_res.resarray.resarray_val[nres - 1].resop;
			op_name = nfs4_op_name(failing_op);
		}

		/*
		 * Tag distinguishes the endpoint by convention -- "open",
		 * "layoutget", "exchange_id", ...  go to the MDS; tags
		 * with the "chunk_" or "ds_" prefix go to a data server.
		 * Caller of mds_compound_init sets the tag; this log
		 * thus carries enough to disambiguate MDS vs DS.
		 */
		fprintf(stderr,
			"mds_compound_send: COMPOUND tag=\"%.*s\" "
			"op[%u]=%s(%u) status=%s(%u)\n",
			(int)mc->mc_args.tag.utf8string_len,
			mc->mc_args.tag.utf8string_val ?
				mc->mc_args.tag.utf8string_val :
				"",
			nres ? nres - 1 : 0, op_name, (unsigned int)failing_op,
			nfs4_err_name(mc->mc_res.status),
			(unsigned int)mc->mc_res.status);

		ret = -EREMOTEIO;
	}

out:
	return ret;
}
