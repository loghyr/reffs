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
#include "nfsv42_xdr.h"
#include "reffs/rcu.h"
#include "reffs/rpc.h"
#include "reffs/cmp.h"
#include "reffs/log.h"
#include "reffs/filehandle.h"
#include "reffs/test.h"
#include "reffs/time.h"
#include "reffs/inode.h"
#include "reffs/nfs4_stats.h"
#include "reffs/super_block.h"
#include "reffs/data_block.h"
#include "reffs/server.h"
#include "reffs/vfs.h"
#include "reffs/identity.h"
#include "reffs/errno.h"
#include "nfs4/trace/nfs4.h"
#include "nfs4/errors.h"
#include "nfs4/ops.h"
#include "nfs4/compound.h"
#include "nfsv42_names.h"

typedef uint32_t (*nfs4_op_cb)(struct compound *compound);

nfs4_op_cb op_table[OP_MAX] = {
	[OP_ACCESS] = nfs4_op_access,
	[OP_CLOSE] = nfs4_op_close,
	[OP_COMMIT] = nfs4_op_commit,
	[OP_CREATE] = nfs4_op_create,
	[OP_DELEGPURGE] = nfs4_op_delegpurge,
	[OP_DELEGRETURN] = nfs4_op_delegreturn,
	[OP_GETATTR] = nfs4_op_getattr,
	[OP_GETFH] = nfs4_op_getfh,
	[OP_LINK] = nfs4_op_link,
	[OP_LOCK] = nfs4_op_lock,
	[OP_LOCKT] = nfs4_op_lockt,
	[OP_LOCKU] = nfs4_op_locku,
	[OP_LOOKUP] = nfs4_op_lookup,
	[OP_LOOKUPP] = nfs4_op_lookupp,
	[OP_NVERIFY] = nfs4_op_nverify,
	[OP_OPEN] = nfs4_op_open,
	[OP_OPENATTR] = nfs4_op_openattr,
	[OP_OPEN_CONFIRM] = nfs4_op_open_confirm,
	[OP_OPEN_DOWNGRADE] = nfs4_op_open_downgrade,
	[OP_PUTFH] = nfs4_op_putfh,
	[OP_PUTPUBFH] = nfs4_op_putpubfh,
	[OP_PUTROOTFH] = nfs4_op_putrootfh,
	[OP_READ] = nfs4_op_read,
	[OP_READDIR] = nfs4_op_readdir,
	[OP_READLINK] = nfs4_op_readlink,
	[OP_REMOVE] = nfs4_op_remove,
	[OP_RENAME] = nfs4_op_rename,
	[OP_RENEW] = nfs4_op_renew,
	[OP_RESTOREFH] = nfs4_op_restorefh,
	[OP_SAVEFH] = nfs4_op_savefh,
	[OP_SECINFO] = nfs4_op_secinfo,
	[OP_SETATTR] = nfs4_op_setattr,
	[OP_SETCLIENTID] = nfs4_op_setclientid,
	[OP_SETCLIENTID_CONFIRM] = nfs4_op_setclientid_confirm,
	[OP_VERIFY] = nfs4_op_verify,
	[OP_WRITE] = nfs4_op_write,
	[OP_RELEASE_LOCKOWNER] = nfs4_op_release_lockowner,
	[OP_BACKCHANNEL_CTL] = nfs4_op_backchannel_ctl,
	[OP_BIND_CONN_TO_SESSION] = nfs4_op_bind_conn_to_session,
	[OP_EXCHANGE_ID] = nfs4_op_exchange_id,
	[OP_CREATE_SESSION] = nfs4_op_create_session,
	[OP_DESTROY_SESSION] = nfs4_op_destroy_session,
	[OP_FREE_STATEID] = nfs4_op_free_stateid,
	[OP_GET_DIR_DELEGATION] = nfs4_op_get_dir_delegation,
	[OP_GETDEVICEINFO] = nfs4_op_getdeviceinfo,
	[OP_GETDEVICELIST] = nfs4_op_getdevicelist,
	[OP_LAYOUTCOMMIT] = nfs4_op_layoutcommit,
	[OP_LAYOUTGET] = nfs4_op_layoutget,
	[OP_LAYOUTRETURN] = nfs4_op_layoutreturn,
	[OP_SECINFO_NO_NAME] = nfs4_op_secinfo_no_name,
	[OP_SEQUENCE] = nfs4_op_sequence,
	[OP_SET_SSV] = nfs4_op_set_ssv,
	[OP_TEST_STATEID] = nfs4_op_test_stateid,
	[OP_WANT_DELEGATION] = nfs4_op_want_delegation,
	[OP_DESTROY_CLIENTID] = nfs4_op_destroy_clientid,
	[OP_RECLAIM_COMPLETE] = nfs4_op_reclaim_complete,
	[OP_ALLOCATE] = nfs4_op_allocate,
	[OP_COPY] = nfs4_op_copy,
	[OP_COPY_NOTIFY] = nfs4_op_copy_notify,
	[OP_DEALLOCATE] = nfs4_op_deallocate,
	[OP_IO_ADVISE] = nfs4_op_io_advise,
	[OP_LAYOUTERROR] = nfs4_op_layouterror,
	[OP_LAYOUTSTATS] = nfs4_op_layoutstats,
	[OP_OFFLOAD_CANCEL] = nfs4_op_offload_cancel,
	[OP_OFFLOAD_STATUS] = nfs4_op_offload_status,
	[OP_READ_PLUS] = nfs4_op_read_plus,
	[OP_SEEK] = nfs4_op_seek,
	[OP_WRITE_SAME] = nfs4_op_write_same,
	[OP_CLONE] = nfs4_op_clone,
	[OP_GETXATTR] = nfs4_op_getxattr,
	[OP_SETXATTR] = nfs4_op_setxattr,
	[OP_LISTXATTRS] = nfs4_op_listxattrs,
	[OP_REMOVEXATTR] = nfs4_op_removexattr,
	[OP_ACCESS_MASK] = nfs4_op_access_mask,
	[OP_CHUNK_COMMIT] = nfs4_op_chunk_commit,
	[OP_CHUNK_ERROR] = nfs4_op_chunk_error,
	[OP_CHUNK_FINALIZE] = nfs4_op_chunk_finalize,
	[OP_CHUNK_HEADER_READ] = nfs4_op_chunk_header_read,
	[OP_CHUNK_LOCK] = nfs4_op_chunk_lock,
	[OP_CHUNK_READ] = nfs4_op_chunk_read,
	[OP_CHUNK_REPAIRED] = nfs4_op_chunk_repaired,
	[OP_CHUNK_ROLLBACK] = nfs4_op_chunk_rollback,
	[OP_CHUNK_UNLOCK] = nfs4_op_chunk_unlock,
	[OP_CHUNK_WRITE] = nfs4_op_chunk_write,
	[OP_CHUNK_WRITE_REPAIR] = nfs4_op_chunk_write_repair,
};

bool dispatch_compound(struct compound *compound)
{
	COMPOUND4args *args = compound->c_args;
	COMPOUND4res *res = compound->c_res;

	/* Grab server_state once for the compound lifetime. */
	if (!compound->c_server_state) {
		compound->c_server_state = server_state_find();
		if (!compound->c_server_state) {
			res->status = NFS4ERR_DELAY;
			return false;
		}
	}

	/*
	 * Convenience macro: record stats for the op at c_curr_op.
	 * Uses c_server_state grabbed once per compound, not per op.
	 *
	 * c_server_state is ref-counted and should never be NULL after
	 * the check above succeeds.  If it IS NULL, something freed the
	 * compound or corrupted memory — LOG and skip stats rather than
	 * crashing.  This is a symptom, not the root cause.
	 */
#define RECORD_OP_STATS(resop_)                                            \
	do {                                                               \
		if (__builtin_expect(!compound->c_server_state, 0)) {      \
			LOG("BUG: c_server_state NULL at op %u — skipping" \
			    " stats (compound %p)",                        \
			    compound->c_curr_op, (void *)compound);        \
			break;                                             \
		}                                                          \
		nfs4_op_stats_record(                                      \
			compound->c_server_state->ss_nfs4_op_stats,        \
			compound->c_curr_sb ?                              \
				compound->c_curr_sb->sb_nfs4_op_stats :    \
				NULL,                                      \
			compound->c_nfs4_client ?                          \
				compound->c_nfs4_client->nc_op_stats :     \
				NULL,                                      \
			(resop_)->resop,                                   \
			(resop_)->nfs_resop4_u.opillegal.status,           \
			reffs_now_ns() - compound->c_op_start_ns, 0, 0);   \
	} while (0)

	/*
	 * Resume case: an op previously paused this compound.  Call its
	 * registered continuation to finish the op, then advance past it.
	 * c_curr_op is still pointing at the paused op.
	 */
	if (compound->c_rt->rt_next_action != NULL) {
		uint32_t (*action)(struct rpc_trans *rt) =
			compound->c_rt->rt_next_action;
		nfs_resop4 *resop =
			&res->resarray.resarray_val[compound->c_curr_op];

		TRACE("resume: op=%u c_server_state=%p compound=%p",
		      compound->c_curr_op, (void *)compound->c_server_state,
		      (void *)compound);

		compound->c_rt->rt_next_action = NULL;
		uint32_t action_flags = action(compound->c_rt);

		/*
		 * Callback itself went async (double-async).
		 * The return value is on our stack — safe to check
		 * even if the compound/task have been handed to
		 * another worker via a fast CQE + task_resume().
		 */
		if (action_flags & NFS4_OP_FLAG_ASYNC) {
			TRACE("resume: double-async compound=%p",
			      (void *)compound);
			return true;
		}

		trace_nfs4_compound_op(compound, __func__, __LINE__);
		RECORD_OP_STATS(resop);

		if (resop->nfs_resop4_u.opillegal.status) {
			res->status = resop->nfs_resop4_u.opillegal.status;
			res->resarray.resarray_len = compound->c_curr_op + 1;
			return false;
		}

		compound->c_curr_op++; /* op complete; advance to next */
	}

	/*
	 * Forward loop.  On a fresh compound c_curr_op is 0 (calloc'd).
	 * On a resume it starts from where the paused op left off + 1.
	 */
	for (; compound->c_curr_op < args->argarray.argarray_len;
	     compound->c_curr_op++) {
		nfs_argop4 *argop =
			&args->argarray.argarray_val[compound->c_curr_op];
		nfs_resop4 *resop =
			&res->resarray.resarray_val[compound->c_curr_op];

		resop->resop = argop->argop;
		if (argop->argop < OP_MAX && op_table[argop->argop]) {
			compound->c_op_start_ns = reffs_now_ns();
			TRACE("dispatch op=%s(%d) curr_op=%u",
			      nfs4_op_name(argop->argop), argop->argop,
			      compound->c_curr_op);
			uint32_t op_flags = op_table[argop->argop](compound);

			/*
			 * Op went async: the return value is on our stack,
			 * safe to check even after a fast CQE + task_resume
			 * hands the compound to another worker.  Do not
			 * dereference compound after this point.
			 */
			if (op_flags & NFS4_OP_FLAG_ASYNC) {
				TRACE("async: compound=%p", (void *)compound);
				return true;
			}

			trace_nfs4_compound_op(compound, __func__, __LINE__);
			RECORD_OP_STATS(resop);
		} else {
			compound->c_op_start_ns = reffs_now_ns();
			nfs4_op_illegal(compound);
			RECORD_OP_STATS(resop);
		}

		if (resop->nfs_resop4_u.opillegal.status) {
			TRACE("dispatch op=%s(%d) FAILED status=%s(%d)",
			      nfs4_op_name(argop->argop), argop->argop,
			      nfs4_err_name(
				      resop->nfs_resop4_u.opillegal.status),
			      resop->nfs_resop4_u.opillegal.status);
			res->status = resop->nfs_resop4_u.opillegal.status;
			res->resarray.resarray_len = compound->c_curr_op + 1;
			return false;
		}
	}

#undef RECORD_OP_STATS

	res->resarray.resarray_len = args->argarray.argarray_len;
	return false;
}
