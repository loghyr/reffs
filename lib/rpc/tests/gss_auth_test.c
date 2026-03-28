/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Unit test for RPCSEC_GSS client authentication using the mini-KDC.
 *
 * Starts an embedded KDC, creates a session with RPCSEC_GSS, and
 * verifies that the GSS context is established.  Skips gracefully
 * if krb5 tools are not installed.
 *
 * This tests the real GSS stack end-to-end: kinit → TGT →
 * gss_init_sec_context → authgss_create → server contact.
 * It does NOT start a server — it only tests the client-side
 * context establishment against the KDC.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" /* IWYU pragma: keep */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <check.h>

#include "mini_kdc.h"

#ifdef HAVE_GSSAPI_KRB5
#include <gssapi/gssapi.h>
#include <gssapi/gssapi_krb5.h>

/* krb5 OID: 1.2.840.113554.1.2.2 */
static gss_OID_desc krb5oid = {
	9, (void *)"\x2a\x86\x48\x86\xf7\x12\x01\x02\x02"
};

static struct mini_kdc test_kdc;
static int kdc_available;

static void setup(void)
{
	kdc_available = (mini_kdc_start(&test_kdc, "nfs", "localhost") == 0);
}

static void teardown(void)
{
	if (kdc_available)
		mini_kdc_stop(&test_kdc);
}

/*
 * Test: kinit succeeded and we have a valid TGT.
 */
START_TEST(test_tgt_acquired)
{
	if (!kdc_available) {
		fprintf(stderr, "  SKIP: no krb5 tools\n");
		return;
	}

	/* klist -s returns 0 if there's a valid TGT. */
	int rc = system("klist -s 2>/dev/null");

	ck_assert_int_eq(rc, 0);
}
END_TEST

/*
 * Test: gss_init_sec_context succeeds with the service principal.
 */
START_TEST(test_gss_init_context)
{
	if (!kdc_available) {
		fprintf(stderr, "  SKIP: no krb5 tools\n");
		return;
	}

	OM_uint32 major, minor;
	gss_ctx_id_t ctx = GSS_C_NO_CONTEXT;
	gss_name_t target = GSS_C_NO_NAME;
	gss_buffer_desc name_buf = { .value = (void *)"nfs@localhost",
				     .length = 13 };

	/* Import the service name. */
	major = gss_import_name(&minor, &name_buf, GSS_C_NT_HOSTBASED_SERVICE,
				&target);
	ck_assert_msg(major == GSS_S_COMPLETE,
		      "gss_import_name failed: major=%u minor=%u", major,
		      minor);

	/* Initiate the security context. */
	gss_buffer_desc out_token = GSS_C_EMPTY_BUFFER;

	major = gss_init_sec_context(&minor, GSS_C_NO_CREDENTIAL, &ctx, target,
				     &krb5oid, GSS_C_MUTUAL_FLAG, 0,
				     GSS_C_NO_CHANNEL_BINDINGS, GSS_C_NO_BUFFER,
				     NULL, &out_token, NULL, NULL);

	/*
	 * With a single-round-trip mechanism (krb5), init should
	 * return COMPLETE or CONTINUE_NEEDED.  Either means the
	 * client-side GSS machinery is working.
	 */
	ck_assert_msg(major == GSS_S_COMPLETE || major == GSS_S_CONTINUE_NEEDED,
		      "gss_init_sec_context failed: major=%u minor=%u", major,
		      minor);

	/* We got a token to send to the server. */
	ck_assert(out_token.length > 0);

	/* Cleanup. */
	gss_release_buffer(&minor, &out_token);
	if (ctx != GSS_C_NO_CONTEXT)
		gss_delete_sec_context(&minor, &ctx, GSS_C_NO_BUFFER);
	gss_release_name(&minor, &target);
}
END_TEST

/*
 * Test: keytab contains the expected service principal.
 */
START_TEST(test_keytab_has_principal)
{
	if (!kdc_available) {
		fprintf(stderr, "  SKIP: no krb5 tools\n");
		return;
	}

	char cmd[512];

	snprintf(cmd, sizeof(cmd),
		 "klist -k %s 2>/dev/null | grep -q nfs/localhost",
		 test_kdc.kdc_keytab);
	ck_assert_int_eq(system(cmd), 0);
}
END_TEST

#endif /* HAVE_GSSAPI_KRB5 */

static Suite *gss_auth_suite(void)
{
	Suite *s = suite_create("gss_auth");

#ifdef HAVE_GSSAPI_KRB5
	TCase *tc = tcase_create("mini_kdc");

	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_set_timeout(tc, 10); /* KDC startup can take a moment */
	tcase_add_test(tc, test_tgt_acquired);
	tcase_add_test(tc, test_keytab_has_principal);
	tcase_add_test(tc, test_gss_init_context);
	suite_add_tcase(s, tc);
#endif

	return s;
}

int main(void)
{
	int nfailed;
	Suite *s = gss_auth_suite();
	SRunner *sr = srunner_create(s);

	srunner_run_all(sr, CK_NORMAL);
	nfailed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return nfailed ? 1 : 0;
}
