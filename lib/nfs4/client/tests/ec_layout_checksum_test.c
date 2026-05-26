/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" /* IWYU pragma: keep */
#endif

/*
 * Unit tests for ec_layout_validate_checksums -- Pending Change 6
 * step 7 client-side supported-set check.
 *
 * The decode-path call site in mds_layout_get needs a live MDS
 * session to exercise end-to-end; these tests cover the policy
 * function the call site delegates to, against hand-built layouts
 * that simulate what the decoder would have produced.
 */

#include <check.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "nfsv42_xdr.h"
#include "ec_client.h"

/* Build a 2-mirror layout with the given algorithms.  Caller frees. */
static struct ec_layout *make_layout(uint32_t alg_a, uint32_t alg_b)
{
	struct ec_layout *layout = calloc(1, sizeof(*layout));

	if (!layout)
		return NULL;
	layout->el_nmirrors = 2;
	layout->el_mirrors = calloc(2, sizeof(struct ec_mirror));
	if (!layout->el_mirrors) {
		free(layout);
		return NULL;
	}
	layout->el_mirrors[0].em_checksum_algorithm = alg_a;
	layout->el_mirrors[1].em_checksum_algorithm = alg_b;
	return layout;
}

static void free_layout(struct ec_layout *layout)
{
	if (!layout)
		return;
	free(layout->el_mirrors);
	free(layout);
}

START_TEST(test_validate_all_crc32)
{
	struct ec_layout *layout =
		make_layout(CHECKSUM_ALG_CRC32, CHECKSUM_ALG_CRC32);

	ck_assert_ptr_nonnull(layout);
	uint32_t bad = 0xFF;

	ck_assert_int_eq(ec_layout_validate_checksums(layout, &bad), 0);
	/* bad_mirror_out is only written on failure. */
	ck_assert_uint_eq(bad, 0xFF);
	free_layout(layout);
}
END_TEST

START_TEST(test_validate_all_none)
{
	/*
	 * CHECKSUM_ALG_NONE means "server has no policy".  The client
	 * is happy to proceed -- it just won't compute or verify a
	 * checksum.  This is a deliberate hedge against the case where
	 * an SB was provisioned before sb-set-checksum-algorithm ran.
	 */
	struct ec_layout *layout =
		make_layout(CHECKSUM_ALG_NONE, CHECKSUM_ALG_NONE);

	ck_assert_ptr_nonnull(layout);
	ck_assert_int_eq(ec_layout_validate_checksums(layout, NULL), 0);
	free_layout(layout);
}
END_TEST

START_TEST(test_validate_first_mirror_unsupported)
{
	struct ec_layout *layout =
		make_layout(CHECKSUM_ALG_SHA256, CHECKSUM_ALG_CRC32);

	ck_assert_ptr_nonnull(layout);
	uint32_t bad = 0xFF;

	ck_assert_int_eq(ec_layout_validate_checksums(layout, &bad), -ENOTSUP);
	ck_assert_uint_eq(bad, 0);
	free_layout(layout);
}
END_TEST

START_TEST(test_validate_second_mirror_unsupported)
{
	struct ec_layout *layout =
		make_layout(CHECKSUM_ALG_CRC32, CHECKSUM_ALG_BLAKE3);

	ck_assert_ptr_nonnull(layout);
	uint32_t bad = 0xFF;

	ck_assert_int_eq(ec_layout_validate_checksums(layout, &bad), -ENOTSUP);
	ck_assert_uint_eq(bad, 1);
	free_layout(layout);
}
END_TEST

START_TEST(test_validate_all_algorithms)
{
	/*
	 * Pin the exact set the client accepts.  When a new
	 * dispatcher lands (e.g., CRC32C), this test will fail and
	 * force the supported-set table in mds_layout.c to be
	 * updated in lock-step.
	 */
	const uint32_t expect_ok[] = { CHECKSUM_ALG_NONE, CHECKSUM_ALG_CRC32 };
	const uint32_t expect_unsupp[] = {
		CHECKSUM_ALG_CRC32C, CHECKSUM_ALG_FLETCHER4,
		CHECKSUM_ALG_SHA256, CHECKSUM_ALG_SHA512, CHECKSUM_ALG_BLAKE3
	};

	for (size_t i = 0; i < sizeof(expect_ok) / sizeof(expect_ok[0]); i++) {
		struct ec_layout *layout =
			make_layout(expect_ok[i], expect_ok[i]);

		ck_assert_int_eq(ec_layout_validate_checksums(layout, NULL), 0);
		free_layout(layout);
	}

	for (size_t i = 0; i < sizeof(expect_unsupp) / sizeof(expect_unsupp[0]);
	     i++) {
		struct ec_layout *layout =
			make_layout(expect_unsupp[i], CHECKSUM_ALG_CRC32);

		ck_assert_int_eq(ec_layout_validate_checksums(layout, NULL),
				 -ENOTSUP);
		free_layout(layout);
	}
}
END_TEST

START_TEST(test_validate_null_layout)
{
	/* Defensive: NULL layout and zero-mirror layout are both no-ops. */
	ck_assert_int_eq(ec_layout_validate_checksums(NULL, NULL), 0);

	struct ec_layout empty = { 0 };

	ck_assert_int_eq(ec_layout_validate_checksums(&empty, NULL), 0);
}
END_TEST

static Suite *ec_layout_checksum_suite(void)
{
	Suite *s = suite_create("ec_layout_validate_checksums");
	TCase *tc = tcase_create("validate");

	tcase_add_test(tc, test_validate_all_crc32);
	tcase_add_test(tc, test_validate_all_none);
	tcase_add_test(tc, test_validate_first_mirror_unsupported);
	tcase_add_test(tc, test_validate_second_mirror_unsupported);
	tcase_add_test(tc, test_validate_all_algorithms);
	tcase_add_test(tc, test_validate_null_layout);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	SRunner *sr = srunner_create(ec_layout_checksum_suite());

	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
