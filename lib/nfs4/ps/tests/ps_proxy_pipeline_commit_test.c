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
 * The shim drives ec_write_codec_with_file -> mds_layout_get for
 * the flush; mds_layout_get goes through mds_compound_send_with_auth
 * which we strong-override here to return -EIO.  This short-
 * circuits the codec before it touches DS state, so the tests can
 * verify the commit-side contract -- "did the flush attempt fire,
 * did the buffer stay around on failure, did no-op COMMIT return
 * the right verifier" -- without standing up a full mock MDS+DS.
 *
 * The happy-path success case (codec returns 0, buffer is dropped)
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
	return -EIO; /* bail the codec before any DS work */
}

/* ------------------------------------------------------------------ */
/* Fixture                                                             */
/* ------------------------------------------------------------------ */

#define TEST_LISTENER_ID 11
#define TEST_FH_BYTE 0xC0

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
	uint8_t data[] = { 'x' };
	struct ps_proxy_write_reply w;
	struct ps_proxy_commit_reply c;

	memset(&w, 0, sizeof(w));
	int ret = ps_proxy_pipeline_write(test_session(), fh, sizeof(fh), 0,
					  sid, 0, 0, data, sizeof(data), NULL,
					  &w);
	ck_assert_int_eq(ret, 0);

	memset(&c, 0, sizeof(c));
	ret = ps_proxy_pipeline_commit(test_session(), fh, sizeof(fh), 0, 0,
				       NULL, &c);
	/*
	 * The mock fails LAYOUTGET so commit returns -EIO, but the
	 * reply.verifier is populated only on the success path.
	 * Repeat WITHOUT a prior WRITE (no buffer) and compare the
	 * verifier the no-op path emits.  Both paths derive from the
	 * same pls_boot_gen, so they MUST match.
	 */
	ck_assert_int_eq(ret, -EIO);

	memset(&c, 0, sizeof(c));
	uint8_t other_fh[] = { 0xCA };

	ret = ps_proxy_pipeline_commit(test_session(), other_fh,
				       sizeof(other_fh), 0, 0, NULL, &c);
	ck_assert_int_eq(ret, 0);
	ck_assert_int_eq(memcmp(c.verifier, w.verifier, PS_PROXY_VERIFIER_SIZE),
			 0);
}
END_TEST

START_TEST(test_commit_keeps_buffer_on_failure)
{
	uint8_t fh[] = { TEST_FH_BYTE };
	uint8_t sid[PS_STATEID_OTHER_SIZE] = { 0xB2 };
	uint8_t data[] = { 'y' };
	struct ps_proxy_write_reply w;
	struct ps_proxy_commit_reply c;

	memset(&w, 0, sizeof(w));
	ck_assert_int_eq(ps_proxy_pipeline_write(test_session(), fh, sizeof(fh),
						 0, sid, 0, 0, data,
						 sizeof(data), NULL, &w),
			 0);
	ck_assert_uint_eq(listener_table_count(), 1);

	memset(&c, 0, sizeof(c));
	int ret = ps_proxy_pipeline_commit(test_session(), fh, sizeof(fh), 0, 0,
					   NULL, &c);

	ck_assert_int_eq(ret, -EIO);
	/* Buffer must remain so the client can retry COMMIT. */
	ck_assert_uint_eq(listener_table_count(), 1);
	/* And the upstream LAYOUTGET was attempted exactly once. */
	ck_assert_int_eq(g_send_call_count, 1);
}
END_TEST

START_TEST(test_commit_attempt_uses_buffered_seqid)
{
	uint8_t fh[] = { TEST_FH_BYTE };
	uint8_t sid[PS_STATEID_OTHER_SIZE] = { 0xB3 };
	uint8_t data[] = { 'z' };
	struct ps_proxy_write_reply w;
	struct ps_proxy_commit_reply c;
	struct ps_listener_state *pls;
	struct ps_write_buffer *buf;

	memset(&w, 0, sizeof(w));
	ck_assert_int_eq(ps_proxy_pipeline_write(test_session(), fh, sizeof(fh),
						 /* stateid_seqid */ 42, sid, 0,
						 0, data, sizeof(data), NULL,
						 &w),
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
	 * Now drive the actual commit attempt.  We don't have a way
	 * to inspect the LAYOUTGET stateid that mds_layout_get sends
	 * (the strong override fires after the compound is built),
	 * so this test pins the buffer-side seqid stash and trusts
	 * the codec call path -- a regression that stops storing
	 * seqid would fail the ck_assert above and surface here.
	 */
	memset(&c, 0, sizeof(c));
	int ret = ps_proxy_pipeline_commit(test_session(), fh, sizeof(fh), 0, 0,
					   NULL, &c);
	ck_assert_int_eq(ret, -EIO);
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
