/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nfsv42_xdr.h"
#include "reffs/log.h"
#include "reffs/posix_shims.h"
#include "reffs/time.h"

#include "ec_client.h"
#include "ps_renewal.h"
#include "ps_renewal_internal.h"
#include "ps_state.h"

static pthread_t s_renewal_thread;
static _Atomic uint32_t s_renewal_running; /* 0 = stopped, 1 = running */
static pthread_mutex_t s_renewal_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_renewal_cv = PTHREAD_COND_INITIALIZER;
static uint32_t s_renewal_interval_seconds;

/*
 * struct renewal_tick_ctx and renewal_tick_one's signature live in
 * ps_renewal_internal.h so the whitebox tests can invoke the same
 * callback the production thread runs through ps_state_listeners_for_each.
 */

/*
 * Build a fresh upstream session for `pls` using the cached bring-up
 * parameters and atomically swap it in via ps_listener_session_replace.
 * On success, resets the per-listener backoff counter.  On failure,
 * advances the backoff schedule and arms the next-attempt deadline.
 *
 * The renewal thread holds NO lock when this runs.  All locking is
 * inside the helpers (ps_listener_session_replace takes the wrlock
 * for the swap; mds_session_create_tls is unlocked work on a fresh
 * private struct that no other thread can reach).
 *
 * Returns 0 on full reconnect success (new session installed,
 * PROXY_REGISTRATION accepted), -errno on failure (new session
 * disposed of, listener still pointing at the dead session).
 */
static int ps_listener_reconnect(struct ps_listener_state *pls)
{
	if (!pls)
		return -EINVAL;
	if (pls->pls_upstream[0] == '\0')
		return -EINVAL;

	struct mds_session *new_ms = calloc(1, sizeof(*new_ms));

	if (!new_ms)
		return -ENOMEM;

	char owner[256];

	snprintf(owner, sizeof(owner), "reffs-ps-%u", pls->pls_listener_id);
	mds_session_set_owner(new_ms, owner);

	int ret = mds_session_create_tls(new_ms, pls->pls_upstream,
					 pls->pls_upstream_port,
					 pls->pls_tls_cert, pls->pls_tls_key,
					 pls->pls_tls_ca, pls->pls_tls_mode,
					 pls->pls_tls_insecure_no_verify);
	if (ret < 0) {
		free(new_ms);
		return ret;
	}

	/*
	 * Honour shutdown that arrived between alloc and TLS handshake.
	 * Don't publish a session reffsd is about to tear down anyway --
	 * cleaner shutdown trace, no race on the rwlock destroy at fini.
	 */
	if (!atomic_load_explicit(&s_renewal_running, memory_order_acquire)) {
		mds_session_destroy(new_ms);
		free(new_ms);
		return -ESHUTDOWN;
	}

	/*
	 * Re-issue PROXY_REGISTRATION with the SAME id the boot path
	 * used.  The MDS treats matching ids as a renewal (idempotent,
	 * lease refreshed) and skips the squat-guard wait it would
	 * apply to a fresh id while a prior registration's lease is
	 * still valid.  If the boot path never stashed an id (registr.
	 * disabled, allowlist mismatch at boot, etc.), skip
	 * registration -- the session is still usable for unprivileged
	 * forwarded ops, and the logged "no registered-PS privilege"
	 * line at boot already told the operator.
	 */
	if (pls->pls_registration_id_len > 0) {
		ret = mds_session_send_proxy_registration(
			new_ms, pls->pls_registration_id,
			pls->pls_registration_id_len);
		if (ret < 0) {
			mds_session_destroy(new_ms);
			free(new_ms);
			return ret;
		}
	}

	/*
	 * Atomic swap: workers blocked on the rdlock for the in-flight
	 * forwarder (if any) finish their RPC against the OLD session
	 * before the wlock is granted; new workers picking up the read
	 * lock after the swap see the NEW session.  ps_listener_session_replace
	 * destroys + frees the old session.
	 */
	int rret = ps_listener_session_replace(pls->pls_listener_id, new_ms);

	if (rret < 0) {
		/* Listener disappeared mid-reconnect -- shutdown race. */
		mds_session_destroy(new_ms);
		free(new_ms);
		return rret;
	}
	return 0;
}

/*
 * Per-listener tick callback.  Either renews the lease or, if the
 * session is dead and the backoff deadline has elapsed, runs a
 * reconnect.
 */
int renewal_tick_one(const struct ps_listener_state *pls_const, void *arg)
{
	struct renewal_tick_ctx *ctx = arg;
	/*
	 * The for_each contract gives us a const pointer; the helpers
	 * below mutate listener state under their own locking so cast
	 * away const at the call boundary -- the registry's storage is
	 * not const, only the iteration view.
	 */
	struct ps_listener_state *pls = (struct ps_listener_state *)pls_const;

	struct mds_session *ms =
		ps_listener_session_borrow(pls->pls_listener_id);

	if (!ms) {
		ctx->skipped_no_session++;
		/*
		 * No session: try to (re)connect if the listener has an
		 * upstream configured and the backoff deadline has elapsed.
		 * This covers both the steady-DEAD state (renewal hit
		 * BADSESSION earlier) and the listener-never-connected
		 * state (boot's mds_session_create_tls failed).
		 */
		if (pls->pls_upstream[0] == '\0')
			return 0;

		uint64_t now_ns = reffs_now_ns();

		if (pls->pls_reconnect_next_attempt_ns != 0 &&
		    now_ns < pls->pls_reconnect_next_attempt_ns) {
			ctx->reconnect_skipped_backoff++;
			return 0;
		}

		ctx->reconnect_attempted++;
		int rret = ps_listener_reconnect(pls);

		if (rret == 0) {
			ctx->reconnect_succeeded++;
			ps_reconnect_backoff_reset(
				&pls->pls_reconnect_backoff_sec);
			pls->pls_reconnect_next_attempt_ns = 0;
			LOG("ps_renewal: listener_id=%u reconnected to upstream %s",
			    pls->pls_listener_id, pls->pls_upstream);
		} else {
			uint32_t wait_sec = ps_reconnect_backoff_next(
				&pls->pls_reconnect_backoff_sec);
			pls->pls_reconnect_next_attempt_ns =
				now_ns + (uint64_t)wait_sec * 1000000000ULL;
			LOG("ps_renewal: listener_id=%u reconnect to %s failed: %s "
			    "(next attempt in %us)",
			    pls->pls_listener_id, pls->pls_upstream,
			    strerror(-rret), wait_sec);
		}
		return 0;
	}

	nfsstat4 sr_status = NFS4_OK;
	int ret = mds_session_renew_lease_ex(ms, &sr_status);

	ps_listener_session_release(pls->pls_listener_id);

	if (ret == 0) {
		ctx->renewed++;
		return 0;
	}

	ctx->failed++;

	if (!ps_session_is_dead(ret, sr_status)) {
		/* Per-op transient (e.g. NFS4ERR_DELAY).  Just log. */
		LOG("ps_renewal: listener_id=%u SEQUENCE renewal failed (transient): %s "
		    "sr_status=%u -- session still alive",
		    pls->pls_listener_id, strerror(-ret), (unsigned)sr_status);
		return 0;
	}

	/*
	 * Session-killer.  Log once, clear the dead pointer so workers
	 * stop touching it (and so the next tick sees no_session and
	 * runs reconnect on the backoff schedule).  Destroy the dead
	 * session here -- ps_listener_session_replace(NULL) does the
	 * destroy under the wlock for us.
	 */
	LOG("ps_renewal: listener_id=%u upstream session is dead "
	    "(errno=%s sr_status=%u) -- forcing reconnect",
	    pls->pls_listener_id, strerror(-ret), (unsigned)sr_status);

	ps_listener_session_replace(pls->pls_listener_id, NULL);
	/*
	 * Force an immediate reconnect attempt on this same tick:
	 * arm the deadline at "now" so the next no-session branch
	 * proceeds without waiting for a future tick.  Backoff stays
	 * at 0 (first attempt is always immediate).
	 */
	pls->pls_reconnect_next_attempt_ns = 0;
	pls->pls_reconnect_backoff_sec = 0;

	uint64_t now_ns = reffs_now_ns();

	ctx->reconnect_attempted++;
	int rret = ps_listener_reconnect(pls);

	if (rret == 0) {
		ctx->reconnect_succeeded++;
		ps_reconnect_backoff_reset(&pls->pls_reconnect_backoff_sec);
		pls->pls_reconnect_next_attempt_ns = 0;
		LOG("ps_renewal: listener_id=%u reconnected to upstream %s",
		    pls->pls_listener_id, pls->pls_upstream);
	} else {
		uint32_t wait_sec = ps_reconnect_backoff_next(
			&pls->pls_reconnect_backoff_sec);
		pls->pls_reconnect_next_attempt_ns =
			now_ns + (uint64_t)wait_sec * 1000000000ULL;
		LOG("ps_renewal: listener_id=%u reconnect to %s failed: %s "
		    "(next attempt in %us)",
		    pls->pls_listener_id, pls->pls_upstream, strerror(-rret),
		    wait_sec);
	}
	return 0;
}

static void *ps_renewal_thread_fn(void *arg __attribute__((unused)))
{
	reffs_pthread_setname_self("ps-renewal");

	while (atomic_load_explicit(&s_renewal_running, memory_order_acquire)) {
		struct renewal_tick_ctx ctx = { 0 };

		ps_state_listeners_for_each(renewal_tick_one, &ctx);

		if (ctx.renewed || ctx.failed || ctx.reconnect_attempted) {
			TRACE("ps_renewal: tick renewed=%u failed=%u "
			      "skipped_no_session=%u reconnect_attempted=%u "
			      "reconnect_succeeded=%u reconnect_skipped_backoff=%u",
			      ctx.renewed, ctx.failed, ctx.skipped_no_session,
			      ctx.reconnect_attempted, ctx.reconnect_succeeded,
			      ctx.reconnect_skipped_backoff);
		}

		/*
		 * Sleep `s_renewal_interval_seconds`, woken early by
		 * ps_renewal_stop() flipping the running flag.
		 */
		struct timespec deadline;

		clock_gettime(CLOCK_REALTIME, &deadline);
		deadline.tv_sec += s_renewal_interval_seconds;

		pthread_mutex_lock(&s_renewal_mtx);
		while (atomic_load_explicit(&s_renewal_running,
					    memory_order_acquire)) {
			int r = pthread_cond_timedwait(
				&s_renewal_cv, &s_renewal_mtx, &deadline);

			if (r == ETIMEDOUT)
				break;
		}
		pthread_mutex_unlock(&s_renewal_mtx);
	}
	return NULL;
}

int ps_renewal_start(uint32_t interval_seconds)
{
	if (interval_seconds == 0)
		return -EINVAL;

	uint32_t expected = 0;

	if (!atomic_compare_exchange_strong_explicit(
		    &s_renewal_running, &expected, 1, memory_order_acq_rel,
		    memory_order_relaxed))
		return 0; /* already running */

	s_renewal_interval_seconds = interval_seconds;

	int ret = pthread_create(&s_renewal_thread, NULL, ps_renewal_thread_fn,
				 NULL);
	if (ret != 0) {
		atomic_store_explicit(&s_renewal_running, 0,
				      memory_order_release);
		return -ret;
	}
	return 0;
}

void ps_renewal_stop(void)
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
}
