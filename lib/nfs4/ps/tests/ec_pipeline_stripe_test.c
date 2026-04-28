/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Unit tests for EC I/O stripe math.
 *
 * Tests the padding/stripe calculations used by ec_write/ec_read
 * without needing a live MDS or data servers.  Verifies that data
 * is correctly padded to shard boundaries for various k values and
 * input lengths.
 */

#include <check.h>
#include <stdlib.h>
#include <string.h>

/*
 * The shard size and padding logic from ec_io.c.  We replicate the
 * formula here to test it in isolation.
 */
#define EC_SHARD_SIZE (64 * 1024)

static size_t stripe_padded_len(size_t data_len, int k)
{
	size_t shard_size = EC_SHARD_SIZE;
	size_t stripe_data = (size_t)k * shard_size;

	return ((data_len + stripe_data - 1) / stripe_data) * stripe_data;
}

static size_t stripe_count(size_t data_len, int k)
{
	size_t shard_size = EC_SHARD_SIZE;
	size_t stripe_data = (size_t)k * shard_size;

	return (data_len + stripe_data - 1) / stripe_data;
}

/* ------------------------------------------------------------------ */
/* Padding tests                                                       */
/* ------------------------------------------------------------------ */

START_TEST(test_pad_zero_length)
{
	/* Zero-length input should pad to zero. */
	ck_assert_uint_eq(stripe_padded_len(0, 4), 0);
	ck_assert_uint_eq(stripe_count(0, 4), 0);
}
END_TEST

START_TEST(test_pad_exact_stripe)
{
	/* Input exactly one stripe: k * shard_size. */
	size_t exact = 4 * EC_SHARD_SIZE;

	ck_assert_uint_eq(stripe_padded_len(exact, 4), exact);
	ck_assert_uint_eq(stripe_count(exact, 4), 1);
}
END_TEST

START_TEST(test_pad_one_byte)
{
	/* Single byte should pad to one full stripe. */
	size_t expected = 4 * EC_SHARD_SIZE;

	ck_assert_uint_eq(stripe_padded_len(1, 4), expected);
	ck_assert_uint_eq(stripe_count(1, 4), 1);
}
END_TEST

START_TEST(test_pad_one_byte_over)
{
	/* One byte over a stripe boundary --> two stripes. */
	size_t one_stripe = 4 * EC_SHARD_SIZE;
	size_t expected = 2 * one_stripe;

	ck_assert_uint_eq(stripe_padded_len(one_stripe + 1, 4), expected);
	ck_assert_uint_eq(stripe_count(one_stripe + 1, 4), 2);
}
END_TEST

START_TEST(test_pad_various_k)
{
	/* k=2: stripe_data = 2*64K = 128K. 100K --> 128K. */
	ck_assert_uint_eq(stripe_padded_len(100 * 1024, 2), 2 * EC_SHARD_SIZE);

	/* k=8: stripe_data = 8*64K = 512K. 1 byte --> 512K. */
	ck_assert_uint_eq(stripe_padded_len(1, 8), 8 * EC_SHARD_SIZE);

	/* k=1: stripe_data = 64K. 64K+1 --> 128K. */
	ck_assert_uint_eq(stripe_padded_len(EC_SHARD_SIZE + 1, 1),
			  2 * EC_SHARD_SIZE);
}
END_TEST

START_TEST(test_pad_multiple_stripes)
{
	/* 3 full stripes with k=4: 3 * 4 * 64K. */
	size_t three_stripes = 3 * 4 * EC_SHARD_SIZE;

	ck_assert_uint_eq(stripe_padded_len(three_stripes, 4), three_stripes);
	ck_assert_uint_eq(stripe_count(three_stripes, 4), 3);

	/* 2 stripes + 1 byte --> 3 stripes. */
	size_t two_plus_one = 2 * 4 * EC_SHARD_SIZE + 1;

	ck_assert_uint_eq(stripe_padded_len(two_plus_one, 4), three_stripes);
	ck_assert_uint_eq(stripe_count(two_plus_one, 4), 3);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Shard index mapping                                                 */
/* ------------------------------------------------------------------ */

START_TEST(test_shard_data_pointer_offsets)
{
	/*
	 * Verify that data_shards[i] points to the correct offset in
	 * the padded buffer for each stripe.  This mirrors the logic
	 * in ec_write().
	 */
	int k = 4;
	size_t shard_size = EC_SHARD_SIZE;
	size_t stripe_data = (size_t)k * shard_size;
	size_t data_len = 2 * stripe_data; /* 2 stripes */
	uint8_t *padded = calloc(1, data_len);

	ck_assert_ptr_nonnull(padded);

	/* Fill with identifiable pattern. */
	for (size_t i = 0; i < data_len; i++)
		padded[i] = (uint8_t)(i & 0xFF);

	/* Stripe 0: shard pointers at offset 0, 64K, 128K, 192K. */
	for (int i = 0; i < k; i++) {
		uint8_t *shard =
			padded + 0 * stripe_data + (size_t)i * shard_size;
		size_t expected_off = (size_t)i * shard_size;

		ck_assert_uint_eq((size_t)(shard - padded), expected_off);
	}

	/* Stripe 1: shard pointers at offset 256K, 320K, 384K, 448K. */
	for (int i = 0; i < k; i++) {
		uint8_t *shard =
			padded + 1 * stripe_data + (size_t)i * shard_size;
		size_t expected_off = stripe_data + (size_t)i * shard_size;

		ck_assert_uint_eq((size_t)(shard - padded), expected_off);
	}

	free(padded);
}
END_TEST

/* ------------------------------------------------------------------ */
/* CHUNK total_blocks math                                             */
/*                                                                     */
/* The v2 CHUNK write path strides each per-stripe write by            */
/* DIV_CEIL(ds_stride, chunk_sz) blocks (ec_pipeline.c lines 656,      */
/* 689) and FINALIZE/COMMIT must cover that same total.  Task #147     */
/* was a truncation bug -- nstripes * (ds_stride / chunk_sz) under-    */
/* covered when ds_stride was not a chunk-multiple, which is the       */
/* mojette-systematic case.  These tests pin both sides agreeing.      */
/* ------------------------------------------------------------------ */

#define DIV_CEIL(a, b) (((a) + (b) - 1) / (b))

static uint32_t total_blocks(size_t nstripes, size_t ds_stride, size_t chunk_sz)
{
	return (uint32_t)(nstripes * DIV_CEIL(ds_stride, chunk_sz));
}

static uint32_t per_stripe_block_stride(size_t ds_stride, size_t chunk_sz)
{
	return (uint32_t)DIV_CEIL(ds_stride, chunk_sz);
}

START_TEST(test_finalize_total_blocks_rs_aligned)
{
	/* RS shard sizes happen to land on chunk boundaries -- this
	 * case worked before #147 because integer truncation produced
	 * the same answer as ceiling.
	 */
	size_t chunk_sz = 4096;
	size_t ds_stride = 24576; /* 6 chunks exactly (k=4 with 24K shards) */
	size_t nstripes = 4;

	ck_assert_uint_eq(per_stripe_block_stride(ds_stride, chunk_sz), 6);
	ck_assert_uint_eq(total_blocks(nstripes, ds_stride, chunk_sz),
			  nstripes * 6);
}
END_TEST

START_TEST(test_finalize_total_blocks_mojette_unaligned)
{
	/*
	 * Mojette parity sizes don't land on chunk boundaries.
	 * Reproducer numbers from the task description: data wsz=4096
	 * (1 block), parity[0] wsz=8208 (3 blocks), parity[1] wsz=12296
	 * (4 blocks).  ds_stride = max = 12296.  chunk_sz = 4096.
	 * Per-stripe write stride must be 4 (4*4096 = 16384 >= 12296),
	 * and total_blocks for FINALIZE/COMMIT must agree.  The pre-#147
	 * truncation gave 3 here, leaving the 4th block PENDING and
	 * causing the next CHUNK_READ to return NFS4ERR_IO.
	 */
	size_t chunk_sz = 4096;
	size_t ds_stride = 12296;
	size_t nstripes = 4;

	ck_assert_uint_eq(per_stripe_block_stride(ds_stride, chunk_sz), 4);
	ck_assert_uint_eq(total_blocks(nstripes, ds_stride, chunk_sz),
			  nstripes * 4);
	/* Belt-and-braces: the bad pre-#147 value would be nstripes * 3. */
	ck_assert_uint_ne(total_blocks(nstripes, ds_stride, chunk_sz),
			  nstripes * 3);
}
END_TEST

START_TEST(test_finalize_total_blocks_one_byte_over)
{
	/* One byte over a chunk boundary -- the most adversarial case
	 * for ceiling vs truncation.  Stride must round up by one. */
	size_t chunk_sz = 4096;
	size_t ds_stride = 4097;
	size_t nstripes = 8;

	ck_assert_uint_eq(per_stripe_block_stride(ds_stride, chunk_sz), 2);
	ck_assert_uint_eq(total_blocks(nstripes, ds_stride, chunk_sz),
			  nstripes * 2);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite setup                                                         */
/* ------------------------------------------------------------------ */

static Suite *ec_io_suite(void)
{
	Suite *s = suite_create("ec_io");
	TCase *tc = tcase_create("stripe_math");

	tcase_add_test(tc, test_pad_zero_length);
	tcase_add_test(tc, test_pad_exact_stripe);
	tcase_add_test(tc, test_pad_one_byte);
	tcase_add_test(tc, test_pad_one_byte_over);
	tcase_add_test(tc, test_pad_various_k);
	tcase_add_test(tc, test_pad_multiple_stripes);
	tcase_add_test(tc, test_shard_data_pointer_offsets);
	tcase_add_test(tc, test_finalize_total_blocks_rs_aligned);
	tcase_add_test(tc, test_finalize_total_blocks_mojette_unaligned);
	tcase_add_test(tc, test_finalize_total_blocks_one_byte_over);

	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	Suite *s = ec_io_suite();
	SRunner *sr = srunner_create(s);

	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return (failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
