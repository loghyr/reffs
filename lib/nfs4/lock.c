/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include "nfsv42_xdr.h"
#include "reffs/log.h"
#include "reffs/rpc.h"
#include "nfs4_internal.h"
#include "ops.h"
#include "errors.h"

void nfs4_op_lock(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	LOCK4args *args = NFS4_OP_ARG_SETUP(c, ph, oplock);
	LOCK4res *res = NFS4_OP_RES_SETUP(c, ph, oplock);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_lockt(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	LOCKT4args *args = NFS4_OP_ARG_SETUP(c, ph, oplockt);
	LOCKT4res *res = NFS4_OP_RES_SETUP(c, ph, oplockt);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_locku(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	LOCKU4args *args = NFS4_OP_ARG_SETUP(c, ph, oplocku);
	LOCKU4res *res = NFS4_OP_RES_SETUP(c, ph, oplocku);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_free_stateid(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	FREE_STATEID4res *res = NFS4_OP_RES_SETUP(c, ph, opfree_stateid);
	nfsstat4 *status = &res->fsr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)res);
}

void nfs4_op_release_lockowner(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	RELEASE_LOCKOWNER4res *res =
		NFS4_OP_RES_SETUP(c, ph, oprelease_lockowner);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)res);
}

void nfs4_op_test_stateid(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	TEST_STATEID4args *args = NFS4_OP_ARG_SETUP(c, ph, optest_stateid);
	TEST_STATEID4res *res = NFS4_OP_RES_SETUP(c, ph, optest_stateid);
	nfsstat4 *status = &res->tsr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}
