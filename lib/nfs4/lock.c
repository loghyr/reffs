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

void nfs4_op_lock(struct compound *c)
{
	LOCK4args *args = NFS4_OP_ARG_SETUP(c, oplock);
	LOCK4res *res = NFS4_OP_RES_SETUP(c, oplock);
	nfsstat4 *status = &res->status;
	LOCK4resok *resok = NFS4_OP_RESOK_SETUP(res, LOCK4res_u, resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_lockt(struct compound *c)
{
	LOCKT4args *args = NFS4_OP_ARG_SETUP(c, oplockt);
	LOCKT4res *res = NFS4_OP_RES_SETUP(c, oplockt);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_locku(struct compound *c)
{
	LOCKU4args *args = NFS4_OP_ARG_SETUP(c, oplocku);
	LOCKU4res *res = NFS4_OP_RES_SETUP(c, oplocku);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_free_stateid(struct compound *c)
{
	FREE_STATEID4res *res = NFS4_OP_RES_SETUP(c, opfree_stateid);
	nfsstat4 *status = &res->fsr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)res);
}

void nfs4_op_release_lockowner(struct compound *c)
{
	RELEASE_LOCKOWNER4args *args =
		NFS4_OP_ARG_SETUP(c, oprelease_lockowner);
	RELEASE_LOCKOWNER4res *res = NFS4_OP_RES_SETUP(c, oprelease_lockowner);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_test_stateid(struct compound *c)
{
	TEST_STATEID4args *args = NFS4_OP_ARG_SETUP(c, optest_stateid);
	TEST_STATEID4res *res = NFS4_OP_RES_SETUP(c, optest_stateid);
	nfsstat4 *status = &res->tsr_status;
	TEST_STATEID4resok *resok =
		NFS4_OP_RESOK_SETUP(res, TEST_STATEID4res_u, tsr_resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}
