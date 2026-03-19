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

void nfs4_op_getxattr(struct compound *c)
{
	GETXATTR4args *args = NFS4_OP_ARG_SETUP(c, opgetxattr);
	GETXATTR4res *res = NFS4_OP_RES_SETUP(c, opgetxattr);
	nfsstat4 *status = &res->gxr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_setxattr(struct compound *c)
{
	SETXATTR4args *args = NFS4_OP_ARG_SETUP(c, opsetxattr);
	SETXATTR4res *res = NFS4_OP_RES_SETUP(c, opsetxattr);
	nfsstat4 *status = &res->sxr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_listxattrs(struct compound *c)
{
	LISTXATTRS4args *args = NFS4_OP_ARG_SETUP(c, oplistxattrs);
	LISTXATTRS4res *res = NFS4_OP_RES_SETUP(c, oplistxattrs);
	nfsstat4 *status = &res->lxr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_removexattr(struct compound *c)
{
	REMOVEXATTR4args *args = NFS4_OP_ARG_SETUP(c, opremovexattr);
	REMOVEXATTR4res *res = NFS4_OP_RES_SETUP(c, opremovexattr);
	nfsstat4 *status = &res->rxr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}
