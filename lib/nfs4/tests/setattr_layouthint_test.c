/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * setattr_layouthint_test -- SETATTR(layout_hint) validation surface.
 *
 * Exercises nfs4_layouthint_validate() directly without the SETATTR
 * compound plumbing.  Covers slice 2 of the Macklem-hint extension
 * per .claude/design/layouthint-mds-hook.md: validate-and-accept
 * the FFv2 layouthint, range-check ffv2lh_stripe_unit, reject
 * non-FFv2 layout types and malformed bodies.
 *
 * The hint is not stored on any inode or consumed at LAYOUTGET in
 * this slice -- those are deferred per the design doc.  These tests
 * confirm the wire-level acceptance path.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <check.h>
#include <stdlib.h>
#include <string.h>

#include "nfsv42_xdr.h"
#include "reffs/darwin_rpc_compat.h"
#include "nfs4/attr.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

/*
 * Encode @lh as the body of a fattr4_layout_hint(LAYOUT4_FLEX_FILES_V2)
 * and call nfs4_layouthint_validate().  Buffer is on the stack -- 256
 * bytes comfortably covers the four-field hint plus the
 * supported_types array head.
 */
static nfsstat4 validate_hint(layouttype4 type, ffv2_layouthint4 *lh)
{
	char buf[256];
	XDR enc;
	fattr4_layout_hint hint;
	u_int len;
	nfsstat4 status;

	xdrmem_create(&enc, buf, sizeof(buf), XDR_ENCODE);
	ck_assert_int_eq(xdr_ffv2_layouthint4(&enc, lh), TRUE);
	len = xdr_getpos(&enc);

	hint.loh_type = type;
	hint.loh_body.loh_body_len = len;
	hint.loh_body.loh_body_val = buf;

	status = nfs4_layouthint_validate(&hint);
	xdr_destroy(&enc);
	return status;
}

/* ------------------------------------------------------------------ */
/* Tests                                                              */
/* ------------------------------------------------------------------ */

START_TEST(test_layouthint_accept_zero_hints)
{
	ffv2_layouthint4 lh = { 0 };
	ck_assert_uint_eq(validate_hint(LAYOUT4_FLEX_FILES_V2, &lh), NFS4_OK);
}
END_TEST

START_TEST(test_layouthint_accept_valid_full_hints)
{
	ffv2_coding_type4 types[1] = { FFV2_ENCODING_RS_VANDERMONDE };
	ffv2_layouthint4 lh = {
		.ffv2lh_supported_types = {
			.ffv2lh_supported_types_len = 1,
			.ffv2lh_supported_types_val = types,
		},
		.ffv2lh_preferred_protection = {
			.fdp_data = 4,
			.fdp_parity = 2,
		},
		.ffv2lh_stripe_unit = 1u << 20,           /* 1 MiB   */
		.ffv2lh_expected_file_size = 16ull << 30, /* 16 GiB  */
	};
	ck_assert_uint_eq(validate_hint(LAYOUT4_FLEX_FILES_V2, &lh), NFS4_OK);
}
END_TEST

START_TEST(test_layouthint_accept_stripe_at_floor)
{
	ffv2_layouthint4 lh = { .ffv2lh_stripe_unit =
					LAYOUTHINT_STRIPE_UNIT_MIN };
	ck_assert_uint_eq(validate_hint(LAYOUT4_FLEX_FILES_V2, &lh), NFS4_OK);
}
END_TEST

START_TEST(test_layouthint_accept_stripe_at_ceiling)
{
	ffv2_layouthint4 lh = { .ffv2lh_stripe_unit =
					LAYOUTHINT_STRIPE_UNIT_MAX };
	ck_assert_uint_eq(validate_hint(LAYOUT4_FLEX_FILES_V2, &lh), NFS4_OK);
}
END_TEST

START_TEST(test_layouthint_reject_stripe_below_floor)
{
	ffv2_layouthint4 lh = { .ffv2lh_stripe_unit = 512 };
	ck_assert_uint_eq(validate_hint(LAYOUT4_FLEX_FILES_V2, &lh),
			  NFS4ERR_INVAL);
}
END_TEST

START_TEST(test_layouthint_reject_stripe_above_ceiling)
{
	ffv2_layouthint4 lh = { .ffv2lh_stripe_unit =
					LAYOUTHINT_STRIPE_UNIT_MAX + 1 };
	ck_assert_uint_eq(validate_hint(LAYOUT4_FLEX_FILES_V2, &lh),
			  NFS4ERR_INVAL);
}
END_TEST

START_TEST(test_layouthint_reject_non_ffv2_type)
{
	ffv2_layouthint4 lh = { 0 };
	ck_assert_uint_eq(validate_hint(LAYOUT4_FLEX_FILES, &lh),
			  NFS4ERR_NOTSUPP);
}
END_TEST

START_TEST(test_layouthint_reject_bad_xdr_body)
{
	/* Hand-build a layouthint with a truncated body that
	 * cannot decode as ffv2_layouthint4.  Two bytes won't
	 * cover the supported_types array length prefix. */
	char buf[2] = { 0 };
	fattr4_layout_hint hint = {
		.loh_type = LAYOUT4_FLEX_FILES_V2,
		.loh_body = {
			.loh_body_len = sizeof(buf),
			.loh_body_val = buf,
		},
	};
	ck_assert_uint_eq(nfs4_layouthint_validate(&hint), NFS4ERR_BADXDR);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                              */
/* ------------------------------------------------------------------ */

static Suite *layouthint_suite(void)
{
	Suite *s = suite_create("setattr_layouthint");
	TCase *tc = tcase_create("validate");

	tcase_add_test(tc, test_layouthint_accept_zero_hints);
	tcase_add_test(tc, test_layouthint_accept_valid_full_hints);
	tcase_add_test(tc, test_layouthint_accept_stripe_at_floor);
	tcase_add_test(tc, test_layouthint_accept_stripe_at_ceiling);
	tcase_add_test(tc, test_layouthint_reject_stripe_below_floor);
	tcase_add_test(tc, test_layouthint_reject_stripe_above_ceiling);
	tcase_add_test(tc, test_layouthint_reject_non_ffv2_type);
	tcase_add_test(tc, test_layouthint_reject_bad_xdr_body);

	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	SRunner *sr = srunner_create(layouthint_suite());
	int failed;

	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);

	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
