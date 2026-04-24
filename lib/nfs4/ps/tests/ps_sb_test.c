/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <check.h>
#include <stdint.h>
#include <string.h>

#include "ps_sb.h"

/*
 * Happy-path round-trip: alloc with a representative FH, confirm
 * every field was copied verbatim, and that mutating the caller's
 * buffer does not disturb the stored copy.  The binding owns its
 * FH bytes -- it must not alias the caller's memory.
 */
START_TEST(test_binding_alloc_and_read)
{
	uint8_t fh[] = { 0x0a, 0x0b, 0x0c, 0x0d, 0x0e };

	struct ps_sb_binding *b = ps_sb_binding_alloc(42, fh, sizeof(fh));

	ck_assert_ptr_nonnull(b);
	ck_assert_uint_eq(b->psb_listener_id, 42);
	ck_assert_uint_eq(b->psb_mds_fh_len, sizeof(fh));
	ck_assert_mem_eq(b->psb_mds_fh, fh, sizeof(fh));

	/* Caller-owned FH is decoupled from the stored copy. */
	fh[0] = 0xFF;
	ck_assert_uint_eq(b->psb_mds_fh[0], 0x0a);

	ps_sb_binding_free(b);
}
END_TEST

/*
 * Argument rejection.  listener_id 0 is reserved for the native
 * listener (ps_state_register enforces the same) and must never
 * mint a binding.  fh / fh_len 0 would leave the binding with no
 * usable anchor, and fh_len above PS_MAX_FH_SIZE would silently
 * truncate into the embedded array.  Every bad-arg combo must
 * return NULL rather than leak a half-initialised binding.
 */
START_TEST(test_binding_alloc_rejects_bad_args)
{
	uint8_t fh[] = { 0x01 };
	uint8_t big[PS_MAX_FH_SIZE + 1];

	memset(big, 0xAB, sizeof(big));

	ck_assert_ptr_null(ps_sb_binding_alloc(0, fh, sizeof(fh)));
	ck_assert_ptr_null(ps_sb_binding_alloc(1, NULL, sizeof(fh)));
	ck_assert_ptr_null(ps_sb_binding_alloc(1, fh, 0));
	ck_assert_ptr_null(ps_sb_binding_alloc(1, big, sizeof(big)));
}
END_TEST

/*
 * Free is NULL-tolerant.  Matches ps_mount_free_exports,
 * super_block_put, etc.; callers that bail on an error path
 * should not need a guard.
 */
START_TEST(test_binding_free_null)
{
	ps_sb_binding_free(NULL);
}
END_TEST

/*
 * Maximum-sized FH round-trips with no truncation or overrun.
 * 128 bytes is the NFSv4 wire cap (RFC 8881) and is the upper
 * bound the PS will ever see; test it explicitly so a future
 * accidental bound check off-by-one can't silently shrink the
 * supported FH size.
 */
START_TEST(test_binding_max_fh_size)
{
	uint8_t fh[PS_MAX_FH_SIZE];

	for (size_t i = 0; i < sizeof(fh); i++)
		fh[i] = (uint8_t)(i & 0xFF);

	struct ps_sb_binding *b = ps_sb_binding_alloc(7, fh, sizeof(fh));

	ck_assert_ptr_nonnull(b);
	ck_assert_uint_eq(b->psb_mds_fh_len, sizeof(fh));
	ck_assert_mem_eq(b->psb_mds_fh, fh, sizeof(fh));

	ps_sb_binding_free(b);
}
END_TEST

static Suite *ps_sb_suite(void)
{
	Suite *s = suite_create("ps_sb");
	TCase *tc = tcase_create("core");

	tcase_add_test(tc, test_binding_alloc_and_read);
	tcase_add_test(tc, test_binding_alloc_rejects_bad_args);
	tcase_add_test(tc, test_binding_free_null);
	tcase_add_test(tc, test_binding_max_fh_size);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	SRunner *sr = srunner_create(ps_sb_suite());

	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return failed == 0 ? 0 : 1;
}
