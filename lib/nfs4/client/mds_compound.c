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

	memcpy(args->sa_sessionid, ms->ms_sessionid, sizeof(sessionid4));
	args->sa_sequenceid = ms->ms_slot_seqid;
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
	struct timeval tv = { .tv_sec = MDS_RPC_TIMEOUT_SEC, .tv_usec = 0 };
	enum clnt_stat rpc_stat;

	memset(&mc->mc_res, 0, sizeof(mc->mc_res));

	rpc_stat = clnt_call(ms->ms_clnt, NFSPROC4_COMPOUND,
			     (xdrproc_t)xdr_COMPOUND4args,
			     (caddr_t)&mc->mc_args, (xdrproc_t)xdr_COMPOUND4res,
			     (caddr_t)&mc->mc_res, tv);

	if (rpc_stat != RPC_SUCCESS)
		return -EIO;

	/* Bump slot seqid on success. */
	ms->ms_slot_seqid++;

	if (mc->mc_res.status != NFS4_OK)
#if defined(EREMOTEIO)
		return -EREMOTEIO;
#else
		/* FreeBSD has no EREMOTEIO.  Use the closest POSIX cousin. */
		return -EIO;
#endif

	return 0;
}
