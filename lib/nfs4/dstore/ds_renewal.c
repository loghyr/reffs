/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

/*
 * MDS-to-DS session keepalive thread.
 *
 * Mirrors lib/nfs4/ps/ps_renewal.c.  Differences from the PS
 * mirror:
 *   1. Per-dstore (not per-listener) state -- registry lookup is
 *      dstore_collect_all rather than ps_state_listeners_for_each.
 *   2. Reconnect is the existing ds_session_create() which already
 *      does EXCHANGE_ID + CREATE_SESSION + PUTROOTFH+GETFH and
 *      publishes the new session via dstore_session_replace.  So
 *      the reconnect arm here is a one-liner, not a 90-line
 *      mds_session_create_tls + PROXY_REGISTRATION dance.
 *   3. The kick API takes a struct dstore * (combined wake +
 *      per-dstore schedule reset).
 *
 * Trigger sources for reconnect:
 *   - Lease keep-alive: every interval_seconds, fan out SEQUENCE
 *     to every NFSv4 dstore with a live session.  A
 *     mds_session_is_dead classification parks the slot and
 *     attempts immediate reconnect (subject to backoff if a
 *     prior reconnect just failed).
 *   - send_and_check_ds dead-classification (caller-side): parks
 *     the slot via dstore_session_replace(ds, NULL) and calls
 *     ds_renewal_kick(ds) which resets the per-dstore backoff
 *     and broadcasts the renewal CV so the next tick attempts
 *     reconnect immediately.  See ds_after_send in
 *     lib/nfs4/dstore/dstore_ops_nfsv4.c.
 *
 * Concurrency model:
 *   - Single worker thread.  Owns no per-dstore lock; all locking
 *     is inside dstore_session_borrow/release/replace which take
 *     the per-dstore rwlock for the swap.
 *   - Iterates via dstore_collect_all snapshot (refs bumped) so
 *     the per-dstore work happens outside any RCU read-side
 *     critical section -- safe for blocking RPC.
 *   - Honours shutdown that arrives mid-RPC: the next tick after
 *     ds_renewal_stop flips s_renewal_running observes 0 and
 *     exits the outer loop.  In-flight ds_session_create cannot
 *     be interrupted but it is bounded by RPC timeout; the join
 *     in ds_renewal_stop will wait for it.
 */

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <rpc/rpc.h>

#include "nfsv3_xdr.h"
#include "nfsv42_xdr.h"
#include "ec_client.h"
#include "reffs/dstore.h"
#include "reffs/dstore_ops.h"
#include "reffs/log.h"
#include "reffs/posix_shims.h"
#include "reffs/time.h"

#include "ds_renewal.h"
#include "ds_renewal_internal.h"

/* NFSv3 keep-alive RPC timeout.  Short -- the renewal worker is the
 * only caller; we want quick failure detection so the next tick can
 * trigger reconnect rather than the worker blocking out the full
 * DS_RPC_TIMEOUT_SEC used by control-plane ops. */
#define DS_NFSV3_KEEPALIVE_TIMEOUT_SEC 5

static pthread_t s_renewal_thread;
static _Atomic uint32_t s_renewal_running; /* 0 = stopped, 1 = running */
static pthread_mutex_t s_renewal_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_renewal_cv = PTHREAD_COND_INITIALIZER;
static uint32_t s_renewal_interval_seconds;

/*
 * struct renewal_tick_ctx lives in ds_renewal_internal.h so the
 * whitebox unit tests (lib/nfs4/dstore/tests/ds_renewal_test.c)
 * can read its counter fields after invoking renewal_tick_one
 * directly.  Mirror of the lib/nfs4/ps/ps_renewal_internal.h
 * pattern used by ps_reconnect_test.
 */

/*
 * NFSv3 keep-alive: send NFSPROC3_NULL on the cached CLIENT handle.
 * Cheap (no args, no response body) and answers the same question as
 * the NFSv4 SEQUENCE keep-alive: is the MDS-to-DS TCP socket still
 * alive and is the peer still answering RPCs?
 *
 * On RPC failure, trigger dstore_reconnect.  Unlike the NFSv4 path
 * (which parks the session pointer and lets the worker re-create on
 * the next tick), NFSv3 has its own re-MOUNT-on-failure path inside
 * dstore_reconnect that handles CLIENT teardown, fresh MOUNT, and
 * publish of the new ds_clnt.  We just call it; the next tick re-
 * tries the NULL via the new handle.
 *
 * Holds ds_clnt_mutex during the clnt_call -- shares the existing
 * NFSv3 vtable contract (every clnt_call site does the same).
 */
static int ds_nfsv3_renewal_one(struct dstore *ds)
{
	struct timeval tv = { .tv_sec = DS_NFSV3_KEEPALIVE_TIMEOUT_SEC,
			      .tv_usec = 0 };
	enum clnt_stat rpc_stat;

	pthread_mutex_lock(&ds->ds_clnt_mutex);
	if (!ds->ds_clnt) {
		pthread_mutex_unlock(&ds->ds_clnt_mutex);
		return -ENOTCONN;
	}

	/*
	 * libtirpc declares xdr_void as `bool_t xdr_void(void)` but
	 * xdrproc_t expects `bool_t (*)(XDR *, ...)` -- a strict
	 * function-pointer-type mismatch that Fedora's clang flags
	 * under -Werror,-Wcast-function-type-mismatch.  Per
	 * .claude/standards.md "XDR Proc Indirection and UBSan
	 * Suppression", cast through `(xdrproc_t)(void *)` to bypass.
	 * libtirpc itself uses this idiom at every xdr_void call.
	 */
	rpc_stat = clnt_call(ds->ds_clnt, NFSPROC3_NULL,
			     (xdrproc_t)(void *)xdr_void, NULL,
			     (xdrproc_t)(void *)xdr_void, NULL, tv);
	pthread_mutex_unlock(&ds->ds_clnt_mutex);

	if (rpc_stat != RPC_SUCCESS) {
		LOG("ds_renewal: dstore[%u] NFSv3 NULL keep-alive failed "
		    "(rpc_stat=%d) -- triggering reconnect",
		    ds->ds_id, (int)rpc_stat);
		return -EIO;
	}
	return 0;
}

/*
 * Run a reconnect attempt on this dstore.  Returns 0 on success
 * (new session installed by ds_session_create), -errno on failure.
 *
 * Holds no dstore-side lock when called.  ds_session_create's
 * internal dstore_session_replace takes the wrlock briefly for
 * the publish; everything else (EXCHANGE_ID / CREATE_SESSION /
 * the GETFH compound) runs on a fresh struct mds_session that no
 * other thread can reach until the publish.
 *
 * Shutdown courtesy: if s_renewal_running flips to 0 during the
 * RPC chain inside ds_session_create, we still return whatever
 * ds_session_create returns; the next outer-loop iteration sees
 * !running and exits.  No need to abort mid-flight.
 */
static int ds_renewal_reconnect(struct dstore *ds)
{
	if (!ds)
		return -EINVAL;
	/*
	 * ds_session_create has its own idempotency short-circuit
	 * ("already connected"), so calling it after the renewal tick
	 * parked the slot and another thread (e.g. an InBand op)
	 * raced ahead and reconnected is safe -- the loser observes
	 * the borrow returning a non-NULL pointer and returns 0
	 * without doing work.
	 */
	return ds_session_create(ds);
}

/*
 * NFSv3 per-dstore tick.  Dispatched from renewal_tick_one when the
 * dstore is REFFS_DS_PROTO_NFSV3.  See ds_nfsv3_renewal_one for the
 * NULL-RPC + reconnect contract.  We do NOT use the rwlock accessor
 * here -- NFSv3's CLIENT handle is gated by ds_clnt_mutex (the pre-
 * existing pattern); the rwlock only protects ds_v4_session.
 *
 * Returns nothing; caller updates ctx counters via the pointer.
 */
static void renewal_tick_one_nfsv3(struct dstore *ds,
				   struct renewal_tick_ctx *ctx)
{
	uint64_t now_ns = reffs_now_ns();
	uint64_t deadline_ns = atomic_load_explicit(
		&ds->ds_reconnect_next_attempt_ns, memory_order_acquire);

	if (deadline_ns != 0 && now_ns < deadline_ns) {
		ctx->reconnect_skipped_backoff++;
		return;
	}

	int ret = ds_nfsv3_renewal_one(ds);

	if (ret == 0) {
		ctx->v3_renewed++;
		atomic_store_explicit(&ds->ds_reconnect_backoff_sec, 0,
				      memory_order_release);
		atomic_store_explicit(&ds->ds_reconnect_next_attempt_ns, 0,
				      memory_order_release);
		return;
	}

	ctx->v3_failed++;

	/*
	 * NULL RPC failed (either ENOTCONN -- ds_clnt is NULL after a
	 * prior reconnect blew the handle, or RPC_*_ERROR from
	 * clnt_call's timeout / disconnect).  Trigger dstore_reconnect,
	 * which is the existing NFSv3 reconnect path: it takes
	 * ds_clnt_mutex, blows the old CLIENT, re-MOUNTs, publishes a
	 * fresh ds_clnt.  Serialised with concurrent NFSv3 ops (they
	 * all take the same mutex).
	 *
	 * dstore_reconnect returns 0 on success, negative errno on
	 * failure.  On failure, advance the per-dstore backoff so the
	 * next tick skips this dstore until the deadline elapses.
	 */
	ctx->reconnect_attempted++;
	int rret = dstore_reconnect(ds);

	if (rret == 0) {
		ctx->reconnect_succeeded++;
		atomic_store_explicit(&ds->ds_reconnect_backoff_sec, 0,
				      memory_order_release);
		atomic_store_explicit(&ds->ds_reconnect_next_attempt_ns, 0,
				      memory_order_release);
		LOG("ds_renewal: dstore[%u] NFSv3 reconnected to %s", ds->ds_id,
		    ds->ds_address);
	} else {
		uint32_t backoff = atomic_load_explicit(
			&ds->ds_reconnect_backoff_sec, memory_order_acquire);
		uint32_t wait_sec = mds_reconnect_backoff_next(&backoff);

		atomic_store_explicit(&ds->ds_reconnect_backoff_sec, backoff,
				      memory_order_release);
		atomic_store_explicit(&ds->ds_reconnect_next_attempt_ns,
				      now_ns + (uint64_t)wait_sec *
						       1000000000ULL,
				      memory_order_release);
		LOG("ds_renewal: dstore[%u] NFSv3 reconnect to %s failed: %s "
		    "(next attempt in %us)",
		    ds->ds_id, ds->ds_address, strerror(-rret), wait_sec);
	}
}

/*
 * Per-dstore tick dispatch.  Routes by protocol:
 *   - local (combined-mode loopback): no keep-alive needed (in-
 *     process VFS, no socket to idle out).  Skipped early.
 *   - NFSv3: delegate to renewal_tick_one_nfsv3 (NULL RPC under
 *     ds_clnt_mutex, dstore_reconnect on failure).
 *   - NFSv4: borrow the session and renew via SEQUENCE; on dead
 *     classification, park via dstore_session_replace(ds, NULL)
 *     and reconnect via ds_session_create.
 *
 * Returns nothing; caller updates ctx counters via the pointer.
 */
void ds_renewal_tick_one(struct dstore *ds, struct renewal_tick_ctx *ctx)
{
	if (!ds || !ctx)
		return;

	if (ds->ds_ops == &dstore_ops_local) {
		ctx->skipped_local++;
		return;
	}

	if (ds->ds_protocol == REFFS_DS_PROTO_NFSV3) {
		renewal_tick_one_nfsv3(ds, ctx);
		return;
	}

	struct mds_session *ms = dstore_session_borrow(ds);

	if (!ms) {
		ctx->skipped_no_session++;
		/*
		 * No session: try to (re)connect if the backoff deadline
		 * has elapsed.  Covers both the steady-DEAD state (an
		 * earlier dead classification parked the slot) and the
		 * never-connected state (boot's ds_session_create failed
		 * and the slot was never published).
		 */
		uint64_t now_ns = reffs_now_ns();

		uint64_t deadline_ns =
			atomic_load_explicit(&ds->ds_reconnect_next_attempt_ns,
					     memory_order_acquire);

		if (deadline_ns != 0 && now_ns < deadline_ns) {
			ctx->reconnect_skipped_backoff++;
			return;
		}

		ctx->reconnect_attempted++;
		int rret = ds_renewal_reconnect(ds);

		if (rret == 0) {
			ctx->reconnect_succeeded++;
			atomic_store_explicit(&ds->ds_reconnect_backoff_sec, 0,
					      memory_order_release);
			atomic_store_explicit(&ds->ds_reconnect_next_attempt_ns,
					      0, memory_order_release);
			LOG("ds_renewal: dstore[%u] reconnected to %s",
			    ds->ds_id, ds->ds_address);
		} else {
			uint32_t backoff = atomic_load_explicit(
				&ds->ds_reconnect_backoff_sec,
				memory_order_acquire);
			uint32_t wait_sec =
				mds_reconnect_backoff_next(&backoff);

			atomic_store_explicit(&ds->ds_reconnect_backoff_sec,
					      backoff, memory_order_release);
			atomic_store_explicit(&ds->ds_reconnect_next_attempt_ns,
					      now_ns + (uint64_t)wait_sec *
							       1000000000ULL,
					      memory_order_release);
			LOG("ds_renewal: dstore[%u] reconnect to %s failed: %s "
			    "(next attempt in %us)",
			    ds->ds_id, ds->ds_address, strerror(-rret),
			    wait_sec);
		}
		return;
	}

	nfsstat4 sr_status = NFS4_OK;
	int ret = mds_session_renew_lease_ex(ms, &sr_status);

	dstore_session_release(ds);

	if (ret == 0) {
		ctx->renewed++;
		/*
		 * Successful renewal proves the session is live; clear
		 * any stale backoff that prior failures may have armed.
		 */
		atomic_store_explicit(&ds->ds_reconnect_backoff_sec, 0,
				      memory_order_release);
		atomic_store_explicit(&ds->ds_reconnect_next_attempt_ns, 0,
				      memory_order_release);
		return;
	}

	ctx->failed++;

	if (!mds_session_is_dead(ret, sr_status)) {
		/* Per-op transient (e.g. NFS4ERR_DELAY).  Just log. */
		LOG("ds_renewal: dstore[%u] SEQUENCE renewal failed (transient): %s "
		    "sr_status=%u -- session still alive",
		    ds->ds_id, strerror(-ret), (unsigned)sr_status);
		return;
	}

	/*
	 * Session-killer.  Log once, park the slot so workers stop
	 * touching the dead pointer, then attempt immediate reconnect.
	 * Backoff stays at 0 -- first attempt after a dead detection
	 * is always immediate.
	 */
	LOG("ds_renewal: dstore[%u] MDS-to-DS session is dead "
	    "(errno=%s sr_status=%u) -- forcing reconnect",
	    ds->ds_id, strerror(-ret), (unsigned)sr_status);

	dstore_session_replace(ds, NULL);
	atomic_store_explicit(&ds->ds_reconnect_next_attempt_ns, 0,
			      memory_order_release);
	atomic_store_explicit(&ds->ds_reconnect_backoff_sec, 0,
			      memory_order_release);

	uint64_t now_ns = reffs_now_ns();

	ctx->reconnect_attempted++;
	int rret = ds_renewal_reconnect(ds);

	if (rret == 0) {
		ctx->reconnect_succeeded++;
		atomic_store_explicit(&ds->ds_reconnect_backoff_sec, 0,
				      memory_order_release);
		atomic_store_explicit(&ds->ds_reconnect_next_attempt_ns, 0,
				      memory_order_release);
		LOG("ds_renewal: dstore[%u] reconnected to %s", ds->ds_id,
		    ds->ds_address);
	} else {
		uint32_t backoff = atomic_load_explicit(
			&ds->ds_reconnect_backoff_sec, memory_order_acquire);
		uint32_t wait_sec = mds_reconnect_backoff_next(&backoff);

		atomic_store_explicit(&ds->ds_reconnect_backoff_sec, backoff,
				      memory_order_release);
		atomic_store_explicit(&ds->ds_reconnect_next_attempt_ns,
				      now_ns + (uint64_t)wait_sec *
						       1000000000ULL,
				      memory_order_release);
		LOG("ds_renewal: dstore[%u] reconnect to %s failed: %s "
		    "(next attempt in %us)",
		    ds->ds_id, ds->ds_address, strerror(-rret), wait_sec);
	}
}

/*
 * One pass over every NFSv4 dstore.  Snapshot via dstore_collect_all
 * (refs bumped) so the per-dstore work runs outside RCU read-side
 * sections -- safe for blocking RPC and rwlock acquisitions.
 *
 * DSTORE_REVOKE_MAX (64) is the snapshot capacity.  No realistic
 * deployment has more dstores than that; if we ever do, this fanout
 * silently truncates -- the truncated dstores miss one renewal tick
 * but will be picked up on the next.  The same buffer size is used
 * by the lease-expiry bulk-revoke path (client_persist.c:122).
 */
static void ds_renewal_one_tick(void)
{
	struct dstore *dss[DSTORE_REVOKE_MAX];
	uint32_t n = dstore_collect_all(dss, DSTORE_REVOKE_MAX);
	struct renewal_tick_ctx ctx = { 0 };

	for (uint32_t i = 0; i < n; i++) {
		if (atomic_load_explicit(&s_renewal_running,
					 memory_order_acquire))
			ds_renewal_tick_one(dss[i], &ctx);
		dstore_put(dss[i]);
	}

	if (ctx.renewed || ctx.failed || ctx.v3_renewed || ctx.v3_failed ||
	    ctx.reconnect_attempted) {
		TRACE("ds_renewal: tick v4_renewed=%u v4_failed=%u "
		      "v3_renewed=%u v3_failed=%u "
		      "skipped_no_session=%u skipped_local=%u "
		      "reconnect_attempted=%u reconnect_succeeded=%u "
		      "reconnect_skipped_backoff=%u",
		      ctx.renewed, ctx.failed, ctx.v3_renewed, ctx.v3_failed,
		      ctx.skipped_no_session, ctx.skipped_local,
		      ctx.reconnect_attempted, ctx.reconnect_succeeded,
		      ctx.reconnect_skipped_backoff);
	}
}

static void *ds_renewal_thread_fn(void *arg __attribute__((unused)))
{
	reffs_pthread_setname_self("ds-renewal");

	/*
	 * RCU read-side requires the thread to register with liburcu.
	 * The tick itself does not take rcu_read_lock (we use
	 * dstore_collect_all which takes/releases internally), but
	 * dstore_session_borrow's internal pthread_rwlock_rdlock does
	 * not need RCU.  The defensive registration keeps us correct
	 * if future tick code does take a read-side critical section.
	 */
	rcu_register_thread();

	while (atomic_load_explicit(&s_renewal_running, memory_order_acquire)) {
		ds_renewal_one_tick();

		/*
		 * Sleep s_renewal_interval_seconds, woken early by
		 * ds_renewal_stop() flipping the running flag or by
		 * ds_renewal_kick() broadcasting the CV.
		 */
		struct timespec deadline;

		clock_gettime(CLOCK_REALTIME, &deadline);
		deadline.tv_sec += s_renewal_interval_seconds;

		/*
		 * Single timed wait per outer tick.  Any wake -- deadline
		 * elapsed, ds_renewal_stop broadcast, ds_renewal_kick
		 * broadcast, spurious -- exits this wait and lets the
		 * outer while re-check the running flag.  Treating
		 * spurious wakeups as an extra tick is harmless (a
		 * live-session tick is one SEQUENCE per dstore,
		 * milliseconds total).
		 *
		 * Diverges from ps_renewal's inner-while pattern: that
		 * one re-waits on non-timeout returns, so ps_renewal_kick's
		 * broadcast does not actually shorten the tick cadence.
		 * For MDS-to-DS we want the kick to actually wake -- it
		 * is the W2 recovery channel called from send_and_check_ds
		 * when a fan-out op observes a dead session, and the
		 * bench evidence the keep-alive slice is validating
		 * against is sensitive to recovery latency.
		 */
		pthread_mutex_lock(&s_renewal_mtx);
		if (atomic_load_explicit(&s_renewal_running,
					 memory_order_acquire))
			(void)pthread_cond_timedwait(&s_renewal_cv,
						     &s_renewal_mtx, &deadline);
		pthread_mutex_unlock(&s_renewal_mtx);
	}

	rcu_unregister_thread();
	return NULL;
}

void ds_renewal_kick(struct dstore *ds)
{
	if (ds) {
		/*
		 * Reset the per-dstore reconnect schedule.  Release-store
		 * pairs with the renewal thread's acquire-load at tick
		 * time.  Order matters: clear next_attempt_ns BEFORE
		 * backoff_sec so a concurrent tick that observes
		 * backoff=0 also observes next_attempt=0 (never the
		 * "fresh backoff but old deadline" combination that
		 * would skip one tick).
		 */
		atomic_store_explicit(&ds->ds_reconnect_next_attempt_ns, 0,
				      memory_order_release);
		atomic_store_explicit(&ds->ds_reconnect_backoff_sec, 0,
				      memory_order_release);
	}

	/*
	 * Wake the renewal thread early.  Safe before start (no thread
	 * is parked on the CV; broadcast is a no-op) and after stop
	 * (s_renewal_running == 0; the thread has already joined and
	 * the CV is just static memory).  The mutex pair is required
	 * by POSIX -- without it the broadcast can race with the
	 * thread's pthread_cond_timedwait setup and lose the wake.
	 */
	pthread_mutex_lock(&s_renewal_mtx);
	pthread_cond_broadcast(&s_renewal_cv);
	pthread_mutex_unlock(&s_renewal_mtx);
}

int ds_renewal_start(uint32_t interval_seconds)
{
	if (interval_seconds == 0) {
		/*
		 * Caller opted out (TOML ds_session_renewal_interval_sec=0).
		 * Treat as success without spawning -- send_and_check_ds
		 * still parks dead sessions; subsequent ops surface
		 * -ENOTCONN until something else (admin restart) brings
		 * the session back.  This is the documented opt-out
		 * trade-off in mds-ds-session-keepalive.md.
		 */
		LOG("ds_renewal: disabled (interval_seconds=0)");
		return 0;
	}

	uint32_t expected = 0;

	if (!atomic_compare_exchange_strong_explicit(
		    &s_renewal_running, &expected, 1, memory_order_acq_rel,
		    memory_order_relaxed))
		return 0; /* already running */

	s_renewal_interval_seconds = interval_seconds;

	int ret = pthread_create(&s_renewal_thread, NULL, ds_renewal_thread_fn,
				 NULL);
	if (ret != 0) {
		atomic_store_explicit(&s_renewal_running, 0,
				      memory_order_release);
		LOG("ds_renewal: pthread_create failed: %s", strerror(ret));
		return -ret;
	}

	TRACE("ds_renewal: started (interval=%us)", interval_seconds);
	return 0;
}

void ds_renewal_stop(void)
{
	uint32_t expected = 1;

	if (!atomic_compare_exchange_strong_explicit(
		    &s_renewal_running, &expected, 0, memory_order_acq_rel,
		    memory_order_relaxed))
		return; /* not running */

	pthread_mutex_lock(&s_renewal_mtx);
	pthread_cond_broadcast(&s_renewal_cv);
	pthread_mutex_unlock(&s_renewal_mtx);

	pthread_join(s_renewal_thread, NULL);
	TRACE("ds_renewal: stopped");
}
