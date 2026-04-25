/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * Unit coverage for ps_proxy_compound_is_gss -- the gate that
 * refuses RPCSEC_GSS-authed compounds on the proxy fast-path
 * until full RPCSEC_GSSv3 forwarding is implemented (see
 * proxy-server.md Action Item 3).
 *
 * The helper is small (one field comparison + NULL guards) and
 * the entire failure mode that surfaces it on the wire is the
 * NFS4ERR_WRONGSEC the per-op proxy fast-paths return when the
 * helper fires.  This test pins each branch directly so a future
 * refactor that loses the NULL guard or flips the flavor compare
 * is caught here, not at op-handler integration time.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <check.h>
#include <stddef.h>
#include <string.h>

#include "reffs/rpc.h"
#include "nfs4/compound.h"

#include "ps_security.h"

/*
 * Synthesise a minimal compound + rpc_trans + rpc_info on the
 * stack so the helper has something to inspect.  The helper only
 * touches compound->c_rt->rt_info.ri_cred.rc_flavor -- the rest
 * of the structures stay zeroed.
 */
static void make_compound(struct compound *c, struct rpc_trans *rt,
			  uint32_t flavor)
{
	memset(c, 0, sizeof(*c));
	memset(rt, 0, sizeof(*rt));
	rt->rt_info.ri_cred.rc_flavor = flavor;
	c->c_rt = rt;
}

START_TEST(test_auth_sys_is_not_gss)
{
	struct compound c;
	struct rpc_trans rt;

	make_compound(&c, &rt, AUTH_SYS);
	ck_assert(!ps_proxy_compound_is_gss(&c));
}
END_TEST

START_TEST(test_auth_none_is_not_gss)
{
	struct compound c;
	struct rpc_trans rt;

	make_compound(&c, &rt, AUTH_NONE);
	ck_assert(!ps_proxy_compound_is_gss(&c));
}
END_TEST

START_TEST(test_rpcsec_gss_is_gss)
{
	struct compound c;
	struct rpc_trans rt;

	make_compound(&c, &rt, RPCSEC_GSS);
	ck_assert(ps_proxy_compound_is_gss(&c));
}
END_TEST

/*
 * The dispatcher in dispatch.c calls per-op handlers that may
 * carry a NULL c_rt before the SEQUENCE op runs.  Helper must
 * NULL-guard rather than dereferencing.
 */
START_TEST(test_null_compound_is_not_gss)
{
	ck_assert(!ps_proxy_compound_is_gss(NULL));
}
END_TEST

START_TEST(test_null_c_rt_is_not_gss)
{
	struct compound c;

	memset(&c, 0, sizeof(c));
	c.c_rt = NULL;
	ck_assert(!ps_proxy_compound_is_gss(&c));
}
END_TEST

static Suite *ps_security_suite(void)
{
	Suite *s = suite_create("ps_security");
	TCase *tc = tcase_create("gss_gate");

	tcase_add_test(tc, test_auth_sys_is_not_gss);
	tcase_add_test(tc, test_auth_none_is_not_gss);
	tcase_add_test(tc, test_rpcsec_gss_is_gss);
	tcase_add_test(tc, test_null_compound_is_not_gss);
	tcase_add_test(tc, test_null_c_rt_is_not_gss);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	SRunner *sr = srunner_create(ps_security_suite());

	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return failed == 0 ? 0 : 1;
}
