/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * Per-listener lifecycle for PS Phase 4a (slice 4a.2a): the
 * RUNNING -> DRAINING -> STOPPED state machine, the listener-borrow
 * contract gate, ps_listener_stop idempotency, and the quiesce-wait
 * for in-flight ops to drain.
 *
 * Test surface intentionally separate from ps_write_buffer_test so a
 * regression in either domain has a clearly-attributable suite.
 *
 * The shutdown-quiesce-deterministic test uses the test-only hook
 * declared in ps_write_buffer_internal.h (ps_test_hook_pre_state_load)
 * to insert a controlled pause between enter_quiesce_or_bail's
 * fetch_add and the pls_state load.  No race-stress -- the test
 * synchronises via a condvar so failure is deterministic.
 */

#include <check.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
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
	atomic_store_explicit(&ps_test_hook_pre_state_load, NULL,
			      memory_order_relaxed);
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

static enum ps_listener_state_kind state_of(uint32_t id)
{
	struct ps_listener_state *pls = mut_listener(id);

	return atomic_load_explicit(&pls->pls_state, memory_order_acquire);
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

START_TEST(test_listener_runs_after_register)
{
	struct reffs_proxy_mds_config cfg = make_cfg(1);

	ck_assert_int_eq(ps_state_register(&cfg), 0);
	ck_assert_int_eq(state_of(1), PS_LISTENER_RUNNING);
}
END_TEST

START_TEST(test_listener_stop_transitions_to_stopped)
{
	struct reffs_proxy_mds_config cfg = make_cfg(2);

	ck_assert_int_eq(ps_state_register(&cfg), 0);
	ck_assert_int_eq(state_of(2), PS_LISTENER_RUNNING);

	ck_assert_int_eq(ps_listener_stop(2), 0);
	ck_assert_int_eq(state_of(2), PS_LISTENER_STOPPED);
}
END_TEST

START_TEST(test_listener_stop_idempotent)
{
	struct reffs_proxy_mds_config cfg = make_cfg(3);

	ck_assert_int_eq(ps_state_register(&cfg), 0);
	ck_assert_int_eq(ps_listener_stop(3), 0);
	ck_assert_int_eq(ps_listener_stop(3), 0);
	ck_assert_int_eq(state_of(3), PS_LISTENER_STOPPED);
}
END_TEST

START_TEST(test_listener_stop_unknown_returns_enoent)
{
	ck_assert_int_eq(ps_listener_stop(/* never registered */ 999), -ENOENT);
}
END_TEST

START_TEST(test_borrow_returns_null_when_draining)
{
	/*
	 * Even with no session attached, the listener-borrow gate
	 * must trip on pls_state != RUNNING.  After ps_listener_stop
	 * the borrow returns NULL regardless of whether a session
	 * pointer is or was set.  Pins the design's Listener-borrow
	 * contract section.
	 */
	struct reffs_proxy_mds_config cfg = make_cfg(4);

	ck_assert_int_eq(ps_state_register(&cfg), 0);
	ck_assert_int_eq(ps_listener_stop(4), 0);
	ck_assert_ptr_null(ps_listener_session_borrow(4));
}
END_TEST

START_TEST(test_enter_quiesce_bails_when_draining)
{
	struct reffs_proxy_mds_config cfg = make_cfg(5);
	struct ps_listener_state *pls;

	ck_assert_int_eq(ps_state_register(&cfg), 0);
	pls = mut_listener(5);
	ck_assert_int_eq(ps_listener_stop(5), 0);

	/* After STOPPED, enter_quiesce must bail and leave the
	 * counter unchanged (its own internal decrement already ran).
	 */
	ck_assert(!ps_write_buffer_enter_quiesce_or_bail(pls));
	ck_assert_uint_eq(atomic_load_explicit(&pls->pls_active_buffer_refs,
					       memory_order_acquire),
			  0);
}
END_TEST

/*
 * Deterministic quiesce-wait test.  Uses the
 * ps_test_hook_pre_state_load hook to insert a controlled pause
 * between fetch_add and the state load inside enter_quiesce_or_bail;
 * during the pause, a second thread runs ps_listener_stop.  The
 * primary thread observes DRAINING on the post-add load and
 * leave_quiesce's -- ps_listener_stop wakes from cv_wait, drains,
 * and returns.
 *
 * Synchronisation primitives:
 *   - g_hook_inside    set by the hook so the test knows the
 *                      primary is parked between fetch_add and
 *                      state load.
 *   - g_proceed        signalled after ps_listener_stop has
 *                      transitioned to DRAINING and is in cv_wait.
 *
 * Failure modes captured:
 *   - hook never fires            -> ck_assert(g_hook_inside) fails
 *   - state load misses DRAINING  -> enter returns true, ck_assert
 *                                    fails on the bail-expected branch
 *   - cv-predicate lost-wakeup    -> ps_listener_stop never returns
 *                                    (libcheck per-test timeout fires)
 */
static pthread_mutex_t g_hook_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_hook_cv = PTHREAD_COND_INITIALIZER;
static bool g_hook_inside;
static bool g_proceed;

static void hook_park_until_drain(void)
{
	pthread_mutex_lock(&g_hook_mu);
	g_hook_inside = true;
	pthread_cond_signal(&g_hook_cv);
	while (!g_proceed)
		pthread_cond_wait(&g_hook_cv, &g_hook_mu);
	pthread_mutex_unlock(&g_hook_mu);
}

static void *enter_thread(void *arg)
{
	struct ps_listener_state *pls = arg;
	bool *result = malloc(sizeof(*result));

	rcu_register_thread();
	*result = ps_write_buffer_enter_quiesce_or_bail(pls);
	rcu_unregister_thread();
	return result;
}

/*
 * test_listener_shutdown_quiesce_deterministic
 *
 * Drives ps_listener_stop to its cv_wait predicate and proves the
 * cv-wake mechanism works.
 *
 * Sequence:
 *   1. Spawn primary thread which parks inside enter_quiesce via the
 *      pre_state_load hook, holding pls_active_buffer_refs at 1.
 *   2. Spawn stopper thread which calls ps_listener_stop.  Stopper
 *      wins the RUNNING->DRAINING CAS, observes counter > 0, enters
 *      pthread_cond_wait on pls_drain_cv.
 *   3. Yield repeatedly so the stopper reaches cv_wait (no precise
 *      hook here; the test relies on libcheck's per-test timeout to
 *      catch a lost-wakeup hang -- if leave_quiesce's mutex+signal
 *      is broken, the stopper sleeps forever).
 *   4. Release the primary.  Primary observes DRAINING on the
 *      post-fetch_add load, leave_quiesce decrements counter to 0,
 *      takes pls_drain_mutex, broadcasts.
 *   5. Stopper wakes from cv_wait, sees counter == 0, destroys the
 *      table, stores STOPPED.
 *   6. Join both threads, assert state == STOPPED.
 *
 * The stopper-blocks-then-wakes step is what
 * test_listener_toctou_state_check_after_increment did NOT exercise
 * (that test stored DRAINING directly without going through the
 * cv_wait path).
 */
static void *stopper_thread(void *arg)
{
	uint32_t *id = arg;
	int *result = malloc(sizeof(*result));

	rcu_register_thread();
	*result = ps_listener_stop(*id);
	rcu_unregister_thread();
	return result;
}

START_TEST(test_listener_shutdown_quiesce_deterministic)
{
	struct reffs_proxy_mds_config cfg = make_cfg(7);
	uint32_t listener_id = 7;
	struct ps_listener_state *pls;
	pthread_t primary_tid, stopper_tid;
	bool *primary_result;
	int *stopper_result;
	void *retval;

	ck_assert_int_eq(ps_state_register(&cfg), 0);
	pls = mut_listener(listener_id);

	/* Arm hook + spawn primary; primary parks holding counter at 1. */
	g_hook_inside = false;
	g_proceed = false;
	atomic_store_explicit(&ps_test_hook_pre_state_load,
			      hook_park_until_drain, memory_order_relaxed);
	ck_assert_int_eq(pthread_create(&primary_tid, NULL, enter_thread, pls),
			 0);
	pthread_mutex_lock(&g_hook_mu);
	while (!g_hook_inside)
		pthread_cond_wait(&g_hook_cv, &g_hook_mu);
	pthread_mutex_unlock(&g_hook_mu);

	/*
	 * Counter is now 1.  Spawn stopper; it should win the CAS,
	 * see counter > 0, enter cv_wait.
	 *
	 * Spin-wait until we OBSERVE the CAS success (state ==
	 * DRAINING) rather than yielding a fixed number of times --
	 * scheduling latency on a busy CI host can easily blow past
	 * a 100-yield budget.  Once DRAINING is observed, yield a
	 * few more times so the stopper has a chance to reach
	 * cv_wait; if it hasn't reached cv_wait yet, leave_quiesce
	 * + broadcast still wakes any future cv_wait.
	 */
	ck_assert_int_eq(pthread_create(&stopper_tid, NULL, stopper_thread,
					&listener_id),
			 0);
	while (atomic_load_explicit(&pls->pls_state, memory_order_acquire) !=
	       PS_LISTENER_DRAINING)
		sched_yield();
	for (int i = 0; i < 8; i++)
		sched_yield();

	/* Verified: stopper won CAS, state is DRAINING, counter still 1. */

	pthread_mutex_lock(&g_hook_mu);
	g_proceed = true;
	pthread_cond_broadcast(&g_hook_cv);
	pthread_mutex_unlock(&g_hook_mu);

	pthread_join(primary_tid, &retval);
	primary_result = retval;
	ck_assert(!(*primary_result));
	free(primary_result);

	pthread_join(stopper_tid, &retval);
	stopper_result = retval;
	ck_assert_int_eq(*stopper_result, 0);
	free(stopper_result);

	ck_assert_int_eq(state_of(listener_id), PS_LISTENER_STOPPED);

	atomic_store_explicit(&ps_test_hook_pre_state_load, NULL,
			      memory_order_relaxed);
}
END_TEST

/*
 * Note: an earlier version of this file had a separate
 * test_listener_toctou_state_check_after_increment that stored
 * PS_LISTENER_DRAINING directly on the listener state, bypassing
 * ps_listener_stop's CAS-elect protocol.  After the CAS-elect
 * landed (verdict-1 BLOCKER B1), direct DRAINING stores leave the
 * listener with no destroyer thread, so a subsequent ps_listener_stop
 * spin-waits for STOPPED forever.  The TOCTOU coverage is already
 * provided by test_listener_shutdown_quiesce_deterministic above
 * (primary parks in the pre_state_load hook; stopper runs
 * ps_listener_stop which is the only legal DRAINING writer).
 */

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

static Suite *ps_listener_lifecycle_suite(void)
{
	Suite *s = suite_create("ps_listener_lifecycle");
	TCase *tc = tcase_create("lifecycle");

	tcase_set_timeout(tc, 5);
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_listener_runs_after_register);
	tcase_add_test(tc, test_listener_stop_transitions_to_stopped);
	tcase_add_test(tc, test_listener_stop_idempotent);
	tcase_add_test(tc, test_listener_stop_unknown_returns_enoent);
	tcase_add_test(tc, test_borrow_returns_null_when_draining);
	tcase_add_test(tc, test_enter_quiesce_bails_when_draining);
	tcase_add_test(tc, test_listener_shutdown_quiesce_deterministic);

	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	SRunner *sr = srunner_create(ps_listener_lifecycle_suite());

	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return failed == 0 ? 0 : 1;
}
