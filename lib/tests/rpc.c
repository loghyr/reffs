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
#include <rpc/auth.h>
#include <rpc/auth_unix.h>
#include <zlib.h>
#include "nfsv3_xdr.h"
#include "nfsv3_test.h"
#include "reffs/test.h"

struct rpc_handler {
	int (*rh_decode)(char *buffer, int len);
	int (*rh_encode)(char *buffer, int len);
};

struct rpc_cred {
	uint32_t rc_flavor;
	union {
		struct authunix_parms rc_unix;
	};
};

struct rpc_info {
	uint32_t ri_xid;
	uint32_t ri_type;
	uint32_t ri_rpc_version;
	uint32_t ri_program;
	uint32_t ri_version;
	uint32_t ri_procedure;
	struct rpc_cred ri_cred;
	uint32_t ri_verifier_flavor;
	enum auth_stat ri_stat;
};

struct rpc_trans {
	struct rpc_info rt_info;
	char *rt_body;
	size_t rt_len;
	size_t rt_offset;
	void *rt_context;
};

struct rpc_operations_handler {
	uint32_t roh_operation;
	xdrproc_t roh_args_f;
	size_t roh_args_size;
	xdrproc_t roh_res_f;
	size_t roh_res_size;
	int (*roh_action)(struct rpc_trans *rt);
};

struct rpc_program_handler {
	uint32_t rph_program;
	uint32_t rph_version;
	const struct rpc_operations_handler *rph_ops;
	size_t rph_ops_len;
};

static inline uint32_t *decode_uint32_t(struct rpc_trans *rt, uint32_t *p,
					uint32_t *dst)
{
	if (rt->rt_offset + sizeof(uint32_t) <= rt->rt_len) {
		*dst = ntohl(*p);
		rt->rt_offset += sizeof(uint32_t);
	} else {
		return NULL;
	}

	return ++p;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
static int nfs3_null(struct rpc_trans *rt)
{
	LOG("NULL");
	printf("There are %lu bytes remaining\n", rt->rt_len - rt->rt_offset);
	return 0;
}

static void print_nfs_fh3_hex(nfs_fh3 *fh)
{
	// Calculate CRC-32
	uLong crc = crc32(0L, Z_NULL, 0);
	crc = crc32(crc, (const Bytef *)fh->data.data_val, fh->data.data_len);

	printf("File handle (length %u):\n", fh->data.data_len);
	printf("[hash (CRC-32): 0x%08lx]\n", crc);
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

static int nfs3_getattr(struct rpc_trans *rt)
{
	XDR xdrs = { 0 };

	GETATTR3res *res;
	GETATTR3args *args;

	void *data;
	xdrproc_t f;

	size_t len;

	uint32_t *p = (uint32_t *)(rt->rt_body + rt->rt_offset);

	uint32_t start_pos, end_pos;

	LOG("GETATTR");

	if (rt->rt_info.ri_type == 0) {
		data = args = calloc(1, sizeof(*args));
		verify(args);
		f = (xdrproc_t)xdr_GETATTR3args;
	} else {
		data = res = calloc(1, sizeof(*res));
		verify(res);
		f = (xdrproc_t)xdr_GETATTR3res;
	}

	xdrmem_create(&xdrs, (char *)p, rt->rt_len - rt->rt_offset, XDR_DECODE);

	start_pos = xdr_getpos(&xdrs);

	if (!f(&xdrs, data)) {
		xdr_free((xdrproc_t)f, (char *)data);
		xdr_destroy(&xdrs);
		return -1;
	}

	end_pos = xdr_getpos(&xdrs);

	len = end_pos - start_pos;

	xdr_destroy(&xdrs);

	rt->rt_offset += len;
	p = (uint32_t *)(p + len / sizeof(uint32_t));

	if (rt->rt_info.ri_type == 0) {
		print_nfs_fh3_hex(&args->object);
	} else {
	}

	xdr_free((xdrproc_t)f, (char *)data);

	printf("There are %lu bytes remaining\n", rt->rt_len - rt->rt_offset);
	return 0;
}

static int nfs3_setattr(struct rpc_trans *rt)
{
	LOG("SETATTR");
	printf("There are %lu bytes remaining\n", rt->rt_len - rt->rt_offset);
	return 0;
}

static int nfs3_lookup(struct rpc_trans *rt)
{
	LOG("LOOKUP");
	printf("There are %lu bytes remaining\n", rt->rt_len - rt->rt_offset);
	return 0;
}

static int nfs3_access(struct rpc_trans *rt)
{
	LOG("ACCESS");
	printf("There are %lu bytes remaining\n", rt->rt_len - rt->rt_offset);
	return 0;
}

static int nfs3_readlink(struct rpc_trans *rt)
{
	LOG("READLINK");
	printf("There are %lu bytes remaining\n", rt->rt_len - rt->rt_offset);
	return 0;
}

static int nfs3_read(struct rpc_trans *rt)
{
	LOG("READ");
	printf("There are %lu bytes remaining\n", rt->rt_len - rt->rt_offset);
	return 0;
}

static int nfs3_write(struct rpc_trans *rt)
{
	LOG("WRITE");
	printf("There are %lu bytes remaining\n", rt->rt_len - rt->rt_offset);
	return 0;
}

static int nfs3_create(struct rpc_trans *rt)
{
	LOG("CREATE");
	printf("There are %lu bytes remaining\n", rt->rt_len - rt->rt_offset);
	return 0;
}

static int nfs3_mkdir(struct rpc_trans *rt)
{
	LOG("MKDIR");
	printf("There are %lu bytes remaining\n", rt->rt_len - rt->rt_offset);
	return 0;
}

static int nfs3_symlink(struct rpc_trans *rt)
{
	LOG("SYMLINK");
	printf("There are %lu bytes remaining\n", rt->rt_len - rt->rt_offset);
	return 0;
}

static int nfs3_mknod(struct rpc_trans *rt)
{
	LOG("MKNOD");
	printf("There are %lu bytes remaining\n", rt->rt_len - rt->rt_offset);
	return 0;
}

static int nfs3_remove(struct rpc_trans *rt)
{
	LOG("REMOVE");
	printf("There are %lu bytes remaining\n", rt->rt_len - rt->rt_offset);
	return 0;
}

static int nfs3_rmdir(struct rpc_trans *rt)
{
	LOG("RMDIR");
	printf("There are %lu bytes remaining\n", rt->rt_len - rt->rt_offset);
	return 0;
}

static int nfs3_rename(struct rpc_trans *rt)
{
	LOG("RENAME");
	printf("There are %lu bytes remaining\n", rt->rt_len - rt->rt_offset);
	return 0;
}

static int nfs3_link(struct rpc_trans *rt)
{
	LOG("LINK");
	printf("There are %lu bytes remaining\n", rt->rt_len - rt->rt_offset);
	return 0;
}

static int nfs3_readdir(struct rpc_trans *rt)
{
	LOG("READDIR");
	printf("There are %lu bytes remaining\n", rt->rt_len - rt->rt_offset);
	return 0;
}

static int nfs3_readdirplus(struct rpc_trans *rt)
{
	LOG("READDIRPLUS");
	printf("There are %lu bytes remaining\n", rt->rt_len - rt->rt_offset);
	return 0;
}

static int nfs3_fsstat(struct rpc_trans *rt)
{
	LOG("FSSTAT");
	printf("There are %lu bytes remaining\n", rt->rt_len - rt->rt_offset);
	return 0;
}

static int nfs3_fsinfo(struct rpc_trans *rt)
{
	LOG("FSINFO");
	printf("There are %lu bytes remaining\n", rt->rt_len - rt->rt_offset);
	return 0;
}

static int nfs3_pathconf(struct rpc_trans *rt)
{
	LOG("PATHCONF");
	printf("There are %lu bytes remaining\n", rt->rt_len - rt->rt_offset);
	return 0;
}

static int nfs3_commit(struct rpc_trans *rt)
{
	LOG("COMMIT");
	printf("There are %lu bytes remaining\n", rt->rt_len - rt->rt_offset);
	return 0;
}
#pragma GCC diagnostic pop

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
};

struct rpc_program_handler nfsv3_handler = {
	.rph_program = NFS3_PROGRAM,
	.rph_version = NFS_V3,
	.rph_ops = nfsv3_operations_handler,
	.rph_ops_len = sizeof(nfsv3_operations_handler) /
		       sizeof(*nfsv3_operations_handler)
};

//#define EXAMINE_PACKET nfs3_null_request_packet_data

#define EXAMINE_PACKET nfs3_getattr_dir_request_packet_data

int main(void)
{
	struct rpc_trans rt = { 0 };
	uint32_t *p;
	size_t i;

	rt.rt_body = (char *)EXAMINE_PACKET;
	rt.rt_len = sizeof(EXAMINE_PACKET) / sizeof(*EXAMINE_PACKET);

	rt.rt_offset = 72; // Jump over the rpc marker
	p = (uint32_t *)(rt.rt_body + 72);

	printf("The size of %p is %lu.\n", (void *)rt.rt_body, rt.rt_len);

	printf("Expected 0x%x, got 0x%x\n", htonl(0xad46842e), *p);

	p = decode_uint32_t(&rt, p, &rt.rt_info.ri_xid);
	verify_msg(p, "Could not decode xid");

	p = decode_uint32_t(&rt, p, &rt.rt_info.ri_type);
	verify_msg(p, "Could not decode type");

	LOG("This is a %s", rt.rt_info.ri_type ? "REPLY" : "CALL");

	p = decode_uint32_t(&rt, p, &rt.rt_info.ri_rpc_version);
	verify_msg(p, "Could not decode rpc version");

	p = decode_uint32_t(&rt, p, &rt.rt_info.ri_program);
	verify_msg(p, "Could not decode program");

	p = decode_uint32_t(&rt, p, &rt.rt_info.ri_version);
	verify_msg(p, "Could not decode version");

	LOG("This is a program %u version %u", rt.rt_info.ri_program,
	    rt.rt_info.ri_version);

	p = decode_uint32_t(&rt, p, &rt.rt_info.ri_procedure);
	verify_msg(p, "Could not decode procedure");

	p = decode_uint32_t(&rt, p, &rt.rt_info.ri_cred.rc_flavor);
	verify_msg(p, "Could not decode auth flavor");

	switch (rt.rt_info.ri_cred.rc_flavor) {
	case AUTH_NONE: {
		uint32_t len;
		p = decode_uint32_t(&rt, p, &len);
		verify_msg(p, "Could not decode auth flavor len");
		LOG("auth is AUTH_NONE, len is %u", len);
		break;
	}
	case AUTH_SYS: {
		XDR xdrs = { 0 };

		uint32_t len;
		p = decode_uint32_t(&rt, p, &len);
		verify_msg(p, "Could not decode auth flavor len");
		verify_msg(len != 0, "auth flavor len %u invalid", len);

		xdrmem_create(&xdrs, (char *)p, rt.rt_len - rt.rt_offset,
			      XDR_DECODE);

		if (!xdr_authunix_parms(&xdrs, &rt.rt_info.ri_cred.rc_unix)) {
			xdr_free((xdrproc_t)xdr_authunix_parms,
				 (char *)&rt.rt_info.ri_cred.rc_unix);
			rt.rt_info.ri_stat = AUTH_BADCRED;
		} else {
			LOG("time = %lu", rt.rt_info.ri_cred.rc_unix.aup_time);
			LOG("machine = %s",
			    rt.rt_info.ri_cred.rc_unix.aup_machname);
			LOG("uid = %u", rt.rt_info.ri_cred.rc_unix.aup_uid);
			LOG("gid = %u", rt.rt_info.ri_cred.rc_unix.aup_gid);
			LOG("len = %u", rt.rt_info.ri_cred.rc_unix.aup_len);
			LOG("gids = %p",
			    (void *)rt.rt_info.ri_cred.rc_unix.aup_gids);
		}

		xdr_destroy(&xdrs);

		rt.rt_offset += len;
		p = (uint32_t *)(p + len / sizeof(uint32_t));

		LOG("auth is AUTH_SYS");
		break;
	}
	case AUTH_SHORT:
		LOG("auth %u is not supported", rt.rt_info.ri_cred.rc_flavor);
		break;
	case AUTH_DH:
		LOG("auth %u is not supported", rt.rt_info.ri_cred.rc_flavor);
		break;
	case RPCSEC_GSS:
		LOG("auth %u is not supported", rt.rt_info.ri_cred.rc_flavor);
		break;
#ifdef NOT_NOW_TLS
	case AUTH_TLS:
		LOG("auth is AUTH_TLS");
		break;
#endif
	default:
		LOG("auth %u is not supported", rt.rt_info.ri_cred.rc_flavor);
		break;
	}

	p = decode_uint32_t(&rt, p, &rt.rt_info.ri_verifier_flavor);
	verify_msg(p, "Could not decode verifier flavor");

	switch (rt.rt_info.ri_verifier_flavor) {
	case AUTH_NONE: {
		uint32_t len;
		p = decode_uint32_t(&rt, p, &len);
		verify_msg(p, "Could not decode verifier flavor len");
		LOG("verifier is AUTH_NONE, len is %u", len);
		break;
	}
	case AUTH_SYS: {
		LOG("verifier is AUTH_SYS");
		break;
	}
	case AUTH_SHORT:
		LOG("verifier %u is not supported",
		    rt.rt_info.ri_cred.rc_flavor);
		break;
	case AUTH_DH:
		LOG("verifier %u is not supported",
		    rt.rt_info.ri_cred.rc_flavor);
		break;
	case RPCSEC_GSS:
		LOG("verifier %u is not supported",
		    rt.rt_info.ri_cred.rc_flavor);
		break;
#ifdef NOT_NOW_TLS
	case AUTH_TLS:
		LOG("verifier is AUTH_TLS");
		break;
#endif
	default:
		LOG("verifier %u is not supported",
		    rt.rt_info.ri_verifier_flavor);
		break;
	}

	for (i = 0; i < nfsv3_handler.rph_ops_len; i++) {
		if (nfsv3_handler.rph_ops[i].roh_operation ==
		    rt.rt_info.ri_procedure) {
			if (nfsv3_handler.rph_ops[i].roh_action) {
				int ret = nfsv3_handler.rph_ops[i].roh_action(
					&rt);
				LOG("action returned %d", ret);
				if (!ret) {
					p = (uint32_t *)(rt.rt_body +
							 rt.rt_offset);
				}
			}
		}
	}

	printf("There are %lu bytes remaining\n", rt.rt_len - rt.rt_offset);

	switch (rt.rt_info.ri_cred.rc_flavor) {
	case AUTH_SYS:
		xdr_free((xdrproc_t)xdr_authunix_parms,
			 (char *)&rt.rt_info.ri_cred.rc_unix);
	default:
		break;
	}

	return 0;
}
