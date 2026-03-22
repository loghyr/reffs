/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include "nfsv42_xdr.h"
#include "reffs/rpc.h"
#include "nfs4/compound.h"
#include "nfs4/ops.h"

uint32_t nfs4_op_chunk_commit(struct compound *compound)
{
	CHUNK_COMMIT4res *res = NFS4_OP_RES_SETUP(compound, opchunk_commit);
	nfsstat4 *status = &res->ccr_status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}

uint32_t nfs4_op_chunk_error(struct compound *compound)
{
	CHUNK_ERROR4res *res = NFS4_OP_RES_SETUP(compound, opchunk_error);
	nfsstat4 *status = &res->cer_status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}

uint32_t nfs4_op_chunk_finalize(struct compound *compound)
{
	CHUNK_FINALIZE4res *res = NFS4_OP_RES_SETUP(compound, opchunk_finalize);
	nfsstat4 *status = &res->cfr_status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}

uint32_t nfs4_op_chunk_header_read(struct compound *compound)
{
	CHUNK_HEADER_READ4res *res =
		NFS4_OP_RES_SETUP(compound, opchunk_header_read);
	nfsstat4 *status = &res->chrr_status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}

uint32_t nfs4_op_chunk_lock(struct compound *compound)
{
	CHUNK_LOCK4res *res = NFS4_OP_RES_SETUP(compound, opchunk_lock);
	nfsstat4 *status = &res->clr_status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}

uint32_t nfs4_op_chunk_read(struct compound *compound)
{
	CHUNK_READ4res *res = NFS4_OP_RES_SETUP(compound, opchunk_read);
	nfsstat4 *status = &res->crr_status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}

uint32_t nfs4_op_chunk_repaired(struct compound *compound)
{
	CHUNK_REPAIRED4res *res = NFS4_OP_RES_SETUP(compound, opchunk_repair);
	nfsstat4 *status = &res->crr_status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}

uint32_t nfs4_op_chunk_rollback(struct compound *compound)
{
	CHUNK_ROLLBACK4res *res = NFS4_OP_RES_SETUP(compound, opchunk_rollback);
	nfsstat4 *status = &res->crbr_status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}

uint32_t nfs4_op_chunk_unlock(struct compound *compound)
{
	CHUNK_UNLOCK4res *res = NFS4_OP_RES_SETUP(compound, opchunk_unlock);
	nfsstat4 *status = &res->cur_status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}

uint32_t nfs4_op_chunk_write(struct compound *compound)
{
	CHUNK_WRITE4res *res = NFS4_OP_RES_SETUP(compound, opchunk_write);
	nfsstat4 *status = &res->cwr_status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}

uint32_t nfs4_op_chunk_write_repair(struct compound *compound)
{
	CHUNK_WRITE_REPAIR4res *res =
		NFS4_OP_RES_SETUP(compound, opchunk_write_repair);
	nfsstat4 *status = &res->cwrr_status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}
