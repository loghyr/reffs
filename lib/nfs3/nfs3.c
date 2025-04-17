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
#include "reffs/rpc.h"
#include "reffs/log.h"
#include "reffs/filehandle.h"
#include "reffs/time.h"
#include "reffs/inode.h"
#include "reffs/super_block.h"

static void print_nfs_fh3_hex(nfs_fh3 *fh)
{
	// Calculate CRC-32
	uint32_t crc =
		crc32(0L, (const Bytef *)fh->data.data_val, fh->data.data_len);

	printf("File handle (length %u):\n", fh->data.data_len);
	printf("[hash (CRC-32): 0x%08x]\n", crc);
	printf("FileHandle: ");

	// Print bytes in hex format
	unsigned char *bytes = (unsigned char *)fh->data.data_val;
	for (u_int i = 0; i < fh->data.data_len; i++) {
		printf("%02x", bytes[i]);

		// Optional formatting for readability
		if ((i + 1) % 16 == 0) {
			printf("\n");
		} else if (i != fh->data.data_len - 1) {
			printf(" ");
		}
	}
	printf("\n");
}

static bool nfs3_gid_in_gids(gid_t gid, uint32_t len, gid_t *gids)
{
	for (uint32_t i = 0; i < len; len++)
		if (gid == gids[i])
			return true;

	return false;
}

static int nfs3_access_check(struct inode *inode, struct rpc_cred *cred,
			     int mode)
{
	uid_t uid = 65534;
	gid_t gid = 65534;

	uint32_t len = 0;
	gid_t *gids = NULL;

	switch (cred->rc_flavor) {
	case AUTH_SYS:
		uid = cred->rc_unix.aup_uid;
		gid = cred->rc_unix.aup_gid;
		len = cred->rc_unix.aup_len;
		gids = cred->rc_unix.aup_gids;
		break;
	case AUTH_NONE:
		break;
	default:
		return EACCES; // Should have already been done at RPC layer
	}

	if (uid == inode->i_uid) {
		if ((mode & W_OK) && !(inode->i_mode & S_IWUSR))
			return EACCES;
		if ((mode & R_OK) && !(inode->i_mode & S_IRUSR))
			return EACCES;
		if ((mode & X_OK) && !(inode->i_mode & S_IXUSR))
			return EACCES;
	} else if (gid == inode->i_gid ||
		   nfs3_gid_in_gids(inode->i_gid, len, gids)) {
		if ((mode & W_OK) && !(inode->i_mode & S_IWGRP))
			return EACCES;
		if ((mode & R_OK) && !(inode->i_mode & S_IRGRP))
			return EACCES;
		if ((mode & X_OK) && !(inode->i_mode & S_IXGRP))
			return EACCES;
	} else {
		if ((mode & W_OK) && !(inode->i_mode & S_IWOTH))
			return EACCES;
		if ((mode & R_OK) && !(inode->i_mode & S_IROTH))
			return EACCES;
		if ((mode & X_OK) && !(inode->i_mode & S_IXOTH))
			return EACCES;
	}

	return 0;
}

static int nfs3_null(struct rpc_trans *rt)
{
	TRACE("NULL: 0x%x", rt->rt_info.ri_xid);
	return 0;
}

static int nfs3_getattr(struct rpc_trans *rt)
{
	TRACE("GETATTR: 0x%x", rt->rt_info.ri_xid);
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	struct super_block *sb = NULL;
	struct inode *inode = NULL;

	GETATTR3args *args = ph->ph_args;
	GETATTR3res *res = ph->ph_res;
	fattr3 *fa = &res->GETATTR3res_u.resok.obj_attributes;

	struct network_file_handle *nfh = NULL;

	if (args->object.data.data_len != sizeof(*nfh)) {
		res->status = NFS3ERR_BADHANDLE;
		goto out;
	}

	nfh = (struct network_file_handle *)args->object.data.data_val;

	sb = super_block_find(nfh->nfh_fsid);
	if (!sb) {
		res->status = NFS3ERR_STALE;
		goto out;
	}

	inode = inode_find(sb, nfh->nfh_ino);
	if (!inode) {
		res->status = NFS3ERR_NOENT;
		goto out;
	}

	res->status = nfs3_access_check(inode, &rt->rt_info.ri_cred, R_OK);
	if (res->status)
		goto out;

	fa->mode = inode->i_mode;
	fa->nlink = inode->i_nlink;
	fa->uid = inode->i_uid;
	fa->gid = inode->i_gid;
	fa->size = inode->i_size;
	fa->used = inode->i_used;
	// fa->rdev = 0;  Implement once we do these types
	fa->fsid = nfh->nfh_fsid;
	fa->fileid = inode->i_ino;
	timespec_to_nfstime3(&inode->i_atime, &fa->atime);
	timespec_to_nfstime3(&inode->i_mtime, &fa->mtime);
	timespec_to_nfstime3(&inode->i_ctime, &fa->ctime);

	print_nfs_fh3_hex(&args->object);

out:
	inode_put(inode);
	super_block_put(sb);
	return res->status;
}

static int nfs3_setattr(struct rpc_trans *rt)
{
	TRACE("SETATTR: 0x%x", rt->rt_info.ri_xid);
	return 0;
}

static int nfs3_lookup(struct rpc_trans *rt)
{
	TRACE("LOOKUP: 0x%x", rt->rt_info.ri_xid);
	return 0;
}

static int nfs3_access(struct rpc_trans *rt)
{
	TRACE("ACCESS: 0x%x", rt->rt_info.ri_xid);
	return 0;
}

static int nfs3_readlink(struct rpc_trans *rt)
{
	TRACE("READLINK: 0x%x", rt->rt_info.ri_xid);
	return 0;
}

static int nfs3_read(struct rpc_trans *rt)
{
	TRACE("READ: 0x%x", rt->rt_info.ri_xid);
	return 0;
}

static int nfs3_write(struct rpc_trans *rt)
{
	TRACE("WRITE: 0x%x", rt->rt_info.ri_xid);
	return 0;
}

static int nfs3_create(struct rpc_trans *rt)
{
	TRACE("CREATE: 0x%x", rt->rt_info.ri_xid);
	return 0;
}

static int nfs3_mkdir(struct rpc_trans *rt)
{
	TRACE("MKDIR: 0x%x", rt->rt_info.ri_xid);
	return 0;
}

static int nfs3_symlink(struct rpc_trans *rt)
{
	TRACE("SYMLINK: 0x%x", rt->rt_info.ri_xid);
	return 0;
}

static int nfs3_mknod(struct rpc_trans *rt)
{
	TRACE("MKNOD: 0x%x", rt->rt_info.ri_xid);
	return 0;
}

static int nfs3_remove(struct rpc_trans *rt)
{
	TRACE("REMOVE: 0x%x", rt->rt_info.ri_xid);
	return 0;
}

static int nfs3_rmdir(struct rpc_trans *rt)
{
	TRACE("RMDIR: 0x%x", rt->rt_info.ri_xid);
	return 0;
}

static int nfs3_rename(struct rpc_trans *rt)
{
	TRACE("RENAME: 0x%x", rt->rt_info.ri_xid);
	return 0;
}

static int nfs3_link(struct rpc_trans *rt)
{
	TRACE("LINK: 0x%x", rt->rt_info.ri_xid);
	return 0;
}

static int nfs3_readdir(struct rpc_trans *rt)
{
	TRACE("READDIR: 0x%x", rt->rt_info.ri_xid);
	return 0;
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
