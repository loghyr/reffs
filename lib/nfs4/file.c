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

void nfs4_op_open(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	OPEN4args *args = NFS4_OP_ARG_SETUP(c, ph, opopen);
	OPEN4res *res = NFS4_OP_RES_SETUP(c, ph, opopen);
	nfsstat4 *status = &res->status;
	OPEN4resok *resok = NFS4_OP_RESOK_SETUP(res, OPEN4res_u, resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_open_confirm(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	OPEN_CONFIRM4args *args = NFS4_OP_ARG_SETUP(c, ph, opopen_confirm);
	OPEN_CONFIRM4res *res = NFS4_OP_RES_SETUP(c, ph, opopen_confirm);
	nfsstat4 *status = &res->status;
	OPEN_CONFIRM4resok *resok =
		NFS4_OP_RESOK_SETUP(res, OPEN_CONFIRM4res_u, resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_open_downgrade(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	OPEN_DOWNGRADE4args *args = NFS4_OP_ARG_SETUP(c, ph, opopen_downgrade);
	OPEN_DOWNGRADE4res *res = NFS4_OP_RES_SETUP(c, ph, opopen_downgrade);
	nfsstat4 *status = &res->status;
	OPEN_DOWNGRADE4resok *resok =
		NFS4_OP_RESOK_SETUP(res, OPEN_DOWNGRADE4res_u, resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_close(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	CLOSE4args *args = NFS4_OP_ARG_SETUP(c, ph, opclose);
	CLOSE4res *res = NFS4_OP_RES_SETUP(c, ph, opclose);
	nfsstat4 *status = &res->status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_read(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	READ4args *args = NFS4_OP_ARG_SETUP(c, ph, opread);
	READ4res *res = NFS4_OP_RES_SETUP(c, ph, opread);
	nfsstat4 *status = &res->status;
	READ4resok *resok = NFS4_OP_RESOK_SETUP(res, READ4res_u, resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_read_plus(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	READ_PLUS4args *args = NFS4_OP_ARG_SETUP(c, ph, opread_plus);
	READ_PLUS4res *res = NFS4_OP_RES_SETUP(c, ph, opread_plus);
	nfsstat4 *status = &res->rp_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_write(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	WRITE4args *args = NFS4_OP_ARG_SETUP(c, ph, opwrite);
	WRITE4res *res = NFS4_OP_RES_SETUP(c, ph, opwrite);
	nfsstat4 *status = &res->status;
	WRITE4resok *resok = NFS4_OP_RESOK_SETUP(res, WRITE4res_u, resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_write_same(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	WRITE_SAME4args *args = NFS4_OP_ARG_SETUP(c, ph, opwrite_same);
	WRITE_SAME4res *res = NFS4_OP_RES_SETUP(c, ph, opwrite_same);
	nfsstat4 *status = &res->wsr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_commit(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	COMMIT4args *args = NFS4_OP_ARG_SETUP(c, ph, opcommit);
	COMMIT4res *res = NFS4_OP_RES_SETUP(c, ph, opcommit);
	nfsstat4 *status = &res->status;
	COMMIT4resok *resok = NFS4_OP_RESOK_SETUP(res, COMMIT4res_u, resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_seek(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	SEEK4args *args = NFS4_OP_ARG_SETUP(c, ph, opseek);
	SEEK4res *res = NFS4_OP_RES_SETUP(c, ph, opseek);
	nfsstat4 *status = &res->sa_status;
	seek_res4 *resok = NFS4_OP_RESOK_SETUP(res, SEEK4res_u, resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_allocate(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	ALLOCATE4args *args = NFS4_OP_ARG_SETUP(c, ph, opallocate);
	ALLOCATE4res *res = NFS4_OP_RES_SETUP(c, ph, opallocate);
	nfsstat4 *status = &res->ar_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_deallocate(struct compound *c)
{
	struct protocol_handler *ph =
		(struct protocol_handler *)c->c_rt->rt_context;

	DEALLOCATE4args *args = NFS4_OP_ARG_SETUP(c, ph, opdeallocate);
	DEALLOCATE4res *res = NFS4_OP_RES_SETUP(c, ph, opdeallocate);
	nfsstat4 *status = &res->dr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}
