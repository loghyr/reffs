/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#include "nfsv42_xdr.h"
#include "nfsv42_names.h"
#include "reffs/log.h"
#include "reffs/rpc.h"
#include "reffs/inode.h"
#include "reffs/lock.h"
#include "reffs/stateid.h"
#include "reffs/filehandle.h"
#include "reffs/server.h"
#include "nfs4/compound.h"
#include "nfs4/ops.h"
#include "nfs4/errors.h"
#include "nfs4/stateid.h"
#include "nfs4/client.h"
#include "nfs4/session.h"
#include "nfs4/cb.h"

uint32_t nfs4_op_delegpurge(struct compound *compound)
{
	DELEGPURGE4res *res = NFS4_OP_RES_SETUP(compound, opdelegpurge);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}

uint32_t nfs4_op_delegreturn(struct compound *compound)
{
	DELEGRETURN4args *args = NFS4_OP_ARG_SETUP(compound, opdelegreturn);
	DELEGRETURN4res *res = NFS4_OP_RES_SETUP(compound, opdelegreturn);
	nfsstat4 *status = &res->status;

	if (network_file_handle_empty(&compound->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		return 0;
	}

	if (stateid4_is_special(&args->deleg_stateid)) {
		*status = NFS4ERR_BAD_STATEID;
		return 0;
	}

	uint32_t seqid, id, type, cookie;
	unpack_stateid4(&args->deleg_stateid, &seqid, &id, &type, &cookie);

	if (type != Delegation_Stateid) {
		*status = NFS4ERR_BAD_STATEID;
		return 0;
	}

	struct stateid *stid = stateid_find(compound->c_inode, id);
	if (!stid || stid->s_tag != Delegation_Stateid ||
	    stid->s_cookie != cookie) {
		stateid_put(stid);
		*status = NFS4ERR_BAD_STATEID;
		return 0;
	}

	if (compound->c_nfs4_client &&
	    stid->s_client != nfs4_client_to_client(compound->c_nfs4_client)) {
		stateid_put(stid);
		*status = NFS4ERR_BAD_STATEID;
		return 0;
	}

	uint32_t cur_seqid = __atomic_load_n(&stid->s_seqid, __ATOMIC_RELAXED);
	if (seqid != 0) {
		if (seqid < cur_seqid) {
			stateid_put(stid);
			*status = NFS4ERR_OLD_STATEID;
			return 0;
		}
		if (seqid > cur_seqid) {
			stateid_put(stid);
			*status = NFS4ERR_BAD_STATEID;
			return 0;
		}
	}

	struct delegation_stateid *ds = stid_to_delegation(stid);

	/*
	 * RFC 9754 OPEN XOR: if this delegation subsumed an open, clean
	 * up the internal open_stateid (share removal + unhash + free)
	 * now, since the client will not send a separate CLOSE.
	 */
	if (ds->ds_open) {
		struct open_stateid *os = ds->ds_open;
		ds->ds_open = NULL;
		pthread_mutex_lock(&compound->c_inode->i_lock_mutex);
		reffs_share_remove(compound->c_inode, &os->os_owner, NULL);
		pthread_mutex_unlock(&compound->c_inode->i_lock_mutex);
		stateid_inode_unhash(&os->os_stid);
		stateid_client_unhash(&os->os_stid);
		stateid_put(&os->os_stid); /* state ref --> freed via RCU */
	}

	/*
	 * Unhash atomically -- if another DELEGRETURN already unhashed
	 * this stateid, bail out to prevent refcount underflow.
	 */
	if (!stateid_inode_unhash(stid)) {
		stateid_put(stid); /* drop the find ref */
		*status = NFS4ERR_BAD_STATEID;
		return 0;
	}
	stateid_client_unhash(stid);

	if (compound->c_curr_stid == stid) {
		stateid_put(stid); /* put c_curr_stid ref */
		compound->c_curr_stid = NULL;
	}

	stateid_put(stid); /* find ref */
	stateid_put(stid); /* state ref --> freed */

	/*
	 * RFC 8881 S12.5.5.1: DELEGRETURN should implicitly return any
	 * write layout the client still holds on this file.
	 * The response has no data, so we can go async immediately.
	 */
	return nfs4_layout_implicit_return_rw(compound,
					      nfs4_op_layoutreturn_resume);
}

uint32_t nfs4_op_get_dir_delegation(struct compound *compound)
{
	GET_DIR_DELEGATION4res *res =
		NFS4_OP_RES_SETUP(compound, opget_dir_delegation);
	nfsstat4 *status = &res->gddr_status;

	if (network_file_handle_empty(&compound->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		return 0;
	}

	if (!S_ISDIR(compound->c_inode->i_mode)) {
		*status = NFS4ERR_NOTDIR;
		return 0;
	}

	struct client *client =
		compound->c_nfs4_client ?
			nfs4_client_to_client(compound->c_nfs4_client) :
			NULL;
	if (!client) {
		*status = NFS4ERR_SERVERFAULT;
		return 0;
	}

	struct delegation_stateid *ds =
		delegation_stateid_alloc(compound->c_inode, client);
	if (!ds) {
		/*
		 * Can't grant -- tell the client the delegation is
		 * unavailable rather than failing the whole compound.
		 */
		GET_DIR_DELEGATION4res_non_fatal *nf =
			&res->GET_DIR_DELEGATION4res_u.gddr_res_non_fatal4;
		nf->gddrnf_status = GDD4_UNAVAIL;
		nf->GET_DIR_DELEGATION4res_non_fatal_u
			.gddrnf_will_signal_deleg_avail = FALSE;
		return 0;
	}

	/*
	 * Bump seqid so the client sees a fresh stateid.
	 * Grandfathered GCC builtin -- s_seqid is not _Atomic.
	 */
	__atomic_fetch_add(&ds->ds_stid.s_seqid, 1, __ATOMIC_ACQ_REL);

	GET_DIR_DELEGATION4res_non_fatal *nf =
		&res->GET_DIR_DELEGATION4res_u.gddr_res_non_fatal4;
	nf->gddrnf_status = GDD4_OK;

	GET_DIR_DELEGATION4resok *resok =
		&nf->GET_DIR_DELEGATION4res_non_fatal_u.gddrnf_resok4;

	/* Use directory's monotonic changeid as the cookie verifier. */
	uint64_t changeid = atomic_load_explicit(&compound->c_inode->i_changeid,
						 memory_order_relaxed);
	memcpy(resok->gddr_cookieverf, &changeid,
	       sizeof(resok->gddr_cookieverf));

	pack_stateid4(&resok->gddr_stateid, &ds->ds_stid);

	/* No notification capabilities -- recall-on-mutate model. */
	resok->gddr_notification.bitmap4_len = 0;
	resok->gddr_notification.bitmap4_val = NULL;
	resok->gddr_child_attributes.bitmap4_len = 0;
	resok->gddr_child_attributes.bitmap4_val = NULL;
	resok->gddr_dir_attributes.bitmap4_len = 0;
	resok->gddr_dir_attributes.bitmap4_val = NULL;

	return 0;
}

/*
 * Recall all directory delegations on dir except those held by exclude.
 * Fire-and-forget -- mirrors the file delegation recall pattern in OPEN.
 */
void nfs4_recall_dir_delegations(struct server_state *ss, struct inode *dir,
				 struct client *exclude)
{
	struct stateid *stid;

	if (!dir || !dir->i_stateids)
		return;

	/*
	 * Iterate delegation stateids.  stateid_inode_find_delegation
	 * returns one at a time (the first non-excluded delegation it
	 * finds), so loop until none remain.
	 */
	while ((stid = stateid_inode_find_delegation(dir, exclude)) != NULL) {
		struct nfs4_client *ds_nc =
			stid->s_client ? client_to_nfs4(stid->s_client) : NULL;
		struct nfs4_session *ds_session =
			ds_nc ? nfs4_session_find_for_client(ss, ds_nc) : NULL;

		if (ds_session) {
			stateid4 recall_sid;
			struct network_file_handle cb_nfh = { 0 };

			cb_nfh.nfh_ino = dir->i_ino;
			cb_nfh.nfh_sb = dir->i_sb->sb_id;

			nfs_fh4 cb_fh4 = {
				.nfs_fh4_len = sizeof(cb_nfh),
				.nfs_fh4_val = (char *)&cb_nfh,
			};

			pack_stateid4(&recall_sid, stid);
			nfs4_cb_recall(ds_session, &recall_sid, &cb_fh4, false);
			nfs4_session_put(ds_session);
		}

		/*
		 * Unhash from the inode so the next iteration doesn't
		 * find this delegation again.  The stateid remains alive
		 * (client ref) until DELEGRETURN or lease expiry.
		 */
		if (stateid_inode_unhash(stid))
			stateid_put(stid); /* drop the hash-table ref */

		stateid_put(stid); /* drop the find ref */
	}
}

uint32_t nfs4_op_want_delegation(struct compound *compound)
{
	WANT_DELEGATION4res *res =
		NFS4_OP_RES_SETUP(compound, opwant_delegation);
	nfsstat4 *status = &res->wdr_status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}
