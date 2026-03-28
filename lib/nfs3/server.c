/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include <rpc/xdr.h>
#include <rpc/auth.h>
#include <rpc/auth_unix.h>
#include <zlib.h>
#include "nfsv3_xdr.h"
#include "reffs/rcu.h"
#include "reffs/rpc.h"
#include "reffs/cmp.h"
#include "reffs/log.h"
#include "reffs/filehandle.h"
#include "reffs/test.h"
#include "reffs/time.h"
#include "reffs/inode.h"
#include "reffs/super_block.h"
#include "reffs/data_block.h"
#include "reffs/io.h"
#include "reffs/server.h"
#include "reffs/vfs.h"
#include "reffs/identity.h"
#include "reffs/errno.h"
#include "reffs/trace/nfs3_server.h"

/*
 * On locking order:
 *
 * 1) Always take the d_rwlock of the dirent after
 * the i_attr_mutex of the inode.
 *
 * 2) Always take the i_db_rwlock of the inode after the
 * i_attr_mutex of the inode.
 */

static void inode_attr_to_fattr(struct inode *inode, fattr3 *fa);
static nfsstat3 errno_to_nfs3(int err);

static void inode_attr_to_fattr(struct inode *inode, fattr3 *fa)
{
	uint16_t type = inode->i_mode & S_IFMT;

	switch (type) {
	case S_IFLNK:
		fa->type = NF3LNK;
		break;
	case S_IFREG:
		fa->type = NF3REG;
		break;
	case S_IFDIR:
		fa->type = NF3DIR;
		break;
	case S_IFCHR:
		fa->type = NF3CHR;
		break;
	case S_IFBLK:
		fa->type = NF3BLK;
		break;
	case S_IFIFO:
		fa->type = NF3FIFO;
		break;
	case S_IFSOCK:
		fa->type = NF3SOCK;
		break;
	}

	fa->mode = inode->i_mode;
	fa->nlink = __atomic_load_n(&inode->i_nlink, __ATOMIC_RELAXED);
	fa->uid = reffs_id_to_uid(inode->i_uid);
	fa->gid = reffs_id_to_uid(inode->i_gid);
	fa->size = inode->i_size;
	fa->used = inode->i_used * inode->i_sb->sb_block_size;
	fa->rdev.specdata1 = inode->i_dev_major;
	fa->rdev.specdata2 = inode->i_dev_minor;
	fa->fsid = inode->i_sb->sb_id;
	fa->fileid = inode->i_ino;
	timespec_to_nfstime3(&inode->i_atime, &fa->atime);
	timespec_to_nfstime3(&inode->i_mtime, &fa->mtime);
	timespec_to_nfstime3(&inode->i_ctime, &fa->ctime);
}

static int directory_inode_find(struct super_block *sb, uint64_t ino,
				struct authunix_parms *ap, int mode,
				struct inode **inode_out)
{
	struct inode *inode;
	int ret = 0;

	*inode_out = NULL;
	inode = inode_find(sb, ino);
	if (!inode) {
		return -ENOENT;
	}

	if (!(S_ISDIR(inode->i_mode))) {
		inode_active_put(inode);
		return -ENOTDIR;
	}

	ret = inode_access_check(inode, ap, X_OK);
	if (ret) {
		inode_active_put(inode);
		return ret;
	}

	ret = inode_access_check(inode, ap, mode);
	if (ret) {
		inode_active_put(inode);
		return ret;
	}

	*inode_out = inode;
	return 0;
}

static int nfs3_op_null(struct rpc_trans *rt)
{
	trace_nfs3_srv_null(rt);
	return 0;
}

static int nfs3_op_getattr(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	struct super_block *sb = NULL;
	struct inode *inode = NULL;

	GETATTR3args *args = ph->ph_args;
	GETATTR3res *res = ph->ph_res;
	fattr3 *fa = &res->GETATTR3res_u.resok.obj_attributes;

	struct network_file_handle *nfh = NULL;
	struct authunix_parms ap;
	int ret = 0;

	trace_nfs3_srv_getattr(rt, args);

	ret = rpc_cred_to_authunix_parms(&rt->rt_info, &ap);
	if (ret)
		goto out;

	if (args->object.data.data_len != sizeof(*nfh)) {
		ret = -EBADHANDLE;
		goto out;
	}

	nfh = (struct network_file_handle *)args->object.data.data_val;

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		ret = -ESTALE;
		goto out;
	}

	inode = inode_find(sb, nfh->nfh_ino);
	if (!inode) {
		ret = -ENOENT;
		goto out;
	}

	pthread_mutex_lock(&inode->i_attr_mutex);
	inode_attr_to_fattr(inode, fa);
	pthread_mutex_unlock(&inode->i_attr_mutex);

out:
	res->status = errno_to_nfs3(ret);
	inode_active_put(inode);
	super_block_put(sb);
	return ret;
}

static void nfs3_sattr3_to_reffs_sattr(sattr3 *sa, struct reffs_sattr *rs)
{
	memset(rs, 0, sizeof(*rs));
	if (sa->mode.set_it) {
		rs->mode = sa->mode.set_mode3_u.mode;
		rs->mode_set = true;
	}
	if (sa->uid.set_it) {
		rs->uid = sa->uid.set_uid3_u.uid;
		rs->uid_set = true;
	}
	if (sa->gid.set_it) {
		rs->gid = sa->gid.set_gid3_u.gid;
		rs->gid_set = true;
	}
	if (sa->size.set_it) {
		rs->size = sa->size.set_size3_u.size;
		rs->size_set = true;
	}
	if (sa->atime.set_it == SET_TO_CLIENT_TIME) {
		nfstime3_to_timespec(&sa->atime.set_atime_u.atime, &rs->atime);
		rs->atime_set = true;
	} else if (sa->atime.set_it == SET_TO_SERVER_TIME) {
		rs->atime_set = true;
		rs->atime_now = true;
	}
	if (sa->mtime.set_it == SET_TO_CLIENT_TIME) {
		nfstime3_to_timespec(&sa->mtime.set_mtime_u.mtime, &rs->mtime);
		rs->mtime_set = true;
	} else if (sa->mtime.set_it == SET_TO_SERVER_TIME) {
		rs->mtime_set = true;
		rs->mtime_now = true;
	}
}

static int nfs3_op_setattr(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	struct super_block *sb = NULL;
	struct inode *inode = NULL;

	SETATTR3args *args = ph->ph_args;
	SETATTR3res *res = ph->ph_res;
	wcc_data *wcc = &res->SETATTR3res_u.resok.obj_wcc;
	sattr3 *sa = &args->new_attributes;

	struct network_file_handle *nfh = NULL;

	size3 size;
	nfstime3 mtime;
	nfstime3 ctime;

	struct authunix_parms ap;
	int ret = 0;

	trace_nfs3_srv_setattr(rt, args);

	ret = rpc_cred_to_authunix_parms(&rt->rt_info, &ap);
	if (ret)
		goto out;

	if (args->object.data.data_len != sizeof(*nfh)) {
		ret = -EBADHANDLE;
		goto out;
	}

	nfh = (struct network_file_handle *)args->object.data.data_val;

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		ret = -ESTALE;
		goto out;
	}

	inode = inode_find(sb, nfh->nfh_ino);
	if (!inode) {
		ret = -ENOENT;
		goto out;
	}

	pthread_mutex_lock(&inode->i_attr_mutex);

	size = inode->i_size;
	timespec_to_nfstime3(&inode->i_ctime, &ctime);
	timespec_to_nfstime3(&inode->i_mtime, &mtime);

	if (args->guard.check) {
		if (!nfstime3_is_timespec(&args->guard.sattrguard3_u.obj_ctime,
					  &inode->i_ctime)) {
			ret = -ENOTSYNC;
			pthread_mutex_unlock(&inode->i_attr_mutex);
			goto update_wcc_nolock;
		}
	}
	pthread_mutex_unlock(&inode->i_attr_mutex);

	struct reffs_sattr rs;
	nfs3_sattr3_to_reffs_sattr(sa, &rs);

	ret = vfs_setattr(inode, &rs, &ap);

update_wcc_nolock:
	if (ret)
		wcc = &res->SETATTR3res_u.resfail.obj_wcc;

	wcc->before.attributes_follow = true;
	wcc->before.pre_op_attr_u.attributes.size = size;
	wcc->before.pre_op_attr_u.attributes.mtime = mtime;
	wcc->before.pre_op_attr_u.attributes.ctime = ctime;

	wcc->after.attributes_follow = true;
	pthread_mutex_lock(&inode->i_attr_mutex);
	inode_attr_to_fattr(inode, &wcc->after.post_op_attr_u.attributes);
	pthread_mutex_unlock(&inode->i_attr_mutex);

out:
	res->status = errno_to_nfs3(ret);
	inode_active_put(inode);
	super_block_put(sb);
	return ret;
}

static int nfs3_op_lookup(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	struct super_block *sb = NULL;
	struct inode *inode = NULL;
	struct inode *exists = NULL;

	LOOKUP3args *args = ph->ph_args;
	LOOKUP3res *res = ph->ph_res;
	LOOKUP3resok *resok = &res->LOOKUP3res_u.resok;

	post_op_attr *poa = &resok->dir_attributes;
	fattr3 *fa;

	struct network_file_handle *nfh = NULL;

	struct authunix_parms ap;
	int ret = 0;

	trace_nfs3_srv_lookup(rt, args);

	ret = rpc_cred_to_authunix_parms(&rt->rt_info, &ap);
	if (ret)
		goto out;

	if (args->what.dir.data.data_len != sizeof(*nfh)) {
		ret = -EBADHANDLE;
		goto out;
	}

	nfh = (struct network_file_handle *)args->what.dir.data.data_val;

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		ret = -ESTALE;
		goto out;
	}

	ret = directory_inode_find(sb, nfh->nfh_ino, &ap, X_OK, &inode);
	if (ret)
		goto out;

	pthread_mutex_lock(&inode->i_attr_mutex);

	exists = inode_name_get_inode(inode, args->what.name);

	/* Get parent directory attributes for WCC */
	poa->attributes_follow = true;
	fa = &poa->post_op_attr_u.attributes;
	inode_update_times_now(inode, REFFS_INODE_UPDATE_ATIME);
	inode_attr_to_fattr(inode, fa);

	pthread_mutex_unlock(&inode->i_attr_mutex);

	if (!exists) {
		ret = -ENOENT;
		/* Use resfail instead of resok for attributes */
		res->LOOKUP3res_u.resfail.dir_attributes = *poa;
		goto out;
	}

	pthread_mutex_lock(&exists->i_attr_mutex);

	// When we add more than one sb, be careul here
	nfh = network_file_handle_construct(sb->sb_id, exists->i_ino);
	if (!nfh) {
		pthread_mutex_unlock(&exists->i_attr_mutex);
		ret = -EJUKEBOX;
		res->LOOKUP3res_u.resfail.dir_attributes = *poa;
		goto out;
	}

	resok->object.data.data_val = (char *)nfh;
	resok->object.data.data_len = sizeof(*nfh);

	resok->obj_attributes.attributes_follow = true;
	fa = &resok->obj_attributes.post_op_attr_u.attributes;
	inode_attr_to_fattr(exists, fa);

	pthread_mutex_unlock(&exists->i_attr_mutex);

out:
	res->status = errno_to_nfs3(ret);
	inode_active_put(exists);
	inode_active_put(inode);
	super_block_put(sb);
	return ret;
}

/*
 * How does ACCESS3 fail in a way that
 * it uses ACCESS3resfail?
 */
static int nfs3_op_access(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	struct super_block *sb = NULL;
	struct inode *inode = NULL;
	struct network_file_handle *nfh = NULL;

	ACCESS3args *args = ph->ph_args;
	ACCESS3res *res = ph->ph_res;
	ACCESS3resok *resok = &res->ACCESS3res_u.resok;

	fattr3 *fa;

	struct authunix_parms ap;

	int ret = 0;

	trace_nfs3_srv_access(rt, args);

	ret = rpc_cred_to_authunix_parms(&rt->rt_info, &ap);
	if (ret)
		goto out;

	if (args->object.data.data_len != sizeof(*nfh)) {
		ret = -EBADHANDLE;
		goto out;
	}

	nfh = (struct network_file_handle *)args->object.data.data_val;

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		ret = -ESTALE;
		goto out;
	}

	inode = inode_find(sb, nfh->nfh_ino);
	if (!inode) {
		ret = -ENOENT;
		goto out;
	}

	if (args->access & ACCESS3_READ) {
		if (inode_access_check_flags(inode, &ap, R_OK,
					     REFFS_ACCESS_OWNER_OVERRIDE) == 0)
			resok->access |= ACCESS3_READ;
	}

	if (args->access & ACCESS3_LOOKUP && (S_ISDIR(inode->i_mode))) {
		if (inode_access_check(inode, &ap, X_OK) == 0)
			resok->access |= ACCESS3_LOOKUP;
	}

	if (args->access & ACCESS3_MODIFY) {
		if (inode_access_check_flags(inode, &ap, W_OK,
					     REFFS_ACCESS_OWNER_OVERRIDE) == 0)
			resok->access |= ACCESS3_MODIFY;
	}

	if (args->access & ACCESS3_EXTEND) {
		if (inode_access_check_flags(inode, &ap, W_OK,
					     REFFS_ACCESS_OWNER_OVERRIDE) == 0)
			resok->access |= ACCESS3_EXTEND;
	}

	if (args->access & ACCESS3_DELETE && (S_ISDIR(inode->i_mode))) {
		if (inode_access_check(inode, &ap, W_OK) == 0)
			resok->access |= ACCESS3_DELETE;
	}

	if (args->access & ACCESS3_EXECUTE && !(S_ISDIR(inode->i_mode))) {
		if (inode_access_check(inode, &ap, X_OK) == 0)
			resok->access |= ACCESS3_EXECUTE;
	}

	fa = &resok->obj_attributes.post_op_attr_u.attributes;
	resok->obj_attributes.attributes_follow = true;

	pthread_mutex_lock(&inode->i_attr_mutex); // Consider reader/writer?

	inode_update_times_now(inode, REFFS_INODE_UPDATE_ATIME);
	inode_attr_to_fattr(inode, fa);

	pthread_mutex_unlock(&inode->i_attr_mutex);

out:
	res->status = errno_to_nfs3(ret);
	inode_active_put(inode);
	super_block_put(sb);
	return ret;
}

static int nfs3_op_readlink(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	struct super_block *sb = NULL;
	struct inode *inode = NULL;

	READLINK3args *args = ph->ph_args;
	READLINK3res *res = ph->ph_res;
	READLINK3resok *resok = &res->READLINK3res_u.resok;

	post_op_attr *poa = &resok->symlink_attributes;
	fattr3 *fa;

	char *name = NULL;

	struct network_file_handle *nfh = NULL;

	struct authunix_parms ap;
	int ret = 0;

	trace_nfs3_srv_readlink(rt, args);

	ret = rpc_cred_to_authunix_parms(&rt->rt_info, &ap);
	if (ret)
		goto out;

	if (args->symlink.data.data_len != sizeof(*nfh)) {
		ret = -EBADHANDLE;
		goto out;
	}

	nfh = (struct network_file_handle *)args->symlink.data.data_val;

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		ret = -ESTALE;
		goto out;
	}

	inode = inode_find(sb, nfh->nfh_ino);
	if (!inode) {
		ret = -ENOENT;
		goto out;
	}

	ret = inode_access_check(inode, &ap, R_OK);
	if (ret)
		goto out;

	if (!S_ISLNK(inode->i_mode)) {
		ret = -EINVAL;
		goto out;
	}

	name = strdup(inode->i_symlink);
	if (!name) {
		ret = -EJUKEBOX;
		goto out;
	}

	pthread_mutex_lock(&inode->i_attr_mutex);

	resok->data = name;
	name = NULL;

	inode_update_times_now(inode, REFFS_INODE_UPDATE_ATIME);

	poa->attributes_follow = true;
	fa = &poa->post_op_attr_u.attributes;
	inode_attr_to_fattr(inode, fa);

	pthread_mutex_unlock(&inode->i_attr_mutex);

out:
	res->status = errno_to_nfs3(ret);
	free(name);
	inode_active_put(inode);
	super_block_put(sb);
	return ret;
}

/*
 * nfs3_op_read_resume -- rt_next_action callback after async pread completes.
 *
 * rt->rt_io_result holds the pread return value (bytes read, or -errno).
 * The buffer was allocated and resok was initialised before the pause.
 */
static uint32_t nfs3_op_read_resume(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	READ3args *args = ph->ph_args;
	READ3res *res = ph->ph_res;
	READ3resok *resok = &res->READ3res_u.resok;
	struct inode *inode = ph->ph_inode;

	rt->rt_next_action = NULL;

	ssize_t nread = rt->rt_io_result;
	if (nread < 0) {
		free(resok->data.data_val);
		resok->data.data_val = NULL;
		resok->data.data_len = 0;
		res->status = NFS3ERR_IO;
		goto out;
	}

	resok->data.data_len = (u_int)nread;
	resok->eof =
		(args->offset + (uint64_t)nread >= (uint64_t)inode->i_size);
	resok->count = resok->data.data_len;

	pthread_mutex_lock(&inode->i_attr_mutex);
	inode_update_times_now(inode, REFFS_INODE_UPDATE_ATIME);
	resok->file_attributes.attributes_follow = true;
	inode_attr_to_fattr(inode,
			    &resok->file_attributes.post_op_attr_u.attributes);
	pthread_mutex_unlock(&inode->i_attr_mutex);

	res->status = NFS3_OK;
out:
	inode_active_put(inode);
	ph->ph_inode = NULL;
	super_block_put(ph->ph_sb);
	ph->ph_sb = NULL;
	return 0;
}

static int nfs3_op_read(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	struct super_block *sb = NULL;
	struct inode *inode = NULL;
	bool went_async = false;

	READ3args *args = ph->ph_args;
	READ3res *res = ph->ph_res;
	READ3resok *resok = &res->READ3res_u.resok;

	post_op_attr *poa = &res->READ3res_u.resok.file_attributes;

	struct network_file_handle *nfh = NULL;
	struct authunix_parms ap;
	int ret = 0;

	if (rt->rt_next_action) {
		rt->rt_next_action(rt);
		return ret;
	}

	trace_nfs3_srv_read(rt, args);

	ret = rpc_cred_to_authunix_parms(&rt->rt_info, &ap);
	if (ret)
		goto out;

	if (args->file.data.data_len != sizeof(*nfh)) {
		ret = -EBADHANDLE;
		goto out;
	}

	nfh = (struct network_file_handle *)args->file.data.data_val;

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		ret = -ESTALE;
		goto out;
	}

	inode = inode_find(sb, nfh->nfh_ino);
	if (!inode) {
		ret = -ENOENT;
		goto out;
	}

	ret = inode_access_check(inode, &ap, R_OK);
	if (ret)
		goto out;

	if (S_ISDIR(inode->i_mode)) {
		ret = -EISDIR;
		goto out;
	}

	if (!inode->i_db) {
		resok->count = 0;
		resok->eof = true;
	} else {
		resok->data.data_val = calloc(args->count, sizeof(char));
		if (!resok->data.data_val) {
			ret = -EJUKEBOX;
			poa = &res->READ3res_u.resfail.file_attributes;
			pthread_mutex_lock(&inode->i_attr_mutex);
			goto update_wcc;
		}

		resok->data.data_len = args->count;

		int db_fd = data_block_get_fd(inode->i_db);
		struct ring_context *rc_backend = io_backend_get_global();
		if (db_fd >= 0 && rc_backend && rt->rt_task) {
			/*
			 * Async path: transfer inode/sb ownership to ph so
			 * the resume callback can release them.  Set both
			 * local pointers to NULL so the out: label does not
			 * double-free on submission failure.
			 */
			ph->ph_inode = inode;
			inode = NULL;
			ph->ph_sb = sb;
			sb = NULL;
			rt->rt_next_action = nfs3_op_read_resume;
			went_async = task_pause(rt->rt_task);
			if (went_async) {
				if (io_request_backend_pread(
					    db_fd, resok->data.data_val,
					    args->count, args->offset, rt,
					    rc_backend) < 0) {
					rt->rt_next_action = NULL;
					task_resume(rt->rt_task);
					went_async = false;
					inode = ph->ph_inode;
					ph->ph_inode = NULL;
					sb = ph->ph_sb;
					ph->ph_sb = NULL;
					free(resok->data.data_val);
					resok->data.data_val = NULL;
					resok->data.data_len = 0;
					ret = -EIO;
					poa = &res->READ3res_u.resfail
						       .file_attributes;
					pthread_mutex_lock(
						&inode->i_attr_mutex);
					goto update_wcc;
				}
				return -EINPROGRESS;
			}
			/* task_pause failed (shouldn't happen): restore and fall through */
			inode = ph->ph_inode;
			ph->ph_inode = NULL;
			sb = ph->ph_sb;
			ph->ph_sb = NULL;
			rt->rt_next_action = NULL;
		}

		pthread_rwlock_rdlock(&inode->i_db_rwlock);
		ssize_t dbr = data_block_read(inode->i_db, resok->data.data_val,
					      args->count, args->offset);
		if (dbr == 0 && args->count > 0) {
			// Read beyond current size
			resok->count = resok->data.data_len = 0;
			resok->eof = true;
			pthread_rwlock_unlock(&inode->i_db_rwlock);
			pthread_mutex_lock(&inode->i_attr_mutex);
			goto update_wcc;
		} else if (dbr < 0) {
			free(resok->data.data_val);
			resok->data.data_val = NULL;
			resok->count = resok->data.data_len = 0;
			ret = dbr;
			poa = &res->READ3res_u.resfail.file_attributes;
			pthread_rwlock_unlock(&inode->i_db_rwlock);
			pthread_mutex_lock(&inode->i_attr_mutex);
			goto update_wcc;
		}

		resok->count = resok->data.data_len = dbr;

		if (args->offset + resok->count >= (uint64_t)inode->i_size)
			resok->eof = true;
		pthread_rwlock_unlock(&inode->i_db_rwlock);
	}

	pthread_mutex_lock(&inode->i_attr_mutex);
	inode_update_times_now(inode, REFFS_INODE_UPDATE_ATIME);

update_wcc:
	poa->attributes_follow = true;
	fattr3 *fa = &poa->post_op_attr_u.attributes;
	inode_attr_to_fattr(inode, fa);

	pthread_mutex_unlock(&inode->i_attr_mutex);

out:
	res->status = errno_to_nfs3(ret);
	inode_active_put(inode);
	super_block_put(sb);
	return ret;
}

/*
 * nfs3_op_write_resume -- rt_next_action callback after async pwrite completes.
 *
 * The pre-pause code pre-extended the file with ftruncate and updated
 * db_size; wcc->before was filled in; inode/sb ownership is in ph.
 */
static uint32_t nfs3_op_write_resume(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	WRITE3res *res = ph->ph_res;
	WRITE3resok *resok = &res->WRITE3res_u.resok;
	wcc_data *wcc = &resok->file_wcc;
	struct inode *inode = ph->ph_inode;
	struct super_block *sb = ph->ph_sb;

	rt->rt_next_action = NULL;

	ssize_t nwritten = rt->rt_io_result;
	if (nwritten < 0) {
		res->status = (nwritten == -ENOSPC) ? NFS3ERR_NOSPC :
						      NFS3ERR_IO;
		wcc = &res->WRITE3res_u.resfail.file_wcc;
		goto wcc_after;
	}

	resok->count = (count3)nwritten;
	resok->committed = FILE_SYNC;

	struct server_state *ss = server_state_find();
	if (!ss) {
		res->status = NFS3ERR_IO;
		wcc = &res->WRITE3res_u.resfail.file_wcc;
		goto wcc_after;
	}
	memcpy(resok->verf, ss->ss_uuid + 8, NFS3_WRITEVERFSIZE);
	server_state_put(ss);

	pthread_rwlock_wrlock(&inode->i_db_rwlock);

	size_t new_db_size = inode->i_db->db_size;
	size3 old_size = wcc->before.pre_op_attr_u.attributes.size;

	inode->i_size = (int64_t)new_db_size;
	inode->i_used = inode->i_size / sb->sb_block_size +
			(inode->i_size % sb->sb_block_size ? 1 : 0);

	size_t old_used, new_used;
	do {
		__atomic_load(&sb->sb_bytes_used, &old_used, __ATOMIC_RELAXED);
		if (new_db_size > (size_t)old_size)
			new_used = old_used + (new_db_size - (size_t)old_size);
		else if ((size_t)old_size > new_db_size)
			new_used = old_used > (size_t)old_size - new_db_size ?
					   old_used - ((size_t)old_size -
						       new_db_size) :
					   0;
		else
			new_used = old_used;
	} while (!__atomic_compare_exchange(&sb->sb_bytes_used, &old_used,
					    &new_used, false, __ATOMIC_SEQ_CST,
					    __ATOMIC_RELAXED));

	pthread_rwlock_unlock(&inode->i_db_rwlock);

	pthread_mutex_lock(&inode->i_attr_mutex);
	inode_update_times_now(inode, REFFS_INODE_UPDATE_CTIME |
					      REFFS_INODE_UPDATE_MTIME);
	res->status = NFS3_OK;

wcc_after:
	wcc->after.attributes_follow = true;
	inode_attr_to_fattr(inode, &wcc->after.post_op_attr_u.attributes);
	pthread_mutex_unlock(&inode->i_attr_mutex);

	inode_active_put(inode);
	ph->ph_inode = NULL;
	super_block_put(sb);
	ph->ph_sb = NULL;
	return 0;
}

static int nfs3_op_write(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	struct super_block *sb = NULL;
	struct inode *inode = NULL;
	bool went_async = false;

	WRITE3args *args = ph->ph_args;
	WRITE3res *res = ph->ph_res;
	WRITE3resok *resok = &res->WRITE3res_u.resok;

	wcc_data *wcc = &res->WRITE3res_u.resok.file_wcc;

	struct network_file_handle *nfh = NULL;
	struct authunix_parms ap;

	size3 size;
	nfstime3 mtime;
	nfstime3 ctime;

	size_t db_size;
	int ret = 0;

	if (rt->rt_next_action) {
		rt->rt_next_action(rt);
		return ret;
	}

	trace_nfs3_srv_write(rt, args);

	ret = rpc_cred_to_authunix_parms(&rt->rt_info, &ap);
	if (ret)
		goto out;

	if (args->file.data.data_len != sizeof(*nfh)) {
		ret = -EBADHANDLE;
		goto out;
	}

	nfh = (struct network_file_handle *)args->file.data.data_val;

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		ret = -ESTALE;
		goto out;
	}

	inode = inode_find(sb, nfh->nfh_ino);
	if (!inode) {
		ret = -ENOENT;
		goto out;
	}

	ret = inode_access_check_flags(inode, &ap, W_OK,
				       REFFS_ACCESS_OWNER_OVERRIDE);
	if (ret)
		goto out;

	if (S_ISDIR(inode->i_mode)) {
		ret = -EISDIR;
		goto out;
	}

	size = inode->i_size;
	timespec_to_nfstime3(&inode->i_ctime, &ctime);
	timespec_to_nfstime3(&inode->i_mtime, &mtime);

	pthread_rwlock_wrlock(&inode->i_db_rwlock);
	if (!inode->i_db) {
		inode->i_db = data_block_alloc(inode, args->data.data_val,
					       args->data.data_len,
					       args->offset);
		if (!inode->i_db) {
			ret = -ENOSPC;
			wcc = &res->WRITE3res_u.resfail.file_wcc;
			pthread_rwlock_unlock(&inode->i_db_rwlock);
			pthread_mutex_lock(&inode->i_attr_mutex);
			goto update_wcc;
		}

		resok->count = args->data.data_len;
	} else {
		int db_fd = data_block_get_fd(inode->i_db);
		struct ring_context *rc_backend = io_backend_get_global();

		if (db_fd >= 0 && rc_backend && rt->rt_task) {
			/*
			 * Async path.  Pre-extend so pwrite does not need to
			 * grow the file; db_size is updated so the resume
			 * callback can compute the size delta.
			 */
			size_t new_db_size =
				(size_t)args->offset + args->data.data_len;
			if (new_db_size > inode->i_db->db_size) {
				if (ftruncate(db_fd, (off_t)new_db_size) < 0) {
					int saved = errno;
					pthread_rwlock_unlock(
						&inode->i_db_rwlock);
					ret = (saved == ENOSPC) ? -ENOSPC :
								  -EIO;
					wcc = &res->WRITE3res_u.resfail.file_wcc;
					pthread_mutex_lock(
						&inode->i_attr_mutex);
					goto update_wcc;
				}
				inode->i_db->db_size = new_db_size;
			}
			pthread_rwlock_unlock(&inode->i_db_rwlock);

			/* Populate wcc before; resume callback reads it. */
			wcc->before.attributes_follow = true;
			wcc->before.pre_op_attr_u.attributes.size = size;
			wcc->before.pre_op_attr_u.attributes.mtime = mtime;
			wcc->before.pre_op_attr_u.attributes.ctime = ctime;

			ph->ph_inode = inode;
			inode = NULL;
			ph->ph_sb = sb;
			sb = NULL;
			rt->rt_next_action = nfs3_op_write_resume;
			went_async = task_pause(rt->rt_task);
			if (went_async) {
				if (io_request_backend_pwrite(
					    db_fd, args->data.data_val,
					    args->data.data_len, args->offset,
					    rt, rc_backend) < 0) {
					rt->rt_next_action = NULL;
					task_resume(rt->rt_task);
					went_async = false;
					inode = ph->ph_inode;
					ph->ph_inode = NULL;
					sb = ph->ph_sb;
					ph->ph_sb = NULL;
					ret = -EIO;
					wcc = &res->WRITE3res_u.resfail.file_wcc;
					pthread_mutex_lock(
						&inode->i_attr_mutex);
					goto update_wcc;
				}
				return -EINPROGRESS;
			}
			/* task_pause failed (shouldn't happen): restore and fall through */
			inode = ph->ph_inode;
			ph->ph_inode = NULL;
			sb = ph->ph_sb;
			ph->ph_sb = NULL;
			rt->rt_next_action = NULL;
		}

		ssize_t dbw = data_block_write(inode->i_db, args->data.data_val,
					       args->data.data_len,
					       args->offset);
		if (dbw < 0) {
			ret = dbw;
			wcc = &res->WRITE3res_u.resfail.file_wcc;
			pthread_rwlock_unlock(&inode->i_db_rwlock);
			pthread_mutex_lock(&inode->i_attr_mutex);
			goto update_wcc;
		}

		resok->count = dbw;
	}

	if ((inode->i_mode & S_ISGID) && ap.aup_uid != 0 &&
	    ap.aup_uid != reffs_id_to_uid(inode->i_uid)) {
		inode->i_mode &= ~S_ISGID;
	}

	if ((inode->i_mode & S_ISUID) && ap.aup_uid != 0 &&
	    ap.aup_uid != reffs_id_to_uid(inode->i_uid)) {
		inode->i_mode &= ~S_ISUID;
	}

	/* For now, it is a RAM disk and all writes are done right away! */
	switch (args->stable) {
	case UNSTABLE:
	case DATA_SYNC:
	case FILE_SYNC:
		resok->committed = FILE_SYNC;
		break;
	};

	struct server_state *ss = server_state_find();
	if (!ss) {
		ret = -ESHUTDOWN;
		wcc = &res->WRITE3res_u.resfail.file_wcc;
		pthread_rwlock_unlock(&inode->i_db_rwlock);
		pthread_mutex_lock(&inode->i_attr_mutex);
		goto update_wcc;
	}

	memcpy(resok->verf, ss->ss_uuid + 8, NFS3_WRITEVERFSIZE);
	server_state_put(ss);

	inode->i_size = inode->i_db->db_size;
	inode->i_used = inode->i_size / sb->sb_block_size +
			(inode->i_size % sb->sb_block_size ? 1 : 0);

	db_size = data_block_get_size(inode->i_db);

	size_t old_used;
	size_t new_used;
	do {
		__atomic_load(&inode->i_sb->sb_bytes_used, &old_used,
			      __ATOMIC_RELAXED);
		if (db_size > (size_t)size) {
			new_used = old_used + (db_size - (size_t)size);
		} else if ((size_t)size > db_size) {
			size_t diff = (size_t)size - db_size;
			if (old_used >= diff)
				new_used = old_used - diff;
			else
				new_used = 0;
		} else {
			new_used = old_used;
		}
	} while (!__atomic_compare_exchange(
		&inode->i_sb->sb_bytes_used, &old_used, &new_used, false,
		__ATOMIC_SEQ_CST, __ATOMIC_RELAXED));

	pthread_rwlock_unlock(&inode->i_db_rwlock);

	pthread_mutex_lock(&inode->i_attr_mutex);
	inode_update_times_now(inode, REFFS_INODE_UPDATE_CTIME |
					      REFFS_INODE_UPDATE_MTIME);

update_wcc:
	if (ret)
		wcc = &res->WRITE3res_u.resfail.file_wcc;

	wcc->before.attributes_follow = true;
	wcc->before.pre_op_attr_u.attributes.size = size;
	wcc->before.pre_op_attr_u.attributes.mtime = mtime;
	wcc->before.pre_op_attr_u.attributes.ctime = ctime;
	wcc->after.attributes_follow = true;

	inode_attr_to_fattr(inode, &wcc->after.post_op_attr_u.attributes);

	pthread_mutex_unlock(&inode->i_attr_mutex);

out:
	res->status = errno_to_nfs3(ret);
	inode_active_put(inode);
	super_block_put(sb);
	return ret;
}

static void createverf3_to_timespec(createverf3 verf, struct timespec *ts)
{
	// First 4 bytes for tv_sec, last 4 bytes for tv_nsec
	memcpy(&ts->tv_sec, verf, 4);
	memcpy(&ts->tv_nsec, verf + 4, 4);
}
static nfsstat3 errno_to_nfs3(int err)
{
	if (err < 0)
		err = -err;

	switch (err) {
	case 0:
		return NFS3_OK;
	case EPERM:
		return NFS3ERR_PERM;
	case ENOENT:
		return NFS3ERR_NOENT;
	case EIO:
		return NFS3ERR_IO;
	case ENXIO:
		return NFS3ERR_NXIO;
	case EACCES:
		return NFS3ERR_ACCES;
	case EEXIST:
		return NFS3ERR_EXIST;
	case EXDEV:
		return NFS3ERR_XDEV;
	case ENODEV:
		return NFS3ERR_NODEV;
	case ENOTDIR:
		return NFS3ERR_NOTDIR;
	case EISDIR:
		return NFS3ERR_ISDIR;
	case EINVAL:
		return NFS3ERR_INVAL;
	case EFBIG:
		return NFS3ERR_FBIG;
	case EAGAIN:
		return NFS3ERR_JUKEBOX;
	case EBADHANDLE:
		return NFS3ERR_BADHANDLE;
	case ENOTSYNC:
		return NFS3ERR_NOT_SYNC;
	case EBADTYPE:
		return NFS3ERR_BADTYPE;
	case ENOSPC:
		return NFS3ERR_NOSPC;
	case EROFS:
		return NFS3ERR_ROFS;
	case EMLINK:
		return NFS3ERR_MLINK;
	case ENAMETOOLONG:
		return NFS3ERR_NAMETOOLONG;
	case ENOTEMPTY:
		return NFS3ERR_NOTEMPTY;
	case EDQUOT:
		return NFS3ERR_DQUOT;
	case ESTALE:
		return NFS3ERR_STALE;
	case EREMOTE:
		return NFS3ERR_REMOTE;
	default:
		return NFS3ERR_IO;
	}
}

static int nfs3_op_create(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	struct super_block *sb = NULL;
	struct inode *inode = NULL;
	struct inode *new_inode = NULL;

	CREATE3args *args = ph->ph_args;
	CREATE3res *res = ph->ph_res;
	CREATE3resok *resok = &res->CREATE3res_u.resok;
	CREATE3resfail *resfail = &res->CREATE3res_u.resfail;

	wcc_data *wcc = &resok->dir_wcc;
	sattr3 *sa = &args->how.createhow3_u.obj_attributes;

	size3 size;
	nfstime3 mtime;
	nfstime3 ctime;

	struct network_file_handle *nfh = NULL;

	struct authunix_parms ap;
	int ret = 0;

	trace_nfs3_srv_create(rt, args);

	ret = rpc_cred_to_authunix_parms(&rt->rt_info, &ap);
	if (ret)
		goto out;

	if (args->where.dir.data.data_len != sizeof(*nfh)) {
		ret = -EBADHANDLE;
		goto out;
	}

	nfh = (struct network_file_handle *)args->where.dir.data.data_val;

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		ret = -ESTALE;
		goto out;
	}

	ret = directory_inode_find(sb, nfh->nfh_ino, &ap, W_OK, &inode);
	if (ret)
		goto out;

	/* Pre-op attributes */
	pthread_mutex_lock(&inode->i_attr_mutex);
	size = inode->i_size;
	timespec_to_nfstime3(&inode->i_mtime, &mtime);
	timespec_to_nfstime3(&inode->i_ctime, &ctime);
	pthread_mutex_unlock(&inode->i_attr_mutex);

	if (args->how.mode == EXCLUSIVE) {
		struct timespec verf;
		createverf3_to_timespec(args->how.createhow3_u.verf, &verf);
		ret = vfs_exclusive_create(inode, args->where.name, &verf, &ap,
					   &new_inode);
		if (new_inode)
			ret = 0;
		else
			ret = -ENOENT;
	} else {
		mode_t mode = 0644;
		if (sa->mode.set_it)
			mode = sa->mode.set_mode3_u.mode;

		ret = vfs_create(inode, args->where.name, mode, &ap, &new_inode,
				 NULL, NULL);

		if (ret == -EEXIST && args->how.mode == UNCHECKED) {
			new_inode =
				inode_name_get_inode(inode, args->where.name);
			if (new_inode)
				ret = 0;
			else
				ret = -ENOENT;
		}
	}

	if (ret || !new_inode) {
		wcc = &resfail->dir_wcc;
		goto update_wcc;
	}

	/* Apply attributes if not exclusive (exclusive uses ctime for verifier) */
	if (args->how.mode != EXCLUSIVE) {
		struct reffs_sattr rs;
		nfs3_sattr3_to_reffs_sattr(sa, &rs);
		ret = vfs_setattr(new_inode, &rs, NULL);
		if (ret) {
			wcc = &resfail->dir_wcc;
			goto update_wcc;
		}
	}

	nfh = network_file_handle_construct(sb->sb_id, new_inode->i_ino);
	if (!nfh) {
		ret = -EJUKEBOX;
		wcc = &resfail->dir_wcc;
		goto update_wcc;
	}

	resok->obj.post_op_fh3_u.handle.data.data_val = (char *)nfh;
	resok->obj.post_op_fh3_u.handle.data.data_len = sizeof(*nfh);
	resok->obj.handle_follows = true;

	resok->obj_attributes.attributes_follow = true;
	inode_attr_to_fattr(new_inode,
			    &resok->obj_attributes.post_op_attr_u.attributes);

update_wcc:
	wcc->before.attributes_follow = true;
	wcc->before.pre_op_attr_u.attributes.size = size;
	wcc->before.pre_op_attr_u.attributes.mtime = mtime;
	wcc->before.pre_op_attr_u.attributes.ctime = ctime;

	wcc->after.attributes_follow = true;
	pthread_mutex_lock(&inode->i_attr_mutex);
	inode_attr_to_fattr(inode, &wcc->after.post_op_attr_u.attributes);
	pthread_mutex_unlock(&inode->i_attr_mutex);

out:
	res->status = errno_to_nfs3(ret);
	inode_active_put(new_inode);
	inode_active_put(inode);
	super_block_put(sb);
	return ret;
}

static int nfs3_op_mkdir(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	struct super_block *sb = NULL;
	struct inode *inode = NULL;
	struct inode *new_inode = NULL;

	MKDIR3args *args = ph->ph_args;
	MKDIR3res *res = ph->ph_res;
	MKDIR3resok *resok = &res->MKDIR3res_u.resok;
	MKDIR3resfail *resfail = &res->MKDIR3res_u.resfail;

	wcc_data *wcc = &resok->dir_wcc;
	sattr3 *sa = &args->attributes;

	size3 size;
	nfstime3 mtime;
	nfstime3 ctime;

	struct network_file_handle *nfh = NULL;

	struct authunix_parms ap;
	int ret = 0;

	trace_nfs3_srv_mkdir(rt, args);

	ret = rpc_cred_to_authunix_parms(&rt->rt_info, &ap);
	if (ret)
		goto out;

	if (args->where.dir.data.data_len != sizeof(*nfh)) {
		ret = -EBADHANDLE;
		goto out;
	}

	nfh = (struct network_file_handle *)args->where.dir.data.data_val;

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		ret = -ESTALE;
		goto out;
	}

	ret = directory_inode_find(sb, nfh->nfh_ino, &ap, W_OK, &inode);
	if (ret)
		goto out;

	/* Pre-op attributes */
	pthread_mutex_lock(&inode->i_attr_mutex);
	size = inode->i_size;
	timespec_to_nfstime3(&inode->i_mtime, &mtime);
	timespec_to_nfstime3(&inode->i_ctime, &ctime);
	pthread_mutex_unlock(&inode->i_attr_mutex);

	mode_t mode = 0777;
	if (sa->mode.set_it)
		mode = sa->mode.set_mode3_u.mode;

	ret = vfs_mkdir(inode, args->where.name, mode, &ap, &new_inode, NULL,
			NULL);
	if (ret) {
		wcc = &resfail->dir_wcc;
		goto update_wcc;
	}

	/* Apply other attributes (UID, GID, times) */
	struct reffs_sattr rs;
	nfs3_sattr3_to_reffs_sattr(sa, &rs);
	ret = vfs_setattr(new_inode, &rs, NULL);
	if (ret) {
		wcc = &resfail->dir_wcc;
		goto update_wcc;
	}

	nfh = network_file_handle_construct(sb->sb_id, new_inode->i_ino);
	if (!nfh) {
		ret = -EJUKEBOX;
		wcc = &resfail->dir_wcc;
		goto update_wcc;
	}

	resok->obj.post_op_fh3_u.handle.data.data_val = (char *)nfh;
	resok->obj.post_op_fh3_u.handle.data.data_len = sizeof(*nfh);
	resok->obj.handle_follows = true;

	resok->obj_attributes.attributes_follow = true;
	inode_attr_to_fattr(new_inode,
			    &resok->obj_attributes.post_op_attr_u.attributes);

update_wcc:
	wcc->before.attributes_follow = true;
	wcc->before.pre_op_attr_u.attributes.size = size;
	wcc->before.pre_op_attr_u.attributes.mtime = mtime;
	wcc->before.pre_op_attr_u.attributes.ctime = ctime;

	wcc->after.attributes_follow = true;
	pthread_mutex_lock(&inode->i_attr_mutex);
	inode_attr_to_fattr(inode, &wcc->after.post_op_attr_u.attributes);
	pthread_mutex_unlock(&inode->i_attr_mutex);

out:
	res->status = errno_to_nfs3(ret);
	inode_active_put(new_inode);
	inode_active_put(inode);
	super_block_put(sb);
	return ret;
}

static int nfs3_op_symlink(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	struct super_block *sb = NULL;
	struct inode *inode = NULL;
	struct inode *new_inode = NULL;

	SYMLINK3args *args = ph->ph_args;
	SYMLINK3res *res = ph->ph_res;
	SYMLINK3resok *resok = &res->SYMLINK3res_u.resok;
	SYMLINK3resfail *resfail = &res->SYMLINK3res_u.resfail;

	wcc_data *wcc = &resok->dir_wcc;
	sattr3 *sa = &args->symlink.symlink_attributes;

	size3 size;
	nfstime3 mtime;
	nfstime3 ctime;

	struct network_file_handle *nfh = NULL;

	struct authunix_parms ap;
	int ret = 0;

	trace_nfs3_srv_symlink(rt, args);

	ret = rpc_cred_to_authunix_parms(&rt->rt_info, &ap);
	if (ret)
		goto out;

	if (args->where.dir.data.data_len != sizeof(*nfh)) {
		ret = -EBADHANDLE;
		goto out;
	}

	nfh = (struct network_file_handle *)args->where.dir.data.data_val;

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		ret = -ESTALE;
		goto out;
	}

	ret = directory_inode_find(sb, nfh->nfh_ino, &ap, W_OK, &inode);
	if (ret)
		goto out;

	if (strlen(args->where.name) > REFFS_MAX_NAME) {
		ret = -ENAMETOOLONG;
		goto out;
	}

	if (strlen(args->symlink.symlink_data) > REFFS_MAX_PATH) {
		ret = -ENAMETOOLONG;
		goto out;
	}

	/* Pre-op attributes */
	pthread_mutex_lock(&inode->i_attr_mutex);
	size = inode->i_size;
	timespec_to_nfstime3(&inode->i_mtime, &mtime);
	timespec_to_nfstime3(&inode->i_ctime, &ctime);
	pthread_mutex_unlock(&inode->i_attr_mutex);

	ret = vfs_symlink(inode, args->where.name, args->symlink.symlink_data,
			  &ap, &new_inode, NULL, NULL);
	if (ret) {
		wcc = &resfail->dir_wcc;
		goto update_wcc;
	}

	/* Apply attributes (UID, GID, etc.) */
	struct reffs_sattr rs;
	nfs3_sattr3_to_reffs_sattr(sa, &rs);
	ret = vfs_setattr(new_inode, &rs, NULL);
	if (ret) {
		wcc = &resfail->dir_wcc;
		goto update_wcc;
	}

	nfh = network_file_handle_construct(sb->sb_id, new_inode->i_ino);
	if (!nfh) {
		ret = -EJUKEBOX;
		wcc = &resfail->dir_wcc;
		goto update_wcc;
	}

	resok->obj.post_op_fh3_u.handle.data.data_val = (char *)nfh;
	resok->obj.post_op_fh3_u.handle.data.data_len = sizeof(*nfh);
	resok->obj.handle_follows = true;

	resok->obj_attributes.attributes_follow = true;
	inode_attr_to_fattr(new_inode,
			    &resok->obj_attributes.post_op_attr_u.attributes);

update_wcc:
	if (ret)
		wcc = &resfail->dir_wcc;

	wcc->after.attributes_follow = true;
	pthread_mutex_lock(&inode->i_attr_mutex);
	inode_attr_to_fattr(inode, &wcc->after.post_op_attr_u.attributes);
	pthread_mutex_unlock(&inode->i_attr_mutex);

	wcc->before.attributes_follow = true;
	wcc->before.pre_op_attr_u.attributes.size = size;
	wcc->before.pre_op_attr_u.attributes.mtime = mtime;
	wcc->before.pre_op_attr_u.attributes.ctime = ctime;

out:
	res->status = errno_to_nfs3(ret);
	inode_active_put(new_inode);
	inode_active_put(inode);
	super_block_put(sb);
	return ret;
}

static int nfs3_op_mknod(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	struct super_block *sb = NULL;
	struct inode *inode = NULL;
	struct inode *new_inode = NULL;

	MKNOD3args *args = ph->ph_args;
	MKNOD3res *res = ph->ph_res;
	MKNOD3resok *resok = &res->MKNOD3res_u.resok;
	MKNOD3resfail *resfail = &res->MKNOD3res_u.resfail;

	wcc_data *wcc = &resok->dir_wcc;
	sattr3 *sa = NULL;

	size3 size;
	nfstime3 mtime;
	nfstime3 ctime;

	struct network_file_handle *nfh = NULL;

	struct authunix_parms ap;
	int ret = 0;

	trace_nfs3_srv_mknod(rt, args);

	ret = rpc_cred_to_authunix_parms(&rt->rt_info, &ap);
	if (ret)
		goto out;

	if (args->where.dir.data.data_len != sizeof(*nfh)) {
		ret = -EBADHANDLE;
		goto out;
	}

	nfh = (struct network_file_handle *)args->where.dir.data.data_val;

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		ret = -ESTALE;
		goto out;
	}

	if (strlen(args->where.name) > REFFS_MAX_NAME) {
		ret = -ENAMETOOLONG;
		goto out;
	}

	ret = directory_inode_find(sb, nfh->nfh_ino, &ap, W_OK, &inode);
	if (ret)
		goto out;

	dev_t rdev = 0;
	mode_t mode = 0;
	switch (args->what.type) {
	case NF3BLK:
		mode = S_IFBLK;
		rdev = makedev(args->what.mknoddata3_u.device.spec.specdata1,
			       args->what.mknoddata3_u.device.spec.specdata2);
		sa = &args->what.mknoddata3_u.device.dev_attributes;
		break;
	case NF3CHR:
		mode = S_IFCHR;
		rdev = makedev(args->what.mknoddata3_u.device.spec.specdata1,
			       args->what.mknoddata3_u.device.spec.specdata2);
		sa = &args->what.mknoddata3_u.device.dev_attributes;
		break;
	case NF3SOCK:
		mode = S_IFSOCK;
		sa = &args->what.mknoddata3_u.pipe_attributes;
		break;
	case NF3FIFO:
		mode = S_IFIFO;
		sa = &args->what.mknoddata3_u.pipe_attributes;
		break;
	default:
		ret = -EBADTYPE;
		goto out;
	}

	if (sa->mode.set_it)
		mode |= sa->mode.set_mode3_u.mode;
	else
		mode |= 0644;

	/* Pre-op attributes */
	pthread_mutex_lock(&inode->i_attr_mutex);
	size = inode->i_size;
	timespec_to_nfstime3(&inode->i_mtime, &mtime);
	timespec_to_nfstime3(&inode->i_ctime, &ctime);
	pthread_mutex_unlock(&inode->i_attr_mutex);

	ret = vfs_mknod(inode, args->where.name, mode, rdev, &ap, &new_inode,
			NULL, NULL);
	if (ret) {
		wcc = &resfail->dir_wcc;
		goto update_wcc;
	}

	/* Apply other attributes (UID, GID, etc.) */
	struct reffs_sattr rs;
	nfs3_sattr3_to_reffs_sattr(sa, &rs);
	ret = vfs_setattr(new_inode, &rs, NULL);
	if (ret) {
		wcc = &resfail->dir_wcc;
		goto update_wcc;
	}

	nfh = network_file_handle_construct(sb->sb_id, new_inode->i_ino);
	if (!nfh) {
		ret = -EJUKEBOX;
		wcc = &resfail->dir_wcc;
		goto update_wcc;
	}

	resok->obj.post_op_fh3_u.handle.data.data_val = (char *)nfh;
	resok->obj.post_op_fh3_u.handle.data.data_len = sizeof(*nfh);
	resok->obj.handle_follows = true;

	resok->obj_attributes.attributes_follow = true;
	inode_attr_to_fattr(new_inode,
			    &resok->obj_attributes.post_op_attr_u.attributes);

update_wcc:
	if (ret)
		wcc = &resfail->dir_wcc;

	wcc->before.attributes_follow = true;
	wcc->before.pre_op_attr_u.attributes.size = size;
	wcc->before.pre_op_attr_u.attributes.mtime = mtime;
	wcc->before.pre_op_attr_u.attributes.ctime = ctime;

	wcc->after.attributes_follow = true;
	pthread_mutex_lock(&inode->i_attr_mutex);
	inode_attr_to_fattr(inode, &wcc->after.post_op_attr_u.attributes);
	pthread_mutex_unlock(&inode->i_attr_mutex);

out:
	res->status = errno_to_nfs3(ret);
	inode_active_put(new_inode);
	inode_active_put(inode);
	super_block_put(sb);
	return ret;
}

static int nfs3_op_remove(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	struct super_block *sb = NULL;
	struct inode *inode = NULL;

	REMOVE3args *args = ph->ph_args;
	REMOVE3res *res = ph->ph_res;
	REMOVE3resok *resok = &res->REMOVE3res_u.resok;
	wcc_data *wcc = &resok->dir_wcc;

	struct network_file_handle *nfh = NULL;

	struct authunix_parms ap;

	uint64_t size;
	struct timespec mtime;
	struct timespec ctime;
	int ret = 0;

	trace_nfs3_srv_remove(rt, args);

	ret = rpc_cred_to_authunix_parms(&rt->rt_info, &ap);
	if (ret)
		goto out;

	if (args->object.dir.data.data_len != sizeof(*nfh)) {
		ret = -EBADHANDLE;
		goto out;
	}

	nfh = (struct network_file_handle *)args->object.dir.data.data_val;

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		ret = -ESTALE;
		goto out;
	}

	ret = directory_inode_find(sb, nfh->nfh_ino, &ap, W_OK, &inode);
	if (ret)
		goto out;

	/* Pre-op attributes */
	pthread_mutex_lock(&inode->i_attr_mutex);
	size = inode->i_size;
	mtime = inode->i_mtime;
	ctime = inode->i_ctime;
	pthread_mutex_unlock(&inode->i_attr_mutex);

	if (strlen(args->object.name) > REFFS_MAX_NAME) {
		ret = -ENAMETOOLONG;
		goto update_wcc;
	}

	ret = vfs_remove(inode, args->object.name, &ap, NULL, NULL);

update_wcc:
	wcc->before.attributes_follow = true;
	wcc->before.pre_op_attr_u.attributes.size = size;
	timespec_to_nfstime3(&mtime,
			     &wcc->before.pre_op_attr_u.attributes.mtime);
	timespec_to_nfstime3(&ctime,
			     &wcc->before.pre_op_attr_u.attributes.ctime);

	wcc->after.attributes_follow = true;
	pthread_mutex_lock(&inode->i_attr_mutex);
	inode_attr_to_fattr(inode, &wcc->after.post_op_attr_u.attributes);
	pthread_mutex_unlock(&inode->i_attr_mutex);

out:
	res->status = errno_to_nfs3(ret);
	inode_active_put(inode);
	super_block_put(sb);
	return ret;
}

static int nfs3_op_rmdir(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	struct super_block *sb = NULL;
	struct inode *inode = NULL;

	RMDIR3args *args = ph->ph_args;
	RMDIR3res *res = ph->ph_res;
	RMDIR3resok *resok = &res->RMDIR3res_u.resok;
	wcc_data *wcc = &resok->dir_wcc;

	struct network_file_handle *nfh = NULL;

	struct authunix_parms ap;

	uint64_t size;
	struct timespec mtime;
	struct timespec ctime;
	int ret = 0;

	trace_nfs3_srv_rmdir(rt, args);

	ret = rpc_cred_to_authunix_parms(&rt->rt_info, &ap);
	if (ret)
		goto out;

	if (args->object.dir.data.data_len != sizeof(*nfh)) {
		ret = -EBADHANDLE;
		goto out;
	}

	nfh = (struct network_file_handle *)args->object.dir.data.data_val;

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		ret = -ESTALE;
		goto out;
	}

	ret = directory_inode_find(sb, nfh->nfh_ino, &ap, W_OK, &inode);
	if (ret)
		goto out;

	/* Pre-op attributes */
	pthread_mutex_lock(&inode->i_attr_mutex);
	size = inode->i_size;
	mtime = inode->i_mtime;
	ctime = inode->i_ctime;
	pthread_mutex_unlock(&inode->i_attr_mutex);

	if (strlen(args->object.name) > REFFS_MAX_NAME) {
		ret = -ENAMETOOLONG;
		goto update_wcc;
	}

	ret = vfs_rmdir(inode, args->object.name, &ap, NULL, NULL);

update_wcc:
	wcc->before.attributes_follow = true;
	wcc->before.pre_op_attr_u.attributes.size = size;
	timespec_to_nfstime3(&mtime,
			     &wcc->before.pre_op_attr_u.attributes.mtime);
	timespec_to_nfstime3(&ctime,
			     &wcc->before.pre_op_attr_u.attributes.ctime);

	wcc->after.attributes_follow = true;
	pthread_mutex_lock(&inode->i_attr_mutex);
	inode_attr_to_fattr(inode, &wcc->after.post_op_attr_u.attributes);
	pthread_mutex_unlock(&inode->i_attr_mutex);

out:
	res->status = errno_to_nfs3(ret);
	inode_active_put(inode);
	super_block_put(sb);
	return ret;
}

static int nfs3_op_rename(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	struct super_block *sb = NULL;

	struct inode *inode_src = NULL;
	struct inode *inode_dst = NULL;

	RENAME3args *args = ph->ph_args;
	RENAME3res *res = ph->ph_res;
	RENAME3resok *resok = &res->RENAME3res_u.resok;
	wcc_data *wcc_src = &resok->fromdir_wcc;
	wcc_data *wcc_dst = &resok->todir_wcc;

	struct network_file_handle *nfh_src = NULL;
	struct network_file_handle *nfh_dst = NULL;

	struct authunix_parms ap;

	uint64_t size_src;
	struct timespec mtime_src;
	struct timespec ctime_src;

	uint64_t size_dst;
	struct timespec mtime_dst;
	struct timespec ctime_dst;
	int ret = 0;

	trace_nfs3_srv_rename(rt, args);

	ret = rpc_cred_to_authunix_parms(&rt->rt_info, &ap);
	if (ret)
		goto out;

	if (args->from.dir.data.data_len != sizeof(*nfh_src)) {
		ret = -EBADHANDLE;
		goto out;
	}
	nfh_src = (struct network_file_handle *)args->from.dir.data.data_val;

	if (args->to.dir.data.data_len != sizeof(*nfh_dst)) {
		ret = -EBADHANDLE;
		goto out;
	}
	nfh_dst = (struct network_file_handle *)args->to.dir.data.data_val;

	if (nfh_src->nfh_sb != nfh_dst->nfh_sb) {
		ret = -EXDEV;
		goto out;
	}

	sb = super_block_find(nfh_src->nfh_sb);
	if (!sb) {
		ret = -ESTALE;
		goto out;
	}

	ret = directory_inode_find(sb, nfh_src->nfh_ino, &ap, W_OK, &inode_src);
	if (ret)
		goto out;

	ret = directory_inode_find(sb, nfh_dst->nfh_ino, &ap, W_OK, &inode_dst);
	if (ret)
		goto out;

	/* Pre-op attributes */
	pthread_mutex_lock(&inode_src->i_attr_mutex);
	size_src = inode_src->i_size;
	mtime_src = inode_src->i_mtime;
	ctime_src = inode_src->i_ctime;
	pthread_mutex_unlock(&inode_src->i_attr_mutex);

	pthread_mutex_lock(&inode_dst->i_attr_mutex);
	size_dst = inode_dst->i_size;
	mtime_dst = inode_dst->i_mtime;
	ctime_dst = inode_dst->i_ctime;
	pthread_mutex_unlock(&inode_dst->i_attr_mutex);

	if (strlen(args->from.name) > REFFS_MAX_NAME ||
	    strlen(args->to.name) > REFFS_MAX_NAME) {
		ret = -ENAMETOOLONG;
		goto update_wcc;
	}

	ret = vfs_rename(inode_src, args->from.name, inode_dst, args->to.name,
			 &ap, NULL, NULL, NULL, NULL);

update_wcc:
	wcc_src->before.attributes_follow = true;
	wcc_src->before.pre_op_attr_u.attributes.size = size_src;
	timespec_to_nfstime3(&mtime_src,
			     &wcc_src->before.pre_op_attr_u.attributes.mtime);
	timespec_to_nfstime3(&ctime_src,
			     &wcc_src->before.pre_op_attr_u.attributes.ctime);

	wcc_dst->before.attributes_follow = true;
	wcc_dst->before.pre_op_attr_u.attributes.size = size_dst;
	timespec_to_nfstime3(&mtime_dst,
			     &wcc_dst->before.pre_op_attr_u.attributes.mtime);
	timespec_to_nfstime3(&ctime_dst,
			     &wcc_dst->before.pre_op_attr_u.attributes.ctime);

	wcc_src->after.attributes_follow = true;
	pthread_mutex_lock(&inode_src->i_attr_mutex);
	inode_attr_to_fattr(inode_src,
			    &wcc_src->after.post_op_attr_u.attributes);
	pthread_mutex_unlock(&inode_src->i_attr_mutex);

	wcc_dst->after.attributes_follow = true;
	pthread_mutex_lock(&inode_dst->i_attr_mutex);
	inode_attr_to_fattr(inode_dst,
			    &wcc_dst->after.post_op_attr_u.attributes);
	pthread_mutex_unlock(&inode_dst->i_attr_mutex);

out:
	res->status = errno_to_nfs3(ret);
	inode_active_put(inode_src);
	inode_active_put(inode_dst);
	super_block_put(sb);
	return ret;
}

static int nfs3_op_link(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	struct super_block *sb = NULL;
	struct inode *inode = NULL;
	struct inode *inode_dir = NULL;

	LINK3args *args = ph->ph_args;
	LINK3res *res = ph->ph_res;
	LINK3resok *resok = &res->LINK3res_u.resok;
	wcc_data *wcc = &resok->linkdir_wcc;
	post_op_attr *poa = &resok->file_attributes;

	struct network_file_handle *nfh = NULL;
	struct network_file_handle *nfh_dir = NULL;

	struct authunix_parms ap;

	uint64_t size;
	struct timespec mtime;
	struct timespec ctime;
	int ret = 0;

	trace_nfs3_srv_link(rt, args);

	ret = rpc_cred_to_authunix_parms(&rt->rt_info, &ap);
	if (ret)
		goto out;

	if (args->file.data.data_len != sizeof(*nfh)) {
		ret = -EBADHANDLE;
		goto out;
	}
	nfh = (struct network_file_handle *)args->file.data.data_val;

	if (args->link.dir.data.data_len != sizeof(*nfh_dir)) {
		ret = -EBADHANDLE;
		goto out;
	}
	nfh_dir = (struct network_file_handle *)args->link.dir.data.data_val;

	if (nfh->nfh_sb != nfh_dir->nfh_sb) {
		ret = -EXDEV;
		goto out;
	}

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		ret = -ESTALE;
		goto out;
	}

	inode = inode_find(sb, nfh->nfh_ino);
	if (!inode) {
		ret = -ENOENT;
		goto out;
	}

	ret = directory_inode_find(sb, nfh_dir->nfh_ino, &ap, W_OK, &inode_dir);
	if (ret)
		goto out;

	/* Pre-op attributes of the directory */
	pthread_mutex_lock(&inode_dir->i_attr_mutex);
	size = inode_dir->i_size;
	mtime = inode_dir->i_mtime;
	ctime = inode_dir->i_ctime;
	pthread_mutex_unlock(&inode_dir->i_attr_mutex);

	if (strlen(args->link.name) > REFFS_MAX_NAME) {
		ret = -ENAMETOOLONG;
		goto update_wcc;
	}

	ret = vfs_link(inode, inode_dir, args->link.name, &ap);

update_wcc:
	wcc->before.attributes_follow = true;
	wcc->before.pre_op_attr_u.attributes.size = size;
	timespec_to_nfstime3(&mtime,
			     &wcc->before.pre_op_attr_u.attributes.mtime);
	timespec_to_nfstime3(&ctime,
			     &wcc->before.pre_op_attr_u.attributes.ctime);

	poa->attributes_follow = true;
	pthread_mutex_lock(&inode->i_attr_mutex);
	inode_attr_to_fattr(inode, &poa->post_op_attr_u.attributes);
	pthread_mutex_unlock(&inode->i_attr_mutex);

	wcc->after.attributes_follow = true;
	pthread_mutex_lock(&inode_dir->i_attr_mutex);
	inode_attr_to_fattr(inode_dir, &wcc->after.post_op_attr_u.attributes);
	pthread_mutex_unlock(&inode_dir->i_attr_mutex);

out:
	res->status = errno_to_nfs3(ret);
	inode_active_put(inode);
	inode_active_put(inode_dir);
	super_block_put(sb);
	return ret;
}

static int nfs3_op_readdir(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	struct super_block *sb = NULL;
	struct inode *inode = NULL;

	READDIR3args *args = ph->ph_args;
	READDIR3res *res = ph->ph_res;
	READDIR3resok *resok = &res->READDIR3res_u.resok;

	post_op_attr *poa = &resok->dir_attributes;
	fattr3 *fa;

	dirlist3 *dl = NULL;
	cookie3 cookie = args->cookie;
	count3 count = 0;

	struct network_file_handle *nfh = NULL;

	struct authunix_parms ap;

	entry3 *e_next = NULL;
	int ret = 0;

	trace_nfs3_srv_readdir(rt, args);

	ret = rpc_cred_to_authunix_parms(&rt->rt_info, &ap);
	if (ret)
		goto out;

	if (args->dir.data.data_len != sizeof(*nfh)) {
		ret = -EBADHANDLE;
		goto out;
	}

	nfh = (struct network_file_handle *)args->dir.data.data_val;

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		ret = -ESTALE;
		goto out;
	}

	ret = directory_inode_find(sb, nfh->nfh_ino, &ap, R_OK, &inode);
	if (ret)
		goto out;

	count = sizeof(READDIR3res);
	if (count > args->count) {
		ret = -EINVAL;
		goto out;
	}

	pthread_mutex_lock(&inode->i_attr_mutex);

	/*
	 * dir_de is the dirent that owns this directory inode.
	 * For the root inode i_dirent is NULL; fall back to sb_dirent.
	 */
	struct reffs_dirent *dir_de = inode->i_dirent ? inode->i_dirent :
							sb->sb_dirent;
	bool dir_de_rdlocked = true;
	pthread_rwlock_rdlock(&dir_de->rd_rwlock);

	/* Determine parent ino for .. entry */
	uint64_t parent_ino =
		(dir_de->rd_parent && dir_de->rd_parent->rd_inode) ?
			dir_de->rd_parent->rd_inode->i_ino :
			inode->i_ino; /* root: .. points to self */

	dl = &resok->reply;

	if (cookie == 0) {
		entry3 *e = calloc(1, sizeof(*e));
		if (!e) {
			if (!dl->entries) {
				free(dl);
				ret = -EJUKEBOX;
				poa = &res->READDIR3res_u.resfail.dir_attributes;
				goto update_wcc;
			}

			goto past_eof;
		}

		e->fileid = inode->i_ino;
		e->cookie = 1;
		e->name = strdup(".");
		if (!e->name) {
			free(e);
			if (!dl->entries) {
				free(dl);
				ret = -EJUKEBOX;
				poa = &res->READDIR3res_u.resfail.dir_attributes;
				goto update_wcc;
			}

			goto past_eof;
		}

		count += sizeof(*e) + strlen(e->name) + 1;
		if (count > args->count) {
			free(e->name);
			free(e);
			goto past_eof;
		}

		if (!dl->entries) {
			dl->entries = e;
		} else {
			e_next->nextentry = e;
		}

		e_next = e;
	}

	if (cookie < 2) {
		entry3 *e = calloc(1, sizeof(*e));
		if (!e) {
			if (!dl->entries) {
				free(dl);
				ret = -EJUKEBOX;
				poa = &res->READDIR3res_u.resfail.dir_attributes;
				goto update_wcc;
			}

			goto past_eof;
		}

		e->fileid = parent_ino;
		e->cookie = 2;
		e->name = strdup("..");
		if (!e->name) {
			free(e);
			if (!dl->entries) {
				free(dl);
				ret = -EJUKEBOX;
				poa = &res->READDIR3res_u.resfail.dir_attributes;
				goto update_wcc;
			}

			goto past_eof;
		}

		count += sizeof(*e) + strlen(e->name) + 1;
		if (count > args->count) {
			free(e->name);
			free(e);
			goto past_eof;
		}

		if (!dl->entries) {
			dl->entries = e;
		} else {
			e_next->nextentry = e;
		}

		e_next = e;
	}

	/*
	 * Phase 1: snapshot dirent identity fields under rcu_read_lock.
	 * rd_siblings is RCU-protected; we may not call dirent_ensure_inode
	 * (which can block on I/O) while holding the read-side lock.
	 */
	struct {
		struct reffs_dirent *rd;
		uint64_t rd_ino;
		uint64_t rd_cookie;
		char *rd_name;
	} *snap = NULL;
	size_t snap_count = 0, snap_cap = 0;

	rcu_read_lock();
	{
		struct reffs_dirent *rd;
		cds_list_for_each_entry_rcu(rd, &inode->i_dirent->rd_children,
					    rd_siblings) {
			if (rd->rd_cookie <= cookie)
				continue;
			if (snap_count == snap_cap) {
				size_t new_cap = snap_cap ? snap_cap * 2 : 16;
				void *tmp =
					realloc(snap, new_cap * sizeof(*snap));
				if (!tmp) {
					rcu_read_unlock();
					free(snap);
					ret = -EJUKEBOX;
					poa = &res->READDIR3res_u.resfail
						       .dir_attributes;
					goto update_wcc;
				}
				snap = tmp;
				snap_cap = new_cap;
			}
			snap[snap_count].rd = rd;
			snap[snap_count].rd_ino = rd->rd_ino;
			snap[snap_count].rd_cookie = rd->rd_cookie;
			snap[snap_count].rd_name = rd->rd_name;
			snap_count++;
		}
	}
	rcu_read_unlock();

	/*
	 * Phase 1 complete: release rd_rwlock now.  Phase 2 calls
	 * dirent_ensure_inode which may block on inode_alloc / LRU eviction.
	 * Holding rd_rwlock across that would deadlock against vfs_lock_dirs
	 * (which takes rd_rwlock as a writer) in concurrent create threads.
	 */
	pthread_rwlock_unlock(&dir_de->rd_rwlock);
	dir_de_rdlocked = false;

	/*
	 * Phase 2: fault in inodes and build the reply list.
	 * dirent_ensure_inode may call inode_alloc / hit the storage
	 * backend; safe to call here with no RCU lock held.
	 */
	for (size_t si = 0; si < snap_count; si++) {
		struct inode *rd_inode = dirent_ensure_inode(snap[si].rd);
		if (!rd_inode) {
			free(snap);
			ret = -EIO;
			poa = &res->READDIR3res_u.resfail.dir_attributes;
			goto update_wcc;
		}

		entry3 *e = calloc(1, sizeof(*e));
		if (!e) {
			inode_active_put(rd_inode);
			free(snap);
			if (!dl->entries) {
				free(dl);
				ret = -EJUKEBOX;
				poa = &res->READDIR3res_u.resfail.dir_attributes;
				goto update_wcc;
			}
			goto past_eof;
		}

		e->fileid = rd_inode->i_ino;
		e->cookie = snap[si].rd_cookie;
		e->name = strdup(snap[si].rd_name);
		inode_active_put(rd_inode);
		if (!e->name) {
			free(e);
			free(snap);
			if (!dl->entries) {
				free(dl);
				ret = -EJUKEBOX;
				poa = &res->READDIR3res_u.resfail.dir_attributes;
				goto update_wcc;
			}
			goto past_eof;
		}

		count += sizeof(*e) + strlen(e->name) + 1;
		if (count > args->count) {
			free(e->name);
			free(e);
			free(snap);
			goto past_eof;
		}

		if (!dl->entries) {
			dl->entries = e;
		} else {
			e_next->nextentry = e;
		}
		e_next = e;
	}
	free(snap);

	dl->eof = true;

past_eof:
	inode_update_times_now(inode, REFFS_INODE_UPDATE_ATIME);

update_wcc:
	if (ret)
		poa = &res->READDIR3res_u.resfail.dir_attributes;

	poa->attributes_follow = true;
	fa = &poa->post_op_attr_u.attributes;
	inode_attr_to_fattr(inode, fa);

	if (dir_de_rdlocked)
		pthread_rwlock_unlock(&dir_de->rd_rwlock);
	pthread_mutex_unlock(&inode->i_attr_mutex);

out:
	res->status = errno_to_nfs3(ret);
	inode_active_put(inode);
	super_block_put(sb);
	return ret;
}

static int nfs3_op_readdirplus(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	struct super_block *sb = NULL;
	struct inode *inode = NULL;

	READDIRPLUS3args *args = ph->ph_args;
	READDIRPLUS3res *res = ph->ph_res;
	READDIRPLUS3resok *resok = &res->READDIRPLUS3res_u.resok;

	post_op_attr *poa_e;
	post_op_attr *poa = &resok->dir_attributes;
	fattr3 *fa;

	dirlistplus3 *dl = NULL;
	cookie3 cookie = args->cookie;
	count3 maxcount = 0;
	count3 dircount = 0;

	struct network_file_handle *nfh = NULL;

	struct authunix_parms ap;

	entryplus3 *e_next = NULL;
	int ret = 0;

	trace_nfs3_srv_readdirplus(rt, args);

	ret = rpc_cred_to_authunix_parms(&rt->rt_info, &ap);
	if (ret)
		goto out;

	if (args->dir.data.data_len != sizeof(*nfh)) {
		ret = -EBADHANDLE;
		goto out;
	}

	nfh = (struct network_file_handle *)args->dir.data.data_val;

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		ret = -ESTALE;
		goto out;
	}

	ret = directory_inode_find(sb, nfh->nfh_ino, &ap, R_OK, &inode);
	if (ret)
		goto out;

	maxcount = sizeof(READDIRPLUS3res);
	if (maxcount > args->maxcount) {
		ret = -EINVAL;
		goto out;
	}

	pthread_mutex_lock(&inode->i_attr_mutex);

	struct reffs_dirent *dir_de = inode->i_dirent ? inode->i_dirent :
							sb->sb_dirent;
	bool dir_de_rdlocked = true;
	pthread_rwlock_rdlock(&dir_de->rd_rwlock);

	dl = &resok->reply;

	/* Determine parent ino for .. entry */
	uint64_t parent_ino =
		(dir_de->rd_parent && dir_de->rd_parent->rd_inode) ?
			dir_de->rd_parent->rd_inode->i_ino :
			inode->i_ino; /* root: .. points to self */

	if (cookie == 0) {
		entryplus3 *e = calloc(1, sizeof(*e));
		if (!e) {
			if (!dl->entries) {
				free(dl);
				ret = -EJUKEBOX;
				poa = &res->READDIRPLUS3res_u.resfail
					       .dir_attributes;
				goto update_wcc;
			}

			goto past_eof;
		}

		e->fileid = inode->i_ino;
		e->cookie = 1;
		e->name = strdup(".");
		if (!e->name) {
			free(e);
			if (!dl->entries) {
				free(dl);
				ret = -EJUKEBOX;
				poa = &res->READDIRPLUS3res_u.resfail
					       .dir_attributes;
				goto update_wcc;
			}

			goto past_eof;
		}

		// With multi-sb, check for mounted on
		nfh = network_file_handle_construct(sb->sb_id, inode->i_ino);
		if (!nfh) {
			free(e->name);
			free(e);

			if (!dl->entries) {
				free(dl);
				ret = -EJUKEBOX;
				poa = &res->READDIRPLUS3res_u.resfail
					       .dir_attributes;
				goto update_wcc;
			}

			goto past_eof;
		}

		e->name_handle.post_op_fh3_u.handle.data.data_val = (char *)nfh;
		e->name_handle.post_op_fh3_u.handle.data.data_len =
			sizeof(*nfh);
		e->name_handle.handle_follows = true;

		poa_e = &e->name_attributes;
		poa_e->attributes_follow = true;
		fa = &poa_e->post_op_attr_u.attributes;
		inode_attr_to_fattr(inode, fa);

		dircount += sizeof(*e) - sizeof(post_op_attr);
		if (dircount > args->dircount) {
			free(nfh);
			free(e->name);
			free(e);
			goto past_eof;
		}

		maxcount += sizeof(*e) + strlen(e->name) + sizeof(*nfh) + 1;
		if (maxcount > args->maxcount) {
			free(nfh);
			free(e->name);
			free(e);
			goto past_eof;
		}

		if (!dl->entries) {
			dl->entries = e;
		} else {
			e_next->nextentry = e;
		}

		e_next = e;
	}

	if (cookie < 2) {
		entryplus3 *e = calloc(1, sizeof(*e));
		if (!e) {
			if (!dl->entries) {
				free(dl);
				res->status = NFS3ERR_JUKEBOX;
				poa = &res->READDIRPLUS3res_u.resfail
					       .dir_attributes;
				goto update_wcc;
			}

			goto past_eof;
		}

		e->fileid = parent_ino;
		e->cookie = 2;
		e->name = strdup("..");
		if (!e->name) {
			free(e);
			if (!dl->entries) {
				free(dl);
				res->status = NFS3ERR_JUKEBOX;
				poa = &res->READDIRPLUS3res_u.resfail
					       .dir_attributes;
				goto update_wcc;
			}

			goto past_eof;
		}

		// With multi-sb, check for mounted on
		nfh = network_file_handle_construct(sb->sb_id, parent_ino);
		if (!nfh) {
			free(e->name);
			free(e);

			if (!dl->entries) {
				free(dl);
				res->status = NFS3ERR_JUKEBOX;
				poa = &res->READDIRPLUS3res_u.resfail
					       .dir_attributes;
				goto update_wcc;
			}

			goto past_eof;
		}

		e->name_handle.post_op_fh3_u.handle.data.data_val = (char *)nfh;
		e->name_handle.post_op_fh3_u.handle.data.data_len =
			sizeof(*nfh);
		e->name_handle.handle_follows = true;

		poa_e = &e->name_attributes;
		poa_e->attributes_follow = true;
		fa = &poa_e->post_op_attr_u.attributes;
		inode_attr_to_fattr(inode, fa);

		dircount += sizeof(*e) - sizeof(post_op_attr);
		if (dircount > args->dircount) {
			free(nfh);
			free(e->name);
			free(e);
			goto past_eof;
		}

		maxcount += sizeof(*e) + strlen(e->name) + sizeof(*nfh) + 1;
		if (maxcount > args->maxcount) {
			free(nfh);
			free(e->name);
			free(e);
			goto past_eof;
		}

		if (!dl->entries) {
			dl->entries = e;
		} else {
			e_next->nextentry = e;
		}

		e_next = e;
	}

	/*
	 * Phase 1: snapshot dirent identity fields under rcu_read_lock.
	 * rd_siblings is RCU-protected; we may not call dirent_ensure_inode
	 * (which can block on I/O) while holding the read-side lock.
	 */
	struct {
		struct reffs_dirent *rd;
		uint64_t rd_ino;
		uint64_t rd_cookie;
		char *rd_name;
	} *snap = NULL;
	size_t snap_count = 0, snap_cap = 0;

	rcu_read_lock();
	{
		struct reffs_dirent *rd;
		cds_list_for_each_entry_rcu(rd, &inode->i_dirent->rd_children,
					    rd_siblings) {
			if (rd->rd_cookie < cookie)
				continue;
			if (snap_count == snap_cap) {
				size_t new_cap = snap_cap ? snap_cap * 2 : 16;
				void *tmp =
					realloc(snap, new_cap * sizeof(*snap));
				if (!tmp) {
					rcu_read_unlock();
					free(snap);
					res->status = NFS3ERR_JUKEBOX;
					poa = &res->READDIRPLUS3res_u.resfail
						       .dir_attributes;
					goto update_wcc;
				}
				snap = tmp;
				snap_cap = new_cap;
			}
			snap[snap_count].rd = rd;
			snap[snap_count].rd_ino = rd->rd_ino;
			snap[snap_count].rd_cookie = rd->rd_cookie;
			snap[snap_count].rd_name = rd->rd_name;
			snap_count++;
		}
	}
	rcu_read_unlock();

	/*
	 * Phase 1 complete: release rd_rwlock before phase 2.
	 * Same reasoning as readdir: dirent_ensure_inode may block,
	 * and holding rd_rwlock (reader) would deadlock against
	 * concurrent vfs_lock_dirs (writer) in create threads.
	 */
	pthread_rwlock_unlock(&dir_de->rd_rwlock);
	dir_de_rdlocked = false;

	/*
	 * Phase 2: fault in inodes and build the reply list.
	 */
	for (size_t si = 0; si < snap_count; si++) {
		struct inode *rd_inode = dirent_ensure_inode(snap[si].rd);
		if (!rd_inode) {
			free(snap);
			res->status = NFS3ERR_SERVERFAULT;
			poa = &res->READDIRPLUS3res_u.resfail.dir_attributes;
			goto update_wcc;
		}

		entryplus3 *e = calloc(1, sizeof(*e));
		if (!e) {
			inode_active_put(rd_inode);
			free(snap);
			if (!dl->entries) {
				free(dl);
				res->status = NFS3ERR_JUKEBOX;
				poa = &res->READDIRPLUS3res_u.resfail
					       .dir_attributes;
				goto update_wcc;
			}
			goto past_eof;
		}

		e->fileid = rd_inode->i_ino;
		e->cookie = snap[si].rd_cookie;
		e->name = strdup(snap[si].rd_name);
		if (!e->name) {
			inode_active_put(rd_inode);
			free(e);
			free(snap);
			if (!dl->entries) {
				free(dl);
				res->status = NFS3ERR_JUKEBOX;
				poa = &res->READDIRPLUS3res_u.resfail
					       .dir_attributes;
				goto update_wcc;
			}
			goto past_eof;
		}

		// With multi-sb, check for mounted on
		nfh = network_file_handle_construct(sb->sb_id, rd_inode->i_ino);
		if (!nfh) {
			inode_active_put(rd_inode);
			free(e->name);
			free(e);
			free(snap);
			if (!dl->entries) {
				free(dl);
				res->status = NFS3ERR_JUKEBOX;
				poa = &res->READDIRPLUS3res_u.resfail
					       .dir_attributes;
				goto update_wcc;
			}
			goto past_eof;
		}

		e->name_handle.post_op_fh3_u.handle.data.data_val = (char *)nfh;
		e->name_handle.post_op_fh3_u.handle.data.data_len =
			sizeof(*nfh);
		e->name_handle.handle_follows = true;

		poa_e = &e->name_attributes;
		poa_e->attributes_follow = true;
		fa = &poa_e->post_op_attr_u.attributes;
		inode_attr_to_fattr(rd_inode, fa);
		inode_active_put(rd_inode);

		dircount += sizeof(*e) - sizeof(post_op_attr);
		if (dircount > args->dircount) {
			free(nfh);
			free(e->name);
			free(e);
			free(snap);
			goto past_eof;
		}

		maxcount += sizeof(*e) + strlen(e->name) + sizeof(*nfh) + 1;
		if (maxcount > args->maxcount) {
			free(nfh);
			free(e->name);
			free(e);
			free(snap);
			goto past_eof;
		}

		if (!dl->entries) {
			dl->entries = e;
		} else {
			e_next->nextentry = e;
		}
		e_next = e;
	}
	free(snap);

	dl->eof = true;

past_eof:
	inode_update_times_now(inode, REFFS_INODE_UPDATE_ATIME);

update_wcc:
	poa->attributes_follow = true;
	fa = &poa->post_op_attr_u.attributes;
	inode_attr_to_fattr(inode, fa);

	if (dir_de_rdlocked)
		pthread_rwlock_unlock(&dir_de->rd_rwlock);
	pthread_mutex_unlock(&inode->i_attr_mutex);

out:
	inode_active_put(inode);
	super_block_put(sb);
	return ret;
}

static int nfs3_op_fsstat(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	struct super_block *sb = NULL;
	struct inode *inode = NULL;

	FSSTAT3args *args = ph->ph_args;
	FSSTAT3res *res = ph->ph_res;
	FSSTAT3resok *resok = &res->FSSTAT3res_u.resok;

	post_op_attr *poa = &resok->obj_attributes;

	struct network_file_handle *nfh = NULL;
	struct authunix_parms ap;
	int ret = 0;

	trace_nfs3_srv_fsstat(rt, args);

	ret = rpc_cred_to_authunix_parms(&rt->rt_info, &ap);
	if (ret)
		goto out;

	if (args->fsroot.data.data_len != sizeof(*nfh)) {
		ret = -EBADHANDLE;
		goto out;
	}

	nfh = (struct network_file_handle *)args->fsroot.data.data_val;

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		ret = -ESTALE;
		goto out;
	}

	inode = inode_find(sb, nfh->nfh_ino);
	if (!inode) {
		ret = -ENOENT;
		goto out;
	}

	ret = inode_access_check(inode, &ap, R_OK);
	if (ret)
		goto out;

	pthread_mutex_lock(&inode->i_attr_mutex);

	inode_update_times_now(inode, REFFS_INODE_UPDATE_ATIME);

	resok->tbytes = sb->sb_bytes_max;
	size_t used;

	__atomic_load(&sb->sb_bytes_used, &used, __ATOMIC_RELAXED);
	resok->fbytes = (used < sb->sb_bytes_max) ? sb->sb_bytes_max - used : 0;
	resok->abytes = resok->fbytes;
	resok->tfiles = sb->sb_inodes_max;
	resok->ffiles = __atomic_load_n(&sb->sb_inodes_used, __ATOMIC_RELAXED);
	resok->afiles = resok->tfiles - resok->ffiles;
	resok->invarsec = 0;

	poa->attributes_follow = true;
	fattr3 *fa = &poa->post_op_attr_u.attributes;
	inode_attr_to_fattr(inode, fa);

	pthread_mutex_unlock(&inode->i_attr_mutex);

out:
	res->status = errno_to_nfs3(ret);
	inode_active_put(inode);
	super_block_put(sb);
	return ret;
}

static int nfs3_op_fsinfo(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	struct super_block *sb = NULL;
	struct inode *inode = NULL;

	FSINFO3args *args = ph->ph_args;
	FSINFO3res *res = ph->ph_res;
	FSINFO3resok *resok = &res->FSINFO3res_u.resok;

	post_op_attr *poa = &resok->obj_attributes;

	struct network_file_handle *nfh = NULL;
	struct authunix_parms ap;
	int ret = 0;

	trace_nfs3_srv_fsinfo(rt, args);

	ret = rpc_cred_to_authunix_parms(&rt->rt_info, &ap);
	if (ret)
		goto out;

	if (args->fsroot.data.data_len != sizeof(*nfh)) {
		ret = -EBADHANDLE;
		goto out;
	}

	nfh = (struct network_file_handle *)args->fsroot.data.data_val;

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		ret = -ESTALE;
		goto out;
	}

	inode = inode_find(sb, nfh->nfh_ino);
	if (!inode) {
		ret = -ENOENT;
		goto out;
	}

	ret = inode_access_check(inode, &ap, R_OK);
	if (ret)
		goto out;

	pthread_mutex_lock(&inode->i_attr_mutex);

	inode_update_times_now(inode, REFFS_INODE_UPDATE_ATIME);
	poa->attributes_follow = true;
	fattr3 *fa = &poa->post_op_attr_u.attributes;
	inode_attr_to_fattr(inode, fa);

	pthread_mutex_unlock(&inode->i_attr_mutex);

	nfstime3 gran = { .seconds = 0, .nseconds = 1 };
	resok->rtmax = 1048576;
	resok->rtpref = resok->rtmax;
	resok->rtmult = sb->sb_block_size;
	resok->wtmax = resok->rtmax;
	resok->wtpref = resok->wtmax;
	resok->wtmult = sb->sb_block_size;
	resok->dtpref = resok->wtmax;
	resok->maxfilesize = 9223372036854775807;
	resok->time_delta = gran;
	resok->properties = FSF3_LINK | FSF3_SYMLINK | FSF3_HOMOGENEOUS |
			    FSF3_CANSETTIME;

out:
	res->status = errno_to_nfs3(ret);
	inode_active_put(inode);
	super_block_put(sb);
	return ret;
}

static int nfs3_op_pathconf(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	struct super_block *sb = NULL;
	struct inode *inode = NULL;

	PATHCONF3args *args = ph->ph_args;
	PATHCONF3res *res = ph->ph_res;
	PATHCONF3resok *resok = &res->PATHCONF3res_u.resok;

	post_op_attr *poa = &resok->obj_attributes;

	struct network_file_handle *nfh = NULL;
	struct authunix_parms ap;
	int ret = 0;

	trace_nfs3_srv_pathconf(rt, args);

	ret = rpc_cred_to_authunix_parms(&rt->rt_info, &ap);
	if (ret)
		goto out;

	if (args->object.data.data_len != sizeof(*nfh)) {
		ret = -EBADHANDLE;
		goto out;
	}

	nfh = (struct network_file_handle *)args->object.data.data_val;

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		ret = -ESTALE;
		goto out;
	}

	inode = inode_find(sb, nfh->nfh_ino);
	if (!inode) {
		ret = -ENOENT;
		goto out;
	}

	ret = inode_access_check(inode, &ap, R_OK);
	if (ret)
		goto out;

	pthread_mutex_lock(&inode->i_attr_mutex);

	inode_update_times_now(inode, REFFS_INODE_UPDATE_ATIME);
	poa->attributes_follow = true;
	fattr3 *fa = &poa->post_op_attr_u.attributes;
	inode_attr_to_fattr(inode, fa);

	pthread_mutex_unlock(&inode->i_attr_mutex);

	resok->linkmax = 255;
	resok->name_max = REFFS_MAX_NAME;
	resok->no_trunc = false;
	resok->chown_restricted = false;
	resok->case_insensitive =
		reffs_case_get() == reffs_text_case_insensitive ? true : false;
	resok->case_preserving = true;

out:
	res->status = errno_to_nfs3(ret);
	inode_active_put(inode);
	super_block_put(sb);
	return ret;
}

static int nfs3_op_commit(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	struct super_block *sb = NULL;
	struct inode *inode = NULL;

	COMMIT3args *args = ph->ph_args;
	COMMIT3res *res = ph->ph_res;
	COMMIT3resok *resok = &res->COMMIT3res_u.resok;

	wcc_data *wcc = &res->COMMIT3res_u.resok.file_wcc;

	struct network_file_handle *nfh = NULL;
	struct authunix_parms ap;

	size3 size;
	nfstime3 mtime;
	nfstime3 ctime;

	int ret = 0;

	trace_nfs3_srv_commit(rt, args);

	ret = rpc_cred_to_authunix_parms(&rt->rt_info, &ap);
	if (ret)
		goto out;

	if (args->file.data.data_len != sizeof(*nfh)) {
		ret = -EBADHANDLE;
		goto out;
	}

	nfh = (struct network_file_handle *)args->file.data.data_val;

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		ret = -ESTALE;
		goto out;
	}

	inode = inode_find(sb, nfh->nfh_ino);
	if (!inode) {
		ret = -ENOENT;
		goto out;
	}

	ret = inode_access_check_flags(inode, &ap, W_OK,
				       REFFS_ACCESS_OWNER_OVERRIDE);
	if (ret)
		goto out;

	struct server_state *ss = server_state_find();
	if (!ss) {
		ret = -ESHUTDOWN;
		goto out;
	}
	memcpy(resok->verf, ss->ss_uuid + 8, NFS3_WRITEVERFSIZE);
	server_state_put(ss);

	pthread_mutex_lock(&inode->i_attr_mutex);

	size = inode->i_size;
	timespec_to_nfstime3(&inode->i_ctime, &ctime);
	timespec_to_nfstime3(&inode->i_mtime, &mtime);

	inode_update_times_now(inode, REFFS_INODE_UPDATE_ATIME);

	wcc->before.attributes_follow = true;
	wcc->before.pre_op_attr_u.attributes.size = size;
	wcc->before.pre_op_attr_u.attributes.mtime = mtime;
	wcc->before.pre_op_attr_u.attributes.ctime = ctime;
	wcc->after.attributes_follow = true;

	inode_attr_to_fattr(inode, &wcc->after.post_op_attr_u.attributes);

	pthread_mutex_unlock(&inode->i_attr_mutex);

out:
	res->status = errno_to_nfs3(ret);
	inode_active_put(inode);
	super_block_put(sb);
	return ret;
}

struct rpc_operations_handler nfs3_operations_handler[] = {
	RPC_OPERATION_INIT(NFSPROC3, NULL, NULL, NULL, NULL, NULL,
			   nfs3_op_null),
	RPC_OPERATION_INIT(NFSPROC3, GETATTR, xdr_GETATTR3args, GETATTR3args,
			   xdr_GETATTR3res, GETATTR3res, nfs3_op_getattr),
	RPC_OPERATION_INIT(NFSPROC3, SETATTR, xdr_SETATTR3args, SETATTR3args,
			   xdr_SETATTR3res, SETATTR3res, nfs3_op_setattr),
	RPC_OPERATION_INIT(NFSPROC3, LOOKUP, xdr_LOOKUP3args, LOOKUP3args,
			   xdr_LOOKUP3res, LOOKUP3res, nfs3_op_lookup),
	RPC_OPERATION_INIT(NFSPROC3, ACCESS, xdr_ACCESS3args, ACCESS3args,
			   xdr_ACCESS3res, ACCESS3res, nfs3_op_access),
	RPC_OPERATION_INIT(NFSPROC3, READLINK, xdr_READLINK3args, READLINK3args,
			   xdr_READLINK3res, READLINK3res, nfs3_op_readlink),
	RPC_OPERATION_INIT(NFSPROC3, READ, xdr_READ3args, READ3args,
			   xdr_READ3res, READ3res, nfs3_op_read),
	RPC_OPERATION_INIT(NFSPROC3, WRITE, xdr_WRITE3args, WRITE3args,
			   xdr_WRITE3res, WRITE3res, nfs3_op_write),
	RPC_OPERATION_INIT(NFSPROC3, CREATE, xdr_CREATE3args, CREATE3args,
			   xdr_CREATE3res, CREATE3res, nfs3_op_create),
	RPC_OPERATION_INIT(NFSPROC3, MKDIR, xdr_MKDIR3args, MKDIR3args,
			   xdr_MKDIR3res, MKDIR3res, nfs3_op_mkdir),
	RPC_OPERATION_INIT(NFSPROC3, SYMLINK, xdr_SYMLINK3args, SYMLINK3args,
			   xdr_SYMLINK3res, SYMLINK3res, nfs3_op_symlink),
	RPC_OPERATION_INIT(NFSPROC3, MKNOD, xdr_MKNOD3args, MKNOD3args,
			   xdr_MKNOD3res, MKNOD3res, nfs3_op_mknod),
	RPC_OPERATION_INIT(NFSPROC3, REMOVE, xdr_REMOVE3args, REMOVE3args,
			   xdr_REMOVE3res, REMOVE3res, nfs3_op_remove),
	RPC_OPERATION_INIT(NFSPROC3, RMDIR, xdr_RMDIR3args, RMDIR3args,
			   xdr_RMDIR3res, RMDIR3res, nfs3_op_rmdir),
	RPC_OPERATION_INIT(NFSPROC3, RENAME, xdr_RENAME3args, RENAME3args,
			   xdr_RENAME3res, RENAME3res, nfs3_op_rename),
	RPC_OPERATION_INIT(NFSPROC3, LINK, xdr_LINK3args, LINK3args,
			   xdr_LINK3res, LINK3res, nfs3_op_link),
	RPC_OPERATION_INIT(NFSPROC3, READDIR, xdr_READDIR3args, READDIR3args,
			   xdr_READDIR3res, READDIR3res, nfs3_op_readdir),
	RPC_OPERATION_INIT(NFSPROC3, READDIRPLUS, xdr_READDIRPLUS3args,
			   READDIRPLUS3args, xdr_READDIRPLUS3res,
			   READDIRPLUS3res, nfs3_op_readdirplus),
	RPC_OPERATION_INIT(NFSPROC3, FSSTAT, xdr_FSSTAT3args, FSSTAT3args,
			   xdr_FSSTAT3res, FSSTAT3res, nfs3_op_fsstat),
	RPC_OPERATION_INIT(NFSPROC3, FSINFO, xdr_FSINFO3args, FSINFO3args,
			   xdr_FSINFO3res, FSINFO3res, nfs3_op_fsinfo),
	RPC_OPERATION_INIT(NFSPROC3, PATHCONF, xdr_PATHCONF3args, PATHCONF3args,
			   xdr_PATHCONF3res, PATHCONF3res, nfs3_op_pathconf),
	RPC_OPERATION_INIT(NFSPROC3, COMMIT, xdr_COMMIT3args, COMMIT3args,
			   xdr_COMMIT3res, COMMIT3res, nfs3_op_commit),
};

static struct rpc_program_handler *nfs3_handler;

volatile sig_atomic_t nfsv3_registered = 0;

int nfs3_protocol_register(void)
{
	if (nfsv3_registered)
		return 0;

	nfsv3_registered = 1;

	nfs3_handler = rpc_program_handler_alloc(
		NFS3_PROGRAM, NFS_V3, nfs3_operations_handler,
		sizeof(nfs3_operations_handler) /
			sizeof(*nfs3_operations_handler));
	if (!nfs3_handler) {
		nfsv3_registered = 0;
		return ENOMEM;
	}

	return 0;
}

int nfs3_protocol_deregister(void)
{
	if (!nfsv3_registered)
		return 0;

	rpc_program_handler_put(nfs3_handler);
	nfs3_handler = NULL;
	nfsv3_registered = 0;

	return 0;
}
