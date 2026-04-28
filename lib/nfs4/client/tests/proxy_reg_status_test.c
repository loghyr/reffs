/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Unit tests for proxy_reg_nfsstat_to_errno -- the PROXY_REGISTRATION
 * COMPOUND/op nfsstat4 -> negative-errno mapping the PS startup loop
 * uses.  Pinned here so future edits to the mapping table can't
 * silently change the operator-visible error surface.
 *
 * The mapping table covers seven nfsstat4 values explicitly; every
 * other status falls through to -EIO.  Test all seven plus a
 * representative unmapped value.
 */

#include <check.h>
#include <errno.h>

#include "nfsv42_xdr.h"
#include "ec_client.h"

START_TEST(test_ok_maps_to_zero)
{
	ck_assert_int_eq(proxy_reg_nfsstat_to_errno(NFS4_OK), 0);
}
END_TEST

START_TEST(test_perm_maps_to_eperm)
{
	ck_assert_int_eq(proxy_reg_nfsstat_to_errno(NFS4ERR_PERM), -EPERM);
}
END_TEST

START_TEST(test_delay_maps_to_eagain)
{
	ck_assert_int_eq(proxy_reg_nfsstat_to_errno(NFS4ERR_DELAY), -EAGAIN);
}
END_TEST

START_TEST(test_inval_maps_to_einval)
{
	ck_assert_int_eq(proxy_reg_nfsstat_to_errno(NFS4ERR_INVAL), -EINVAL);
}
END_TEST

START_TEST(test_notsupp_and_op_illegal_map_to_enosys)
{
	ck_assert_int_eq(proxy_reg_nfsstat_to_errno(NFS4ERR_NOTSUPP), -ENOSYS);
	ck_assert_int_eq(proxy_reg_nfsstat_to_errno(NFS4ERR_OP_ILLEGAL),
			 -ENOSYS);
}
END_TEST

START_TEST(test_badxdr_maps_to_ebadmsg)
{
	ck_assert_int_eq(proxy_reg_nfsstat_to_errno(NFS4ERR_BADXDR), -EBADMSG);
}
END_TEST

START_TEST(test_op_not_in_session_maps_to_eproto)
{
	ck_assert_int_eq(proxy_reg_nfsstat_to_errno(NFS4ERR_OP_NOT_IN_SESSION),
			 -EPROTO);
}
END_TEST

START_TEST(test_unmapped_default_eio)
{
	/*
	 * NFS4ERR_STALE is not on the explicit map; every unmapped
	 * status must fall through to -EIO.  If a future slice adds
	 * STALE to the table, replace this assertion (don't drop
	 * the unmapped-default coverage).
	 */
	ck_assert_int_eq(proxy_reg_nfsstat_to_errno(NFS4ERR_STALE), -EIO);
}
END_TEST

static Suite *proxy_reg_status_suite(void)
{
	Suite *s = suite_create("proxy_reg_status");
	TCase *tc = tcase_create("core");

	tcase_add_test(tc, test_ok_maps_to_zero);
	tcase_add_test(tc, test_perm_maps_to_eperm);
	tcase_add_test(tc, test_delay_maps_to_eagain);
	tcase_add_test(tc, test_inval_maps_to_einval);
	tcase_add_test(tc, test_notsupp_and_op_illegal_map_to_enosys);
	tcase_add_test(tc, test_badxdr_maps_to_ebadmsg);
	tcase_add_test(tc, test_op_not_in_session_maps_to_eproto);
	tcase_add_test(tc, test_unmapped_default_eio);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	SRunner *sr = srunner_create(proxy_reg_status_suite());

	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return failed == 0 ? 0 : 1;
}
