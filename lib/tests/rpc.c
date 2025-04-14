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
#include <rpc/xdr.h>
#include "nfsv3_xdr.h"
#include "nfsv3_test.h"
#include "reffs/test.h"

struct rpc_handler {
	int (*rh_decode)(char *buffer, int len);
	int (*rh_encode)(char *buffer, int len);
};

struct rpc_info {
	uint32_t ri_xid;
	uint32_t ri_type;
	uint32_t ri_rpc_version;
	uint32_t ri_program;
	uint32_t ri_version;
	uint32_t ri_procedure;
	uint32_t ri_auth_flavor;
};

struct rpc_trans {
	struct rpc_info rt_info;
	uint32_t *rt_body;
	size_t rt_len;
	size_t rt_offset;
	void *rt_context;
};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
static int nfs3_null(uint32_t *base, uint32_t *consumed)
{
	return 0;
}

static int nfs3_getattr(uint32_t *base, uint32_t *consumed)
{
	return 0;
}

static int nfs3_setattr(uint32_t *base, uint32_t *consumed)
{
	return 0;
}

static int nfs3_lookup(uint32_t *base, uint32_t *consumed)
{
	return 0;
}

static int nfs3_access(uint32_t *base, uint32_t *consumed)
{
	return 0;
}

static int nfs3_readlink(uint32_t *base, uint32_t *consumed)
{
	return 0;
}

static int nfs3_read(uint32_t *base, uint32_t *consumed)
{
	return 0;
}

static int nfs3_write(uint32_t *base, uint32_t *consumed)
{
	return 0;
}

static int nfs3_create(uint32_t *base, uint32_t *consumed)
{
	return 0;
}

static int nfs3_mkdir(uint32_t *base, uint32_t *consumed)
{
	return 0;
}

static int nfs3_symlink(uint32_t *base, uint32_t *consumed)
{
	return 0;
}

static int nfs3_mknod(uint32_t *base, uint32_t *consumed)
{
	return 0;
}

static int nfs3_remove(uint32_t *base, uint32_t *consumed)
{
	return 0;
}

static int nfs3_rmdir(uint32_t *base, uint32_t *consumed)
{
	return 0;
}

static int nfs3_rename(uint32_t *base, uint32_t *consumed)
{
	return 0;
}

static int nfs3_link(uint32_t *base, uint32_t *consumed)
{
	return 0;
}

static int nfs3_readdir(uint32_t *base, uint32_t *consumed)
{
	return 0;
}

static int nfs3_readdirplus(uint32_t *base, uint32_t *consumed)
{
	return 0;
}

static int nfs3_fsstat(uint32_t *base, uint32_t *consumed)
{
	return 0;
}

static int nfs3_fsinfo(uint32_t *base, uint32_t *consumed)
{
	return 0;
}

static int nfs3_pathconf(uint32_t *base, uint32_t *consumed)
{
	return 0;
}

static int nfs3_commit(uint32_t *base, uint32_t *consumed)
{
	return 0;
}
#pragma GCC diagnostic pop

struct rpc_operations_handler {
	uint32_t roh_operation;
	xdrproc_t roh_args_f;
	size_t roh_args_size;
	xdrproc_t roh_res_f;
	size_t roh_res_size;
	int (*roh_action)(uint32_t *buf, uint32_t *consumed);
};

struct rpc_program_handler {
	uint32_t rph_program;
	uint32_t rph_version;
	const struct rpc_operations_handler *rph_ops;
};

#define RPC_OPERATION_INIT(OP, ARGS_F, ARGS, RES_F, RES, CALL) \
	{ .roh_operation = OP,                                 \
	  .roh_args_f = (xdrproc_t)ARGS_F,                     \
	  .roh_args_size = sizeof(ARGS),                       \
	  .roh_res_f = (xdrproc_t)RES_F,                       \
	  .roh_res_size = sizeof(RES),                         \
	  .roh_action = CALL }

const struct rpc_operations_handler nfsv3_operations_handler[] = {
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
	{ 0 }
};

struct rpc_program_handler nfsv3_handler = { .rph_program = NFS3_PROGRAM,
					     .rph_version = NFS_V3,
					     .rph_ops =
						     nfsv3_operations_handler };

static inline uint32_t *decode_uint32_t(struct rpc_trans *rt, uint32_t *p,
					uint32_t *dst)
{
	if (rt->rt_offset + sizeof(uint32_t) < rt->rt_len) {
		*dst = ntohl(*p);
		rt->rt_offset += sizeof(uint32_t);
	} else {
		return NULL;
	}

	return ++p;
}

int main(void)
{
	struct rpc_trans rt = { 0 };
	uint32_t *p;

	rt.rt_body = (uint32_t *)nfs3_fsinfo_request_packet_data;
	rt.rt_len = sizeof(nfs3_null_request_packet_data) /
		    sizeof(*nfs3_null_request_packet_data);

	rt.rt_offset = 72 / sizeof(uint32_t); // Jump over the rpc marker
	p = (uint32_t *)((char *)rt.rt_body + 72);

	printf("The size of %p is %lu.\n", (void *)rt.rt_body, rt.rt_len);

	printf("Expected 0x%x, got 0x%x\n", htonl(0xad46842e), *p);

	p = decode_uint32_t(&rt, p, &rt.rt_info.ri_xid);
	verify_msg(p, "Could not decode xid");
	verify_msg(rt.rt_info.ri_xid == 0xad46842e,
		   "Incorrect XID (0x%x vs 0x%x)", rt.rt_info.ri_xid,
		   0xad46842e);

	p = decode_uint32_t(&rt, p, &rt.rt_info.ri_type);
	verify_msg(p, "Could not decode type");
	verify_msg(rt.rt_info.ri_type == 0, "Incorrect type (%u vs %u)",
		   rt.rt_info.ri_type, 0);

	p = decode_uint32_t(&rt, p, &rt.rt_info.ri_rpc_version);
	verify_msg(p, "Could not decode rpc version");
	verify_msg(rt.rt_info.ri_rpc_version == 2,
		   "Incorrect rpc version (%u vs %u)",
		   rt.rt_info.ri_rpc_version, 2);

	p = decode_uint32_t(&rt, p, &rt.rt_info.ri_program);
	verify_msg(p, "Could not decode program");
	verify_msg(rt.rt_info.ri_program == 100003,
		   "Incorrect program (%u vs %u)", rt.rt_info.ri_program,
		   100003);

	p = decode_uint32_t(&rt, p, &rt.rt_info.ri_version);
	verify_msg(p, "Could not decode version");
	verify_msg(rt.rt_info.ri_version == 3, "Incorrect version (%u vs %u)",
		   rt.rt_info.ri_version, 3);

	p = decode_uint32_t(&rt, p, &rt.rt_info.ri_procedure);
	verify_msg(p, "Could not decode procedure");
	verify_msg(rt.rt_info.ri_procedure == NFSPROC3_FSINFO,
		   "Incorrect procedure (%u vs %u)", rt.rt_info.ri_procedure,
		   NFSPROC3_FSINFO);

	printf("There are %lu words remaining\n", rt.rt_len - rt.rt_offset);

	return 0;
}
