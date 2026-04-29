/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Unit tests for Mojette erasure codec (systematic + non-systematic).
 */

#include <check.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "reffs/ec.h"

/*
 * Grid: k rows of P uint64_t elements.  shard_len = P * 8 bytes.
 * For k=4, P=16: shard_len = 128 bytes, total data = 512 bytes.
 *
 * For non-systematic tests, projection shards may be larger than
 * data shards.  MAX_PROJ_ELEMS is the max projection size for
 * p_max=3, q=1, P=16, Q=4: |3|*(16-1) + |1|*(4-1) + 1 = 49.
 */
#define TEST_K 4
#define TEST_M 2
#define TEST_P 16
#define SHARD_LEN (TEST_P * (int)sizeof(uint64_t))
#define MAX_PROJ_BYTES (49 * (int)sizeof(uint64_t))

/*
 * Production-relevant 24 KiB shard geometry: k=4 means a 96 KiB
 * payload fans out to 4 data shards of 24 KiB each (matches
 * deploy/sanity/run-sanity.sh).  P_24K = 24576 / 8 = 3072 cols;
 * the largest projection (p=3, q=1, P=3072, Q=4) is
 * |3|*(P-1) + |1|*(Q-1) + 1 = 9217 elements = 73736 bytes.  Buffer
 * sizing for non-systematic at this geometry must account for the
 * full 9217 * 8 footprint per shard.
 */
#define BIG_K 4
#define BIG_M 2
#define BIG_P 3072
#define BIG_SHARD_LEN (BIG_P * (int)sizeof(uint64_t))
#define BIG_MAX_PROJ_BYTES (9217 * (int)sizeof(uint64_t))

/* Fill a shard with a deterministic pattern. */
static void fill_shard(uint8_t *buf, size_t len, int idx)
{
	uint64_t *p = (uint64_t *)buf;
	size_t n = len / sizeof(uint64_t);

	for (size_t i = 0; i < n; i++)
		p[i] = (uint64_t)(idx * 1000 + i * 37 + 13);
}

/* Allocate k data shards + m parity buffers of max_shard_len each. */
static void alloc_shards(uint8_t **data, uint8_t **parity, uint8_t **orig,
			 int k, int m, size_t data_len, size_t data_alloc_len,
			 size_t parity_alloc_len)
{
	for (int i = 0; i < k; i++) {
		data[i] = calloc(1, data_alloc_len);
		orig[i] = calloc(1, data_len);
		fill_shard(data[i], data_len, i);
		memcpy(orig[i], data[i], data_len);
	}
	for (int i = 0; i < m; i++)
		parity[i] = calloc(1, parity_alloc_len);
}

static void free_bufs(uint8_t **bufs, int n)
{
	for (int i = 0; i < n; i++)
		free(bufs[i]);
}

/* ------------------------------------------------------------------ */
/* Systematic tests                                                    */
/* ------------------------------------------------------------------ */

START_TEST(test_sys_init)
{
	struct ec_codec *c = ec_mojette_sys_create(TEST_K, TEST_M);

	ck_assert_ptr_nonnull(c);
	ck_assert_int_eq(c->ec_k, TEST_K);
	ck_assert_int_eq(c->ec_m, TEST_M);
	ck_assert_str_eq(c->ec_name, "mojette-systematic");
	ck_assert(c->ec_shard_size != NULL);
	ec_codec_destroy(c);
}
END_TEST

START_TEST(test_sys_encode_decode_no_loss)
{
	struct ec_codec *c = ec_mojette_sys_create(TEST_K, TEST_M);
	uint8_t *data[TEST_K], *parity[TEST_M], *orig[TEST_K];

	alloc_shards(data, parity, orig, TEST_K, TEST_M, SHARD_LEN, SHARD_LEN,
		     SHARD_LEN * 4);

	int ret = c->ec_encode(c, data, parity, SHARD_LEN);

	ck_assert_int_eq(ret, 0);

	/* Data should be unchanged (systematic). */
	for (int i = 0; i < TEST_K; i++)
		ck_assert_int_eq(memcmp(data[i], orig[i], SHARD_LEN), 0);

	/* Decode with all present. */
	uint8_t *shards[TEST_K + TEST_M];
	bool present[TEST_K + TEST_M];

	for (int i = 0; i < TEST_K; i++)
		shards[i] = data[i];
	for (int i = 0; i < TEST_M; i++)
		shards[TEST_K + i] = parity[i];
	for (int i = 0; i < TEST_K + TEST_M; i++)
		present[i] = true;

	ret = c->ec_decode(c, shards, present, SHARD_LEN);
	ck_assert_int_eq(ret, 0);

	for (int i = 0; i < TEST_K; i++)
		ck_assert_int_eq(memcmp(data[i], orig[i], SHARD_LEN), 0);

	free_bufs(data, TEST_K);
	free_bufs(parity, TEST_M);
	free_bufs(orig, TEST_K);
	ec_codec_destroy(c);
}
END_TEST

START_TEST(test_sys_one_data_loss)
{
	struct ec_codec *c = ec_mojette_sys_create(TEST_K, TEST_M);
	uint8_t *data[TEST_K], *parity[TEST_M], *orig[TEST_K];

	alloc_shards(data, parity, orig, TEST_K, TEST_M, SHARD_LEN, SHARD_LEN,
		     SHARD_LEN * 4);

	c->ec_encode(c, data, parity, SHARD_LEN);

	/* Lose data shard 1. */
	uint8_t *shards[TEST_K + TEST_M];
	bool present[TEST_K + TEST_M];

	for (int i = 0; i < TEST_K; i++)
		shards[i] = data[i];
	for (int i = 0; i < TEST_M; i++)
		shards[TEST_K + i] = parity[i];
	for (int i = 0; i < TEST_K + TEST_M; i++)
		present[i] = true;

	present[1] = false;
	memset(data[1], 0, SHARD_LEN);

	int ret = c->ec_decode(c, shards, present, SHARD_LEN);

	ck_assert_int_eq(ret, 0);
	ck_assert_int_eq(memcmp(data[1], orig[1], SHARD_LEN), 0);

	free_bufs(data, TEST_K);
	free_bufs(parity, TEST_M);
	free_bufs(orig, TEST_K);
	ec_codec_destroy(c);
}
END_TEST

START_TEST(test_sys_two_data_loss)
{
	struct ec_codec *c = ec_mojette_sys_create(TEST_K, TEST_M);
	uint8_t *data[TEST_K], *parity[TEST_M], *orig[TEST_K];

	alloc_shards(data, parity, orig, TEST_K, TEST_M, SHARD_LEN, SHARD_LEN,
		     SHARD_LEN * 4);

	c->ec_encode(c, data, parity, SHARD_LEN);

	uint8_t *shards[TEST_K + TEST_M];
	bool present[TEST_K + TEST_M];

	for (int i = 0; i < TEST_K; i++)
		shards[i] = data[i];
	for (int i = 0; i < TEST_M; i++)
		shards[TEST_K + i] = parity[i];
	for (int i = 0; i < TEST_K + TEST_M; i++)
		present[i] = true;

	/* Lose data shards 0 and 3. */
	present[0] = false;
	present[3] = false;
	memset(data[0], 0, SHARD_LEN);
	memset(data[3], 0, SHARD_LEN);

	int ret = c->ec_decode(c, shards, present, SHARD_LEN);

	ck_assert_int_eq(ret, 0);
	ck_assert_int_eq(memcmp(data[0], orig[0], SHARD_LEN), 0);
	ck_assert_int_eq(memcmp(data[3], orig[3], SHARD_LEN), 0);

	free_bufs(data, TEST_K);
	free_bufs(parity, TEST_M);
	free_bufs(orig, TEST_K);
	ec_codec_destroy(c);
}
END_TEST

START_TEST(test_sys_too_many_losses)
{
	struct ec_codec *c = ec_mojette_sys_create(TEST_K, TEST_M);
	uint8_t *data[TEST_K], *parity[TEST_M], *orig[TEST_K];

	alloc_shards(data, parity, orig, TEST_K, TEST_M, SHARD_LEN, SHARD_LEN,
		     SHARD_LEN * 4);

	c->ec_encode(c, data, parity, SHARD_LEN);

	uint8_t *shards[TEST_K + TEST_M];
	bool present[TEST_K + TEST_M];

	for (int i = 0; i < TEST_K; i++)
		shards[i] = data[i];
	for (int i = 0; i < TEST_M; i++)
		shards[TEST_K + i] = parity[i];
	for (int i = 0; i < TEST_K + TEST_M; i++)
		present[i] = true;

	/* Lose 3 shards (> m=2). */
	present[0] = false;
	present[1] = false;
	present[4] = false;

	int ret = c->ec_decode(c, shards, present, SHARD_LEN);

	ck_assert_int_eq(ret, -EIO);

	free_bufs(data, TEST_K);
	free_bufs(parity, TEST_M);
	free_bufs(orig, TEST_K);
	ec_codec_destroy(c);
}
END_TEST

/*
 * 24 KiB-shard correctness.  Production sanity (run-sanity.sh)
 * fans 96 KiB out across k=4 mirrors at 24 KiB per data shard; the
 * codec must round-trip at that geometry, including under loss.
 * BIG_P=3072 makes the inverse non-trivial -- the prior tests at
 * P=16 hide any large-grid regression in moj_inverse.
 */

START_TEST(test_sys_24k_no_loss)
{
	struct ec_codec *c = ec_mojette_sys_create(BIG_K, BIG_M);
	uint8_t *data[BIG_K], *parity[BIG_M], *orig[BIG_K];

	/*
	 * Parity buffers must hold the largest projection (p=3 case
	 * is 9217 elements, the p=2 case is 6146).  Sized at
	 * BIG_MAX_PROJ_BYTES so neither parity write overruns.
	 */
	alloc_shards(data, parity, orig, BIG_K, BIG_M, BIG_SHARD_LEN,
		     BIG_SHARD_LEN, BIG_MAX_PROJ_BYTES);

	int ret = c->ec_encode(c, data, parity, BIG_SHARD_LEN);

	ck_assert_int_eq(ret, 0);

	/* Data is unchanged (systematic). */
	for (int i = 0; i < BIG_K; i++)
		ck_assert_int_eq(memcmp(data[i], orig[i], BIG_SHARD_LEN), 0);

	uint8_t *shards[BIG_K + BIG_M];
	bool present[BIG_K + BIG_M];

	for (int i = 0; i < BIG_K; i++)
		shards[i] = data[i];
	for (int i = 0; i < BIG_M; i++)
		shards[BIG_K + i] = parity[i];
	for (int i = 0; i < BIG_K + BIG_M; i++)
		present[i] = true;

	ret = c->ec_decode(c, shards, present, BIG_SHARD_LEN);
	ck_assert_int_eq(ret, 0);
	for (int i = 0; i < BIG_K; i++)
		ck_assert_int_eq(memcmp(data[i], orig[i], BIG_SHARD_LEN), 0);

	free_bufs(data, BIG_K);
	free_bufs(parity, BIG_M);
	free_bufs(orig, BIG_K);
	ec_codec_destroy(c);
}
END_TEST

START_TEST(test_sys_24k_one_data_loss)
{
	struct ec_codec *c = ec_mojette_sys_create(BIG_K, BIG_M);
	uint8_t *data[BIG_K], *parity[BIG_M], *orig[BIG_K];

	alloc_shards(data, parity, orig, BIG_K, BIG_M, BIG_SHARD_LEN,
		     BIG_SHARD_LEN, BIG_MAX_PROJ_BYTES);

	c->ec_encode(c, data, parity, BIG_SHARD_LEN);

	uint8_t *shards[BIG_K + BIG_M];
	bool present[BIG_K + BIG_M];

	for (int i = 0; i < BIG_K; i++)
		shards[i] = data[i];
	for (int i = 0; i < BIG_M; i++)
		shards[BIG_K + i] = parity[i];
	for (int i = 0; i < BIG_K + BIG_M; i++)
		present[i] = true;

	/* Lose data shard 1.  Inverse runs at n_inv=1, Q=1 -- cheap. */
	present[1] = false;
	memset(data[1], 0, BIG_SHARD_LEN);

	int ret = c->ec_decode(c, shards, present, BIG_SHARD_LEN);

	ck_assert_int_eq(ret, 0);
	ck_assert_int_eq(memcmp(data[1], orig[1], BIG_SHARD_LEN), 0);

	free_bufs(data, BIG_K);
	free_bufs(parity, BIG_M);
	free_bufs(orig, BIG_K);
	ec_codec_destroy(c);
}
END_TEST

START_TEST(test_sys_24k_two_data_loss)
{
	struct ec_codec *c = ec_mojette_sys_create(BIG_K, BIG_M);
	uint8_t *data[BIG_K], *parity[BIG_M], *orig[BIG_K];

	alloc_shards(data, parity, orig, BIG_K, BIG_M, BIG_SHARD_LEN,
		     BIG_SHARD_LEN, BIG_MAX_PROJ_BYTES);

	c->ec_encode(c, data, parity, BIG_SHARD_LEN);

	uint8_t *shards[BIG_K + BIG_M];
	bool present[BIG_K + BIG_M];

	for (int i = 0; i < BIG_K; i++)
		shards[i] = data[i];
	for (int i = 0; i < BIG_M; i++)
		shards[BIG_K + i] = parity[i];
	for (int i = 0; i < BIG_K + BIG_M; i++)
		present[i] = true;

	/*
	 * Lose data shards 0 and 3 -- non-adjacent missing rows.
	 * Inverse runs at n_inv=2, P=3072, Q=2 -- ~6144 cells, ~75M
	 * inner-loop ops; well under the 2-second per-test budget
	 * even with ASAN slowdown.
	 */
	present[0] = false;
	present[3] = false;
	memset(data[0], 0, BIG_SHARD_LEN);
	memset(data[3], 0, BIG_SHARD_LEN);

	int ret = c->ec_decode(c, shards, present, BIG_SHARD_LEN);

	ck_assert_int_eq(ret, 0);
	ck_assert_int_eq(memcmp(data[0], orig[0], BIG_SHARD_LEN), 0);
	ck_assert_int_eq(memcmp(data[3], orig[3], BIG_SHARD_LEN), 0);

	free_bufs(data, BIG_K);
	free_bufs(parity, BIG_M);
	free_bufs(orig, BIG_K);
	ec_codec_destroy(c);
}
END_TEST

START_TEST(test_sys_24k_only_parity_loss)
{
	struct ec_codec *c = ec_mojette_sys_create(BIG_K, BIG_M);
	uint8_t *data[BIG_K], *parity[BIG_M], *orig[BIG_K];

	alloc_shards(data, parity, orig, BIG_K, BIG_M, BIG_SHARD_LEN,
		     BIG_SHARD_LEN, BIG_MAX_PROJ_BYTES);

	c->ec_encode(c, data, parity, BIG_SHARD_LEN);

	/* Capture parity values to compare after re-encode. */
	uint8_t *parity_orig[BIG_M];

	for (int i = 0; i < BIG_M; i++) {
		parity_orig[i] = malloc(BIG_MAX_PROJ_BYTES);
		ck_assert_ptr_nonnull(parity_orig[i]);
		memcpy(parity_orig[i], parity[i], BIG_MAX_PROJ_BYTES);
	}

	uint8_t *shards[BIG_K + BIG_M];
	bool present[BIG_K + BIG_M];

	for (int i = 0; i < BIG_K; i++)
		shards[i] = data[i];
	for (int i = 0; i < BIG_M; i++)
		shards[BIG_K + i] = parity[i];
	for (int i = 0; i < BIG_K + BIG_M; i++)
		present[i] = true;

	/* Lose only parity (re-encode path -- no inverse needed). */
	present[BIG_K] = false;
	present[BIG_K + 1] = false;
	memset(parity[0], 0, BIG_MAX_PROJ_BYTES);
	memset(parity[1], 0, BIG_MAX_PROJ_BYTES);

	int ret = c->ec_decode(c, shards, present, BIG_SHARD_LEN);

	ck_assert_int_eq(ret, 0);

	/*
	 * Parity must round-trip bit-exactly.  Compare only the
	 * meaningful prefix (each projection has a distinct size,
	 * and projection size < BIG_MAX_PROJ_BYTES for p < 3).
	 */
	size_t p2_bytes = c->ec_shard_size(c, BIG_K + 0, BIG_SHARD_LEN);
	size_t p3_bytes = c->ec_shard_size(c, BIG_K + 1, BIG_SHARD_LEN);

	ck_assert_int_eq(memcmp(parity[0], parity_orig[0], p2_bytes), 0);
	ck_assert_int_eq(memcmp(parity[1], parity_orig[1], p3_bytes), 0);

	for (int i = 0; i < BIG_M; i++)
		free(parity_orig[i]);
	free_bufs(data, BIG_K);
	free_bufs(parity, BIG_M);
	free_bufs(orig, BIG_K);
	ec_codec_destroy(c);
}
END_TEST

START_TEST(test_sys_24k_one_data_one_parity_loss)
{
	struct ec_codec *c = ec_mojette_sys_create(BIG_K, BIG_M);
	uint8_t *data[BIG_K], *parity[BIG_M], *orig[BIG_K];

	alloc_shards(data, parity, orig, BIG_K, BIG_M, BIG_SHARD_LEN,
		     BIG_SHARD_LEN, BIG_MAX_PROJ_BYTES);

	c->ec_encode(c, data, parity, BIG_SHARD_LEN);

	uint8_t *shards[BIG_K + BIG_M];
	bool present[BIG_K + BIG_M];

	for (int i = 0; i < BIG_K; i++)
		shards[i] = data[i];
	for (int i = 0; i < BIG_M; i++)
		shards[BIG_K + i] = parity[i];
	for (int i = 0; i < BIG_K + BIG_M; i++)
		present[i] = true;

	/*
	 * Lose data[2] AND parity[1] (the larger p=3 projection).
	 * Decoder must rebuild data[2] from the single remaining
	 * parity (p=2), then re-derive the missing parity via
	 * encode.  This is the realistic single-DS-failure case
	 * where one mirror serving a parity chunk is also gone.
	 */
	present[2] = false;
	present[BIG_K + 1] = false;
	memset(data[2], 0, BIG_SHARD_LEN);
	memset(parity[1], 0, BIG_MAX_PROJ_BYTES);

	int ret = c->ec_decode(c, shards, present, BIG_SHARD_LEN);

	ck_assert_int_eq(ret, 0);
	ck_assert_int_eq(memcmp(data[2], orig[2], BIG_SHARD_LEN), 0);

	free_bufs(data, BIG_K);
	free_bufs(parity, BIG_M);
	free_bufs(orig, BIG_K);
	ec_codec_destroy(c);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Non-systematic tests                                                */
/* ------------------------------------------------------------------ */

START_TEST(test_nonsys_init)
{
	struct ec_codec *c = ec_mojette_nonsys_create(TEST_K, TEST_M);

	ck_assert_ptr_nonnull(c);
	ck_assert_str_eq(c->ec_name, "mojette-non-systematic");
	ec_codec_destroy(c);
}
END_TEST

START_TEST(test_nonsys_encode_decode_no_loss)
{
	struct ec_codec *c = ec_mojette_nonsys_create(TEST_K, TEST_M);
	uint8_t *data[TEST_K], *parity[TEST_M], *orig[TEST_K];

	alloc_shards(data, parity, orig, TEST_K, TEST_M, SHARD_LEN,
		     MAX_PROJ_BYTES, MAX_PROJ_BYTES);

	int ret = c->ec_encode(c, data, parity, SHARD_LEN);

	ck_assert_int_eq(ret, 0);

	/* After encode, data[] now holds projections (NOT original data). */

	/* Decode with all present. */
	uint8_t *shards[TEST_K + TEST_M];
	bool present[TEST_K + TEST_M];

	for (int i = 0; i < TEST_K; i++)
		shards[i] = data[i];
	for (int i = 0; i < TEST_M; i++)
		shards[TEST_K + i] = parity[i];
	for (int i = 0; i < TEST_K + TEST_M; i++)
		present[i] = true;

	ret = c->ec_decode(c, shards, present, SHARD_LEN);
	ck_assert_int_eq(ret, 0);

	/* After decode, shards[0..k-1] should be original data. */
	for (int i = 0; i < TEST_K; i++)
		ck_assert_int_eq(memcmp(shards[i], orig[i], SHARD_LEN), 0);

	free_bufs(data, TEST_K);
	free_bufs(parity, TEST_M);
	free_bufs(orig, TEST_K);
	ec_codec_destroy(c);
}
END_TEST

START_TEST(test_nonsys_one_loss)
{
	struct ec_codec *c = ec_mojette_nonsys_create(TEST_K, TEST_M);
	uint8_t *data[TEST_K], *parity[TEST_M], *orig[TEST_K];

	alloc_shards(data, parity, orig, TEST_K, TEST_M, SHARD_LEN,
		     MAX_PROJ_BYTES, MAX_PROJ_BYTES);

	c->ec_encode(c, data, parity, SHARD_LEN);

	uint8_t *shards[TEST_K + TEST_M];
	bool present[TEST_K + TEST_M];

	for (int i = 0; i < TEST_K; i++)
		shards[i] = data[i];
	for (int i = 0; i < TEST_M; i++)
		shards[TEST_K + i] = parity[i];
	for (int i = 0; i < TEST_K + TEST_M; i++)
		present[i] = true;

	/* Lose shard 2. */
	present[2] = false;

	int ret = c->ec_decode(c, shards, present, SHARD_LEN);

	ck_assert_int_eq(ret, 0);
	for (int i = 0; i < TEST_K; i++)
		ck_assert_int_eq(memcmp(shards[i], orig[i], SHARD_LEN), 0);

	free_bufs(data, TEST_K);
	free_bufs(parity, TEST_M);
	free_bufs(orig, TEST_K);
	ec_codec_destroy(c);
}
END_TEST

START_TEST(test_nonsys_max_loss)
{
	struct ec_codec *c = ec_mojette_nonsys_create(TEST_K, TEST_M);
	uint8_t *data[TEST_K], *parity[TEST_M], *orig[TEST_K];

	alloc_shards(data, parity, orig, TEST_K, TEST_M, SHARD_LEN,
		     MAX_PROJ_BYTES, MAX_PROJ_BYTES);

	c->ec_encode(c, data, parity, SHARD_LEN);

	uint8_t *shards[TEST_K + TEST_M];
	bool present[TEST_K + TEST_M];

	for (int i = 0; i < TEST_K; i++)
		shards[i] = data[i];
	for (int i = 0; i < TEST_M; i++)
		shards[TEST_K + i] = parity[i];
	for (int i = 0; i < TEST_K + TEST_M; i++)
		present[i] = true;

	/* Lose first 2 shards (= m). */
	present[0] = false;
	present[1] = false;

	int ret = c->ec_decode(c, shards, present, SHARD_LEN);

	ck_assert_int_eq(ret, 0);
	for (int i = 0; i < TEST_K; i++)
		ck_assert_int_eq(memcmp(shards[i], orig[i], SHARD_LEN), 0);

	free_bufs(data, TEST_K);
	free_bufs(parity, TEST_M);
	free_bufs(orig, TEST_K);
	ec_codec_destroy(c);
}
END_TEST

START_TEST(test_nonsys_too_many_losses)
{
	struct ec_codec *c = ec_mojette_nonsys_create(TEST_K, TEST_M);
	uint8_t *data[TEST_K], *parity[TEST_M], *orig[TEST_K];

	alloc_shards(data, parity, orig, TEST_K, TEST_M, SHARD_LEN,
		     MAX_PROJ_BYTES, MAX_PROJ_BYTES);

	c->ec_encode(c, data, parity, SHARD_LEN);

	uint8_t *shards[TEST_K + TEST_M];
	bool present[TEST_K + TEST_M];

	for (int i = 0; i < TEST_K; i++)
		shards[i] = data[i];
	for (int i = 0; i < TEST_M; i++)
		shards[TEST_K + i] = parity[i];
	for (int i = 0; i < TEST_K + TEST_M; i++)
		present[i] = true;

	present[0] = false;
	present[2] = false;
	present[4] = false;

	int ret = c->ec_decode(c, shards, present, SHARD_LEN);

	ck_assert_int_eq(ret, -EIO);

	free_bufs(data, TEST_K);
	free_bufs(parity, TEST_M);
	free_bufs(orig, TEST_K);
	ec_codec_destroy(c);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

static Suite *mojette_codec_suite(void)
{
	Suite *s = suite_create("mojette_codec");

	TCase *tc_sys = tcase_create("systematic");

	tcase_add_test(tc_sys, test_sys_init);
	tcase_add_test(tc_sys, test_sys_encode_decode_no_loss);
	tcase_add_test(tc_sys, test_sys_one_data_loss);
	tcase_add_test(tc_sys, test_sys_two_data_loss);
	tcase_add_test(tc_sys, test_sys_too_many_losses);
	tcase_add_test(tc_sys, test_sys_24k_no_loss);
	tcase_add_test(tc_sys, test_sys_24k_one_data_loss);
	tcase_add_test(tc_sys, test_sys_24k_two_data_loss);
	tcase_add_test(tc_sys, test_sys_24k_only_parity_loss);
	tcase_add_test(tc_sys, test_sys_24k_one_data_one_parity_loss);
	suite_add_tcase(s, tc_sys);

	TCase *tc_nonsys = tcase_create("non-systematic");

	tcase_add_test(tc_nonsys, test_nonsys_init);
	tcase_add_test(tc_nonsys, test_nonsys_encode_decode_no_loss);
	tcase_add_test(tc_nonsys, test_nonsys_one_loss);
	tcase_add_test(tc_nonsys, test_nonsys_max_loss);
	tcase_add_test(tc_nonsys, test_nonsys_too_many_losses);
	suite_add_tcase(s, tc_nonsys);

	return s;
}

int main(void)
{
	Suite *s = mojette_codec_suite();
	SRunner *sr = srunner_create(s);

	srunner_run_all(sr, CK_NORMAL);
	int nf = srunner_ntests_failed(sr);

	srunner_free(sr);
	return nf == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
