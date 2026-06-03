/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * default_coding_dispatch_test.c -- unit tests for the LAYOUTGET
 * dispatch helpers added in step 5 of
 * .claude/design/per-export-default-coding.md.
 *
 * These exercise the two pure helpers
 * (default_coding_resolve_target and
 * default_coding_resolve_segment) directly, without spinning up
 * the full LAYOUTGET integration fixture (compound +
 * super_block + dstores + runway).  The helpers contain the
 * plan-review B2 fix -- "runway target drives k/m; reject
 * NFS4ERR_LAYOUTUNAVAILABLE on short runway instead of
 * silently degrading geometry" -- so unit-testing them in
 * isolation gives high-confidence coverage of the bug-fix
 * site.
 *
 * The end-to-end LAYOUTGET-issues-ffm_coding_type behaviour
 * is exercised by step 10's bench (functional test in the
 * design's "Functional tests" section).
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

#include <check.h>

#include "nfsv42_xdr.h"
#include "reffs/coding_spec.h"
#include "libreffs_test.h"

/*
 * Forward decls of the helpers under test.  These are defined
 * non-static in lib/nfs4/server/layout.c; we declare them here
 * rather than spinning up a private test header for two
 * symbols.  Keep in sync with the layout.c definitions.
 */
uint32_t default_coding_resolve_target(const struct reffs_coding_spec *coding,
				       layouttype4 layout_type, uint32_t nds,
				       uint32_t ss_layout_width);

int default_coding_resolve_segment(const struct reffs_coding_spec *coding,
				   uint32_t nfiles, uint16_t *out_k,
				   uint16_t *out_m, uint32_t *out_codec_type);

/* ------------------------------------------------------------------ */
/* default_coding_resolve_target                                       */
/* ------------------------------------------------------------------ */

/*
 * File layouts always return nds regardless of the sb's default
 * coding -- file layout is single-DS-per-layout, no mirroring.
 */
START_TEST(test_target_file_layout_returns_nds)
{
	struct reffs_coding_spec rs = {
		.cs_codec_type = REFFS_CODEC_RS_VANDERMONDE,
		.cs_k = 4,
		.cs_m = 2,
	};

	/* nds=7, ss_layout_width=10: file layouts use nds, not k+m. */
	ck_assert_uint_eq(default_coding_resolve_target(
				  &rs, LAYOUT4_NFSV4_1_FILES, 7, 10),
			  7);
}
END_TEST

/*
 * Flex files with explicit default_coding: target = k + m,
 * NOT ss_layout_width.  This is the headline behaviour change
 * for per-export codec selection.
 */
START_TEST(test_target_flex_files_uses_k_plus_m)
{
	struct reffs_coding_spec rs = {
		.cs_codec_type = REFFS_CODEC_RS_VANDERMONDE,
		.cs_k = 4,
		.cs_m = 2,
	};

	/* ss_layout_width=10 must NOT be used when default_coding is set. */
	ck_assert_uint_eq(default_coding_resolve_target(
				  &rs, LAYOUT4_FLEX_FILES_V2, 20, 10),
			  6);
}
END_TEST

/*
 * Flex files with NO default_coding (legacy / unset): target =
 * ss_layout_width.  Preserves today's behaviour for exports
 * that haven't been migrated to the new per-export config.
 */
START_TEST(test_target_flex_files_unset_uses_layout_width)
{
	struct reffs_coding_spec unset = { 0, 0, 0 };

	ck_assert_uint_eq(default_coding_resolve_target(
				  &unset, LAYOUT4_FLEX_FILES_V2, 20, 10),
			  10);
}
END_TEST

/*
 * Flex files, no default_coding, ss_layout_width = 0:
 * REFFS_LAYOUT_WIDTH_DEFAULT fallback.  Captures the existing
 * "REFFS_LAYOUT_WIDTH_DEFAULT" defaulting that the production
 * code did before refactor.
 */
START_TEST(test_target_flex_files_unset_zero_width_defaults)
{
	struct reffs_coding_spec unset = { 0, 0, 0 };

	uint32_t target = default_coding_resolve_target(
		&unset, LAYOUT4_FLEX_FILES_V2, 20, 0);

	/* REFFS_LAYOUT_WIDTH_DEFAULT is defined in reffs/server.h
	 * (which we don't include here) -- assert it's non-zero
	 * and not 20 (the spurious nds value). */
	ck_assert_uint_gt(target, 0);
	ck_assert_uint_ne(target, 20);
}
END_TEST

/* ------------------------------------------------------------------ */
/* default_coding_resolve_segment                                      */
/* ------------------------------------------------------------------ */

/*
 * Plan-review B2 headline test: explicit RS(4,2), runway popped
 * 6 files (k + m == 6 == target).  ls_k must be 4 from the
 * config, NOT 6 from nfiles.  Today's pre-fix code silently
 * set ls_k = nfiles, corrupting the codec geometry.
 */
START_TEST(test_segment_runway_target_drives_k_m)
{
	struct reffs_coding_spec rs = {
		.cs_codec_type = REFFS_CODEC_RS_VANDERMONDE,
		.cs_k = 4,
		.cs_m = 2,
	};
	uint16_t k = 0, m = 0;
	uint32_t codec = 0;

	int rc = default_coding_resolve_segment(&rs, 6, &k, &m, &codec);

	ck_assert_int_eq(rc, 0);
	ck_assert_uint_eq(k, 4);
	ck_assert_uint_eq(m, 2);
	ck_assert_uint_eq(codec, FFV2_ENCODING_RS_VANDERMONDE);
}
END_TEST

/*
 * Mojette-sys(8,2), runway popped 10 files.  ls_k=8, ls_m=2,
 * codec=MOJETTE_SYSTEMATIC.  Pins the codec-type plumbing for
 * a non-RS spec.
 */
START_TEST(test_segment_mojette_sys_8_2_default)
{
	struct reffs_coding_spec moj = {
		.cs_codec_type = REFFS_CODEC_MOJETTE_SYSTEMATIC,
		.cs_k = 8,
		.cs_m = 2,
	};
	uint16_t k = 0, m = 0;
	uint32_t codec = 0;

	int rc = default_coding_resolve_segment(&moj, 10, &k, &m, &codec);

	ck_assert_int_eq(rc, 0);
	ck_assert_uint_eq(k, 8);
	ck_assert_uint_eq(m, 2);
	ck_assert_uint_eq(codec, FFV2_ENCODING_MOJETTE_SYSTEMATIC);
}
END_TEST

/*
 * Plan-review B2 / test_layoutget_insufficient_dstores:
 * default_coding wants k+m=6 but the runway popped only 5.
 * Resolve returns -EAGAIN, which the LAYOUTGET dispatch maps
 * to NFS4ERR_LAYOUTUNAVAILABLE.  Today's pre-fix code silently
 * accepted ls_k=5, ls_m=0 -- this test catches a regression
 * to that broken behaviour.
 */
START_TEST(test_segment_insufficient_dstores)
{
	struct reffs_coding_spec rs = {
		.cs_codec_type = REFFS_CODEC_RS_VANDERMONDE,
		.cs_k = 4,
		.cs_m = 2,
	};
	uint16_t k = 99, m = 99;
	uint32_t codec = 99;

	int rc = default_coding_resolve_segment(&rs, 5, &k, &m, &codec);

	ck_assert_int_eq(rc, -EAGAIN);
	/* Out params untouched on EAGAIN -- caller must not consume them. */
	ck_assert_uint_eq(k, 99);
	ck_assert_uint_eq(m, 99);
	ck_assert_uint_eq(codec, 99);
}
END_TEST

/*
 * test_layoutget_dstore_count_drops: default_coding set
 * successfully when dstores were plentiful, but at LAYOUTGET
 * time one dstore is offline so the runway-pop short-falls.
 * Same -EAGAIN expectation as the insufficient_dstores case;
 * the distinct test name documents the operational scenario.
 */
START_TEST(test_segment_dstore_count_drops_returns_eagain)
{
	struct reffs_coding_spec rs = {
		.cs_codec_type = REFFS_CODEC_RS_VANDERMONDE,
		.cs_k = 8,
		.cs_m = 2,
	};
	uint16_t k = 0, m = 0;
	uint32_t codec = 0;

	/* k+m = 10, but runway only delivered 8 (two dstores down). */
	int rc = default_coding_resolve_segment(&rs, 8, &k, &m, &codec);

	ck_assert_int_eq(rc, -EAGAIN);
}
END_TEST

/*
 * Unset default_coding: legacy behaviour preserved -- ls_k =
 * nfiles, ls_m = 0, codec = PASSTHROUGH.  This is the path
 * exports without per-export configuration still take.
 */
START_TEST(test_segment_unset_falls_back_to_legacy)
{
	struct reffs_coding_spec unset = { 0, 0, 0 };
	uint16_t k = 0, m = 99;
	uint32_t codec = 99;

	int rc = default_coding_resolve_segment(&unset, 6, &k, &m, &codec);

	ck_assert_int_eq(rc, 0);
	ck_assert_uint_eq(k, 6);
	ck_assert_uint_eq(m, 0);
	ck_assert_uint_eq(codec, FFV2_ENCODING_PASSTHROUGH);
}
END_TEST

/*
 * Explicit PASSTHROUGH spec (k=anything, m=0) -- distinct from
 * the unset case in intent (admin explicitly chose
 * "passthrough" via TOML so cs_codec_type is non-zero).  The
 * spec is therefore NOT formally "unset", but
 * default_coding_resolve_segment must observe the same legacy
 * behaviour: the runway count drives ls_k, ls_m = 0,
 * codec = PASSTHROUGH.  The helper keys off cs_m == 0 (which
 * the setter invariant ties to PASSTHROUGH) so the explicit
 * and unset paths converge cleanly.
 */
START_TEST(test_segment_explicit_passthrough_equals_unset)
{
	struct reffs_coding_spec pt = {
		.cs_codec_type = REFFS_CODEC_PASSTHROUGH,
		.cs_k = 0,
		.cs_m = 0,
	};
	uint16_t k = 0, m = 0;
	uint32_t codec = 0;

	/* Explicit PASSTHROUGH carries a non-zero codec_type, so it
	 * is NOT the all-zero "unset" sentinel even though m == 0. */
	ck_assert(!reffs_coding_spec_is_unset(&pt));

	int rc = default_coding_resolve_segment(&pt, 4, &k, &m, &codec);

	ck_assert_int_eq(rc, 0);
	ck_assert_uint_eq(k, 4);
	ck_assert_uint_eq(m, 0);
	ck_assert_uint_eq(codec, FFV2_ENCODING_PASSTHROUGH);
}
END_TEST

/* ------------------------------------------------------------------ */

static Suite *default_coding_dispatch_suite(void)
{
	Suite *s = suite_create("default_coding_dispatch");

	TCase *tc_target = tcase_create("resolve_target");
	tcase_add_test(tc_target, test_target_file_layout_returns_nds);
	tcase_add_test(tc_target, test_target_flex_files_uses_k_plus_m);
	tcase_add_test(tc_target,
		       test_target_flex_files_unset_uses_layout_width);
	tcase_add_test(tc_target,
		       test_target_flex_files_unset_zero_width_defaults);
	suite_add_tcase(s, tc_target);

	TCase *tc_seg = tcase_create("resolve_segment");
	tcase_add_test(tc_seg, test_segment_runway_target_drives_k_m);
	tcase_add_test(tc_seg, test_segment_mojette_sys_8_2_default);
	tcase_add_test(tc_seg, test_segment_insufficient_dstores);
	tcase_add_test(tc_seg, test_segment_dstore_count_drops_returns_eagain);
	tcase_add_test(tc_seg, test_segment_unset_falls_back_to_legacy);
	tcase_add_test(tc_seg, test_segment_explicit_passthrough_equals_unset);
	suite_add_tcase(s, tc_seg);

	return s;
}

int main(void)
{
	return reffs_test_run_suite(default_coding_dispatch_suite(), NULL,
				    NULL);
}
