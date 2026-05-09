/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Whitebox tests for ps_proxy_send_with_kick (lib/nfs4/ps/ps_proxy_ops.c)
 * -- the worker-side compound-send wrapper that wakes the renewal
 * thread on a session-killer wire status.
 *
 * The production mds_compound_send_with_auth (declared weak in
 * lib/nfs4/client/mds_compound.c) is overridden here so each test
 * controls the (return value, SEQUENCE sr_status) tuple the wrapper
 * sees, then the test asserts whether ps_listener_kick_reconnect
 * fired by inspecting the listener's reconnect schedule:
 *   - kick fired  -> backoff_sec == 0 && next_attempt_ns == 0
 *   - no kick     -> the schedule keeps the prior values
 *
 * Companion of ps_reconnect_test.c (mechanism + its own backoff /
 * borrow / kick-helper tests).  Lives in its own binary so the
 * strong-override of mds_compound_send_with_auth doesn't interfere
 * with other PS tests in the same suite.
 */

#include <check.h>
#include <errno.h>
#include <rpc/rpc.h>
#include <rpc/auth_unix.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "nfsv42_xdr.h"
#include "reffs/settings.h"

#include "ec_client.h"
#include "ps_proxy_ops_internal.h"
#include "ps_state.h"

/* ------------------------------------------------------------------ */
/* Strong override of mds_compound_send_with_auth.  The production    */
/* version in lib/nfs4/client/mds_compound.c is __attribute__((weak)) */
/* so this file's strong symbol wins at link time.                     */
/* ------------------------------------------------------------------ */

static int g_mock_ret;
static nfsstat4 g_mock_sr_status;
static bool g_mock_seq_present;

int mds_compound_send_with_auth(struct mds_compound *mc,
				struct mds_session *ms __attribute__((unused)),
				const struct authunix_parms *creds
				__attribute__((unused)))
{
	/*
	 * Construct a minimal SEQUENCE result so the wrapper's peek at
	 * resarray[0].opsequence.sr_status sees what the test wants.
	 * The wrapper guards on resop == OP_SEQUENCE so we set that too.
	 *
	 * mc->mc_res.resarray.resarray_val is owned by the test (static
	 * storage); the wrapper doesn't free it, and mds_compound_fini
	 * is never called here because we don't go through compound_init.
	 *
	 * DO NOT add an mds_compound_fini call or any xdr_free on
	 * &mc->mc_res to a future test: libtirpc's xdr_free would call
	 * free() on the static stub_results buffer -- undefined
	 * behaviour.  Tests here own the wrapper's view of mc and must
	 * stay synthetic end-to-end.
	 */
	static nfs_resop4 stub_results[1];

	memset(stub_results, 0, sizeof(stub_results));
	if (g_mock_seq_present) {
		stub_results[0].resop = OP_SEQUENCE;
		stub_results[0].nfs_resop4_u.opsequence.sr_status =
			g_mock_sr_status;
		mc->mc_res.resarray.resarray_val = stub_results;
		mc->mc_res.resarray.resarray_len = 1;
	} else {
		mc->mc_res.resarray.resarray_val = NULL;
		mc->mc_res.resarray.resarray_len = 0;
	}
	return g_mock_ret;
}

static void mock_reset(void)
{
	g_mock_ret = 0;
	g_mock_sr_status = NFS4_OK;
	g_mock_seq_present = false;
}

/* ------------------------------------------------------------------ */
/* Test fixture.                                                       */
/* ------------------------------------------------------------------ */

static void setup(void)
{
	ps_state_init();
	mock_reset();
}

static void teardown(void)
{
	ps_state_fini();
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

/*
 * Pre-arm the listener's reconnect schedule with non-zero values so
 * the assertion "kick fired -> schedule cleared" is meaningful.
 * Without arming, the values are already 0 and we couldn't tell a
 * no-op from a successful kick.
 */
static void arm_schedule(uint32_t listener_id, uint64_t deadline_ns,
			 uint32_t backoff_sec)
{
	struct ps_listener_state *pls =
		(struct ps_listener_state *)ps_state_find(listener_id);

	ck_assert_ptr_nonnull(pls);
	atomic_store_explicit(&pls->pls_reconnect_next_attempt_ns, deadline_ns,
			      memory_order_release);
	atomic_store_explicit(&pls->pls_reconnect_backoff_sec, backoff_sec,
			      memory_order_release);
}

static bool schedule_was_cleared(uint32_t listener_id)
{
	const struct ps_listener_state *pls = ps_state_find(listener_id);

	ck_assert_ptr_nonnull(pls);
	uint64_t d = atomic_load_explicit(&pls->pls_reconnect_next_attempt_ns,
					  memory_order_acquire);
	uint32_t b = atomic_load_explicit(&pls->pls_reconnect_backoff_sec,
					  memory_order_acquire);
	return d == 0 && b == 0;
}

/* ------------------------------------------------------------------ */
/* Tests.                                                              */
/* ------------------------------------------------------------------ */

/*
 * Untagged session (ms_kick_listener_id == 0): the wrapper must NOT
 * fire a kick regardless of the underlying send result.  This is the
 * ec_demo / dstore-MDS-to-DS path: PS infrastructure isn't involved
 * and there's no listener to wake.
 */
START_TEST(test_kick_skipped_when_session_untagged)
{
	struct reffs_proxy_mds_config c = make_cfg(70);

	ck_assert_int_eq(ps_state_register(&c), 0);
	arm_schedule(70, 1234567890ULL, 32);

	struct mds_session ms;

	memset(&ms, 0, sizeof(ms));
	atomic_store_explicit(&ms.ms_kick_listener_id, 0, memory_order_relaxed);

	struct mds_compound mc;

	memset(&mc, 0, sizeof(mc));

	g_mock_ret = -EREMOTEIO;
	g_mock_sr_status = NFS4ERR_BADSESSION;
	g_mock_seq_present = true;

	int ret = ps_proxy_send_with_kick(&mc, &ms, NULL);

	ck_assert_int_eq(ret, -EREMOTEIO);
	/* Schedule untouched -- no kick fired. */
	ck_assert(!schedule_was_cleared(70));
}
END_TEST

/*
 * Tagged session, BADSESSION sr_status: the wrapper must fire the
 * kick on the tagged listener, clearing its schedule.  This is the
 * core promised behaviour: a worker observing a session-killer wakes
 * the renewal thread and zeros the backoff so the next tick attempts
 * a reconnect immediately.
 */
START_TEST(test_kick_fires_on_badsession_status)
{
	struct reffs_proxy_mds_config c = make_cfg(71);

	ck_assert_int_eq(ps_state_register(&c), 0);
	arm_schedule(71, 9999999999ULL, 16);

	struct mds_session ms;

	memset(&ms, 0, sizeof(ms));
	atomic_store_explicit(&ms.ms_kick_listener_id, 71,
			      memory_order_relaxed);

	struct mds_compound mc;

	memset(&mc, 0, sizeof(mc));

	g_mock_ret = -EREMOTEIO;
	g_mock_sr_status = NFS4ERR_BADSESSION;
	g_mock_seq_present = true;

	int ret = ps_proxy_send_with_kick(&mc, &ms, NULL);

	ck_assert_int_eq(ret, -EREMOTEIO);
	ck_assert(schedule_was_cleared(71));
}
END_TEST

/*
 * Tagged session, connection-killer errno (no SEQUENCE result): the
 * wrapper must still fire the kick.  Wire-level failures (-EPIPE,
 * -ECONNRESET, etc.) classify dead in ps_session_is_dead's errno
 * branch, with sr_status defaulted to NFS4_OK.
 */
START_TEST(test_kick_fires_on_connection_errno)
{
	struct reffs_proxy_mds_config c = make_cfg(72);

	ck_assert_int_eq(ps_state_register(&c), 0);
	arm_schedule(72, 5000000000ULL, 8);

	struct mds_session ms;

	memset(&ms, 0, sizeof(ms));
	atomic_store_explicit(&ms.ms_kick_listener_id, 72,
			      memory_order_relaxed);

	struct mds_compound mc;

	memset(&mc, 0, sizeof(mc));

	g_mock_ret = -EPIPE;
	g_mock_sr_status = NFS4_OK; /* irrelevant */
	g_mock_seq_present = false; /* no decoded compound */

	int ret = ps_proxy_send_with_kick(&mc, &ms, NULL);

	ck_assert_int_eq(ret, -EPIPE);
	ck_assert(schedule_was_cleared(72));
}
END_TEST

/*
 * Tagged session, NFS4ERR_DELAY sr_status: the wrapper must NOT fire
 * the kick.  DELAY is per-op transient; the session is healthy.
 * Re-issuing PROXY_REGISTRATION on a still-good session would be
 * wasted work and would log a misleading "session dead" event.
 */
START_TEST(test_kick_skipped_on_per_op_transient)
{
	struct reffs_proxy_mds_config c = make_cfg(73);

	ck_assert_int_eq(ps_state_register(&c), 0);
	arm_schedule(73, 7000000000ULL, 4);

	struct mds_session ms;

	memset(&ms, 0, sizeof(ms));
	atomic_store_explicit(&ms.ms_kick_listener_id, 73,
			      memory_order_relaxed);

	struct mds_compound mc;

	memset(&mc, 0, sizeof(mc));

	g_mock_ret = -EREMOTEIO;
	g_mock_sr_status = NFS4ERR_DELAY;
	g_mock_seq_present = true;

	int ret = ps_proxy_send_with_kick(&mc, &ms, NULL);

	ck_assert_int_eq(ret, -EREMOTEIO);
	/* Schedule preserved -- DELAY is not a session killer. */
	ck_assert(!schedule_was_cleared(73));
}
END_TEST

/*
 * Tagged session, successful send (ret == 0): the wrapper must NOT
 * fire the kick.  Healthy traffic must not perturb the schedule;
 * otherwise every successful op would zero the backoff that the
 * renewal thread legitimately armed after a prior failed reconnect.
 */
START_TEST(test_kick_skipped_on_success)
{
	struct reffs_proxy_mds_config c = make_cfg(74);

	ck_assert_int_eq(ps_state_register(&c), 0);
	arm_schedule(74, 3000000000ULL, 2);

	struct mds_session ms;

	memset(&ms, 0, sizeof(ms));
	atomic_store_explicit(&ms.ms_kick_listener_id, 74,
			      memory_order_relaxed);

	struct mds_compound mc;

	memset(&mc, 0, sizeof(mc));

	g_mock_ret = 0;
	g_mock_sr_status = NFS4_OK;
	g_mock_seq_present = true;

	int ret = ps_proxy_send_with_kick(&mc, &ms, NULL);

	ck_assert_int_eq(ret, 0);
	ck_assert(!schedule_was_cleared(74));
}
END_TEST

static Suite *ps_proxy_kick_suite(void)
{
	Suite *s = suite_create("ps_proxy_kick");
	TCase *tc = tcase_create("kick_wiring");

	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_kick_skipped_when_session_untagged);
	tcase_add_test(tc, test_kick_fires_on_badsession_status);
	tcase_add_test(tc, test_kick_fires_on_connection_errno);
	tcase_add_test(tc, test_kick_skipped_on_per_op_transient);
	tcase_add_test(tc, test_kick_skipped_on_success);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	Suite *s = ps_proxy_kick_suite();
	SRunner *sr = srunner_create(s);

	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return failed ? 1 : 0;
}
