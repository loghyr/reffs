/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Behaviour of ps_proxy_pipeline_commit (PS Phase 4a slice 4a.2c).
 *
 * The shim drives ec_write_encoding_with_file -> mds_layout_get for
 * the flush; mds_layout_get goes through mds_compound_send_with_auth
 * which we strong-override here to return -EIO.  This short-
 * circuits the encoding before it touches DS state, so the tests can
 * verify the commit-side contract -- "did the flush attempt fire,
 * did the buffer stay around on failure, did no-op COMMIT return
 * the right verifier" -- without standing up a full mock MDS+DS.
 *
 * The happy-path success case (encoding returns 0, buffer is dropped)
 * is exercised end-to-end by ec_demo against the bench docker-
 * compose; see scripts/ci_ps_phase4a_test.sh (slice 4a.5).
 */

#include <check.h>
#include <errno.h>
#include <rpc/rpc.h>
#include <rpc/auth_unix.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <urcu.h>

#include "ec_client.h"
#include "reffs/settings.h"
#include "ps_proxy_ops.h"
#include "ps_state.h"
#include "ps_write_buffer.h"
#include "ps_write_buffer_internal.h"

/* ------------------------------------------------------------------ */
/* Strong override of mds_compound_send_with_auth (declared weak in   */
/* lib/nfs4/client/mds_compound.c -- mirrors the existing creds-      */
/* forward test file's override).                                      */
/* ------------------------------------------------------------------ */

static int g_send_call_count;
static const struct authunix_parms *g_captured_creds;

int mds_compound_send_with_auth(struct mds_compound *mc __attribute__((unused)),
				struct mds_session *ms __attribute__((unused)),
				const struct authunix_parms *creds)
{
	g_send_call_count++;
	g_captured_creds = creds;
	return -EIO; /* bail the encoding before any DS work */
}

/* ------------------------------------------------------------------ */
/* Fixture                                                             */
/* ------------------------------------------------------------------ */

#define TEST_LISTENER_ID 11
#define TEST_FH_BYTE 0xC0

/*
 * Geometry constants matching the WRITE handler's hardcoded Phase 4a
 * snapshot (k=4, m=2, shard=4096).  Tests that exercise the slice
 * 4b.2 fully-dirty flush path WRITE a TG_STRIPE-sized buffer so the
 * dirty-bitmap walker sees a fully-dirty entry (pds_partial_mask
 * NULL) and invokes ec_write_stripe_with_file.  Tests that exercise
 * the partial-mask -EIO path WRITE a sub-stripe range instead.
 */
#define TG_K 4u
#define TG_SHARD 4096u
#define TG_STRIPE (TG_K * TG_SHARD) /* 16384 */

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
	g_captured_creds = NULL;
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

static size_t listener_table_count(void)
{
	struct ps_listener_state *pls =
		(struct ps_listener_state *)ps_state_find(TEST_LISTENER_ID);

	return ps_write_buffer_table_count(pls);
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

START_TEST(test_commit_no_buffer_returns_verifier)
{
	uint8_t fh[] = { TEST_FH_BYTE };
	struct ps_proxy_commit_reply reply;
	int verf_nonzero = 0;

	memset(&reply, 0, sizeof(reply));
	int ret = ps_proxy_pipeline_commit(test_session(), fh, sizeof(fh), 0, 0,
					   NULL, &reply);

	ck_assert_int_eq(ret, 0);
	ck_assert_int_eq(g_send_call_count, 0); /* no upstream call */
	for (int i = 0; i < PS_PROXY_VERIFIER_SIZE; i++)
		if (reply.verifier[i] != 0)
			verf_nonzero = 1;
	ck_assert_int_eq(verf_nonzero, 1);
}
END_TEST

START_TEST(test_commit_returns_same_verifier_as_writes)
{
	uint8_t fh[] = { TEST_FH_BYTE };
	uint8_t sid[PS_STATEID_OTHER_SIZE] = { 0xB1 };
	uint8_t *data = calloc(1, TG_STRIPE);
	struct ps_proxy_write_reply w;
	struct ps_proxy_commit_reply c;

	ck_assert_ptr_nonnull(data);
	memset(&w, 0, sizeof(w));
	int ret = ps_proxy_pipeline_write(test_session(), fh, sizeof(fh), 0,
					  sid, 0, 0, data, TG_STRIPE, NULL, &w);
	ck_assert_int_eq(ret, 0);

	memset(&c, 0, sizeof(c));
	ret = ps_proxy_pipeline_commit(test_session(), fh, sizeof(fh), 0, 0,
				       NULL, &c);
	/*
	 * The full-stripe WRITE marks the buffer fully-dirty for
	 * stripe 0; the slice 4b.2 commit walk dispatches
	 * ec_write_stripe_with_file -> mds_layout_get, the mock
	 * returns -EIO, the encoding bails before any DS work, and
	 * pipeline_commit returns -EIO to the caller.
	 * reply.verifier is populated only on the success path;
	 * fire a no-buffer COMMIT below to capture the listener
	 * verifier and prove it matches the WRITE's verifier --
	 * both derive from pls_boot_gen.
	 */
	ck_assert_int_eq(ret, -EIO);

	memset(&c, 0, sizeof(c));
	uint8_t other_fh[] = { 0xCA };

	ret = ps_proxy_pipeline_commit(test_session(), other_fh,
				       sizeof(other_fh), 0, 0, NULL, &c);
	ck_assert_int_eq(ret, 0);
	ck_assert_int_eq(memcmp(c.verifier, w.verifier, PS_PROXY_VERIFIER_SIZE),
			 0);
	free(data);
}
END_TEST

START_TEST(test_commit_keeps_buffer_on_failure)
{
	uint8_t fh[] = { TEST_FH_BYTE };
	uint8_t sid[PS_STATEID_OTHER_SIZE] = { 0xB2 };
	uint8_t *data = calloc(1, TG_STRIPE);
	struct ps_proxy_write_reply w;
	struct ps_proxy_commit_reply c;

	ck_assert_ptr_nonnull(data);
	memset(&w, 0, sizeof(w));
	ck_assert_int_eq(ps_proxy_pipeline_write(test_session(), fh, sizeof(fh),
						 0, sid, 0, 0, data, TG_STRIPE,
						 NULL, &w),
			 0);
	ck_assert_uint_eq(listener_table_count(), 1);

	memset(&c, 0, sizeof(c));
	int ret = ps_proxy_pipeline_commit(test_session(), fh, sizeof(fh), 0, 0,
					   NULL, &c);

	ck_assert_int_eq(ret, -EIO);
	/*
	 * Buffer must remain so the client can retry COMMIT.  In
	 * slice 4b.2 the failure path also leaves the dirty entry
	 * for stripe 0 set so the retry walks back to the same
	 * ec_write_stripe_with_file call.
	 */
	ck_assert_uint_eq(listener_table_count(), 1);
	/* And the upstream LAYOUTGET was attempted exactly once
	 * (one fully-dirty stripe -> one per-stripe primitive call
	 * -> one mds_layout_get attempt before the strong-override
	 * returns -EIO). */
	ck_assert_int_eq(g_send_call_count, 1);
	free(data);
}
END_TEST

START_TEST(test_commit_attempt_uses_buffered_seqid)
{
	uint8_t fh[] = { TEST_FH_BYTE };
	uint8_t sid[PS_STATEID_OTHER_SIZE] = { 0xB3 };
	uint8_t *data = calloc(1, TG_STRIPE);
	struct ps_proxy_write_reply w;
	struct ps_proxy_commit_reply c;
	struct ps_listener_state *pls;
	struct ps_write_buffer *buf;

	ck_assert_ptr_nonnull(data);
	memset(&w, 0, sizeof(w));
	ck_assert_int_eq(ps_proxy_pipeline_write(test_session(), fh, sizeof(fh),
						 /* stateid_seqid */ 42, sid, 0,
						 0, data, TG_STRIPE, NULL, &w),
			 0);

	/*
	 * Peek the buffer state directly: pipeline_write should have
	 * stashed seqid=42 in pwb_stateid_seqid.  This is the value
	 * pipeline_commit's LAYOUTGET will carry.
	 */
	pls = (struct ps_listener_state *)ps_state_find(TEST_LISTENER_ID);
	ck_assert(ps_write_buffer_enter_quiesce_or_bail(pls));
	buf = ps_write_buffer_find_by_fh(pls, fh, sizeof(fh));
	ck_assert_ptr_nonnull(buf);
	ck_assert_uint_eq(buf->pwb_stateid_seqid, 42);
	ps_write_buffer_release_find_ref(buf, pls);

	/*
	 * Drive the commit attempt with a fully-dirty buffer so the
	 * 4b.2 walk dispatches ec_write_stripe_with_file -- which is
	 * the path that carries the stashed seqid into LAYOUTGET.
	 * The strong override fires after the compound is built so
	 * we can't inspect the stateid on the wire, but a regression
	 * that stops storing seqid would surface above on the
	 * pwb_stateid_seqid peek.
	 */
	memset(&c, 0, sizeof(c));
	int ret = ps_proxy_pipeline_commit(test_session(), fh, sizeof(fh), 0, 0,
					   NULL, &c);
	ck_assert_int_eq(ret, -EIO);
	ck_assert_int_eq(g_send_call_count, 1);
	free(data);
}
END_TEST

START_TEST(test_commit_arg_validation)
{
	uint8_t fh[] = { TEST_FH_BYTE };
	struct ps_proxy_commit_reply reply;

	memset(&reply, 0, sizeof(reply));
	ck_assert_int_eq(ps_proxy_pipeline_commit(NULL, fh, sizeof(fh), 0, 0,
						  NULL, &reply),
			 -EINVAL);
	ck_assert_int_eq(ps_proxy_pipeline_commit(test_session(), NULL,
						  sizeof(fh), 0, 0, NULL,
						  &reply),
			 -EINVAL);
	ck_assert_int_eq(ps_proxy_pipeline_commit(test_session(), fh, 0, 0, 0,
						  NULL, &reply),
			 -EINVAL);
	ck_assert_int_eq(ps_proxy_pipeline_commit(test_session(), fh,
						  PS_MAX_FH_SIZE + 1, 0, 0,
						  NULL, &reply),
			 -E2BIG);
}
END_TEST

START_TEST(test_commit_after_listener_stop_returns_eagain)
{
	uint8_t fh[] = { TEST_FH_BYTE };
	struct ps_proxy_commit_reply reply;

	ck_assert_int_eq(ps_listener_stop(TEST_LISTENER_ID), 0);

	memset(&reply, 0, sizeof(reply));
	ck_assert_int_eq(ps_proxy_pipeline_commit(test_session(), fh,
						  sizeof(fh), 0, 0, NULL,
						  &reply),
			 -EAGAIN);
}
END_TEST

START_TEST(test_commit_partial_stripe_attempts_rmw_read)
{
	uint8_t fh[] = { TEST_FH_BYTE };
	uint8_t sid[PS_STATEID_OTHER_SIZE] = { 0xB4 };
	uint8_t data[] = { 'p' }; /* 1 byte -- a sub-shard WRITE */
	struct ps_proxy_write_reply w;
	struct ps_proxy_commit_reply c;

	memset(&w, 0, sizeof(w));
	ck_assert_int_eq(ps_proxy_pipeline_write(test_session(), fh, sizeof(fh),
						 0, sid, 0, 0, data,
						 sizeof(data), NULL, &w),
			 0);

	/*
	 * Slice 4b.3 contract: a partial-mask dirty entry triggers
	 * the RMW prefix read (ec_read_stripe_with_file) BEFORE the
	 * encode + write back.  The strong-override fails the very
	 * first compound (the RMW read's LAYOUTGET) with -EIO;
	 * expected counts:
	 *   - ret == -EIO (per-stripe failure propagates).
	 *   - g_send_call_count == 1 -- LAYOUTGET attempted.  This
	 *     is the slice 4b.2 -> 4b.3 flip: 4b.2 short-circuited
	 *     at 0 sends, 4b.3 fires at least 1.
	 *   - buffer kept (listener_table_count == 1) so the client
	 *     can retry COMMIT once the DSes are reachable.
	 *
	 * The success path (post-decode merge + CHUNK_WRITE +
	 * FINALIZE + COMMIT) is exercised end-to-end by
	 * scripts/ci_ps_phase4b_test.sh against a real MDS+DS
	 * topology.  Pinning it in a unit test would require a
	 * full mock MDS+DS pair, which is out of scope.
	 */
	memset(&c, 0, sizeof(c));
	int ret = ps_proxy_pipeline_commit(test_session(), fh, sizeof(fh), 0, 0,
					   NULL, &c);
	ck_assert_int_eq(ret, -EIO);
	ck_assert_int_eq(g_send_call_count, 1);
	ck_assert_uint_eq(listener_table_count(), 1);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Slice 4b.5: COMMIT range honouring                                   */
/* ------------------------------------------------------------------ */

/*
 * Peek the dirty hash table for a single stripe, returning true if
 * the stripe is currently marked dirty.  Wraps the locking + ref
 * dance so the test bodies stay readable.
 */
static bool peek_stripe_dirty(uint8_t *fh, size_t fh_len, uint32_t stripe_no)
{
	struct ps_listener_state *pls =
		(struct ps_listener_state *)ps_state_find(TEST_LISTENER_ID);
	struct ps_write_buffer *buf;
	struct ps_dirty_stripe *ds;
	bool dirty;

	ck_assert(ps_write_buffer_enter_quiesce_or_bail(pls));
	buf = ps_write_buffer_find_by_fh(pls, fh, fh_len);
	if (!buf)
		return false;
	pthread_mutex_lock(&buf->pwb_mutex);
	ds = ps_write_buffer_dirty_lookup(buf, stripe_no);
	dirty = (ds != NULL);
	pthread_mutex_unlock(&buf->pwb_mutex);
	ps_write_buffer_release_find_ref(buf, pls);
	return dirty;
}

START_TEST(test_commit_range_intersects_dirty)
{
	uint8_t fh[] = { TEST_FH_BYTE };
	uint8_t sid[PS_STATEID_OTHER_SIZE] = { 0xB6 };
	uint8_t *data = calloc(1, TG_STRIPE);
	struct ps_proxy_write_reply w;
	struct ps_proxy_commit_reply c;

	ck_assert_ptr_nonnull(data);

	/* Mark stripe 0 dirty. */
	memset(&w, 0, sizeof(w));
	ck_assert_int_eq(ps_proxy_pipeline_write(test_session(), fh, sizeof(fh),
						 0, sid, 0, 0, data, TG_STRIPE,
						 NULL, &w),
			 0);
	/* Mark stripe 5 dirty (byte range 5*TG_STRIPE .. 6*TG_STRIPE). */
	memset(&w, 0, sizeof(w));
	ck_assert_int_eq(ps_proxy_pipeline_write(test_session(), fh, sizeof(fh),
						 0, sid, 5ULL * TG_STRIPE, 0,
						 data, TG_STRIPE, NULL, &w),
			 0);

	ck_assert(peek_stripe_dirty(fh, sizeof(fh), 0));
	ck_assert(peek_stripe_dirty(fh, sizeof(fh), 5));

	/*
	 * COMMIT(offset=0, count=TG_STRIPE).  Only stripe 0 intersects
	 * the requested range; stripe 5 is filtered out by 4b.5's range
	 * honouring and never reaches the per-stripe flush primitive.
	 * The mock fails the stripe-0 LAYOUTGET with -EIO, so the
	 * commit returns -EIO and both stripes remain dirty (stripe 0
	 * because the flush failed, stripe 5 because it was skipped by
	 * range).  Exactly one upstream attempt fires.
	 */
	memset(&c, 0, sizeof(c));
	int ret = ps_proxy_pipeline_commit(test_session(), fh, sizeof(fh), 0,
					   TG_STRIPE, NULL, &c);
	ck_assert_int_eq(ret, -EIO);
	ck_assert_int_eq(g_send_call_count, 1);
	ck_assert_uint_eq(listener_table_count(), 1);
	ck_assert(peek_stripe_dirty(fh, sizeof(fh), 0));
	ck_assert(peek_stripe_dirty(fh, sizeof(fh), 5));

	free(data);
}
END_TEST

START_TEST(test_commit_range_excludes_dirty_skips_flush)
{
	uint8_t fh[] = { TEST_FH_BYTE };
	uint8_t sid[PS_STATEID_OTHER_SIZE] = { 0xB7 };
	uint8_t *data = calloc(1, TG_STRIPE);
	struct ps_proxy_write_reply w;
	struct ps_proxy_commit_reply c;

	ck_assert_ptr_nonnull(data);

	/* Only stripe 5 dirty. */
	memset(&w, 0, sizeof(w));
	ck_assert_int_eq(ps_proxy_pipeline_write(test_session(), fh, sizeof(fh),
						 0, sid, 5ULL * TG_STRIPE, 0,
						 data, TG_STRIPE, NULL, &w),
			 0);
	ck_assert(peek_stripe_dirty(fh, sizeof(fh), 5));

	/*
	 * COMMIT(offset=0, count=TG_STRIPE).  Stripe 5's range
	 * [5*TG_STRIPE, 6*TG_STRIPE) does not intersect [0, TG_STRIPE);
	 * the range filter skips the only dirty entry.  Zero upstream
	 * attempts fire, the COMMIT returns success (the requested
	 * range had no dirty bytes), and the buffer stays alive with
	 * stripe 5 still dirty for a later wider-range COMMIT.
	 */
	memset(&c, 0, sizeof(c));
	int ret = ps_proxy_pipeline_commit(test_session(), fh, sizeof(fh), 0,
					   TG_STRIPE, NULL, &c);
	ck_assert_int_eq(ret, 0);
	ck_assert_int_eq(g_send_call_count, 0);
	ck_assert_uint_eq(listener_table_count(), 1);
	ck_assert(peek_stripe_dirty(fh, sizeof(fh), 5));

	free(data);
}
END_TEST

START_TEST(test_commit_range_zero_count_flushes_all)
{
	uint8_t fh[] = { TEST_FH_BYTE };
	uint8_t sid[PS_STATEID_OTHER_SIZE] = { 0xB8 };
	uint8_t *data = calloc(1, TG_STRIPE);
	struct ps_proxy_write_reply w;
	struct ps_proxy_commit_reply c;

	ck_assert_ptr_nonnull(data);

	/* Only stripe 5 dirty -- far outside any non-zero count range
	 * that starts at offset 0.  count=0 must override that. */
	memset(&w, 0, sizeof(w));
	ck_assert_int_eq(ps_proxy_pipeline_write(test_session(), fh, sizeof(fh),
						 0, sid, 5ULL * TG_STRIPE, 0,
						 data, TG_STRIPE, NULL, &w),
			 0);
	ck_assert(peek_stripe_dirty(fh, sizeof(fh), 5));

	/*
	 * COMMIT(offset=0, count=0) -- the RFC 8881 S18.3.4 "commit
	 * everything" sentinel.  The range filter must be disabled so
	 * stripe 5 is included in the flush walk; the mock then fails
	 * its LAYOUTGET with -EIO.  Exactly one upstream attempt
	 * fires; without the count==0 special case, the filter would
	 * exclude every dirty stripe (no [offset, offset+0) range
	 * intersects anything) and zero attempts would fire.
	 */
	memset(&c, 0, sizeof(c));
	int ret = ps_proxy_pipeline_commit(test_session(), fh, sizeof(fh), 0, 0,
					   NULL, &c);
	ck_assert_int_eq(ret, -EIO);
	ck_assert_int_eq(g_send_call_count, 1);
	ck_assert_uint_eq(listener_table_count(), 1);
	ck_assert(peek_stripe_dirty(fh, sizeof(fh), 5));

	free(data);
}
END_TEST

START_TEST(test_commit_multi_dirty_stops_on_first_failure)
{
	uint8_t fh[] = { TEST_FH_BYTE };
	uint8_t sid[PS_STATEID_OTHER_SIZE] = { 0xB5 };
	uint8_t *data = calloc(1, TG_STRIPE);
	struct ps_proxy_write_reply w;
	struct ps_proxy_commit_reply c;

	ck_assert_ptr_nonnull(data);

	/* WRITE stripe 0 (offset 0). */
	memset(&w, 0, sizeof(w));
	ck_assert_int_eq(ps_proxy_pipeline_write(test_session(), fh, sizeof(fh),
						 0, sid, 0, 0, data, TG_STRIPE,
						 NULL, &w),
			 0);
	/* WRITE stripe 1 (offset TG_STRIPE). */
	memset(&w, 0, sizeof(w));
	ck_assert_int_eq(ps_proxy_pipeline_write(test_session(), fh, sizeof(fh),
						 0, sid, TG_STRIPE, 0, data,
						 TG_STRIPE, NULL, &w),
			 0);

	/*
	 * Two fully-dirty stripes.  The walk dispatches
	 * ec_write_stripe_with_file for stripe 0 first; the strong
	 * override fails LAYOUTGET on that call, and the loop
	 * `break`s before attempting stripe 1.  Expected:
	 *   - ret == -EIO
	 *   - g_send_call_count == 1 (exactly the failing stripe's
	 *     LAYOUTGET attempt; stripe 1 NOT attempted)
	 *   - buffer stays (listener_table_count == 1)
	 *   - both dirty entries remain (we don't observe them
	 *     directly here -- the dirty-bitmap whitebox tests in
	 *     ps_write_buffer_rmw_test pin that contract).
	 */
	memset(&c, 0, sizeof(c));
	int ret = ps_proxy_pipeline_commit(test_session(), fh, sizeof(fh), 0, 0,
					   NULL, &c);
	ck_assert_int_eq(ret, -EIO);
	ck_assert_int_eq(g_send_call_count, 1);
	ck_assert_uint_eq(listener_table_count(), 1);

	free(data);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

static Suite *ps_proxy_pipeline_commit_suite(void)
{
	Suite *s = suite_create("ps_proxy_pipeline_commit");
	TCase *tc = tcase_create("flush");

	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_commit_no_buffer_returns_verifier);
	tcase_add_test(tc, test_commit_returns_same_verifier_as_writes);
	tcase_add_test(tc, test_commit_keeps_buffer_on_failure);
	tcase_add_test(tc, test_commit_attempt_uses_buffered_seqid);
	tcase_add_test(tc, test_commit_arg_validation);
	tcase_add_test(tc, test_commit_after_listener_stop_returns_eagain);
	tcase_add_test(tc, test_commit_partial_stripe_attempts_rmw_read);
	tcase_add_test(tc, test_commit_multi_dirty_stops_on_first_failure);
	tcase_add_test(tc, test_commit_range_intersects_dirty);
	tcase_add_test(tc, test_commit_range_excludes_dirty_skips_flush);
	tcase_add_test(tc, test_commit_range_zero_count_flushes_all);

	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	SRunner *sr = srunner_create(ps_proxy_pipeline_commit_suite());

	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return failed == 0 ? 0 : 1;
}
