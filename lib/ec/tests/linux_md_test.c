/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Unit tests for the Linux md P+Q double-parity encoding wrapper
 * (lib/ec/linux_md.c).  Mirrors the case-coverage of rs_test.c
 * and snapraid_test.c.
 *
 * Also includes a wire-compat cross-check against
 * FFV2_ENCODING_SNAPRAID_CAUCHY at m=2: an IETF-126 review
 * remark asserted that SnapRAID's first two Cauchy-matrix rows
 * reproduce Linux md's P+Q coefficients byte-for-byte.
 * test_wire_compat_with_snapraid_at_m2 verifies this at k=4
 * and k=6.
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
	struct ec_encoding *c = ec_linux_md_create(4);

	ck_assert_ptr_nonnull(c);
	ck_assert_int_eq(c->ec_k, 4);
	ck_assert_int_eq(c->ec_m, 2);
	ck_assert_str_eq(c->ec_name, "linux-md-raid6");
	ec_encoding_destroy(c);
}
END_TEST

START_TEST(test_init_invalid)
{
	ck_assert_ptr_null(ec_linux_md_create(0));
	ck_assert_ptr_null(ec_linux_md_create(1)); /* below LINUX_MD_MIN_DATA */
	ck_assert_ptr_null(ec_linux_md_create(254));
	ck_assert_ptr_null(ec_linux_md_create(-1));
}
END_TEST

static void roundtrip(int k, int nmiss, const int *miss)
{
	struct ec_encoding *c = ec_linux_md_create(k);

	ck_assert_ptr_nonnull(c);

	int n = k + 2;
	uint8_t **orig = calloc(n, sizeof(*orig));
	uint8_t **shards = calloc(n, sizeof(*shards));
	uint8_t **data = calloc(k, sizeof(*data));
	uint8_t *parity[2];
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
	parity[1] = shards[k + 1];

	ck_assert_int_eq(c->ec_encode(c, data, parity, SHARD_LEN), 0);
	memcpy(orig[k], shards[k], SHARD_LEN);
	memcpy(orig[k + 1], shards[k + 1], SHARD_LEN);

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

START_TEST(test_roundtrip_one_data)
{
	int miss[] = { 1 };

	roundtrip(4, 1, miss);
}
END_TEST

START_TEST(test_roundtrip_p_only)
{
	int miss[] = { 4 }; /* k=4, so P is at index 4 */

	roundtrip(4, 1, miss);
}
END_TEST

START_TEST(test_roundtrip_q_only)
{
	int miss[] = { 5 };

	roundtrip(4, 1, miss);
}
END_TEST

START_TEST(test_roundtrip_two_data)
{
	int miss[] = { 0, 3 };

	roundtrip(4, 2, miss);
}
END_TEST

START_TEST(test_roundtrip_data_plus_p)
{
	int miss[] = { 2, 4 };

	roundtrip(4, 2, miss);
}
END_TEST

START_TEST(test_roundtrip_data_plus_q)
{
	int miss[] = { 1, 5 };

	roundtrip(4, 2, miss);
}
END_TEST

START_TEST(test_roundtrip_p_plus_q)
{
	int miss[] = { 4, 5 };

	roundtrip(4, 2, miss);
}
END_TEST

START_TEST(test_decode_too_many_missing)
{
	int k = 4;
	int n = k + 2;
	struct ec_encoding *c = ec_linux_md_create(k);
	uint8_t *shards[6];
	bool present[6];

	ck_assert_ptr_nonnull(c);

	for (int i = 0; i < n; i++) {
		shards[i] = calloc(SHARD_LEN, 1);
		present[i] = true;
	}
	/* Drop 3 shards but only 2 parity blocks exist. */
	present[0] = false;
	present[1] = false;
	present[4] = false;

	ck_assert_int_eq(c->ec_decode(c, shards, present, SHARD_LEN), -EIO);

	for (int i = 0; i < n; i++)
		free(shards[i]);
	ec_encoding_destroy(c);
}
END_TEST

START_TEST(test_roundtrip_larger_k_two_data)
{
	/* Two data losses at k=8, m=2; both indices < p_idx=8. */
	int miss[] = { 3, 6 };

	roundtrip(8, 2, miss);
}
END_TEST

/*
 * k=1 previously tested here but ec_linux_md_create() now caps
 * at k >= 2 (LINUX_MD_MIN_DATA).  See linux_md.c: the vendored
 * raid6 SIMD dispatchers hand-unroll around n >= 4 disks and
 * misbehave at n = 3 (k=1 + P + Q).  Coverage of the k=1
 * boundary lives in test_init_invalid instead.
 */

START_TEST(test_zero_length_roundtrip)
{
	/* Encode + decode with shard_len == 0 must succeed and be a
	 * no-op.  Exercises the early-return in linux_md_encode /
	 * linux_md_decode without touching parity pointers. */
	struct ec_encoding *c = ec_linux_md_create(4);
	uint8_t *data[4] = { NULL, NULL, NULL, NULL };
	uint8_t *parity[2] = { NULL, NULL };
	uint8_t *shards[6] = { NULL };
	bool present[6] = { true, true, true, true, true, true };

	ck_assert_int_eq(c->ec_encode(c, data, parity, 0), 0);
	ck_assert_int_eq(c->ec_decode(c, shards, present, 0), 0);
	ec_encoding_destroy(c);
}
END_TEST

/*
 * Wire-compat cross-check against SnapRAID at m=2 (the IETF-126
 * on-list assertion: SnapRAID's first two Cauchy rows reproduce
 * Linux md's P+Q coefficients).  Encode the same data with both
 * encoders and memcmp the parity output.
 *
 * If this ever fails, the on-list assertion is wrong OR the
 * vendored SnapRAID upstream changed its matrix construction;
 * either way, the wire-compat story between LINUX_MD_RAID and
 * SNAPRAID_CAUCHY needs re-examination.
 */
static void wire_compat_at_k(int k)
{
	int n = k + 2;
	struct ec_encoding *md = ec_linux_md_create(k);
	struct ec_encoding *sr = ec_snapraid_create(k, 2);

	ck_assert_ptr_nonnull(md);
	ck_assert_ptr_nonnull(sr);

	uint8_t **data_md = calloc(k, sizeof(*data_md));
	uint8_t **data_sr = calloc(k, sizeof(*data_sr));
	uint8_t *parity_md[2];
	uint8_t *parity_sr[2];

	for (int i = 0; i < k; i++) {
		data_md[i] = calloc(SHARD_LEN, 1);
		data_sr[i] = calloc(SHARD_LEN, 1);
		fill_pattern(data_md[i], SHARD_LEN, i);
		memcpy(data_sr[i], data_md[i], SHARD_LEN);
	}
	for (int i = 0; i < 2; i++) {
		parity_md[i] = calloc(SHARD_LEN, 1);
		parity_sr[i] = calloc(SHARD_LEN, 1);
	}

	ck_assert_int_eq(md->ec_encode(md, data_md, parity_md, SHARD_LEN), 0);
	ck_assert_int_eq(sr->ec_encode(sr, data_sr, parity_sr, SHARD_LEN), 0);

	/*
	 * P should agree between the two encoders: both are XOR of
	 * all data by construction.
	 */
	ck_assert_mem_eq(parity_md[0], parity_sr[0], SHARD_LEN);
	/*
	 * Q is the interesting cross-check -- Linux md computes Q as
	 * sum(2^i * data_i), SnapRAID's second Cauchy row per the
	 * IETF-126 on-list assertion should produce the same
	 * coefficients.
	 */
	ck_assert_mem_eq(parity_md[1], parity_sr[1], SHARD_LEN);

	for (int i = 0; i < k; i++) {
		free(data_md[i]);
		free(data_sr[i]);
	}
	free(data_md);
	free(data_sr);
	for (int i = 0; i < 2; i++) {
		free(parity_md[i]);
		free(parity_sr[i]);
	}
	ec_encoding_destroy(md);
	ec_encoding_destroy(sr);
	(void)n;
}

START_TEST(test_wire_compat_with_snapraid_k4)
{
	wire_compat_at_k(4);
}
END_TEST

START_TEST(test_wire_compat_with_snapraid_k6)
{
	wire_compat_at_k(6);
}
END_TEST

Suite *linux_md_suite(void)
{
	Suite *s = suite_create("linux_md");
	TCase *tc = tcase_create("basic");

	/*
	 * raid6_select_algo() runs a full per-arch dispatch benchmark
	 * (measures every SIMD variant against int64x{1,2,4,8}) once
	 * per fork, taking ~300-500 ms.  Check forks per test by
	 * default, so 15 tests run 15 dispatchers back-to-back.  The
	 * default 4-second Check timeout is too tight when the
	 * framework CPU-contends with the dispatcher.  Give each test
	 * 30 s -- ample headroom without hiding real bugs.
	 */
	tcase_set_timeout(tc, 30);

	tcase_add_test(tc, test_init_valid);
	tcase_add_test(tc, test_init_invalid);
	tcase_add_test(tc, test_roundtrip_no_loss);
	tcase_add_test(tc, test_roundtrip_one_data);
	tcase_add_test(tc, test_roundtrip_p_only);
	tcase_add_test(tc, test_roundtrip_q_only);
	tcase_add_test(tc, test_roundtrip_two_data);
	tcase_add_test(tc, test_roundtrip_data_plus_p);
	tcase_add_test(tc, test_roundtrip_data_plus_q);
	tcase_add_test(tc, test_roundtrip_p_plus_q);
	tcase_add_test(tc, test_decode_too_many_missing);
	tcase_add_test(tc, test_roundtrip_larger_k_two_data);
	tcase_add_test(tc, test_zero_length_roundtrip);
	tcase_add_test(tc, test_wire_compat_with_snapraid_k4);
	tcase_add_test(tc, test_wire_compat_with_snapraid_k6);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	Suite *s = linux_md_suite();
	SRunner *sr = srunner_create(s);

	srunner_run_all(sr, CK_NORMAL);
	int failed = srunner_ntests_failed(sr);

	srunner_free(sr);
	return failed == 0 ? 0 : 1;
}
