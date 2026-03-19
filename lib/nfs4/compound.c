/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <pthread.h>
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
#include "nfs4/trace/nfs4.h"
#include "nfs4/compound.h"
#include "nfs4/errors.h"
#include "nfs4/ops.h"

static void compound_free(struct compound *c)
{
	if (!c)
		return;

	nfs4_session_put(c->c_session);
	nfs4_client_put(c->c_nfs4_client);
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
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	int ret = 0;

	c = calloc(1, sizeof(*c));
	if (!c)
		return NULL;

	c->c_rt = rt;
	c->c_args = (COMPOUND4args *)ph->ph_args;
	c->c_res = (COMPOUND4res *)ph->ph_res;

	ret = rpc_cred_to_authunix_parms(&rt->rt_info.ri_cred, &c->c_ap);
	if (ret) {
		free(c);
		return NULL;
	}

	return c;
}

int nfs4_proc_compound(struct rpc_trans *rt)
{
	struct compound *c = compound_alloc(rt);
	COMPOUND4res *res;
	COMPOUND4args *args;
	nfs_opnum4 op;

	if (!c) {
		return NFS4ERR_DELAY;
	}

	res = c->c_res;
	args = c->c_args;

	trace_nfs4_srv_compound(rt);

	res->status = 0;

	if (args->minorversion != 1 && args->minorversion != 2) {
		res->status = NFS4ERR_MINOR_VERS_MISMATCH;
		goto out;
	}

	/*
	 * RFC 5661 §16.2.3: the server MUST copy the tag from the
	 * request into the response.  XDR free will call free() on
	 * res->tag.utf8string_val independently of the args, so we
	 * need a separate heap allocation.
	 */
	if (args->tag.utf8string_len > 0) {
		res->tag.utf8string_val = malloc(args->tag.utf8string_len);
		if (res->tag.utf8string_val) {
			memcpy(res->tag.utf8string_val,
			       args->tag.utf8string_val,
			       args->tag.utf8string_len);
			res->tag.utf8string_len = args->tag.utf8string_len;
		}
	}

	res->resarray.resarray_val =
		calloc(args->argarray.argarray_len, sizeof(nfs_resop4));
	if (!res->resarray.resarray_val) {
		res->status = NFS4ERR_DELAY;
		goto out;
	}
	res->resarray.resarray_len = args->argarray.argarray_len;

	if (args->argarray.argarray_len == 0) {
		res->status = NFS4_OK;
		goto out;
	}

	op = NFS4_OP_NUM(c);
	if (op != OP_SEQUENCE && op != OP_EXCHANGE_ID &&
	    op != OP_CREATE_SESSION && op != OP_DESTROY_SESSION &&
	    op != OP_BIND_CONN_TO_SESSION && op != OP_DESTROY_CLIENTID) {
		nfs_resop4 *resop = &res->resarray.resarray_val[0];

		if (op < OP_ACCESS || op > OP_CHUNK_WRITE_REPAIR) {
			resop->resop = OP_ILLEGAL;
			resop->nfs_resop4_u.opillegal.status =
				NFS4ERR_OP_ILLEGAL;
			res->status = NFS4ERR_OP_ILLEGAL;
		} else {
			resop->resop = op;
			resop->nfs_resop4_u.opillegal.status =
				NFS4ERR_OP_NOT_IN_SESSION;
			res->status = NFS4ERR_OP_NOT_IN_SESSION;
		}
		res->resarray.resarray_len = 1;

		goto out;
	}

	dispatch_compound(c);

	/*
	 * Transition the slot from IN_USE to CACHED.  This must happen
	 * after dispatch so that any concurrent replay of the same seqid
	 * gets NFS4ERR_DELAY rather than a stale cached reply.
	 */
	if (c->c_slot) {
		pthread_mutex_lock(&c->c_slot->sl_mutex);
		if (c->c_slot->sl_state == NFS4_SLOT_IN_USE) {
			XDR xdrs;
			uint32_t reply_size;

			reply_size =
				xdr_sizeof((xdrproc_t)xdr_COMPOUND4res, res);

			free(c->c_slot->sl_reply);
			c->c_slot->sl_reply = calloc(1, reply_size);
			if (c->c_slot->sl_reply) {
				xdrmem_create(&xdrs, c->c_slot->sl_reply,
					      reply_size, XDR_ENCODE);
				if (xdr_COMPOUND4res(&xdrs, res)) {
					c->c_slot->sl_reply_len = reply_size;
					c->c_slot->sl_state = NFS4_SLOT_CACHED;
				} else {
					free(c->c_slot->sl_reply);
					c->c_slot->sl_reply = NULL;
					c->c_slot->sl_reply_len = 0;
					c->c_slot->sl_state = NFS4_SLOT_IDLE;
				}
				xdr_destroy(&xdrs);
			} else {
				c->c_slot->sl_reply_len = 0;
				c->c_slot->sl_state = NFS4_SLOT_IDLE;
			}
		}
		pthread_mutex_unlock(&c->c_slot->sl_mutex);
	}

out:
	compound_free(c);
	return res->status;
}
