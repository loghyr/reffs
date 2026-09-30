/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Unit tests for cb_pending lifecycle and race-safety primitives.
 *
 * Tests the allocation, try_complete CAS, and free paths without
 * needing a running server or backchannel connection.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include <check.h>

#include "nfsv42_xdr.h"
#include "nfs4/cb.h"
#include "reffs/io.h"
#include "reffs/rpc.h"
#include "reffs/task.h"

START_TEST(test_old_stateid_reaches_callback_decoder)
{
	struct cb_pending *cp =
		cb_pending_alloc(NULL, NULL, OP_CB_LAYOUTRECALL);
	struct rpc_trans *out = rpc_trans_create();
	CB_COMPOUND4res reply = { .status = NFS4ERR_OLD_STATEID };
	char packet[256] = { 0 };
	XDR xdr;
	uint32_t *header = (uint32_t *)packet;

	ck_assert_ptr_nonnull(cp);
	ck_assert_ptr_nonnull(out);
	free(out->rt_context);
	out->rt_context = cp;
	cb_pending_get(cp);
	out->rt_raw_reply = true;
	out->rt_cb = cb_reply_handler;
	out->rt_info.ri_xid = 0x10000001;
	ck_assert_int_eq(io_register_request(out), 0);
	/* Suppress queueing a task after decode; this test owns cp. */
	atomic_store(&cp->cp_status, -ECANCELED);
	header[0] = htonl(out->rt_info.ri_xid);
	header[1] = htonl(1); /* RPC REPLY */
	header[2] = htonl(0); /* MSG_ACCEPTED */
	header[3] = htonl(0); /* AUTH_NONE */
	header[4] = htonl(0); /* verifier length */
	header[5] = htonl(0); /* SUCCESS */
	xdrmem_create(&xdr, packet + 24, sizeof(packet) - 24, XDR_ENCODE);
	ck_assert(xdr_CB_COMPOUND4res(&xdr, &reply));
	struct task task = {
		.t_buffer = packet,
		.t_bytes_read = (int)(24 + xdr_getpos(&xdr)),
	};

	xdr_destroy(&xdr);
	ck_assert_int_eq(rpc_process_task(&task), 0);
	ck_assert_int_eq(cp->cp_res.status, NFS4ERR_OLD_STATEID);
	ck_assert_int_eq(atomic_load(&cp->cp_status), -ECANCELED);
	cb_pending_free(cp);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Alloc / free                                                        */
/* ------------------------------------------------------------------ */

START_TEST(test_alloc_basic)
{
	struct cb_pending *cp = cb_pending_alloc(NULL, NULL, OP_CB_GETATTR);

	ck_assert_ptr_nonnull(cp);
	ck_assert_ptr_null(cp->cp_task);
	ck_assert_ptr_null(cp->cp_compound);
	ck_assert_uint_eq(cp->cp_op, OP_CB_GETATTR);
	ck_assert_int_eq(atomic_load(&cp->cp_status), CB_PENDING_INFLIGHT);
	ck_assert_uint_eq(cp->cp_xid, 0);

	cb_pending_free(cp);
}
END_TEST

START_TEST(test_free_null)
{
	/* Must not crash. */
	cb_pending_free(NULL);
}
END_TEST

/* ------------------------------------------------------------------ */
/* try_complete -- single winner                                        */
/* ------------------------------------------------------------------ */

START_TEST(test_try_complete_success)
{
	struct cb_pending *cp = cb_pending_alloc(NULL, NULL, OP_CB_GETATTR);

	/* First try should win. */
	int won = cb_pending_try_complete(cp, 0);

	ck_assert_int_ne(won, 0);
	ck_assert_int_eq(atomic_load(&cp->cp_status), 0);

	cb_pending_free(cp);
}
END_TEST

START_TEST(test_try_complete_timeout)
{
	struct cb_pending *cp = cb_pending_alloc(NULL, NULL, OP_CB_GETATTR);

	int won = cb_pending_try_complete(cp, -ETIMEDOUT);

	ck_assert_int_ne(won, 0);
	ck_assert_int_eq(atomic_load(&cp->cp_status), -ETIMEDOUT);

	cb_pending_free(cp);
}
END_TEST

/* ------------------------------------------------------------------ */
/* try_complete -- race: only one winner                                */
/* ------------------------------------------------------------------ */

START_TEST(test_try_complete_double_race)
{
	struct cb_pending *cp = cb_pending_alloc(NULL, NULL, OP_CB_GETATTR);

	/* First caller (reply handler) wins with status 0. */
	int won1 = cb_pending_try_complete(cp, 0);

	ck_assert_int_ne(won1, 0);

	/* Second caller (timeout) loses. */
	int won2 = cb_pending_try_complete(cp, -ETIMEDOUT);

	ck_assert_int_eq(won2, 0);

	/* Status stays at 0 (first winner's value). */
	ck_assert_int_eq(atomic_load(&cp->cp_status), 0);

	cb_pending_free(cp);
}
END_TEST

START_TEST(test_try_complete_timeout_wins_reply_loses)
{
	struct cb_pending *cp = cb_pending_alloc(NULL, NULL, OP_CB_GETATTR);

	/* Timeout fires first. */
	int won1 = cb_pending_try_complete(cp, -ETIMEDOUT);

	ck_assert_int_ne(won1, 0);

	/* Late reply loses. */
	int won2 = cb_pending_try_complete(cp, 0);

	ck_assert_int_eq(won2, 0);

	/* Status stays at -ETIMEDOUT. */
	ck_assert_int_eq(atomic_load(&cp->cp_status), -ETIMEDOUT);

	cb_pending_free(cp);
}
END_TEST

/* ------------------------------------------------------------------ */
/* try_complete -- cannot complete if already completed                  */
/* ------------------------------------------------------------------ */

START_TEST(test_try_complete_eio)
{
	struct cb_pending *cp = cb_pending_alloc(NULL, NULL, OP_CB_GETATTR);

	int won = cb_pending_try_complete(cp, -EIO);

	ck_assert_int_ne(won, 0);
	ck_assert_int_eq(atomic_load(&cp->cp_status), -EIO);

	/* Cannot complete again. */
	int won2 = cb_pending_try_complete(cp, 0);

	ck_assert_int_eq(won2, 0);
	ck_assert_int_eq(atomic_load(&cp->cp_status), -EIO);

	cb_pending_free(cp);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                                */
/* ------------------------------------------------------------------ */

static Suite *cb_pending_suite(void)
{
	Suite *s = suite_create("cb_pending");
	TCase *tc = tcase_create("core");

	tcase_add_test(tc, test_alloc_basic);
	tcase_add_test(tc, test_free_null);
	tcase_add_test(tc, test_try_complete_success);
	tcase_add_test(tc, test_try_complete_timeout);
	tcase_add_test(tc, test_try_complete_double_race);
	tcase_add_test(tc, test_try_complete_timeout_wins_reply_loses);
	tcase_add_test(tc, test_try_complete_eio);
	tcase_add_test(tc, test_old_stateid_reaches_callback_decoder);

	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	Suite *s = cb_pending_suite();
	SRunner *sr = srunner_create(s);

	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return (failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
