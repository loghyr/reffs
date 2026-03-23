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

uint32_t nfs4_op_copy(struct compound *compound)
{
	COPY4res *res = NFS4_OP_RES_SETUP(compound, opcopy);
	nfsstat4 *status = &res->cr_status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}

uint32_t nfs4_op_copy_notify(struct compound *compound)
{
	COPY_NOTIFY4res *res = NFS4_OP_RES_SETUP(compound, opcopy_notify);
	nfsstat4 *status = &res->cnr_status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}

uint32_t nfs4_op_clone(struct compound *compound)
{
	CLONE4res *res = NFS4_OP_RES_SETUP(compound, opclone);
	nfsstat4 *status = &res->cl_status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}
uint32_t nfs4_op_offload_cancel(struct compound *compound)
{
	OFFLOAD_CANCEL4res *res = NFS4_OP_RES_SETUP(compound, opoffload_cancel);
	nfsstat4 *status = &res->ocr_status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}

uint32_t nfs4_op_offload_status(struct compound *compound)
{
	OFFLOAD_STATUS4res *res = NFS4_OP_RES_SETUP(compound, opoffload_status);
	nfsstat4 *status = &res->osr_status;

	*status = NFS4ERR_NOTSUPP;

	return 0;
}
