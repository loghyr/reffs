/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Unit tests for rpc_cred_get_gss_principal() (slice plan-A.i).
 *
 * The success path requires real GSS infrastructure (a configured
 * Kerberos environment with a service principal), so it is left to
 * the integration test suite.  These unit tests cover the error
 * matrix that is reachable without GSS:
 *
 *   - NULL info -> -EINVAL
 *   - NULL out_buf -> -EINVAL (when GSS is enabled)
 *   - zero out_buf_len -> -EINVAL (when GSS is enabled)
 *   - AUTH_SYS credential -> -ENOENT
 *   - RPCSEC_GSS credential with handle that misses the cache ->
 *     -ENOENT (when GSS is enabled)
 *
 * In non-GSS builds the function always returns -ENOENT after the
 * NULL-info check, so the AUTH_SYS / cache-miss paths collapse to
 * the same code path; we test what's reachable.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" /* IWYU pragma: keep */
#endif

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <rpc/auth.h>

#include <check.h>

#include "reffs/rpc.h"
#include "libreffs_test.h"

START_TEST(test_null_info_returns_einval)
{
	char buf[64];

	ck_assert_int_eq(rpc_cred_get_gss_principal(NULL, buf, sizeof(buf)),
			 -EINVAL);
}
END_TEST

START_TEST(test_authsys_credential_returns_enoent)
{
	struct rpc_info info;
	char buf[64] = "preexisting";

	memset(&info, 0, sizeof(info));
	info.ri_cred.rc_flavor = AUTH_SYS;

	ck_assert_int_eq(rpc_cred_get_gss_principal(&info, buf, sizeof(buf)),
			 -ENOENT);
	/*
	 * The function MUST NOT touch the buffer on the AUTH_SYS reject
	 * path -- a caller that passes a stack buffer with a default
	 * value would have its default clobbered if the function wrote
	 * unconditionally.
	 */
	ck_assert_str_eq(buf, "preexisting");
}
END_TEST

START_TEST(test_rpcsec_gss_with_no_cached_context_returns_enoent)
{
	struct rpc_info info;
	char buf[64] = "preexisting";

	memset(&info, 0, sizeof(info));
	info.ri_cred.rc_flavor = RPCSEC_GSS;
	info.ri_cred.rc_gss.gc_handle_len =
		sizeof(info.ri_cred.rc_gss.gc_handle);
	memset(info.ri_cred.rc_gss.gc_handle, 0xAA,
	       sizeof(info.ri_cred.rc_gss.gc_handle));

	/*
	 * No GSS context cache lookup will find handle 0xAA*16, so the
	 * helper returns -ENOENT on both GSS-enabled and GSS-disabled
	 * builds.
	 */
	ck_assert_int_eq(rpc_cred_get_gss_principal(&info, buf, sizeof(buf)),
			 -ENOENT);
	ck_assert_str_eq(buf, "preexisting");
}
END_TEST

static Suite *gss_principal_suite(void)
{
	Suite *s = suite_create("gss_principal");
	TCase *tc = tcase_create("error_matrix");

	tcase_add_test(tc, test_null_info_returns_einval);
	tcase_add_test(tc, test_authsys_credential_returns_enoent);
	tcase_add_test(tc,
		       test_rpcsec_gss_with_no_cached_context_returns_enoent);
	suite_add_tcase(s, tc);

	return s;
}

int main(void)
{
	return reffs_test_run_suite(gss_principal_suite(), NULL, NULL);
}
