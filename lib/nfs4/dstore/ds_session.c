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

void resolve_ds_ip(struct dstore *ds);

/* ------------------------------------------------------------------ */
/* Session create / destroy                                            */
/* ------------------------------------------------------------------ */

int ds_session_create(struct dstore *ds)
{
	struct mds_session *ms;
	int ret;

	/*
	 * Already-connected short-circuit.  Read via the borrow accessor
	 * so concurrent reconnect (renewal thread) cannot swap the
	 * pointer between this check and the early return.  Release
	 * immediately -- we only consult the pointer to decide whether
	 * to skip work, we don't hold ms across any RPC here.
	 */
	struct mds_session *existing = dstore_session_borrow(ds);

	if (existing) {
		dstore_session_release(ds);
		return 0;
	}

	ms = calloc(1, sizeof(*ms));
	if (!ms)
		return -ENOMEM;

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
	/*
	 * mds_session_clnt_open parses the host string for a "host:port"
	 * suffix and bypasses portmap when a port is present.  When the
	 * dstore config carries an explicit port (e.g. multi-host realnet
	 * benches that publish each DS on a distinct host port to dodge
	 * portmap-on-shared-IP), the NFSv3 dstore path at
	 * lib/nfs4/dstore/dstore.c honours ds->ds_port via clnttcp_create
	 * but the NFSv4 path here was passing the bare address -- libtirpc
	 * then fell through to portmap and the connect failed with
	 * "RPC: Program not registered".  Format the suffix once for the
	 * MDS-to-DS NFSv4 session so per-DS port plumbing matches the v3
	 * path's behaviour.
	 */
	char host_buf[REFFS_CONFIG_MAX_HOST + 8];
	const char *host_arg;

	if (ds->ds_port > 0) {
		snprintf(host_buf, sizeof(host_buf), "%s:%u", ds->ds_address,
			 (unsigned)ds->ds_port);
		host_arg = host_buf;
	} else {
		host_arg = ds->ds_address;
	}

	ret = mds_session_create(ms, host_arg);
	if (ret) {
		LOG("ds_session: failed to connect to DS %s (dstore %u): %d",
		    host_arg, ds->ds_id, ret);
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

	if (mds_compound_add_sequence(&mc, ms))
		goto fh_err;

	if (!mds_compound_add_op(&mc, OP_PUTROOTFH))
		goto fh_err;

	if (!mds_compound_add_op(&mc, OP_GETFH))
		goto fh_err;

	ret = mds_compound_send(&mc, ms);
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

	/*
	 * Resolve hostname to dotted-decimal IP for use in
	 * GETDEVICEINFO uaddrs.  The MDS-side encoder requires
	 * a numeric IPv4 address (uaddr "h1.h2.h3.h4.p1.p2"); the
	 * NFSv3 mount path does this in mount_get_root_fh, but
	 * NFSv4 dstores took a separate code path and were leaving
	 * ds_ip unset, producing uaddrs like ".8.1" that the
	 * client's parse_uaddr rejects with -EINVAL.
	 */
	resolve_ds_ip(ds);

	__atomic_or_fetch(&ds->ds_state, DSTORE_IS_MOUNTED, __ATOMIC_RELEASE);

	mds_compound_fini(&mc);
	/*
	 * Publish the new session via the wrlock-taking accessor.  Any
	 * pre-existing session pointer (e.g., after a reconnect that
	 * parked the slot to NULL) is destroyed outside the wlock by
	 * dstore_session_replace itself.  On the initial bring-up from
	 * dstore_alloc the slot is NULL, so the destroy block is a
	 * no-op.
	 */
	(void)dstore_session_replace(ds, ms);

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
	mds_compound_fini(&mc);
	mds_session_destroy(ms);
	free(ms);
	return ret;
}

void ds_session_destroy(struct dstore *ds)
{
	/*
	 * Park the slot via the wrlock-taking accessor.  Replace with
	 * NULL atomically publishes the absence to all future borrowers,
	 * then destroys the old session outside the wlock.  Idempotent:
	 * second call observes old_session == NULL and is a no-op.
	 */
	(void)dstore_session_replace(ds, NULL);
}

/* ------------------------------------------------------------------ */
/* Borrow / release / replace under ds_v4_session_rwlock              */
/* ------------------------------------------------------------------ */

/*
 * The borrow / release / replace pattern closes a pre-existing latent
 * UAF window where dstore_ops_nfsv4 callers dereferenced
 * ds->ds_v4_session without synchronisation against reconnect.  See
 * .claude/design/mds-ds-session-keepalive.md "BLOCKER B1" and the
 * mirror implementation at lib/nfs4/ps/ps_state.c:507-607 -- the
 * shape is identical; we replicate it (rather than share code) because
 * the dstore has no listener-id indirection (callers already hold the
 * struct dstore *) and the gate condition is "pointer non-NULL",
 * not "listener RUNNING".
 */

struct mds_session *dstore_session_borrow(struct dstore *ds)
{
	if (!ds)
		return NULL;

	pthread_rwlock_rdlock(&ds->ds_v4_session_rwlock);

	/*
	 * Load the pointer UNDER the rdlock so dstore_session_replace
	 * cannot swap it out from under us.  No separate "state"
	 * acquire-load needed -- a NULL pointer already encodes the
	 * "parked during reconnect" state (replace publishes NULL
	 * before destroying the old session).
	 */
	struct mds_session *ms = ds->ds_v4_session;

	if (!ms) {
		pthread_rwlock_unlock(&ds->ds_v4_session_rwlock);
		return NULL;
	}
	return ms;
}

void dstore_session_release(struct dstore *ds)
{
	if (!ds)
		return;
	pthread_rwlock_unlock(&ds->ds_v4_session_rwlock);
}

int dstore_session_replace(struct dstore *ds, struct mds_session *new_session)
{
	if (!ds)
		return -EINVAL;

	pthread_rwlock_wrlock(&ds->ds_v4_session_rwlock);
	struct mds_session *old_session = ds->ds_v4_session;

	ds->ds_v4_session = new_session;
	pthread_rwlock_unlock(&ds->ds_v4_session_rwlock);

	/*
	 * Destroy the old session OUTSIDE the wrlock.  See the mirror
	 * rationale in lib/nfs4/ps/ps_state.c:589-605:
	 *   1. mds_session_destroy issues DESTROY_SESSION +
	 *      DESTROY_CLIENTID -- RPC-timeout-many seconds on a wedged
	 *      DS.  Holding the wrlock that long blocks every new
	 *      borrower; replace calls would also serialise on each
	 *      other through destroy.
	 *   2. By the time we get here, no thread can reach the old
	 *      session via ds_v4_session: the publish-pointer was
	 *      cleared under the wlock above, and existing borrowers
	 *      still hold the rdlock -- our wrlock acquisition above
	 *      already waited them out.
	 */
	if (old_session) {
		mds_session_destroy(old_session);
		free(old_session);
	}
	return 0;
}
