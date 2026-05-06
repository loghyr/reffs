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
#include <stdlib.h>
#include <string.h>

#include <rpc/rpc.h>

#include "nfsv42_xdr.h"
#include "ec_client.h"

#define MDS_RPC_TIMEOUT_SEC 30

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
		ret = -EIO;
		goto out;
	}

	if (mc->mc_res.status != NFS4_OK)
		ret = -EREMOTEIO;

out:
	return ret;
}
