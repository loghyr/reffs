/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * TLS write-count balance tests.
 *
 * Verifies that ci_write_count is correctly decremented when a
 * post-handshake IO_CONTEXT_DIRECT_TLS_DATA write CQE is processed
 * via io_handle_write().
 *
 * Background: io_request_write_op() calls io_context_create() (which
 * increments ci_write_count and sets ACTIVE), then overwrites ic_state
 * to IO_CONTEXT_DIRECT_TLS_DATA, clearing the ACTIVE bit.  When the
 * CQE arrives the handler must still call io_handle_write() so that
 * io_context_destroy() fires and decrements ci_write_count.  Skipping
 * that call caused unbounded growth of ci_write_count on TLS connections,
 * stalling all further I/O (observed with Olga Kornievskaia's cthon
 * basic test against hs-124, 2026-04-15).
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
 * Fake fd that is unlikely to collide with any real open fd in the
 * test process.  Must not be 0/1/2 (stdio).
 */
#define TEST_FD 199

static void write_count_setup(void)
{
	io_context_init();
	io_conn_init();
	io_conn_register(TEST_FD, CONN_CONNECTED, CONN_ROLE_CLIENT);
}

static void write_count_teardown(void)
{
	io_conn_cleanup();
}

/*
 * test_tls_direct_write_count_balance
 *
 * Simulates the lifecycle of an IO_CONTEXT_DIRECT_TLS_DATA context:
 *
 *  1. io_context_create() -- sets ACTIVE, increments write count
 *  2. overwrite ic_state -- strips ACTIVE (as io_request_write_op does)
 *  3. verify count == 1
 *  4. io_handle_write() with bytes_written == len -- drives io_context_destroy
 *  5. verify count == 0
 *
 * This is the exact sequence the event loop follows for every
 * post-handshake TLS data write.  Before the fix, step 4 was skipped
 * for non-handshake contexts, leaving the count permanently at 1.
 */
START_TEST(test_tls_direct_write_count_balance)
{
	const int len = 32;
	char *buf = malloc(len);
	ck_assert_ptr_nonnull(buf);
	memset(buf, 0xAB, len);

	/* Step 1: create context (increments write count, sets ACTIVE). */
	struct io_context *ic =
		io_context_create(OP_TYPE_WRITE, TEST_FD, buf, len);
	ck_assert_ptr_nonnull(ic);

	struct conn_info *ci = io_conn_get(TEST_FD);
	ck_assert_ptr_nonnull(ci);
	ck_assert_int_eq(ci->ci_write_count, 1);

	/*
	 * Step 2: overwrite state to IO_CONTEXT_DIRECT_TLS_DATA,
	 * clearing ACTIVE.  This mirrors io_request_write_op() line 184.
	 */
	ic->ic_state = IO_CONTEXT_DIRECT_TLS_DATA;

	/* Step 3: count is still 1 (incremented at create time). */
	ck_assert_int_eq(ci->ci_write_count, 1);

	/*
	 * Step 4: process the write completion.  Pass rc=NULL -- the
	 * IO_CONTEXT_DIRECT_TLS_DATA early-return path in io_handle_write
	 * does not use rc.
	 *
	 * io_handle_write() must call io_context_destroy(), which calls
	 * io_conn_remove_write_op() and decrements ci_write_count.
	 *
	 * ic is owned by io_handle_write() after this call.
	 */
	int ret = io_handle_write(ic, len, NULL);
	ck_assert_int_eq(ret, 0);

	/* Step 5: count back to 0. */
	ck_assert_int_eq(ci->ci_write_count, 0);
	/* buf was freed by io_context_destroy -- do not touch it. */
}
END_TEST

/*
 * test_tls_direct_write_count_multiple
 *
 * Three successive IO_CONTEXT_DIRECT_TLS_DATA write completions must
 * each decrement the count.  After all three the count must be zero.
 * Ensures there is no off-by-one or fence issue across multiple RPCs.
 */
START_TEST(test_tls_direct_write_count_multiple)
{
	const int len = 16;
	const int N = 3;

	struct conn_info *ci = io_conn_get(TEST_FD);
	ck_assert_ptr_nonnull(ci);

	for (int i = 0; i < N; i++) {
		char *buf = malloc(len);
		ck_assert_ptr_nonnull(buf);

		struct io_context *ic =
			io_context_create(OP_TYPE_WRITE, TEST_FD, buf, len);
		ck_assert_ptr_nonnull(ic);
		ic->ic_state = IO_CONTEXT_DIRECT_TLS_DATA;

		/* One pending write. */
		ck_assert_int_eq(ci->ci_write_count, 1);

		int ret = io_handle_write(ic, len, NULL);
		ck_assert_int_eq(ret, 0);

		/* Back to zero after each completion. */
		ck_assert_int_eq(ci->ci_write_count, 0);
	}
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

static Suite *write_count_suite(void)
{
	Suite *s = suite_create("tls_write_count");
	TCase *tc;

	tc = tcase_create("write_count");
	tcase_add_checked_fixture(tc, write_count_setup, write_count_teardown);
	tcase_add_test(tc, test_tls_direct_write_count_balance);
	tcase_add_test(tc, test_tls_direct_write_count_multiple);
	suite_add_tcase(s, tc);

	return s;
}

int main(void)
{
	Suite *s = write_count_suite();
	SRunner *sr = srunner_create(s);
	srunner_run_all(sr, CK_NORMAL);
	int nfailed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return nfailed == 0 ? 0 : 1;
}
