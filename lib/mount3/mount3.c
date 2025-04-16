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
#include <rpc/xdr.h>
#include <rpc/auth.h>
#include <rpc/auth_unix.h>
#include <zlib.h>
#include "mntv3_xdr.h"
#include "reffs/rpc.h"
#include "reffs/log.h"

static int mount3_null(struct rpc_trans *rt)
{
	LOG("NULL");
	printf("There are %lu bytes remaining\n", rt->rt_len - rt->rt_offset);
	return 0;
}

static int mount3_mnt(struct rpc_trans *rt)
{
	LOG("MNT");
	// struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	printf("There are %lu bytes remaining\n", rt->rt_len - rt->rt_offset);
	return 0;
}

static int mount3_dump(struct rpc_trans *rt)
{
	LOG("DUMP");
	printf("There are %lu bytes remaining\n", rt->rt_len - rt->rt_offset);
	return 0;
}

static int mount3_umnt(struct rpc_trans *rt)
{
	LOG("UMNT");
	printf("There are %lu bytes remaining\n", rt->rt_len - rt->rt_offset);
	return 0;
}

static int mount3_umntall(struct rpc_trans *rt)
{
	LOG("UMNTALL");
	printf("There are %lu bytes remaining\n", rt->rt_len - rt->rt_offset);
	return 0;
}

static int mount3_exports(struct rpc_trans *rt)
{
	LOG("EXPORTS");
	printf("There are %lu bytes remaining\n", rt->rt_len - rt->rt_offset);
	return 0;
}

const struct rpc_operations_handler mount3_operations_handler[] = {
	RPC_OPERATION_INIT(MOUNTPROC3_NULL, NULL, NULL, NULL, NULL,
			   mount3_null),
	RPC_OPERATION_INIT(MOUNTPROC3_MNT, xdr_dirpath, dirpath, xdr_mountres3,
			   mountres3, mount3_mnt),
	RPC_OPERATION_INIT(MOUNTPROC3_DUMP, NULL, NULL, xdr_mountlist,
			   mountlist, mount3_dump),
	RPC_OPERATION_INIT(MOUNTPROC3_UMNT, xdr_dirpath, dirpath, NULL, NULL,
			   mount3_umnt),
	RPC_OPERATION_INIT(MOUNTPROC3_UMNTALL, NULL, NULL, NULL, NULL,
			   mount3_umntall),
	RPC_OPERATION_INIT(MOUNTPROC3_EXPORT, NULL, NULL, xdr_exports, exports,
			   mount3_exports),
};

static struct rpc_program_handler *mount3_handler;

volatile sig_atomic_t registered = 0;

int mount3_protocol_register(void)
{
	if (registered)
		return 0;

	registered = 1;

	mount3_handler = rpc_program_handler_alloc(
		MOUNT_PROGRAM, MOUNT_V3, mount3_operations_handler,
		sizeof(mount3_operations_handler) /
			sizeof(*mount3_operations_handler));
	if (!mount3_handler) {
		registered = 0;
		return ENOMEM;
	}

	return 0;
}

int mount3_protocol_deregister(void)
{
	if (!registered)
		return 0;

	rpc_program_handler_put(mount3_handler);
	mount3_handler = NULL;
	registered = 0;

	return 0;
}
