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
#include <sys/sysmacros.h>
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
#include "reffs/stateid.h"
#include "reffs/super_block.h"
#include "reffs/data_block.h"
#include "reffs/server.h"
#include "reffs/vfs.h"
#include "reffs/identity.h"
#include "reffs/errno.h"
#include "reffs/trace/nfs4_server.h"
#include "nfs4/compound.h"
#include "nfs4/errors.h"
#include "nfs4/ops.h"

static void compound_free(struct compound *c)
{
	if (!c)
		return;

	inode_active_put(c->c_inode);
	super_block_put(c->c_curr_sb);
	super_block_put(c->c_saved_sb);
	stateid_put(c->c_curr_stid);
	stateid_put(c->c_saved_stid);
	free(c);
}

static struct compound *compound_alloc(struct rpc_trans *rt)
{
	struct compound *c;
	int ret = 0;

	c = calloc(1, sizeof(*c));
	if (!c)
		return NULL;

	c->c_rt = rt;

	ret = rpc_cred_to_authunix_parms(&rt->rt_info.ri_cred, &c->c_ap);
	if (ret) {
		free(c);
		return NULL;
	}

	return c;
}

int nfs4_proc_compound(struct rpc_trans *rt)
{
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;

	COMPOUND4res *res = ((COMPOUND4res *)ph->ph_res);

	struct compound *c;

	nfs_opnum4 op;

	trace_nfs4_srv_compound(rt);

	res->status = 0;

	c = compound_alloc(rt);
	if (!c) {
		return NFS4ERR_DELAY;
	}

	op = NFS4_OP_NUM(c);
	if (op != OP_SEQUENCE && op != OP_EXCHANGE_ID &&
	    op != OP_CREATE_SESSION && op != OP_CREATE_SESSION &&
	    op != OP_BIND_CONN_TO_SESSION) {
		nfs_resop4 *resop = &res->resarray.resarray_val[0];
		nfsstat4 *status = &resop->nfs_resop4_u.opillegal.status;
		res->status = *status = NFS4ERR_OP_NOT_IN_SESSION;

		goto out;
	}

	dispatch_compound(c);

out:
	compound_free(c);
	return res->status;
}
