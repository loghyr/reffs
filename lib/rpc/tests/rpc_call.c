/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_JEMALLOC
#include <jemalloc/jemalloc.h>
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
#include "reffs/rpc.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
static int nfs3_null(struct rpc_trans *rt)
{
	LOG("NULL");
	printf("There are %lu bytes remaining\n",
	       rt->rt_body_len - rt->rt_offset);
	return 0;
}

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

static int nfs3_getattr(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	GETATTR3args *args = ph->ph_args;

	print_nfs_fh3_hex(&args->object);

	printf("There are %lu bytes remaining\n",
	       rt->rt_body_len - rt->rt_offset);
	return 0;
}

static int nfs3_setattr(struct rpc_trans *rt)
{
	LOG("SETATTR");
	printf("There are %lu bytes remaining\n",
	       rt->rt_body_len - rt->rt_offset);
	return 0;
}

static int nfs3_lookup(struct rpc_trans *rt)
{
	LOG("LOOKUP");
	printf("There are %lu bytes remaining\n",
	       rt->rt_body_len - rt->rt_offset);
	return 0;
}

static int nfs3_access(struct rpc_trans *rt)
{
	LOG("ACCESS");
	printf("There are %lu bytes remaining\n",
	       rt->rt_body_len - rt->rt_offset);
	return 0;
}

static int nfs3_readlink(struct rpc_trans *rt)
{
	LOG("READLINK");
	printf("There are %lu bytes remaining\n",
	       rt->rt_body_len - rt->rt_offset);
	return 0;
}

static int nfs3_read(struct rpc_trans *rt)
{
	LOG("READ");
	printf("There are %lu bytes remaining\n",
	       rt->rt_body_len - rt->rt_offset);
	return 0;
}

static int nfs3_write(struct rpc_trans *rt)
{
	LOG("WRITE");
	printf("There are %lu bytes remaining\n",
	       rt->rt_body_len - rt->rt_offset);
	return 0;
}

static int nfs3_create(struct rpc_trans *rt)
{
	LOG("CREATE");
	printf("There are %lu bytes remaining\n",
	       rt->rt_body_len - rt->rt_offset);
	return 0;
}

static int nfs3_mkdir(struct rpc_trans *rt)
{
	LOG("MKDIR");
	printf("There are %lu bytes remaining\n",
	       rt->rt_body_len - rt->rt_offset);
	return 0;
}

static int nfs3_symlink(struct rpc_trans *rt)
{
	LOG("SYMLINK");
	printf("There are %lu bytes remaining\n",
	       rt->rt_body_len - rt->rt_offset);
	return 0;
}

static int nfs3_mknod(struct rpc_trans *rt)
{
	LOG("MKNOD");
	printf("There are %lu bytes remaining\n",
	       rt->rt_body_len - rt->rt_offset);
	return 0;
}

static int nfs3_remove(struct rpc_trans *rt)
{
	LOG("REMOVE");
	printf("There are %lu bytes remaining\n",
	       rt->rt_body_len - rt->rt_offset);
	return 0;
}

static int nfs3_rmdir(struct rpc_trans *rt)
{
	LOG("RMDIR");
	printf("There are %lu bytes remaining\n",
	       rt->rt_body_len - rt->rt_offset);
	return 0;
}

static int nfs3_rename(struct rpc_trans *rt)
{
	LOG("RENAME");
	printf("There are %lu bytes remaining\n",
	       rt->rt_body_len - rt->rt_offset);
	return 0;
}

static int nfs3_link(struct rpc_trans *rt)
{
	LOG("LINK");
	printf("There are %lu bytes remaining\n",
	       rt->rt_body_len - rt->rt_offset);
	return 0;
}

static int nfs3_readdir(struct rpc_trans *rt)
{
	LOG("READDIR");
	printf("There are %lu bytes remaining\n",
	       rt->rt_body_len - rt->rt_offset);
	return 0;
}

static int nfs3_readdirplus(struct rpc_trans *rt)
{
	LOG("READDIRPLUS");
	printf("There are %lu bytes remaining\n",
	       rt->rt_body_len - rt->rt_offset);
	return 0;
}

static int nfs3_fsstat(struct rpc_trans *rt)
{
	LOG("FSSTAT");
	printf("There are %lu bytes remaining\n",
	       rt->rt_body_len - rt->rt_offset);
	return 0;
}

static int nfs3_fsinfo(struct rpc_trans *rt)
{
	LOG("FSINFO");
	printf("There are %lu bytes remaining\n",
	       rt->rt_body_len - rt->rt_offset);
	return 0;
}

static int nfs3_pathconf(struct rpc_trans *rt)
{
	LOG("PATHCONF");
	printf("There are %lu bytes remaining\n",
	       rt->rt_body_len - rt->rt_offset);
	return 0;
}

static int nfs3_commit(struct rpc_trans *rt)
{
	LOG("COMMIT");
	printf("There are %lu bytes remaining\n",
	       rt->rt_body_len - rt->rt_offset);
	return 0;
}
#pragma GCC diagnostic pop

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

//#define EXAMINE_PACKET nfs3_null_request_packet_data

#define EXAMINE_PACKET nfs3_getattr_dir_request_packet_data

int main(void)
{
	struct rpc_trans *rt;
	struct protocol_handler *ph;
	uint32_t *p;

	int ret;

	struct rpc_program_handler *nfsv3_handler;

	rt = calloc(1, sizeof(*rt));
	verify(rt);

	ph = calloc(1, sizeof(*ph));
	verify(ph);

	rt->rt_context = (void *)ph;

	rt->rt_body = (char *)EXAMINE_PACKET;
	rt->rt_body_len = sizeof(EXAMINE_PACKET) / sizeof(*EXAMINE_PACKET);

	nfsv3_handler = rpc_program_handler_alloc(
		NFS3_PROGRAM, NFS_V3, nfsv3_operations_handler,
		sizeof(nfsv3_operations_handler) /
			sizeof(*nfsv3_operations_handler));
	verify(nfsv3_handler);

	rt->rt_offset = 72; // Jump over the rpc marker
	p = (uint32_t *)(rt->rt_body + 72);

	printf("The size of %p is %lu.\n", (void *)rt->rt_body,
	       rt->rt_body_len);

	p = rpc_decode_uint32_t(rt, p, &rt->rt_info.ri_xid);
	verify_msg(p, "Could not decode xid");

	p = rpc_decode_uint32_t(rt, p, &rt->rt_info.ri_type);
	verify_msg(p, "Could not decode type");

	LOG("This is a %s", rt->rt_info.ri_type ? "REPLY" : "CALL");

	verify_msg(rt->rt_info.ri_type == 0,
		   "Select an example which is a call");

	p = rpc_decode_uint32_t(rt, p, &rt->rt_info.ri_rpc_version);
	verify_msg(p, "Could not decode rpc version");

	p = rpc_decode_uint32_t(rt, p, &rt->rt_info.ri_program);
	verify_msg(p, "Could not decode program");

	p = rpc_decode_uint32_t(rt, p, &rt->rt_info.ri_version);
	verify_msg(p, "Could not decode version");

	LOG("This is a program %u version %u", rt->rt_info.ri_program,
	    rt->rt_info.ri_version);

	p = rpc_decode_uint32_t(rt, p, &rt->rt_info.ri_procedure);
	verify_msg(p, "Could not decode procedure");

	p = rpc_decode_uint32_t(rt, p, &rt->rt_info.ri_cred.rc_flavor);
	verify_msg(p, "Could not decode auth flavor");

	switch (rt->rt_info.ri_cred.rc_flavor) {
	case AUTH_NONE: {
		uint32_t len;
		p = rpc_decode_uint32_t(rt, p, &len);
		verify_msg(p, "Could not decode auth flavor len");
		LOG("auth is AUTH_NONE, len is %u", len);
		break;
	}
	case AUTH_SYS: {
		XDR xdrs = { 0 };

		uint32_t len;
		p = rpc_decode_uint32_t(rt, p, &len);
		verify_msg(p, "Could not decode auth flavor len");
		verify_msg(len != 0, "auth flavor len %u invalid", len);

		xdrmem_create(&xdrs, (char *)p, rt->rt_body_len - rt->rt_offset,
			      XDR_DECODE);

		if (!xdr_authunix_parms(&xdrs, &rt->rt_info.ri_cred.rc_unix)) {
			xdr_free((xdrproc_t)xdr_authunix_parms,
				 (char *)&rt->rt_info.ri_cred.rc_unix);
			rt->rt_info.ri_auth_stat = AUTH_BADCRED;
		} else {
			LOG("time = %lu", rt->rt_info.ri_cred.rc_unix.aup_time);
			LOG("machine = %s",
			    rt->rt_info.ri_cred.rc_unix.aup_machname);
			LOG("uid = %u", rt->rt_info.ri_cred.rc_unix.aup_uid);
			LOG("gid = %u", rt->rt_info.ri_cred.rc_unix.aup_gid);
			LOG("len = %u", rt->rt_info.ri_cred.rc_unix.aup_len);
			LOG("gids = %p",
			    (void *)rt->rt_info.ri_cred.rc_unix.aup_gids);
		}

		xdr_destroy(&xdrs);

		rt->rt_offset += len;
		p = (uint32_t *)(p + len / sizeof(uint32_t));

		LOG("auth is AUTH_SYS");
		break;
	}
	case AUTH_SHORT:
		LOG("auth %u is not supported", rt->rt_info.ri_cred.rc_flavor);
		break;
	case AUTH_DH:
		LOG("auth %u is not supported", rt->rt_info.ri_cred.rc_flavor);
		break;
	case RPCSEC_GSS:
		LOG("auth %u is not supported", rt->rt_info.ri_cred.rc_flavor);
		break;
#ifdef NOT_NOW_TLS
	case AUTH_TLS:
		LOG("auth is AUTH_TLS");
		break;
#endif
	default:
		LOG("auth %u is not supported", rt->rt_info.ri_cred.rc_flavor);
		break;
	}

	p = rpc_decode_uint32_t(rt, p, &rt->rt_info.ri_verifier_flavor);
	verify_msg(p, "Could not decode verifier flavor");

	switch (rt->rt_info.ri_verifier_flavor) {
	case AUTH_NONE: {
		uint32_t len;
		p = rpc_decode_uint32_t(rt, p, &len);
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
		    rt->rt_info.ri_cred.rc_flavor);
		break;
	case AUTH_DH:
		LOG("verifier %u is not supported",
		    rt->rt_info.ri_cred.rc_flavor);
		break;
	case RPCSEC_GSS:
		LOG("verifier %u is not supported",
		    rt->rt_info.ri_cred.rc_flavor);
		break;
#ifdef NOT_NOW_TLS
	case AUTH_TLS:
		LOG("verifier is AUTH_TLS");
		break;
#endif
	default:
		LOG("verifier %u is not supported",
		    rt->rt_info.ri_verifier_flavor);
		break;
	}

	ret = rpc_protocol_allocate_call(rt);
	verify(ret == 0);

	ret = rpc_protocol_op_call(rt);
	LOG("action returned %d", ret);
	if (!ret) {
		p = (uint32_t *)(rt->rt_body + rt->rt_offset);
	}

	printf("There are %lu bytes remaining\n",
	       rt->rt_body_len - rt->rt_offset);

	rpc_protocol_free(rt);

	rpc_program_handler_put(nfsv3_handler);

	return 0;
}
