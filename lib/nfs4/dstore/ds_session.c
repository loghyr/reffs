/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * MDS-->DS NFSv4.2 session for control-plane and InBand I/O.
 *
 * The MDS acts as a plain NFSv4 client (USE_NON_PNFS) to the DS.
 * Uses the ec_demo client library (mds_session / mds_compound)
 * for session management and compound building.
 *
 * Session is single-slot (serializes all operations to a DS).
 * NOT_NOW_BROWN_COW: multi-slot for concurrent InBand I/O.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <rpc/rpc.h>

#include "nfsv42_xdr.h"
#include "ec_client.h"
#include "reffs/dstore.h"
#include "reffs/dstore_ops.h"
#include "reffs/filehandle.h"
#include "reffs/log.h"

/* ------------------------------------------------------------------ */
/* Session create / destroy                                            */
/* ------------------------------------------------------------------ */

int ds_session_create(struct dstore *ds)
{
	struct mds_session *ms;
	int ret;

	TRACE("ds_session_create: entry dstore[%u] addr=%s", ds->ds_id,
	      ds->ds_address);

	if (ds->ds_v4_session) {
		TRACE("ds_session_create: dstore[%u] already connected",
		      ds->ds_id);
		return 0; /* already connected */
	}

	ms = calloc(1, sizeof(*ms));
	if (!ms) {
		TRACE("ds_session_create: dstore[%u] calloc failed", ds->ds_id);
		return -ENOMEM;
	}

	/*
	 * Build a unique owner string: "mds-ds:<ds_id>:<hostname>:<pid>"
	 * This distinguishes the MDS-->DS session from client-->DS sessions.
	 */
	char hostname[64];

	if (gethostname(hostname, sizeof(hostname)) < 0)
		snprintf(hostname, sizeof(hostname), "mds");

	snprintf(ms->ms_owner, sizeof(ms->ms_owner), "mds-ds:%u:%s:%u",
		 ds->ds_id, hostname, (unsigned)getpid());

	/*
	 * Connect to the DS.  mds_session_create does:
	 *   clnt_create --> EXCHANGE_ID --> CREATE_SESSION --> RECLAIM_COMPLETE
	 *
	 * Slice plan-A.iii flipped mds_session_create's EXCHANGE_ID
	 * flag from EXCHGID4_FLAG_USE_PNFS_MDS to USE_NON_PNFS (the
	 * PS is a regular NFSv4 client of its upstream MDS).  The DS
	 * also wants USE_NON_PNFS, so the change is the right
	 * direction for this caller too.  Note: any code path on the
	 * DS server side that previously gated on USE_PNFS_MDS
	 * (e.g., TRUST_STATEID per
	 * lib/nfs4/server/trust_stateid_ops.c) now sees USE_NON_PNFS
	 * here as well; those paths need verification before being
	 * relied on against an MDS-to-DS session.
	 */
	TRACE("ds_session_create: dstore[%u] calling mds_session_create",
	      ds->ds_id);
	ret = mds_session_create(ms, ds->ds_address);
	TRACE("ds_session_create: dstore[%u] mds_session_create returned %d",
	      ds->ds_id, ret);
	if (ret) {
		LOG("ds_session: failed to connect to DS %s (dstore %u): %d",
		    ds->ds_address, ds->ds_id, ret);
		free(ms);
		return ret;
	}

	/*
	 * Get the DS root filehandle via PUTROOTFH + GETFH.
	 */
	struct mds_compound mc;

	ret = mds_compound_init(&mc, 3, "ds_get_root_fh");
	if (ret) {
		mds_session_destroy(ms);
		free(ms);
		return ret;
	}

	if (!mds_compound_add_sequence(&mc, ms))
		goto fh_err;

	if (!mds_compound_add_op(&mc, OP_PUTROOTFH))
		goto fh_err;

	if (!mds_compound_add_op(&mc, OP_GETFH))
		goto fh_err;

	ret = mds_compound_send(&mc, ms);
	TRACE("ds_session_create: dstore[%u] PUTROOTFH+GETFH send ret=%d "
	      "compound_status=%u resarray_len=%u",
	      ds->ds_id, ret, (unsigned)mc.mc_res.status,
	      mc.mc_res.resarray.resarray_len);
	if (ret)
		goto fh_err;

	if (mc.mc_res.status != NFS4_OK ||
	    mc.mc_res.resarray.resarray_len < 3) {
		ret = -EREMOTEIO;
		goto fh_err;
	}

	nfs_resop4 *getfh_res = &mc.mc_res.resarray.resarray_val[2];
	GETFH4resok *getfh_ok =
		&getfh_res->nfs_resop4_u.opgetfh.GETFH4res_u.resok4;

	if (getfh_ok->object.nfs_fh4_len > DSTORE_MAX_FH) {
		LOG("ds_session: DS %s root FH too large (%u > %u)",
		    ds->ds_address, getfh_ok->object.nfs_fh4_len,
		    DSTORE_MAX_FH);
		ret = -EOVERFLOW;
		goto fh_err;
	}

	memcpy(ds->ds_root_fh, getfh_ok->object.nfs_fh4_val,
	       getfh_ok->object.nfs_fh4_len);
	ds->ds_root_fh_len = getfh_ok->object.nfs_fh4_len;
	uint64_t after_mount __attribute__((unused)) = __atomic_or_fetch(
		&ds->ds_state, DSTORE_IS_MOUNTED, __ATOMIC_RELEASE);

	TRACE("ds_session_create: dstore[%u] MOUNTED set, state now 0x%lx, fh_len=%u",
	      ds->ds_id, (unsigned long)after_mount, ds->ds_root_fh_len);

	mds_compound_fini(&mc);
	ds->ds_v4_session = ms;

	/*
	 * Capability probe: send TRUST_STATEID with anonymous stateid.
	 * NFS4ERR_INVAL response means the DS supports tight coupling
	 * (pNFS Flex Files v2 TRUST_STATEID / REVOKE_STATEID).
	 * ds_tight_coupled is read-only after this point.
	 */
	if (ds->ds_ops->probe_tight_coupling) {
		int r = ds->ds_ops->probe_tight_coupling(ds);

		ds->ds_tight_coupled = (r == 0);
		TRACE("ds_session: DS %s (dstore %u) tight coupling %s",
		      ds->ds_address, ds->ds_id,
		      ds->ds_tight_coupled ? "enabled" : "disabled");
	}

	TRACE("ds_session: connected to DS %s (dstore %u), root FH %u bytes",
	      ds->ds_address, ds->ds_id, ds->ds_root_fh_len);
	return 0;

fh_err:
	TRACE("ds_session_create: dstore[%u] fh_err exit ret=%d", ds->ds_id,
	      ret);
	mds_compound_fini(&mc);
	mds_session_destroy(ms);
	free(ms);
	return ret;
}

void ds_session_destroy(struct dstore *ds)
{
	if (!ds->ds_v4_session)
		return;

	mds_session_destroy(ds->ds_v4_session);
	free(ds->ds_v4_session);
	ds->ds_v4_session = NULL;
}
