/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#include <check.h>
#include <stdbool.h>
#include "errors.h"
#include "reffs/test.h"

START_TEST (error1)
{
	ck_assert_int_eq(nfs4_error_valid_for_op(OP_ACCESS, NFS4ERR_ACCESS), true);
}

START_TEST (error2)
{
	ck_assert_int_eq(nfs4_error_valid_for_op(OP_ACCESS, NFS4ERR_ADMIN_REVOKED), false);
}

START_TEST (error3)
{
	ck_assert_int_eq(nfs4_error_valid_for_op(OP_WRITE_SAME, NFS4ERR_WRONG_TYPE), true);
}

START_TEST (error4)
{
	ck_assert_int_eq(nfs4_error_valid_for_op(OP_WRITE_SAME, NFS4ERR_SEQ_FALSE_RETRY), false);
}

START_TEST (error5)
{
	ck_assert_int_eq(nfs4_error_valid_for_cb_op(OP_CB_WANTS_CANCELLED, NFS4ERR_DELAY), true);
}

START_TEST (error6)
{
	ck_assert_int_eq(nfs4_error_valid_for_cb_op(OP_CB_WANTS_CANCELLED, NFS4ERR_SEQ_MISORDERED), false);
}

int main(void)
{
	return 0;
}
