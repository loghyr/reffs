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
#include "reffs/dstore_ops.h"
#include "reffs/time.h"

#include "ds_renewal.h"
#include "ds_renewal_internal.h"
#include "nfs4_test_harness.h"

extern const struct dstore_ops dstore_ops_local;

/*
 * ds_renewal unit tests.
 *
 * Mirrors the lib/nfs4/ps/tests/ps_reconnect_test.c whitebox
 * pattern: the renewal-tick dispatcher is exposed via
 * ds_renewal_internal.h so tests invoke renewal_tick_one()
 * directly with synthesised dstores and assert on the
 * struct renewal_tick_ctx counters that record which branch of
 * the decision tree fired.
 *
 * Coverage matrix:
 *
 *   - Lifecycle: start / stop / kick API (test_renewal_disabled,
 *     thread_lifecycle, kick_null_safe, kick_resets_per_dstore_schedule,
 *     kick_nfsv3_dstore_clears_schedule, stop_before_start,
 *     immediate_stop, kick_during_running).
 *
 *   - Borrow / release / replace (test_session_borrow_release_replace_cycle):
 *     BLOCKER B1 surface from the keep-alive slice; idempotent
 *     replace(NULL).
 *
 *   - Tick decision tree (whitebox via internal header):
 *       * test_tick_local_skip                 -- local dstore takes
 *         the no-op fast path; ctx->skipped_local++.
 *       * test_tick_nfsv3_in_backoff           -- NFSv3 dstore with
 *         a future next_attempt_ns deadline never issues the NULL
 *         RPC; ctx->reconnect_skipped_backoff++.
 *       * test_tick_nfsv4_no_session_in_backoff -- NFSv4 dstore with
 *         no session and a future deadline records both
 *         skipped_no_session++ and reconnect_skipped_backoff++ and
 *         does not call ds_session_create.
 *
 *   The "active RPC" branches (live SEQUENCE renewal, dead-session
 *   reconnect, NFSv3 NULL fail -> dstore_reconnect, backoff
 *   schedule advance after a failed connect) are NOT_NOW_BROWN_COW
 *   here -- they require either a mock CLIENT* / mock
 *   mds_session_create or a real DS fixture.  The chunk-collision
 *   Track 2 bench (deploy/benchmark/run_chunk_collision_track2.sh)
 *   exercises them end-to-end and is the canonical signal until a
 *   mocked fixture lands.
 */

/*
 * The check-fixture setup/teardown brackets each test, but the
 * RCU registration + trace init + nfs4 protocol register happen
 * once per process inside reffs_test_run_suite (called via
 * nfs4_test_run() in main).  We follow the dstore_test.c pattern
 * for consistency: per-test nfs4_test_setup()/teardown() reaches
 * through to the global init the first time and is idempotent on
 * subsequent calls.
 *
 * The bare main() that ran srunner_create + srunner_run_all
 * directly (without the harness) leaked every dstore_alloc'd
 * entry under -fsanitize=address: dstore_release queues
 * call_rcu(dstore_free_rcu), and without rcu_register_thread the
 * callback never fires.  rcu_barrier in dstore_fini does nothing
 * if the calling thread is unregistered.
 */
static void setup(void)
{
	nfs4_test_setup();
	dstore_init();
}

static void teardown(void)
{
	/*
	 * dstore_fini drains the hash table and rcu_barriers pending
	 * call_rcu callbacks.  Each test that starts the renewal thread
	 * is responsible for ds_renewal_stop before returning so the
	 * worker is joined before dstore_fini destroys the hash.
	 */
	dstore_fini();
	nfs4_test_teardown();
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

	/*
	 * dstore_alloc returns refcount=2 (one for the hash table, one
	 * for the caller -- see dstore.c:565-566).  Drop the caller ref;
	 * dstore_fini's dstore_unload_all walks the hash and drops the
	 * other one.  The earlier "unhash + put" pattern leaked because
	 * unhash removes from the hash table without decrementing the
	 * refcount, so put then dropped only the caller ref and the
	 * orphaned hash ref kept the dstore alive past dstore_fini.
	 */
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

	/* See comment in test_renewal_kick_resets_per_dstore_schedule
	 * for the dstore_alloc refcount-2 convention. */
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

	/* See comment in test_renewal_kick_resets_per_dstore_schedule
	 * for the dstore_alloc refcount-2 convention. */
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

/*
 * Whitebox: local dstore takes the early no-op path in
 * renewal_tick_one.  dstore_alloc with address="127.0.0.1" selects
 * dstore_ops_local; the dispatcher recognises the vtable and bumps
 * ctx->skipped_local without touching any other branch.  This is
 * the path combined-mode reffsd takes: no socket to keep alive, so
 * the renewal thread must skip cleanly.
 */
START_TEST(test_tick_local_skip)
{
	struct dstore *ds = dstore_alloc(200, "127.0.0.1", 0, "/test",
					 REFFS_DS_PROTO_NFSV3, false, false);

	ck_assert_ptr_nonnull(ds);
	ck_assert_ptr_eq(ds->ds_ops, &dstore_ops_local);

	struct renewal_tick_ctx ctx = { 0 };

	ds_renewal_tick_one(ds, &ctx);

	ck_assert_uint_eq(ctx.skipped_local, 1);
	ck_assert_uint_eq(ctx.skipped_no_session, 0);
	ck_assert_uint_eq(ctx.reconnect_attempted, 0);
	ck_assert_uint_eq(ctx.reconnect_skipped_backoff, 0);
	ck_assert_uint_eq(ctx.v3_renewed, 0);
	ck_assert_uint_eq(ctx.v3_failed, 0);
	ck_assert_uint_eq(ctx.renewed, 0);
	ck_assert_uint_eq(ctx.failed, 0);

	dstore_put(ds);
}
END_TEST

/*
 * Whitebox: NFSv3 dstore with a future next_attempt_ns deadline
 * takes the in-backoff early-return in renewal_tick_one_nfsv3 and
 * issues NO NULL RPC.  This is the cooperative-throttle path that
 * protects a wedged DS from a hammering renewal loop -- a failure
 * here would let the worker thrash on a DS that just refused a
 * connection.
 *
 * The dstore uses TEST-NET-1 (RFC 5737, 192.0.2.0/24) so the
 * address is guaranteed non-local across hosts (no interface ever
 * binds it), which keeps dstore_alloc's local-detection bypass
 * from selecting dstore_ops_local.
 */
START_TEST(test_tick_nfsv3_in_backoff)
{
	struct dstore *ds = dstore_alloc(201, "192.0.2.1", 0, "/test",
					 REFFS_DS_PROTO_NFSV3, false, false);

	ck_assert_ptr_nonnull(ds);
	ck_assert_ptr_ne(ds->ds_ops, &dstore_ops_local);

	/* Plant a deadline 1 hour out -- well past any test runtime. */
	uint64_t now_ns = reffs_now_ns();

	atomic_store_explicit(&ds->ds_reconnect_next_attempt_ns,
			      now_ns + 3600ULL * 1000000000ULL,
			      memory_order_release);
	atomic_store_explicit(&ds->ds_reconnect_backoff_sec, 16,
			      memory_order_release);

	struct renewal_tick_ctx ctx = { 0 };

	ds_renewal_tick_one(ds, &ctx);

	ck_assert_uint_eq(ctx.reconnect_skipped_backoff, 1);
	ck_assert_uint_eq(ctx.skipped_local, 0);
	ck_assert_uint_eq(ctx.v3_renewed, 0);
	ck_assert_uint_eq(ctx.v3_failed, 0);
	ck_assert_uint_eq(ctx.reconnect_attempted, 0);
	/* Skip path must NOT clear backoff state. */
	ck_assert_uint_eq(atomic_load_explicit(&ds->ds_reconnect_backoff_sec,
					       memory_order_acquire),
			  16);

	dstore_put(ds);
}
END_TEST

/*
 * Whitebox: NFSv4 dstore with no session AND a future deadline.
 * dstore_session_borrow returns NULL (the slot was never published
 * because do_mount=false), and the dispatcher's no-session arm
 * checks the same per-dstore deadline.  Future deadline -> skipped,
 * NO ds_session_create attempt.  This is the parked-by-prior-
 * reconnect-failure steady state.
 *
 * Asserts both skipped_no_session and reconnect_skipped_backoff
 * because the dispatcher bumps both -- the borrow returning NULL
 * is independent of whether the deadline gates the reconnect.
 */
START_TEST(test_tick_nfsv4_no_session_in_backoff)
{
	struct dstore *ds = dstore_alloc(202, "192.0.2.2", 0, "/test",
					 REFFS_DS_PROTO_NFSV4, false, false);

	ck_assert_ptr_nonnull(ds);
	ck_assert_ptr_ne(ds->ds_ops, &dstore_ops_local);

	uint64_t now_ns = reffs_now_ns();

	atomic_store_explicit(&ds->ds_reconnect_next_attempt_ns,
			      now_ns + 3600ULL * 1000000000ULL,
			      memory_order_release);
	atomic_store_explicit(&ds->ds_reconnect_backoff_sec, 32,
			      memory_order_release);

	struct renewal_tick_ctx ctx = { 0 };

	ds_renewal_tick_one(ds, &ctx);

	ck_assert_uint_eq(ctx.skipped_no_session, 1);
	ck_assert_uint_eq(ctx.reconnect_skipped_backoff, 1);
	ck_assert_uint_eq(ctx.skipped_local, 0);
	ck_assert_uint_eq(ctx.reconnect_attempted, 0);
	ck_assert_uint_eq(ctx.renewed, 0);
	ck_assert_uint_eq(ctx.failed, 0);
	/* Skip path must NOT clear backoff state. */
	ck_assert_uint_eq(atomic_load_explicit(&ds->ds_reconnect_backoff_sec,
					       memory_order_acquire),
			  32);

	dstore_put(ds);
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
	tcase_add_test(tc, test_tick_local_skip);
	tcase_add_test(tc, test_tick_nfsv3_in_backoff);
	tcase_add_test(tc, test_tick_nfsv4_no_session_in_backoff);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	return nfs4_test_run(ds_renewal_suite());
}
