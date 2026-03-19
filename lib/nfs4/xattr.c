/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include "nfsv42_xdr.h"
#include "nfsv42_names.h"
#include "reffs/log.h"
#include "reffs/rpc.h"
#include "nfs4/compound.h"
#include "nfs4/ops.h"
#include "nfs4/errors.h"

void nfs4_op_getxattr(struct compound *compound)
{
	GETXATTR4args *args = NFS4_OP_ARG_SETUP(compound, opgetxattr);
	GETXATTR4res *res = NFS4_OP_RES_SETUP(compound, opgetxattr);
	nfsstat4 *status = &res->gxr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_setxattr(struct compound *compound)
{
	SETXATTR4args *args = NFS4_OP_ARG_SETUP(compound, opsetxattr);
	SETXATTR4res *res = NFS4_OP_RES_SETUP(compound, opsetxattr);
	nfsstat4 *status = &res->sxr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_listxattrs(struct compound *compound)
{
	LISTXATTRS4args *args = NFS4_OP_ARG_SETUP(compound, oplistxattrs);
	LISTXATTRS4res *res = NFS4_OP_RES_SETUP(compound, oplistxattrs);
	nfsstat4 *status = &res->lxr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_removexattr(struct compound *compound)
{
	REMOVEXATTR4args *args = NFS4_OP_ARG_SETUP(compound, opremovexattr);
	REMOVEXATTR4res *res = NFS4_OP_RES_SETUP(compound, opremovexattr);
	nfsstat4 *status = &res->rxr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}
