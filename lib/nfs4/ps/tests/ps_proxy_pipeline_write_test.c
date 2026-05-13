/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Buffer-fill behaviour of ps_proxy_pipeline_write (PS Phase 4a
 * slice 4a.2b) plus the slice 4b.6 inline-flush behaviour for
 * stable != UNSTABLE4.  The UNSTABLE4 tests pin the buffered-bytes
 * contract clients see across sequential, sparse, and overwrite
 * WRITE patterns -- without any upstream MDS / DS round-trip.
 * The 4b.6 tests cover the FILE_SYNC4 / DATA_SYNC4 path, which
 * calls into ec_*_stripe_with_file -> mds_compound_send_with_auth;
 * those tests use the strong-override below to short-circuit the
 * codec with -EIO so we can assert on attempt counts and post-
 * flush dirty state without standing up a full mock MDS+DS.
 *
 * Tests reach into struct ps_write_buffer directly (via the
 * whitebox internal header) to verify the bytes landed where
 * expected and to peek the dirty-stripe bitmap after a flush.
 */

#include <check.h>
#include <errno.h>
#include <pthread.h>
#include <rpc/rpc.h>
#include <rpc/auth_unix.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <urcu.h>
#include <urcu/rculfhash.h>

#include "reffs/settings.h"
#include "ec_client.h"
#include "ps_proxy_ops.h"
#include "ps_state.h"
#include "ps_write_buffer.h"
#include "ps_write_buffer_internal.h"

/* ------------------------------------------------------------------ */
/* Strong override of mds_compound_send_with_auth (declared weak in   */
/* lib/nfs4/client/mds_compound.c).  Active for every test in this    */
/* TU -- the 4a UNSTABLE4 tests never reach upstream so the override  */
/* is silent for them; the 4b.6 inline-flush tests rely on it.        */
/* ------------------------------------------------------------------ */

static int g_send_call_count;

int mds_compound_send_with_auth(struct mds_compound *mc __attribute__((unused)),
				struct mds_session *ms __attribute__((unused)),
				const struct authunix_parms *creds
				__attribute__((unused)))
{
	g_send_call_count++;
	return -EIO; /* bail the codec before any DS work */
}

/* ------------------------------------------------------------------ */
/* Fixture                                                             */
/* ------------------------------------------------------------------ */

#define TEST_LISTENER_ID 7
#define TEST_UPSTREAM_FH_BYTE 0xAB

static struct mds_session g_test_session;

static void setup(void)
{
	struct reffs_proxy_mds_config cfg;

	rcu_register_thread();
	ps_state_init();
	memset(&cfg, 0, sizeof(cfg));
	cfg.id = TEST_LISTENER_ID;
	cfg.port = 4098;
	cfg.mds_port = 2049;
	cfg.mds_probe = 20490;
	strncpy(cfg.address, "10.0.0.1", sizeof(cfg.address) - 1);
	ck_assert_int_eq(ps_state_register(&cfg), 0);

	memset(&g_test_session, 0, sizeof(g_test_session));
	atomic_store_explicit(&g_test_session.ms_kick_listener_id,
			      TEST_LISTENER_ID, memory_order_relaxed);

	g_send_call_count = 0;
}

static void teardown(void)
{
	ps_state_fini();
	rcu_unregister_thread();
}

static struct mds_session *test_session(void)
{
	return &g_test_session;
}

/*
 * Reach into the buffer table to find the buffer for (stateid, fh)
 * so the test can inspect pwb_data / pwb_high_water.  Returns a
 * borrowed pointer (NOT a find ref) -- the test holds no ref so
 * the buffer must not be torn down while we look at it.  Safe in
 * these tests because we never trigger teardown between
 * pipeline_write and the inspection.
 */
static struct ps_write_buffer *peek_buffer(const uint8_t *stateid_other,
					   const uint8_t *fh, uint32_t fh_len)
{
	struct ps_listener_state *pls =
		(struct ps_listener_state *)ps_state_find(TEST_LISTENER_ID);
	struct ps_write_buffer *buf;

	ck_assert_ptr_nonnull(pls);
	ck_assert(ps_write_buffer_enter_quiesce_or_bail(pls));
	buf = ps_write_buffer_lookup_or_alloc(pls, stateid_other, fh, fh_len);
	ck_assert_ptr_nonnull(buf);
	ps_write_buffer_release_find_ref(buf, pls);
	return buf;
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

START_TEST(test_write_returns_unstable_with_verifier)
{
	uint8_t fh[] = { TEST_UPSTREAM_FH_BYTE };
	uint8_t sid[PS_STATEID_OTHER_SIZE] = { 0x01 };
	uint8_t data[] = { 'h', 'i' };
	struct ps_proxy_write_reply reply;
	int verf_nonzero = 0;

	memset(&reply, 0, sizeof(reply));
	int ret = ps_proxy_pipeline_write(test_session(), fh, sizeof(fh), 0,
					  sid, 0, /* stable */ 0, data,
					  sizeof(data), NULL, &reply);

	ck_assert_int_eq(ret, 0);
	ck_assert_uint_eq(reply.count, sizeof(data));
	ck_assert_uint_eq(reply.committed, 0); /* UNSTABLE4 */
	for (int i = 0; i < PS_PROXY_VERIFIER_SIZE; i++)
		if (reply.verifier[i] != 0)
			verf_nonzero = 1;
	ck_assert_int_eq(verf_nonzero, 1);
}
END_TEST

START_TEST(test_write_buffer_appends_in_order)
{
	uint8_t fh[] = { TEST_UPSTREAM_FH_BYTE };
	uint8_t sid[PS_STATEID_OTHER_SIZE] = { 0x02 };
	uint8_t first[64 * 1024];
	uint8_t second[64 * 1024];
	struct ps_proxy_write_reply r1, r2;
	struct ps_write_buffer *buf;

	memset(first, 0xAA, sizeof(first));
	memset(second, 0xBB, sizeof(second));
	memset(&r1, 0, sizeof(r1));
	memset(&r2, 0, sizeof(r2));

	int ret = ps_proxy_pipeline_write(test_session(), fh, sizeof(fh), 0,
					  sid, /* offset */ 0, 0, first,
					  sizeof(first), NULL, &r1);
	ck_assert_int_eq(ret, 0);
	ret = ps_proxy_pipeline_write(test_session(), fh, sizeof(fh), 0, sid,
				      /* offset */ sizeof(first), 0, second,
				      sizeof(second), NULL, &r2);
	ck_assert_int_eq(ret, 0);

	buf = peek_buffer(sid, fh, sizeof(fh));
	ck_assert_uint_eq(buf->pwb_high_water, sizeof(first) + sizeof(second));
	for (size_t i = 0; i < sizeof(first); i++)
		ck_assert_uint_eq(buf->pwb_data[i], 0xAA);
	for (size_t i = 0; i < sizeof(second); i++)
		ck_assert_uint_eq(buf->pwb_data[sizeof(first) + i], 0xBB);
}
END_TEST

START_TEST(test_write_buffer_sparse_holes_zero_filled)
{
	uint8_t fh[] = { TEST_UPSTREAM_FH_BYTE };
	uint8_t sid[PS_STATEID_OTHER_SIZE] = { 0x03 };
	uint8_t a = 0xCC;
	uint8_t b = 0xDD;
	struct ps_proxy_write_reply r1, r2;
	struct ps_write_buffer *buf;

	memset(&r1, 0, sizeof(r1));
	memset(&r2, 0, sizeof(r2));

	int ret = ps_proxy_pipeline_write(test_session(), fh, sizeof(fh), 0,
					  sid, /* offset */ 0, 0, &a, 1, NULL,
					  &r1);
	ck_assert_int_eq(ret, 0);
	ret = ps_proxy_pipeline_write(test_session(), fh, sizeof(fh), 0, sid,
				      /* offset */ 1024 * 1024, 0, &b, 1, NULL,
				      &r2);
	ck_assert_int_eq(ret, 0);

	buf = peek_buffer(sid, fh, sizeof(fh));
	ck_assert_uint_eq(buf->pwb_high_water, 1024 * 1024 + 1);
	ck_assert_uint_eq(buf->pwb_data[0], 0xCC);
	ck_assert_uint_eq(buf->pwb_data[1024 * 1024], 0xDD);
	/* Spot-check the hole bytes. */
	ck_assert_uint_eq(buf->pwb_data[1], 0);
	ck_assert_uint_eq(buf->pwb_data[4096], 0);
	ck_assert_uint_eq(buf->pwb_data[1024 * 1024 - 1], 0);
}
END_TEST

START_TEST(test_write_buffer_overwrite_replaces)
{
	uint8_t fh[] = { TEST_UPSTREAM_FH_BYTE };
	uint8_t sid[PS_STATEID_OTHER_SIZE] = { 0x04 };
	uint8_t v1[] = { 'a', 'b', 'c' };
	uint8_t v2[] = { 'X', 'Y', 'Z' };
	struct ps_proxy_write_reply r;
	struct ps_write_buffer *buf;

	memset(&r, 0, sizeof(r));
	int ret = ps_proxy_pipeline_write(test_session(), fh, sizeof(fh), 0,
					  sid, 0, 0, v1, sizeof(v1), NULL, &r);
	ck_assert_int_eq(ret, 0);

	memset(&r, 0, sizeof(r));
	ret = ps_proxy_pipeline_write(test_session(), fh, sizeof(fh), 0, sid, 0,
				      0, v2, sizeof(v2), NULL, &r);
	ck_assert_int_eq(ret, 0);

	buf = peek_buffer(sid, fh, sizeof(fh));
	ck_assert_uint_eq(buf->pwb_high_water, sizeof(v2));
	ck_assert_int_eq(memcmp(buf->pwb_data, v2, sizeof(v2)), 0);
}
END_TEST

START_TEST(test_write_larger_than_cap_returns_fbig)
{
	uint8_t fh[] = { TEST_UPSTREAM_FH_BYTE };
	uint8_t sid[PS_STATEID_OTHER_SIZE] = { 0x05 };
	struct ps_proxy_write_reply r;

	/*
	 * Lie about data_len -- we don't need to allocate 1 GiB to
	 * test the cap check.  The shim only inspects data_len before
	 * touching `data`, and the cap check returns -EFBIG before
	 * any read.  Using a stack buffer of 1 byte with a forged
	 * data_len > MAX is safe because we never reach the memcpy.
	 */
	uint8_t dummy = 0;

	memset(&r, 0, sizeof(r));
	int ret = ps_proxy_pipeline_write(test_session(), fh, sizeof(fh), 0,
					  sid, 0, 0, &dummy,
					  /* data_len */
					  REFFS_PS_WRITE_BUFFER_MAX + 1, NULL,
					  &r);
	ck_assert_int_eq(ret, -EFBIG);
	ck_assert_uint_eq(r.count, 0);
}
END_TEST

START_TEST(test_write_after_listener_stop_returns_eagain)
{
	uint8_t fh[] = { TEST_UPSTREAM_FH_BYTE };
	uint8_t sid[PS_STATEID_OTHER_SIZE] = { 0x06 };
	uint8_t data[] = { 'q' };
	struct ps_proxy_write_reply r;

	ck_assert_int_eq(ps_listener_stop(TEST_LISTENER_ID), 0);

	memset(&r, 0, sizeof(r));
	int ret = ps_proxy_pipeline_write(test_session(), fh, sizeof(fh), 0,
					  sid, 0, 0, data, sizeof(data), NULL,
					  &r);
	ck_assert_int_eq(ret, -EAGAIN);
}
END_TEST

START_TEST(test_write_arg_validation)
{
	uint8_t fh[] = { TEST_UPSTREAM_FH_BYTE };
	uint8_t sid[PS_STATEID_OTHER_SIZE] = { 0x07 };
	uint8_t data[] = { 'z' };
	struct ps_proxy_write_reply r;

	memset(&r, 0, sizeof(r));

	/* NULL ms */
	ck_assert_int_eq(ps_proxy_pipeline_write(NULL, fh, sizeof(fh), 0, sid,
						 0, 0, data, sizeof(data), NULL,
						 &r),
			 -EINVAL);
	/* NULL fh */
	ck_assert_int_eq(ps_proxy_pipeline_write(test_session(), NULL,
						 sizeof(fh), 0, sid, 0, 0, data,
						 sizeof(data), NULL, &r),
			 -EINVAL);
	/* fh_len == 0 */
	ck_assert_int_eq(ps_proxy_pipeline_write(test_session(), fh, 0, 0, sid,
						 0, 0, data, sizeof(data), NULL,
						 &r),
			 -EINVAL);
	/* data_len == 0 */
	ck_assert_int_eq(ps_proxy_pipeline_write(test_session(), fh, sizeof(fh),
						 0, sid, 0, 0, data, 0, NULL,
						 &r),
			 -EINVAL);
	/* fh_len > PS_MAX_FH_SIZE */
	ck_assert_int_eq(ps_proxy_pipeline_write(
				 test_session(), fh, PS_MAX_FH_SIZE + 1, 0, sid,
				 0, 0, data, sizeof(data), NULL, &r),
			 -E2BIG);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Slice 4b.6: FILE_SYNC4 / DATA_SYNC4 inline flush                    */
/* ------------------------------------------------------------------ */

/*
 * Geometry constants mirror the WRITE handler's hardcoded Phase 4a
 * snapshot (k=4, m=2, shard=4096).  A full TG_STRIPE-sized WRITE
 * marks the buffer fully-dirty for the touched stripe, so the
 * 4b.6 inline flush dispatches via ec_write_stripe_with_file
 * (fully-dirty fast path) rather than the partial-mask RMW path.
 */
#define TG_K 4u
#define TG_SHARD 4096u
#define TG_STRIPE (TG_K * TG_SHARD) /* 16384 */

/*
 * Peek a single stripe's dirty entry by stripe number.  Returns
 * true if the stripe is currently in pwb_dirty_ht.  Acquires
 * quiesce + find ref + mutex for the duration of the lookup,
 * then releases the find ref.
 */
static bool peek_stripe_dirty(const uint8_t *sid, const uint8_t *fh,
			      size_t fh_len, uint32_t stripe_no)
{
	struct ps_listener_state *pls =
		(struct ps_listener_state *)ps_state_find(TEST_LISTENER_ID);
	struct ps_write_buffer *buf;
	struct ps_dirty_stripe *ds;
	bool dirty;

	ck_assert(ps_write_buffer_enter_quiesce_or_bail(pls));
	buf = ps_write_buffer_lookup(pls, sid, fh, fh_len);
	if (!buf)
		return false;
	pthread_mutex_lock(&buf->pwb_mutex);
	ds = ps_write_buffer_dirty_lookup(buf, stripe_no);
	dirty = (ds != NULL);
	pthread_mutex_unlock(&buf->pwb_mutex);
	ps_write_buffer_release_find_ref(buf, pls);
	return dirty;
}

START_TEST(test_write_unstable_buffers_only)
{
	uint8_t fh[] = { TEST_UPSTREAM_FH_BYTE };
	uint8_t sid[PS_STATEID_OTHER_SIZE] = { 0xD1 };
	uint8_t *data = calloc(1, TG_STRIPE);
	struct ps_proxy_write_reply r;

	ck_assert_ptr_nonnull(data);
	memset(&r, 0, sizeof(r));

	int ret = ps_proxy_pipeline_write(test_session(), fh, sizeof(fh), 0,
					  sid, 0, /* stable UNSTABLE4 */ 0,
					  data, TG_STRIPE, NULL, &r);
	/*
	 * UNSTABLE4 is the 4a deferred-flush path: bytes land in the
	 * buffer, no upstream RPC fires, reply carries UNSTABLE4 +
	 * the composed listener verifier.  Stripe 0 is now dirty
	 * pending a future COMMIT.
	 */
	ck_assert_int_eq(ret, 0);
	ck_assert_uint_eq(r.count, TG_STRIPE);
	ck_assert_uint_eq(r.committed, 0); /* UNSTABLE4 */
	ck_assert_int_eq(g_send_call_count, 0);
	ck_assert(peek_stripe_dirty(sid, fh, sizeof(fh), 0));

	free(data);
}
END_TEST

START_TEST(test_write_file_sync_flushes_inline)
{
	uint8_t fh[] = { TEST_UPSTREAM_FH_BYTE };
	uint8_t sid[PS_STATEID_OTHER_SIZE] = { 0xD2 };
	uint8_t *data = calloc(1, TG_STRIPE);
	struct ps_proxy_write_reply r;

	ck_assert_ptr_nonnull(data);
	memset(&r, 0, sizeof(r));

	int ret = ps_proxy_pipeline_write(test_session(), fh, sizeof(fh), 0,
					  sid, 0, /* stable FILE_SYNC4 */ 2,
					  data, TG_STRIPE, NULL, &r);
	/*
	 * FILE_SYNC4 triggers the slice 4b.6 inline flush.  The
	 * strong-override fails ec_write_stripe_with_file's
	 * LAYOUTGET with -EIO, the per-stripe helper returns -EIO,
	 * and the WRITE propagates -EIO.  Exactly one upstream
	 * attempt fires (the fully-dirty stripe 0); the dirty
	 * entry for stripe 0 stays set so the client can retry.
	 * The UNSTABLE4 path (the test above) fires zero attempts;
	 * this asymmetry is the smoking gun that the inline flush
	 * was invoked.
	 */
	ck_assert_int_eq(ret, -EIO);
	ck_assert_int_eq(g_send_call_count, 1);
	ck_assert(peek_stripe_dirty(sid, fh, sizeof(fh), 0));

	free(data);
}
END_TEST

START_TEST(test_write_data_sync_flushes_inline)
{
	uint8_t fh[] = { TEST_UPSTREAM_FH_BYTE };
	uint8_t sid[PS_STATEID_OTHER_SIZE] = { 0xD3 };
	uint8_t *data = calloc(1, TG_STRIPE);
	struct ps_proxy_write_reply r;

	ck_assert_ptr_nonnull(data);
	memset(&r, 0, sizeof(r));

	int ret = ps_proxy_pipeline_write(test_session(), fh, sizeof(fh), 0,
					  sid, 0, /* stable DATA_SYNC4 */ 1,
					  data, TG_STRIPE, NULL, &r);
	/*
	 * DATA_SYNC4 takes the same inline-flush path as FILE_SYNC4
	 * (reffs does not distinguish data vs metadata sync on the
	 * DS side).  Same -EIO + 1-attempt + dirty-bit-retained
	 * expectations as test_write_file_sync_flushes_inline.
	 */
	ck_assert_int_eq(ret, -EIO);
	ck_assert_int_eq(g_send_call_count, 1);
	ck_assert(peek_stripe_dirty(sid, fh, sizeof(fh), 0));

	free(data);
}
END_TEST

START_TEST(test_write_file_sync_failure_keeps_buffer)
{
	uint8_t fh[] = { TEST_UPSTREAM_FH_BYTE };
	uint8_t sid[PS_STATEID_OTHER_SIZE] = { 0xD4 };
	uint8_t *data = calloc(1, TG_STRIPE);
	struct ps_proxy_write_reply r;
	struct ps_listener_state *pls;

	ck_assert_ptr_nonnull(data);
	memset(&r, 0, sizeof(r));

	int ret = ps_proxy_pipeline_write(test_session(), fh, sizeof(fh), 0,
					  sid, 0, /* FILE_SYNC4 */ 2, data,
					  TG_STRIPE, NULL, &r);
	/*
	 * Failure-path lifetime contract: when the inline flush
	 * fails the WRITE returns -EIO, but the buffer survives
	 * (release_find_ref drops only the per-op ref; the table
	 * ref keeps the buffer alive for a client retry).  The
	 * dirty bit for the touched stripe is retained so the
	 * retry's COMMIT walks the same stripe.  The companion
	 * test_write_file_sync_flushes_inline already pins the
	 * -EIO + dirty-bit-kept return contract; this test adds
	 * the buffer-survival assertion via listener_table_count.
	 */
	ck_assert_int_eq(ret, -EIO);
	ck_assert_int_eq(g_send_call_count, 1);
	ck_assert(peek_stripe_dirty(sid, fh, sizeof(fh), 0));
	pls = (struct ps_listener_state *)ps_state_find(TEST_LISTENER_ID);
	ck_assert_uint_eq(ps_write_buffer_table_count(pls), 1);

	free(data);
}
END_TEST

START_TEST(test_write_file_sync_other_buffer_unaffected)
{
	uint8_t fh[] = { TEST_UPSTREAM_FH_BYTE };
	uint8_t sid[PS_STATEID_OTHER_SIZE] = { 0xD5 };
	uint8_t *data = calloc(1, TG_STRIPE);
	struct ps_proxy_write_reply r1, r2;

	ck_assert_ptr_nonnull(data);
	memset(&r1, 0, sizeof(r1));
	memset(&r2, 0, sizeof(r2));

	/*
	 * UNSTABLE4 WRITE to stripe 0 (buffered, no flush).
	 */
	ck_assert_int_eq(ps_proxy_pipeline_write(test_session(), fh, sizeof(fh),
						 0, sid, 0, /* UNSTABLE4 */ 0,
						 data, TG_STRIPE, NULL, &r1),
			 0);
	ck_assert(peek_stripe_dirty(sid, fh, sizeof(fh), 0));
	ck_assert_int_eq(g_send_call_count, 0);

	/*
	 * FILE_SYNC4 WRITE to stripe 5 (offset 5 * TG_STRIPE).  The
	 * range filter inside pwb_flush_range_locked scopes the
	 * flush walk to stripes intersecting [5*TG_STRIPE,
	 * 6*TG_STRIPE), which is just stripe 5.  Stripe 0 stays
	 * buffered and dirty.  The strong-override fails stripe 5
	 * with -EIO so we observe g_send_call_count == 1 and the
	 * WRITE returns -EIO.
	 *
	 * Discrimination caveat: the strong-override's
	 * unconditional -EIO means the walk loop bails after the
	 * first attempt regardless of which stripe came first in
	 * the lfht iteration order.  This test confirms the
	 * post-flush state (both stripes dirty, buffer alive, one
	 * upstream attempt) but does NOT by itself distinguish
	 * "range filter scoped the walk to {5}" from "no filter,
	 * walk hit 0 or 5 first and broke" -- both produce the
	 * same assertions.  The Group D suite as a whole pins the
	 * contract: test_write_unstable_buffers_only (0 attempts)
	 * + test_write_file_sync_flushes_inline (1 attempt on the
	 * just-written stripe) establish that the inline flush is
	 * gated on stable and on the WRITE's stripe.  A success-
	 * path mock that disambiguates the walk order is queued
	 * as a follow-up (would also let us assert
	 * `committed = FILE_SYNC4` end-to-end).
	 */
	int ret = ps_proxy_pipeline_write(test_session(), fh, sizeof(fh), 0,
					  sid, 5ULL * TG_STRIPE,
					  /* FILE_SYNC4 */ 2, data, TG_STRIPE,
					  NULL, &r2);
	ck_assert_int_eq(ret, -EIO);
	ck_assert_int_eq(g_send_call_count, 1);
	/* Stripe 0 dirty: it was outside the WRITE's byte range. */
	ck_assert(peek_stripe_dirty(sid, fh, sizeof(fh), 0));
	/* Stripe 5 dirty: attempted, failed, bit kept for retry. */
	ck_assert(peek_stripe_dirty(sid, fh, sizeof(fh), 5));

	free(data);
}
END_TEST

START_TEST(test_write_unknown_stable_rejected)
{
	uint8_t fh[] = { TEST_UPSTREAM_FH_BYTE };
	uint8_t sid[PS_STATEID_OTHER_SIZE] = { 0xD6 };
	uint8_t data[] = { 'x' };
	struct ps_proxy_write_reply r;

	memset(&r, 0, sizeof(r));
	/*
	 * stable_how4 is 0/1/2; 3 is wire-malformed.  Reject before
	 * any buffer / upstream work happens.
	 */
	int ret = ps_proxy_pipeline_write(test_session(), fh, sizeof(fh), 0,
					  sid, 0, /* invalid */ 3, data,
					  sizeof(data), NULL, &r);
	ck_assert_int_eq(ret, -EINVAL);
	ck_assert_int_eq(g_send_call_count, 0);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

static Suite *ps_proxy_pipeline_write_suite(void)
{
	Suite *s = suite_create("ps_proxy_pipeline_write");
	TCase *tc = tcase_create("buffer_fill");

	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_write_returns_unstable_with_verifier);
	tcase_add_test(tc, test_write_buffer_appends_in_order);
	tcase_add_test(tc, test_write_buffer_sparse_holes_zero_filled);
	tcase_add_test(tc, test_write_buffer_overwrite_replaces);
	tcase_add_test(tc, test_write_larger_than_cap_returns_fbig);
	tcase_add_test(tc, test_write_after_listener_stop_returns_eagain);
	tcase_add_test(tc, test_write_arg_validation);
	tcase_add_test(tc, test_write_unstable_buffers_only);
	tcase_add_test(tc, test_write_file_sync_flushes_inline);
	tcase_add_test(tc, test_write_data_sync_flushes_inline);
	tcase_add_test(tc, test_write_file_sync_failure_keeps_buffer);
	tcase_add_test(tc, test_write_file_sync_other_buffer_unaffected);
	tcase_add_test(tc, test_write_unknown_stable_rejected);

	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	SRunner *sr = srunner_create(ps_proxy_pipeline_write_suite());

	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return failed == 0 ? 0 : 1;
}
