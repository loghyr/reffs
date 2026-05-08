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

#include "ec_client.h"
#include "ps_renewal.h"
#include "ps_state.h"
#include "reffs/log.h"

static pthread_t s_renewal_thread;
static _Atomic uint32_t s_renewal_running; /* 0 = stopped, 1 = running */
static pthread_mutex_t s_renewal_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_renewal_cv = PTHREAD_COND_INITIALIZER;
static uint32_t s_renewal_interval_seconds;

struct renewal_tick_ctx {
	uint32_t renewed;
	uint32_t failed;
	uint32_t skipped_no_session;
};

/*
 * Per-listener callback: send SEQUENCE on this listener's upstream
 * session if one is attached.  Errors are logged once per tick (we
 * don't want to spam the log when the upstream is down for a long
 * stretch).
 */
static int renewal_tick_one(const struct ps_listener_state *pls, void *arg)
{
	struct renewal_tick_ctx *ctx = arg;

	if (!pls->pls_session) {
		ctx->skipped_no_session++;
		return 0;
	}

	int ret = mds_session_renew_lease(pls->pls_session);

	if (ret == 0) {
		ctx->renewed++;
	} else {
		ctx->failed++;
		LOG("ps_renewal: listener_id=%u SEQUENCE renewal failed: %s "
		    "-- next forwarded op may see NFS4ERR_BADSESSION",
		    pls->pls_listener_id, strerror(-ret));
	}
	return 0;
}

static void *ps_renewal_thread_fn(void *arg __attribute__((unused)))
{
	pthread_setname_np(pthread_self(), "ps-renewal");

	while (atomic_load_explicit(&s_renewal_running, memory_order_acquire)) {
		struct renewal_tick_ctx ctx = { 0 };

		ps_state_listeners_for_each(renewal_tick_one, &ctx);

		if (ctx.renewed || ctx.failed) {
			TRACE("ps_renewal: tick renewed=%u failed=%u "
			      "skipped_no_session=%u",
			      ctx.renewed, ctx.failed, ctx.skipped_no_session);
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
