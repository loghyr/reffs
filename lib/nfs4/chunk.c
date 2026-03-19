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

void nfs4_op_chunk_commit(struct compound *c)
{
	CHUNK_COMMIT4args *args = NFS4_OP_ARG_SETUP(c, opchunk_commit);
	CHUNK_COMMIT4res *res = NFS4_OP_RES_SETUP(c, opchunk_commit);
	nfsstat4 *status = &res->ccr_status;
	CHUNK_COMMIT4resok *resok =
		NFS4_OP_RESOK_SETUP(res, CHUNK_COMMIT4res_u, ccr_resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_chunk_error(struct compound *c)
{
	CHUNK_ERROR4args *args = NFS4_OP_ARG_SETUP(c, opchunk_error);
	CHUNK_ERROR4res *res = NFS4_OP_RES_SETUP(c, opchunk_error);
	nfsstat4 *status = &res->cer_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_chunk_finalize(struct compound *c)
{
	CHUNK_FINALIZE4args *args = NFS4_OP_ARG_SETUP(c, opchunk_finalize);
	CHUNK_FINALIZE4res *res = NFS4_OP_RES_SETUP(c, opchunk_finalize);
	nfsstat4 *status = &res->cfr_status;
	CHUNK_FINALIZE4resok *resok =
		NFS4_OP_RESOK_SETUP(res, CHUNK_FINALIZE4res_u, cfr_resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_chunk_header_read(struct compound *c)
{
	CHUNK_HEADER_READ4args *args =
		NFS4_OP_ARG_SETUP(c, opchunk_header_read);
	CHUNK_HEADER_READ4res *res = NFS4_OP_RES_SETUP(c, opchunk_header_read);
	nfsstat4 *status = &res->chrr_status;
	CHUNK_HEADER_READ4resok *resok =
		NFS4_OP_RESOK_SETUP(res, CHUNK_HEADER_READ4res_u, chrr_resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_chunk_lock(struct compound *c)
{
	CHUNK_LOCK4args *args = NFS4_OP_ARG_SETUP(c, opchunk_lock);
	CHUNK_LOCK4res *res = NFS4_OP_RES_SETUP(c, opchunk_lock);
	nfsstat4 *status = &res->clr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_chunk_read(struct compound *c)
{
	CHUNK_READ4args *args = NFS4_OP_ARG_SETUP(c, opchunk_read);
	CHUNK_READ4res *res = NFS4_OP_RES_SETUP(c, opchunk_read);
	nfsstat4 *status = &res->crr_status;
	CHUNK_READ4resok *resok =
		NFS4_OP_RESOK_SETUP(res, CHUNK_READ4res_u, crr_resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_chunk_repaired(struct compound *c)
{
	CHUNK_REPAIRED4args *args = NFS4_OP_ARG_SETUP(c, opchunk_repair);
	CHUNK_REPAIRED4res *res = NFS4_OP_RES_SETUP(c, opchunk_repair);
	nfsstat4 *status = &res->crr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_chunk_rollback(struct compound *c)
{
	CHUNK_ROLLBACK4args *args = NFS4_OP_ARG_SETUP(c, opchunk_rollback);
	CHUNK_ROLLBACK4res *res = NFS4_OP_RES_SETUP(c, opchunk_rollback);
	nfsstat4 *status = &res->crbr_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_chunk_unlock(struct compound *c)
{
	CHUNK_UNLOCK4args *args = NFS4_OP_ARG_SETUP(c, opchunk_unlock);
	CHUNK_UNLOCK4res *res = NFS4_OP_RES_SETUP(c, opchunk_unlock);
	nfsstat4 *status = &res->cur_status;

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p", __func__, nfs4_err_name(*status),
	    *status, (void *)args, (void *)res);
}

void nfs4_op_chunk_write(struct compound *c)
{
	CHUNK_WRITE4args *args = NFS4_OP_ARG_SETUP(c, opchunk_write);
	CHUNK_WRITE4res *res = NFS4_OP_RES_SETUP(c, opchunk_write);
	nfsstat4 *status = &res->cwr_status;
	CHUNK_WRITE4resok *resok =
		NFS4_OP_RESOK_SETUP(res, CHUNK_WRITE4res_u, cwr_resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}

void nfs4_op_chunk_write_repair(struct compound *c)
{
	CHUNK_WRITE_REPAIR4args *args =
		NFS4_OP_ARG_SETUP(c, opchunk_write_repair);
	CHUNK_WRITE_REPAIR4res *res =
		NFS4_OP_RES_SETUP(c, opchunk_write_repair);
	nfsstat4 *status = &res->cwrr_status;
	CHUNK_WRITE_REPAIR4resok *resok =
		NFS4_OP_RESOK_SETUP(res, CHUNK_WRITE_REPAIR4res_u, cwrr_resok4);

	*status = NFS4ERR_NOTSUPP;

	LOG("%s status=%s(%d) args=%p res=%p resok=%p", __func__,
	    nfs4_err_name(*status), *status, (void *)args, (void *)res,
	    (void *)resok);
}
