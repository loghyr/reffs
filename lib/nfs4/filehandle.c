/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include "nfsv42_xdr.h"
#include "nfsv42_names.h"
#include "reffs/log.h"
#include "reffs/rpc.h"
#include "reffs/memory.h"
#include "reffs/test.h"
#include "nfs4/compound.h"
#include "nfs4/ops.h"
#include "nfs4/errors.h"

void nfs4_op_getfh(struct compound *compound)
{
	GETFH4res *res = NFS4_OP_RES_SETUP(compound, opgetfh);
	nfsstat4 *status = &res->status;
	GETFH4resok *resok = NFS4_OP_RESOK_SETUP(res, GETFH4res_u, resok4);

	if (network_file_handle_empty(&compound->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		return;
	}

	resok->object.nfs_fh4_val =
		memdup(&compound->c_curr_nfh, sizeof(compound->c_curr_nfh));
	if (!resok->object.nfs_fh4_val) {
		*status = NFS4ERR_DELAY; // Yes, not valid, but a missing error!
		return;
	}
	resok->object.nfs_fh4_len = sizeof(compound->c_curr_nfh);
}

void nfs4_op_putfh(struct compound *compound)
{
	PUTFH4args *args = NFS4_OP_ARG_SETUP(compound, opputfh);
	PUTFH4res *res = NFS4_OP_RES_SETUP(compound, opputfh);
	nfsstat4 *status = &res->status;

	struct network_file_handle *nfh =
		(struct network_file_handle *)args->object.nfs_fh4_val;

	if (args->object.nfs_fh4_len != sizeof(compound->c_curr_nfh)) {
		*status = NFS4ERR_BADHANDLE;
		return;
	}

	if (network_file_handle_empty(nfh)) {
		*status = NFS4ERR_BADHANDLE;
		return;
	}

	if (nfh->nfh_sb == 0 || nfh->nfh_ino == 0) {
		*status = NFS4ERR_BADHANDLE;
		return;
	}

	if (nfh->nfh_sb != compound->c_curr_nfh.nfh_sb) {
		super_block_put(compound->c_curr_sb);
		compound->c_curr_sb = super_block_find(nfh->nfh_sb);
		if (!compound->c_curr_sb) {
			*status = NFS4ERR_STALE;
			return;
		}

		inode_active_put(compound->c_inode);
		compound->c_inode = NULL;
	} else if (nfh->nfh_ino != compound->c_curr_nfh.nfh_ino) {
		inode_active_put(compound->c_inode);
		compound->c_inode = NULL;
	}

	compound->c_curr_nfh.nfh_sb = nfh->nfh_sb;
	compound->c_curr_nfh.nfh_ino = nfh->nfh_ino;

	if (!compound->c_inode) {
		compound->c_inode = inode_find(compound->c_curr_sb,
					       compound->c_curr_nfh.nfh_ino);
		if (!compound->c_inode) {
			*status = NFS4ERR_STALE;
			return;
		}
	}

	stateid_put(compound->c_curr_stid);
	compound->c_curr_stid = NULL;
}

void nfs4_op_putpubfh(struct compound *compound)
{
	PUTPUBFH4res *res = NFS4_OP_RES_SETUP(compound, opputpubfh);
	nfsstat4 *status = &res->status;

	if (compound->c_curr_nfh.nfh_sb != SUPER_BLOCK_ROOT_ID) {
		super_block_put(compound->c_curr_sb);
		compound->c_curr_sb = super_block_find(SUPER_BLOCK_ROOT_ID);
		if (!compound->c_curr_sb) {
			*status = NFS4ERR_SERVERFAULT;
			return;
		}
		inode_active_put(compound->c_inode);
		compound->c_inode = NULL;
	} else if (compound->c_curr_nfh.nfh_ino != INODE_ROOT_ID) {
		inode_active_put(compound->c_inode);
		compound->c_inode = NULL;
	}

	compound->c_curr_nfh.nfh_sb = SUPER_BLOCK_ROOT_ID;
	compound->c_curr_nfh.nfh_ino = INODE_ROOT_ID;

	stateid_put(compound->c_curr_stid);
	compound->c_curr_stid = NULL;
}

void nfs4_op_putrootfh(struct compound *compound)
{
	PUTROOTFH4res *res = NFS4_OP_RES_SETUP(compound, opputrootfh);
	nfsstat4 *status = &res->status;

	if (compound->c_curr_nfh.nfh_sb != SUPER_BLOCK_ROOT_ID) {
		super_block_put(compound->c_curr_sb);
		compound->c_curr_sb = super_block_find(SUPER_BLOCK_ROOT_ID);
		if (!compound->c_curr_sb) {
			*status = NFS4ERR_SERVERFAULT;
			return;
		}
		inode_active_put(compound->c_inode);
		compound->c_inode = NULL;
	} else if (compound->c_curr_nfh.nfh_ino != INODE_ROOT_ID) {
		inode_active_put(compound->c_inode);
		compound->c_inode = NULL;
	}

	compound->c_curr_nfh.nfh_sb = SUPER_BLOCK_ROOT_ID;
	compound->c_curr_nfh.nfh_ino = INODE_ROOT_ID;

	if (!compound->c_inode) {
		compound->c_inode = inode_find(compound->c_curr_sb,
					       compound->c_curr_nfh.nfh_ino);
		if (!compound->c_inode) {
			*status = NFS4ERR_STALE;
			return;
		}
	}

	stateid_put(compound->c_curr_stid);
	compound->c_curr_stid = NULL;
}

void nfs4_op_restorefh(struct compound *compound)
{
	RESTOREFH4res *res = NFS4_OP_RES_SETUP(compound, oprestorefh);
	nfsstat4 *status = &res->status;

	if (network_file_handle_empty(&compound->c_saved_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		return;
	}

	super_block_put(compound->c_curr_sb);
	compound->c_curr_sb = super_block_get(compound->c_saved_sb);
	compound->c_curr_nfh = compound->c_saved_nfh;
	inode_active_put(compound->c_inode);
	compound->c_inode = NULL;

	if (!compound->c_inode) {
		compound->c_inode = inode_find(compound->c_curr_sb,
					       compound->c_curr_nfh.nfh_ino);
		if (!compound->c_inode) {
			*status = NFS4ERR_STALE;
			return;
		}
	}

	stateid_put(compound->c_curr_stid);
	compound->c_curr_stid = NULL;
	if (compound->c_saved_stid) {
		compound->c_curr_stid = stateid_get(compound->c_saved_stid);
		verify_msg(compound->c_curr_stid,
			   "Could not get loaded stateid");
	}
}

void nfs4_op_savefh(struct compound *compound)
{
	SAVEFH4res *res = NFS4_OP_RES_SETUP(compound, opsavefh);
	nfsstat4 *status = &res->status;

	if (network_file_handle_empty(&compound->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		return;
	}

	super_block_put(compound->c_saved_sb);
	compound->c_saved_sb = super_block_get(compound->c_curr_sb);
	if (!compound->c_saved_sb) {
		*status = NFS4ERR_DELAY;
		return;
	}

	compound->c_saved_nfh = compound->c_curr_nfh;

	stateid_put(compound->c_saved_stid);
	compound->c_saved_stid = NULL;
	if (compound->c_curr_stid) {
		compound->c_saved_stid = stateid_get(compound->c_curr_stid);
		verify_msg(compound->c_saved_stid,
			   "Could not get loaded stateid");
	}
}
