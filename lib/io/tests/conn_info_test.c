/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * conn_info bookkeeping tests.
 *
 * Exercises the backend-agnostic connection-tracking module extracted
 * into lib/io/conn_info.c:
 *
 *   - register / get / unregister lifecycle
 *   - add/remove read/write/accept/connect op counts
 *   - state machine transitions driven by op counts
 *   - error-state transition
 *   - has_read_ops / has_write_ops queries
 *   - per-fd write serialization gate (try_start / done)
 *
 * These tests cover the FreeBSD kqueue backend's bookkeeping (previously
 * stubs) and remain valid on Linux since the same source file is shared.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <check.h>

#include <openssl/ssl.h>

#include "reffs/log.h"
#include "reffs/io.h"

#include "io_internal.h"

/*
 * Three distinct fds well away from stdio (0/1/2) and from each other's
 * MAX_CONNECTIONS hash slot to avoid collision artefacts.  Chosen by
 * hand; if MAX_CONNECTIONS ever gets small enough to collide, the
 * io_conn_register collision branch will log and return NULL, which
 * the tests would catch immediately.
 */
#define FD_A 201
#define FD_B 202
#define FD_C 203

/*
 * Backs the io_conn_ssl_* lifecycle tests with real SSL objects.
 * Created per-test in setup() so install/acquire/clear exercise the
 * actual OpenSSL refcount path the fix relies on.
 */
static SSL_CTX *test_ctx;

static void setup(void)
{
	io_conn_init();
	test_ctx = SSL_CTX_new(TLS_method());
}

static void teardown(void)
{
	/*
	 * io_conn_cleanup() first: it SSL_free()s any object still in a
	 * slot, which drops the SSL's ref on test_ctx -- so the ctx must
	 * outlive it.
	 */
	io_conn_cleanup();
	if (test_ctx) {
		SSL_CTX_free(test_ctx);
		test_ctx = NULL;
	}
}

START_TEST(test_register_and_get)
{
	struct conn_info *ci =
		io_conn_register(FD_A, CONN_CONNECTING, CONN_ROLE_CLIENT);
	ck_assert_ptr_nonnull(ci);
	ck_assert_int_eq(ci->ci_fd, FD_A);
	ck_assert_int_eq(ci->ci_state, CONN_CONNECTING);
	ck_assert_int_eq(ci->ci_role, CONN_ROLE_CLIENT);
	ck_assert_int_eq(ci->ci_read_count, 0);
	ck_assert_int_eq(ci->ci_write_count, 0);

	struct conn_info *got = io_conn_get(FD_A);
	ck_assert_ptr_eq(ci, got);

	ck_assert_ptr_null(io_conn_get(FD_B));
}
END_TEST

START_TEST(test_unregister_clears_slot)
{
	(void)io_conn_register(FD_A, CONN_CONNECTED, CONN_ROLE_CLIENT);
	ck_assert_ptr_nonnull(io_conn_get(FD_A));

	ck_assert_int_eq(io_conn_unregister(FD_A), 0);
	ck_assert_ptr_null(io_conn_get(FD_A));

	/* Unregister of an unknown fd is a no-op error. */
	ck_assert_int_eq(io_conn_unregister(FD_B), -1);
}
END_TEST

START_TEST(test_add_remove_read_op)
{
	(void)io_conn_register(FD_A, CONN_CONNECTED, CONN_ROLE_CLIENT);

	ck_assert_int_eq(io_conn_add_read_op(FD_A), 0);
	ck_assert_int_eq(io_conn_add_read_op(FD_A), 0);
	struct conn_info *ci = io_conn_get(FD_A);
	ck_assert_int_eq(ci->ci_read_count, 2);
	ck_assert_int_eq(ci->ci_state, CONN_READING);
	ck_assert(io_conn_has_read_ops(FD_A));

	ck_assert_int_eq(io_conn_remove_read_op(FD_A), 0);
	ck_assert_int_eq(ci->ci_read_count, 1);
	ck_assert(io_conn_has_read_ops(FD_A));

	ck_assert_int_eq(io_conn_remove_read_op(FD_A), 0);
	ck_assert_int_eq(ci->ci_read_count, 0);
	ck_assert(!io_conn_has_read_ops(FD_A));
	ck_assert_int_eq(ci->ci_state, CONN_CONNECTED);
}
END_TEST

START_TEST(test_add_remove_write_op)
{
	(void)io_conn_register(FD_A, CONN_CONNECTED, CONN_ROLE_CLIENT);

	ck_assert_int_eq(io_conn_add_write_op(FD_A), 0);
	struct conn_info *ci = io_conn_get(FD_A);
	ck_assert_int_eq(ci->ci_write_count, 1);
	ck_assert_int_eq(ci->ci_state, CONN_WRITING);
	ck_assert(io_conn_has_write_ops(FD_A));

	ck_assert_int_eq(io_conn_remove_write_op(FD_A), 0);
	ck_assert_int_eq(ci->ci_write_count, 0);
	ck_assert(!io_conn_has_write_ops(FD_A));
	ck_assert_int_eq(ci->ci_state, CONN_CONNECTED);
}
END_TEST

START_TEST(test_readwrite_state)
{
	(void)io_conn_register(FD_A, CONN_CONNECTED, CONN_ROLE_CLIENT);

	ck_assert_int_eq(io_conn_add_read_op(FD_A), 0);
	ck_assert_int_eq(io_conn_add_write_op(FD_A), 0);
	struct conn_info *ci = io_conn_get(FD_A);
	ck_assert_int_eq(ci->ci_state, CONN_READWRITE);

	ck_assert_int_eq(io_conn_remove_read_op(FD_A), 0);
	ck_assert_int_eq(ci->ci_state, CONN_WRITING);

	ck_assert_int_eq(io_conn_remove_write_op(FD_A), 0);
	ck_assert_int_eq(ci->ci_state, CONN_CONNECTED);
}
END_TEST

START_TEST(test_accept_and_connect_ops)
{
	(void)io_conn_register(FD_A, CONN_LISTENING, CONN_ROLE_SERVER);

	ck_assert_int_eq(io_conn_add_accept_op(FD_A), 0);
	struct conn_info *ci = io_conn_get(FD_A);
	ck_assert_int_eq(ci->ci_accept_count, 1);
	ck_assert_int_eq(ci->ci_state, CONN_ACCEPTING);

	ck_assert_int_eq(io_conn_remove_accept_op(FD_A), 0);
	ck_assert_int_eq(ci->ci_accept_count, 0);
	ck_assert_int_eq(ci->ci_state, CONN_LISTENING);

	(void)io_conn_register(FD_B, CONN_UNUSED, CONN_ROLE_CLIENT);
	ck_assert_int_eq(io_conn_add_connect_op(FD_B), 0);
	struct conn_info *cb = io_conn_get(FD_B);
	ck_assert_int_eq(cb->ci_connect_count, 1);
	ck_assert_int_eq(cb->ci_state, CONN_CONNECTING);

	ck_assert_int_eq(io_conn_remove_connect_op(FD_B), 0);
	ck_assert_int_eq(cb->ci_connect_count, 0);
	ck_assert_int_eq(cb->ci_state, CONN_CONNECTED);
}
END_TEST

START_TEST(test_set_error)
{
	(void)io_conn_register(FD_A, CONN_CONNECTED, CONN_ROLE_CLIENT);

	ck_assert_int_eq(io_conn_set_error(FD_A, 42), 0);
	struct conn_info *ci = io_conn_get(FD_A);
	ck_assert_int_eq(ci->ci_state, CONN_ERROR);
	ck_assert_int_eq(ci->ci_error, 42);
	ck_assert(io_conn_is_state(FD_A, CONN_ERROR));
	ck_assert(!io_conn_is_state(FD_A, CONN_CONNECTED));
}
END_TEST

START_TEST(test_ops_on_unknown_fd)
{
	/* Every op on an unregistered fd should fail cleanly. */
	ck_assert_int_eq(io_conn_add_read_op(FD_C), -1);
	ck_assert_int_eq(io_conn_remove_read_op(FD_C), -1);
	ck_assert_int_eq(io_conn_add_write_op(FD_C), -1);
	ck_assert_int_eq(io_conn_remove_write_op(FD_C), -1);
	ck_assert_int_eq(io_conn_set_error(FD_C, 1), -1);
	ck_assert(!io_conn_has_read_ops(FD_C));
	ck_assert(!io_conn_has_write_ops(FD_C));
	ck_assert(!io_conn_is_state(FD_C, CONN_CONNECTED));
}
END_TEST

START_TEST(test_write_gate_single)
{
	(void)io_conn_register(FD_A, CONN_CONNECTED, CONN_ROLE_CLIENT);

	/* First writer gets the gate. */
	struct io_context ic1 = { 0 };
	ck_assert(io_conn_write_try_start(FD_A, &ic1));

	/* Queue is empty -- done releases the gate and returns NULL. */
	ck_assert_ptr_null(io_conn_write_done(FD_A, ic1.ic_write_gen));

	/* Gate should be free again. */
	struct io_context ic2 = { 0 };
	ck_assert(io_conn_write_try_start(FD_A, &ic2));
	ck_assert_ptr_null(io_conn_write_done(FD_A, ic2.ic_write_gen));
}
END_TEST

START_TEST(test_write_gate_contended)
{
	(void)io_conn_register(FD_A, CONN_CONNECTED, CONN_ROLE_CLIENT);

	struct io_context ic1 = { 0 };
	struct io_context ic2 = { 0 };
	struct io_context ic3 = { 0 };

	ck_assert(io_conn_write_try_start(FD_A, &ic1));
	/* ic2 and ic3 queue behind ic1 since the gate is held. */
	ck_assert(!io_conn_write_try_start(FD_A, &ic2));
	ck_assert(!io_conn_write_try_start(FD_A, &ic3));

	/* Handoff goes to ic2 then ic3 in FIFO order. */
	struct io_context *next = io_conn_write_done(FD_A, ic1.ic_write_gen);
	ck_assert_ptr_eq(next, &ic2);

	next = io_conn_write_done(FD_A, ic2.ic_write_gen);
	ck_assert_ptr_eq(next, &ic3);

	/* Queue empty now. */
	ck_assert_ptr_null(io_conn_write_done(FD_A, ic3.ic_write_gen));
}
END_TEST

START_TEST(test_write_gate_fd_reuse)
{
	/*
	 * Simulate a stale write CQE arriving after the fd was closed and
	 * reused for a new connection.  done() with the old generation must
	 * not release the new connection's gate.
	 */
	(void)io_conn_register(FD_A, CONN_CONNECTED, CONN_ROLE_CLIENT);
	struct io_context ic_old = { 0 };
	ck_assert(io_conn_write_try_start(FD_A, &ic_old));
	uint32_t old_gen = ic_old.ic_write_gen;

	io_conn_unregister(FD_A);

	/* Same fd reused for a new connection -- generation must differ. */
	(void)io_conn_register(FD_A, CONN_CONNECTED, CONN_ROLE_CLIENT);
	struct io_context ic_new = { 0 };
	ck_assert(io_conn_write_try_start(FD_A, &ic_new));
	ck_assert_int_ne((int)old_gen, (int)ic_new.ic_write_gen);

	/* Stale done() with old generation must not release the new gate. */
	ck_assert_ptr_null(io_conn_write_done(FD_A, old_gen));

	/* Gate is still active -- a new writer must queue. */
	struct io_context ic_queued = { 0 };
	ck_assert(!io_conn_write_try_start(FD_A, &ic_queued));

	/* Correct done() hands off to the queued writer. */
	struct io_context *next = io_conn_write_done(FD_A, ic_new.ic_write_gen);
	ck_assert_ptr_eq(next, &ic_queued);
	ck_assert_ptr_null(io_conn_write_done(FD_A, ic_queued.ic_write_gen));
}
END_TEST

START_TEST(test_write_gate_partial_write_continuation)
{
	/*
	 * Simulate a partial-write continuation: io_handle_write creates a new
	 * io_context, propagates IO_CONTEXT_WRITE_OWNED, and must also copy
	 * ic_write_gen.  If ic_write_gen is zero (calloc default) on the
	 * continuation, io_conn_write_done() will see a generation mismatch and
	 * never release the gate.
	 */
	(void)io_conn_register(FD_A, CONN_CONNECTED, CONN_ROLE_CLIENT);

	struct io_context ic_orig = { 0 };
	ck_assert(io_conn_write_try_start(FD_A, &ic_orig));

	/* Simulate the partial-write continuation: new context copies gen. */
	struct io_context ic_cont = { 0 };
	ic_cont.ic_write_gen = ic_orig.ic_write_gen;

	/* done() via the continuation must release the gate. */
	ck_assert_ptr_null(io_conn_write_done(FD_A, ic_cont.ic_write_gen));

	/* Gate is free -- next writer gets it immediately. */
	struct io_context ic_next = { 0 };
	ck_assert(io_conn_write_try_start(FD_A, &ic_next));
	ck_assert_ptr_null(io_conn_write_done(FD_A, ic_next.ic_write_gen));
}
END_TEST

/*
 * SSL-object lifecycle tests (INV-5 / INV-6 fix).  These exercise the
 * io_conn_ssl_* / io_conn_tls_* accessors that replaced unguarded
 * ci_ssl access.  The defect they guard against: io_conn_get()
 * returned a raw conn_info whose ci_ssl a consumer dereferenced after
 * conn_mutex was dropped, racing a concurrent SSL_free in teardown.
 */
START_TEST(test_ssl_install_acquire_release)
{
	(void)io_conn_register(FD_A, CONN_CONNECTED, CONN_ROLE_SERVER);

	SSL *ssl = SSL_new(test_ctx);
	ck_assert_ptr_nonnull(ssl);
	io_conn_ssl_install(FD_A, ssl);

	/* install marks the connection handshaking, not yet enabled. */
	bool enabled = true, handshaking = false;
	ck_assert(io_conn_tls_snapshot(FD_A, &enabled, &handshaking));
	ck_assert(!enabled);
	ck_assert(handshaking);

	/* acquire returns the installed SSL with a use-ref held. */
	SSL *got = io_conn_ssl_acquire(FD_A);
	ck_assert_ptr_eq(got, ssl);
	io_conn_ssl_release(got);

	/* clear drops the slot ref and frees the SSL; acquire now NULL. */
	io_conn_ssl_clear(FD_A);
	ck_assert_ptr_null(io_conn_ssl_acquire(FD_A));
}
END_TEST

START_TEST(test_ssl_acquire_unregistered)
{
	/* No connection registered for FD_C. */
	ck_assert_ptr_null(io_conn_ssl_acquire(FD_C));
	/* snapshot reports the fd as absent. */
	ck_assert(!io_conn_tls_snapshot(FD_C, NULL, NULL));
}
END_TEST

START_TEST(test_ssl_acquire_no_ssl)
{
	/* Registered, but no SSL installed -- acquire returns NULL. */
	(void)io_conn_register(FD_A, CONN_CONNECTED, CONN_ROLE_SERVER);
	ck_assert_ptr_null(io_conn_ssl_acquire(FD_A));
}
END_TEST

START_TEST(test_ssl_clear_idempotent)
{
	(void)io_conn_register(FD_A, CONN_CONNECTED, CONN_ROLE_SERVER);

	SSL *ssl = SSL_new(test_ctx);
	ck_assert_ptr_nonnull(ssl);
	io_conn_ssl_install(FD_A, ssl);

	io_conn_ssl_clear(FD_A);
	/* Second clear is a no-op -- no double free. */
	io_conn_ssl_clear(FD_A);

	/* Clear on a registered fd that never had an SSL is also safe. */
	(void)io_conn_register(FD_B, CONN_CONNECTED, CONN_ROLE_SERVER);
	io_conn_ssl_clear(FD_B);
}
END_TEST

START_TEST(test_ssl_clear_with_outstanding_ref)
{
	/*
	 * The core INV-5 / INV-6 safety property: a use-ref taken by
	 * io_conn_ssl_acquire() keeps the SSL object alive even after
	 * io_conn_ssl_clear() drops the slot's ref.  The acquirer may
	 * safely dereference the SSL until it releases.
	 */
	(void)io_conn_register(FD_A, CONN_CONNECTED, CONN_ROLE_SERVER);

	SSL *ssl = SSL_new(test_ctx);
	ck_assert_ptr_nonnull(ssl);
	io_conn_ssl_install(FD_A, ssl);

	SSL *use = io_conn_ssl_acquire(FD_A);
	ck_assert_ptr_eq(use, ssl);

	/* Drop the slot's ref while the use-ref is still outstanding. */
	io_conn_ssl_clear(FD_A);

	/* The SSL must still be valid -- dereference it (no UAF). */
	ck_assert_ptr_nonnull(SSL_get_version(use));

	/* Releasing the last (use) ref frees it; ASAN/LSAN verify. */
	io_conn_ssl_release(use);
}
END_TEST

START_TEST(test_ssl_unregister_clears_ssl)
{
	(void)io_conn_register(FD_A, CONN_CONNECTED, CONN_ROLE_SERVER);

	SSL *ssl = SSL_new(test_ctx);
	ck_assert_ptr_nonnull(ssl);
	io_conn_ssl_install(FD_A, ssl);

	/* unregister tears down the SSL along with the slot. */
	ck_assert_int_eq(io_conn_unregister(FD_A), 0);
	ck_assert_ptr_null(io_conn_ssl_acquire(FD_A));
}
END_TEST

START_TEST(test_tls_snapshot)
{
	(void)io_conn_register(FD_A, CONN_CONNECTED, CONN_ROLE_SERVER);

	bool enabled = true, handshaking = true;

	/* Fresh connection: both flags clear. */
	ck_assert(io_conn_tls_snapshot(FD_A, &enabled, &handshaking));
	ck_assert(!enabled);
	ck_assert(!handshaking);

	/* set_state writes the flags as a pair. */
	io_conn_tls_set_state(FD_A, true, false);
	ck_assert(io_conn_tls_snapshot(FD_A, &enabled, &handshaking));
	ck_assert(enabled);
	ck_assert(!handshaking);

	/* Unregistered fd: snapshot returns false. */
	ck_assert(!io_conn_tls_snapshot(FD_B, &enabled, &handshaking));
}
END_TEST

/*
 * CONN_CLOSING force-drain (conn-info-closing-wedge.md, Slice 1).
 *
 * io_conn_unregister leaves a slot in CONN_CLOSING with its
 * in-flight op counters intact; the slot only reaches CONN_UNUSED
 * once every counter drains to zero (conn_drain_if_idle_locked).
 * If a counter leaks -- a CQE-completion path that skips its
 * io_conn_remove_*_op -- the slot is wedged forever and
 * io_conn_register refuses to reuse it.  io_conn_check_timeouts
 * is the backstop: a CONN_CLOSING slot idle past the closing
 * deadline is force-drained back to CONN_UNUSED.
 *
 * wedge_slot() reproduces the leak deterministically: register,
 * add two read ops, remove only one, unregister.  The slot lands
 * in CONN_CLOSING with ci_read_count == 1 and never drains on its
 * own.  A wedged slot is observable through io_conn_register:
 * it returns NULL while the slot is CONN_CLOSING and non-NULL
 * once the slot is reclaimed.
 *
 * The closing deadline passed to io_conn_check_timeouts is -1 in
 * the force-drain tests: a slot's age is now - ci_last_activity,
 * always >= 0, so a -1 deadline force-drains every CONN_CLOSING
 * slot regardless of age.  That keeps the tests free of any sleep
 * (the freshly-wedged slot is 0 s old).  The deadline IS exercised
 * from the other side by test_closing_not_force_drained_before_
 * timeout, which passes a positive deadline and asserts the young
 * slot is spared.
 */
static void wedge_slot(int fd)
{
	(void)io_conn_register(fd, CONN_CONNECTED, CONN_ROLE_CLIENT);
	ck_assert_int_eq(io_conn_add_read_op(fd), 0);
	ck_assert_int_eq(io_conn_add_read_op(fd), 0);
	/* Remove only one of the two -- simulates the Bug-A leak. */
	ck_assert_int_eq(io_conn_remove_read_op(fd), 0);
	ck_assert_int_eq(io_conn_unregister(fd), 0);
}

START_TEST(test_closing_drain_completes_when_balanced)
{
	/* Healthy path: every add paired with a remove -> the slot
	 * drains to CONN_UNUSED the instant io_conn_unregister runs,
	 * with no force-drain needed. */
	(void)io_conn_register(FD_A, CONN_CONNECTED, CONN_ROLE_CLIENT);
	ck_assert_int_eq(io_conn_add_read_op(FD_A), 0);
	ck_assert_int_eq(io_conn_add_read_op(FD_A), 0);
	ck_assert_int_eq(io_conn_remove_read_op(FD_A), 0);
	ck_assert_int_eq(io_conn_remove_read_op(FD_A), 0);
	ck_assert_int_eq(io_conn_unregister(FD_A), 0);

	/* Slot is CONN_UNUSED: immediately re-registerable. */
	ck_assert_ptr_nonnull(
		io_conn_register(FD_A, CONN_CONNECTED, CONN_ROLE_CLIENT));
}
END_TEST

START_TEST(test_closing_force_drain_reclaims_wedged_slot)
{
	wedge_slot(FD_A);

	/* Wedged: io_conn_register refuses the CONN_CLOSING slot. */
	ck_assert_ptr_null(
		io_conn_register(FD_A, CONN_CONNECTED, CONN_ROLE_CLIENT));

	/* Sweep: closing deadline -1 force-drains regardless of age,
	 * idle deadline huge so no live-idle slot is touched. */
	int reaped = io_conn_check_timeouts(3600, -1);
	ck_assert_int_ge(reaped, 1);

	/* Slot reclaimed -> registration now succeeds. */
	ck_assert_ptr_nonnull(
		io_conn_register(FD_A, CONN_CONNECTED, CONN_ROLE_CLIENT));
}
END_TEST

START_TEST(test_closing_not_force_drained_before_timeout)
{
	wedge_slot(FD_A);

	/* Slot just entered CONN_CLOSING; with a 5 s closing deadline
	 * it is too young to force-drain -- a drain still in progress
	 * must not be reaped out from under itself. */
	int reaped = io_conn_check_timeouts(3600, 5);
	ck_assert_int_eq(reaped, 0);

	/* Still wedged. */
	ck_assert_ptr_null(
		io_conn_register(FD_A, CONN_CONNECTED, CONN_ROLE_CLIENT));
}
END_TEST

START_TEST(test_closing_force_drain_spares_live_connections)
{
	/* A live CONN_CONNECTED connection alongside a wedged slot:
	 * the closing force-drain must reclaim the wedged slot without
	 * disturbing the live one (idle deadline huge). */
	(void)io_conn_register(FD_B, CONN_CONNECTED, CONN_ROLE_CLIENT);
	wedge_slot(FD_A);

	int reaped = io_conn_check_timeouts(3600, -1);
	ck_assert_int_ge(reaped, 1);

	/* FD_A reclaimed; FD_B untouched and still live. */
	ck_assert_ptr_nonnull(
		io_conn_register(FD_A, CONN_CONNECTED, CONN_ROLE_CLIENT));
	struct conn_info *live = io_conn_get(FD_B);

	ck_assert_ptr_nonnull(live);
	ck_assert_int_eq(live->ci_state, CONN_CONNECTED);
}
END_TEST

static Suite *conn_info_suite(void)
{
	Suite *s = suite_create("conn_info");
	TCase *tc = tcase_create("core");
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_register_and_get);
	tcase_add_test(tc, test_unregister_clears_slot);
	tcase_add_test(tc, test_add_remove_read_op);
	tcase_add_test(tc, test_add_remove_write_op);
	tcase_add_test(tc, test_readwrite_state);
	tcase_add_test(tc, test_accept_and_connect_ops);
	tcase_add_test(tc, test_set_error);
	tcase_add_test(tc, test_ops_on_unknown_fd);
	tcase_add_test(tc, test_write_gate_single);
	tcase_add_test(tc, test_write_gate_contended);
	tcase_add_test(tc, test_write_gate_fd_reuse);
	tcase_add_test(tc, test_write_gate_partial_write_continuation);
	tcase_add_test(tc, test_ssl_install_acquire_release);
	tcase_add_test(tc, test_ssl_acquire_unregistered);
	tcase_add_test(tc, test_ssl_acquire_no_ssl);
	tcase_add_test(tc, test_ssl_clear_idempotent);
	tcase_add_test(tc, test_ssl_clear_with_outstanding_ref);
	tcase_add_test(tc, test_ssl_unregister_clears_ssl);
	tcase_add_test(tc, test_tls_snapshot);
	tcase_add_test(tc, test_closing_drain_completes_when_balanced);
	tcase_add_test(tc, test_closing_force_drain_reclaims_wedged_slot);
	tcase_add_test(tc, test_closing_not_force_drained_before_timeout);
	tcase_add_test(tc, test_closing_force_drain_spares_live_connections);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	Suite *s = conn_info_suite();
	SRunner *sr = srunner_create(s);
	srunner_run_all(sr, CK_NORMAL);
	int nfailed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return nfailed == 0 ? 0 : 1;
}
