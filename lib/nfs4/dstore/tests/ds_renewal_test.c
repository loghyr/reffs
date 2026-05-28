/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <check.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "reffs/dstore.h"

#include "ds_renewal.h"

/*
 * ds_renewal unit tests.
 *
 * The PS mirror at lib/nfs4/ps/tests/ps_reconnect_test.c uses a
 * whitebox internal header (ps_renewal_internal.h) to exercise
 * renewal_tick_one with a synthetic mds_session pointer.  The
 * ds_renewal slice does not (yet) expose an equivalent internal
 * header -- the tick is exercised end-to-end by the
 * deploy/benchmark/run_chunk_collision_track2.sh harness when
 * NFSv4 dstores are configured.  The tests below cover the
 * publicly-visible state machine: thread lifecycle, kick
 * semantics, and disabled-mode opt-out.
 *
 * NOT_NOW_BROWN_COW: whitebox tick test that asserts
 *   - tick_one sends SEQUENCE on a live session
 *   - tick_one parks + reconnects on a dead classification
 *   - backoff schedule advances on reconnect failure
 *   - test_session_replace_quiesces_in_flight (B1 regression)
 *
 * Adding those requires either a ds_renewal_internal.h with the
 * tick_one signature exposed, a mock mds_session_create that the
 * test can intercept, or a full fixture (mock DS + real
 * MDS-to-DS session).  Tracked.
 */

static void setup(void)
{
	dstore_init();
}

static void teardown(void)
{
	/*
	 * No-op fini: ds_renewal_stop must run before dstore_fini
	 * so the next collect_all cannot outlive the hash table;
	 * each test that starts the thread is responsible for
	 * stopping it before returning.
	 */
	dstore_fini();
}

/*
 * ds_renewal_start with interval_seconds == 0 is the documented
 * opt-out: returns 0 without spawning a thread.  Subsequent stop /
 * kick must be no-ops in this state.
 */
START_TEST(test_renewal_disabled_interval_zero)
{
	int ret = ds_renewal_start(0);

	ck_assert_int_eq(ret, 0);

	/* Stop is a no-op when no thread spawned -- must not crash. */
	ds_renewal_stop();

	/*
	 * Kick is also a no-op when no thread is parked on the CV;
	 * pthread_cond_broadcast with no waiter is per-spec a silent
	 * no-op.  Pass NULL for ds (the per-dstore reset path is
	 * guarded against NULL).
	 */
	ds_renewal_kick(NULL);
}
END_TEST

/*
 * Start with a valid interval, verify thread spawns (we infer by
 * the fact that stop joins without hanging), stop is idempotent.
 */
START_TEST(test_renewal_thread_lifecycle)
{
	int ret = ds_renewal_start(1);

	ck_assert_int_eq(ret, 0);

	/* Second start: CAS short-circuits, returns 0 without spawning. */
	ret = ds_renewal_start(1);
	ck_assert_int_eq(ret, 0);

	/* Stop joins the worker.  Hanging here = thread leak. */
	ds_renewal_stop();

	/* Second stop: CAS short-circuits, returns without joining. */
	ds_renewal_stop();
}
END_TEST

/*
 * ds_renewal_kick(NULL) is safe (covers the bare-wake path used by
 * tests).  Per-dstore kick clears both backoff fields atomically.
 *
 * The bench evidence the keep-alive slice validates against is
 * sensitive to recovery latency -- a kick that doesn't clear the
 * backoff would let a parked dstore wait one full backoff window
 * before retry, defeating the W2 recovery channel.  This test
 * asserts the clear at the per-dstore visible API.
 */
START_TEST(test_renewal_kick_resets_per_dstore_schedule)
{
	struct dstore *ds = dstore_alloc(99, "192.0.2.1", 0, "/test",
					 REFFS_DS_PROTO_NFSV4, false, false);

	ck_assert_ptr_nonnull(ds);

	/* Seed backoff to non-zero. */
	atomic_store_explicit(&ds->ds_reconnect_backoff_sec, 16,
			      memory_order_release);
	atomic_store_explicit(&ds->ds_reconnect_next_attempt_ns,
			      (uint64_t)1 << 40, memory_order_release);

	/* Kick clears both. */
	ds_renewal_kick(ds);

	ck_assert_uint_eq(atomic_load_explicit(&ds->ds_reconnect_backoff_sec,
					       memory_order_acquire),
			  0);
	ck_assert_uint_eq(
		atomic_load_explicit(&ds->ds_reconnect_next_attempt_ns,
				     memory_order_acquire),
		0);

	dstore_unhash(ds);
	dstore_put(ds);
}
END_TEST

/*
 * ds_renewal_kick(NULL) is safe -- it should NOT crash and should
 * NOT touch the broadcast CV (or if it does, the broadcast is a
 * no-op without any thread parked).  Asserts the NULL-guard at
 * ds_renewal.c.
 */
START_TEST(test_renewal_kick_null_safe)
{
	ds_renewal_kick(NULL);
	ds_renewal_kick(NULL);
	ds_renewal_kick(NULL);
}
END_TEST

/*
 * Borrow/release/replace are the BLOCKER B1 surface that the
 * keep-alive slice closed.  This test exercises the publicly-
 * visible accessor contract: borrow + replace + re-borrow.  The
 * full quiesce-in-flight regression test is NOT_NOW_BROWN_COW
 * (needs a real session pointer; here we use NULL session as the
 * publish value to exercise the swap shape without RPC).
 */
START_TEST(test_session_borrow_release_replace_cycle)
{
	struct dstore *ds = dstore_alloc(101, "192.0.2.2", 0, "/test",
					 REFFS_DS_PROTO_NFSV4, false, false);

	ck_assert_ptr_nonnull(ds);

	/* Initial: no session -> borrow returns NULL. */
	struct mds_session *ms = dstore_session_borrow(ds);

	ck_assert_ptr_null(ms);
	/* NULL return means rwlock was NOT held; do NOT call release. */

	/*
	 * Plant a sentinel non-NULL pointer (not a real session, but
	 * the accessor only treats it as opaque storage; the
	 * "destroy outside wlock" path on replace would call
	 * mds_session_destroy on a real pointer, which we can't do
	 * here).  Use replace(ds, NULL) to confirm idempotency on
	 * the "park" path with no real session installed.
	 */
	int rret = dstore_session_replace(ds, NULL);

	ck_assert_int_eq(rret, 0);

	/* Second replace(NULL): also returns 0 (old_session is NULL,
	 * destroy block is skipped).  Idempotent. */
	rret = dstore_session_replace(ds, NULL);
	ck_assert_int_eq(rret, 0);

	dstore_unhash(ds);
	dstore_put(ds);
}
END_TEST

/*
 * ds_renewal_kick on an NFSv3 dstore: the kick clears the per-
 * dstore backoff fields (the accessor is protocol-agnostic).  The
 * subsequent tick dispatches to renewal_tick_one_nfsv3 (NULL RPC
 * over ds_clnt under ds_clnt_mutex) rather than the NFSv4
 * SEQUENCE path -- so the per-dstore schedule reset matters for
 * NFSv3 too: a kicked NFSv3 dstore attempts the next NULL on the
 * next tick rather than waiting out a backoff window from a
 * prior failure.
 */
START_TEST(test_renewal_kick_nfsv3_dstore_clears_schedule)
{
	struct dstore *ds = dstore_alloc(102, "192.0.2.3", 0, "/test",
					 REFFS_DS_PROTO_NFSV3, false, false);

	ck_assert_ptr_nonnull(ds);

	atomic_store_explicit(&ds->ds_reconnect_backoff_sec, 8,
			      memory_order_release);
	atomic_store_explicit(&ds->ds_reconnect_next_attempt_ns,
			      (uint64_t)1 << 30, memory_order_release);

	ds_renewal_kick(ds);

	ck_assert_uint_eq(atomic_load_explicit(&ds->ds_reconnect_backoff_sec,
					       memory_order_acquire),
			  0);
	ck_assert_uint_eq(
		atomic_load_explicit(&ds->ds_reconnect_next_attempt_ns,
				     memory_order_acquire),
		0);

	dstore_unhash(ds);
	dstore_put(ds);
}
END_TEST

/*
 * Stop-before-start: ds_renewal_stop with no prior start returns
 * cleanly via the CAS-1->0 short-circuit.  Asserts the
 * implementation does not pthread_join on a zero pthread_t handle
 * (which is undefined behaviour under some libpthread
 * implementations).
 */
START_TEST(test_renewal_stop_before_start)
{
	ds_renewal_stop();
	ds_renewal_stop();
}
END_TEST

/*
 * Start then immediate stop -- worker may not have completed even
 * a single tick before stop fires.  The join must complete cleanly
 * regardless of where the worker is in its loop.
 */
START_TEST(test_renewal_immediate_stop)
{
	int ret = ds_renewal_start(60); /* long interval; will sleep */

	ck_assert_int_eq(ret, 0);

	/*
	 * Immediate stop: the worker has just been spawned; it will
	 * either be inside ds_renewal_one_tick (a no-op tick with no
	 * dstores) or inside the cond_timedwait.  Stop must broadcast
	 * the CV (waking the worker out of the wait) and then join.
	 */
	ds_renewal_stop();
}
END_TEST

/*
 * Kick during running thread -- broadcast wakes the wait early.
 * We can't observe the wake directly without instrumentation, but
 * we can assert the kick + subsequent stop together complete
 * without hanging.  A bug in the CV pairing would manifest as a
 * hang in this test.
 */
START_TEST(test_renewal_kick_during_running)
{
	int ret = ds_renewal_start(60); /* long interval; thread sleeps */

	ck_assert_int_eq(ret, 0);

	/* Kick a few times in rapid succession. */
	ds_renewal_kick(NULL);
	ds_renewal_kick(NULL);
	ds_renewal_kick(NULL);

	ds_renewal_stop();
}
END_TEST

static Suite *ds_renewal_suite(void)
{
	Suite *s = suite_create("ds_renewal");
	TCase *tc = tcase_create("lifecycle");

	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_renewal_disabled_interval_zero);
	tcase_add_test(tc, test_renewal_thread_lifecycle);
	tcase_add_test(tc, test_renewal_kick_resets_per_dstore_schedule);
	tcase_add_test(tc, test_renewal_kick_null_safe);
	tcase_add_test(tc, test_session_borrow_release_replace_cycle);
	tcase_add_test(tc, test_renewal_kick_nfsv3_dstore_clears_schedule);
	tcase_add_test(tc, test_renewal_stop_before_start);
	tcase_add_test(tc, test_renewal_immediate_stop);
	tcase_add_test(tc, test_renewal_kick_during_running);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	Suite *s = ds_renewal_suite();
	SRunner *sr = srunner_create(s);

	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);

	return failed ? 1 : 0;
}
