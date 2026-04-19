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
#include <stdatomic.h>
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

static void compound_free(struct compound *compound)
{
	if (!compound)
		return;

	server_state_put(compound->c_server_state);
	nfs4_session_put(compound->c_session);
	nfs4_client_put(compound->c_nfs4_client);
	inode_active_put(compound->c_inode);
	super_block_put(compound->c_curr_sb);
	super_block_put(compound->c_saved_sb);
	stateid_put(compound->c_curr_stid);
	stateid_put(compound->c_saved_stid);
	free(compound);
}

/*
 * nfs4_compound_finalize -- cache the reply in the session slot and free
 * the compound.
 *
 * Must be called exactly once per compound, after dispatch_compound() has
 * run to completion (i.e. the task is not paused).  For compounds that
 * short-circuit before dispatch (minorversion mismatch, empty argarray,
 * bad first op), c_slot is NULL and the caching block is skipped.
 */
static void nfs4_compound_finalize(struct compound *compound)
{
	if (compound->c_slot) {
		COMPOUND4res *res = compound->c_res;

		pthread_mutex_lock(&compound->c_slot->sl_mutex);
		if (compound->c_slot->sl_state == NFS4_SLOT_IN_USE) {
			XDR xdrs;
			uint32_t reply_size;

			reply_size =
				xdr_sizeof((xdrproc_t)xdr_COMPOUND4res, res);

			free(compound->c_slot->sl_reply);
			compound->c_slot->sl_reply = calloc(1, reply_size);
			if (compound->c_slot->sl_reply) {
				xdrmem_create(&xdrs, compound->c_slot->sl_reply,
					      reply_size, XDR_ENCODE);
				if (xdr_COMPOUND4res(&xdrs, res)) {
					compound->c_slot->sl_reply_len =
						reply_size;
					compound->c_slot->sl_state =
						NFS4_SLOT_CACHED;
				} else {
					free(compound->c_slot->sl_reply);
					compound->c_slot->sl_reply = NULL;
					compound->c_slot->sl_reply_len = 0;
					compound->c_slot->sl_state =
						NFS4_SLOT_IDLE;
				}
				xdr_destroy(&xdrs);
			} else {
				compound->c_slot->sl_reply_len = 0;
				compound->c_slot->sl_state = NFS4_SLOT_IDLE;
			}
		}
		pthread_mutex_unlock(&compound->c_slot->sl_mutex);
	}

	compound_free(compound);
}

static _Atomic uint64_t compound_alloc_counter;

static struct compound *compound_alloc(struct rpc_trans *rt)
{
	struct compound *compound;
	struct protocol_handler *ph = (struct protocol_handler *)rt->rt_context;
	int ret = 0;

	compound = calloc(1, sizeof(*compound));
	if (!compound)
		return NULL;

	compound->c_rt = rt;
	compound->c_args = (COMPOUND4args *)ph->ph_args;
	compound->c_res = (COMPOUND4res *)ph->ph_res;
	compound->c_alloc_seq = atomic_fetch_add_explicit(
		&compound_alloc_counter, 1, memory_order_relaxed);

	ret = rpc_cred_to_authunix_parms(&rt->rt_info, &compound->c_ap);
	if (ret) {
		free(compound);
		return NULL;
	}

	return compound;
}

int nfs4_proc_compound(struct rpc_trans *rt)
{
	struct compound *compound;
	COMPOUND4res *res;

	if (rt->rt_compound != NULL) {
		/*
		 * Resume path: this task was paused by an async op and has
		 * been re-enqueued.  Skip alloc and all request validation --
		 * those already ran on the first pass.
		 */
		compound = rt->rt_compound;
		res = compound->c_res;
	} else {
		/*
		 * Fresh path: allocate the compound and validate the request
		 * before dispatching.
		 */
		COMPOUND4args *args;
		nfs_opnum4 op;

		compound = compound_alloc(rt);
		if (!compound)
			return NFS4ERR_DELAY;
		rt->rt_compound = compound;

		res = compound->c_res;
		args = compound->c_args;

		trace_nfs4_srv_compound(rt);

		if (args->minorversion != 1 && args->minorversion != 2) {
			res->status = NFS4ERR_MINOR_VERS_MISMATCH;
			goto out;
		}

		/*
		 * RFC 5661 S16.2.3: the server MUST copy the tag from the
		 * request into the response.  XDR free will call free() on
		 * res->tag.utf8string_val independently of the args, so we
		 * need a separate heap allocation.
		 */
		if (args->tag.utf8string_len > 0) {
			res->tag.utf8string_val =
				malloc(args->tag.utf8string_len);
			if (res->tag.utf8string_val) {
				memcpy(res->tag.utf8string_val,
				       args->tag.utf8string_val,
				       args->tag.utf8string_len);
				res->tag.utf8string_len =
					args->tag.utf8string_len;
			}
		}

		res->resarray.resarray_val =
			calloc(args->argarray.argarray_len, sizeof(nfs_resop4));
		if (!res->resarray.resarray_val) {
			res->status = NFS4ERR_DELAY;
			goto out;
		}
		res->resarray.resarray_len = args->argarray.argarray_len;

		if (args->argarray.argarray_len == 0)
			goto out;

		op = NFS4_OP_NUM(compound);
		if (op != OP_SEQUENCE && op != OP_EXCHANGE_ID &&
		    op != OP_CREATE_SESSION && op != OP_DESTROY_SESSION &&
		    op != OP_BIND_CONN_TO_SESSION &&
		    op != OP_DESTROY_CLIENTID) {
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
	}

	/*
	 * If dispatch_compound() returns true an op went async.  Leave
	 * everything alive; the task will be re-enqueued by the async
	 * completer via task_resume() and the worker will call
	 * rpc_protocol_op_call() again with rt_compound already set.
	 */
	if (dispatch_compound(compound))
		return EINPROGRESS;

out:
	rt->rt_compound = NULL;
	nfs4_compound_finalize(compound);
	return res->status;
}
