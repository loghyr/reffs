/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Per-stripe dirty-bitmap state for PS Phase 4b (slice 4b.1).  4b.1
 * adds the data structures + WRITE-time dirty-marking that the
 * later slices (4b.2 per-stripe full-stripe flush, 4b.3 partial-
 * stripe RMW) build on.  This file covers Group A from the design's
 * "Tests first" section: pure dirty-bitmap mechanics, no flush or
 * RMW exercise yet.
 *
 * See .claude/design/proxy-server-phase4b.md.
 */

#include <check.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <urcu.h>

#include "reffs/settings.h"
#include "ps_state.h"
#include "ps_write_buffer.h"
#include "ps_write_buffer_internal.h"

/* ------------------------------------------------------------------ */
/* Fixture                                                             */
/* ------------------------------------------------------------------ */

static void setup(void)
{
	rcu_register_thread();
	ps_state_init();
}

static void teardown(void)
{
	ps_state_fini();
	rcu_unregister_thread();
}

static struct reffs_proxy_mds_config make_cfg(uint32_t id)
{
	struct reffs_proxy_mds_config cfg;

	memset(&cfg, 0, sizeof(cfg));
	cfg.id = id;
	cfg.port = 4098;
	cfg.mds_port = 2049;
	cfg.mds_probe = 20490;
	strncpy(cfg.address, "10.0.0.1", sizeof(cfg.address) - 1);
	return cfg;
}

static struct ps_listener_state *mut_listener(uint32_t id)
{
	return (struct ps_listener_state *)ps_state_find(id);
}

/*
 * Standard test geometry: k=4 data shards, m=2 parity, 4 KiB per
 * shard => 16 KiB per stripe.  Matches the Phase 4a hardcoded
 * encode geometry in ps_proxy_pipeline_commit.
 */
#define TG_K 4u
#define TG_M 2u
#define TG_SHARD 4096u
#define TG_STRIPE (TG_K * TG_SHARD) /* 16384 */

static const struct ps_write_buffer_geom std_geom = {
	.pwbg_k = TG_K,
	.pwbg_m = TG_M,
	.pwbg_shard_size = TG_SHARD,
};

/*
 * Allocate a buffer + set standard geometry under pwb_mutex.
 * Returns with the find ref held (caller releases) and the mutex
 * dropped.  Tests reacquire pwb_mutex around their own mutations.
 */
static struct ps_write_buffer *alloc_with_geom(struct ps_listener_state *pls,
					       const uint8_t *stateid,
					       const uint8_t *fh,
					       uint32_t fh_len)
{
	struct ps_write_buffer *buf;

	ck_assert(ps_write_buffer_enter_quiesce_or_bail(pls));
	buf = ps_write_buffer_lookup_or_alloc(pls, stateid, fh, fh_len);
	ck_assert_ptr_nonnull(buf);

	pthread_mutex_lock(&buf->pwb_mutex);
	ck_assert_int_eq(ps_write_buffer_set_geom(buf, &std_geom), 0);
	pthread_mutex_unlock(&buf->pwb_mutex);
	return buf;
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

START_TEST(test_dirty_bitmap_initial_all_clean)
{
	/*
	 * A freshly-allocated buffer with geometry set but no WRITE has
	 * zero dirty stripes; lookup of any stripe number returns NULL.
	 */
	struct reffs_proxy_mds_config cfg = make_cfg(1);
	uint8_t stateid[PS_STATEID_OTHER_SIZE] = { 0xA1 };
	uint8_t fh[] = { 0xF1 };
	struct ps_listener_state *pls;
	struct ps_write_buffer *buf;

	ck_assert_int_eq(ps_state_register(&cfg), 0);
	pls = mut_listener(1);
	buf = alloc_with_geom(pls, stateid, fh, sizeof(fh));

	pthread_mutex_lock(&buf->pwb_mutex);
	ck_assert_uint_eq(ps_write_buffer_dirty_count(buf), 0);
	ck_assert_ptr_null(ps_write_buffer_dirty_lookup(buf, 0));
	ck_assert_ptr_null(ps_write_buffer_dirty_lookup(buf, 99));
	pthread_mutex_unlock(&buf->pwb_mutex);

	ps_write_buffer_release_find_ref(buf, pls);
}
END_TEST

START_TEST(test_dirty_bitmap_full_stripe_write_sets_one_bit)
{
	/*
	 * WRITE covering exactly stripe 2: offset = 2*16384, count = 16384.
	 * Result: exactly one dirty entry, fully dirty (no partial mask).
	 */
	struct reffs_proxy_mds_config cfg = make_cfg(2);
	uint8_t stateid[PS_STATEID_OTHER_SIZE] = { 0xA2 };
	uint8_t fh[] = { 0xF2 };
	struct ps_listener_state *pls;
	struct ps_write_buffer *buf;
	struct ps_dirty_stripe *ds;

	ck_assert_int_eq(ps_state_register(&cfg), 0);
	pls = mut_listener(2);
	buf = alloc_with_geom(pls, stateid, fh, sizeof(fh));

	pthread_mutex_lock(&buf->pwb_mutex);
	ck_assert_int_eq(
		ps_write_buffer_mark_dirty(buf, 2u * TG_STRIPE, TG_STRIPE), 0);
	ck_assert_uint_eq(ps_write_buffer_dirty_count(buf), 1);

	ds = ps_write_buffer_dirty_lookup(buf, 2);
	ck_assert_ptr_nonnull(ds);
	ck_assert(ps_dirty_stripe_is_fully_dirty(ds));

	/* Neighbouring stripes stay clean. */
	ck_assert_ptr_null(ps_write_buffer_dirty_lookup(buf, 0));
	ck_assert_ptr_null(ps_write_buffer_dirty_lookup(buf, 1));
	ck_assert_ptr_null(ps_write_buffer_dirty_lookup(buf, 3));
	pthread_mutex_unlock(&buf->pwb_mutex);

	ps_write_buffer_release_find_ref(buf, pls);
}
END_TEST

START_TEST(test_dirty_bitmap_partial_stripe_write_sets_partial_mask)
{
	/*
	 * WRITE of 1 KiB at offset 0: touches only shard 0 of stripe 0.
	 * Partial mask must show shard 0 dirty, shards 1..3 clean.
	 */
	struct reffs_proxy_mds_config cfg = make_cfg(3);
	uint8_t stateid[PS_STATEID_OTHER_SIZE] = { 0xA3 };
	uint8_t fh[] = { 0xF3 };
	struct ps_listener_state *pls;
	struct ps_write_buffer *buf;
	struct ps_dirty_stripe *ds;

	ck_assert_int_eq(ps_state_register(&cfg), 0);
	pls = mut_listener(3);
	buf = alloc_with_geom(pls, stateid, fh, sizeof(fh));

	pthread_mutex_lock(&buf->pwb_mutex);
	ck_assert_int_eq(ps_write_buffer_mark_dirty(buf, 0, 1024), 0);
	ck_assert_uint_eq(ps_write_buffer_dirty_count(buf), 1);

	ds = ps_write_buffer_dirty_lookup(buf, 0);
	ck_assert_ptr_nonnull(ds);
	ck_assert(!ps_dirty_stripe_is_fully_dirty(ds));
	ck_assert(ps_dirty_stripe_shard_is_dirty(ds, 0));
	ck_assert(!ps_dirty_stripe_shard_is_dirty(ds, 1));
	ck_assert(!ps_dirty_stripe_shard_is_dirty(ds, 2));
	ck_assert(!ps_dirty_stripe_shard_is_dirty(ds, 3));
	pthread_mutex_unlock(&buf->pwb_mutex);

	ps_write_buffer_release_find_ref(buf, pls);
}
END_TEST

START_TEST(test_dirty_bitmap_cross_stripe_write_sets_two_bits)
{
	/*
	 * WRITE spanning the end of stripe 0 + start of stripe 1.
	 * Offset = 16384 - 1024 = 15360 (last 1 KiB of shard 3 in
	 * stripe 0), count = 2048 (1 KiB in stripe 0 shard 3, then
	 * 1 KiB in stripe 1 shard 0).  Each stripe records exactly
	 * the shard it touched.
	 */
	struct reffs_proxy_mds_config cfg = make_cfg(4);
	uint8_t stateid[PS_STATEID_OTHER_SIZE] = { 0xA4 };
	uint8_t fh[] = { 0xF4 };
	struct ps_listener_state *pls;
	struct ps_write_buffer *buf;
	struct ps_dirty_stripe *ds0, *ds1;

	ck_assert_int_eq(ps_state_register(&cfg), 0);
	pls = mut_listener(4);
	buf = alloc_with_geom(pls, stateid, fh, sizeof(fh));

	pthread_mutex_lock(&buf->pwb_mutex);
	ck_assert_int_eq(
		ps_write_buffer_mark_dirty(buf, TG_STRIPE - 1024, 2048), 0);
	ck_assert_uint_eq(ps_write_buffer_dirty_count(buf), 2);

	ds0 = ps_write_buffer_dirty_lookup(buf, 0);
	ds1 = ps_write_buffer_dirty_lookup(buf, 1);
	ck_assert_ptr_nonnull(ds0);
	ck_assert_ptr_nonnull(ds1);

	/* Stripe 0: only shard 3 dirty. */
	ck_assert(!ps_dirty_stripe_is_fully_dirty(ds0));
	ck_assert(!ps_dirty_stripe_shard_is_dirty(ds0, 0));
	ck_assert(!ps_dirty_stripe_shard_is_dirty(ds0, 1));
	ck_assert(!ps_dirty_stripe_shard_is_dirty(ds0, 2));
	ck_assert(ps_dirty_stripe_shard_is_dirty(ds0, 3));

	/* Stripe 1: only shard 0 dirty. */
	ck_assert(!ps_dirty_stripe_is_fully_dirty(ds1));
	ck_assert(ps_dirty_stripe_shard_is_dirty(ds1, 0));
	ck_assert(!ps_dirty_stripe_shard_is_dirty(ds1, 1));
	ck_assert(!ps_dirty_stripe_shard_is_dirty(ds1, 2));
	ck_assert(!ps_dirty_stripe_shard_is_dirty(ds1, 3));
	pthread_mutex_unlock(&buf->pwb_mutex);

	ps_write_buffer_release_find_ref(buf, pls);
}
END_TEST

START_TEST(test_dirty_bitmap_overwrite_same_stripe_partial_widens_mask)
{
	/*
	 * Two writes to disjoint sub-ranges of stripe 0:
	 *   write 1: 1 KiB at offset 0    -> shard 0 dirty
	 *   write 2: 1 KiB at offset 8192 -> shard 2 dirty
	 * Union: shards 0 and 2 dirty; shards 1 and 3 stay clean.
	 * Stripe is NOT fully dirty.
	 */
	struct reffs_proxy_mds_config cfg = make_cfg(5);
	uint8_t stateid[PS_STATEID_OTHER_SIZE] = { 0xA5 };
	uint8_t fh[] = { 0xF5 };
	struct ps_listener_state *pls;
	struct ps_write_buffer *buf;
	struct ps_dirty_stripe *ds;

	ck_assert_int_eq(ps_state_register(&cfg), 0);
	pls = mut_listener(5);
	buf = alloc_with_geom(pls, stateid, fh, sizeof(fh));

	pthread_mutex_lock(&buf->pwb_mutex);
	ck_assert_int_eq(ps_write_buffer_mark_dirty(buf, 0, 1024), 0);
	ck_assert_int_eq(ps_write_buffer_mark_dirty(buf, 2u * TG_SHARD, 1024),
			 0);
	ck_assert_uint_eq(ps_write_buffer_dirty_count(buf), 1);

	ds = ps_write_buffer_dirty_lookup(buf, 0);
	ck_assert_ptr_nonnull(ds);
	ck_assert(!ps_dirty_stripe_is_fully_dirty(ds));
	ck_assert(ps_dirty_stripe_shard_is_dirty(ds, 0));
	ck_assert(!ps_dirty_stripe_shard_is_dirty(ds, 1));
	ck_assert(ps_dirty_stripe_shard_is_dirty(ds, 2));
	ck_assert(!ps_dirty_stripe_shard_is_dirty(ds, 3));
	pthread_mutex_unlock(&buf->pwb_mutex);

	ps_write_buffer_release_find_ref(buf, pls);
}
END_TEST

START_TEST(test_dirty_bitmap_full_stripe_overwrite_clears_partial_mask)
{
	/*
	 * Partial-stripe write followed by a full-stripe write to the
	 * same stripe collapses the partial mask: no RMW needed at
	 * flush.  After the second write the entry is fully dirty.
	 */
	struct reffs_proxy_mds_config cfg = make_cfg(6);
	uint8_t stateid[PS_STATEID_OTHER_SIZE] = { 0xA6 };
	uint8_t fh[] = { 0xF6 };
	struct ps_listener_state *pls;
	struct ps_write_buffer *buf;
	struct ps_dirty_stripe *ds;
	uint32_t i;

	ck_assert_int_eq(ps_state_register(&cfg), 0);
	pls = mut_listener(6);
	buf = alloc_with_geom(pls, stateid, fh, sizeof(fh));

	pthread_mutex_lock(&buf->pwb_mutex);
	ck_assert_int_eq(ps_write_buffer_mark_dirty(buf, 0, 1024), 0);
	ds = ps_write_buffer_dirty_lookup(buf, 0);
	ck_assert(!ps_dirty_stripe_is_fully_dirty(ds));

	ck_assert_int_eq(ps_write_buffer_mark_dirty(buf, 0, TG_STRIPE), 0);
	ds = ps_write_buffer_dirty_lookup(buf, 0);
	ck_assert_ptr_nonnull(ds);
	ck_assert(ps_dirty_stripe_is_fully_dirty(ds));

	/* Fully-dirty entry reports every shard as dirty. */
	for (i = 0; i < TG_K; i++)
		ck_assert(ps_dirty_stripe_shard_is_dirty(ds, i));
	pthread_mutex_unlock(&buf->pwb_mutex);

	ps_write_buffer_release_find_ref(buf, pls);
}
END_TEST

START_TEST(test_dirty_bitmap_high_water_independent_of_dirty)
{
	/*
	 * Sparse write at high offset bumps pwb_high_water but only
	 * marks the stripe at that offset -- intermediate stripes
	 * stay clean.  Pins the "dirty table grows by touched stripes,
	 * not by file extent" contract.
	 */
	struct reffs_proxy_mds_config cfg = make_cfg(7);
	uint8_t stateid[PS_STATEID_OTHER_SIZE] = { 0xA7 };
	uint8_t fh[] = { 0xF7 };
	struct ps_listener_state *pls;
	struct ps_write_buffer *buf;
	/*
	 * Offset 1 MiB lands in stripe 1MiB / 16KiB = 64.  Write covers
	 * exactly shard 0 of stripe 64 (4 KiB == 1 shard).
	 */
	uint64_t off = 1024u * 1024u;
	uint32_t cnt = TG_SHARD;
	uint32_t expected_stripe = (uint32_t)(off / TG_STRIPE); /* 64 */

	ck_assert_int_eq(ps_state_register(&cfg), 0);
	pls = mut_listener(7);
	buf = alloc_with_geom(pls, stateid, fh, sizeof(fh));

	pthread_mutex_lock(&buf->pwb_mutex);
	ck_assert_int_eq(ps_write_buffer_mark_dirty(buf, off, cnt), 0);
	ck_assert_uint_eq(ps_write_buffer_dirty_count(buf), 1);
	ck_assert_ptr_nonnull(
		ps_write_buffer_dirty_lookup(buf, expected_stripe));
	/* Stripes between 0 and expected_stripe-1 stay clean. */
	ck_assert_ptr_null(ps_write_buffer_dirty_lookup(buf, 0));
	ck_assert_ptr_null(ps_write_buffer_dirty_lookup(buf, 1));
	ck_assert_ptr_null(
		ps_write_buffer_dirty_lookup(buf, expected_stripe - 1));
	ck_assert_ptr_null(
		ps_write_buffer_dirty_lookup(buf, expected_stripe + 1));
	pthread_mutex_unlock(&buf->pwb_mutex);

	ps_write_buffer_release_find_ref(buf, pls);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

static Suite *ps_write_buffer_rmw_suite(void)
{
	Suite *s = suite_create("ps_write_buffer_rmw");
	TCase *tc = tcase_create("dirty_bitmap");

	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_dirty_bitmap_initial_all_clean);
	tcase_add_test(tc, test_dirty_bitmap_full_stripe_write_sets_one_bit);
	tcase_add_test(
		tc, test_dirty_bitmap_partial_stripe_write_sets_partial_mask);
	tcase_add_test(tc, test_dirty_bitmap_cross_stripe_write_sets_two_bits);
	tcase_add_test(
		tc,
		test_dirty_bitmap_overwrite_same_stripe_partial_widens_mask);
	tcase_add_test(
		tc,
		test_dirty_bitmap_full_stripe_overwrite_clears_partial_mask);
	tcase_add_test(tc, test_dirty_bitmap_high_water_independent_of_dirty);

	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	SRunner *sr = srunner_create(ps_write_buffer_rmw_suite());

	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return failed == 0 ? 0 : 1;
}
