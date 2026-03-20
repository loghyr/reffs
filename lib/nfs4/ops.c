/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <stdlib.h>
#include <rpc/auth.h>

#include "nfsv42_xdr.h"
#include "nfsv42_names.h"
#include "reffs/log.h"
#include "reffs/rpc.h"
#include "reffs/inode.h"
#include "nfs4/compound.h"
#include "nfs4/ops.h"
#include "nfs4/errors.h"

void nfs4_op_secinfo(struct compound *compound)
{
	SECINFO4args *args = NFS4_OP_ARG_SETUP(compound, opsecinfo);
	SECINFO4res *res = NFS4_OP_RES_SETUP(compound, opsecinfo);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_secinfo_no_name(struct compound *compound)
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
	 * Both CURRENT_FH and PARENT styles: this server accepts only
	 * AUTH_SYS.  Return a single-entry list regardless of which
	 * object's security the client is asking about.
	 *
	 * Per RFC 8881 s18.45.3 the current filehandle is consumed on
	 * success; clear it here so subsequent ops in the compound get
	 * NFS4ERR_NOFILEHANDLE if they rely on it.
	 */
	(void)style;

	resok->SECINFO4resok_val = calloc(1, sizeof(secinfo4));
	if (!resok->SECINFO4resok_val) {
		*status = NFS4ERR_DELAY;
		goto out;
	}
	resok->SECINFO4resok_len = 1;
	resok->SECINFO4resok_val[0].flavor = AUTH_SYS;

	inode_active_put(compound->c_inode);
	compound->c_inode = NULL;

	*status = NFS4_OK;

out:
	LOG("%s style=%d status=%s(%d)", __func__, (int)style,
	    nfs4_err_name(*status), *status);
}

void nfs4_op_io_advise(struct compound *compound)
{
	IO_ADVISE4args *args = NFS4_OP_ARG_SETUP(compound, opio_advise);
	IO_ADVISE4res *res = NFS4_OP_RES_SETUP(compound, opio_advise);
	nfsstat4 *status = &res->ior_status;
	IO_ADVISE4resok *resok =
		NFS4_OP_RESOK_SETUP(res, IO_ADVISE4res_u, resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_illegal(struct compound *compound)
{
	nfs_argop4 *argop =
		&compound->c_args->argarray.argarray_val[compound->c_curr_op];
	nfs_resop4 *resop =
		&compound->c_res->resarray.resarray_val[compound->c_curr_op];
	nfsstat4 *status = &resop->nfs_resop4_u.opillegal.status;

	resop->resop = OP_ILLEGAL;
	*status = NFS4ERR_OP_ILLEGAL;

	LOG("%s op=%s(%d) status=%s(%d)", __func__, nfs4_op_name(argop->argop),
	    argop->argop, nfs4_err_name(*status), *status);
}
