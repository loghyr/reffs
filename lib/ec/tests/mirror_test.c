/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Unit tests for the mirror codec.
 *
 * The MIRRORED encoding (FFV2_ENCODING_MIRRORED on the wire,
 * draft-haynes-nfsv4-flexfiles-v2 sec-encoding-mirrored) replicates
 * data[0] verbatim across all k replicas.  Decode picks any one
 * present shard as authoritative and populates the missing slots
 * with a copy of it.
 *
 * These tests exercise the codec interface (ec_codec_t / encode /
 * decode) in isolation; the pipeline plumbing that lays shards
 * out at the same file offset and the wire-level FFv2 emit live
 * in lib/nfs4/ps/ec_pipeline.c and are exercised by
 * scripts/test_mirror_local.sh.
 *
 * Case coverage matches the reviewer-named matrix:
 *   1. encode replicates data[0] into data[1..k-1]
 *   2. encode is a no-op when caller already aliased data[i] = data[0]
 *   3. encode rejects NULL data[i] mid-array
 *   4. decode picks the first present shard
 *   5. decode fails -EIO when no shard is present
 *   6. decode rejects NULL shards[i] on a missing slot
 *   7. k=1 trivial case (single replica, no copy work)
 */

#include <check.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "reffs/ec.h"

#define SHARD_LEN 128

/* Fill a buffer with a deterministic pattern keyed off `seed`. */
static void fill_pattern(uint8_t *buf, size_t len, int seed)
{
	for (size_t i = 0; i < len; i++)
		buf[i] = (uint8_t)((seed * 31 + i * 11 + 7) & 0xff);
}

START_TEST(test_init_valid)
{
	struct ec_codec *c = ec_mirror_create(3);

	ck_assert_ptr_nonnull(c);
	ck_assert_int_eq(c->ec_k, 3);
	ck_assert_int_eq(c->ec_m, 0);
	ck_assert_str_eq(c->ec_name, "mirror");
	ec_codec_destroy(c);
}
END_TEST

START_TEST(test_init_invalid)
{
	ck_assert_ptr_null(ec_mirror_create(0));
	ck_assert_ptr_null(ec_mirror_create(-1));
	ck_assert_ptr_null(ec_mirror_create(256)); /* k > 255 */
}
END_TEST

/* Case 1: encode replicates data[0] into data[1..k-1]. */
START_TEST(test_encode_replicates_data0)
{
	int k = 4;
	struct ec_codec *c = ec_mirror_create(k);

	ck_assert_ptr_nonnull(c);

	uint8_t *data[4];
	uint8_t orig[SHARD_LEN];

	fill_pattern(orig, SHARD_LEN, 1);
	for (int i = 0; i < k; i++) {
		data[i] = calloc(SHARD_LEN, 1);
		ck_assert_ptr_nonnull(data[i]);
	}
	memcpy(data[0], orig, SHARD_LEN);
	/* data[1..3] start zeroed. */

	ck_assert_int_eq(c->ec_encode(c, data, NULL, SHARD_LEN), 0);

	/* Every shard now carries the same payload as data[0]. */
	for (int i = 0; i < k; i++)
		ck_assert_mem_eq(data[i], orig, SHARD_LEN);

	for (int i = 0; i < k; i++)
		free(data[i]);
	ec_codec_destroy(c);
}
END_TEST

/*
 * Case 2: encode skips memcpy when data[i] is aliased to data[0].
 * No crash, no spurious work; output unchanged.
 */
START_TEST(test_encode_aliased_buffers_noop)
{
	int k = 3;
	struct ec_codec *c = ec_mirror_create(k);

	ck_assert_ptr_nonnull(c);

	uint8_t backing[SHARD_LEN];

	fill_pattern(backing, SHARD_LEN, 2);

	uint8_t *data[3] = { backing, backing, backing };

	ck_assert_int_eq(c->ec_encode(c, data, NULL, SHARD_LEN), 0);

	/* All three pointers are the same buffer; content unchanged. */
	uint8_t expected[SHARD_LEN];

	fill_pattern(expected, SHARD_LEN, 2);
	ck_assert_mem_eq(backing, expected, SHARD_LEN);

	ec_codec_destroy(c);
}
END_TEST

/*
 * Case 3: NULL data[i] in the middle of the array is rejected
 * with -EINVAL rather than crashing.
 */
START_TEST(test_encode_null_data_rejected)
{
	int k = 3;
	struct ec_codec *c = ec_mirror_create(k);

	ck_assert_ptr_nonnull(c);

	uint8_t *data[3];

	data[0] = calloc(SHARD_LEN, 1);
	data[1] = NULL; /* the gap */
	data[2] = calloc(SHARD_LEN, 1);

	ck_assert_int_eq(c->ec_encode(c, data, NULL, SHARD_LEN), -EINVAL);

	free(data[0]);
	free(data[2]);
	ec_codec_destroy(c);

	/* And NULL data[0] -- the source -- is also rejected. */
	c = ec_mirror_create(k);
	ck_assert_ptr_nonnull(c);
	uint8_t *data2[3] = { NULL, NULL, NULL };

	ck_assert_int_eq(c->ec_encode(c, data2, NULL, SHARD_LEN), -EINVAL);

	/* NULL data array entirely. */
	ck_assert_int_eq(c->ec_encode(c, NULL, NULL, SHARD_LEN), -EINVAL);

	ec_codec_destroy(c);
}
END_TEST

/* Case 4: decode picks the first present shard, populates missing slots. */
START_TEST(test_decode_picks_present_shard)
{
	int k = 4;
	struct ec_codec *c = ec_mirror_create(k);

	ck_assert_ptr_nonnull(c);

	uint8_t *shards[4];
	uint8_t source[SHARD_LEN];

	fill_pattern(source, SHARD_LEN, 5);

	for (int i = 0; i < k; i++) {
		shards[i] = calloc(SHARD_LEN, 1);
		ck_assert_ptr_nonnull(shards[i]);
	}
	/* Only shards[2] holds the real bytes; the rest are zero. */
	memcpy(shards[2], source, SHARD_LEN);

	bool present[4] = { false, false, true, false };

	ck_assert_int_eq(c->ec_decode(c, shards, present, SHARD_LEN), 0);

	for (int i = 0; i < k; i++)
		ck_assert_mem_eq(shards[i], source, SHARD_LEN);

	for (int i = 0; i < k; i++)
		free(shards[i]);
	ec_codec_destroy(c);
}
END_TEST

/* Case 5: decode returns -EIO when no shard is present. */
START_TEST(test_decode_all_absent_eio)
{
	int k = 3;
	struct ec_codec *c = ec_mirror_create(k);

	ck_assert_ptr_nonnull(c);

	uint8_t *shards[3];

	for (int i = 0; i < k; i++)
		shards[i] = calloc(SHARD_LEN, 1);

	bool present[3] = { false, false, false };

	ck_assert_int_eq(c->ec_decode(c, shards, present, SHARD_LEN), -EIO);

	for (int i = 0; i < k; i++)
		free(shards[i]);
	ec_codec_destroy(c);
}
END_TEST

/*
 * Case 6: NULL shards[i] on a missing slot is rejected with -EINVAL
 * rather than crashing inside memcpy.  Also covers NULL shards[]
 * entirely and NULL on the chosen source shard.
 */
START_TEST(test_decode_null_shard_rejected)
{
	int k = 3;
	struct ec_codec *c = ec_mirror_create(k);

	ck_assert_ptr_nonnull(c);

	/* (a) NULL on a slot we'd otherwise write into. */
	uint8_t *shards_a[3];
	uint8_t buf[SHARD_LEN];

	fill_pattern(buf, SHARD_LEN, 9);
	shards_a[0] = buf;
	shards_a[1] = NULL; /* the missing-target gap */
	shards_a[2] = calloc(SHARD_LEN, 1);

	bool present_a[3] = { true, false, false };

	ck_assert_int_eq(c->ec_decode(c, shards_a, present_a, SHARD_LEN),
			 -EINVAL);
	free(shards_a[2]);

	/* (b) NULL on the source slot the decode picks first. */
	uint8_t *shards_b[3];

	shards_b[0] = NULL; /* the picked source */
	shards_b[1] = calloc(SHARD_LEN, 1);
	shards_b[2] = calloc(SHARD_LEN, 1);

	bool present_b[3] = { true, false, false };

	ck_assert_int_eq(c->ec_decode(c, shards_b, present_b, SHARD_LEN),
			 -EINVAL);
	free(shards_b[1]);
	free(shards_b[2]);

	/* (c) NULL shards[] entirely. */
	bool present_c[3] = { true, false, false };

	ck_assert_int_eq(c->ec_decode(c, NULL, present_c, SHARD_LEN), -EINVAL);

	ec_codec_destroy(c);
}
END_TEST

/*
 * Case 7: k = 1 trivial case.  Encode has nothing to replicate;
 * decode either trivially returns OK (present[0] true) or -EIO
 * (present[0] false).
 */
START_TEST(test_k1_trivial)
{
	struct ec_codec *c = ec_mirror_create(1);

	ck_assert_ptr_nonnull(c);
	ck_assert_int_eq(c->ec_k, 1);

	uint8_t buf[SHARD_LEN];

	fill_pattern(buf, SHARD_LEN, 3);
	uint8_t *data[1] = { buf };

	ck_assert_int_eq(c->ec_encode(c, data, NULL, SHARD_LEN), 0);

	/* Content unchanged; nothing to replicate to. */
	uint8_t expected[SHARD_LEN];

	fill_pattern(expected, SHARD_LEN, 3);
	ck_assert_mem_eq(buf, expected, SHARD_LEN);

	bool present_ok[1] = { true };

	ck_assert_int_eq(c->ec_decode(c, data, present_ok, SHARD_LEN), 0);
	ck_assert_mem_eq(buf, expected, SHARD_LEN);

	bool present_fail[1] = { false };

	ck_assert_int_eq(c->ec_decode(c, data, present_fail, SHARD_LEN), -EIO);

	ec_codec_destroy(c);
}
END_TEST

Suite *mirror_suite(void)
{
	Suite *s = suite_create("Mirror Codec");
	TCase *tc = tcase_create("mirror");

	tcase_add_test(tc, test_init_valid);
	tcase_add_test(tc, test_init_invalid);
	tcase_add_test(tc, test_encode_replicates_data0);
	tcase_add_test(tc, test_encode_aliased_buffers_noop);
	tcase_add_test(tc, test_encode_null_data_rejected);
	tcase_add_test(tc, test_decode_picks_present_shard);
	tcase_add_test(tc, test_decode_all_absent_eio);
	tcase_add_test(tc, test_decode_null_shard_rejected);
	tcase_add_test(tc, test_k1_trivial);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	SRunner *sr = srunner_create(mirror_suite());

	srunner_run_all(sr, CK_NORMAL);
	int nf = srunner_ntests_failed(sr);

	srunner_free(sr);
	return nf ? EXIT_FAILURE : EXIT_SUCCESS;
}
