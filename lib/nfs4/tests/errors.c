/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <check.h>
#include <stdbool.h>
#include "errors.h"
#include "reffs/test.h"

START_TEST(test_error1)
{
	ck_assert_int_eq(nfs4_error_valid_for_op(OP_ACCESS, NFS4ERR_ACCESS),
			 true);
}

START_TEST(test_error2)
{
	ck_assert_int_eq(nfs4_error_valid_for_op(OP_ACCESS,
						 NFS4ERR_ADMIN_REVOKED),
			 false);
}

START_TEST(test_error3)
{
	ck_assert_int_eq(nfs4_error_valid_for_op(OP_WRITE_SAME,
						 NFS4ERR_WRONG_TYPE),
			 true);
}

START_TEST(test_error4)
{
	ck_assert_int_eq(nfs4_error_valid_for_op(OP_WRITE_SAME,
						 NFS4ERR_SEQ_FALSE_RETRY),
			 false);
}

START_TEST(test_error5)
{
	ck_assert_int_eq(nfs4_error_valid_for_cb_op(OP_CB_WANTS_CANCELLED,
						    NFS4ERR_DELAY),
			 true);
}

START_TEST(test_error6)
{
	ck_assert_int_eq(nfs4_error_valid_for_cb_op(OP_CB_WANTS_CANCELLED,
						    NFS4ERR_SEQ_MISORDERED),
			 false);
}

Suite *error_suite(void)
{
	Suite *s;
	TCase *tc_core;

	s = suite_create("NFSv4 Errors");

	/* Core test case */
	tc_core = tcase_create("Core");

	tcase_add_test(tc_core, test_error1);
	tcase_add_test(tc_core, test_error2);
	tcase_add_test(tc_core, test_error3);
	tcase_add_test(tc_core, test_error4);
	tcase_add_test(tc_core, test_error5);
	tcase_add_test(tc_core, test_error6);
	suite_add_tcase(s, tc_core);

	return s;
}

int main(void)
{
	int number_failed;
	Suite *s;
	SRunner *sr;

	s = error_suite();
	sr = srunner_create(s);

	srunner_run_all(sr, CK_NORMAL);
	number_failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
