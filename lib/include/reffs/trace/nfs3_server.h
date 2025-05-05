/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_TRACE_NFS3_SERVER_H
#define _REFFS_TRACE_NFS3_SERVER_H

#include <stdbool.h>
#include <stdint.h>
#include "reffs/trace/common.h"
#include "reffs/rpc.h"
#include "nfsv3_xdr.h"

static inline uint32_t nfs3_getfh_crc(nfs_fh3 *fh)
{
	return crc32(0L, (const Bytef *)fh->data.data_val, fh->data.data_len);
}

/* NFS3 operation trace functions */
static inline void trace_nfs3_srv_null(struct rpc_trans *rt)
{
	reffs_trace_event(REFFS_TRACE_CAT_NFS, "nfs3_null", __LINE__,
			  "xid=0x%08x", rt->rt_info.ri_xid);
}

static inline void trace_nfs3_srv_getattr(struct rpc_trans *rt,
					  GETATTR3args *args)
{
	uint32_t crc = nfs3_getfh_crc(&args->object);
	struct network_file_handle *nfh =
		(struct network_file_handle *)args->object.data.data_val;

	reffs_trace_event(REFFS_TRACE_CAT_NFS, "nfs3_getattr", __LINE__,
			  "xid=0x%08x sb=%lu ino=%lu crc=0x%08x",
			  rt->rt_info.ri_xid, nfh->nfh_sb, nfh->nfh_ino, crc);
}

static inline void trace_nfs3_srv_setattr(struct rpc_trans *rt,
					  SETATTR3args *args)
{
	uint32_t crc = nfs3_getfh_crc(&args->object);
	struct network_file_handle *nfh =
		(struct network_file_handle *)args->object.data.data_val;

	reffs_trace_event(REFFS_TRACE_CAT_NFS, "nfs3_setattr", __LINE__,
			  "xid=0x%08x sb=%lu ino=%lu crc=0x%08x",
			  rt->rt_info.ri_xid, nfh->nfh_sb, nfh->nfh_ino, crc);
}

static inline void trace_nfs3_srv_lookup(struct rpc_trans *rt,
					 LOOKUP3args *args)
{
	uint32_t crc = nfs3_getfh_crc(&args->what.dir);
	struct network_file_handle *nfh =
		(struct network_file_handle *)args->what.dir.data.data_val;

	reffs_trace_event(REFFS_TRACE_CAT_NFS, "nfs3_lookup", __LINE__,
			  "xid=0x%08x sb=%lu ino=%lu crc=0x%08x",
			  rt->rt_info.ri_xid, nfh->nfh_sb, nfh->nfh_ino, crc);
}

static inline void trace_nfs3_srv_access(struct rpc_trans *rt,
					 ACCESS3args *args)
{
	uint32_t crc = nfs3_getfh_crc(&args->object);
	struct network_file_handle *nfh =
		(struct network_file_handle *)args->object.data.data_val;

	reffs_trace_event(REFFS_TRACE_CAT_NFS, "nfs3_access", __LINE__,
			  "xid=0x%08x sb=%lu ino=%lu crc=0x%08x",
			  rt->rt_info.ri_xid, nfh->nfh_sb, nfh->nfh_ino, crc);
}

static inline void trace_nfs3_srv_readlink(struct rpc_trans *rt,
					   READLINK3args *args)
{
	uint32_t crc = nfs3_getfh_crc(&args->symlink);
	struct network_file_handle *nfh =
		(struct network_file_handle *)args->symlink.data.data_val;

	reffs_trace_event(REFFS_TRACE_CAT_NFS, "nfs3_readlink", __LINE__,
			  "xid=0x%08x sb=%lu ino=%lu crc=0x%08x",
			  rt->rt_info.ri_xid, nfh->nfh_sb, nfh->nfh_ino, crc);
}

static inline void trace_nfs3_srv_read(struct rpc_trans *rt, READ3args *args)
{
	uint32_t crc = nfs3_getfh_crc(&args->file);
	struct network_file_handle *nfh =
		(struct network_file_handle *)args->file.data.data_val;

	reffs_trace_event(
		REFFS_TRACE_CAT_NFS, "nfs3_read", __LINE__,
		"xid=0x%08x sb=%lu ino=%lu off=%zu count=%u crc=0x%08x",
		rt->rt_info.ri_xid, nfh->nfh_sb, nfh->nfh_ino, args->offset,
		args->count, crc);
}

static inline void trace_nfs3_srv_write(struct rpc_trans *rt, WRITE3args *args)
{
	uint32_t crc = nfs3_getfh_crc(&args->file);
	struct network_file_handle *nfh =
		(struct network_file_handle *)args->file.data.data_val;

	reffs_trace_event(REFFS_TRACE_CAT_NFS, "nfs3_write", __LINE__,
			  "xid=0x%08x sb=%lu ino=%lu off=%lu len=%u crc=0x%08x",
			  rt->rt_info.ri_xid, nfh->nfh_sb, nfh->nfh_ino,
			  args->offset, args->data.data_len, crc);
}

static inline void trace_nfs3_srv_create(struct rpc_trans *rt,
					 CREATE3args *args)
{
	uint32_t crc = nfs3_getfh_crc(&args->where.dir);
	struct network_file_handle *nfh =
		(struct network_file_handle *)args->where.dir.data.data_val;

	reffs_trace_event(REFFS_TRACE_CAT_NFS, "nfs3_create", __LINE__,
			  "xid=0x%08x sb=%lu ino=%lu name=%s crc=0x%08x",
			  rt->rt_info.ri_xid, nfh->nfh_sb, nfh->nfh_ino,
			  args->where.name, crc);
}

static inline void trace_nfs3_srv_mkdir(struct rpc_trans *rt, MKDIR3args *args)
{
	uint32_t crc = nfs3_getfh_crc(&args->where.dir);
	struct network_file_handle *nfh =
		(struct network_file_handle *)args->where.dir.data.data_val;

	reffs_trace_event(REFFS_TRACE_CAT_NFS, "nfs3_mkdir", __LINE__,
			  "xid=0x%08x sb=%lu ino=%lu name=%s crc=0x%08x",
			  rt->rt_info.ri_xid, nfh->nfh_sb, nfh->nfh_ino,
			  args->where.name, crc);
}

static inline void trace_nfs3_srv_symlink(struct rpc_trans *rt,
					  SYMLINK3args *args)
{
	uint32_t crc = nfs3_getfh_crc(&args->where.dir);
	struct network_file_handle *nfh =
		(struct network_file_handle *)args->where.dir.data.data_val;

	reffs_trace_event(REFFS_TRACE_CAT_NFS, "nfs3_symlink", __LINE__,
			  "xid=0x%08x sb=%lu ino=%lu name=%s crc=0x%08x",
			  rt->rt_info.ri_xid, nfh->nfh_sb, nfh->nfh_ino,
			  args->symlink.symlink_data, crc);
}

static inline void trace_nfs3_srv_mknod(struct rpc_trans *rt, MKNOD3args *args)
{
	uint32_t crc = nfs3_getfh_crc(&args->where.dir);
	struct network_file_handle *nfh =
		(struct network_file_handle *)args->where.dir.data.data_val;

	reffs_trace_event(REFFS_TRACE_CAT_NFS, "nfs3_mknod", __LINE__,
			  "xid=0x%08x sb=%lu ino=%lu name=%s crc=0x%08x",
			  rt->rt_info.ri_xid, nfh->nfh_sb, nfh->nfh_ino,
			  args->where.name, crc);
}

static inline void trace_nfs3_srv_remove(struct rpc_trans *rt,
					 REMOVE3args *args)
{
	uint32_t crc = nfs3_getfh_crc(&args->object.dir);
	struct network_file_handle *nfh =
		(struct network_file_handle *)args->object.dir.data.data_val;

	reffs_trace_event(REFFS_TRACE_CAT_NFS, "nfs3_remove", __LINE__,
			  "xid=0x%08x sb=%lu ino=%lu name=%s crc=0x%08x",
			  rt->rt_info.ri_xid, nfh->nfh_sb, nfh->nfh_ino,
			  args->object.name, crc);
}

static inline void trace_nfs3_srv_rmdir(struct rpc_trans *rt, RMDIR3args *args)
{
	uint32_t crc = nfs3_getfh_crc(&args->object.dir);
	struct network_file_handle *nfh =
		(struct network_file_handle *)args->object.dir.data.data_val;

	reffs_trace_event(REFFS_TRACE_CAT_NFS, "nfs3_rmdir", __LINE__,
			  "xid=0x%08x sb=%lu ino=%lu name=%s crc=0x%08x",
			  rt->rt_info.ri_xid, nfh->nfh_sb, nfh->nfh_ino,
			  args->object.name, crc);
}

static inline void trace_nfs3_srv_rename(struct rpc_trans *rt,
					 RENAME3args *args)
{
	uint32_t crc_src = nfs3_getfh_crc(&args->from.dir);
	uint32_t crc_to = nfs3_getfh_crc(&args->to.dir);
	struct network_file_handle *nfh_src =
		(struct network_file_handle *)args->from.dir.data.data_val;
	struct network_file_handle *nfh_dst =
		(struct network_file_handle *)args->to.dir.data.data_val;

	reffs_trace_event(
		REFFS_TRACE_CAT_NFS, "nfs3_rename", __LINE__,
		"xid=0x%08x sb_src=%lu ino_src=%lu sb_dst=%lu ino_dst=%lu name=%s crc_src=0x%08x crc_to=0x%08x",
		rt->rt_info.ri_xid, nfh_src->nfh_sb, nfh_src->nfh_ino,
		nfh_dst->nfh_sb, nfh_dst->nfh_ino, args->to.name, crc_src,
		crc_to);
}

static inline void trace_nfs3_srv_link(struct rpc_trans *rt, LINK3args *args)
{
	uint32_t crc = nfs3_getfh_crc(&args->file);
	uint32_t crc_dir = nfs3_getfh_crc(&args->link.dir);
	struct network_file_handle *nfh =
		(struct network_file_handle *)args->file.data.data_val;
	struct network_file_handle *nfh_dir =
		(struct network_file_handle *)args->link.dir.data.data_val;

	reffs_trace_event(
		REFFS_TRACE_CAT_NFS, "nfs3_link", __LINE__,
		"xid=0x%08x sb=%lu ino=%lu sb_dir=%lu ino_dir=%lu name=%s crc=0x%08x crc_dir=0x%08x",
		rt->rt_info.ri_xid, nfh->nfh_sb, nfh->nfh_ino, nfh_dir->nfh_sb,
		nfh_dir->nfh_ino, args->link.name, crc, crc_dir);
}

static inline void trace_nfs3_srv_readdir(struct rpc_trans *rt,
					  READDIR3args *args)
{
	uint32_t crc = nfs3_getfh_crc(&args->dir);
	struct network_file_handle *nfh =
		(struct network_file_handle *)args->dir.data.data_val;

	reffs_trace_event(REFFS_TRACE_CAT_NFS, "nfs3_readdir", __LINE__,
			  "xid=0x%08x sb=%lu ino=%lu cookie=0x%08lx crc=0x%08x",
			  rt->rt_info.ri_xid, nfh->nfh_sb, nfh->nfh_ino,
			  args->cookie, crc);
}

static inline void trace_nfs3_srv_readdirplus(struct rpc_trans *rt,
					      READDIRPLUS3args *args)
{
	uint32_t crc = nfs3_getfh_crc(&args->dir);
	struct network_file_handle *nfh =
		(struct network_file_handle *)args->dir.data.data_val;

	reffs_trace_event(REFFS_TRACE_CAT_NFS, "nfs3_readdirplus", __LINE__,
			  "xid=0x%08x sb=%lu ino=%lu cookie=0x%08lx crc=0x%08x",
			  rt->rt_info.ri_xid, nfh->nfh_sb, nfh->nfh_ino,
			  args->cookie, crc);
}

static inline void trace_nfs3_srv_fsstat(struct rpc_trans *rt,
					 FSSTAT3args *args)
{
	uint32_t crc = nfs3_getfh_crc(&args->fsroot);
	struct network_file_handle *nfh =
		(struct network_file_handle *)args->fsroot.data.data_val;

	reffs_trace_event(REFFS_TRACE_CAT_NFS, "nfs3_fsstat", __LINE__,
			  "xid=0x%08x sb=%lu ino=%lu crc=0x%08x",
			  rt->rt_info.ri_xid, nfh->nfh_sb, nfh->nfh_ino, crc);
}

static inline void trace_nfs3_srv_fsinfo(struct rpc_trans *rt,
					 FSINFO3args *args)
{
	uint32_t crc = nfs3_getfh_crc(&args->fsroot);
	struct network_file_handle *nfh =
		(struct network_file_handle *)args->fsroot.data.data_val;

	reffs_trace_event(REFFS_TRACE_CAT_NFS, "nfs3_fsinfo", __LINE__,
			  "xid=0x%08x sb=%lu ino=%lu crc=0x%08x",
			  rt->rt_info.ri_xid, nfh->nfh_sb, nfh->nfh_ino, crc);
}

static inline void trace_nfs3_srv_pathconf(struct rpc_trans *rt,
					   PATHCONF3args *args)
{
	uint32_t crc = nfs3_getfh_crc(&args->object);
	struct network_file_handle *nfh =
		(struct network_file_handle *)args->object.data.data_val;

	reffs_trace_event(REFFS_TRACE_CAT_NFS, "nfs3_pathconf", __LINE__,
			  "xid=0x%08x sb=%lu ino=%lu crc=0x%08x",
			  rt->rt_info.ri_xid, nfh->nfh_sb, nfh->nfh_ino, crc);
}

static inline void trace_nfs3_srv_commit(struct rpc_trans *rt,
					 COMMIT3args *args)
{
	uint32_t crc = nfs3_getfh_crc(&args->file);
	struct network_file_handle *nfh =
		(struct network_file_handle *)args->file.data.data_val;

	reffs_trace_event(REFFS_TRACE_CAT_NFS, "nfs3_commit", __LINE__,
			  "xid=0x%08x sb=%lu ino=%lu crc=0x%08x",
			  rt->rt_info.ri_xid, nfh->nfh_sb, nfh->nfh_ino, crc);
}

#endif /* _REFFS_TRACE_NFS3_SERVER_H */
