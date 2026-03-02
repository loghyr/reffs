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
#include "reffs/server.h"
#include "reffs/identity.h"
#include "reffs/trace/nfs3_server.h"

#define NFS3_PATH_MAX (1023)
#define NFS3_NAME_MAX (255)

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
				  struct authunix_parms *ap, uint64_t *flags)
{
	/* Check if user is in the file's current group */
	bool user_in_current_group =
		is_user_in_group(ap ? ap->aup_uid : 0, inode->i_gid, ap);

	/*
         * Permission Checks:
         * 1. Root can do anything
         * 2. Non-root can only change ownership if they own the file
         * 3. Non-root can only change group to a group they're a member of
         */

	/* If no auth params or root user, allow all changes */
	if (!ap || ap->aup_uid == 0) {
		/* Root can do anything */
	} else { /* Non-root user */
		/* Changing owner requires being the owner */
		if (sa->uid.set_it && ap->aup_uid != inode->i_uid)
			return NFS3ERR_PERM;

		/* Changing owner to someone else requires root */
		if (sa->uid.set_it && sa->uid.set_uid3_u.uid != (uid_t)-1 &&
		    sa->uid.set_uid3_u.uid != inode->i_uid)
			return NFS3ERR_PERM;

		/* Changing group requires being the owner */
		if (sa->gid.set_it && ap->aup_uid != inode->i_uid)
			return NFS3ERR_PERM;

		/* Changing group to a real value (not -1) requires membership */
		if (sa->gid.set_it && sa->gid.set_gid3_u.gid != (gid_t)-1) {
			gid_t target_gid = sa->gid.set_gid3_u.gid;
			bool user_in_target_group = false;

			/* Primary group membership */
			if (ap->aup_gid == target_gid)
				user_in_target_group = true;

			/*
			 * Supplementary groups membership
                         * NFS protocol may use different representations for -1 groups
                         * so be cautious and validate each group
                         */
			if (!user_in_target_group && ap->aup_len > 0) {
				for (uint32_t i = 0; i < ap->aup_len; i++) {
					if (ap->aup_gids[i] == target_gid) {
						user_in_target_group = true;
						break;
					}
				}
			}

			/* If not in any group, deny the change */
			if (!user_in_target_group)
				return NFS3ERR_PERM;
		}
	}

	/* Handle file size changes */
	if (sa->size.set_it) {
		if (inode->i_mode & S_IFDIR)
			return NFS3ERR_ISDIR;
		size_t old_size = inode->i_size;
		size_t new_size = sa->size.set_size3_u.size;

		if (!inode->i_db) {
			if (new_size > 0) {
				inode->i_db = data_block_alloc(inode, NULL,
							       new_size, 0);

				if (!inode->i_db)
					return NFS3ERR_NOSPC;
			}
			inode->i_size = new_size;
		} else {
			size_t sz = data_block_resize(inode->i_db, new_size);
			if ((ssize_t)sz < 0)
				return NFS3ERR_NOSPC;
			inode->i_size = sz;
		}

		inode->i_used =
			inode->i_size / inode->i_sb->sb_block_size +
			(inode->i_size % inode->i_sb->sb_block_size ? 1 : 0);
		__atomic_add_fetch(&inode->i_sb->sb_bytes_used,
				   (ssize_t)inode->i_size - (ssize_t)old_size,
				   __ATOMIC_RELAXED);
		if (flags)
			*flags |= REFFS_INODE_UPDATE_CTIME |
				  REFFS_INODE_UPDATE_MTIME;
	}

	if (sa->mode.set_it) {
		/* Check ownership for mode changes - only owner or root can change mode */
		if (ap && ap->aup_uid != 0 && ap->aup_uid != inode->i_uid) {
			return NFS3ERR_PERM;
		}

		uint16_t file_type = inode->i_mode & S_IFMT;
		uint16_t new_mode = sa->mode.set_mode3_u.mode & 07777;

		/* Only clear S_ISGID if attempting to set it AND user is not in the file's group */
		if ((new_mode & S_ISGID) && /* Trying to set S_ISGID */
		    S_ISREG(inode->i_mode) && /* Is a regular file */
		    ap && ap->aup_uid != 0 && /* Not root */
		    !user_in_current_group) { /* Not in file's group */
			/* Clear the S_ISGID bit */
			new_mode &= ~S_ISGID;
		}

		/* Apply the new mode */
		inode->i_mode = new_mode | file_type;
		if (flags)
			*flags |= REFFS_INODE_UPDATE_CTIME;
	}

	/* Store original mode to check if we need to clear set-ID bits */
	mode_t orig_mode = inode->i_mode;
	bool is_uid_change = sa->uid.set_it &&
			     sa->uid.set_uid3_u.uid != (uid_t)-1 &&
			     sa->uid.set_uid3_u.uid != inode->i_uid;
	bool is_gid_change = sa->gid.set_it &&
			     sa->gid.set_gid3_u.gid != (gid_t)-1 &&
			     sa->gid.set_gid3_u.gid != inode->i_gid;

	/* Apply ownership changes */
	if (sa->uid.set_it && sa->uid.set_uid3_u.uid != (uid_t)-1) {
		inode->i_uid = sa->uid.set_uid3_u.uid;
		if (flags)
			*flags |= REFFS_INODE_UPDATE_CTIME;
	}
	if (sa->gid.set_it && sa->gid.set_gid3_u.gid != (gid_t)-1) {
		inode->i_gid = sa->gid.set_gid3_u.gid;
		if (flags)
			*flags |= REFFS_INODE_UPDATE_CTIME;
	}

	/* Clear set-ID bits according to POSIX rules for chown */
	if ((is_uid_change || is_gid_change) &&
	    (orig_mode & (S_ISUID | S_ISGID))) {
		if (ap && ap->aup_uid != 0) {
			/* For non-root users: always clear both SUID and SGID bits when changing ownership */
			inode->i_mode &= ~(S_ISUID | S_ISGID);
		} else {
			/* For root users: based on test expectations, either:
                         * - Clear both bits (0555), or
                         * - Keep both bits (06555)
                         * We'll clear both to match the test expectations
                         */
			inode->i_mode &= ~(S_ISUID | S_ISGID);
		}
	}

	if ((sa->atime.set_it != DONT_CHANGE ||
	     sa->mtime.set_it != DONT_CHANGE) &&
	    ap && ap->aup_uid != 0 && ap->aup_uid != inode->i_uid) {
		return NFS3ERR_PERM;
	}

	/* Handle timestamp changes */
	switch (sa->atime.set_it) {
	case DONT_CHANGE:
		break;
	case SET_TO_SERVER_TIME:
		if (flags)
			*flags |= REFFS_INODE_UPDATE_CTIME |
				  REFFS_INODE_UPDATE_ATIME;
		break;
	case SET_TO_CLIENT_TIME:
		nfstime3_to_timespec(&sa->atime.set_atime_u.atime,
				     &inode->i_atime);
		if (flags)
			*flags |= REFFS_INODE_UPDATE_CTIME;
		break;
	}
	switch (sa->mtime.set_it) {
	case DONT_CHANGE:
		break;
	case SET_TO_SERVER_TIME:
		if (flags)
			*flags |= REFFS_INODE_UPDATE_CTIME |
				  REFFS_INODE_UPDATE_MTIME;
		break;
	case SET_TO_CLIENT_TIME:
		nfstime3_to_timespec(&sa->mtime.set_mtime_u.mtime,
				     &inode->i_mtime);
		if (flags) {
			*flags |= REFFS_INODE_UPDATE_CTIME;
			*flags &= ~REFFS_INODE_UPDATE_MTIME;
		}
		break;
	}
	return NFS3_OK;
}

#ifdef NOT_NOW
static void print_nfs_fh3_hex(nfs_fh3 *fh)
{
	uint32_t crc =
		crc32(0L, (const Bytef *)fh->data.data_val, fh->data.data_len);

	struct network_file_handle *nfh;

	nfh = (struct network_file_handle *)fh;

	TRACE("FileHandle: sb = %lu, ino =%lu, vers = %u, CRC32 = 0x%08x",
	      nfh->nfh_sb, nfh->nfh_ino, nfh->nfh_vers, crc);
}
#endif

static struct inode *directory_inode_find(struct super_block *sb, uint64_t ino,
					  struct authunix_parms *ap, int mode,
					  nfsstat3 *status)
{
	struct inode *inode;

	inode = inode_find(sb, ino);
	if (!inode) {
		*status = NFS3ERR_NOENT;
		goto out;
	}

	if (!(inode->i_mode & S_IFDIR)) {
		*status = NFS3ERR_NOTDIR;
		goto out;
	}

	*status = inode_access_check(inode, ap, X_OK);
	if (*status)
		goto out;

	*status = inode_access_check(inode, ap, mode);
out:
	return inode;
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

	trace_nfs3_srv_getattr(rt, args);

	res->status = rpc_cred_to_authunix_parms(&rt->rt_info.ri_cred, &ap);
	if (res->status)
		goto out;

	if (args->object.data.data_len != sizeof(*nfh)) {
		res->status = NFS3ERR_BADHANDLE;
		goto out;
	}

	nfh = (struct network_file_handle *)args->object.data.data_val;

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

	pthread_mutex_lock(&inode->i_attr_mutex);
	inode_attr_to_fattr(inode, fa);
	pthread_mutex_unlock(&inode->i_attr_mutex);

out:
	inode_put(inode);
	super_block_put(sb);
	return res->status;
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

	uint64_t flags = 0;
	struct authunix_parms ap;

	trace_nfs3_srv_setattr(rt, args);

	res->status = rpc_cred_to_authunix_parms(&rt->rt_info.ri_cred, &ap);
	if (res->status)
		goto out;

	if (args->object.data.data_len != sizeof(*nfh)) {
		res->status = NFS3ERR_BADHANDLE;
		goto out;
	}

	nfh = (struct network_file_handle *)args->object.data.data_val;

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

	pthread_mutex_lock(&inode->i_attr_mutex);

	size = inode->i_size;
	timespec_to_nfstime3(&inode->i_ctime, &ctime);
	timespec_to_nfstime3(&inode->i_mtime, &mtime);

	if (args->guard.check) {
		if (!nfstime3_is_timespec(&args->guard.sattrguard3_u.obj_ctime,
					  &inode->i_ctime)) {
			res->status = NFS3ERR_NOT_SYNC;
			wcc = &res->SETATTR3res_u.resfail.obj_wcc;
			goto update_wcc;
		}
	}

	pthread_rwlock_wrlock(&inode->i_db_rwlock);
	res->status = nfs3_apply_sattr3(inode, sa, &ap, &flags);
	pthread_rwlock_unlock(&inode->i_db_rwlock);
	if (res->status) {
		wcc = &res->SETATTR3res_u.resfail.obj_wcc;
		goto update_wcc;
	}

	if (flags)
		inode_update_times_now(inode, flags);

update_wcc:
	wcc->before.attributes_follow = true;
	fattr3 *fa = &wcc->after.post_op_attr_u.attributes;
	wcc->before.pre_op_attr_u.attributes.size = size;
	wcc->before.pre_op_attr_u.attributes.mtime = mtime;
	wcc->before.pre_op_attr_u.attributes.ctime = ctime;
	wcc->after.attributes_follow = true;
	inode_attr_to_fattr(inode, fa);

	pthread_mutex_unlock(&inode->i_attr_mutex);

out:
	inode_put(inode);
	super_block_put(sb);
	return res->status;
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

	trace_nfs3_srv_lookup(rt, args);

	res->status = rpc_cred_to_authunix_parms(&rt->rt_info.ri_cred, &ap);
	if (res->status)
		goto out;

	if (args->what.dir.data.data_len != sizeof(*nfh)) {
		res->status = NFS3ERR_BADHANDLE;
		goto out;
	}

	nfh = (struct network_file_handle *)args->what.dir.data.data_val;

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		res->status = NFS3ERR_STALE;
		goto out;
	}

	inode = directory_inode_find(sb, nfh->nfh_ino, &ap, X_OK, &res->status);
	if (res->status)
		goto out;

	pthread_mutex_lock(&inode->i_attr_mutex);

	exists = inode_name_get_inode(inode, args->what.name);
	if (!exists) {
		res->status = NFS3ERR_NOENT;
		poa = &res->LOOKUP3res_u.resfail.dir_attributes;
		goto update_wcc;
	}

	pthread_mutex_lock(&exists->i_attr_mutex);

	// When we add more than one sb, be careul here
	nfh = network_file_handle_construct(sb->sb_id, exists->i_ino);
	pthread_mutex_unlock(&exists->i_attr_mutex);
	if (!nfh) {
		res->status = NFS3ERR_JUKEBOX;
		poa = &res->LOOKUP3res_u.resfail.dir_attributes;
		goto update_wcc;
	}

	resok->object.data.data_val = (char *)nfh;
	resok->object.data.data_len = sizeof(*nfh);

	resok->obj_attributes.attributes_follow = true;
	fa = &resok->obj_attributes.post_op_attr_u.attributes;

	inode_update_times_now(inode, REFFS_INODE_UPDATE_ATIME);
	inode_attr_to_fattr(exists, fa);

update_wcc:
	poa->attributes_follow = true;
	fa = &poa->post_op_attr_u.attributes;

	inode_attr_to_fattr(inode, fa);

	pthread_mutex_unlock(&inode->i_attr_mutex);

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

	nfsstat3 status;

	trace_nfs3_srv_access(rt, args);

	res->status = rpc_cred_to_authunix_parms(&rt->rt_info.ri_cred, &ap);
	if (res->status)
		goto out;

	if (args->object.data.data_len != sizeof(*nfh)) {
		res->status = NFS3ERR_BADHANDLE;
		goto out;
	}

	nfh = (struct network_file_handle *)args->object.data.data_val;

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
		status = inode_access_check(inode, &ap, R_OK);
		if (!status)
			resok->access |= ACCESS3_READ;
	}

	if (args->access & ACCESS3_LOOKUP && (inode->i_mode & S_IFDIR)) {
		status = inode_access_check(inode, &ap, X_OK);
		if (!status)
			resok->access |= ACCESS3_LOOKUP;
	}

	if (args->access & ACCESS3_MODIFY) {
		status = inode_access_check(inode, &ap, W_OK);
		if (!status)
			resok->access |= ACCESS3_MODIFY;
	}

	if (args->access & ACCESS3_EXTEND) {
		status = inode_access_check(inode, &ap, W_OK);
		if (!status)
			resok->access |= ACCESS3_EXTEND;
	}

	if (args->access & ACCESS3_DELETE && (inode->i_mode & S_IFDIR)) {
		status = inode_access_check(inode, &ap, W_OK);
		if (!status)
			resok->access |= ACCESS3_DELETE;
	}

	if (args->access & ACCESS3_EXECUTE && !(inode->i_mode & S_IFDIR)) {
		status = inode_access_check(inode, &ap, X_OK);
		if (!status)
			resok->access |= ACCESS3_EXECUTE;
	}

	fa = &resok->obj_attributes.post_op_attr_u.attributes;
	resok->obj_attributes.attributes_follow = true;

	pthread_mutex_lock(&inode->i_attr_mutex); // Consider reader/writer?

	inode_update_times_now(inode, REFFS_INODE_UPDATE_ATIME);
	inode_attr_to_fattr(inode, fa);

	pthread_mutex_unlock(&inode->i_attr_mutex);

out:
	inode_put(inode);
	super_block_put(sb);
	return res->status;
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

	char *name;

	struct network_file_handle *nfh = NULL;

	struct authunix_parms ap;

	trace_nfs3_srv_readlink(rt, args);

	res->status = rpc_cred_to_authunix_parms(&rt->rt_info.ri_cred, &ap);
	if (res->status)
		goto out;

	if (args->symlink.data.data_len != sizeof(*nfh)) {
		res->status = NFS3ERR_BADHANDLE;
		goto out;
	}

	nfh = (struct network_file_handle *)args->symlink.data.data_val;

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

	res->status = inode_access_check(inode, &ap, R_OK);
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

out:
	inode_put(inode);
	super_block_put(sb);
	return res->status;
}

static int nfs3_op_read(struct rpc_trans *rt)
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

	trace_nfs3_srv_read(rt, args);

	res->status = rpc_cred_to_authunix_parms(&rt->rt_info.ri_cred, &ap);
	if (res->status)
		goto out;

	if (args->file.data.data_len != sizeof(*nfh)) {
		res->status = NFS3ERR_BADHANDLE;
		goto out;
	}

	nfh = (struct network_file_handle *)args->file.data.data_val;

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

	res->status = inode_access_check(inode, &ap, R_OK);
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
		ssize_t dbr = data_block_read(inode->i_db, resok->data.data_val,
					      args->count, args->offset);
		if (dbr == 0 && args->count > 0) {
			// Read beyond current size
			resok->count = resok->data.data_len = 0;
			resok->eof = true;
			res->status = NFS3_OK;
			pthread_rwlock_unlock(&inode->i_db_rwlock);
			goto update_wcc;
		} else if (dbr < 0) {
			free(resok->data.data_val);
			resok->count = resok->data.data_len = 0;
			res->status =
				-dbr; // What about ENOMEM? Need errno_to_v3()
			poa = &res->READ3res_u.resfail.file_attributes;
			pthread_rwlock_unlock(&inode->i_db_rwlock);
			goto update_wcc;
		}

		resok->count = resok->data.data_len = res->status;
		res->status = NFS3_OK;

		if (args->offset + resok->count >= inode->i_db->db_size)
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

out:
	inode_put(inode);
	super_block_put(sb);
	return res->status;
}

static int nfs3_op_write(struct rpc_trans *rt)
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

	size_t db_size;

	trace_nfs3_srv_write(rt, args);

	res->status = rpc_cred_to_authunix_parms(&rt->rt_info.ri_cred, &ap);
	if (res->status)
		goto out;

	if (args->file.data.data_len != sizeof(*nfh)) {
		res->status = NFS3ERR_BADHANDLE;
		goto out;
	}

	nfh = (struct network_file_handle *)args->file.data.data_val;

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

	res->status = inode_access_check(inode, &ap, W_OK);
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
		inode->i_db = data_block_alloc(inode, args->data.data_val,
					       args->data.data_len,
					       args->offset);
		if (!inode->i_db) {
			res->status = NFS3ERR_NOSPC;
			wcc = &res->WRITE3res_u.resfail.file_wcc;
			pthread_rwlock_unlock(&inode->i_db_rwlock);
			goto update_wcc;
		}

		resok->count = args->data.data_len;
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
		res->status = NFS3_OK;
	}

	if ((inode->i_mode & S_ISGID) && ap.aup_uid != 0 &&
	    ap.aup_uid != inode->i_uid) {
		inode->i_mode &= ~S_ISGID;
	}

	if ((inode->i_mode & S_ISUID) && ap.aup_uid != 0 &&
	    ap.aup_uid != inode->i_uid) {
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

	inode_update_times_now(inode, REFFS_INODE_UPDATE_CTIME |
					      REFFS_INODE_UPDATE_MTIME);

	uuid_t *uuid = server_boot_uuid_get();
	memcpy(resok->verf, (*uuid) + 8, NFS3_WRITEVERFSIZE);

	inode->i_size = inode->i_db->db_size;
	inode->i_used = inode->i_size / sb->sb_block_size +
			(inode->i_size % sb->sb_block_size ? 1 : 0);

	db_size = data_block_get_size(inode->i_db);

	__atomic_add_fetch(&inode->i_sb->sb_bytes_used, db_size - size,
			   __ATOMIC_RELAXED);

	pthread_rwlock_unlock(&inode->i_db_rwlock);

update_wcc:
	wcc->before.attributes_follow = true;
	wcc->before.pre_op_attr_u.attributes.size = size;
	wcc->before.pre_op_attr_u.attributes.mtime = mtime;
	wcc->before.pre_op_attr_u.attributes.ctime = ctime;
	wcc->after.attributes_follow = true;

	inode_attr_to_fattr(inode, &wcc->after.post_op_attr_u.attributes);

	pthread_mutex_unlock(&inode->i_attr_mutex);

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

static int nfs3_op_create(struct rpc_trans *rt)
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

	struct network_file_handle *nfh = NULL;

	struct reffs_dirent *rd = NULL;
	struct authunix_parms ap;

	createverf3 cv;

	trace_nfs3_srv_create(rt, args);

	res->status = rpc_cred_to_authunix_parms(&rt->rt_info.ri_cred, &ap);
	if (res->status)
		goto out;

	if (args->where.dir.data.data_len != sizeof(*nfh)) {
		res->status = NFS3ERR_BADHANDLE;
		goto out;
	}

	nfh = (struct network_file_handle *)args->where.dir.data.data_val;

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		res->status = NFS3ERR_STALE;
		goto out;
	}

	if (strlen(args->where.name) > NFS3_NAME_MAX) {
		res->status = NFS3ERR_NAMETOOLONG;
		goto out;
	}

	inode = directory_inode_find(sb, nfh->nfh_ino, &ap, W_OK, &res->status);
	if (res->status)
		goto out;

	pthread_mutex_lock(&inode->i_attr_mutex);

	size = inode->i_size;
	timespec_to_nfstime3(&inode->i_ctime, &ctime);
	timespec_to_nfstime3(&inode->i_mtime, &mtime);

	pthread_rwlock_wrlock(&inode->i_parent->rd_rwlock);
	exists = inode_name_get_inode(inode, args->where.name);
	if (exists) {
		switch (args->how.mode) {
		case UNCHECKED:
			break;
		case GUARDED:
			res->status = NFS3ERR_EXIST;
			pthread_rwlock_unlock(&inode->i_parent->rd_rwlock);
			wcc = &resfail->dir_wcc;
			goto update_wcc;
		case EXCLUSIVE:
			timespec_to_createverf3(&exists->i_ctime, cv);
			if (memcmp(cv, args->how.createhow3_u.verf,
				   NFS3_CREATEVERFSIZE)) {
				res->status = NFS3ERR_EXIST;
				pthread_rwlock_unlock(
					&inode->i_parent->rd_rwlock);
				wcc = &resfail->dir_wcc;
				goto update_wcc;
			}
			res->status = NFS3_OK;
			break;
		}

		res->status = inode_access_check(exists, &ap, W_OK);
		if (res->status) {
			pthread_rwlock_unlock(&inode->i_parent->rd_rwlock);
			wcc = &resfail->dir_wcc;
			goto update_wcc;
		}

		tmp = exists;
	} else {
		rd = dirent_alloc(inode->i_parent, args->where.name,
				  reffs_life_action_birth);
		if (!rd) {
			res->status = NFS3ERR_NOENT;
			pthread_rwlock_unlock(&inode->i_parent->rd_rwlock);
			wcc = &resfail->dir_wcc;
			goto update_wcc;
		}

		rd->rd_inode =
			inode_alloc(sb, __atomic_add_fetch(&sb->sb_next_ino, 1,
							   __ATOMIC_RELAXED));
		if (!rd->rd_inode) {
			dirent_parent_release(rd, reffs_life_action_death);
			res->status = NFS3ERR_NOENT;
			pthread_rwlock_unlock(&inode->i_parent->rd_rwlock);
			wcc = &resfail->dir_wcc;
			goto update_wcc;
		}

		tmp = rd->rd_inode;

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

	if (args->how.mode == EXCLUSIVE) {
		createverf3_to_timespec(args->how.createhow3_u.verf,
					&tmp->i_ctime);
	} else {
		res->status = nfs3_apply_sattr3(tmp, sa, NULL, NULL);
		if (res->status) {
			wcc = &resfail->dir_wcc;
			goto update_wcc;
		}
	}

	nfh = network_file_handle_construct(sb->sb_id, tmp->i_ino);
	if (!nfh) {
		res->status = NFS3ERR_JUKEBOX;
		wcc = &resfail->dir_wcc;
		goto update_wcc;
	}

	resok->obj.post_op_fh3_u.handle.data.data_val = (char *)nfh;
	resok->obj.post_op_fh3_u.handle.data.data_len = sizeof(*nfh);
	resok->obj.handle_follows = true;

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

	pthread_rwlock_unlock(&inode->i_parent->rd_rwlock);
	pthread_mutex_unlock(&inode->i_attr_mutex);

out:
	inode_put(exists);
	inode_put(inode);
	super_block_put(sb);
	return res->status;
}

static int nfs3_op_mkdir(struct rpc_trans *rt)
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
	sattr3 *sa = &args->attributes;

	size3 size;
	nfstime3 mtime;
	nfstime3 ctime;

	struct network_file_handle *nfh = NULL;

	struct reffs_dirent *rd = NULL;
	struct authunix_parms ap;

	trace_nfs3_srv_mkdir(rt, args);

	res->status = rpc_cred_to_authunix_parms(&rt->rt_info.ri_cred, &ap);
	if (res->status)
		goto out;

	if (args->where.dir.data.data_len != sizeof(*nfh)) {
		res->status = NFS3ERR_BADHANDLE;
		goto out;
	}

	nfh = (struct network_file_handle *)args->where.dir.data.data_val;

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		res->status = NFS3ERR_STALE;
		goto out;
	}

	inode = directory_inode_find(sb, nfh->nfh_ino, &ap, W_OK, &res->status);
	if (res->status)
		goto out;

	if (strlen(args->where.name) > NFS3_NAME_MAX) {
		res->status = NFS3ERR_NAMETOOLONG;
		goto out;
	}

	pthread_mutex_lock(&inode->i_attr_mutex);

	size = inode->i_size;
	timespec_to_nfstime3(&inode->i_mtime, &mtime);
	timespec_to_nfstime3(&inode->i_ctime, &ctime);

	pthread_rwlock_wrlock(&inode->i_parent->rd_rwlock);
	if (inode_name_is_child(inode, args->where.name)) {
		res->status = NFS3ERR_EXIST;
		wcc = &resfail->dir_wcc;
		pthread_rwlock_unlock(&inode->i_parent->rd_rwlock);
		goto update_wcc;
	}

	rd = dirent_alloc(inode->i_parent, args->where.name,
			  reffs_life_action_birth);
	if (!rd) {
		res->status = NFS3ERR_NOENT;
		wcc = &resfail->dir_wcc;
		pthread_rwlock_unlock(&inode->i_parent->rd_rwlock);
		goto update_wcc;
	}

	rd->rd_inode = inode_alloc(sb, __atomic_add_fetch(&sb->sb_next_ino, 1,
							  __ATOMIC_RELAXED));
	if (!rd->rd_inode) {
		dirent_parent_release(rd, reffs_life_action_death);
		res->status = NFS3ERR_NOENT;
		wcc = &resfail->dir_wcc;
		pthread_rwlock_unlock(&inode->i_parent->rd_rwlock);
		goto update_wcc;
	}
	pthread_rwlock_unlock(&inode->i_parent->rd_rwlock);

	rd->rd_inode->i_uid = ap.aup_uid;
	rd->rd_inode->i_gid = ap.aup_gid;
	clock_gettime(CLOCK_REALTIME, &rd->rd_inode->i_mtime);
	rd->rd_inode->i_atime = rd->rd_inode->i_mtime;
	rd->rd_inode->i_btime = rd->rd_inode->i_mtime;
	rd->rd_inode->i_ctime = rd->rd_inode->i_mtime;
	rd->rd_inode->i_mode = S_IFDIR | inode->i_mode;
	rd->rd_inode->i_size = sb->sb_block_size;
	rd->rd_inode->i_used = sb->sb_block_size;
	rd->rd_inode->i_nlink = 2;

	res->status = nfs3_apply_sattr3(rd->rd_inode, sa, NULL, NULL);
	if (res->status) {
		wcc = &resfail->dir_wcc;
		goto update_wcc;
	}

	nfh = network_file_handle_construct(sb->sb_id, rd->rd_inode->i_ino);
	if (!nfh) {
		res->status = NFS3ERR_JUKEBOX;
		wcc = &resfail->dir_wcc;
		goto update_wcc;
	}

	resok->obj.post_op_fh3_u.handle.data.data_val = (char *)nfh;
	resok->obj.post_op_fh3_u.handle.data.data_len = sizeof(*nfh);
	resok->obj.handle_follows = true;

	wcc->before.attributes_follow = true;
	wcc->before.pre_op_attr_u.attributes.size = size;
	wcc->before.pre_op_attr_u.attributes.mtime = mtime;
	wcc->before.pre_op_attr_u.attributes.ctime = ctime;

	resok->obj_attributes.attributes_follow = true;
	fa = &resok->obj_attributes.post_op_attr_u.attributes;
	inode_attr_to_fattr(rd->rd_inode, fa);

	rd->rd_inode->i_parent = rd;

update_wcc:
	wcc->after.attributes_follow = true;
	fa = &wcc->after.post_op_attr_u.attributes;
	inode_attr_to_fattr(inode, fa);

	pthread_mutex_unlock(&inode->i_attr_mutex);

out:
	inode_put(inode);
	super_block_put(sb);
	return res->status;
}

static int nfs3_op_symlink(struct rpc_trans *rt)
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

	size3 size;
	nfstime3 mtime;
	nfstime3 ctime;

	char *name = NULL;

	struct network_file_handle *nfh = NULL;
	struct network_file_handle *nfh_new = NULL;

	struct reffs_dirent *rd = NULL;
	struct authunix_parms ap;

	trace_nfs3_srv_symlink(rt, args);

	res->status = rpc_cred_to_authunix_parms(&rt->rt_info.ri_cred, &ap);
	if (res->status)
		goto out;

	if (args->where.dir.data.data_len != sizeof(*nfh)) {
		res->status = NFS3ERR_BADHANDLE;
		goto out;
	}

	nfh = (struct network_file_handle *)args->where.dir.data.data_val;

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		res->status = NFS3ERR_STALE;
		goto out;
	}

	inode = directory_inode_find(sb, nfh->nfh_ino, &ap, W_OK, &res->status);
	if (res->status)
		goto out;

	if (strlen(args->symlink.symlink_data) > NFS3_PATH_MAX) {
		res->status = NFS3ERR_NAMETOOLONG;
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

	pthread_rwlock_wrlock(&inode->i_parent->rd_rwlock);
	if (inode_name_is_child(inode, args->where.name)) {
		res->status = NFS3ERR_EXIST;
		wcc = &resfail->dir_wcc;
		pthread_rwlock_unlock(&inode->i_parent->rd_rwlock);
		goto update_wcc;
	}

	rd = dirent_alloc(inode->i_parent, args->where.name,
			  reffs_life_action_birth);
	if (!rd) {
		res->status = NFS3ERR_NOENT;
		wcc = &resfail->dir_wcc;
		pthread_rwlock_unlock(&inode->i_parent->rd_rwlock);
		goto update_wcc;
	}

	rd->rd_inode = inode_alloc(sb, __atomic_add_fetch(&sb->sb_next_ino, 1,
							  __ATOMIC_RELAXED));
	if (!rd->rd_inode) {
		dirent_parent_release(rd, reffs_life_action_death);
		res->status = NFS3ERR_NOENT;
		wcc = &resfail->dir_wcc;
		pthread_rwlock_unlock(&inode->i_parent->rd_rwlock);
		goto update_wcc;
	}
	pthread_rwlock_unlock(&inode->i_parent->rd_rwlock);

	rd->rd_inode->i_uid = ap.aup_uid;
	rd->rd_inode->i_gid = ap.aup_gid;
	clock_gettime(CLOCK_REALTIME, &rd->rd_inode->i_mtime);
	rd->rd_inode->i_atime = rd->rd_inode->i_mtime;
	rd->rd_inode->i_btime = rd->rd_inode->i_mtime;
	rd->rd_inode->i_ctime = rd->rd_inode->i_mtime;
	rd->rd_inode->i_mode = (S_IFLNK | inode->i_mode) & ~S_IFDIR;
	rd->rd_inode->i_size = sb->sb_block_size;
	rd->rd_inode->i_used = sb->sb_block_size;
	rd->rd_inode->i_nlink = 1;

	nfh_new = network_file_handle_construct(sb->sb_id, rd->rd_inode->i_ino);
	if (!nfh_new) {
		res->status = NFS3ERR_JUKEBOX;
		wcc = &resfail->dir_wcc;
		goto update_wcc;
	}

	res->status = nfs3_apply_sattr3(rd->rd_inode, sa, NULL, NULL);
	if (res->status) {
		wcc = &resfail->dir_wcc;
		goto update_wcc;
	}

	rd->rd_inode->i_symlink = name;
	name = NULL;

	resok->obj.post_op_fh3_u.handle.data.data_val = (char *)nfh_new;
	resok->obj.post_op_fh3_u.handle.data.data_len = sizeof(*nfh_new);
	resok->obj.handle_follows = true;
	nfh_new = NULL;

	wcc->before.attributes_follow = true;
	wcc->before.pre_op_attr_u.attributes.size = size;
	wcc->before.pre_op_attr_u.attributes.mtime = mtime;
	wcc->before.pre_op_attr_u.attributes.ctime = ctime;

	resok->obj_attributes.attributes_follow = true;
	fa = &resok->obj_attributes.post_op_attr_u.attributes;
	inode_attr_to_fattr(rd->rd_inode, fa);

update_wcc:
	wcc->after.attributes_follow = true;
	fa = &wcc->after.post_op_attr_u.attributes;
	inode_attr_to_fattr(inode, fa);

	pthread_mutex_unlock(&inode->i_attr_mutex);

out:
	free(nfh_new);
	free(name);
	inode_put(inode);
	super_block_put(sb);
	return res->status;
}

static int nfs3_op_mknod(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	struct super_block *sb = NULL;
	struct inode *inode = NULL;

	MKNOD3args *args = ph->ph_args;
	MKNOD3res *res = ph->ph_res;
	MKNOD3resok *resok = &res->MKNOD3res_u.resok;

	wcc_data *wcc = &resok->dir_wcc;
	fattr3 *fa = &wcc->after.post_op_attr_u.attributes;
	sattr3 *sa;

	size3 size;
	nfstime3 mtime;
	nfstime3 ctime;

	struct network_file_handle *nfh = NULL;

	struct reffs_dirent *rd = NULL;
	struct authunix_parms ap;

	trace_nfs3_srv_mknod(rt, args);

	res->status = rpc_cred_to_authunix_parms(&rt->rt_info.ri_cred, &ap);
	if (res->status)
		goto out;

	if (args->where.dir.data.data_len != sizeof(*nfh)) {
		res->status = NFS3ERR_BADHANDLE;
		goto out;
	}

	nfh = (struct network_file_handle *)args->where.dir.data.data_val;

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		res->status = NFS3ERR_STALE;
		goto out;
	}

	if (strlen(args->where.name) > NFS3_NAME_MAX) {
		res->status = NFS3ERR_NAMETOOLONG;
		goto out;
	}

	inode = directory_inode_find(sb, nfh->nfh_ino, &ap, W_OK, &res->status);
	if (res->status)
		goto out;

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

	pthread_rwlock_wrlock(&inode->i_parent->rd_rwlock);
	if (inode_name_is_child(inode, args->where.name)) {
		res->status = NFS3ERR_EXIST;
		wcc = &res->MKNOD3res_u.resfail.dir_wcc;
		fa = &wcc->after.post_op_attr_u.attributes;
		pthread_rwlock_unlock(&inode->i_parent->rd_rwlock);
		goto update_wcc;
	}

	rd = dirent_alloc(inode->i_parent, args->where.name,
			  reffs_life_action_birth);
	if (!rd) {
		res->status = NFS3ERR_NOENT;
		wcc = &res->MKNOD3res_u.resfail.dir_wcc;
		fa = &wcc->after.post_op_attr_u.attributes;
		pthread_rwlock_unlock(&inode->i_parent->rd_rwlock);
		goto update_wcc;
	}

	rd->rd_inode = inode_alloc(sb, __atomic_add_fetch(&sb->sb_next_ino, 1,
							  __ATOMIC_RELAXED));
	if (!rd->rd_inode) {
		dirent_parent_release(rd, reffs_life_action_death);
		res->status = NFS3ERR_NOENT;
		wcc = &res->MKNOD3res_u.resfail.dir_wcc;
		fa = &wcc->after.post_op_attr_u.attributes;
		pthread_rwlock_unlock(&inode->i_parent->rd_rwlock);
		goto update_wcc;
	}
	pthread_rwlock_unlock(&inode->i_parent->rd_rwlock);

	rd->rd_inode->i_uid = ap.aup_uid;
	rd->rd_inode->i_gid = ap.aup_gid;
	clock_gettime(CLOCK_REALTIME, &rd->rd_inode->i_mtime);
	rd->rd_inode->i_atime = rd->rd_inode->i_mtime;
	rd->rd_inode->i_btime = rd->rd_inode->i_mtime;
	rd->rd_inode->i_ctime = rd->rd_inode->i_mtime;
	rd->rd_inode->i_mode = inode->i_mode & ~S_IFDIR;
	rd->rd_inode->i_size = 4096;
	rd->rd_inode->i_used = 0;
	rd->rd_inode->i_nlink = 1;

	switch (args->what.type) {
	case NF3REG:
	case NF3DIR:
	case NF3LNK:
		verify_msg(0, "Type changed: %u", args->what.type);
		goto out;
	case NF3BLK:
		sa = &args->what.mknoddata3_u.device.dev_attributes;
		rd->rd_inode->i_dev_major =
			args->what.mknoddata3_u.device.spec.specdata1;
		rd->rd_inode->i_dev_minor =
			args->what.mknoddata3_u.device.spec.specdata2;
		rd->rd_inode->i_mode |= S_IFBLK;
		break;
	case NF3CHR:
		sa = &args->what.mknoddata3_u.device.dev_attributes;
		rd->rd_inode->i_dev_major =
			args->what.mknoddata3_u.device.spec.specdata1;
		rd->rd_inode->i_dev_minor =
			args->what.mknoddata3_u.device.spec.specdata2;
		rd->rd_inode->i_mode |= S_IFCHR;
		break;
	case NF3SOCK:
		sa = &args->what.mknoddata3_u.pipe_attributes;
		rd->rd_inode->i_mode |= S_IFSOCK;
		break;
	case NF3FIFO:
		sa = &args->what.mknoddata3_u.pipe_attributes;
		rd->rd_inode->i_mode |= S_IFIFO;
		break;
	}

	res->status = nfs3_apply_sattr3(rd->rd_inode, sa, NULL, NULL);
	if (res->status) {
		wcc = &res->MKNOD3res_u.resfail.dir_wcc;
		fa = &wcc->after.post_op_attr_u.attributes;
		goto update_wcc;
	}

	nfh = network_file_handle_construct(sb->sb_id, rd->rd_inode->i_ino);
	if (!nfh) {
		res->status = NFS3ERR_JUKEBOX;
		wcc = &res->MKNOD3res_u.resfail.dir_wcc;
		fa = &wcc->after.post_op_attr_u.attributes;
		goto update_wcc;
	}

	resok->obj.post_op_fh3_u.handle.data.data_val = (char *)nfh;
	resok->obj.post_op_fh3_u.handle.data.data_len = sizeof(*nfh);
	resok->obj.handle_follows = true;

	inode_update_times_now(inode, REFFS_INODE_UPDATE_CTIME |
					      REFFS_INODE_UPDATE_MTIME);

	wcc->before.attributes_follow = true;
	wcc->before.pre_op_attr_u.attributes.size = size;
	wcc->before.pre_op_attr_u.attributes.mtime = mtime;
	wcc->before.pre_op_attr_u.attributes.ctime = ctime;

	resok->obj_attributes.attributes_follow = true;
	fa = &resok->obj_attributes.post_op_attr_u.attributes;

	inode_attr_to_fattr(rd->rd_inode, fa);

update_wcc:
	wcc->after.attributes_follow = true;
	fa = &wcc->after.post_op_attr_u.attributes;
	inode_attr_to_fattr(inode, fa);

	pthread_mutex_unlock(&inode->i_attr_mutex);

out:
	inode_put(inode);
	super_block_put(sb);
	return res->status;
}

static int nfs3_op_remove(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	struct super_block *sb = NULL;
	struct inode *inode = NULL;
	struct reffs_dirent *rd = NULL;

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

	trace_nfs3_srv_remove(rt, args);

	res->status = rpc_cred_to_authunix_parms(&rt->rt_info.ri_cred, &ap);
	if (res->status)
		goto out;

	if (args->object.dir.data.data_len != sizeof(*nfh)) {
		res->status = NFS3ERR_BADHANDLE;
		goto out;
	}

	nfh = (struct network_file_handle *)args->object.dir.data.data_val;

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		res->status = NFS3ERR_STALE;
		goto out;
	}

	inode = directory_inode_find(sb, nfh->nfh_ino, &ap, W_OK, &res->status);
	if (res->status)
		goto out;

	if (strlen(args->object.name) > NFS3_NAME_MAX) {
		res->status = NFS3ERR_NAMETOOLONG;
		goto out;
	}

	pthread_rwlock_wrlock(&inode->i_parent->rd_rwlock);

	size = inode->i_size;
	timespec_to_nfstime3(&inode->i_ctime, &ctime);
	timespec_to_nfstime3(&inode->i_mtime, &mtime);

	pthread_mutex_lock(&inode->i_attr_mutex);
	rd = dirent_find(inode->i_parent, reffs_case_get(), args->object.name);
	if (!rd) {
		res->status = NFS3ERR_NOENT;
		wcc = &res->REMOVE3res_u.resfail.dir_wcc;
		pthread_rwlock_unlock(&inode->i_parent->rd_rwlock);
		goto update_wcc;
	}

	dirent_parent_release(rd, reffs_life_action_delayed_death);
	dirent_put(rd); // One for remove
	dirent_put(rd); // One for the find
	pthread_rwlock_unlock(&inode->i_parent->rd_rwlock);

update_wcc:
	wcc->before.attributes_follow = true;
	wcc->before.pre_op_attr_u.attributes.size = size;
	wcc->before.pre_op_attr_u.attributes.mtime = mtime;
	wcc->before.pre_op_attr_u.attributes.ctime = ctime;

	wcc->after.attributes_follow = true;
	fa = &wcc->after.post_op_attr_u.attributes;

	inode_attr_to_fattr(inode, fa);

	pthread_mutex_unlock(&inode->i_attr_mutex);

out:
	inode_put(inode);
	super_block_put(sb);
	return res->status;
}

static int nfs3_op_rmdir(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	struct super_block *sb = NULL;
	struct inode *inode = NULL;
	struct inode *exists = NULL;
	struct reffs_dirent *rd = NULL;

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

	trace_nfs3_srv_rmdir(rt, args);

	res->status = rpc_cred_to_authunix_parms(&rt->rt_info.ri_cred, &ap);
	if (res->status)
		goto out;

	if (args->object.dir.data.data_len != sizeof(*nfh)) {
		res->status = NFS3ERR_BADHANDLE;
		goto out;
	}

	nfh = (struct network_file_handle *)args->object.dir.data.data_val;

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		res->status = NFS3ERR_STALE;
		goto out;
	}

	if (strlen(args->object.name) > NFS3_NAME_MAX) {
		res->status = NFS3ERR_NAMETOOLONG;
		goto out;
	}

	inode = directory_inode_find(sb, nfh->nfh_ino, &ap, W_OK, &res->status);
	if (res->status)
		goto out;

	pthread_mutex_lock(&inode->i_attr_mutex);

	size = inode->i_size;
	timespec_to_nfstime3(&inode->i_ctime, &ctime);
	timespec_to_nfstime3(&inode->i_mtime, &mtime);

	pthread_rwlock_wrlock(&inode->i_parent->rd_rwlock);

	exists = inode_name_get_inode(inode, args->object.name);
	if (!exists) {
		res->status = NFS3ERR_NOENT;
		wcc = &res->RMDIR3res_u.resfail.dir_wcc;
		pthread_rwlock_unlock(&inode->i_parent->rd_rwlock);
		goto update_wcc;
	}

	if (!(exists->i_mode & S_IFDIR)) {
		res->status = NFS3ERR_NOTDIR;
		wcc = &res->RMDIR3res_u.resfail.dir_wcc;
		pthread_rwlock_unlock(&inode->i_parent->rd_rwlock);
		goto update_wcc;
	}

	if (exists->i_nlink > 2) {
		res->status = NFS3ERR_NOTEMPTY;
		wcc = &res->RMDIR3res_u.resfail.dir_wcc;
		pthread_rwlock_unlock(&inode->i_parent->rd_rwlock);
		goto update_wcc;
	}

	rd = dirent_find(inode->i_parent, reffs_case_get(), args->object.name);
	if (!rd) {
		res->status = NFS3ERR_NOENT;
		wcc = &res->RMDIR3res_u.resfail.dir_wcc;
		pthread_rwlock_unlock(&inode->i_parent->rd_rwlock);
		goto update_wcc;
	}

	dirent_parent_release(rd, reffs_life_action_death);
	dirent_put(rd); // One for remove
	dirent_put(rd); // One for the find
	pthread_rwlock_unlock(&inode->i_parent->rd_rwlock);

update_wcc:
	wcc->before.attributes_follow = true;
	wcc->before.pre_op_attr_u.attributes.size = size;
	wcc->before.pre_op_attr_u.attributes.mtime = mtime;
	wcc->before.pre_op_attr_u.attributes.ctime = ctime;

	wcc->after.attributes_follow = true;
	fa = &wcc->after.post_op_attr_u.attributes;

	inode_attr_to_fattr(inode, fa);

	pthread_mutex_unlock(&inode->i_attr_mutex);

out:
	inode_put(exists);
	inode_put(inode);
	super_block_put(sb);
	return res->status;
}

static int nfs3_op_rename(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	struct super_block *sb = NULL;

	struct inode *inode_src = NULL;
	struct inode *inode_dst = NULL;

	struct reffs_dirent *rd_src = NULL;
	struct reffs_dirent *rd_dst = NULL;

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

	trace_nfs3_srv_rename(rt, args);

	res->status = rpc_cred_to_authunix_parms(&rt->rt_info.ri_cred, &ap);
	if (res->status)
		goto out;

	if (args->from.dir.data.data_len != sizeof(*nfh_src)) {
		res->status = NFS3ERR_BADHANDLE;
		goto out;
	}

	nfh_src = (struct network_file_handle *)args->from.dir.data.data_val;

	if (args->to.dir.data.data_len != sizeof(*nfh_dst)) {
		res->status = NFS3ERR_BADHANDLE;
		goto out;
	}

	nfh_dst = (struct network_file_handle *)args->to.dir.data.data_val;

	if (nfh_src->nfh_sb != nfh_dst->nfh_sb) {
		res->status = NFS3ERR_XDEV;
		goto out;
	}

	sb = super_block_find(nfh_src->nfh_sb);
	if (!sb) {
		res->status = NFS3ERR_STALE;
		goto out;
	}

	inode_src = directory_inode_find(sb, nfh_src->nfh_ino, &ap, W_OK,
					 &res->status);
	if (res->status)
		goto out;

	inode_dst = directory_inode_find(sb, nfh_dst->nfh_ino, &ap, W_OK,
					 &res->status);
	if (res->status)
		goto out;

	/*
	 * Note: Hold the d_rwlocks longer than we need to!
	 */
	if (inode_src == inode_dst) {
		pthread_mutex_lock(&inode_src->i_attr_mutex);
		pthread_rwlock_wrlock(&inode_src->i_parent->rd_rwlock);
	} else if (inode_src->i_ino < inode_dst->i_ino) {
		pthread_mutex_lock(&inode_src->i_attr_mutex);
		pthread_rwlock_wrlock(&inode_src->i_parent->rd_rwlock);
		pthread_mutex_lock(&inode_dst->i_attr_mutex);
		pthread_rwlock_wrlock(&inode_dst->i_parent->rd_rwlock);
	} else {
		pthread_mutex_lock(&inode_dst->i_attr_mutex);
		pthread_rwlock_wrlock(&inode_dst->i_parent->rd_rwlock);
		pthread_mutex_lock(&inode_src->i_attr_mutex);
		pthread_rwlock_wrlock(&inode_src->i_parent->rd_rwlock);
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

	rd_src = dirent_find(inode_src->i_parent, rtc, args->from.name);
	if (!rd_src) {
		res->status = NFS3ERR_NOENT;
		wcc_src = &res->RENAME3res_u.resfail.fromdir_wcc;
		wcc_dst = &res->RENAME3res_u.resfail.todir_wcc;
		goto update_wcc;
	}

	rd_dst = dirent_find(inode_dst->i_parent, rtc, args->to.name);

	if (strlen(args->to.name) > NFS3_NAME_MAX) {
		res->status = NFS3ERR_NAMETOOLONG;
		goto out;
	}

	name = strdup(args->to.name);
	if (!name) {
		res->status = NFS3ERR_JUKEBOX;
		wcc_src = &res->RENAME3res_u.resfail.fromdir_wcc;
		wcc_dst = &res->RENAME3res_u.resfail.todir_wcc;
		goto update_wcc;
	}

	rcu_read_lock();
	old = rcu_xchg_pointer(&rd_src->rd_name, name);
	reffs_string_release(old);

	if (inode_src->i_parent == inode_dst->i_parent) {
		if (rd_dst) {
			// Rename over file in same dir
			dirent_parent_release(rd_dst, reffs_life_action_death);
		}
	} else {
		if (rd_dst) {
			// Rename over file in different dir
			dirent_parent_release(rd_dst, reffs_life_action_death);
		}

		dirent_parent_release(rd_src, reffs_life_action_update);
		dirent_parent_attach(rd_src, inode_dst->i_parent,
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

	inode_attr_to_fattr(inode_dst, fa);

	if (inode_src == inode_dst) {
		pthread_rwlock_unlock(&inode_src->i_parent->rd_rwlock);
		pthread_mutex_unlock(&inode_src->i_attr_mutex);
	} else if (inode_src->i_ino < inode_dst->i_ino) {
		pthread_rwlock_unlock(&inode_dst->i_parent->rd_rwlock);
		pthread_mutex_unlock(&inode_dst->i_attr_mutex);
		pthread_rwlock_unlock(&inode_src->i_parent->rd_rwlock);
		pthread_mutex_unlock(&inode_src->i_attr_mutex);
	} else {
		pthread_rwlock_unlock(&inode_src->i_parent->rd_rwlock);
		pthread_mutex_unlock(&inode_src->i_attr_mutex);
		pthread_rwlock_unlock(&inode_dst->i_parent->rd_rwlock);
		pthread_mutex_unlock(&inode_dst->i_attr_mutex);
	}

out:
	dirent_put(rd_dst);
	inode_put(inode_dst);
	dirent_put(rd_src);
	inode_put(inode_src);
	super_block_put(sb);
	return res->status;
}

static int nfs3_op_link(struct rpc_trans *rt)
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

	struct reffs_dirent *rd = NULL;
	struct authunix_parms ap;

	trace_nfs3_srv_link(rt, args);

	res->status = rpc_cred_to_authunix_parms(&rt->rt_info.ri_cred, &ap);
	if (res->status)
		goto out;

	if (args->file.data.data_len != sizeof(*nfh)) {
		res->status = NFS3ERR_BADHANDLE;
		goto out;
	}

	if (args->link.dir.data.data_len != sizeof(*nfh)) {
		res->status = NFS3ERR_BADHANDLE;
		goto out;
	}

	nfh = (struct network_file_handle *)args->file.data.data_val;
	nfh_dir = (struct network_file_handle *)args->link.dir.data.data_val;

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

	inode_dir = directory_inode_find(sb, nfh_dir->nfh_ino, &ap, W_OK,
					 &res->status);
	if (!inode_dir) {
		res->status = NFS3ERR_NOENT;
		goto out;
	}

	if (!(inode_dir->i_mode & S_IFDIR)) {
		res->status = NFS3ERR_NOTDIR;
		goto out;
	}

	if (strlen(args->link.name) > NFS3_NAME_MAX) {
		res->status = NFS3ERR_NAMETOOLONG;
		goto out;
	}

	pthread_mutex_lock(&inode_dir->i_attr_mutex);
	pthread_mutex_lock(&inode->i_attr_mutex);

	size = inode_dir->i_size;
	timespec_to_nfstime3(&inode_dir->i_ctime, &ctime);
	timespec_to_nfstime3(&inode_dir->i_mtime, &mtime);

	pthread_rwlock_wrlock(&inode_dir->i_parent->rd_rwlock);
	exists = inode_name_get_inode(inode_dir, args->link.name);
	if (exists) {
		res->status = NFS3ERR_EXIST;
		pthread_rwlock_unlock(&inode_dir->i_parent->rd_rwlock);
		wcc = &resfail->linkdir_wcc;
		poa = &resfail->file_attributes;
		goto update_wcc;
	}

	rd = dirent_alloc(inode_dir->i_parent, args->link.name,
			  reffs_life_action_birth);
	if (!rd) {
		res->status = NFS3ERR_NOENT;
		pthread_rwlock_unlock(&inode_dir->i_parent->rd_rwlock);
		wcc = &resfail->linkdir_wcc;
		poa = &resfail->file_attributes;
		goto update_wcc;
	}

	rd->rd_inode = inode_get(inode);
	if (!rd->rd_inode) {
		dirent_parent_release(rd, reffs_life_action_death);
		res->status = NFS3ERR_NOENT;
		pthread_rwlock_unlock(&inode->i_parent->rd_rwlock);
		wcc = &resfail->linkdir_wcc;
		poa = &resfail->file_attributes;
		goto update_wcc;
	}

	__atomic_fetch_add(&inode->i_nlink, 1, __ATOMIC_RELAXED);

	pthread_rwlock_unlock(&inode_dir->i_parent->rd_rwlock);

	inode_update_times_now(inode, REFFS_INODE_UPDATE_CTIME);

update_wcc:
	wcc->before.attributes_follow = true;
	wcc->before.pre_op_attr_u.attributes.size = size;
	wcc->before.pre_op_attr_u.attributes.mtime = mtime;
	wcc->before.pre_op_attr_u.attributes.ctime = ctime;

	poa->attributes_follow = true;
	fa = &poa->post_op_attr_u.attributes;
	inode_attr_to_fattr(inode, fa);

	wcc->after.attributes_follow = true;
	fa = &wcc->after.post_op_attr_u.attributes;
	inode_attr_to_fattr(inode_dir, fa);

	pthread_mutex_unlock(&inode->i_attr_mutex);
	pthread_mutex_unlock(&inode_dir->i_attr_mutex);

out:
	inode_put(exists);
	inode_put(inode);
	inode_put(inode_dir);
	super_block_put(sb);
	return res->status;
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

	struct reffs_dirent *rd = NULL;
	struct authunix_parms ap;

	entry3 *e_next = NULL;

	trace_nfs3_srv_readdir(rt, args);

	res->status = rpc_cred_to_authunix_parms(&rt->rt_info.ri_cred, &ap);
	if (res->status)
		goto out;

	if (args->dir.data.data_len != sizeof(*nfh)) {
		res->status = NFS3ERR_BADHANDLE;
		goto out;
	}

	nfh = (struct network_file_handle *)args->dir.data.data_val;

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		res->status = NFS3ERR_STALE;
		goto out;
	}

	inode = directory_inode_find(sb, nfh->nfh_ino, &ap, W_OK, &res->status);
	if (res->status)
		goto out;

	count = sizeof(READDIR3res);
	if (count > args->count) {
		res->status = NFS3ERR_INVAL;
		goto out;
	}

	pthread_mutex_lock(&inode->i_attr_mutex);
	pthread_rwlock_rdlock(&inode->i_parent->rd_rwlock);

	/* Determine parent ino for .. entry */
	uint64_t parent_ino =
		(inode->i_parent && inode->i_parent->rd_inode) ?
			inode->i_parent->rd_inode->i_ino :
			inode->i_ino; /* root: .. points to self */

	dl = &resok->reply;

	if (cookie == 0) {
		entry3 *e = calloc(1, sizeof(*e));
		if (!e) {
			if (!dl->entries) {
				free(dl);
				res->status = NFS3ERR_JUKEBOX;
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
				res->status = NFS3ERR_JUKEBOX;
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
				res->status = NFS3ERR_JUKEBOX;
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
				res->status = NFS3ERR_JUKEBOX;
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

	rcu_read_lock();
	cds_list_for_each_entry_rcu(rd, &inode->i_parent->rd_inode->i_children,
				    rd_siblings) {
		if (rd->rd_cookie <= cookie)
			continue;

		entry3 *e = calloc(1, sizeof(*e));
		if (!e) {
			if (!dl->entries) {
				free(dl);
				res->status = NFS3ERR_JUKEBOX;
				poa = &res->READDIR3res_u.resfail.dir_attributes;
				rcu_read_unlock();
				goto update_wcc;
			}

			break;
		}

		e->fileid = rd->rd_inode->i_ino;
		e->cookie = rd->rd_cookie;
		e->name = strdup(rd->rd_name);
		if (!e->name) {
			free(e);
			if (!dl->entries) {
				free(dl);
				res->status = NFS3ERR_JUKEBOX;
				poa = &res->READDIR3res_u.resfail.dir_attributes;
				rcu_read_unlock();
				goto update_wcc;
			}

			break;
		}

		count += sizeof(*e) + strlen(e->name) + 1;
		if (count > args->count) {
			free(e->name);
			free(e);
			rcu_read_unlock();
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

	pthread_rwlock_unlock(&inode->i_parent->rd_rwlock);
	pthread_mutex_unlock(&inode->i_attr_mutex);

out:
	inode_put(inode);
	super_block_put(sb);
	return res->status;
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

	struct reffs_dirent *rd = NULL;
	struct authunix_parms ap;

	entryplus3 *e_next = NULL;

	trace_nfs3_srv_readdirplus(rt, args);

	res->status = rpc_cred_to_authunix_parms(&rt->rt_info.ri_cred, &ap);
	if (res->status)
		goto out;

	if (args->dir.data.data_len != sizeof(*nfh)) {
		res->status = NFS3ERR_BADHANDLE;
		goto out;
	}

	nfh = (struct network_file_handle *)args->dir.data.data_val;

	sb = super_block_find(nfh->nfh_sb);
	if (!sb) {
		res->status = NFS3ERR_STALE;
		goto out;
	}

	inode = directory_inode_find(sb, nfh->nfh_ino, &ap, W_OK, &res->status);
	if (res->status)
		goto out;

	maxcount = sizeof(READDIRPLUS3res);
	if (maxcount > args->maxcount) {
		res->status = NFS3ERR_INVAL;
		goto out;
	}

	pthread_mutex_lock(&inode->i_attr_mutex);
	pthread_rwlock_rdlock(&inode->i_parent->rd_rwlock);

	dl = &resok->reply;

	/* Determine parent ino for .. entry */
	uint64_t parent_ino =
		(inode->i_parent && inode->i_parent->rd_inode) ?
			inode->i_parent->rd_inode->i_ino :
			inode->i_ino; /* root: .. points to self */

	if (cookie == 0) {
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

		e->fileid = inode->i_ino;
		e->cookie = 1;
		e->name = strdup(".");
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
		nfh = network_file_handle_construct(sb->sb_id, inode->i_ino);
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

	rcu_read_lock();
	cds_list_for_each_entry_rcu(rd, &inode->i_parent->rd_inode->i_children,
				    rd_siblings) {
		if (rd->rd_cookie < cookie)
			continue;

		entryplus3 *e = calloc(1, sizeof(*e));
		if (!e) {
			if (!dl->entries) {
				free(dl);
				res->status = NFS3ERR_JUKEBOX;
				poa = &res->READDIRPLUS3res_u.resfail
					       .dir_attributes;
				rcu_read_unlock();
				goto update_wcc;
			}

			break;
		}

		e->fileid = rd->rd_inode->i_ino;
		e->cookie = rd->rd_cookie;
		e->name = strdup(rd->rd_name);
		if (!e->name) {
			free(e);
			if (!dl->entries) {
				free(dl);
				res->status = NFS3ERR_JUKEBOX;
				poa = &res->READDIRPLUS3res_u.resfail
					       .dir_attributes;
				rcu_read_unlock();
				goto update_wcc;
			}

			break;
		}

		// With multi-sb, check for mounted on
		nfh = network_file_handle_construct(sb->sb_id,
						    rd->rd_inode->i_ino);
		if (!nfh) {
			free(e->name);
			free(e);

			if (!dl->entries) {
				free(dl);
				res->status = NFS3ERR_JUKEBOX;
				poa = &res->READDIRPLUS3res_u.resfail
					       .dir_attributes;
				rcu_read_unlock();
				goto update_wcc;
			}

			break;
		}

		e->name_handle.post_op_fh3_u.handle.data.data_val = (char *)nfh;
		e->name_handle.post_op_fh3_u.handle.data.data_len =
			sizeof(*nfh);
		e->name_handle.handle_follows = true;

		poa_e = &e->name_attributes;
		poa_e->attributes_follow = true;
		fa = &poa_e->post_op_attr_u.attributes;
		inode_attr_to_fattr(rd->rd_inode, fa);

		dircount += sizeof(*e) - sizeof(post_op_attr);
		if (dircount > args->dircount) {
			free(nfh);
			free(e->name);
			free(e);
			rcu_read_unlock();
			goto past_eof;
		}

		maxcount += sizeof(*e) + strlen(e->name) + sizeof(*nfh) + 1;
		if (maxcount > args->maxcount) {
			free(nfh);
			free(e->name);
			free(e);
			rcu_read_unlock();
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

	pthread_rwlock_unlock(&inode->i_parent->rd_rwlock);
	pthread_mutex_unlock(&inode->i_attr_mutex);

out:
	inode_put(inode);
	super_block_put(sb);
	return res->status;
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

	trace_nfs3_srv_fsstat(rt, args);

	res->status = rpc_cred_to_authunix_parms(&rt->rt_info.ri_cred, &ap);
	if (res->status)
		goto out;

	if (args->fsroot.data.data_len != sizeof(*nfh)) {
		res->status = NFS3ERR_BADHANDLE;
		goto out;
	}

	nfh = (struct network_file_handle *)args->fsroot.data.data_val;

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

	res->status = inode_access_check(inode, &ap, R_OK);
	if (res->status)
		goto out;

	pthread_mutex_lock(&inode->i_attr_mutex);

	inode_update_times_now(inode, REFFS_INODE_UPDATE_ATIME);

	resok->tbytes = sb->sb_bytes_max;
	resok->fbytes = sb->sb_bytes_max - sb->sb_bytes_used;
	resok->abytes = resok->fbytes;
	resok->tfiles = sb->sb_inodes_max;
	resok->ffiles = sb->sb_inodes_used;
	resok->afiles = resok->tfiles - resok->ffiles;
	resok->invarsec = 0;

	poa->attributes_follow = true;
	fattr3 *fa = &poa->post_op_attr_u.attributes;
	inode_attr_to_fattr(inode, fa);

	pthread_mutex_unlock(&inode->i_attr_mutex);

out:
	inode_put(inode);
	super_block_put(sb);
	return res->status;
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

	trace_nfs3_srv_fsinfo(rt, args);

	res->status = rpc_cred_to_authunix_parms(&rt->rt_info.ri_cred, &ap);
	if (res->status)
		goto out;

	if (args->fsroot.data.data_len != sizeof(*nfh)) {
		res->status = NFS3ERR_BADHANDLE;
		goto out;
	}

	nfh = (struct network_file_handle *)args->fsroot.data.data_val;

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

	res->status = inode_access_check(inode, &ap, R_OK);
	if (res->status)
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
	inode_put(inode);
	super_block_put(sb);
	return res->status;
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

	trace_nfs3_srv_pathconf(rt, args);

	res->status = rpc_cred_to_authunix_parms(&rt->rt_info.ri_cred, &ap);
	if (res->status)
		goto out;

	if (args->object.data.data_len != sizeof(*nfh)) {
		res->status = NFS3ERR_BADHANDLE;
		goto out;
	}

	nfh = (struct network_file_handle *)args->object.data.data_val;

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

	res->status = inode_access_check(inode, &ap, R_OK);
	if (res->status)
		goto out;

	pthread_mutex_lock(&inode->i_attr_mutex);

	inode_update_times_now(inode, REFFS_INODE_UPDATE_ATIME);
	poa->attributes_follow = true;
	fattr3 *fa = &poa->post_op_attr_u.attributes;
	inode_attr_to_fattr(inode, fa);

	pthread_mutex_unlock(&inode->i_attr_mutex);

	resok->linkmax = 255;
	resok->name_max = NFS3_NAME_MAX;
	resok->no_trunc = false;
	resok->chown_restricted = false;
	resok->case_insensitive =
		reffs_case_get() == reffs_text_case_insensitive ? true : false;
	resok->case_preserving = true;

out:
	inode_put(inode);
	super_block_put(sb);
	return res->status;
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

	uuid_t *uuid;

	trace_nfs3_srv_commit(rt, args);

	res->status = rpc_cred_to_authunix_parms(&rt->rt_info.ri_cred, &ap);
	if (res->status)
		goto out;

	if (args->file.data.data_len != sizeof(*nfh)) {
		res->status = NFS3ERR_BADHANDLE;
		goto out;
	}

	nfh = (struct network_file_handle *)args->file.data.data_val;

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

	res->status = inode_access_check(inode, &ap, R_OK);
	if (res->status)
		goto out;

	uuid = server_boot_uuid_get();
	memcpy(resok->verf, (*uuid) + 8, NFS3_WRITEVERFSIZE);

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
	inode_put(inode);
	super_block_put(sb);
	return res->status;
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
