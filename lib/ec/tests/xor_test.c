/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Unit tests for XOR single-parity encoding (lib/ec/xor.c).
 *
 * Mirrors the case-coverage style of rs_test.c and snapraid_test.c
 * so the three encodings can be compared directly.
 */

#include <check.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "reffs/ec.h"

#define SHARD_LEN 4096

static void fill_pattern(uint8_t *buf, size_t len, int shard_idx)
{
	for (size_t i = 0; i < len; i++)
		buf[i] = (uint8_t)((shard_idx * 37 + i * 7 + 13) & 0xff);
}

START_TEST(test_init_valid)
{
	struct ec_encoding *c = ec_xor_create(4);

	ck_assert_ptr_nonnull(c);
	ck_assert_int_eq(c->ec_k, 4);
	ck_assert_int_eq(c->ec_m, 1);
	ck_assert_str_eq(c->ec_name, "xor-parity");
	ec_encoding_destroy(c);
}
END_TEST

START_TEST(test_init_invalid)
{
	ck_assert_ptr_null(ec_xor_create(0));
	ck_assert_ptr_null(ec_xor_create(255));
	ck_assert_ptr_null(ec_xor_create(-1));
}
END_TEST

/* Round-trip helper: encode k data shards, drop 'nmiss' shards
 * (indexes in 'miss[]'), decode, check every original shard
 * recovers byte-for-byte. */
static void roundtrip(int k, int nmiss, const int *miss)
{
	struct ec_encoding *c = ec_xor_create(k);

	ck_assert_ptr_nonnull(c);

	int n = k + 1;
	uint8_t **orig = calloc(n, sizeof(*orig));
	uint8_t **shards = calloc(n, sizeof(*shards));
	uint8_t **data = calloc(k, sizeof(*data));
	uint8_t *parity[1];
	bool *present = calloc(n, sizeof(*present));

	for (int i = 0; i < n; i++) {
		orig[i] = calloc(SHARD_LEN, 1);
		shards[i] = calloc(SHARD_LEN, 1);
		present[i] = true;
	}
	for (int i = 0; i < k; i++) {
		fill_pattern(orig[i], SHARD_LEN, i);
		memcpy(shards[i], orig[i], SHARD_LEN);
		data[i] = shards[i];
	}
	parity[0] = shards[k];

	ck_assert_int_eq(c->ec_encode(c, data, parity, SHARD_LEN), 0);
	memcpy(orig[k], shards[k], SHARD_LEN); /* snapshot parity */

	for (int j = 0; j < nmiss; j++) {
		memset(shards[miss[j]], 0, SHARD_LEN);
		present[miss[j]] = false;
	}

	ck_assert_int_eq(c->ec_decode(c, shards, present, SHARD_LEN), 0);

	for (int i = 0; i < n; i++)
		ck_assert_mem_eq(shards[i], orig[i], SHARD_LEN);

	for (int i = 0; i < n; i++) {
		free(orig[i]);
		free(shards[i]);
	}
	free(orig);
	free(shards);
	free(data);
	free(present);
	ec_encoding_destroy(c);
}

START_TEST(test_roundtrip_no_loss)
{
	int miss[] = { 0 };

	roundtrip(4, 0, miss);
}
END_TEST

START_TEST(test_roundtrip_first_data_missing)
{
	int miss[] = { 0 };

	roundtrip(4, 1, miss);
}
END_TEST

START_TEST(test_roundtrip_middle_data_missing)
{
	int miss[] = { 2 };

	roundtrip(4, 1, miss);
}
END_TEST

START_TEST(test_roundtrip_parity_missing)
{
	int miss[] = { 4 };

	roundtrip(4, 1, miss);
}
END_TEST

START_TEST(test_decode_too_many_missing)
{
	int k = 4;
	struct ec_encoding *c = ec_xor_create(k);

	ck_assert_ptr_nonnull(c);

	int n = k + 1;
	uint8_t *shards[5];
	bool present[5];

	for (int i = 0; i < n; i++) {
		shards[i] = calloc(SHARD_LEN, 1);
		present[i] = true;
	}
	/* Drop 2 shards but only 1 parity block exists. */
	present[0] = false;
	present[1] = false;

	ck_assert_int_eq(c->ec_decode(c, shards, present, SHARD_LEN), -EIO);

	for (int i = 0; i < n; i++)
		free(shards[i]);
	ec_encoding_destroy(c);
}
END_TEST

START_TEST(test_k1_trivial)
{
	/* k=1 is degenerate: parity is a copy of data[0].  Test that
	 * encode + decode still work for this corner case. */
	int miss[] = { 0 };

	roundtrip(1, 0, miss);
	roundtrip(1, 1, miss);
	miss[0] = 1;
	roundtrip(1, 1, miss);
}
END_TEST

START_TEST(test_k254_max)
{
	/* Widest shape we accept; catches off-by-one at the cap. */
	int miss[] = { 128 };

	roundtrip(254, 1, miss);
}
END_TEST

START_TEST(test_encode_zero_len)
{
	/* Zero-length shard is a valid no-op. */
	int k = 4;
	struct ec_encoding *c = ec_xor_create(k);

	ck_assert_ptr_nonnull(c);

	uint8_t *data[4] = { NULL, NULL, NULL, NULL };
	uint8_t *parity[1] = { NULL };

	ck_assert_int_eq(c->ec_encode(c, data, parity, 0), 0);
	ec_encoding_destroy(c);
}
END_TEST

Suite *xor_suite(void)
{
	Suite *s = suite_create("xor");
	TCase *tc = tcase_create("basic");

	tcase_add_test(tc, test_init_valid);
	tcase_add_test(tc, test_init_invalid);
	tcase_add_test(tc, test_roundtrip_no_loss);
	tcase_add_test(tc, test_roundtrip_first_data_missing);
	tcase_add_test(tc, test_roundtrip_middle_data_missing);
	tcase_add_test(tc, test_roundtrip_parity_missing);
	tcase_add_test(tc, test_decode_too_many_missing);
	tcase_add_test(tc, test_k1_trivial);
	tcase_add_test(tc, test_k254_max);
	tcase_add_test(tc, test_encode_zero_len);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	Suite *s = xor_suite();
	SRunner *sr = srunner_create(s);

	srunner_run_all(sr, CK_NORMAL);
	int failed = srunner_ntests_failed(sr);

	srunner_free(sr);
	return failed == 0 ? 0 : 1;
}
