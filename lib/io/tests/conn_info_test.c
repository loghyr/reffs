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

#include "reffs/log.h"
#include "reffs/io.h"

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

static void setup(void)
{
	io_conn_init();
}

static void teardown(void)
{
	io_conn_cleanup();
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
	ck_assert_ptr_null(io_conn_write_done(FD_A));

	/* Gate should be free again. */
	struct io_context ic2 = { 0 };
	ck_assert(io_conn_write_try_start(FD_A, &ic2));
	ck_assert_ptr_null(io_conn_write_done(FD_A));
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
	struct io_context *next = io_conn_write_done(FD_A);
	ck_assert_ptr_eq(next, &ic2);

	next = io_conn_write_done(FD_A);
	ck_assert_ptr_eq(next, &ic3);

	/* Queue empty now. */
	ck_assert_ptr_null(io_conn_write_done(FD_A));
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
