/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <string.h>
#include <sys/stat.h>

#include "nfsv42_xdr.h"
#include "nfsv42_names.h"
#include "reffs/log.h"
#include "reffs/rpc.h"
#include "reffs/dirent.h"
#include "reffs/identity.h"
#include "nfs4/compound.h"
#include "nfs4/ops.h"
#include "nfs4/errors.h"

void nfs4_op_lookup(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	LOOKUP4args *args = NFS4_OP_ARG_SETUP(c, ph, oplookup);
	LOOKUP4res *res = NFS4_OP_RES_SETUP(c, ph, oplookup);
	nfsstat4 *status = &res->status;

	struct reffs_dirent *child_de = NULL;
	struct inode *child = NULL;
	char *name = NULL;
	int ret;

	if (network_file_handle_empty(&c->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		goto out;
	}

	if (!S_ISDIR(c->c_inode->i_mode)) {
		*status = NFS4ERR_NOTDIR;
		goto out;
	}

	if (args->objname.utf8string_len == 0) {
		*status = NFS4ERR_INVAL;
		goto out;
	}

	if (args->objname.utf8string_len > REFFS_MAX_NAME) {
		*status = NFS4ERR_NAMETOOLONG;
		goto out;
	}

	name = strndup(args->objname.utf8string_val,
		       args->objname.utf8string_len);
	if (!name) {
		*status = NFS4ERR_DELAY;
		goto out;
	}

	/* "." and ".." are not valid LOOKUP components — use LOOKUPP */
	if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
		*status = NFS4ERR_BADNAME;
		goto out;
	}

	ret = inode_access_check(c->c_inode, &c->c_ap, X_OK);
	if (ret) {
		*status = errno_to_nfs4(ret, OP_LOOKUP);
		goto out;
	}

	/*
	 * After PUTFH the inode may be loaded without its dirent chain.
	 * Reconstruct before calling dirent_load_child_by_name.
	 */
	if (!c->c_inode->i_dirent) {
		ret = inode_reconstruct_path_to_root(c->c_inode);
		if (ret) {
			*status = NFS4ERR_STALE;
			goto out;
		}
	}

	child_de = dirent_load_child_by_name(c->c_inode->i_dirent, name);
	if (!child_de) {
		*status = NFS4ERR_NOENT;
		goto out;
	}

	child = dirent_ensure_inode(child_de);
	if (!child) {
		*status = NFS4ERR_SERVERFAULT;
		goto out;
	}

	inode_active_put(c->c_inode);
	c->c_inode = child;
	c->c_curr_nfh.nfh_ino = child->i_ino;

	stateid_put(c->c_curr_stid);
	c->c_curr_stid = NULL;

	*status = NFS4_OK;

out:
	free(name);
	dirent_put(child_de);
	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_lookupp(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	LOOKUPP4res *res = NFS4_OP_RES_SETUP(c, ph, oplookupp);
	nfsstat4 *status = &res->status;

	struct reffs_dirent *parent_de = NULL;
	struct inode *parent = NULL;

	if (network_file_handle_empty(&c->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		goto out;
	}

	/* At the root there is no parent */
	if (c->c_curr_nfh.nfh_ino == INODE_ROOT_ID) {
		*status = NFS4ERR_NOENT;
		goto out;
	}

	/*
	 * inode_ensure_parent_dirent loads and links the parent dirent so
	 * that a subsequent LOOKUP on the returned directory works without
	 * needing inode_reconstruct_path_to_root.
	 */
	parent_de = inode_ensure_parent_dirent(c->c_inode);
	if (!parent_de) {
		*status = NFS4ERR_STALE;
		goto out;
	}

	parent = dirent_ensure_inode(parent_de);
	if (!parent) {
		*status = NFS4ERR_STALE;
		goto out;
	}

	inode_active_put(c->c_inode);
	c->c_inode = parent;
	c->c_curr_nfh.nfh_ino = parent->i_ino;

	stateid_put(c->c_curr_stid);
	c->c_curr_stid = NULL;

	*status = NFS4_OK;

out:
	dirent_put(parent_de);
	LOG("%s status=%s(%d) res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)res);
}

void nfs4_op_create(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	CREATE4args *args = NFS4_OP_ARG_SETUP(c, ph, opcreate);
	CREATE4res *res = NFS4_OP_RES_SETUP(c, ph, opcreate);
	nfsstat4 *status = &res->status;
	CREATE4resok *resok = NFS4_OP_RESOK_SETUP(res, CREATE4res_u, resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_remove(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	REMOVE4args *args = NFS4_OP_ARG_SETUP(c, ph, opremove);
	REMOVE4res *res = NFS4_OP_RES_SETUP(c, ph, opremove);
	nfsstat4 *status = &res->status;
	REMOVE4resok *resok = NFS4_OP_RESOK_SETUP(res, REMOVE4res_u, resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_rename(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	RENAME4args *args = NFS4_OP_ARG_SETUP(c, ph, oprename);
	RENAME4res *res = NFS4_OP_RES_SETUP(c, ph, oprename);
	nfsstat4 *status = &res->status;
	RENAME4resok *resok = NFS4_OP_RESOK_SETUP(res, RENAME4res_u, resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_link(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	LINK4args *args = NFS4_OP_ARG_SETUP(c, ph, oplink);
	LINK4res *res = NFS4_OP_RES_SETUP(c, ph, oplink);
	nfsstat4 *status = &res->status;
	LINK4resok *resok = NFS4_OP_RESOK_SETUP(res, LINK4res_u, resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_openattr(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	OPENATTR4args *args = NFS4_OP_ARG_SETUP(c, ph, opopenattr);
	OPENATTR4res *res = NFS4_OP_RES_SETUP(c, ph, opopenattr);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_readlink(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	READLINK4res *res = NFS4_OP_RES_SETUP(c, ph, opreadlink);
	nfsstat4 *status = &res->status;
	READLINK4resok *resok =
		NFS4_OP_RESOK_SETUP(res, READLINK4res_u, resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)res, (void *)resok);
}
