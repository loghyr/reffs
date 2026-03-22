/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>

#include "nfsv42_xdr.h"
#include "nfsv42_names.h"
#include "reffs/log.h"
#include "reffs/rpc.h"
#include "reffs/dirent.h"
#include "reffs/identity.h"
#include "reffs/vfs.h"
#include "nfs4/compound.h"
#include "nfs4/ops.h"
#include "nfs4/errors.h"
#include "reffs/time.h"
#include "nfs4/trace/nfs4.h"

static inline changeid4 timespec_to_changeid(const struct timespec *ts)
{
	return (changeid4)timespec_to_ns(ts);
}

uint32_t nfs4_op_lookup(struct compound *compound)
{
	LOOKUP4args *args = NFS4_OP_ARG_SETUP(compound, oplookup);
	LOOKUP4res *res = NFS4_OP_RES_SETUP(compound, oplookup);
	nfsstat4 *status = &res->status;

	struct reffs_dirent *child_de = NULL;
	struct inode *child = NULL;
	char *name = NULL;
	int ret;

	if (network_file_handle_empty(&compound->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		goto out;
	}

	if (!S_ISDIR(compound->c_inode->i_mode)) {
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

	ret = inode_access_check(compound->c_inode, &compound->c_ap, X_OK);
	if (ret) {
		*status = errno_to_nfs4(ret, OP_LOOKUP);
		goto out;
	}

	/*
	 * After PUTFH the inode may be loaded without its dirent chain.
	 * Reconstruct before calling dirent_load_child_by_name.
	 */
	if (!compound->c_inode->i_dirent) {
		ret = inode_reconstruct_path_to_root(compound->c_inode);
		if (ret) {
			*status = NFS4ERR_STALE;
			goto out;
		}
	}

	child_de = dirent_load_child_by_name(compound->c_inode->i_dirent, name);
	if (!child_de) {
		*status = NFS4ERR_NOENT;
		goto out;
	}

	child = dirent_ensure_inode(child_de);
	if (!child) {
		*status = NFS4ERR_SERVERFAULT;
		goto out;
	}

	inode_active_put(compound->c_inode);
	compound->c_inode = child;
	compound->c_curr_nfh.nfh_ino = child->i_ino;

	stateid_put(compound->c_curr_stid);
	compound->c_curr_stid = NULL;

out:
	trace_nfs4_name(compound, name, __func__, __LINE__);
	free(name);
	dirent_put(child_de);

	return 0;
}

uint32_t nfs4_op_lookupp(struct compound *compound)
{
	LOOKUPP4res *res = NFS4_OP_RES_SETUP(compound, oplookupp);
	nfsstat4 *status = &res->status;

	struct reffs_dirent *parent_de = NULL;
	struct inode *parent = NULL;

	if (network_file_handle_empty(&compound->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		goto out;
	}

	/* At the root there is no parent */
	if (compound->c_curr_nfh.nfh_ino == INODE_ROOT_ID) {
		*status = NFS4ERR_NOENT;
		goto out;
	}

	/*
	 * inode_ensure_parent_dirent loads and links the parent dirent so
	 * that a subsequent LOOKUP on the returned directory works without
	 * needing inode_reconstruct_path_to_root.
	 */
	parent_de = inode_ensure_parent_dirent(compound->c_inode);
	if (!parent_de) {
		*status = NFS4ERR_STALE;
		goto out;
	}

	parent = dirent_ensure_inode(parent_de);
	if (!parent) {
		*status = NFS4ERR_STALE;
		goto out;
	}

	inode_active_put(compound->c_inode);
	compound->c_inode = parent;
	compound->c_curr_nfh.nfh_ino = parent->i_ino;

	stateid_put(compound->c_curr_stid);
	compound->c_curr_stid = NULL;

out:
	dirent_put(parent_de);

	return 0;
}

uint32_t nfs4_op_create(struct compound *compound)
{
	CREATE4args *args = NFS4_OP_ARG_SETUP(compound, opcreate);
	CREATE4res *res = NFS4_OP_RES_SETUP(compound, opcreate);
	nfsstat4 *status = &res->status;
	CREATE4resok *resok = NFS4_OP_RESOK_SETUP(res, CREATE4res_u, resok4);

	struct inode *new_inode = NULL;
	struct timespec dir_before, dir_after;
	char *name = NULL;
	char *linkpath = NULL;
	int ret;

	if (network_file_handle_empty(&compound->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		goto out;
	}

	if (!S_ISDIR(compound->c_inode->i_mode)) {
		*status = NFS4ERR_NOTDIR;
		goto out;
	}

	/* NF4REG is not valid for CREATE — clients must use OPEN. */
	if (args->objtype.type == NF4REG) {
		*status = NFS4ERR_INVAL;
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
	if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
		*status = NFS4ERR_BADNAME;
		goto out;
	}

	switch (args->objtype.type) {
	case NF4DIR:
		ret = vfs_mkdir(compound->c_inode, name, 0777, &compound->c_ap,
				&new_inode, &dir_before, &dir_after);
		break;

	case NF4LNK: {
		linktext4 *ld = &args->objtype.createtype4_u.linkdata;
		linkpath = strndup(ld->linktext4_val, ld->linktext4_len);
		if (!linkpath) {
			*status = NFS4ERR_DELAY;
			goto out;
		}
		ret = vfs_symlink(compound->c_inode, name, linkpath,
				  &compound->c_ap, &new_inode, &dir_before,
				  &dir_after);
		break;
	}

	case NF4BLK: {
		specdata4 *sd = &args->objtype.createtype4_u.devdata;
		ret = vfs_mknod(compound->c_inode, name, S_IFBLK | 0666,
				makedev(sd->specdata1, sd->specdata2),
				&compound->c_ap, &new_inode, &dir_before,
				&dir_after);
		break;
	}

	case NF4CHR: {
		specdata4 *sd = &args->objtype.createtype4_u.devdata;
		ret = vfs_mknod(compound->c_inode, name, S_IFCHR | 0666,
				makedev(sd->specdata1, sd->specdata2),
				&compound->c_ap, &new_inode, &dir_before,
				&dir_after);
		break;
	}

	case NF4SOCK:
		ret = vfs_mknod(compound->c_inode, name, S_IFSOCK | 0666, 0,
				&compound->c_ap, &new_inode, &dir_before,
				&dir_after);
		break;

	case NF4FIFO:
		ret = vfs_mknod(compound->c_inode, name, S_IFIFO | 0666, 0,
				&compound->c_ap, &new_inode, &dir_before,
				&dir_after);
		break;

	default:
		*status = NFS4ERR_BADTYPE;
		goto out;
	}

	if (ret) {
		*status = ret == -EEXIST ? NFS4ERR_EXIST :
			  ret == -ENOSPC ? NFS4ERR_NOSPC :
					   errno_to_nfs4(ret, OP_CREATE);
		goto out;
	}

	/* Switch current FH to the newly created object. */
	inode_active_put(compound->c_inode);
	compound->c_inode = new_inode;
	new_inode = NULL;
	compound->c_curr_nfh.nfh_ino = compound->c_inode->i_ino;

	resok->cinfo.atomic = TRUE;
	resok->cinfo.before = timespec_to_changeid(&dir_before);
	resok->cinfo.after = timespec_to_changeid(&dir_after);
	resok->attrset.bitmap4_len = 0;
	resok->attrset.bitmap4_val = NULL;

out:
	trace_nfs4_name(compound, name, __func__, __LINE__);
	inode_active_put(new_inode);
	free(linkpath);
	free(name);

	return 0;
}

uint32_t nfs4_op_remove(struct compound *compound)
{
	REMOVE4args *args = NFS4_OP_ARG_SETUP(compound, opremove);
	REMOVE4res *res = NFS4_OP_RES_SETUP(compound, opremove);
	nfsstat4 *status = &res->status;
	REMOVE4resok *resok = NFS4_OP_RESOK_SETUP(res, REMOVE4res_u, resok4);

	struct timespec dir_before, dir_after;
	char *name = NULL;
	int ret;

	if (network_file_handle_empty(&compound->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		goto out;
	}

	if (!S_ISDIR(compound->c_inode->i_mode)) {
		*status = NFS4ERR_NOTDIR;
		goto out;
	}

	if (args->target.utf8string_len == 0) {
		*status = NFS4ERR_INVAL;
		goto out;
	}
	if (args->target.utf8string_len > REFFS_MAX_NAME) {
		*status = NFS4ERR_NAMETOOLONG;
		goto out;
	}
	name = strndup(args->target.utf8string_val,
		       args->target.utf8string_len);
	if (!name) {
		*status = NFS4ERR_DELAY;
		goto out;
	}
	if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
		*status = NFS4ERR_BADNAME;
		goto out;
	}

	/*
	 * Try removing as a non-directory first.  vfs_remove() returns
	 * -EISDIR if the target is a directory; fall back to vfs_rmdir().
	 */
	ret = vfs_remove(compound->c_inode, name, &compound->c_ap, &dir_before,
			 &dir_after);
	if (ret == -EISDIR)
		ret = vfs_rmdir(compound->c_inode, name, &compound->c_ap,
				&dir_before, &dir_after);

	if (ret) {
		*status = errno_to_nfs4(ret, OP_REMOVE);
		goto out;
	}

	resok->cinfo.atomic = TRUE;
	resok->cinfo.before = timespec_to_changeid(&dir_before);
	resok->cinfo.after = timespec_to_changeid(&dir_after);

out:
	trace_nfs4_name(compound, name, __func__, __LINE__);
	free(name);

	return 0;
}

uint32_t nfs4_op_rename(struct compound *compound)
{
	RENAME4args *args = NFS4_OP_ARG_SETUP(compound, oprename);
	RENAME4res *res = NFS4_OP_RES_SETUP(compound, oprename);
	nfsstat4 *status = &res->status;
	RENAME4resok *resok = NFS4_OP_RESOK_SETUP(res, RENAME4res_u, resok4);

	struct inode *old_dir = NULL;
	struct timespec old_before, old_after, new_before, new_after;
	char *oldname = NULL;
	char *newname = NULL;
	int ret;

	if (network_file_handle_empty(&compound->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		goto out;
	}

	if (network_file_handle_empty(&compound->c_saved_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		goto out;
	}

	if (!S_ISDIR(compound->c_inode->i_mode)) {
		*status = NFS4ERR_NOTDIR;
		goto out;
	}

	/* Validate names. */
	if (args->oldname.utf8string_len == 0 ||
	    args->newname.utf8string_len == 0) {
		*status = NFS4ERR_INVAL;
		goto out;
	}
	if (args->oldname.utf8string_len > REFFS_MAX_NAME ||
	    args->newname.utf8string_len > REFFS_MAX_NAME) {
		*status = NFS4ERR_NAMETOOLONG;
		goto out;
	}
	oldname = strndup(args->oldname.utf8string_val,
			  args->oldname.utf8string_len);
	newname = strndup(args->newname.utf8string_val,
			  args->newname.utf8string_len);
	if (!oldname || !newname) {
		*status = NFS4ERR_DELAY;
		goto out;
	}
	if (strcmp(oldname, ".") == 0 || strcmp(oldname, "..") == 0 ||
	    strcmp(newname, ".") == 0 || strcmp(newname, "..") == 0) {
		*status = NFS4ERR_BADNAME;
		goto out;
	}

	/*
	 * RFC 5661 §18.26: source is SAVED_FH, destination is CURRENT_FH.
	 * Load the saved directory inode.
	 */
	old_dir =
		inode_find(compound->c_saved_sb, compound->c_saved_nfh.nfh_ino);
	if (!old_dir) {
		*status = NFS4ERR_STALE;
		goto out;
	}

	if (!S_ISDIR(old_dir->i_mode)) {
		*status = NFS4ERR_NOTDIR;
		goto out;
	}

	ret = vfs_rename(old_dir, oldname, compound->c_inode, newname,
			 &compound->c_ap, &old_before, &old_after, &new_before,
			 &new_after);
	if (ret) {
		*status = errno_to_nfs4(ret, OP_RENAME);
		goto out;
	}

	resok->source_cinfo.atomic = TRUE;
	resok->source_cinfo.before = timespec_to_changeid(&old_before);
	resok->source_cinfo.after = timespec_to_changeid(&old_after);
	resok->target_cinfo.atomic = TRUE;
	resok->target_cinfo.before = timespec_to_changeid(&new_before);
	resok->target_cinfo.after = timespec_to_changeid(&new_after);

	*status = NFS4_OK;

out:
	inode_active_put(old_dir);
	free(oldname);
	free(newname);

	return 0;
}

uint32_t nfs4_op_link(struct compound *compound)
{
	LINK4args *args = NFS4_OP_ARG_SETUP(compound, oplink);
	LINK4res *res = NFS4_OP_RES_SETUP(compound, oplink);
	nfsstat4 *status = &res->status;
	LINK4resok *resok = NFS4_OP_RESOK_SETUP(res, LINK4res_u, resok4);

	struct inode *src_inode = NULL;
	struct timespec dir_before, dir_after;
	char *name = NULL;
	int ret;

	/*
	 * RFC 5661 §18.9.3: CURRENT_FH is the target directory;
	 * SAVED_FH is the file to link.
	 */
	if (network_file_handle_empty(&compound->c_curr_nfh) ||
	    network_file_handle_empty(&compound->c_saved_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		goto out;
	}

	if (!S_ISDIR(compound->c_inode->i_mode)) {
		*status = NFS4ERR_NOTDIR;
		goto out;
	}

	if (args->newname.utf8string_len == 0) {
		*status = NFS4ERR_INVAL;
		goto out;
	}
	if (args->newname.utf8string_len > REFFS_MAX_NAME) {
		*status = NFS4ERR_NAMETOOLONG;
		goto out;
	}

	name = strndup(args->newname.utf8string_val,
		       args->newname.utf8string_len);
	if (!name) {
		*status = NFS4ERR_DELAY;
		goto out;
	}
	if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
		*status = NFS4ERR_BADNAME;
		goto out;
	}

	src_inode =
		inode_find(compound->c_saved_sb, compound->c_saved_nfh.nfh_ino);
	if (!src_inode) {
		*status = NFS4ERR_STALE;
		goto out;
	}

	if (S_ISDIR(src_inode->i_mode)) {
		*status = NFS4ERR_ISDIR;
		goto out;
	}

	pthread_mutex_lock(&compound->c_inode->i_attr_mutex);
	dir_before = compound->c_inode->i_mtime;
	pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);

	ret = vfs_link(src_inode, compound->c_inode, name, &compound->c_ap);
	if (ret) {
		*status = errno_to_nfs4(ret, OP_LINK);
		goto out;
	}

	pthread_mutex_lock(&compound->c_inode->i_attr_mutex);
	dir_after = compound->c_inode->i_mtime;
	pthread_mutex_unlock(&compound->c_inode->i_attr_mutex);

	resok->cinfo.atomic = TRUE;
	resok->cinfo.before = timespec_to_changeid(&dir_before);
	resok->cinfo.after = timespec_to_changeid(&dir_after);

out:
	inode_active_put(src_inode);
	trace_nfs4_name(compound, name, __func__, __LINE__);
	free(name);

	return 0;
}

uint32_t nfs4_op_openattr(struct compound *compound)
{
	OPENATTR4res *res = NFS4_OP_RES_SETUP(compound, opopenattr);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}

uint32_t nfs4_op_readlink(struct compound *compound)
{
	READLINK4res *res = NFS4_OP_RES_SETUP(compound, opreadlink);
	nfsstat4 *status = &res->status;
	READLINK4resok *resok =
		NFS4_OP_RESOK_SETUP(res, READLINK4res_u, resok4);

	if (network_file_handle_empty(&compound->c_curr_nfh)) {
		*status = NFS4ERR_NOFILEHANDLE;
		return 0;
	}

	if (!S_ISLNK(compound->c_inode->i_mode)) {
		*status = NFS4ERR_INVAL;
		return 0;
	}

	if (!compound->c_inode->i_symlink) {
		*status = NFS4ERR_SERVERFAULT;
		return 0;
	}

	resok->link.linktext4_val = strdup(compound->c_inode->i_symlink);
	if (!resok->link.linktext4_val) {
		*status = NFS4ERR_DELAY;
		return 0;
	}
	resok->link.linktext4_len = strlen(compound->c_inode->i_symlink);

	return 0;
}
