/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Buffer-fill behaviour of ps_proxy_pipeline_write (PS Phase 4a
 * slice 4a.2b).  These tests pin the contract clients see across
 * sequential, sparse, and overwrite WRITE patterns -- without
 * involving any upstream MDS / DS round-trip.  The COMMIT-time
 * flush is slice 4a.2c.
 *
 * Tests reach into struct ps_write_buffer directly (via the
 * whitebox internal header) to verify the bytes landed where
 * expected.  No mds_compound_send_with_auth override needed:
 * pipeline_write never calls upstream.
 */

#include <check.h>
#include <errno.h>
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
