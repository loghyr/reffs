/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#include "errors.h"
#include "reffs/test.h"

int main(void)
{
	verify(nfs4_error_valid_for_op(OP_ACCESS, NFS4ERR_ACCESS));
	verify(!nfs4_error_valid_for_op(OP_ACCESS, NFS4ERR_ADMIN_REVOKED));
	verify(nfs4_error_valid_for_op(OP_WRITE_SAME, NFS4ERR_WRONG_TYPE));
	verify(!nfs4_error_valid_for_op(OP_WRITE_SAME, NFS4ERR_SEQ_FALSE_RETRY));
	verify(nfs4_error_valid_for_cb_op(OP_CB_WANTS_CANCELLED, NFS4ERR_DELAY));
	verify(!nfs4_error_valid_for_cb_op(OP_CB_WANTS_CANCELLED, NFS4ERR_SEQ_MISORDERED));
	return 0;
}
