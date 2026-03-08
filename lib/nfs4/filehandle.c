/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include "nfsv42_xdr.h"
#include "reffs/log.h"
#include "reffs/rpc.h"
#include "reffs/memory.h"
#include "compound.h"
#include "ops.h"
#include "errors.h"

void nfs4_op_getfh(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	GETFH4res *res = NFS4_OP_RES_SETUP(c, ph, opgetfh);
	nfsstat4 *status = &res->status;
	GETFH4resok *resok = NFS4_OP_RESOK_SETUP(res, GETFH4res_u, resok4);

	if (network_file_handle_empty(&c->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		goto out;
	}

	resok->object.nfs_fh4_val =
		memdup(&c->c_curr_nfh, sizeof(c->c_curr_nfh));
	if (!resok->object.nfs_fh4_val) {
		*status = NFS4ERR_DELAY; // Yes, not valid, but a missing error!
		goto out;
	}
	resok->object.nfs_fh4_len = sizeof(c->c_curr_nfh);

out:
	LOG("%s status=%s(%d) res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)res, (void *)resok);
}

void nfs4_op_putfh(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	PUTFH4args *args = NFS4_OP_ARG_SETUP(c, ph, opputfh);
	PUTFH4res *res = NFS4_OP_RES_SETUP(c, ph, opputfh);
	nfsstat4 *status = &res->status;

	struct network_file_handle *nfh =
		(struct network_file_handle *)args->object.nfs_fh4_val;

	if (args->object.nfs_fh4_len != sizeof(c->c_curr_nfh)) {
		*status = NFS4ERR_BADHANDLE;
		goto out;
	}

	if (network_file_handle_empty(nfh)) {
		*status = NFS4ERR_BADHANDLE;
		goto out;
	}

	if (nfh->nfh_sb != c->c_curr_nfh.nfh_sb) {
		super_block_put(c->c_curr_sb);
		c->c_curr_sb = super_block_find(nfh->nfh_sb);
		if (!c->c_curr_sb) {
			*status = NFS4ERR_STALE;
			goto out;
		}

		inode_active_put(c->c_inode);
		c->c_inode = NULL;
	} else if (nfh->nfh_ino != c->c_curr_nfh.nfh_ino) {
		inode_active_put(c->c_inode);
		c->c_inode = NULL;
	}

	c->c_curr_nfh.nfh_sb = nfh->nfh_sb;
	c->c_curr_nfh.nfh_ino = nfh->nfh_ino;

	if (!c->c_inode) {
		/*
		 * Inode may not be loaded in the sb,
		 * so wait until needed to load it.
		 */
		c->c_inode = inode_find(c->c_curr_sb, c->c_curr_nfh.nfh_ino);
	}

out:
	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_putpubfh(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	PUTPUBFH4res *res = NFS4_OP_RES_SETUP(c, ph, opputpubfh);
	nfsstat4 *status = &res->status;

	if (c->c_curr_nfh.nfh_sb != SUPER_BLOCK_ROOT_ID) {
		super_block_put(c->c_curr_sb);
		c->c_curr_sb = super_block_find(SUPER_BLOCK_ROOT_ID);
		if (!c->c_curr_sb) {
			*status = NFS4ERR_SERVERFAULT;
			goto out;
		}
		inode_active_put(c->c_inode);
		c->c_inode = NULL;
	} else if (c->c_curr_nfh.nfh_ino != INODE_ROOT_ID) {
		inode_active_put(c->c_inode);
		c->c_inode = NULL;
	}

	c->c_curr_nfh.nfh_sb = SUPER_BLOCK_ROOT_ID;
	c->c_curr_nfh.nfh_ino = INODE_ROOT_ID;

out:
	LOG("%s status=%s(%d) res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)res);
}

void nfs4_op_putrootfh(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	PUTROOTFH4res *res = NFS4_OP_RES_SETUP(c, ph, opputrootfh);
	nfsstat4 *status = &res->status;

	if (c->c_curr_nfh.nfh_sb != SUPER_BLOCK_ROOT_ID) {
		super_block_put(c->c_curr_sb);
		c->c_curr_sb = super_block_find(SUPER_BLOCK_ROOT_ID);
		if (!c->c_curr_sb) {
			*status = NFS4ERR_SERVERFAULT;
			goto out;
		}
		inode_active_put(c->c_inode);
		c->c_inode = NULL;
	} else if (c->c_curr_nfh.nfh_ino != INODE_ROOT_ID) {
		inode_active_put(c->c_inode);
		c->c_inode = NULL;
	}

	c->c_curr_nfh.nfh_sb = SUPER_BLOCK_ROOT_ID;
	c->c_curr_nfh.nfh_ino = INODE_ROOT_ID;

	if (!c->c_inode) {
		/*
		 * Inode may not be loaded in the sb,
		 * so wait until needed to load it.
		 */
		c->c_inode = inode_find(c->c_curr_sb, c->c_curr_nfh.nfh_ino);
	}

out:
	LOG("%s status=%s(%d) res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)res);
}

void nfs4_op_restorefh(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	RESTOREFH4res *res = NFS4_OP_RES_SETUP(c, ph, oprestorefh);
	nfsstat4 *status = &res->status;

	if (network_file_handle_empty(&c->c_saved_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		goto out;
	}

	super_block_put(c->c_curr_sb);
	c->c_curr_sb = c->c_saved_sb;
	c->c_saved_sb = NULL;
	c->c_curr_nfh = c->c_saved_nfh;
	memset(&c->c_saved_nfh, '\0', sizeof(c->c_saved_nfh));
	inode_active_put(c->c_inode);
	c->c_inode = NULL;

	if (!c->c_inode) {
		/*
		 * Inode may not be loaded in the sb,
		 * so wait until needed to load it.
		 */
		c->c_inode = inode_find(c->c_curr_sb, c->c_curr_nfh.nfh_ino);
	}

out:
	LOG("%s status=%s(%d) res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)res);
}

void nfs4_op_savefh(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	SAVEFH4res *res = NFS4_OP_RES_SETUP(c, ph, opsavefh);
	nfsstat4 *status = &res->status;

	if (network_file_handle_empty(&c->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		goto out;
	}

	super_block_put(c->c_saved_sb);
	c->c_saved_sb = super_block_get(c->c_curr_sb);
	if (!c->c_saved_sb) {
		*status = NFS4ERR_DELAY;
		goto out;
	}

	c->c_saved_nfh = c->c_curr_nfh;

out:
	LOG("%s status=%s(%d) res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)res);
}
