/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <check.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nfsv42_xdr.h"
#include "reffs/settings.h"

#include "ps_renewal.h"
#include "ps_renewal_internal.h"
#include "ps_state.h"

static void setup(void)
{
	ps_state_init();
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
 * ps_session_is_dead distinguishes session-killer wire codes from
 * per-op transients.  Each session-killer must classify true; no
 * other code may.
 */
START_TEST(test_classify_session_dead_status)
{
	ck_assert(ps_session_is_dead(-EREMOTEIO, NFS4ERR_BADSESSION));
	ck_assert(ps_session_is_dead(-EREMOTEIO, NFS4ERR_DEADSESSION));
	ck_assert(ps_session_is_dead(-EREMOTEIO, NFS4ERR_STALE_CLIENTID));
	ck_assert(ps_session_is_dead(-EREMOTEIO, NFS4ERR_BAD_SESSION_DIGEST));
}
END_TEST

START_TEST(test_classify_per_op_transient)
{
	/*
	 * NFS4ERR_DELAY is a per-op retry signal; the session itself
	 * is alive.  NFS4ERR_GRACE same.  NFS4ERR_BADSLOT means our
	 * single slot bookkeeping drifted -- recover internal to the
	 * session, not by tearing it down.
	 */
	ck_assert(!ps_session_is_dead(-EREMOTEIO, NFS4ERR_DELAY));
	ck_assert(!ps_session_is_dead(-EREMOTEIO, NFS4ERR_GRACE));
	ck_assert(!ps_session_is_dead(-EREMOTEIO, NFS4ERR_BADSLOT));
	ck_assert(!ps_session_is_dead(0, NFS4_OK));
}
END_TEST

START_TEST(test_classify_connection_lost)
{
	/*
	 * RPC-layer drops surface as one of these errnos with no
	 * decoded sr_status.  Each must classify dead.
	 */
	ck_assert(ps_session_is_dead(-EIO, 0));
	ck_assert(ps_session_is_dead(-EPIPE, 0));
	ck_assert(ps_session_is_dead(-ECONNRESET, 0));
	ck_assert(ps_session_is_dead(-ETIMEDOUT, 0));
	ck_assert(ps_session_is_dead(-ENOTCONN, 0));
	ck_assert(ps_session_is_dead(-ENETUNREACH, 0));
}
END_TEST

START_TEST(test_classify_unrelated_errno)
{
	/*
	 * EINVAL / ENOMEM are caller bugs / resource issues, not
	 * session death.  Don't trigger reconnect for them.
	 */
	ck_assert(!ps_session_is_dead(-EINVAL, 0));
	ck_assert(!ps_session_is_dead(-ENOMEM, 0));
	ck_assert(!ps_session_is_dead(-EAGAIN, 0));
}
END_TEST

/*
 * Backoff schedule: 0, 1, 2, 4, 8, 16, 32, 60, 60, ...  First call
 * (counter == 0) returns 0 (immediate retry permitted) and bumps
 * the counter to 1.  Subsequent calls double, capped at 60.
 */
START_TEST(test_backoff_progression)
{
	uint32_t b = 0;

	ck_assert_uint_eq(ps_reconnect_backoff_next(&b), 0);
	ck_assert_uint_eq(b, 1);
	ck_assert_uint_eq(ps_reconnect_backoff_next(&b), 1);
	ck_assert_uint_eq(b, 2);
	ck_assert_uint_eq(ps_reconnect_backoff_next(&b), 2);
	ck_assert_uint_eq(b, 4);
	ck_assert_uint_eq(ps_reconnect_backoff_next(&b), 4);
	ck_assert_uint_eq(b, 8);
	ck_assert_uint_eq(ps_reconnect_backoff_next(&b), 8);
	ck_assert_uint_eq(b, 16);
	ck_assert_uint_eq(ps_reconnect_backoff_next(&b), 16);
	ck_assert_uint_eq(b, 32);
	ck_assert_uint_eq(ps_reconnect_backoff_next(&b), 32);
	ck_assert_uint_eq(b, 60); /* capped here */
	ck_assert_uint_eq(ps_reconnect_backoff_next(&b), 60);
	ck_assert_uint_eq(b, 60); /* sticks at cap */
	ck_assert_uint_eq(ps_reconnect_backoff_next(&b), 60);
	ck_assert_uint_eq(b, 60);
}
END_TEST

START_TEST(test_backoff_reset)
{
	uint32_t b = 0;

	ps_reconnect_backoff_next(&b); /* -> 1 */
	ps_reconnect_backoff_next(&b); /* -> 2 */
	ps_reconnect_backoff_next(&b); /* -> 4 */
	ck_assert_uint_eq(b, 4);
	ps_reconnect_backoff_reset(&b);
	ck_assert_uint_eq(b, 0);
	/* After reset the schedule starts again from immediate. */
	ck_assert_uint_eq(ps_reconnect_backoff_next(&b), 0);
	ck_assert_uint_eq(b, 1);
}
END_TEST

START_TEST(test_backoff_null_safe)
{
	/* NULL counter must not crash; both helpers tolerate it. */
	ck_assert_uint_eq(ps_reconnect_backoff_next(NULL), 0);
	ps_reconnect_backoff_reset(NULL);
}
END_TEST

/*
 * Borrow on a registered listener with no session returns NULL and
 * does NOT hold any lock (release-after-NULL must be a no-op for
 * correctness; otherwise callers leak the lock).
 */
START_TEST(test_borrow_returns_null_when_no_session)
{
	struct reffs_proxy_mds_config c = make_cfg(7);

	ck_assert_int_eq(ps_state_register(&c), 0);
	ck_assert_ptr_null(ps_listener_session_borrow(7));

	/*
	 * Release after a NULL borrow is a no-op (no lock held); we
	 * MUST be able to call it safely without paired lock state.
	 * If borrow returned NULL with the lock held, this would
	 * crash on the next borrow attempt because the lock would
	 * already be locked-as-reader.
	 */
	ps_listener_session_release(7);
	ck_assert_ptr_null(ps_listener_session_borrow(7));
}
END_TEST

START_TEST(test_borrow_returns_null_for_unknown_listener)
{
	ck_assert_ptr_null(ps_listener_session_borrow(99));
	ps_listener_session_release(99); /* must not crash */
}
END_TEST

/*
 * After ps_state_set_session(non-NULL), borrow returns the same
 * pointer.  After ps_state_set_session(NULL), borrow returns NULL
 * again.  This validates the lock-aware initial-publish path.
 *
 * Uses fake non-NULL pointers as session sentinels -- this test
 * never dereferences mds_session, only verifies the registry's
 * pointer-tracking under lock.
 */
START_TEST(test_borrow_after_set_session_returns_pointer)
{
	struct reffs_proxy_mds_config c = make_cfg(8);
	struct mds_session *fake_sentinel = (struct mds_session *)0xCAFEF00D;

	ck_assert_int_eq(ps_state_register(&c), 0);
	ck_assert_int_eq(ps_state_set_session(8, fake_sentinel), 0);

	struct mds_session *got = ps_listener_session_borrow(8);

	ck_assert_ptr_eq(got, fake_sentinel);
	ps_listener_session_release(8);

	/* Clear the session before fini -- ps_state_fini contract. */
	ck_assert_int_eq(ps_state_set_session(8, NULL), 0);
	ck_assert_ptr_null(ps_listener_session_borrow(8));
}
END_TEST

/*
 * Concurrent readers may all hold the read lock simultaneously --
 * this test spawns 4 reader threads doing tight borrow/release
 * loops and asserts no thread sees a NULL session for the duration.
 *
 * No writer is active (ps_listener_session_replace is exercised in
 * the next test).  Goal: make sure borrow/release is concurrent-
 * safe on the read side, no spurious NULL or assertion failure.
 */
struct reader_ctx {
	uint32_t listener_id;
	uint64_t loops_done;
	int saw_null;
	struct mds_session *expected;
	pthread_t tid;
};

static void *reader_loop(void *arg)
{
	struct reader_ctx *ctx = arg;
	struct timespec deadline;

	clock_gettime(CLOCK_MONOTONIC, &deadline);
	deadline.tv_sec += 1; /* run for 1 second */

	while (1) {
		struct timespec now;

		clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec > deadline.tv_sec ||
		    (now.tv_sec == deadline.tv_sec &&
		     now.tv_nsec >= deadline.tv_nsec))
			break;

		struct mds_session *ms =
			ps_listener_session_borrow(ctx->listener_id);
		if (!ms)
			ctx->saw_null = 1;
		else if (ms != ctx->expected)
			ctx->saw_null = 1; /* repurpose as "wrong pointer" */
		ps_listener_session_release(ctx->listener_id);
		ctx->loops_done++;
	}
	return NULL;
}

START_TEST(test_concurrent_readers)
{
	struct reffs_proxy_mds_config c = make_cfg(11);
	struct mds_session *fake = (struct mds_session *)0xDEADBEEF;

	ck_assert_int_eq(ps_state_register(&c), 0);
	ck_assert_int_eq(ps_state_set_session(11, fake), 0);

	struct reader_ctx ctxs[4] = { 0 };

	for (int i = 0; i < 4; i++) {
		ctxs[i].listener_id = 11;
		ctxs[i].expected = fake;
		ck_assert_int_eq(pthread_create(&ctxs[i].tid, NULL, reader_loop,
						&ctxs[i]),
				 0);
	}
	for (int i = 0; i < 4; i++) {
		pthread_join(ctxs[i].tid, NULL);
		ck_assert_int_eq(ctxs[i].saw_null, 0);
		ck_assert_uint_gt(ctxs[i].loops_done, 0);
	}

	ck_assert_int_eq(ps_state_set_session(11, NULL), 0);
}
END_TEST

/*
 * ps_listener_session_replace returns -ENOENT for an unknown id
 * without crashing.
 */
START_TEST(test_replace_unknown_listener)
{
	ck_assert_int_eq(ps_listener_session_replace(999, NULL), -ENOENT);
}
END_TEST

/*
 * ps_listener_session_replace(NULL) on a listener with no prior
 * session is a no-op success (the new pointer is NULL, the old was
 * NULL, no destroy needed).  This is the "clear before destroy at
 * shutdown" path.
 */
START_TEST(test_replace_null_to_null)
{
	struct reffs_proxy_mds_config c = make_cfg(12);

	ck_assert_int_eq(ps_state_register(&c), 0);
	ck_assert_int_eq(ps_listener_session_replace(12, NULL), 0);
	ck_assert_ptr_null(ps_listener_session_borrow(12));
}
END_TEST

/*
 * ps_state_set_registration_id round-trips through the listener
 * state.  A renewal-thread reconnect needs the same id the boot
 * stashed; the registry is the carrier.
 */
START_TEST(test_set_registration_id_roundtrip)
{
	struct reffs_proxy_mds_config c = make_cfg(13);
	uint8_t id[16];

	ck_assert_int_eq(ps_state_register(&c), 0);
	for (int i = 0; i < 16; i++)
		id[i] = (uint8_t)(i + 1);

	ck_assert_int_eq(ps_state_set_registration_id(13, id, sizeof(id)), 0);

	const struct ps_listener_state *pls = ps_state_find(13);

	ck_assert_ptr_nonnull(pls);
	ck_assert_uint_eq(pls->pls_registration_id_len, 16);
	ck_assert_mem_eq(pls->pls_registration_id, id, 16);
}
END_TEST

START_TEST(test_set_registration_id_unknown_listener)
{
	uint8_t id[1] = { 0xAB };

	ck_assert_int_eq(ps_state_set_registration_id(99, id, 1), -ENOENT);
}
END_TEST

START_TEST(test_set_registration_id_too_long)
{
	struct reffs_proxy_mds_config c = make_cfg(14);
	uint8_t huge[64] = { 0 };

	ck_assert_int_eq(ps_state_register(&c), 0);
	ck_assert_int_eq(ps_state_set_registration_id(14, huge, sizeof(huge)),
			 -EINVAL);
}
END_TEST

START_TEST(test_set_registration_id_clear)
{
	struct reffs_proxy_mds_config c = make_cfg(15);
	uint8_t id[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

	ck_assert_int_eq(ps_state_register(&c), 0);
	ck_assert_int_eq(ps_state_set_registration_id(15, id, 8), 0);
	ck_assert_int_eq(ps_state_set_registration_id(15, NULL, 0), 0);

	const struct ps_listener_state *pls = ps_state_find(15);

	ck_assert_uint_eq(pls->pls_registration_id_len, 0);
}
END_TEST

/*
 * After ps_state_register, the cached TLS bring-up params reflect
 * the cfg's values.  The renewal thread reads these to replay
 * mds_session_create_tls on reconnect.
 */
/*
 * Reviewer BLOCKER follow-up: ps_listener_session_replace's central
 * correctness claim is that the wlock waits for in-flight readers
 * before the swap takes effect.  This test exercises the rwlock
 * quiesce property using ps_state_set_session (which takes the same
 * wlock without involving mds_session_destroy on the fake pointer).
 *
 * Coverage limit: the destroy half of ps_listener_session_replace
 * (calls mds_session_destroy + free on the old pointer) is not
 * exercised here -- it would require a real (or fully mocked)
 * mds_session, which is more infrastructure than this slice merits
 * since the destroy is mechanical post-unlock work and a UAF in the
 * destroy path would surface as ASAN under the bench soak.  The
 * critical property -- "no reader observes a torn pointer because
 * the wlock waits for the rdlock to drop" -- IS validated here.
 */
struct quiesce_writer_ctx {
	uint32_t listener_id;
	struct mds_session *new_value;
	int started;
	int finished;
	pthread_mutex_t mtx;
	pthread_cond_t cv;
};

static void *quiesce_writer_thread(void *arg)
{
	struct quiesce_writer_ctx *ctx = arg;

	pthread_mutex_lock(&ctx->mtx);
	ctx->started = 1;
	pthread_cond_broadcast(&ctx->cv);
	pthread_mutex_unlock(&ctx->mtx);

	/*
	 * This call must block until the test releases the rdlock
	 * via ps_listener_session_release().  We cannot directly
	 * observe the block from this thread; the main thread checks
	 * by waiting briefly + verifying ctx->finished is still 0.
	 */
	ps_state_set_session(ctx->listener_id, ctx->new_value);

	pthread_mutex_lock(&ctx->mtx);
	ctx->finished = 1;
	pthread_cond_broadcast(&ctx->cv);
	pthread_mutex_unlock(&ctx->mtx);
	return NULL;
}

START_TEST(test_session_replace_quiesces_in_flight)
{
	struct reffs_proxy_mds_config c = make_cfg(17);
	struct mds_session *fake = (struct mds_session *)0xC0FFEEUL;
	struct mds_session *fake2 = (struct mds_session *)0xBA5EBA11UL;

	ck_assert_int_eq(ps_state_register(&c), 0);
	ck_assert_int_eq(ps_state_set_session(17, fake), 0);

	struct mds_session *borrowed = ps_listener_session_borrow(17);

	ck_assert_ptr_eq(borrowed, fake);

	struct quiesce_writer_ctx ctx = {
		.listener_id = 17,
		.new_value = fake2,
		.started = 0,
		.finished = 0,
		.mtx = PTHREAD_MUTEX_INITIALIZER,
		.cv = PTHREAD_COND_INITIALIZER,
	};
	pthread_t writer;

	ck_assert_int_eq(
		pthread_create(&writer, NULL, quiesce_writer_thread, &ctx), 0);

	/* Wait for the writer thread to enter set_session. */
	pthread_mutex_lock(&ctx.mtx);
	while (!ctx.started)
		pthread_cond_wait(&ctx.cv, &ctx.mtx);
	pthread_mutex_unlock(&ctx.mtx);

	/*
	 * Give the writer a generous window to reach (and block on)
	 * the wrlock.  After this sleep the writer is either blocked
	 * (correct) or has finished (BUG: we still hold the rdlock).
	 */
	struct timespec sleep = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };

	nanosleep(&sleep, NULL);

	pthread_mutex_lock(&ctx.mtx);
	int finished_while_held = ctx.finished;

	pthread_mutex_unlock(&ctx.mtx);
	ck_assert_int_eq(finished_while_held, 0);

	/* The borrow still observes the original pointer. */
	ck_assert_ptr_eq(borrowed, fake);
	ck_assert_ptr_eq(ps_listener_session_borrow(17), fake);
	ps_listener_session_release(17); /* drop the second borrow */
	ps_listener_session_release(17); /* drop the original borrow */

	/* Now the writer must complete. */
	pthread_join(writer, NULL);

	/* And the swap took effect. */
	struct mds_session *after = ps_listener_session_borrow(17);

	ck_assert_ptr_eq(after, fake2);
	ps_listener_session_release(17);

	ck_assert_int_eq(ps_state_set_session(17, NULL), 0);
}
END_TEST

/*
 * Reviewer BLOCKER follow-up: minimal lifecycle test for the
 * renewal-thread machinery (start, then stop without crash or leak).
 * Coverage limit: this does NOT exercise mid-reconnect shutdown
 * (would require a stub mds_session_create_tls that blocks on a
 * test-controlled condvar).  The shutdown-during-reconnect path is
 * defended in code by the s_renewal_running check inside
 * ps_listener_reconnect after TLS handshake -- if shutdown arrives
 * between alloc and handshake completion, the new session is
 * destroyed and freed before publish.  The bench soak covers this
 * dynamically; an in-process reproducer is NOT_NOW_BROWN_COW.
 */
/*
 * ps_renewal_start refuses interval=0.  The renewal thread loop
 * uses interval as the cond_timedwait deadline; a zero interval
 * would tight-loop or trigger UB depending on libc.  Defensive
 * validation matters here because the value flows from a config
 * file (which the operator can typo).
 */
START_TEST(test_renewal_start_zero_interval_invalid)
{
	ck_assert_int_eq(ps_renewal_start(0), -EINVAL);
}
END_TEST

/*
 * Second ps_renewal_start while the thread is already running must
 * NOT spawn a second worker -- the atomic CAS gates it -- and must
 * return 0 (not an error).  ps_renewal_stop then joins the single
 * thread cleanly.
 */
START_TEST(test_renewal_start_idempotent)
{
	ck_assert_int_eq(ps_renewal_start(1), 0);
	ck_assert_int_eq(ps_renewal_start(1), 0);
	ck_assert_int_eq(ps_renewal_start(1), 0);
	ps_renewal_stop();
	/* Re-startable after a clean stop. */
	ck_assert_int_eq(ps_renewal_start(1), 0);
	ps_renewal_stop();
}
END_TEST

/*
 * ps_renewal_stop while the thread is not running must be a no-op
 * (no pthread_join of an uninitialised handle, no crash).  Idempotent
 * for the case where shutdown can be called twice -- once by an
 * explicit teardown path, once by atexit / cleanup.
 */
START_TEST(test_renewal_stop_when_not_running)
{
	ps_renewal_stop();
	ps_renewal_stop();
}
END_TEST

/*
 * Tick on a listener with no session and no upstream configured:
 * bumps skipped_no_session and takes no action.  Models a
 * registration where the operator left address blank -- the renewal
 * thread must not spin trying to connect to an empty string.
 */
START_TEST(test_tick_no_session_no_upstream)
{
	struct reffs_proxy_mds_config c = make_cfg(50);

	c.address[0] = '\0';
	c.mds_port = 0;
	ck_assert_int_eq(ps_state_register(&c), 0);

	const struct ps_listener_state *pls = ps_state_find(50);

	ck_assert_ptr_nonnull(pls);
	ck_assert_int_eq(pls->pls_upstream[0], '\0');

	struct renewal_tick_ctx ctx = { 0 };

	ck_assert_int_eq(renewal_tick_one(pls, &ctx), 0);

	ck_assert_uint_eq(ctx.skipped_no_session, 1);
	ck_assert_uint_eq(ctx.reconnect_attempted, 0);
	ck_assert_uint_eq(ctx.reconnect_skipped_backoff, 0);
	ck_assert_uint_eq(ctx.reconnect_succeeded, 0);
	ck_assert_uint_eq(ctx.renewed, 0);
	ck_assert_uint_eq(ctx.failed, 0);
}
END_TEST

/*
 * Tick on a listener with no session and an upstream, but the
 * backoff deadline is in the future: bumps reconnect_skipped_backoff
 * and takes NO connect attempt.  The deadline gates the retry so
 * the renewal thread does not hammer a known-bad upstream.
 */
START_TEST(test_tick_no_session_in_backoff)
{
	struct reffs_proxy_mds_config c = make_cfg(51);

	ck_assert_int_eq(ps_state_register(&c), 0);

	/*
	 * Cast away const to set the backoff deadline.  The for_each
	 * contract gives a const view; the registry storage itself is
	 * mutable -- mirrors what renewal_tick_one does internally.
	 */
	struct ps_listener_state *pls =
		(struct ps_listener_state *)ps_state_find(51);

	ck_assert_ptr_nonnull(pls);

	struct timespec mono;

	clock_gettime(CLOCK_MONOTONIC, &mono);
	uint64_t now_ns =
		(uint64_t)mono.tv_sec * 1000000000ULL + (uint64_t)mono.tv_nsec;
	/* 1 hour out -- well past any test runtime. */
	pls->pls_reconnect_next_attempt_ns = now_ns + 3600ULL * 1000000000ULL;
	pls->pls_reconnect_backoff_sec = 16;

	struct renewal_tick_ctx ctx = { 0 };

	ck_assert_int_eq(renewal_tick_one(pls, &ctx), 0);

	ck_assert_uint_eq(ctx.skipped_no_session, 1);
	ck_assert_uint_eq(ctx.reconnect_skipped_backoff, 1);
	ck_assert_uint_eq(ctx.reconnect_attempted, 0);
	ck_assert_uint_eq(ctx.reconnect_succeeded, 0);
	/* Skip path must NOT touch backoff state. */
	ck_assert_uint_eq(pls->pls_reconnect_backoff_sec, 16);
}
END_TEST

/*
 * Tick on a listener with no session, an upstream, and no backoff
 * deadline: must attempt the reconnect.  Pointing at 127.0.0.1:1
 * makes connect() return ECONNREFUSED in microseconds (port 1 is
 * IANA-reserved tcpmux, virtually never bound on dev/CI hosts).
 * After the failed attempt, renewal_tick_one must arm a deadline
 * and advance the backoff schedule -- proving the failure path is
 * wired, not just the decision to attempt.
 */
START_TEST(test_tick_no_session_no_backoff_attempts_reconnect)
{
	struct reffs_proxy_mds_config c = make_cfg(52);

	strncpy(c.address, "127.0.0.1", sizeof(c.address) - 1);
	c.mds_port = 1;
	ck_assert_int_eq(ps_state_register(&c), 0);

	struct ps_listener_state *pls =
		(struct ps_listener_state *)ps_state_find(52);

	ck_assert_ptr_nonnull(pls);
	ck_assert_uint_eq(pls->pls_reconnect_next_attempt_ns, 0);
	ck_assert_uint_eq(pls->pls_reconnect_backoff_sec, 0);

	struct renewal_tick_ctx ctx = { 0 };

	ck_assert_int_eq(renewal_tick_one(pls, &ctx), 0);

	ck_assert_uint_eq(ctx.skipped_no_session, 1);
	ck_assert_uint_eq(ctx.reconnect_attempted, 1);
	ck_assert_uint_eq(ctx.reconnect_skipped_backoff, 0);
	/* Connect refused -- reconnect could not succeed. */
	ck_assert_uint_eq(ctx.reconnect_succeeded, 0);
	/* Failure path armed a future deadline + advanced backoff. */
	ck_assert_uint_ne(pls->pls_reconnect_next_attempt_ns, 0);
	ck_assert_uint_ne(pls->pls_reconnect_backoff_sec, 0);
}
END_TEST

START_TEST(test_register_caches_tls_params)
{
	struct reffs_proxy_mds_config c = make_cfg(16);

	strncpy(c.tls_cert, "/tmp/cert.pem", sizeof(c.tls_cert) - 1);
	strncpy(c.tls_key, "/tmp/key.pem", sizeof(c.tls_key) - 1);
	strncpy(c.tls_ca, "/tmp/ca.pem", sizeof(c.tls_ca) - 1);
	c.tls_mode = 2; /* DIRECT */
	c.tls_insecure_no_verify = true;

	ck_assert_int_eq(ps_state_register(&c), 0);

	const struct ps_listener_state *pls = ps_state_find(16);

	ck_assert_ptr_nonnull(pls);
	ck_assert_str_eq(pls->pls_tls_cert, "/tmp/cert.pem");
	ck_assert_str_eq(pls->pls_tls_key, "/tmp/key.pem");
	ck_assert_str_eq(pls->pls_tls_ca, "/tmp/ca.pem");
	ck_assert_int_eq(pls->pls_tls_mode, 2);
	ck_assert_int_eq(pls->pls_tls_insecure_no_verify, 1);
	/* Backoff fields zero-initialised at registration. */
	ck_assert_uint_eq(pls->pls_reconnect_backoff_sec, 0);
	ck_assert_uint_eq(pls->pls_reconnect_next_attempt_ns, 0);
}
END_TEST

static Suite *ps_reconnect_suite(void)
{
	Suite *s = suite_create("ps_reconnect");

	TCase *classify = tcase_create("classify");

	tcase_add_test(classify, test_classify_session_dead_status);
	tcase_add_test(classify, test_classify_per_op_transient);
	tcase_add_test(classify, test_classify_connection_lost);
	tcase_add_test(classify, test_classify_unrelated_errno);
	suite_add_tcase(s, classify);

	TCase *backoff = tcase_create("backoff");

	tcase_add_test(backoff, test_backoff_progression);
	tcase_add_test(backoff, test_backoff_reset);
	tcase_add_test(backoff, test_backoff_null_safe);
	suite_add_tcase(s, backoff);

	TCase *borrow = tcase_create("borrow");

	tcase_add_checked_fixture(borrow, setup, teardown);
	tcase_add_test(borrow, test_borrow_returns_null_when_no_session);
	tcase_add_test(borrow, test_borrow_returns_null_for_unknown_listener);
	tcase_add_test(borrow, test_borrow_after_set_session_returns_pointer);
	tcase_add_test(borrow, test_concurrent_readers);
	tcase_add_test(borrow, test_replace_unknown_listener);
	tcase_add_test(borrow, test_replace_null_to_null);
	suite_add_tcase(s, borrow);

	TCase *regid = tcase_create("registration_id");

	tcase_add_checked_fixture(regid, setup, teardown);
	tcase_add_test(regid, test_set_registration_id_roundtrip);
	tcase_add_test(regid, test_set_registration_id_unknown_listener);
	tcase_add_test(regid, test_set_registration_id_too_long);
	tcase_add_test(regid, test_set_registration_id_clear);
	tcase_add_test(regid, test_register_caches_tls_params);
	suite_add_tcase(s, regid);

	TCase *quiesce = tcase_create("quiesce");

	tcase_add_checked_fixture(quiesce, setup, teardown);
	tcase_add_test(quiesce, test_session_replace_quiesces_in_flight);
	suite_add_tcase(s, quiesce);

	TCase *renewal = tcase_create("renewal_thread");

	/*
	 * Fixture wraps every test so the registry starts clean and is
	 * fully torn down between cases -- the tick tests register
	 * listeners with overlapping ids that must not leak across
	 * runs.  start/stop tests don't register listeners but share
	 * the fixture for symmetry.
	 *
	 * Bump the per-test budget a little -- the renewal thread
	 * spins up a real pthread + cond + mutex, and the
	 * attempts-reconnect test issues a real connect() to a
	 * refused port.  Default is 4s; we stay well under 2s but
	 * give headroom for slow CI.
	 */
	tcase_add_checked_fixture(renewal, setup, teardown);
	tcase_set_timeout(renewal, 5);
	tcase_add_test(renewal, test_renewal_start_zero_interval_invalid);
	tcase_add_test(renewal, test_renewal_start_idempotent);
	tcase_add_test(renewal, test_renewal_stop_when_not_running);
	tcase_add_test(renewal, test_tick_no_session_no_upstream);
	tcase_add_test(renewal, test_tick_no_session_in_backoff);
	tcase_add_test(renewal,
		       test_tick_no_session_no_backoff_attempts_reconnect);
	suite_add_tcase(s, renewal);

	return s;
}

int main(void)
{
	int failed;
	Suite *s = ps_reconnect_suite();
	SRunner *sr = srunner_create(s);

	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return failed ? 1 : 0;
}
