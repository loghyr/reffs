/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
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
#if defined(__linux__)
#include <sys/sysmacros.h>
#endif
#include <rpc/xdr.h>
#include <rpc/auth.h>
#include <rpc/auth_unix.h>
#include <zlib.h>
#include "nfsv42_xdr.h"
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
#include "reffs/vfs.h"
#include "reffs/identity.h"
#include "reffs/errno.h"
#include "nfs4/trace/nfs4.h"
#include "nfs4/attr.h"
#include "nfs4/compound.h"
#include "nfs4/cb.h"
#include "nfs4/errors.h"
#include "nfs4/lease_reaper.h"
#include "nfs4/trust_stateid.h"
#include "nfs4/migration_record.h"

/*
 * On locking order:
 *
 * 1) Always take the d_rwlock of the dirent after
 * the i_attr_mutex of the inode.
 *
 * 2) Always take the i_db_rwlock of the inode after the
 * i_attr_mutex of the inode.
 */

static int nfs4_proc_null(struct rpc_trans *rt)
{
	trace_nfs4_srv_null(rt);
	return 0;
}

struct rpc_operations_handler nfs4_operations_handler[] = {
	RPC_OPERATION_INIT(NFSPROC4, NULL, NULL, NULL, NULL, NULL,
			   nfs4_proc_null),
	RPC_OPERATION_INIT(NFSPROC4, COMPOUND, xdr_COMPOUND4args, COMPOUND4args,
			   xdr_COMPOUND4res, COMPOUND4res, nfs4_proc_compound),
};

static struct rpc_program_handler *nfs4_handler;

volatile sig_atomic_t nfsv4_registered = 0;

int nfs4_protocol_register(void)
{
	if (nfsv4_registered)
		return 0;

	if (nfs4_attribute_init()) {
		return ENOMEM;
	}

	nfsv4_registered = 1;

	nfs4_handler = rpc_program_handler_alloc(
		NFS4_PROGRAM, NFS_V4, nfs4_operations_handler,
		sizeof(nfs4_operations_handler) /
			sizeof(*nfs4_operations_handler));
	if (!nfs4_handler) {
		nfsv4_registered = 0;
		return ENOMEM;
	}

	cb_timeout_init();
	lease_reaper_init();
	trust_stateid_init();
	migration_record_init();

	return 0;
}

int nfs4_protocol_deregister(void)
{
	if (!nfsv4_registered)
		return 0;

	migration_record_fini();
	trust_stateid_fini();
	lease_reaper_fini();
	cb_timeout_fini();

	rpc_program_handler_put(nfs4_handler);
	nfs4_handler = NULL;
	nfsv4_registered = 0;

	nfs4_attribute_fini();

	return 0;
}
