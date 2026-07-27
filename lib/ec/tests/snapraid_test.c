/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Unit tests for the SnapRAID Cauchy encoding wrapper
 * (lib/ec/snapraid.c) over Andrea Mazzoleni's vendored raid/
 * library.  Mirrors the case coverage of rs_test.c so a reader can
 * compare the two encoding types directly.
 */

#include <check.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "reffs/ec.h"

#define SHARD_LEN 4096 /* multiple of 64 -- raid_gen() requires */

static void fill_pattern(uint8_t *buf, size_t len, int shard_idx)
{
	for (size_t i = 0; i < len; i++)
		buf[i] = (uint8_t)((shard_idx * 37 + i * 7 + 13) & 0xff);
}

START_TEST(test_init_valid)
{
	struct ec_encoding *c = ec_snapraid_create(4, 2);

	ck_assert_ptr_nonnull(c);
	ck_assert_int_eq(c->ec_k, 4);
	ck_assert_int_eq(c->ec_m, 2);
	ck_assert_str_eq(c->ec_name, "snapraid-cauchy");
	ec_encoding_destroy(c);
}
END_TEST

START_TEST(test_init_invalid)
{
	ck_assert_ptr_null(ec_snapraid_create(0, 2));
	ck_assert_ptr_null(ec_snapraid_create(4, 0));
	ck_assert_ptr_null(ec_snapraid_create(252, 2)); /* k > 251 */
	ck_assert_ptr_null(ec_snapraid_create(4, 7)); /* m > 6 */
}
END_TEST

/* Round-trip helper: encode k data shards, drop 'nmiss' shards
 * (indexes in 'miss[]'), decode, check every original shard
 * recovers byte-for-byte. */
static void roundtrip(int k, int m, int nmiss, const int *miss)
{
	struct ec_encoding *c = ec_snapraid_create(k, m);

	ck_assert_ptr_nonnull(c);

	int n = k + m;
	uint8_t **orig = calloc(n, sizeof(*orig));
	uint8_t **shards = calloc(n, sizeof(*shards));
	uint8_t **data = calloc(k, sizeof(*data));
	uint8_t **parity = calloc(m, sizeof(*parity));
	bool *present = calloc(n, sizeof(*present));

	ck_assert_ptr_nonnull(orig);
	ck_assert_ptr_nonnull(shards);
	ck_assert_ptr_nonnull(data);
	ck_assert_ptr_nonnull(parity);
	ck_assert_ptr_nonnull(present);

	for (int i = 0; i < n; i++) {
		orig[i] = calloc(SHARD_LEN, 1);
		shards[i] = calloc(SHARD_LEN, 1);
		ck_assert_ptr_nonnull(orig[i]);
		ck_assert_ptr_nonnull(shards[i]);
		present[i] = true;
	}
	for (int i = 0; i < k; i++) {
		fill_pattern(orig[i], SHARD_LEN, i);
		memcpy(shards[i], orig[i], SHARD_LEN);
		data[i] = shards[i];
	}
	for (int i = 0; i < m; i++)
		parity[i] = shards[k + i];

	ck_assert_int_eq(c->ec_encode(c, data, parity, SHARD_LEN), 0);

	/* Snapshot parities as ground truth so we can check parity
	 * reconstruction too, not just data reconstruction. */
	for (int i = 0; i < m; i++)
		memcpy(orig[k + i], shards[k + i], SHARD_LEN);

	/* Zero the missing shards and mark them absent. */
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
	free(parity);
	free(present);
	ec_encoding_destroy(c);
}

START_TEST(test_roundtrip_no_loss)
{
	int miss[] = { 0 };

	roundtrip(4, 2, 0, miss);
}
END_TEST

START_TEST(test_roundtrip_one_data_missing)
{
	int miss[] = { 1 };

	roundtrip(4, 2, 1, miss);
}
END_TEST

START_TEST(test_roundtrip_two_data_missing)
{
	int miss[] = { 0, 3 };

	roundtrip(4, 2, 2, miss);
}
END_TEST

START_TEST(test_roundtrip_data_plus_parity)
{
	int miss[] = { 2, 5 }; /* one data + one parity */

	roundtrip(4, 2, 2, miss);
}
END_TEST

START_TEST(test_roundtrip_parity_only)
{
	int miss[] = { 4, 5 }; /* both parities gone */

	roundtrip(4, 2, 2, miss);
}
END_TEST

START_TEST(test_decode_too_many_missing)
{
	int k = 4, m = 2, n = k + m;
	struct ec_encoding *c = ec_snapraid_create(k, m);

	ck_assert_ptr_nonnull(c);

	uint8_t *shards[6];
	bool present[6];

	for (int i = 0; i < n; i++) {
		shards[i] = calloc(SHARD_LEN, 1);
		present[i] = true;
	}
	/* Drop 3 shards but only 2 parity blocks exist. */
	present[0] = false;
	present[1] = false;
	present[2] = false;

	ck_assert_int_eq(c->ec_decode(c, shards, present, SHARD_LEN), -EIO);

	for (int i = 0; i < n; i++)
		free(shards[i]);
	ec_encoding_destroy(c);
}
END_TEST

START_TEST(test_encode_unaligned_shard_len)
{
	int k = 2, m = 1;
	struct ec_encoding *c = ec_snapraid_create(k, m);

	ck_assert_ptr_nonnull(c);

	uint8_t *data[2] = { calloc(128, 1), calloc(128, 1) };
	uint8_t *parity[1] = { calloc(128, 1) };

	/* raid_gen() requires size % 64 == 0; 65 is not. */
	ck_assert_int_eq(c->ec_encode(c, data, parity, 65), -EINVAL);

	free(data[0]);
	free(data[1]);
	free(parity[0]);
	ec_encoding_destroy(c);
}
END_TEST

START_TEST(test_roundtrip_larger_geometry)
{
	/* 6 data + 3 parity, drop two data + one parity -- verifies
	 * the Cauchy matrix's MDS property for higher-parity levels. */
	int miss[] = { 0, 4, 7 };

	roundtrip(6, 3, 3, miss);
}
END_TEST

Suite *snapraid_suite(void)
{
	Suite *s = suite_create("snapraid");
	TCase *tc = tcase_create("basic");

	tcase_add_test(tc, test_init_valid);
	tcase_add_test(tc, test_init_invalid);
	tcase_add_test(tc, test_roundtrip_no_loss);
	tcase_add_test(tc, test_roundtrip_one_data_missing);
	tcase_add_test(tc, test_roundtrip_two_data_missing);
	tcase_add_test(tc, test_roundtrip_data_plus_parity);
	tcase_add_test(tc, test_roundtrip_parity_only);
	tcase_add_test(tc, test_decode_too_many_missing);
	tcase_add_test(tc, test_encode_unaligned_shard_len);
	tcase_add_test(tc, test_roundtrip_larger_geometry);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	Suite *s = snapraid_suite();
	SRunner *sr = srunner_create(s);

	srunner_run_all(sr, CK_NORMAL);
	int failed = srunner_ntests_failed(sr);

	srunner_free(sr);
	return failed == 0 ? 0 : 1;
}
