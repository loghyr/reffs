/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <rpc/xdr.h>

struct rpc_handler {
	int (*rh_decode)(char *buffer, int len);
	int (*rh_encode)(char *buffer, int len);
};

struct rpc_program_handler {
	uint32_t rph_program;
	uint32_t rph_version;
	uint32_t rph_operation;
	xdrproc_t rph_decode;
	void *rph_args;
	xdrproc_t rph_encode;
	void *rph_results;
	int (*rph_handler)(void *args);
};

struct rpc_program_handler nfsv3_program_handler[] = {
	{ NFS3_PROGRAM, NFS_V3, NFSPROC3_NULL, NULL, NULL, NULL, NULL, nfs3_null },
	{ NFS3_PROGRAM, NFS_V3, NFSPROC3_GETATTR, xdr_GETATTR3args, GETATTR3args, xdr_GETATTR3res, GETATTR3res, nfs3_getattr },
	{ NFS3_PROGRAM, NFS_V3, NFSPROC3_SETATTR, xdr_SETATTR3args, SETATTR3args, xdr_SETATTR3res, SETATTR3res, nfs3_setattr },
	{ NFS3_PROGRAM, NFS_V3, NFSPROC3_LOOKUP, xdr_LOOKUP3args, LOOKUP3args, xdr_LOOKUP3res, LOOKUP3res, nfs3_lookup },
	{ NFS3_PROGRAM, NFS_V3, NFSPROC3_ACCESS, xdr_ACCESS3args, ACCESS3args, xdr_ACCESS3res, ACCESS3res, nfs3_access },
	{ NFS3_PROGRAM, NFS_V3, NFSPROC3_READLINK, xdr_READLINK3args, READLINK3args, xdr_READLINK3res, READLINK3res, nfs3_readlink },
	{ NFS3_PROGRAM, NFS_V3, NFSPROC3_READ, xdr_READ3args, READ3args, xdr_READ3res, READ3res, nfs3_read },
	{ NFS3_PROGRAM, NFS_V3, NFSPROC3_WRITE, xdr_WRITE3args, WRITE3args, xdr_WRITE3res, WRITE3res, nfs3_write },
	{ NFS3_PROGRAM, NFS_V3, NFSPROC3_CREATE, xdr_CREATE3args, CREATE3args, xdr_CREATE3res, CREATE3res, nfs3_create },
	{ NFS3_PROGRAM, NFS_V3, NFSPROC3_MKDIR, xdr_MKDIR3args, MKDIR3args, xdr_MKDIR3res, MKDIR3res, nfs3_mkdir },
	{ NFS3_PROGRAM, NFS_V3, NFSPROC3_SYMLINK, xdr_SYMLINK3args, SYMLINK3args, xdr_SYMLINK3res, SYMLINK3res, nfs3_symlink },
	{ NFS3_PROGRAM, NFS_V3, NFSPROC3_MKNOD, xdr_MKNOD3args, MKNOD3args, xdr_MKNOD3res, MKNOD3res, nfs3_mknod },
	{ NFS3_PROGRAM, NFS_V3, NFSPROC3_REMOVE, xdr_REMOVE3args, REMOVE3args, xdr_REMOVE3res, REMOVE3res, nfs3_remove },
	{ NFS3_PROGRAM, NFS_V3, NFSPROC3_RMDIR, xdr_RMDIR3args, RMDIR3args, xdr_RMDIR3res, RMDIR3res, nfs3_rmdir },
	{ NFS3_PROGRAM, NFS_V3, NFSPROC3_RENAME, xdr_RENAME3args, RENAME3args, xdr_RENAME3res, RENAME3res, nfs3_rename },
	{ NFS3_PROGRAM, NFS_V3, NFSPROC3_LINK, xdr_LINK3args, LINK3args, xdr_LINK3res, LINK3res, nfs3_link },
	{ NFS3_PROGRAM, NFS_V3, NFSPROC3_READDIR, xdr_READDIR3args, READDIR3args, xdr_READDIR3res, READDIR3res, nfs3_readdir },
	{ NFS3_PROGRAM, NFS_V3, NFSPROC3_READDIRPLUS, xdr_READDIRPLUS3args, READDIRPLUS3args, xdr_READDIRPLUS3res, READDIRPLUS3res, nfs3_readdirplus },
	{ NFS3_PROGRAM, NFS_V3, NFSPROC3_FSSTAT, xdr_FSSTAT3args, FSSTAT3args, xdr_FSSTAT3res, FSSTAT3res, nfs3_fsstat },
	{ NFS3_PROGRAM, NFS_V3, NFSPROC3_FSINFO, xdr_FSINFO3args, FSINFO3args, xdr_FSINFO3res, FSINFO3res, nfs3_fsinfo },
	{ NFS3_PROGRAM, NFS_V3, NFSPROC3_PATHCONF, xdr_PATHCONF3args, PATHCONF3args, xdr_PATHCONF3res, PATHCONF3res, nfs3_pathconf },
	{ NFS3_PROGRAM, NFS_V3, NFSPROC3_COMMIT, xdr_COMMIT3args, COMMIT3args, xdr_COMMIT3res, COMMIT3res, nfs3_commit },
};
