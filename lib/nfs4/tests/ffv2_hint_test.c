/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Unit tests for the FFv2 codec-negotiation hint helpers in
 * lib/nfs4/common/ffv2_hint.c.  Covers the pack side (client
 * writes loga_layouthint.loh_body) and the pick side (MDS decodes
 * it and chooses an ffv2_coding_type4), plus the round-trip.
 *
 * Wire surface: RFC 8881 S18.43 (loga_layouthint) +
 *   draft-haynes-nfsv4-flexfiles-v2 sec-codec-negotiation.
 */

#include <check.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "nfsv42_xdr.h"

#include "nfs4/ffv2_hint.h"

/* ------------------------------------------------------------------ */
/* ffv2_hint_pick                                                      */
/* ------------------------------------------------------------------ */

START_TEST(test_pick_null_hint_default)
{
	uint32_t r = ffv2_hint_pick(NULL, LAYOUT4_FLEX_FILES_V2);

	ck_assert_uint_eq(r, FFV2_ENCODING_PASSTHROUGH);
}
END_TEST

START_TEST(test_pick_non_ffv2_layout_default)
{
	/* Even a fully-formed hint is ignored for non-FFv2 layouts. */
	uint32_t types[] = { FFV2_ENCODING_MIRRORED };
	char *body = NULL;
	uint32_t len = 0;

	ck_assert_int_eq(ffv2_hint_pack(types, 1, &body, &len), 0);

	layouthint4 h = {
		.loh_type = LAYOUT4_FLEX_FILES_V2,
		.loh_body.loh_body_val = body,
		.loh_body.loh_body_len = len,
	};

	ck_assert_uint_eq(ffv2_hint_pick(&h, LAYOUT4_FLEX_FILES),
			  FFV2_ENCODING_PASSTHROUGH);
	ck_assert_uint_eq(ffv2_hint_pick(&h, LAYOUT4_NFSV4_1_FILES),
			  FFV2_ENCODING_PASSTHROUGH);
	free(body);
}
END_TEST

START_TEST(test_pick_loh_type_mismatch_default)
{
	uint32_t types[] = { FFV2_ENCODING_MIRRORED };
	char *body = NULL;
	uint32_t len = 0;

	ck_assert_int_eq(ffv2_hint_pack(types, 1, &body, &len), 0);

	/* loh_type says FFv1, body says MIRRORED -- pick defaults. */
	layouthint4 h = {
		.loh_type = LAYOUT4_FLEX_FILES,
		.loh_body.loh_body_val = body,
		.loh_body.loh_body_len = len,
	};

	ck_assert_uint_eq(ffv2_hint_pick(&h, LAYOUT4_FLEX_FILES_V2),
			  FFV2_ENCODING_PASSTHROUGH);
	free(body);
}
END_TEST

START_TEST(test_pick_empty_body_default)
{
	layouthint4 h = {
		.loh_type = LAYOUT4_FLEX_FILES_V2,
		.loh_body.loh_body_val = NULL,
		.loh_body.loh_body_len = 0,
	};

	ck_assert_uint_eq(ffv2_hint_pick(&h, LAYOUT4_FLEX_FILES_V2),
			  FFV2_ENCODING_PASSTHROUGH);
}
END_TEST

START_TEST(test_pick_garbage_body_default)
{
	/* Random bytes that won't decode as ffv2_layouthint4. */
	char body[] = { (char)0xff, (char)0xff, (char)0xff, (char)0xff, 0x00 };
	layouthint4 h = {
		.loh_type = LAYOUT4_FLEX_FILES_V2,
		.loh_body.loh_body_val = body,
		.loh_body.loh_body_len = sizeof(body),
	};

	uint32_t r = ffv2_hint_pick(&h, LAYOUT4_FLEX_FILES_V2);

	/* Either the decode succeeds with a bogus type (we'd reject and
	 * default) or it fails outright -- both paths return
	 * PASSTHROUGH. */
	ck_assert_uint_eq(r, FFV2_ENCODING_PASSTHROUGH);
}
END_TEST

START_TEST(test_pick_each_known_encoding)
{
	const uint32_t cases[] = {
		FFV2_ENCODING_PASSTHROUGH,
		FFV2_ENCODING_MOJETTE_SYSTEMATIC,
		FFV2_ENCODING_MOJETTE_NON_SYSTEMATIC,
		FFV2_ENCODING_RS_VANDERMONDE,
		FFV2_ENCODING_MIRRORED,
	};

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		uint32_t types[] = { cases[i] };
		char *body = NULL;
		uint32_t len = 0;

		ck_assert_int_eq(ffv2_hint_pack(types, 1, &body, &len), 0);
		ck_assert_uint_gt(len, 0);

		layouthint4 h = {
			.loh_type = LAYOUT4_FLEX_FILES_V2,
			.loh_body.loh_body_val = body,
			.loh_body.loh_body_len = len,
		};

		ck_assert_uint_eq(ffv2_hint_pick(&h, LAYOUT4_FLEX_FILES_V2),
				  cases[i]);
		free(body);
	}
}
END_TEST

START_TEST(test_pick_unknown_first_default)
{
	/* Forward-compat: a future codec value the server doesn't know
	 * about must not be picked. */
	uint32_t types[] = { 0x42 /* unassigned */, FFV2_ENCODING_MIRRORED };
	char *body = NULL;
	uint32_t len = 0;

	ck_assert_int_eq(ffv2_hint_pack(types, 2, &body, &len), 0);

	layouthint4 h = {
		.loh_type = LAYOUT4_FLEX_FILES_V2,
		.loh_body.loh_body_val = body,
		.loh_body.loh_body_len = len,
	};

	/* Server reads ONLY the first entry per the negotiation rule;
	 * unknown means fall back to PASSTHROUGH. */
	ck_assert_uint_eq(ffv2_hint_pick(&h, LAYOUT4_FLEX_FILES_V2),
			  FFV2_ENCODING_PASSTHROUGH);
	free(body);
}
END_TEST

START_TEST(test_pick_first_wins_multi_entry)
{
	/* Order matters: pick honours the client's first preference. */
	uint32_t types[] = {
		FFV2_ENCODING_RS_VANDERMONDE,
		FFV2_ENCODING_PASSTHROUGH,
		FFV2_ENCODING_MIRRORED,
	};
	char *body = NULL;
	uint32_t len = 0;

	ck_assert_int_eq(ffv2_hint_pack(types, 3, &body, &len), 0);

	layouthint4 h = {
		.loh_type = LAYOUT4_FLEX_FILES_V2,
		.loh_body.loh_body_val = body,
		.loh_body.loh_body_len = len,
	};

	ck_assert_uint_eq(ffv2_hint_pick(&h, LAYOUT4_FLEX_FILES_V2),
			  FFV2_ENCODING_RS_VANDERMONDE);
	free(body);
}
END_TEST

/* ------------------------------------------------------------------ */
/* ffv2_hint_pack                                                      */
/* ------------------------------------------------------------------ */

START_TEST(test_pack_null_args_rejected)
{
	uint32_t types[] = { FFV2_ENCODING_MIRRORED };
	char *body = NULL;
	uint32_t len = 0;

	ck_assert_int_eq(ffv2_hint_pack(NULL, 1, &body, &len), -EINVAL);
	ck_assert_int_eq(ffv2_hint_pack(types, 0, &body, &len), -EINVAL);
	ck_assert_int_eq(ffv2_hint_pack(types, 1, NULL, &len), -EINVAL);
	ck_assert_int_eq(ffv2_hint_pack(types, 1, &body, NULL), -EINVAL);
}
END_TEST

START_TEST(test_pack_emits_nonempty_body)
{
	uint32_t types[] = { FFV2_ENCODING_MIRRORED };
	char *body = NULL;
	uint32_t len = 0;

	ck_assert_int_eq(ffv2_hint_pack(types, 1, &body, &len), 0);
	ck_assert_ptr_nonnull(body);
	ck_assert_uint_gt(len, 0);
	/*
	 * XDR encoding of ffv2_layouthint4 with one supported type and
	 * a zero ffv2_data_protection4 is small (< 32 bytes).
	 */
	ck_assert_uint_lt(len, 64);
	free(body);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

Suite *ffv2_hint_suite(void)
{
	Suite *s = suite_create("FFv2 Hint");
	TCase *tc = tcase_create("ffv2_hint");

	tcase_add_test(tc, test_pick_null_hint_default);
	tcase_add_test(tc, test_pick_non_ffv2_layout_default);
	tcase_add_test(tc, test_pick_loh_type_mismatch_default);
	tcase_add_test(tc, test_pick_empty_body_default);
	tcase_add_test(tc, test_pick_garbage_body_default);
	tcase_add_test(tc, test_pick_each_known_encoding);
	tcase_add_test(tc, test_pick_unknown_first_default);
	tcase_add_test(tc, test_pick_first_wins_multi_entry);
	tcase_add_test(tc, test_pack_null_args_rejected);
	tcase_add_test(tc, test_pack_emits_nonempty_body);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	SRunner *sr = srunner_create(ffv2_hint_suite());

	srunner_run_all(sr, CK_NORMAL);
	int nf = srunner_ntests_failed(sr);

	srunner_free(sr);
	return nf ? EXIT_FAILURE : EXIT_SUCCESS;
}
