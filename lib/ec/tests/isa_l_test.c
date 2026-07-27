/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Unit tests for the Intel ISA-L RS (Vandermonde) encoding
 * wrapper (lib/ec/isa_l.c).  Mirrors the case-coverage of
 * rs_test.c, snapraid_test.c, and linux_md_test.c.
 *
 * Wire-side: FFV2_ENCODING_ISA_L_RS (proposed 0x9).  Field
 * GF(2^8), same as RS_VANDERMONDE (0x4), SNAPRAID_CAUCHY (0x6),
 * and LINUX_MD_RAID (0x8).
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
	struct ec_encoding *c = ec_isa_l_create(4, 2);

	ck_assert_ptr_nonnull(c);
	ck_assert_int_eq(c->ec_k, 4);
	ck_assert_int_eq(c->ec_m, 2);
	ck_assert_str_eq(c->ec_name, "isa-l-rs");
	ec_encoding_destroy(c);
}
END_TEST

START_TEST(test_init_invalid)
{
	ck_assert_ptr_null(ec_isa_l_create(0, 2));
	ck_assert_ptr_null(ec_isa_l_create(4, 0));
	ck_assert_ptr_null(ec_isa_l_create(-1, 2));
	/* k + m > 254 -- GF(2^8) field boundary */
	ck_assert_ptr_null(ec_isa_l_create(200, 100));
}
END_TEST

static void roundtrip(int k, int m, int nmiss, const int *miss)
{
	struct ec_encoding *c = ec_isa_l_create(k, m);

	ck_assert_ptr_nonnull(c);

	int n = k + m;
	uint8_t **orig = calloc(n, sizeof(*orig));
	uint8_t **shards = calloc(n, sizeof(*shards));
	uint8_t **data = calloc(k, sizeof(*data));
	uint8_t **parity = calloc(m, sizeof(*parity));
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
	for (int i = 0; i < m; i++)
		parity[i] = shards[k + i];

	ck_assert_int_eq(c->ec_encode(c, data, parity, SHARD_LEN), 0);
	for (int i = 0; i < m; i++)
		memcpy(orig[k + i], shards[k + i], SHARD_LEN);

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

START_TEST(test_roundtrip_one_data_k4m2)
{
	int miss[] = { 1 };

	roundtrip(4, 2, 1, miss);
}
END_TEST

START_TEST(test_roundtrip_one_parity_k4m2)
{
	int miss[] = { 5 };

	roundtrip(4, 2, 1, miss);
}
END_TEST

START_TEST(test_roundtrip_two_data_k4m2)
{
	int miss[] = { 0, 3 };

	roundtrip(4, 2, 2, miss);
}
END_TEST

START_TEST(test_roundtrip_data_plus_parity_k4m2)
{
	int miss[] = { 2, 4 };

	roundtrip(4, 2, 2, miss);
}
END_TEST

START_TEST(test_roundtrip_two_parity_k4m2)
{
	int miss[] = { 4, 5 };

	roundtrip(4, 2, 2, miss);
}
END_TEST

START_TEST(test_roundtrip_k8m4_four_miss)
{
	/* k=8, m=4 -- exercise 4-parity Cauchy-typical geometry.
	 * Drop 2 data + 2 parity to use the full decode-matrix path. */
	int miss[] = { 1, 5, 8, 11 };

	roundtrip(8, 4, 4, miss);
}
END_TEST

START_TEST(test_roundtrip_k6m3_three_data)
{
	/* Three data losses at k=6, m=3 -- classic "one full parity's
	 * worth of data reconstruction" case. */
	int miss[] = { 0, 2, 4 };

	roundtrip(6, 3, 3, miss);
}
END_TEST

START_TEST(test_decode_too_many_missing)
{
	int k = 4;
	int m = 2;
	int n = k + m;
	struct ec_encoding *c = ec_isa_l_create(k, m);
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

START_TEST(test_roundtrip_k1m1)
{
	/* Minimum geometry: 1 data + 1 parity.  Encoder is degenerate
	 * (parity == data * 1 in the field) but must roundtrip. */
	int miss[] = { 0 };

	roundtrip(1, 1, 1, miss);
}
END_TEST

START_TEST(test_zero_length_roundtrip)
{
	/* shard_len == 0 must succeed and be a no-op. */
	struct ec_encoding *c = ec_isa_l_create(4, 2);
	uint8_t *data[4] = { NULL };
	uint8_t *parity[2] = { NULL };
	uint8_t *shards[6] = { NULL };
	bool present[6] = { true, true, true, true, true, true };

	ck_assert_int_eq(c->ec_encode(c, data, parity, 0), 0);
	ck_assert_int_eq(c->ec_decode(c, shards, present, 0), 0);
	ec_encoding_destroy(c);
}
END_TEST

/*
 * Cross-check: at m <= 2, ISA-L's gf_gen_rs_matrix produces
 * P = XOR-of-all-data and Q = sum(2^i * data_i) in GF(2^8),
 * the exact byte pattern that Linux md P+Q ships and that
 * SnapRAID's first two Cauchy rows reproduce (already
 * verified against Linux md in Slice 6.2).  So ISA-L, Linux
 * md, and SnapRAID all wire-agree at m <= 2.
 *
 * Slice S.1 (2026-07-27) added reffs's own RS_VANDERMONDE
 * (0x4) to that agreement: build_encoding_matrix() now
 * hand-crafts the P/Q parity rows at m <= 2 instead of using
 * the normalized-Vandermonde bottom rows.  The four encodings
 * are now byte-identical at m <= 2; RS_VANDERMONDE stays a
 * distinct enum because its m >= 3 rows still use the
 * normalized-Vandermonde path (and RS_VANDERMONDE's decoder
 * knows how to invert them).
 *
 * At m >= 3, ISA-L and SnapRAID diverge: ISA-L keeps
 * appending Vandermonde rows (1, gen^i, gen^(2i), ...) with a
 * new generator per row, while SnapRAID's Cauchy uses
 * 1/(x_i + y_j) with SnapRAID-specific point choice.  Assert
 * that divergence at m=4 so a wrong-refactor cannot silently
 * merge SNAPRAID_CAUCHY (0x6) into ISA_L_RS (0x9).  reffs's
 * RS_VANDERMONDE also diverges from both at m >= 3 (its own
 * point set is still {1, 2, 3, 4, ...} pre-normalization);
 * widening the wire-compat map further is tracked as follow-up
 * Slice S.2.
 */
static bool parity_bytes_equal(uint8_t *a, uint8_t *b, size_t len)
{
	return memcmp(a, b, len) == 0;
}

START_TEST(test_matrix_agrees_with_snapraid_at_m2)
{
	int k = 4;
	int m = 2;
	size_t len = SHARD_LEN;
	struct ec_encoding *il = ec_isa_l_create(k, m);
	struct ec_encoding *rv = ec_rs_create(k, m);
	struct ec_encoding *sr = ec_snapraid_create(k, m);

	ck_assert_ptr_nonnull(il);
	ck_assert_ptr_nonnull(rv);
	ck_assert_ptr_nonnull(sr);

	uint8_t **data = calloc(k, sizeof(*data));
	uint8_t **p_il = calloc(m, sizeof(*p_il));
	uint8_t **p_rv = calloc(m, sizeof(*p_rv));
	uint8_t **p_sr = calloc(m, sizeof(*p_sr));

	for (int i = 0; i < k; i++) {
		data[i] = calloc(len, 1);
		fill_pattern(data[i], len, i);
	}
	for (int i = 0; i < m; i++) {
		p_il[i] = calloc(len, 1);
		p_rv[i] = calloc(len, 1);
		p_sr[i] = calloc(len, 1);
	}

	ck_assert_int_eq(il->ec_encode(il, data, p_il, len), 0);
	ck_assert_int_eq(rv->ec_encode(rv, data, p_rv, len), 0);
	ck_assert_int_eq(sr->ec_encode(sr, data, p_sr, len), 0);

	/* ISA-L and SnapRAID agree on BOTH P and Q at m=2. */
	ck_assert(parity_bytes_equal(p_il[0], p_sr[0], len));
	ck_assert(parity_bytes_equal(p_il[1], p_sr[1], len));

	/* Slice S.1: reffs's rs.c now hand-crafts P/Q at m<=2, so
	 * RS_VANDERMONDE also agrees with ISA-L on BOTH rows here. */
	ck_assert(parity_bytes_equal(p_il[0], p_rv[0], len));
	ck_assert(parity_bytes_equal(p_il[1], p_rv[1], len));

	for (int i = 0; i < k; i++)
		free(data[i]);
	for (int i = 0; i < m; i++) {
		free(p_il[i]);
		free(p_rv[i]);
		free(p_sr[i]);
	}
	free(data);
	free(p_il);
	free(p_rv);
	free(p_sr);
	ec_encoding_destroy(il);
	ec_encoding_destroy(rv);
	ec_encoding_destroy(sr);
}
END_TEST

/*
 * Slice S.1 m=1 lock-in: rs-vand at m=1 must produce the plain
 * XOR-of-all-data parity byte, byte-identical to XOR_PARITY,
 * to ISA-L's single Reed-Solomon row (which reduces to XOR when
 * m=1), and to Linux md's P row.  Locks in the "RS_VANDERMONDE
 * joins the wire-compat map at m=1" invariant so a future
 * refactor of build_encoding_matrix() can't silently regress it.
 */
START_TEST(test_matrix_agrees_at_m1_across_encoders)
{
	int k = 4;
	int m = 1;
	size_t len = SHARD_LEN;
	struct ec_encoding *il = ec_isa_l_create(k, m);
	struct ec_encoding *rv = ec_rs_create(k, m);
	struct ec_encoding *xp = ec_xor_create(k);

	ck_assert_ptr_nonnull(il);
	ck_assert_ptr_nonnull(rv);
	ck_assert_ptr_nonnull(xp);

	uint8_t **data = calloc(k, sizeof(*data));
	uint8_t **p_il = calloc(m, sizeof(*p_il));
	uint8_t **p_rv = calloc(m, sizeof(*p_rv));
	uint8_t **p_xp = calloc(m, sizeof(*p_xp));

	for (int i = 0; i < k; i++) {
		data[i] = calloc(len, 1);
		fill_pattern(data[i], len, i);
	}
	for (int i = 0; i < m; i++) {
		p_il[i] = calloc(len, 1);
		p_rv[i] = calloc(len, 1);
		p_xp[i] = calloc(len, 1);
	}

	ck_assert_int_eq(il->ec_encode(il, data, p_il, len), 0);
	ck_assert_int_eq(rv->ec_encode(rv, data, p_rv, len), 0);
	ck_assert_int_eq(xp->ec_encode(xp, data, p_xp, len), 0);

	/* All three encodings collapse to plain XOR at m=1. */
	ck_assert(parity_bytes_equal(p_il[0], p_xp[0], len));
	ck_assert(parity_bytes_equal(p_rv[0], p_xp[0], len));

	for (int i = 0; i < k; i++)
		free(data[i]);
	for (int i = 0; i < m; i++) {
		free(p_il[i]);
		free(p_rv[i]);
		free(p_xp[i]);
	}
	free(data);
	free(p_il);
	free(p_rv);
	free(p_xp);
	ec_encoding_destroy(il);
	ec_encoding_destroy(rv);
	ec_encoding_destroy(xp);
}
END_TEST

START_TEST(test_matrix_diverges_from_snapraid_at_m4)
{
	int k = 4;
	int m = 4;
	size_t len = SHARD_LEN;
	struct ec_encoding *il = ec_isa_l_create(k, m);
	struct ec_encoding *sr = ec_snapraid_create(k, m);

	ck_assert_ptr_nonnull(il);
	ck_assert_ptr_nonnull(sr);

	uint8_t **data = calloc(k, sizeof(*data));
	uint8_t **p_il = calloc(m, sizeof(*p_il));
	uint8_t **p_sr = calloc(m, sizeof(*p_sr));

	for (int i = 0; i < k; i++) {
		data[i] = calloc(len, 1);
		fill_pattern(data[i], len, i);
	}
	for (int i = 0; i < m; i++) {
		p_il[i] = calloc(len, 1);
		p_sr[i] = calloc(len, 1);
	}

	ck_assert_int_eq(il->ec_encode(il, data, p_il, len), 0);
	ck_assert_int_eq(sr->ec_encode(sr, data, p_sr, len), 0);

	/* Rows 0, 1 still agree by the m<=2 property.  Rows 2, 3 must
	 * NOT agree -- SnapRAID Cauchy has SnapRAID-specific point
	 * choice while ISA-L continues its Vandermonde sequence. */
	ck_assert(parity_bytes_equal(p_il[0], p_sr[0], len));
	ck_assert(parity_bytes_equal(p_il[1], p_sr[1], len));
	ck_assert(!parity_bytes_equal(p_il[2], p_sr[2], len));
	ck_assert(!parity_bytes_equal(p_il[3], p_sr[3], len));

	for (int i = 0; i < k; i++)
		free(data[i]);
	for (int i = 0; i < m; i++) {
		free(p_il[i]);
		free(p_sr[i]);
	}
	free(data);
	free(p_il);
	free(p_sr);
	ec_encoding_destroy(il);
	ec_encoding_destroy(sr);
}
END_TEST

Suite *isa_l_suite(void)
{
	Suite *s = suite_create("isa_l");
	TCase *tc = tcase_create("basic");

	tcase_add_test(tc, test_init_valid);
	tcase_add_test(tc, test_init_invalid);
	tcase_add_test(tc, test_roundtrip_no_loss);
	tcase_add_test(tc, test_roundtrip_one_data_k4m2);
	tcase_add_test(tc, test_roundtrip_one_parity_k4m2);
	tcase_add_test(tc, test_roundtrip_two_data_k4m2);
	tcase_add_test(tc, test_roundtrip_data_plus_parity_k4m2);
	tcase_add_test(tc, test_roundtrip_two_parity_k4m2);
	tcase_add_test(tc, test_roundtrip_k8m4_four_miss);
	tcase_add_test(tc, test_roundtrip_k6m3_three_data);
	tcase_add_test(tc, test_decode_too_many_missing);
	tcase_add_test(tc, test_roundtrip_k1m1);
	tcase_add_test(tc, test_zero_length_roundtrip);
	tcase_add_test(tc, test_matrix_agrees_with_snapraid_at_m2);
	tcase_add_test(tc, test_matrix_agrees_at_m1_across_encoders);
	tcase_add_test(tc, test_matrix_diverges_from_snapraid_at_m4);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	Suite *s = isa_l_suite();
	SRunner *sr = srunner_create(s);

	srunner_run_all(sr, CK_NORMAL);
	int failed = srunner_ntests_failed(sr);

	srunner_free(sr);
	return failed == 0 ? 0 : 1;
}
