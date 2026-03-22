/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Unit tests for Reed-Solomon erasure codec.
 */

#include <check.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "reffs/ec.h"

#define SHARD_LEN 4096

/* Fill a buffer with a deterministic pattern based on shard index. */
static void fill_pattern(uint8_t *buf, size_t len, int shard_idx)
{
	for (size_t i = 0; i < len; i++)
		buf[i] = (uint8_t)((shard_idx * 37 + i * 7 + 13) & 0xff);
}

START_TEST(test_init_valid)
{
	struct ec_codec *c = ec_rs_create(4, 2);

	ck_assert_ptr_nonnull(c);
	ck_assert_int_eq(c->ec_k, 4);
	ck_assert_int_eq(c->ec_m, 2);
	ec_codec_destroy(c);
}
END_TEST

START_TEST(test_init_invalid)
{
	ck_assert_ptr_null(ec_rs_create(0, 2));
	ck_assert_ptr_null(ec_rs_create(4, 0));
	ck_assert_ptr_null(ec_rs_create(200, 200)); /* k+m > 255 */
}
END_TEST

START_TEST(test_encode_decode_no_loss)
{
	int k = 4, m = 2;
	struct ec_codec *c = ec_rs_create(k, m);

	ck_assert_ptr_nonnull(c);

	uint8_t *data[4], *parity[2], *orig[4];

	for (int i = 0; i < k; i++) {
		data[i] = calloc(SHARD_LEN, 1);
		orig[i] = calloc(SHARD_LEN, 1);
		fill_pattern(data[i], SHARD_LEN, i);
		memcpy(orig[i], data[i], SHARD_LEN);
	}
	for (int i = 0; i < m; i++)
		parity[i] = calloc(SHARD_LEN, 1);

	ck_assert_int_eq(c->ec_encode(c, data, parity, SHARD_LEN), 0);

	/* All present — decode should be a no-op. */
	uint8_t *shards[6];

	for (int i = 0; i < k; i++)
		shards[i] = data[i];
	for (int i = 0; i < m; i++)
		shards[k + i] = parity[i];

	bool present[6] = { true, true, true, true, true, true };

	ck_assert_int_eq(c->ec_decode(c, shards, present, SHARD_LEN), 0);

	/* Data unchanged. */
	for (int i = 0; i < k; i++)
		ck_assert_mem_eq(data[i], orig[i], SHARD_LEN);

	for (int i = 0; i < k; i++) {
		free(data[i]);
		free(orig[i]);
	}
	for (int i = 0; i < m; i++)
		free(parity[i]);
	ec_codec_destroy(c);
}
END_TEST

START_TEST(test_one_data_loss)
{
	int k = 4, m = 2;
	struct ec_codec *c = ec_rs_create(k, m);

	ck_assert_ptr_nonnull(c);

	uint8_t *data[4], *parity[2], *orig[4];

	for (int i = 0; i < k; i++) {
		data[i] = calloc(SHARD_LEN, 1);
		orig[i] = calloc(SHARD_LEN, 1);
		fill_pattern(data[i], SHARD_LEN, i);
		memcpy(orig[i], data[i], SHARD_LEN);
	}
	for (int i = 0; i < m; i++)
		parity[i] = calloc(SHARD_LEN, 1);

	ck_assert_int_eq(c->ec_encode(c, data, parity, SHARD_LEN), 0);

	/* Erase shard 1. */
	uint8_t *shards[6];

	for (int i = 0; i < k; i++)
		shards[i] = data[i];
	for (int i = 0; i < m; i++)
		shards[k + i] = parity[i];

	memset(shards[1], 0, SHARD_LEN);
	bool present[6] = { true, false, true, true, true, true };

	ck_assert_int_eq(c->ec_decode(c, shards, present, SHARD_LEN), 0);
	ck_assert_mem_eq(shards[1], orig[1], SHARD_LEN);

	for (int i = 0; i < k; i++) {
		free(data[i]);
		free(orig[i]);
	}
	for (int i = 0; i < m; i++)
		free(parity[i]);
	ec_codec_destroy(c);
}
END_TEST

START_TEST(test_one_parity_loss)
{
	int k = 4, m = 2;
	struct ec_codec *c = ec_rs_create(k, m);

	ck_assert_ptr_nonnull(c);

	uint8_t *data[4], *parity[2], *orig_parity[2];

	for (int i = 0; i < k; i++) {
		data[i] = calloc(SHARD_LEN, 1);
		fill_pattern(data[i], SHARD_LEN, i);
	}
	for (int i = 0; i < m; i++) {
		parity[i] = calloc(SHARD_LEN, 1);
		orig_parity[i] = calloc(SHARD_LEN, 1);
	}

	ck_assert_int_eq(c->ec_encode(c, data, parity, SHARD_LEN), 0);
	for (int i = 0; i < m; i++)
		memcpy(orig_parity[i], parity[i], SHARD_LEN);

	/* Erase parity shard 0. */
	uint8_t *shards[6];

	for (int i = 0; i < k; i++)
		shards[i] = data[i];
	for (int i = 0; i < m; i++)
		shards[k + i] = parity[i];

	memset(shards[4], 0, SHARD_LEN);
	bool present[6] = { true, true, true, true, false, true };

	ck_assert_int_eq(c->ec_decode(c, shards, present, SHARD_LEN), 0);
	ck_assert_mem_eq(shards[4], orig_parity[0], SHARD_LEN);

	for (int i = 0; i < k; i++)
		free(data[i]);
	for (int i = 0; i < m; i++) {
		free(parity[i]);
		free(orig_parity[i]);
	}
	ec_codec_destroy(c);
}
END_TEST

START_TEST(test_mixed_loss)
{
	int k = 4, m = 2;
	struct ec_codec *c = ec_rs_create(k, m);

	ck_assert_ptr_nonnull(c);

	uint8_t *data[4], *parity[2], *orig[4], *orig_parity[2];

	for (int i = 0; i < k; i++) {
		data[i] = calloc(SHARD_LEN, 1);
		orig[i] = calloc(SHARD_LEN, 1);
		fill_pattern(data[i], SHARD_LEN, i);
		memcpy(orig[i], data[i], SHARD_LEN);
	}
	for (int i = 0; i < m; i++) {
		parity[i] = calloc(SHARD_LEN, 1);
		orig_parity[i] = calloc(SHARD_LEN, 1);
	}

	ck_assert_int_eq(c->ec_encode(c, data, parity, SHARD_LEN), 0);
	for (int i = 0; i < m; i++)
		memcpy(orig_parity[i], parity[i], SHARD_LEN);

	/* Erase data shard 0 + parity shard 1 (m=2, within budget). */
	uint8_t *shards[6];

	for (int i = 0; i < k; i++)
		shards[i] = data[i];
	for (int i = 0; i < m; i++)
		shards[k + i] = parity[i];

	memset(shards[0], 0, SHARD_LEN);
	memset(shards[5], 0, SHARD_LEN);
	bool present[6] = { false, true, true, true, true, false };

	ck_assert_int_eq(c->ec_decode(c, shards, present, SHARD_LEN), 0);
	ck_assert_mem_eq(shards[0], orig[0], SHARD_LEN);
	ck_assert_mem_eq(shards[5], orig_parity[1], SHARD_LEN);

	for (int i = 0; i < k; i++) {
		free(data[i]);
		free(orig[i]);
	}
	for (int i = 0; i < m; i++) {
		free(parity[i]);
		free(orig_parity[i]);
	}
	ec_codec_destroy(c);
}
END_TEST

START_TEST(test_too_many_losses)
{
	int k = 4, m = 2;
	struct ec_codec *c = ec_rs_create(k, m);

	ck_assert_ptr_nonnull(c);

	uint8_t *shards[6];

	for (int i = 0; i < 6; i++)
		shards[i] = calloc(SHARD_LEN, 1);

	/* 3 missing > m=2 */
	bool present[6] = { false, false, false, true, true, true };

	ck_assert_int_eq(c->ec_decode(c, shards, present, SHARD_LEN), -EIO);

	for (int i = 0; i < 6; i++)
		free(shards[i]);
	ec_codec_destroy(c);
}
END_TEST

START_TEST(test_small_shard)
{
	int k = 3, m = 1;
	struct ec_codec *c = ec_rs_create(k, m);

	ck_assert_ptr_nonnull(c);

	uint8_t *data[3], *parity[1], *orig[3];

	for (int i = 0; i < k; i++) {
		data[i] = calloc(1, 1);
		orig[i] = calloc(1, 1);
		data[i][0] = (uint8_t)(i + 10);
		orig[i][0] = data[i][0];
	}
	parity[0] = calloc(1, 1);

	ck_assert_int_eq(c->ec_encode(c, data, parity, 1), 0);

	uint8_t *shards[4] = { data[0], data[1], data[2], parity[0] };

	memset(shards[2], 0, 1);
	bool present[4] = { true, true, false, true };

	ck_assert_int_eq(c->ec_decode(c, shards, present, 1), 0);
	ck_assert_uint_eq(shards[2][0], orig[2][0]);

	for (int i = 0; i < k; i++) {
		free(data[i]);
		free(orig[i]);
	}
	free(parity[0]);
	ec_codec_destroy(c);
}
END_TEST

START_TEST(test_large_k)
{
	int k = 16, m = 4;
	int n = k + m;
	struct ec_codec *c = ec_rs_create(k, m);
	size_t len = 1024;

	ck_assert_ptr_nonnull(c);

	uint8_t *data[16], *parity[4], *orig[16];

	for (int i = 0; i < k; i++) {
		data[i] = calloc(len, 1);
		orig[i] = calloc(len, 1);
		fill_pattern(data[i], len, i);
		memcpy(orig[i], data[i], len);
	}
	for (int i = 0; i < m; i++)
		parity[i] = calloc(len, 1);

	ck_assert_int_eq(c->ec_encode(c, data, parity, len), 0);

	/* Erase 4 shards (max for m=4): data[0], data[5], data[10], parity[2]. */
	uint8_t *shards[20];

	for (int i = 0; i < k; i++)
		shards[i] = data[i];
	for (int i = 0; i < m; i++)
		shards[k + i] = parity[i];

	bool present[20];

	for (int i = 0; i < n; i++)
		present[i] = true;
	present[0] = false;
	memset(shards[0], 0, len);
	present[5] = false;
	memset(shards[5], 0, len);
	present[10] = false;
	memset(shards[10], 0, len);
	present[18] = false;
	memset(shards[18], 0, len);

	ck_assert_int_eq(c->ec_decode(c, shards, present, len), 0);
	ck_assert_mem_eq(shards[0], orig[0], len);
	ck_assert_mem_eq(shards[5], orig[5], len);
	ck_assert_mem_eq(shards[10], orig[10], len);

	for (int i = 0; i < k; i++) {
		free(data[i]);
		free(orig[i]);
	}
	for (int i = 0; i < m; i++)
		free(parity[i]);
	ec_codec_destroy(c);
}
END_TEST

Suite *rs_suite(void)
{
	Suite *s = suite_create("Reed-Solomon Codec");

	TCase *tc = tcase_create("rs");
	tcase_add_test(tc, test_init_valid);
	tcase_add_test(tc, test_init_invalid);
	tcase_add_test(tc, test_encode_decode_no_loss);
	tcase_add_test(tc, test_one_data_loss);
	tcase_add_test(tc, test_one_parity_loss);
	tcase_add_test(tc, test_mixed_loss);
	tcase_add_test(tc, test_too_many_losses);
	tcase_add_test(tc, test_small_shard);
	tcase_add_test(tc, test_large_k);
	suite_add_tcase(s, tc);

	return s;
}

int main(void)
{
	SRunner *sr = srunner_create(rs_suite());

	srunner_run_all(sr, CK_NORMAL);
	int nf = srunner_ntests_failed(sr);

	srunner_free(sr);
	return nf ? EXIT_FAILURE : EXIT_SUCCESS;
}
