/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
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
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
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

/*
 * On locking order:
 *
 * 1) Always take the d_rwlock of the dirent after
 * the i_attr_mutex of the inode.
 *
 * 2) Always take the i_db_rwlock of the inode after the
 * i_attr_mutex of the inode.
 */

static void inode_attr_to_fattr(struct inode *inode, fattr3 *fa)
{
	fa->mode = inode->i_mode;
	fa->nlink = inode->i_nlink;
	fa->uid = inode->i_uid;
	fa->gid = inode->i_gid;
	fa->size = inode->i_size;
	fa->used = inode->i_used;
	fa->rdev.specdata1 = inode->i_dev_major;
	fa->rdev.specdata2 = inode->i_dev_minor;
	fa->fsid = inode->i_sb->sb_id;
	fa->fileid = inode->i_ino;
	timespec_to_nfstime3(&inode->i_atime, &fa->atime);
	timespec_to_nfstime3(&inode->i_mtime, &fa->mtime);
	timespec_to_nfstime3(&inode->i_ctime, &fa->ctime);
}

static nfsstat3 nfs3_apply_sattr3(struct inode *inode, sattr3 *sa,
				  uint64_t *flags)
{
	if (sa->size.set_it) {
		if (inode->i_mode & S_IFDIR)
			return NFS3ERR_ISDIR;

		size_t sz = data_block_resize(inode->i_db,
					      sa->size.set_size3_u.size);
		if (sz < 0)
			return -sz;

		inode->i_size = inode->i_db->db_size;
		inode->i_used =
			inode->i_size / 4096 + (inode->i_size % 4096 ? 1 : 0);
		*flags |= REFFS_INODE_UPDATE_CTIME | REFFS_INODE_UPDATE_MTIME;
	}

	if (sa->mode.set_it) {
		inode->i_mode = (sa->mode.set_mode3_u.mode & 07777);
		*flags |= REFFS_INODE_UPDATE_CTIME;
	}

	if (sa->uid.set_it) {
		inode->i_uid = sa->uid.set_uid3_u.uid;
		*flags |= REFFS_INODE_UPDATE_CTIME;
	}

	if (sa->gid.set_it) {
		inode->i_gid = sa->gid.set_gid3_u.gid;
		*flags |= REFFS_INODE_UPDATE_CTIME;
	}

	switch (sa->atime.set_it) {
	case DONT_CHANGE:
		break;
	case SET_TO_SERVER_TIME:
		*flags |= REFFS_INODE_UPDATE_CTIME | REFFS_INODE_UPDATE_ATIME;
		break;
	case SET_TO_CLIENT_TIME:
		nfstime3_to_timespec(&sa->atime.set_atime_u.atime,
				     &inode->i_atime);
		*flags |= REFFS_INODE_UPDATE_CTIME;
		break;
	}

	switch (sa->mtime.set_it) {
	case DONT_CHANGE:
		break;
	case SET_TO_SERVER_TIME:
		*flags |= REFFS_INODE_UPDATE_CTIME | REFFS_INODE_UPDATE_MTIME;
		break;
	case SET_TO_CLIENT_TIME:
		nfstime3_to_timespec(&sa->mtime.set_mtime_u.mtime,
				     &inode->i_mtime);
		*flags |= REFFS_INODE_UPDATE_CTIME;
		*flags &= ~REFFS_INODE_UPDATE_MTIME;
		break;
	}

	return NFS3_OK;
}

static uint32_t nfs3_getfh_crc(nfs_fh3 *fh)
{
	return crc32(0L, (const Bytef *)fh->data.data_val, fh->data.data_len);
}

static void print_nfs_fh3_hex(nfs_fh3 *fh)
{
	uint32_t crc =
		crc32(0L, (const Bytef *)fh->data.data_val, fh->data.data_len);

	struct network_file_handle *nfh;

	nfh = (struct network_file_handle *)fh;

	TRACE("FileHandle: sb = %lu, ino =%lu, vers = %u, CRC32 = 0x%08x",
	      nfh->nfh_sb, nfh->nfh_ino, nfh->nfh_vers, crc);
}

static bool nfs3_gid_in_gids(gid_t gid, uint32_t len, gid_t *gids)
{
	for (uint32_t i = 0; i < len; len++)
		if (gid == gids[i])
			return true;

	return false;
}

static nfsstat3 nfs3_access_check(struct inode *inode, struct rpc_cred *cred,
				  struct authunix_parms *ap, int mode)
{
	switch (cred->rc_flavor) {
	case AUTH_SYS:
		ap->aup_uid = cred->rc_unix.aup_uid;
		ap->aup_gid = cred->rc_unix.aup_gid;
		ap->aup_len = cred->rc_unix.aup_len;
		ap->aup_gids = cred->rc_unix.aup_gids;
		break;
	case AUTH_NONE:
		ap->aup_uid = 65534;
		ap->aup_gid = 65534;

		ap->aup_len = 0;
		ap->aup_gids = NULL;

		break;
	default:
		return NFS3ERR_ACCES; // Should have already been done at RPC layer
	}

	if (ap->aup_uid == inode->i_uid) {
		if ((mode & W_OK) && !(inode->i_mode & S_IWUSR))
			return NFS3ERR_ACCES;
		if ((mode & R_OK) && !(inode->i_mode & S_IRUSR))
			return NFS3ERR_ACCES;
		if ((mode & X_OK) && !(inode->i_mode & S_IXUSR))
			return NFS3ERR_ACCES;
	} else if (ap->aup_gid == inode->i_gid ||
		   nfs3_gid_in_gids(inode->i_gid, ap->aup_len, ap->aup_gids)) {
		if ((mode & W_OK) && !(inode->i_mode & S_IWGRP))
			return NFS3ERR_ACCES;
		if ((mode & R_OK) && !(inode->i_mode & S_IRGRP))
			return NFS3ERR_ACCES;
		if ((mode & X_OK) && !(inode->i_mode & S_IXGRP))
			return NFS3ERR_ACCES;
	} else {
		if ((mode & W_OK) && !(inode->i_mode & S_IWOTH))
			return NFS3ERR_ACCES;
		if ((mode & R_OK) && !(inode->i_mode & S_IROTH))
			return NFS3ERR_ACCES;
		if ((mode & X_OK) && !(inode->i_mode & S_IXOTH))
			return NFS3ERR_ACCES;
	}

	return NFS3_OK;
}

static int nfs3_null(struct rpc_trans *rt)
{
	TRACE("NULL: xid=0x%08x", rt->rt_info.ri_xid);
	return 0;
}

static int nfs3_getattr(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	struct super_block *sb = NULL;
	struct inode *inode = NULL;

	GETATTR3args *args = ph->ph_args;
	GETATTR3res *res = ph->ph_res;
	fattr3 *fa = &res->GETATTR3res_u.resok.obj_attributes;

	struct network_file_handle *nfh = NULL;
	struct authunix_parms ap;

	if (args->object.data.data_len != sizeof(*nfh)) {
		res->status = NFS3ERR_BADHANDLE;
		uint32_t crc = nfs3_getfh_crc(&args->object);
		TRACE("GETATTR: xid=0x%08x badfh crc=0x%08x",
		      rt->rt_info.ri_xid, crc);
		goto out;
	}

	nfh = (struct network_file_handle *)args->object.data.data_val;
	TRACE("GETATTR: xid=0x%08x sb=%lu ino=%lu", rt->rt_info.ri_xid,
	      nfh->nfh_sb, nfh->nfh_ino);

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		res->status = NFS3ERR_STALE;
		goto out;
	}

	inode = inode_find(sb, nfh->nfh_ino);
	if (!inode) {
		res->status = NFS3ERR_NOENT;
		goto out;
	}

	res->status = nfs3_access_check(inode, &rt->rt_info.ri_cred, &ap, R_OK);
	if (res->status)
		goto out;

	inode_attr_to_fattr(inode, fa);

	print_nfs_fh3_hex(&args->object);

out:
	inode_put(inode);
	super_block_put(sb);
	return res->status;
}

static int nfs3_setattr(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	struct super_block *sb = NULL;
	struct inode *inode = NULL;

	SETATTR3args *args = ph->ph_args;
	SETATTR3res *res = ph->ph_res;
	wcc_data *wcc = &res->SETATTR3res_u.resok.obj_wcc;
	fattr3 *fa = &wcc->after.post_op_attr_u.attributes;
	sattr3 *sa = &args->new_attributes;

	struct network_file_handle *nfh = NULL;

	size3 size;
	nfstime3 mtime;
	nfstime3 ctime;

	uint64_t flags = 0;
	struct authunix_parms ap;

	if (args->object.data.data_len != sizeof(*nfh)) {
		res->status = NFS3ERR_BADHANDLE;
		uint32_t crc = nfs3_getfh_crc(&args->object);
		TRACE("SETATTR: xid=0x%08x badfh crc=0x%08x",
		      rt->rt_info.ri_xid, crc);
		goto out;
	}

	nfh = (struct network_file_handle *)args->object.data.data_val;
	TRACE("SETATTR: xid=0x%08x sb=%lu ino=%lu", rt->rt_info.ri_xid,
	      nfh->nfh_sb, nfh->nfh_ino);

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		res->status = NFS3ERR_STALE;
		goto out;
	}

	inode = inode_find(sb, nfh->nfh_ino);
	if (!inode) {
		res->status = NFS3ERR_NOENT;
		goto out;
	}

	res->status = nfs3_access_check(inode, &rt->rt_info.ri_cred, &ap, W_OK);
	if (res->status)
		goto out;

	pthread_mutex_lock(&inode->i_attr_mutex);

	size = inode->i_size;
	timespec_to_nfstime3(&inode->i_ctime, &ctime);
	timespec_to_nfstime3(&inode->i_mtime, &mtime);

	if (args->guard.check) {
		if (!nfstime3_is_timespec(&args->guard.sattrguard3_u.obj_ctime,
					  &inode->i_ctime)) {
			res->status = NFS3ERR_NOT_SYNC;
			wcc = &res->SETATTR3res_u.resfail.obj_wcc;
			fa = &wcc->after.post_op_attr_u.attributes;
			goto update_wcc;
		}
	}

	pthread_rwlock_wrlock(&inode->i_db_rwlock);
	res->status = nfs3_apply_sattr3(inode, sa, &flags);
	pthread_rwlock_unlock(&inode->i_db_rwlock);
	if (res->status) {
		wcc = &res->SETATTR3res_u.resfail.obj_wcc;
		fa = &wcc->after.post_op_attr_u.attributes;
		goto update_wcc;
	}

	if (flags)
		inode_update_times_now(inode, flags);

update_wcc:
	wcc->before.attributes_follow = true;
	wcc->before.pre_op_attr_u.attributes.size = size;
	wcc->before.pre_op_attr_u.attributes.mtime = mtime;
	wcc->before.pre_op_attr_u.attributes.ctime = ctime;
	wcc->after.attributes_follow = true;
	inode_attr_to_fattr(inode, fa);

	pthread_mutex_unlock(&inode->i_attr_mutex);

	print_nfs_fh3_hex(&args->object);

out:
	inode_put(inode);
	super_block_put(sb);
	return res->status;
}

static int nfs3_lookup(struct rpc_trans *rt)
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

	if (args->what.dir.data.data_len != sizeof(*nfh)) {
		res->status = NFS3ERR_BADHANDLE;
		uint32_t crc = nfs3_getfh_crc(&args->what.dir);
		TRACE("LOOKUP: xid=0x%08x badfh crc=0x%08x", rt->rt_info.ri_xid,
		      crc);
		goto out;
	}

	nfh = (struct network_file_handle *)args->what.dir.data.data_val;
	TRACE("LOOKUP: xid=0x%08x sb=%lu ino=%lu", rt->rt_info.ri_xid,
	      nfh->nfh_sb, nfh->nfh_ino);

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		res->status = NFS3ERR_STALE;
		goto out;
	}

	inode = inode_find(sb, nfh->nfh_ino);
	if (!inode) {
		res->status = NFS3ERR_NOENT;
		goto out;
	}

	res->status = nfs3_access_check(inode, &rt->rt_info.ri_cred, &ap, R_OK);
	if (res->status)
		goto out;

	if (!(inode->i_mode & S_IFDIR)) {
		res->status = NFS3ERR_NOTDIR;
		goto out;
	}

	pthread_mutex_lock(&inode->i_attr_mutex);

	exists = inode_name_get_inode(inode, args->what.name);
	if (!exists) {
		res->status = NFS3ERR_NOENT;
		poa = &res->LOOKUP3res_u.resfail.dir_attributes;
		goto update_wcc;
	}

	nfh = calloc(1, sizeof(struct nfs_fh3));
	if (!nfh) {
		res->status = NFS3ERR_JUKEBOX;
		poa = &res->LOOKUP3res_u.resfail.dir_attributes;
		goto update_wcc;
	}

	resok->object.data.data_val = (char *)nfh;
	resok->object.data.data_len = sizeof(nfs_fh3);
	nfh->nfh_vers = FILEHANDLE_VERSION_CURR;
	nfh->nfh_sb = sb->sb_id;
	nfh->nfh_ino = exists->i_ino;

	resok->obj_attributes.attributes_follow = true;
	fa = &resok->obj_attributes.post_op_attr_u.attributes;

	inode_attr_to_fattr(exists, fa);

update_wcc:
	poa->attributes_follow = true;
	fa = &poa->post_op_attr_u.attributes;

	inode_attr_to_fattr(inode, fa);

	pthread_mutex_unlock(&inode->i_attr_mutex);

	print_nfs_fh3_hex(&args->what.dir);

out:
	inode_put(exists);
	inode_put(inode);
	super_block_put(sb);
	return res->status;
}

/*
 * How does ACCESS3 fail in a way that
 * it uses ACCESS3resfail?
 */
static int nfs3_access(struct rpc_trans *rt)
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

	nfsstat3 status;

	if (args->object.data.data_len != sizeof(*nfh)) {
		res->status = NFS3ERR_BADHANDLE;
		uint32_t crc = nfs3_getfh_crc(&args->object);
		TRACE("ACCESS: xid=0x%08x badfh crc=0x%08x", rt->rt_info.ri_xid,
		      crc);
		goto out;
	}

	nfh = (struct network_file_handle *)args->object.data.data_val;
	TRACE("ACCESS: xid=0x%08x sb=%lu ino=%lu", rt->rt_info.ri_xid,
	      nfh->nfh_sb, nfh->nfh_ino);

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		res->status = NFS3ERR_STALE;
		goto out;
	}

	inode = inode_find(sb, nfh->nfh_ino);
	if (!inode) {
		res->status = NFS3ERR_NOENT;
		goto out;
	}

	if (args->access & ACCESS3_READ) {
		status = nfs3_access_check(inode, &rt->rt_info.ri_cred, &ap,
					   R_OK);
		if (!status)
			resok->access |= ACCESS3_READ;
	}

	if (args->access & ACCESS3_LOOKUP && (inode->i_mode & S_IFDIR)) {
		status = nfs3_access_check(inode, &rt->rt_info.ri_cred, &ap,
					   R_OK);
		if (!status)
			resok->access |= ACCESS3_LOOKUP;
	}

	if (args->access & ACCESS3_MODIFY) {
		status = nfs3_access_check(inode, &rt->rt_info.ri_cred, &ap,
					   W_OK);
		if (!status)
			resok->access |= ACCESS3_MODIFY;
	}

	if (args->access & ACCESS3_EXTEND) {
		status = nfs3_access_check(inode, &rt->rt_info.ri_cred, &ap,
					   W_OK);
		if (!status)
			resok->access |= ACCESS3_EXTEND;
	}

	if (args->access & ACCESS3_DELETE && (inode->i_mode & S_IFDIR)) {
		status = nfs3_access_check(inode, &rt->rt_info.ri_cred, &ap,
					   W_OK);
		if (!status)
			resok->access |= ACCESS3_DELETE;
	}

	if (args->access & ACCESS3_EXECUTE && !(inode->i_mode & S_IFDIR)) {
		status = nfs3_access_check(inode, &rt->rt_info.ri_cred, &ap,
					   X_OK);
		if (!status)
			resok->access |= ACCESS3_EXECUTE;
	}

	fa = &resok->obj_attributes.post_op_attr_u.attributes;
	resok->obj_attributes.attributes_follow = true;

	pthread_mutex_lock(&inode->i_attr_mutex); // Consider reader/writer?

	inode_attr_to_fattr(inode, fa);

	pthread_mutex_unlock(&inode->i_attr_mutex);

out:
	inode_put(inode);
	super_block_put(sb);
	return res->status;
}

static int nfs3_readlink(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	struct super_block *sb = NULL;
	struct inode *inode = NULL;

	READLINK3args *args = ph->ph_args;
	READLINK3res *res = ph->ph_res;
	READLINK3resok *resok = &res->READLINK3res_u.resok;

	post_op_attr *poa = &resok->symlink_attributes;
	fattr3 *fa;

	char *name;

	struct network_file_handle *nfh = NULL;

	struct authunix_parms ap;

	if (args->symlink.data.data_len != sizeof(*nfh)) {
		res->status = NFS3ERR_BADHANDLE;
		uint32_t crc = nfs3_getfh_crc(&args->symlink);
		TRACE("SYMLINK: xid=0x%08x badfh crc=0x%08x",
		      rt->rt_info.ri_xid, crc);
		goto out;
	}

	nfh = (struct network_file_handle *)args->symlink.data.data_val;
	TRACE("READLINK: xid=0x%08x sb=%lu ino=%lu", rt->rt_info.ri_xid,
	      nfh->nfh_sb, nfh->nfh_ino);

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		res->status = NFS3ERR_STALE;
		goto out;
	}

	inode = inode_find(sb, nfh->nfh_ino);
	if (!inode) {
		res->status = NFS3ERR_NOENT;
		goto out;
	}

	res->status = nfs3_access_check(inode, &rt->rt_info.ri_cred, &ap, R_OK);
	if (res->status)
		goto out;

	if (!(inode->i_mode & S_IFLNK)) {
		res->status = NFS3ERR_INVAL;
		goto out;
	}

	name = strdup(inode->i_symlink);
	if (!name) {
		res->status = NFS3ERR_JUKEBOX;
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

	print_nfs_fh3_hex(&args->symlink);

out:
	inode_put(inode);
	super_block_put(sb);
	return res->status;
}

static int nfs3_read(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	struct super_block *sb = NULL;
	struct inode *inode = NULL;

	READ3args *args = ph->ph_args;
	READ3res *res = ph->ph_res;
	READ3resok *resok = &res->READ3res_u.resok;

	post_op_attr *poa = &res->READ3res_u.resok.file_attributes;

	struct network_file_handle *nfh = NULL;
	struct authunix_parms ap;

	if (args->file.data.data_len != sizeof(*nfh)) {
		res->status = NFS3ERR_BADHANDLE;
		uint32_t crc = nfs3_getfh_crc(&args->file);
		TRACE("READ: xid=0x%08x badfh crc=0x%08x", rt->rt_info.ri_xid,
		      crc);
		goto out;
	}

	nfh = (struct network_file_handle *)args->file.data.data_val;
	TRACE("READ: xid=0x%08x sb=%lu ino=%lu", rt->rt_info.ri_xid,
	      nfh->nfh_sb, nfh->nfh_ino);

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		res->status = NFS3ERR_STALE;
		goto out;
	}

	inode = inode_find(sb, nfh->nfh_ino);
	if (!inode) {
		res->status = NFS3ERR_NOENT;
		goto out;
	}

	res->status = nfs3_access_check(inode, &rt->rt_info.ri_cred, &ap, R_OK);
	if (res->status)
		goto out;

	if (inode->i_mode & S_IFDIR) {
		res->status = NFS3ERR_ISDIR;
		goto out;
	}

	pthread_mutex_lock(&inode->i_attr_mutex);

	if (!inode->i_db) {
		resok->count = 0;
		resok->eof = true;
	} else {
		resok->data.data_val = calloc(args->count, sizeof(char));
		if (!resok->data.data_val) {
			res->status = NFS3ERR_JUKEBOX;
			poa = &res->READ3res_u.resfail.file_attributes;
			goto update_wcc;
		}

		resok->data.data_len = args->count;

		pthread_rwlock_rdlock(&inode->i_db_rwlock);
		res->status = data_block_read(inode->i_db, resok->data.data_val,
					      args->count, args->offset);
		if (!res->status && args->count) {
			res->status = EOVERFLOW;
			free(resok->data.data_val);
			resok->data.data_len = 0;
			poa = &res->READ3res_u.resfail.file_attributes;
			pthread_rwlock_unlock(&inode->i_db_rwlock);
			goto update_wcc;
		}

		resok->data.data_len = res->status;

		if (args->offset + args->count > inode->i_db->db_size)
			resok->eof = true;
		pthread_rwlock_unlock(&inode->i_db_rwlock);

		resok->count = resok->data.data_len;
	}

	inode_update_times_now(inode, REFFS_INODE_UPDATE_ATIME);

update_wcc:
	poa->attributes_follow = true;
	fattr3 *fa = &poa->post_op_attr_u.attributes;
	inode_attr_to_fattr(inode, fa);

	pthread_mutex_unlock(&inode->i_attr_mutex);

	print_nfs_fh3_hex(&args->file);

out:
	inode_put(inode);
	super_block_put(sb);
	return res->status;
}

static void timespec_to_writeverf3(struct timespec *ts, writeverf3 verf)
{
	// First 4 bytes for tv_sec, last 4 bytes for tv_nsec
	memcpy(verf, &ts->tv_sec, 4);
	memcpy(verf + 4, &ts->tv_nsec, 4);
}

static int nfs3_write(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	struct super_block *sb = NULL;
	struct inode *inode = NULL;

	WRITE3args *args = ph->ph_args;
	WRITE3res *res = ph->ph_res;
	WRITE3resok *resok = &res->WRITE3res_u.resok;

	wcc_data *wcc = &res->WRITE3res_u.resok.file_wcc;

	struct network_file_handle *nfh = NULL;
	struct authunix_parms ap;

	size3 size;
	nfstime3 mtime;
	nfstime3 ctime;

	if (args->file.data.data_len != sizeof(*nfh)) {
		res->status = NFS3ERR_BADHANDLE;
		uint32_t crc = nfs3_getfh_crc(&args->file);
		TRACE("WRITE: xid=0x%08x badfh crc=0x%08x", rt->rt_info.ri_xid,
		      crc);
		goto out;
	}

	nfh = (struct network_file_handle *)args->file.data.data_val;
	TRACE("WRITE: xid=0x%08x sb=%lu ino=%lu", rt->rt_info.ri_xid,
	      nfh->nfh_sb, nfh->nfh_ino);

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		res->status = NFS3ERR_STALE;
		goto out;
	}

	inode = inode_find(sb, nfh->nfh_ino);
	if (!inode) {
		res->status = NFS3ERR_NOENT;
		goto out;
	}

	res->status = nfs3_access_check(inode, &rt->rt_info.ri_cred, &ap, W_OK);
	if (res->status)
		goto out;

	if (inode->i_mode & S_IFDIR) {
		res->status = NFS3ERR_ISDIR;
		goto out;
	}

	pthread_mutex_lock(&inode->i_attr_mutex);

	size = inode->i_size;
	timespec_to_nfstime3(&inode->i_ctime, &ctime);
	timespec_to_nfstime3(&inode->i_mtime, &mtime);

	pthread_rwlock_wrlock(&inode->i_db_rwlock);
	if (!inode->i_db) {
		inode->i_db = data_block_alloc(
			args->data.data_val, args->data.data_len, args->offset);
		if (!inode->i_db) {
			res->status = NFS3ERR_NOSPC;
			wcc = &res->WRITE3res_u.resfail.file_wcc;
			pthread_rwlock_unlock(&inode->i_db_rwlock);
			goto update_wcc;
		}
	} else {
		res->status = data_block_write(inode->i_db, args->data.data_val,
					       args->data.data_len,
					       args->offset);
		if (res->status < 0) {
			res->status = -res->status;
			wcc = &res->WRITE3res_u.resfail.file_wcc;
			pthread_rwlock_unlock(&inode->i_db_rwlock);
			goto update_wcc;
		}

		resok->count = res->status;
	}

	/* For now, it is a RAM disk and all writes are done right away! */
	switch (args->stable) {
	case UNSTABLE:
	case DATA_SYNC:
	case FILE_SYNC:
		resok->committed = FILE_SYNC;
		break;
	};

	inode_update_times_now(inode, REFFS_INODE_UPDATE_CTIME |
					      REFFS_INODE_UPDATE_MTIME);

	timespec_to_writeverf3(&inode->i_ctime, resok->verf);

	inode->i_size = inode->i_db->db_size;
	inode->i_used = inode->i_size / 4096 + (inode->i_size % 4096 ? 1 : 0);
	pthread_rwlock_unlock(&inode->i_db_rwlock);

update_wcc:
	wcc->before.attributes_follow = true;
	wcc->before.pre_op_attr_u.attributes.size = size;
	wcc->before.pre_op_attr_u.attributes.mtime = mtime;
	wcc->before.pre_op_attr_u.attributes.ctime = ctime;
	wcc->after.attributes_follow = true;

	inode_attr_to_fattr(inode, &wcc->after.post_op_attr_u.attributes);

	pthread_mutex_unlock(&inode->i_attr_mutex);

	print_nfs_fh3_hex(&args->file);

out:
	inode_put(inode);
	super_block_put(sb);
	return res->status;
}

static void createverf3_to_timespec(createverf3 verf, struct timespec *ts)
{
	// First 4 bytes for tv_sec, last 4 bytes for tv_nsec
	memcpy(&ts->tv_sec, verf, 4);
	memcpy(&ts->tv_nsec, verf + 4, 4);
}

static void timespec_to_createverf3(struct timespec *ts, createverf3 verf)
{
	// First 4 bytes for tv_sec, last 4 bytes for tv_nsec
	memcpy(verf, &ts->tv_sec, 4);
	memcpy(verf + 4, &ts->tv_nsec, 4);
}

static int nfs3_create(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	struct super_block *sb = NULL;
	struct inode *inode = NULL;
	struct inode *exists = NULL;
	struct inode *tmp = NULL;

	CREATE3args *args = ph->ph_args;
	CREATE3res *res = ph->ph_res;
	CREATE3resok *resok = &res->CREATE3res_u.resok;
	CREATE3resfail *resfail = &res->CREATE3res_u.resfail;

	wcc_data *wcc = &resok->dir_wcc;
	fattr3 *fa;
	sattr3 *sa = &args->how.createhow3_u.obj_attributes;

	size3 size;
	nfstime3 mtime;
	nfstime3 ctime;

	uint64_t flags = 0;

	struct network_file_handle *nfh = NULL;

	struct dirent *de = NULL;
	struct authunix_parms ap;

	createverf3 cv;

	if (args->where.dir.data.data_len != sizeof(*nfh)) {
		res->status = NFS3ERR_BADHANDLE;
		uint32_t crc = nfs3_getfh_crc(&args->where.dir);
		TRACE("CREATE: xid=0x%08x badfh crc=0x%08x", rt->rt_info.ri_xid,
		      crc);
		goto out;
	}

	nfh = (struct network_file_handle *)args->where.dir.data.data_val;
	TRACE("CREATE: xid=0x%08x sb=%lu ino=%lu", rt->rt_info.ri_xid,
	      nfh->nfh_sb, nfh->nfh_ino);

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		res->status = NFS3ERR_STALE;
		goto out;
	}

	inode = inode_find(sb, nfh->nfh_ino);
	if (!inode) {
		res->status = NFS3ERR_NOENT;
		goto out;
	}

	res->status = nfs3_access_check(inode, &rt->rt_info.ri_cred, &ap, W_OK);
	if (res->status)
		goto out;

	if (!(inode->i_mode & S_IFDIR)) {
		res->status = NFS3ERR_NOTDIR;
		goto out;
	}

	pthread_mutex_lock(&inode->i_attr_mutex);

	size = inode->i_size;
	timespec_to_nfstime3(&inode->i_ctime, &ctime);
	timespec_to_nfstime3(&inode->i_mtime, &mtime);

	pthread_rwlock_wrlock(&inode->i_parent->d_rwlock);
	exists = inode_name_get_inode(inode, args->where.name);
	if (exists) {
		switch (args->how.mode) {
		case UNCHECKED:
			break;
		case GUARDED:
			res->status = NFS3ERR_EXIST;
			pthread_rwlock_unlock(&inode->i_parent->d_rwlock);
			wcc = &resfail->dir_wcc;
			goto update_wcc;
		case EXCLUSIVE:
			timespec_to_createverf3(&exists->i_ctime, cv);
			if (!memcmp(cv, args->how.createhow3_u.verf,
				    NFS3_CREATEVERFSIZE)) {
				res->status = NFS3ERR_EXIST;
				pthread_rwlock_unlock(
					&inode->i_parent->d_rwlock);
				wcc = &resfail->dir_wcc;
				goto update_wcc;
			}
			break;
		}

		res->status = nfs3_access_check(exists, &rt->rt_info.ri_cred,
						&ap, W_OK);
		if (res->status) {
			pthread_rwlock_unlock(&inode->i_parent->d_rwlock);
			wcc = &resfail->dir_wcc;
			goto update_wcc;
		}

		tmp = exists;
	} else {
		de = dirent_alloc(inode->i_parent, args->where.name,
				  reffs_life_action_birth);
		if (!de) {
			res->status = NFS3ERR_NOENT;
			pthread_rwlock_unlock(&inode->i_parent->d_rwlock);
			wcc = &resfail->dir_wcc;
			goto update_wcc;
		}

		de->d_inode =
			inode_alloc(sb, uatomic_add_return(&sb->sb_next_ino, 1,
							   __ATOMIC_RELAXED));
		if (!de->d_inode) {
			dirent_parent_release(de, reffs_life_action_death);
			res->status = NFS3ERR_NOENT;
			pthread_rwlock_unlock(&inode->i_parent->d_rwlock);
			wcc = &resfail->dir_wcc;
			goto update_wcc;
		}

		tmp = de->d_inode;

		tmp->i_size = 0;
		tmp->i_used = 0;
		tmp->i_nlink = 1;

		tmp->i_uid = ap.aup_uid;
		tmp->i_gid = ap.aup_gid;
		clock_gettime(CLOCK_REALTIME, &tmp->i_mtime);
		tmp->i_atime = tmp->i_mtime;
		tmp->i_btime = tmp->i_mtime;
		tmp->i_ctime = tmp->i_mtime;
		tmp->i_mode = (S_IFREG | inode->i_mode) & ~S_IFDIR;
	}
	pthread_rwlock_unlock(&inode->i_parent->d_rwlock);

	if (args->how.mode == EXCLUSIVE) {
		createverf3_to_timespec(args->how.createhow3_u.verf,
					&tmp->i_ctime);
	} else {
		res->status = nfs3_apply_sattr3(inode, sa, &flags);
		if (res->status) {
			wcc = &resfail->dir_wcc;
			goto update_wcc;
		}
	}

	if (flags)
		inode_update_times_now(inode, flags);
	nfh = calloc(1, sizeof(struct nfs_fh3));
	if (!nfh) {
		res->status = NFS3ERR_JUKEBOX;
		wcc = &resfail->dir_wcc;
		goto update_wcc;
	}

	resok->obj.post_op_fh3_u.handle.data.data_val = (char *)nfh;
	resok->obj.post_op_fh3_u.handle.data.data_len = sizeof(nfs_fh3);
	resok->obj.handle_follows = true;
	nfh->nfh_vers = FILEHANDLE_VERSION_CURR;
	nfh->nfh_sb = sb->sb_id;
	nfh->nfh_ino = tmp->i_ino;

	wcc->before.attributes_follow = true;
	wcc->before.pre_op_attr_u.attributes.size = size;
	wcc->before.pre_op_attr_u.attributes.mtime = mtime;
	wcc->before.pre_op_attr_u.attributes.ctime = ctime;

	resok->obj_attributes.attributes_follow = true;
	fa = &resok->obj_attributes.post_op_attr_u.attributes;

	inode_attr_to_fattr(tmp, fa);

update_wcc:
	wcc->after.attributes_follow = true;
	fa = &wcc->after.post_op_attr_u.attributes;
	inode_attr_to_fattr(inode, fa);

	pthread_mutex_unlock(&inode->i_attr_mutex);

	print_nfs_fh3_hex(&args->where.dir);

out:
	inode_put(exists);
	inode_put(inode);
	super_block_put(sb);
	return res->status;
}

static int nfs3_mkdir(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	struct super_block *sb = NULL;
	struct inode *inode = NULL;

	MKDIR3args *args = ph->ph_args;
	MKDIR3res *res = ph->ph_res;
	MKDIR3resok *resok = &res->MKDIR3res_u.resok;
	MKDIR3resfail *resfail = &res->MKDIR3res_u.resfail;

	wcc_data *wcc = &resok->dir_wcc;
	fattr3 *fa = &wcc->after.post_op_attr_u.attributes;

	size3 size;
	nfstime3 mtime;
	nfstime3 ctime;

	struct network_file_handle *nfh = NULL;

	struct dirent *de = NULL;
	struct authunix_parms ap;

	if (args->where.dir.data.data_len != sizeof(*nfh)) {
		res->status = NFS3ERR_BADHANDLE;
		uint32_t crc = nfs3_getfh_crc(&args->where.dir);
		TRACE("MKDIR: xid=0x%08x badfh crc=0x%08x", rt->rt_info.ri_xid,
		      crc);
		goto out;
	}

	nfh = (struct network_file_handle *)args->where.dir.data.data_val;
	TRACE("MKDIR: xid=0x%08x sb=%lu ino=%lu", rt->rt_info.ri_xid,
	      nfh->nfh_sb, nfh->nfh_ino);

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		res->status = NFS3ERR_STALE;
		goto out;
	}

	inode = inode_find(sb, nfh->nfh_ino);
	if (!inode) {
		res->status = NFS3ERR_NOENT;
		goto out;
	}

	res->status = nfs3_access_check(inode, &rt->rt_info.ri_cred, &ap, W_OK);
	if (res->status)
		goto out;

	if (!(inode->i_mode & S_IFDIR)) {
		res->status = NFS3ERR_NOTDIR;
		goto out;
	}

	pthread_mutex_lock(&inode->i_attr_mutex);

	size = inode->i_size;
	timespec_to_nfstime3(&inode->i_mtime, &mtime);
	timespec_to_nfstime3(&inode->i_ctime, &ctime);

	pthread_rwlock_wrlock(&inode->i_parent->d_rwlock);
	if (inode_name_is_child(inode, args->where.name)) {
		res->status = NFS3ERR_EXIST;
		wcc = &resfail->dir_wcc;
		pthread_rwlock_unlock(&inode->i_parent->d_rwlock);
		goto update_wcc;
	}

	de = dirent_alloc(inode->i_parent, args->where.name,
			  reffs_life_action_birth);
	if (!de) {
		res->status = NFS3ERR_NOENT;
		wcc = &resfail->dir_wcc;
		pthread_rwlock_unlock(&inode->i_parent->d_rwlock);
		goto update_wcc;
	}

	de->d_inode = inode_alloc(sb, uatomic_add_return(&sb->sb_next_ino, 1,
							 __ATOMIC_RELAXED));
	if (!de->d_inode) {
		dirent_parent_release(de, reffs_life_action_death);
		res->status = NFS3ERR_NOENT;
		wcc = &resfail->dir_wcc;
		pthread_rwlock_unlock(&inode->i_parent->d_rwlock);
		goto update_wcc;
	}
	pthread_rwlock_unlock(&inode->i_parent->d_rwlock);

	de->d_inode->i_uid = ap.aup_uid;
	de->d_inode->i_gid = ap.aup_gid;
	clock_gettime(CLOCK_REALTIME, &de->d_inode->i_mtime);
	de->d_inode->i_atime = de->d_inode->i_mtime;
	de->d_inode->i_btime = de->d_inode->i_mtime;
	de->d_inode->i_ctime = de->d_inode->i_mtime;
	de->d_inode->i_mode = S_IFDIR |
			      inode->i_mode; // Inherit from the parent!
	de->d_inode->i_size = 4096;
	de->d_inode->i_used = 8;
	de->d_inode->i_nlink = 2;

	nfh = calloc(1, sizeof(struct nfs_fh3));
	if (!nfh) {
		res->status = NFS3ERR_JUKEBOX;
		wcc = &resfail->dir_wcc;
		goto update_wcc;
	}

	resok->obj.post_op_fh3_u.handle.data.data_val = (char *)nfh;
	resok->obj.post_op_fh3_u.handle.data.data_len = sizeof(nfs_fh3);
	resok->obj.handle_follows = true;
	nfh->nfh_vers = FILEHANDLE_VERSION_CURR;
	nfh->nfh_sb = sb->sb_id;
	nfh->nfh_ino = de->d_inode->i_ino;

	wcc->before.attributes_follow = true;
	wcc->before.pre_op_attr_u.attributes.size = size;
	wcc->before.pre_op_attr_u.attributes.mtime = mtime;
	wcc->before.pre_op_attr_u.attributes.ctime = ctime;

	resok->obj_attributes.attributes_follow = true;
	fa = &resok->obj_attributes.post_op_attr_u.attributes;
	inode_attr_to_fattr(de->d_inode, fa);

update_wcc:
	wcc->after.attributes_follow = true;
	fa = &wcc->after.post_op_attr_u.attributes;
	inode_attr_to_fattr(inode, fa);

	pthread_mutex_unlock(&inode->i_attr_mutex);

	print_nfs_fh3_hex(&args->where.dir);

out:
	inode_put(inode);
	super_block_put(sb);
	return res->status;
}

static int nfs3_symlink(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	struct super_block *sb = NULL;
	struct inode *inode = NULL;

	SYMLINK3args *args = ph->ph_args;
	SYMLINK3res *res = ph->ph_res;
	SYMLINK3resok *resok = &res->SYMLINK3res_u.resok;
	SYMLINK3resfail *resfail = &res->SYMLINK3res_u.resfail;

	wcc_data *wcc = &resok->dir_wcc;
	fattr3 *fa;
	sattr3 *sa = &args->symlink.symlink_attributes;

	uint64_t flags = 0;

	size3 size;
	nfstime3 mtime;
	nfstime3 ctime;

	char *name = NULL;

	struct network_file_handle *nfh = NULL;

	struct dirent *de = NULL;
	struct authunix_parms ap;

	if (args->where.dir.data.data_len != sizeof(*nfh)) {
		res->status = NFS3ERR_BADHANDLE;
		uint32_t crc = nfs3_getfh_crc(&args->where.dir);
		TRACE("SYMLINK: xid=0x%08x badfh crc=0x%08x",
		      rt->rt_info.ri_xid, crc);
		goto out;
	}

	nfh = (struct network_file_handle *)args->where.dir.data.data_val;
	TRACE("SYMLINK: xid=0x%08x sb=%lu ino=%lu", rt->rt_info.ri_xid,
	      nfh->nfh_sb, nfh->nfh_ino);

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		res->status = NFS3ERR_STALE;
		goto out;
	}

	inode = inode_find(sb, nfh->nfh_ino);
	if (!inode) {
		res->status = NFS3ERR_NOENT;
		goto out;
	}

	res->status = nfs3_access_check(inode, &rt->rt_info.ri_cred, &ap, W_OK);
	if (res->status)
		goto out;

	if (!(inode->i_mode & S_IFDIR)) {
		res->status = NFS3ERR_NOTDIR;
		goto out;
	}

	name = strdup(args->symlink.symlink_data);
	if (!name) {
		res->status = NFS3ERR_JUKEBOX;
		goto out;
	}

	pthread_mutex_lock(&inode->i_attr_mutex);

	size = inode->i_size;
	timespec_to_nfstime3(&inode->i_mtime, &mtime);
	timespec_to_nfstime3(&inode->i_ctime, &ctime);

	pthread_rwlock_wrlock(&inode->i_parent->d_rwlock);
	if (inode_name_is_child(inode, args->where.name)) {
		res->status = NFS3ERR_EXIST;
		wcc = &resfail->dir_wcc;
		pthread_rwlock_unlock(&inode->i_parent->d_rwlock);
		goto update_wcc;
	}

	de = dirent_alloc(inode->i_parent, args->where.name,
			  reffs_life_action_birth);
	if (!de) {
		res->status = NFS3ERR_NOENT;
		wcc = &resfail->dir_wcc;
		pthread_rwlock_unlock(&inode->i_parent->d_rwlock);
		goto update_wcc;
	}

	de->d_inode = inode_alloc(sb, uatomic_add_return(&sb->sb_next_ino, 1,
							 __ATOMIC_RELAXED));
	if (!de->d_inode) {
		dirent_parent_release(de, reffs_life_action_death);
		res->status = NFS3ERR_NOENT;
		wcc = &resfail->dir_wcc;
		pthread_rwlock_unlock(&inode->i_parent->d_rwlock);
		goto update_wcc;
	}
	pthread_rwlock_unlock(&inode->i_parent->d_rwlock);

	de->d_inode->i_uid = ap.aup_uid;
	de->d_inode->i_gid = ap.aup_gid;
	clock_gettime(CLOCK_REALTIME, &de->d_inode->i_mtime);
	de->d_inode->i_atime = de->d_inode->i_mtime;
	de->d_inode->i_btime = de->d_inode->i_mtime;
	de->d_inode->i_ctime = de->d_inode->i_mtime;
	de->d_inode->i_mode = (S_IFLNK | inode->i_mode) & ~S_IFDIR;
	de->d_inode->i_size = 4096;
	de->d_inode->i_used = 8;
	de->d_inode->i_nlink = 2;

	nfh = calloc(1, sizeof(struct nfs_fh3));
	if (!nfh) {
		res->status = NFS3ERR_JUKEBOX;
		wcc = &resfail->dir_wcc;
		goto update_wcc;
	}

	res->status = nfs3_apply_sattr3(inode, sa, &flags);
	if (res->status) {
		wcc = &resfail->dir_wcc;
		goto update_wcc;
	}

	de->d_inode->i_symlink = name;
	name = NULL;

	resok->obj.post_op_fh3_u.handle.data.data_val = (char *)nfh;
	resok->obj.post_op_fh3_u.handle.data.data_len = sizeof(nfs_fh3);
	resok->obj.handle_follows = true;
	nfh->nfh_vers = FILEHANDLE_VERSION_CURR;
	nfh->nfh_sb = sb->sb_id;
	nfh->nfh_ino = de->d_inode->i_ino;

	wcc->before.attributes_follow = true;
	wcc->before.pre_op_attr_u.attributes.size = size;
	wcc->before.pre_op_attr_u.attributes.mtime = mtime;
	wcc->before.pre_op_attr_u.attributes.ctime = ctime;

	resok->obj_attributes.attributes_follow = true;
	fa = &resok->obj_attributes.post_op_attr_u.attributes;
	inode_attr_to_fattr(de->d_inode, fa);

update_wcc:
	wcc->after.attributes_follow = true;
	fa = &wcc->after.post_op_attr_u.attributes;
	inode_attr_to_fattr(inode, fa);

	pthread_mutex_unlock(&inode->i_attr_mutex);

	print_nfs_fh3_hex(&args->where.dir);

out:
	free(name);
	inode_put(inode);
	super_block_put(sb);
	return res->status;
}

static int nfs3_mknod(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	struct super_block *sb = NULL;
	struct inode *inode = NULL;
	struct inode *tmp = NULL;

	MKNOD3args *args = ph->ph_args;
	MKNOD3res *res = ph->ph_res;
	MKNOD3resok *resok = &res->MKNOD3res_u.resok;

	wcc_data *wcc = &resok->dir_wcc;
	fattr3 *fa = &wcc->after.post_op_attr_u.attributes;
	sattr3 *sa;

	size3 size;
	nfstime3 mtime;
	nfstime3 ctime;

	uint64_t flags = 0;

	struct network_file_handle *nfh = NULL;

	struct dirent *de = NULL;
	struct authunix_parms ap;

	if (args->where.dir.data.data_len != sizeof(*nfh)) {
		res->status = NFS3ERR_BADHANDLE;
		uint32_t crc = nfs3_getfh_crc(&args->where.dir);
		TRACE("MKNOD: xid=0x%08x badfh crc=0x%08x", rt->rt_info.ri_xid,
		      crc);
		goto out;
	}

	nfh = (struct network_file_handle *)args->where.dir.data.data_val;
	TRACE("MKNOD: xid=0x%08x sb=%lu ino=%lu", rt->rt_info.ri_xid,
	      nfh->nfh_sb, nfh->nfh_ino);

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		res->status = NFS3ERR_STALE;
		goto out;
	}

	inode = inode_find(sb, nfh->nfh_ino);
	if (!inode) {
		res->status = NFS3ERR_NOENT;
		goto out;
	}

	res->status = nfs3_access_check(inode, &rt->rt_info.ri_cred, &ap, W_OK);
	if (res->status)
		goto out;

	if (!(inode->i_mode & S_IFDIR)) {
		res->status = NFS3ERR_NOTDIR;
		goto out;
	}

	switch (args->what.type) {
	case NF3REG:
	case NF3DIR:
	case NF3LNK:
		res->status = NFS3ERR_BADTYPE;
		goto out;
	case NF3BLK:
	case NF3CHR:
	case NF3SOCK:
	case NF3FIFO:
		break;
	}

	pthread_mutex_lock(&inode->i_attr_mutex);

	size = inode->i_size;
	timespec_to_nfstime3(&inode->i_mtime, &mtime);
	timespec_to_nfstime3(&inode->i_ctime, &ctime);

	pthread_rwlock_wrlock(&inode->i_parent->d_rwlock);
	if (inode_name_is_child(inode, args->where.name)) {
		res->status = NFS3ERR_EXIST;
		wcc = &res->MKNOD3res_u.resfail.dir_wcc;
		fa = &wcc->after.post_op_attr_u.attributes;
		pthread_rwlock_unlock(&inode->i_parent->d_rwlock);
		goto update_wcc;
	}

	de = dirent_alloc(inode->i_parent, args->where.name,
			  reffs_life_action_birth);
	if (!de) {
		res->status = NFS3ERR_NOENT;
		wcc = &res->MKNOD3res_u.resfail.dir_wcc;
		fa = &wcc->after.post_op_attr_u.attributes;
		pthread_rwlock_unlock(&inode->i_parent->d_rwlock);
		goto update_wcc;
	}

	de->d_inode = inode_alloc(sb, uatomic_add_return(&sb->sb_next_ino, 1,
							 __ATOMIC_RELAXED));
	if (!de->d_inode) {
		dirent_parent_release(de, reffs_life_action_death);
		res->status = NFS3ERR_NOENT;
		wcc = &res->MKNOD3res_u.resfail.dir_wcc;
		fa = &wcc->after.post_op_attr_u.attributes;
		pthread_rwlock_unlock(&inode->i_parent->d_rwlock);
		goto update_wcc;
	}
	pthread_rwlock_unlock(&inode->i_parent->d_rwlock);

	de->d_inode->i_uid = ap.aup_uid;
	de->d_inode->i_gid = ap.aup_gid;
	clock_gettime(CLOCK_REALTIME, &de->d_inode->i_mtime);
	de->d_inode->i_atime = de->d_inode->i_mtime;
	de->d_inode->i_btime = de->d_inode->i_mtime;
	de->d_inode->i_ctime = de->d_inode->i_mtime;
	de->d_inode->i_mode = inode->i_mode &
			      ~S_IFDIR; // Inherit from the parent!
	de->d_inode->i_size = 4096;
	de->d_inode->i_used = 8;
	de->d_inode->i_nlink = 2;

	switch (args->what.type) {
	case NF3REG:
	case NF3DIR:
	case NF3LNK:
		verify_msg(0, "Type changed: %u", args->what.type);
		goto out;
	case NF3BLK:
		sa = &args->what.mknoddata3_u.device.dev_attributes;
		de->d_inode->i_dev_major =
			args->what.mknoddata3_u.device.spec.specdata1;
		de->d_inode->i_dev_minor =
			args->what.mknoddata3_u.device.spec.specdata2;
		de->d_inode->i_mode |= S_IFBLK;
		break;
	case NF3CHR:
		sa = &args->what.mknoddata3_u.device.dev_attributes;
		de->d_inode->i_dev_major =
			args->what.mknoddata3_u.device.spec.specdata1;
		de->d_inode->i_dev_minor =
			args->what.mknoddata3_u.device.spec.specdata2;
		de->d_inode->i_mode |= S_IFCHR;
		break;
	case NF3SOCK:
		sa = &args->what.mknoddata3_u.pipe_attributes;
		de->d_inode->i_mode |= S_IFSOCK;
		break;
	case NF3FIFO:
		sa = &args->what.mknoddata3_u.pipe_attributes;
		de->d_inode->i_mode |= S_IFIFO;
		break;
	}

	res->status = nfs3_apply_sattr3(de->d_inode, sa, &flags);
	if (res->status) {
		wcc = &res->MKNOD3res_u.resfail.dir_wcc;
		fa = &wcc->after.post_op_attr_u.attributes;
		goto update_wcc;
	}

	nfh = calloc(1, sizeof(struct nfs_fh3));
	if (!nfh) {
		res->status = NFS3ERR_JUKEBOX;
		wcc = &res->MKNOD3res_u.resfail.dir_wcc;
		fa = &wcc->after.post_op_attr_u.attributes;
		goto update_wcc;
	}

	resok->obj.post_op_fh3_u.handle.data.data_val = (char *)nfh;
	resok->obj.post_op_fh3_u.handle.data.data_len = sizeof(nfs_fh3);
	resok->obj.handle_follows = true;
	nfh->nfh_vers = FILEHANDLE_VERSION_CURR;
	nfh->nfh_sb = sb->sb_id;
	nfh->nfh_ino = de->d_inode->i_ino;

	inode_update_times_now(inode, REFFS_INODE_UPDATE_CTIME |
					      REFFS_INODE_UPDATE_MTIME);

	wcc->before.attributes_follow = true;
	wcc->before.pre_op_attr_u.attributes.size = size;
	wcc->before.pre_op_attr_u.attributes.mtime = mtime;
	wcc->before.pre_op_attr_u.attributes.ctime = ctime;

	resok->obj_attributes.attributes_follow = true;
	fa = &resok->obj_attributes.post_op_attr_u.attributes;

	inode_attr_to_fattr(tmp, fa);

update_wcc:
	wcc->after.attributes_follow = true;
	fa = &wcc->after.post_op_attr_u.attributes;
	inode_attr_to_fattr(inode, fa);

	pthread_mutex_unlock(&inode->i_attr_mutex);

	print_nfs_fh3_hex(&args->where.dir);

out:
	inode_put(inode);
	super_block_put(sb);
	return res->status;
}

static int nfs3_remove(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	struct super_block *sb = NULL;
	struct inode *inode = NULL;
	struct dirent *de = NULL;

	REMOVE3args *args = ph->ph_args;
	REMOVE3res *res = ph->ph_res;
	REMOVE3resok *resok = &res->REMOVE3res_u.resok;
	wcc_data *wcc = &resok->dir_wcc;
	fattr3 *fa;

	struct network_file_handle *nfh = NULL;

	struct authunix_parms ap;

	size3 size;
	nfstime3 mtime;
	nfstime3 ctime;

	if (args->object.dir.data.data_len != sizeof(*nfh)) {
		res->status = NFS3ERR_BADHANDLE;
		uint32_t crc = nfs3_getfh_crc(&args->object.dir);
		TRACE("REMOVE: xid=0x%08x badfh crc=0x%08x", rt->rt_info.ri_xid,
		      crc);
		goto out;
	}

	nfh = (struct network_file_handle *)args->object.dir.data.data_val;
	TRACE("REMOVE: xid=0x%08x sb=%lu ino=%lu", rt->rt_info.ri_xid,
	      nfh->nfh_sb, nfh->nfh_ino);

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		res->status = NFS3ERR_STALE;
		goto out;
	}

	inode = inode_find(sb, nfh->nfh_ino);
	if (!inode) {
		res->status = NFS3ERR_NOENT;
		goto out;
	}

	res->status = nfs3_access_check(inode, &rt->rt_info.ri_cred, &ap, W_OK);
	if (res->status)
		goto out;

	if (!(inode->i_mode & S_IFDIR)) {
		res->status = NFS3ERR_NOTDIR;
		goto out;
	}

	pthread_rwlock_wrlock(&inode->i_parent->d_rwlock);

	size = inode->i_size;
	timespec_to_nfstime3(&inode->i_ctime, &ctime);
	timespec_to_nfstime3(&inode->i_mtime, &mtime);

	pthread_mutex_lock(&inode->i_attr_mutex);
	de = dirent_find(inode->i_parent, reffs_case_get(), args->object.name);
	if (!de) {
		res->status = NFS3ERR_NOENT;
		wcc = &res->REMOVE3res_u.resfail.dir_wcc;
		pthread_rwlock_unlock(&inode->i_parent->d_rwlock);
		goto update_wcc;
	}

	dirent_parent_release(de, reffs_life_action_death);
	pthread_rwlock_unlock(&inode->i_parent->d_rwlock);

update_wcc:
	wcc->before.attributes_follow = true;
	wcc->before.pre_op_attr_u.attributes.size = size;
	wcc->before.pre_op_attr_u.attributes.mtime = mtime;
	wcc->before.pre_op_attr_u.attributes.ctime = ctime;

	wcc->after.attributes_follow = true;
	fa = &wcc->after.post_op_attr_u.attributes;

	inode_attr_to_fattr(inode, fa);

	pthread_mutex_unlock(&inode->i_attr_mutex);

	print_nfs_fh3_hex(&args->object.dir);

out:
	dirent_put(de);
	inode_put(inode);
	super_block_put(sb);
	return res->status;
}

static int nfs3_rmdir(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	struct super_block *sb = NULL;
	struct inode *inode = NULL;
	struct inode *exists = NULL;
	struct dirent *de = NULL;

	RMDIR3args *args = ph->ph_args;
	RMDIR3res *res = ph->ph_res;
	RMDIR3resok *resok = &res->RMDIR3res_u.resok;
	wcc_data *wcc = &resok->dir_wcc;
	fattr3 *fa;

	struct network_file_handle *nfh = NULL;

	struct authunix_parms ap;

	size3 size;
	nfstime3 mtime;
	nfstime3 ctime;

	if (args->object.dir.data.data_len != sizeof(*nfh)) {
		res->status = NFS3ERR_BADHANDLE;
		uint32_t crc = nfs3_getfh_crc(&args->object.dir);
		TRACE("RMDIR: xid=0x%08x badfh crc=0x%08x", rt->rt_info.ri_xid,
		      crc);
		goto out;
	}

	nfh = (struct network_file_handle *)args->object.dir.data.data_val;
	TRACE("RMDIR: xid=0x%08x sb=%lu ino=%lu", rt->rt_info.ri_xid,
	      nfh->nfh_sb, nfh->nfh_ino);

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		res->status = NFS3ERR_STALE;
		goto out;
	}

	inode = inode_find(sb, nfh->nfh_ino);
	if (!inode) {
		res->status = NFS3ERR_NOENT;
		goto out;
	}

	res->status = nfs3_access_check(inode, &rt->rt_info.ri_cred, &ap, W_OK);
	if (res->status)
		goto out;

	if (!(inode->i_mode & S_IFDIR)) {
		res->status = NFS3ERR_NOTDIR;
		goto out;
	}

	pthread_mutex_lock(&inode->i_attr_mutex);

	size = inode->i_size;
	timespec_to_nfstime3(&inode->i_ctime, &ctime);
	timespec_to_nfstime3(&inode->i_mtime, &mtime);

	pthread_rwlock_wrlock(&inode->i_parent->d_rwlock);
	exists = inode_name_get_inode(inode, args->object.name);
	if (!exists) {
		res->status = NFS3ERR_NOENT;
		wcc = &res->RMDIR3res_u.resfail.dir_wcc;
		pthread_rwlock_unlock(&inode->i_parent->d_rwlock);
		goto update_wcc;
	}

	if (!(exists->i_mode & S_IFDIR)) {
		res->status = NFS3ERR_NOTDIR;
		wcc = &res->RMDIR3res_u.resfail.dir_wcc;
		pthread_rwlock_unlock(&inode->i_parent->d_rwlock);
		goto update_wcc;
	}

	de = dirent_find(inode->i_parent, reffs_case_get(), args->object.name);
	if (!de) {
		res->status = NFS3ERR_NOENT;
		wcc = &res->RMDIR3res_u.resfail.dir_wcc;
		pthread_rwlock_unlock(&inode->i_parent->d_rwlock);
		goto update_wcc;
	}

	dirent_parent_release(de, reffs_life_action_death);
	pthread_rwlock_unlock(&inode->i_parent->d_rwlock);

update_wcc:
	wcc->before.attributes_follow = true;
	wcc->before.pre_op_attr_u.attributes.size = size;
	wcc->before.pre_op_attr_u.attributes.mtime = mtime;
	wcc->before.pre_op_attr_u.attributes.ctime = ctime;

	wcc->after.attributes_follow = true;
	fa = &wcc->after.post_op_attr_u.attributes;

	inode_attr_to_fattr(inode, fa);

	pthread_mutex_unlock(&inode->i_attr_mutex);

	print_nfs_fh3_hex(&args->object.dir);

out:
	dirent_put(de);
	inode_put(exists);
	inode_put(inode);
	super_block_put(sb);
	return res->status;
}

static int nfs3_rename(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	struct super_block *sb = NULL;

	struct inode *inode_src = NULL;
	struct inode *inode_dst = NULL;

	struct dirent *de_src = NULL;
	struct dirent *de_dst = NULL;

	RENAME3args *args = ph->ph_args;
	RENAME3res *res = ph->ph_res;
	RENAME3resok *resok = &res->RENAME3res_u.resok;
	wcc_data *wcc_src = &resok->fromdir_wcc;
	wcc_data *wcc_dst = &resok->todir_wcc;
	fattr3 *fa;

	char *old;

	struct network_file_handle *nfh_src = NULL;
	struct network_file_handle *nfh_dst = NULL;

	struct authunix_parms ap;

	size3 size_src;
	nfstime3 mtime_src;
	nfstime3 ctime_src;

	size3 size_dst;
	nfstime3 mtime_dst;
	nfstime3 ctime_dst;

	reffs_strng_compare cmp = reffs_text_case_cmp();
	enum reffs_text_case rtc = reffs_case_get();

	char *name = NULL;

	if (args->from.dir.data.data_len != sizeof(*nfh_src)) {
		res->status = NFS3ERR_BADHANDLE;
		uint32_t crc_src = nfs3_getfh_crc(&args->from.dir);
		uint32_t crc_to = nfs3_getfh_crc(&args->to.dir);
		TRACE("RENAME: xid=0x%08x badfh crc_src=0x%08x crc_to=0x%08x",
		      rt->rt_info.ri_xid, crc_src, crc_to);
		goto out;
	}

	nfh_src = (struct network_file_handle *)args->from.dir.data.data_val;

	if (args->to.dir.data.data_len != sizeof(*nfh_dst)) {
		res->status = NFS3ERR_BADHANDLE;
		uint32_t crc_src = nfs3_getfh_crc(&args->from.dir);
		uint32_t crc_to = nfs3_getfh_crc(&args->to.dir);
		TRACE("RENAME: xid=0x%08x badfh crc_src=0x%08x crc_to=0x%08x",
		      rt->rt_info.ri_xid, crc_src, crc_to);
		goto out;
	}

	nfh_dst = (struct network_file_handle *)args->to.dir.data.data_val;
	TRACE("RENAME: xid=0x%08x sb_src=%lu ino_src=%lu sb_dst=%lu ino_dst=%lu",
	      rt->rt_info.ri_xid, nfh_src->nfh_sb, nfh_src->nfh_ino,
	      nfh_dst->nfh_sb, nfh_dst->nfh_ino);

	if (nfh_src->nfh_sb != nfh_dst->nfh_sb) {
		res->status = NFS3ERR_XDEV;
		goto out;
	}

	sb = super_block_find(nfh_src->nfh_sb);
	if (!sb) {
		res->status = NFS3ERR_STALE;
		goto out;
	}

	inode_src = inode_find(sb, nfh_src->nfh_ino);
	if (!inode_src) {
		res->status = NFS3ERR_NOENT;
		goto out;
	}

	inode_dst = inode_find(sb, nfh_dst->nfh_ino);
	if (!inode_dst) {
		res->status = NFS3ERR_NOENT;
		goto out;
	}

	res->status =
		nfs3_access_check(inode_src, &rt->rt_info.ri_cred, &ap, W_OK);
	if (res->status)
		goto out;

	res->status =
		nfs3_access_check(inode_dst, &rt->rt_info.ri_cred, &ap, W_OK);
	if (res->status)
		goto out;

	if (!(inode_src->i_mode & S_IFDIR)) {
		res->status = NFS3ERR_NOTDIR;
		goto out;
	}

	if (!(inode_dst->i_mode & S_IFDIR)) {
		res->status = NFS3ERR_NOTDIR;
		goto out;
	}

	/*
	 * Note: Hold the d_rwlocks longer than we need to!
	 */
	if (inode_src == inode_dst) {
		pthread_mutex_lock(&inode_src->i_attr_mutex);
		pthread_rwlock_wrlock(&inode_src->i_parent->d_rwlock);
	} else {
		pthread_mutex_lock(&inode_src->i_attr_mutex);
		pthread_rwlock_wrlock(&inode_src->i_parent->d_rwlock);
		pthread_mutex_lock(&inode_dst->i_attr_mutex);
		pthread_rwlock_wrlock(&inode_dst->i_parent->d_rwlock);
	}

	size_src = inode_src->i_size;
	timespec_to_nfstime3(&inode_src->i_ctime, &ctime_src);
	timespec_to_nfstime3(&inode_src->i_mtime, &mtime_src);

	size_dst = inode_dst->i_size;
	timespec_to_nfstime3(&inode_dst->i_ctime, &ctime_dst);
	timespec_to_nfstime3(&inode_dst->i_mtime, &mtime_dst);

	// Do nothing as they are the same name
	if (network_file_handles_equal(nfh_src, nfh_dst) &&
	    !cmp(args->from.name, args->to.name)) {
		goto update_wcc;
	}

	de_src = dirent_find(inode_src->i_parent, rtc, args->from.name);
	if (!de_src) {
		res->status = NFS3ERR_NOENT;
		wcc_src = &res->RENAME3res_u.resfail.fromdir_wcc;
		wcc_dst = &res->RENAME3res_u.resfail.todir_wcc;
		goto update_wcc;
	}

	de_dst = dirent_find(inode_dst->i_parent, rtc, args->to.name);

	name = strdup(args->to.name);
	if (!name) {
		res->status = NFS3ERR_JUKEBOX;
		wcc_src = &res->RENAME3res_u.resfail.fromdir_wcc;
		wcc_dst = &res->RENAME3res_u.resfail.todir_wcc;
		goto update_wcc;
	}

	rcu_read_lock();
	old = rcu_xchg_pointer(&de_src->d_name, name);
	reffs_string_release(old);

	if (inode_src->i_parent == inode_dst->i_parent) {
		if (de_dst) {
			// Rename over file in same dir
			dirent_parent_release(de_dst, reffs_life_action_death);
		}
	} else {
		if (de_dst) {
			// Rename over file in different dir
			dirent_parent_release(de_dst, reffs_life_action_death);
		}

		dirent_parent_release(de_src, reffs_life_action_update);
		dirent_parent_attach(de_src, inode_dst->i_parent,
				     reffs_life_action_update);
	}

	inode_update_times_now(inode_src, REFFS_INODE_UPDATE_CTIME |
						  REFFS_INODE_UPDATE_MTIME);
	rcu_read_unlock();

update_wcc:
	wcc_src->before.attributes_follow = true;
	wcc_src->before.pre_op_attr_u.attributes.size = size_src;
	wcc_src->before.pre_op_attr_u.attributes.mtime = mtime_src;
	wcc_src->before.pre_op_attr_u.attributes.ctime = ctime_src;

	wcc_src->after.attributes_follow = true;
	fa = &wcc_src->after.post_op_attr_u.attributes;

	inode_attr_to_fattr(inode_src, fa);

	wcc_dst->before.attributes_follow = true;
	wcc_dst->before.pre_op_attr_u.attributes.size = size_dst;
	wcc_dst->before.pre_op_attr_u.attributes.mtime = mtime_dst;
	wcc_dst->before.pre_op_attr_u.attributes.ctime = ctime_dst;

	wcc_dst->after.attributes_follow = true;
	fa = &wcc_dst->after.post_op_attr_u.attributes;

	inode_attr_to_fattr(inode_src, fa);

	if (inode_src == inode_dst) {
		pthread_rwlock_unlock(&inode_src->i_parent->d_rwlock);
		pthread_mutex_unlock(&inode_src->i_attr_mutex);
	} else {
		pthread_rwlock_unlock(&inode_dst->i_parent->d_rwlock);
		pthread_mutex_unlock(&inode_dst->i_attr_mutex);
		pthread_rwlock_unlock(&inode_src->i_parent->d_rwlock);
		pthread_mutex_unlock(&inode_src->i_attr_mutex);
	}

	print_nfs_fh3_hex(&args->from.dir);
	print_nfs_fh3_hex(&args->to.dir);

out:
	dirent_put(de_dst);
	inode_put(inode_dst);
	dirent_put(de_src);
	inode_put(inode_src);
	super_block_put(sb);
	return res->status;
}

static int nfs3_link(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	struct super_block *sb = NULL;
	struct inode *inode = NULL;
	struct inode *inode_dir = NULL;
	struct inode *exists = NULL;

	LINK3args *args = ph->ph_args;
	LINK3res *res = ph->ph_res;
	LINK3resok *resok = &res->LINK3res_u.resok;
	LINK3resfail *resfail = &res->LINK3res_u.resfail;

	wcc_data *wcc = &resok->linkdir_wcc;
	post_op_attr *poa = &resok->file_attributes;
	fattr3 *fa;

	size3 size;
	nfstime3 mtime;
	nfstime3 ctime;

	struct network_file_handle *nfh = NULL;
	struct network_file_handle *nfh_dir = NULL;

	struct dirent *de = NULL;
	struct authunix_parms ap;

	if (args->file.data.data_len != sizeof(*nfh)) {
		res->status = NFS3ERR_BADHANDLE;
		uint32_t crc = nfs3_getfh_crc(&args->file);
		TRACE("LINK: xid=0x%08x badfh crc=0x%08x", rt->rt_info.ri_xid,
		      crc);
		goto out;
	}

	if (args->link.dir.data.data_len != sizeof(*nfh)) {
		res->status = NFS3ERR_BADHANDLE;
		uint32_t crc = nfs3_getfh_crc(&args->link.dir);
		TRACE("LINK: xid=0x%08x badfh crc=0x%08x", rt->rt_info.ri_xid,
		      crc);
		goto out;
	}

	nfh = (struct network_file_handle *)args->file.data.data_val;
	nfh_dir = (struct network_file_handle *)args->link.dir.data.data_val;

	TRACE("LINK: xid=0x%08x sb=%lu ino=%lu", rt->rt_info.ri_xid,
	      nfh->nfh_sb, nfh->nfh_ino);

	if (nfh->nfh_sb != nfh_dir->nfh_sb) {
		res->status = NFS3ERR_XDEV;
		goto out;
	}

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		res->status = NFS3ERR_STALE;
		goto out;
	}

	inode = inode_find(sb, nfh->nfh_ino);
	if (!inode) {
		res->status = NFS3ERR_NOENT;
		goto out;
	}

	inode_dir = inode_find(sb, nfh_dir->nfh_ino);
	if (!inode_dir) {
		res->status = NFS3ERR_NOENT;
		goto out;
	}

	res->status = nfs3_access_check(inode, &rt->rt_info.ri_cred, &ap, R_OK);
	if (res->status)
		goto out;

	if (!(inode->i_mode & S_IFREG)) {
		res->status = NFS3ERR_INVAL;
		goto out;
	}

	res->status =
		nfs3_access_check(inode_dir, &rt->rt_info.ri_cred, &ap, W_OK);
	if (res->status)
		goto out;

	if (!(inode->i_mode & S_IFDIR)) {
		res->status = NFS3ERR_NOTDIR;
		goto out;
	}

	pthread_mutex_lock(&inode_dir->i_attr_mutex);
	pthread_mutex_lock(&inode->i_attr_mutex);

	size = inode_dir->i_size;
	timespec_to_nfstime3(&inode_dir->i_ctime, &ctime);
	timespec_to_nfstime3(&inode_dir->i_mtime, &mtime);

	pthread_rwlock_wrlock(&inode_dir->i_parent->d_rwlock);
	exists = inode_name_get_inode(inode_dir, args->link.name);
	if (exists) {
		res->status = NFS3ERR_EXIST;
		pthread_rwlock_unlock(&inode_dir->i_parent->d_rwlock);
		wcc = &resfail->linkdir_wcc;
		poa = &resfail->file_attributes;
		goto update_wcc;
	}

	de = dirent_alloc(inode_dir->i_parent, args->link.name,
			  reffs_life_action_birth);
	if (!de) {
		res->status = NFS3ERR_NOENT;
		pthread_rwlock_unlock(&inode_dir->i_parent->d_rwlock);
		wcc = &resfail->linkdir_wcc;
		poa = &resfail->file_attributes;
		goto update_wcc;
	}

	de->d_inode = inode_get(inode);
	if (!de->d_inode) {
		dirent_parent_release(de, reffs_life_action_death);
		res->status = NFS3ERR_NOENT;
		pthread_rwlock_unlock(&inode->i_parent->d_rwlock);
		wcc = &resfail->linkdir_wcc;
		poa = &resfail->file_attributes;
		goto update_wcc;
	}

	uatomic_inc(&inode->i_nlink, __ATOMIC_RELAXED);

	pthread_rwlock_unlock(&inode_dir->i_parent->d_rwlock);

	inode_update_times_now(inode, REFFS_INODE_UPDATE_CTIME);

update_wcc:
	wcc->before.attributes_follow = true;
	wcc->before.pre_op_attr_u.attributes.size = size;
	wcc->before.pre_op_attr_u.attributes.mtime = mtime;
	wcc->before.pre_op_attr_u.attributes.ctime = ctime;

	poa->attributes_follow = true;
	fa = &poa->post_op_attr_u.attributes;

	inode_attr_to_fattr(inode_dir, fa);

	wcc->after.attributes_follow = true;
	fa = &wcc->after.post_op_attr_u.attributes;
	inode_attr_to_fattr(inode, fa);

	pthread_mutex_unlock(&inode->i_attr_mutex);
	pthread_mutex_unlock(&inode_dir->i_attr_mutex);

	print_nfs_fh3_hex(&args->file);

out:
	inode_put(exists);
	inode_put(inode);
	inode_put(inode_dir);
	super_block_put(sb);
	return res->status;
}

static int nfs3_readdir(struct rpc_trans *rt)
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

	struct dirent *de = NULL;
	struct authunix_parms ap;

	entry3 *e_next;

	if (args->dir.data.data_len != sizeof(*nfh)) {
		res->status = NFS3ERR_BADHANDLE;
		uint32_t crc = nfs3_getfh_crc(&args->dir);
		TRACE("READDIR: xid=0x%08x badfh crc=0x%08x",
		      rt->rt_info.ri_xid, crc);
		goto out;
	}

	nfh = (struct network_file_handle *)args->dir.data.data_val;
	TRACE("READDIR: xid=0x%08x sb=%lu ino=%lu cookie=0x%08lx",
	      rt->rt_info.ri_xid, nfh->nfh_sb, nfh->nfh_ino, args->cookie);

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		res->status = NFS3ERR_STALE;
		goto out;
	}

	inode = inode_find(sb, nfh->nfh_ino);
	if (!inode) {
		res->status = NFS3ERR_NOENT;
		goto out;
	}

	res->status = nfs3_access_check(inode, &rt->rt_info.ri_cred, &ap, R_OK);
	if (res->status)
		goto out;

	if (!(inode->i_mode & S_IFDIR)) {
		res->status = NFS3ERR_NOTDIR;
		goto out;
	}

	count = sizeof(READDIR3res);
	if (count > args->count) {
		res->status = NFS3ERR_INVAL;
		goto out;
	}

	pthread_mutex_lock(&inode->i_attr_mutex);
	pthread_rwlock_rdlock(&inode->i_parent->d_rwlock);

	dl = &resok->reply;

	rcu_read_lock();
	cds_list_for_each_entry_rcu(de, &de->d_inode->i_children, d_siblings) {
		if (de->d_cookie < cookie)
			continue;

		entry3 *e = calloc(1, sizeof(*e));
		if (!e) {
			if (!dl->entries) {
				free(dl);
				res->status = NFS3ERR_JUKEBOX;
				poa = &res->READDIR3res_u.resfail.dir_attributes;
				goto update_wcc;
			}

			break;
		}

		e->fileid = de->d_inode->i_ino;
		e->cookie = de->d_cookie;
		e->name = strdup(de->d_name);
		if (!e->name) {
			if (!dl->entries) {
				free(dl);
				res->status = NFS3ERR_JUKEBOX;
				poa = &res->READDIR3res_u.resfail.dir_attributes;
				goto update_wcc;
			}

			break;
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
	rcu_read_unlock();

	dl->eof = true;

past_eof:
	inode_update_times_now(inode, REFFS_INODE_UPDATE_ATIME);

update_wcc:
	poa->attributes_follow = true;
	fa = &poa->post_op_attr_u.attributes;
	inode_attr_to_fattr(inode, fa);

	pthread_rwlock_unlock(&inode->i_parent->d_rwlock);
	pthread_mutex_unlock(&inode->i_attr_mutex);

	print_nfs_fh3_hex(&args->dir);

out:
	inode_put(inode);
	super_block_put(sb);
	return res->status;
}

static int nfs3_readdirplus(struct rpc_trans *rt)
{
	TRACE("READDIRPLUS: 0x%x", rt->rt_info.ri_xid);
	return 0;
}

static int nfs3_fsstat(struct rpc_trans *rt)
{
	TRACE("FSSTAT: 0x%x", rt->rt_info.ri_xid);
	return 0;
}

static int nfs3_fsinfo(struct rpc_trans *rt)
{
	TRACE("FSINFO: 0x%x", rt->rt_info.ri_xid);
	return 0;
}

static int nfs3_pathconf(struct rpc_trans *rt)
{
	TRACE("PATHCONF: 0x%x", rt->rt_info.ri_xid);
	return 0;
}

static int nfs3_commit(struct rpc_trans *rt)
{
	TRACE("COMMIT: 0x%x", rt->rt_info.ri_xid);
	return 0;
}

const struct rpc_operations_handler nfs3_operations_handler[] = {
	RPC_OPERATION_INIT(NFSPROC3_NULL, NULL, NULL, NULL, NULL, nfs3_null),
	RPC_OPERATION_INIT(NFSPROC3_GETATTR, xdr_GETATTR3args, GETATTR3args,
			   xdr_GETATTR3res, GETATTR3res, nfs3_getattr),
	RPC_OPERATION_INIT(NFSPROC3_SETATTR, xdr_SETATTR3args, SETATTR3args,
			   xdr_SETATTR3res, SETATTR3res, nfs3_setattr),
	RPC_OPERATION_INIT(NFSPROC3_LOOKUP, xdr_LOOKUP3args, LOOKUP3args,
			   xdr_LOOKUP3res, LOOKUP3res, nfs3_lookup),
	RPC_OPERATION_INIT(NFSPROC3_ACCESS, xdr_ACCESS3args, ACCESS3args,
			   xdr_ACCESS3res, ACCESS3res, nfs3_access),
	RPC_OPERATION_INIT(NFSPROC3_READLINK, xdr_READLINK3args, READLINK3args,
			   xdr_READLINK3res, READLINK3res, nfs3_readlink),
	RPC_OPERATION_INIT(NFSPROC3_READ, xdr_READ3args, READ3args,
			   xdr_READ3res, READ3res, nfs3_read),
	RPC_OPERATION_INIT(NFSPROC3_WRITE, xdr_WRITE3args, WRITE3args,
			   xdr_WRITE3res, WRITE3res, nfs3_write),
	RPC_OPERATION_INIT(NFSPROC3_CREATE, xdr_CREATE3args, CREATE3args,
			   xdr_CREATE3res, CREATE3res, nfs3_create),
	RPC_OPERATION_INIT(NFSPROC3_MKDIR, xdr_MKDIR3args, MKDIR3args,
			   xdr_MKDIR3res, MKDIR3res, nfs3_mkdir),
	RPC_OPERATION_INIT(NFSPROC3_SYMLINK, xdr_SYMLINK3args, SYMLINK3args,
			   xdr_SYMLINK3res, SYMLINK3res, nfs3_symlink),
	RPC_OPERATION_INIT(NFSPROC3_MKNOD, xdr_MKNOD3args, MKNOD3args,
			   xdr_MKNOD3res, MKNOD3res, nfs3_mknod),
	RPC_OPERATION_INIT(NFSPROC3_REMOVE, xdr_REMOVE3args, REMOVE3args,
			   xdr_REMOVE3res, REMOVE3res, nfs3_remove),
	RPC_OPERATION_INIT(NFSPROC3_RMDIR, xdr_RMDIR3args, RMDIR3args,
			   xdr_RMDIR3res, RMDIR3res, nfs3_rmdir),
	RPC_OPERATION_INIT(NFSPROC3_RENAME, xdr_RENAME3args, RENAME3args,
			   xdr_RENAME3res, RENAME3res, nfs3_rename),
	RPC_OPERATION_INIT(NFSPROC3_LINK, xdr_LINK3args, LINK3args,
			   xdr_LINK3res, LINK3res, nfs3_link),
	RPC_OPERATION_INIT(NFSPROC3_READDIR, xdr_READDIR3args, READDIR3args,
			   xdr_READDIR3res, READDIR3res, nfs3_readdir),
	RPC_OPERATION_INIT(NFSPROC3_READDIRPLUS, xdr_READDIRPLUS3args,
			   READDIRPLUS3args, xdr_READDIRPLUS3res,
			   READDIRPLUS3res, nfs3_readdirplus),
	RPC_OPERATION_INIT(NFSPROC3_FSSTAT, xdr_FSSTAT3args, FSSTAT3args,
			   xdr_FSSTAT3res, FSSTAT3res, nfs3_fsstat),
	RPC_OPERATION_INIT(NFSPROC3_FSINFO, xdr_FSINFO3args, FSINFO3args,
			   xdr_FSINFO3res, FSINFO3res, nfs3_fsinfo),
	RPC_OPERATION_INIT(NFSPROC3_PATHCONF, xdr_PATHCONF3args, PATHCONF3args,
			   xdr_PATHCONF3res, PATHCONF3res, nfs3_pathconf),
	RPC_OPERATION_INIT(NFSPROC3_COMMIT, xdr_COMMIT3args, COMMIT3args,
			   xdr_COMMIT3res, COMMIT3res, nfs3_commit),
};

static struct rpc_program_handler *nfs3_handler;

volatile sig_atomic_t registered = 0;

int nfs3_protocol_register(void)
{
	if (registered)
		return 0;

	registered = 1;

	nfs3_handler = rpc_program_handler_alloc(
		NFS3_PROGRAM, NFS_V3, nfs3_operations_handler,
		sizeof(nfs3_operations_handler) /
			sizeof(*nfs3_operations_handler));
	if (!nfs3_handler) {
		registered = 0;
		return ENOMEM;
	}

	return 0;
}

int nfs3_protocol_deregister(void)
{
	if (!registered)
		return 0;

	rpc_program_handler_put(nfs3_handler);
	nfs3_handler = NULL;
	registered = 0;

	return 0;
}
