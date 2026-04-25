/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * Unit coverage for ps_owner_wrap -- prefixing the end-client's
 * clientid4 (8 BE bytes) onto the raw open-owner so two end clients
 * on the same PS session do not collide on the upstream MDS's
 * stateowner table.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <check.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "ps_owner.h"

#define BIG_CLIENTID UINT64_C(0x0102030405060708)

START_TEST(test_wrapped_size_is_tag_plus_raw)
{
	ck_assert_uint_eq(ps_owner_wrapped_size(0), PS_OWNER_TAG_SIZE);
	ck_assert_uint_eq(ps_owner_wrapped_size(16), PS_OWNER_TAG_SIZE + 16);
	ck_assert_uint_eq(ps_owner_wrapped_size(1024),
			  PS_OWNER_TAG_SIZE + 1024);
}
END_TEST

/*
 * Happy path: clientid is laid down big-endian in the first 8 bytes,
 * raw owner follows verbatim.  Pinned to a known clientid value so a
 * future endianness regression is caught against literal bytes.
 */
START_TEST(test_wrap_lays_down_be_clientid_then_raw)
{
	const uint8_t raw[] = "alice";
	uint8_t out[64];
	uint32_t out_len = 0;

	int r = ps_owner_wrap(BIG_CLIENTID, raw, sizeof(raw) - 1, out,
			      sizeof(out), &out_len);
	ck_assert_int_eq(r, 0);
	ck_assert_uint_eq(out_len, PS_OWNER_TAG_SIZE + (sizeof(raw) - 1));

	const uint8_t want_tag[PS_OWNER_TAG_SIZE] = {
		0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
	};
	ck_assert_mem_eq(out, want_tag, PS_OWNER_TAG_SIZE);
	ck_assert_mem_eq(out + PS_OWNER_TAG_SIZE, raw, sizeof(raw) - 1);
}
END_TEST

/*
 * The whole point of the wrap: same raw owner with two different
 * clientids must produce two different wrapped buffers.  Without
 * this guarantee, two end clients on the same PS session that both
 * use owner string "openowner" (NFSv4.0 Linux client default) would
 * collide on the MDS's stateowner table.
 */
START_TEST(test_different_clientids_disambiguate_same_raw)
{
	const uint8_t raw[] = "openowner";
	uint8_t out_a[32], out_b[32];
	uint32_t a_len = 0, b_len = 0;

	ck_assert_int_eq(ps_owner_wrap(0xAAAAAAAAAAAAAAAA, raw, sizeof(raw) - 1,
				       out_a, sizeof(out_a), &a_len),
			 0);
	ck_assert_int_eq(ps_owner_wrap(0xBBBBBBBBBBBBBBBB, raw, sizeof(raw) - 1,
				       out_b, sizeof(out_b), &b_len),
			 0);

	ck_assert_uint_eq(a_len, b_len);
	/* Tags differ, raw tail matches -- that's the whole property. */
	ck_assert(memcmp(out_a, out_b, PS_OWNER_TAG_SIZE) != 0);
	ck_assert_mem_eq(out_a + PS_OWNER_TAG_SIZE, out_b + PS_OWNER_TAG_SIZE,
			 sizeof(raw) - 1);
}
END_TEST

/*
 * Empty raw owner: wrapped form is just the 8-byte tag.  Useful for
 * AUTH_NONE / unauth flows where the end client may send a zero-len
 * owner (rare but allowed by the wire encoding).
 */
START_TEST(test_zero_length_raw_owner_is_just_tag)
{
	uint8_t out[16];
	uint32_t out_len = 0;

	ck_assert_int_eq(ps_owner_wrap(BIG_CLIENTID, NULL, 0, out, sizeof(out),
				       &out_len),
			 0);
	ck_assert_uint_eq(out_len, PS_OWNER_TAG_SIZE);

	const uint8_t want_tag[PS_OWNER_TAG_SIZE] = {
		0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
	};
	ck_assert_mem_eq(out, want_tag, PS_OWNER_TAG_SIZE);
}
END_TEST

/*
 * Output buffer shorter than ps_owner_wrapped_size(raw_len) must
 * return -ENOSPC without writing past the boundary.  Also confirms
 * the "buffer exactly the right size" boundary case succeeds.
 */
START_TEST(test_short_output_buffer_is_enospc)
{
	const uint8_t raw[] = "alice";
	uint8_t too_small[PS_OWNER_TAG_SIZE + 4];
	uint8_t exact[PS_OWNER_TAG_SIZE + 5];
	uint32_t out_len = 99;

	int r = ps_owner_wrap(BIG_CLIENTID, raw, sizeof(raw) - 1, too_small,
			      sizeof(too_small), &out_len);
	ck_assert_int_eq(r, -ENOSPC);
	ck_assert_uint_eq(out_len, 99); /* untouched */

	out_len = 0;
	r = ps_owner_wrap(BIG_CLIENTID, raw, sizeof(raw) - 1, exact,
			  sizeof(exact), &out_len);
	ck_assert_int_eq(r, 0);
	ck_assert_uint_eq(out_len, sizeof(exact));
}
END_TEST

/*
 * NULL out / out_len_out, and NULL raw with a non-zero raw_len, are
 * -EINVAL.  Confirms the helper guards before any deref.
 */
START_TEST(test_bad_args_are_einval)
{
	uint8_t out[16];
	uint32_t out_len = 0;
	const uint8_t raw[] = "x";

	ck_assert_int_eq(ps_owner_wrap(BIG_CLIENTID, raw, 1, NULL, sizeof(out),
				       &out_len),
			 -EINVAL);
	ck_assert_int_eq(ps_owner_wrap(BIG_CLIENTID, raw, 1, out, sizeof(out),
				       NULL),
			 -EINVAL);
	ck_assert_int_eq(ps_owner_wrap(BIG_CLIENTID, NULL, 1, out, sizeof(out),
				       &out_len),
			 -EINVAL);
}
END_TEST

static Suite *ps_owner_suite(void)
{
	Suite *s = suite_create("ps_owner");
	TCase *tc = tcase_create("wrap");

	tcase_add_test(tc, test_wrapped_size_is_tag_plus_raw);
	tcase_add_test(tc, test_wrap_lays_down_be_clientid_then_raw);
	tcase_add_test(tc, test_different_clientids_disambiguate_same_raw);
	tcase_add_test(tc, test_zero_length_raw_owner_is_just_tag);
	tcase_add_test(tc, test_short_output_buffer_is_enospc);
	tcase_add_test(tc, test_bad_args_are_einval);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	SRunner *sr = srunner_create(ps_owner_suite());

	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return failed == 0 ? 0 : 1;
}
