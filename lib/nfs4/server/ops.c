/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <rpc/auth.h>

#include "nfsv42_xdr.h"
#include "nfsv42_names.h"
#include "reffs/log.h"
#include "reffs/nfs4_stats.h"
#include "reffs/rpc.h"
#include "reffs/inode.h"
#include "reffs/dirent.h"
#include "nfs4/compound.h"
#include "nfs4/ops.h"
#include "nfs4/errors.h"

_Static_assert(OP_MAX <= REFFS_NFS4_OP_MAX,
	       "OP_MAX exceeds REFFS_NFS4_OP_MAX — bump REFFS_NFS4_OP_MAX");

void nfs4_op_stats_record(struct reffs_op_stats global[REFFS_NFS4_OP_MAX],
			  struct reffs_op_stats sb[REFFS_NFS4_OP_MAX],
			  struct reffs_op_stats client[REFFS_NFS4_OP_MAX],
			  uint32_t op, uint32_t nfs4_status,
			  uint64_t elapsed_ns, uint64_t bytes_in,
			  uint64_t bytes_out)
{
	if (op >= OP_MAX)
		return;

	struct reffs_op_stats *scopes[3] = { global, sb, client };

	for (int i = 0; i < 3; i++) {
		struct reffs_op_stats *s = scopes[i];
		if (!s)
			continue;

		s = &s[op];
		atomic_fetch_add_explicit(&s->os_calls, 1,
					  memory_order_relaxed);
		if (nfs4_status)
			atomic_fetch_add_explicit(&s->os_errors, 1,
						  memory_order_relaxed);
		if (bytes_in)
			atomic_fetch_add_explicit(&s->os_bytes_in, bytes_in,
						  memory_order_relaxed);
		if (bytes_out)
			atomic_fetch_add_explicit(&s->os_bytes_out, bytes_out,
						  memory_order_relaxed);

		atomic_fetch_add_explicit(&s->os_duration_total, elapsed_ns,
					  memory_order_relaxed);

		/* Update high-water mark with a CAS loop. */
		uint64_t cur = atomic_load_explicit(&s->os_duration_max,
						    memory_order_relaxed);
		while (elapsed_ns > cur) {
			if (atomic_compare_exchange_weak_explicit(
				    &s->os_duration_max, &cur, elapsed_ns,
				    memory_order_relaxed, memory_order_relaxed))
				break;
		}
	}
}

uint32_t nfs4_op_secinfo(struct compound *compound)
{
	SECINFO4args *args = NFS4_OP_ARG_SETUP(compound, opsecinfo);
	SECINFO4res *res = NFS4_OP_RES_SETUP(compound, opsecinfo);
	nfsstat4 *status = &res->status;
	SECINFO4resok *resok = NFS4_OP_RESOK_SETUP(res, SECINFO4res_u, resok4);

	struct reffs_dirent *child_de = NULL;
	char *name = NULL;

	if (!compound->c_inode) {
		*status = NFS4ERR_NOFILEHANDLE;
		goto out;
	}

	if (!S_ISDIR(compound->c_inode->i_mode)) {
		*status = NFS4ERR_NOTDIR;
		goto out;
	}

	*status = nfs4_validate_component(&args->name);
	if (*status)
		goto out;

	name = strndup(args->name.utf8string_val, args->name.utf8string_len);
	if (!name) {
		*status = NFS4ERR_DELAY;
		goto out;
	}

	/*
	 * Verify the component exists.  We don't need the child inode
	 * itself — only confirmation that the name is valid.
	 */
	if (!compound->c_inode->i_dirent) {
		int ret = inode_reconstruct_path_to_root(compound->c_inode);

		if (ret) {
			*status = NFS4ERR_STALE;
			goto out;
		}
	}

	child_de = dirent_load_child_by_name(compound->c_inode->i_dirent, name);
	if (!child_de) {
		*status = NFS4ERR_NOENT;
		goto out;
	}

	*status = nfs4_build_secinfo(compound, resok);
	if (*status)
		goto out;

	/*
	 * RFC 8881 s18.29.3: current FH is consumed on success.
	 * Clear both c_inode and c_curr_nfh so subsequent ops
	 * (e.g., GETFH) see NFS4ERR_NOFILEHANDLE.
	 */
	inode_active_put(compound->c_inode);
	compound->c_inode = NULL;
	memset(&compound->c_curr_nfh, 0, sizeof(compound->c_curr_nfh));

out:
	TRACE("SECINFO name=%s status=%s(%d)", name ? name : "(null)",
	      nfs4_err_name(*status), *status);
	free(name);
	dirent_put(child_de);

	return 0;
}

uint32_t nfs4_op_secinfo_no_name(struct compound *compound)
{
	SECINFO_NO_NAME4args *args =
		NFS4_OP_ARG_SETUP(compound, opsecinfo_no_name);
	SECINFO_NO_NAME4res *res =
		NFS4_OP_RES_SETUP(compound, opsecinfo_no_name);
	nfsstat4 *status = &res->status;
	SECINFO4resok *resok = NFS4_OP_RESOK_SETUP(res, SECINFO4res_u, resok4);
	secinfo_style4 style = *args;

	if (!compound->c_inode) {
		*status = NFS4ERR_NOFILEHANDLE;
		goto out;
	}

	/*
	 * RFC 8881 s18.45.3: SECINFO_STYLE4_PARENT on the root of
	 * the pseudo-filesystem has no parent — return NFS4ERR_NOENT,
	 * same as LOOKUPP at the root.
	 */
	if (style == SECINFO_STYLE4_PARENT &&
	    compound->c_curr_nfh.nfh_sb == SUPER_BLOCK_ROOT_ID &&
	    compound->c_curr_nfh.nfh_ino == INODE_ROOT_ID) {
		*status = NFS4ERR_NOENT;
		goto out;
	}

	/*
	 * Both CURRENT_FH and PARENT styles: return the export's
	 * allowed security flavors.
	 *
	 * Per RFC 8881 s18.45.3 the current filehandle is consumed on
	 * success; clear both c_inode and c_curr_nfh so subsequent ops
	 * (e.g., GETFH) see NFS4ERR_NOFILEHANDLE.
	 */
	*status = nfs4_build_secinfo(compound, resok);
	if (*status)
		goto out;

	inode_active_put(compound->c_inode);
	compound->c_inode = NULL;
	memset(&compound->c_curr_nfh, 0, sizeof(compound->c_curr_nfh));

out:
	TRACE("%s style=%d status=%s(%d)", __func__, (int)style,
	      nfs4_err_name(*status), *status);

	return 0;
}

uint32_t nfs4_op_io_advise(struct compound *compound)
{
	IO_ADVISE4res *res = NFS4_OP_RES_SETUP(compound, opio_advise);
	nfsstat4 *status = &res->ior_status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}

uint32_t nfs4_op_illegal(struct compound *compound)
{
	nfs_argop4 *argop =
		&compound->c_args->argarray.argarray_val[compound->c_curr_op];
	nfs_resop4 *resop =
		&compound->c_res->resarray.resarray_val[compound->c_curr_op];
	nfsstat4 *status = &resop->nfs_resop4_u.opillegal.status;

	resop->resop = OP_ILLEGAL;
	*status = NFS4ERR_OP_ILLEGAL;

	TRACE("%s op=%s(%d) status=%s(%d)", __func__,
	      nfs4_op_name(argop->argop), argop->argop, nfs4_err_name(*status),
	      *status);

	return 0;
}
