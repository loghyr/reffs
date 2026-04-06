/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Tests for the async compound state machine (dispatch_compound).
 *
 * These tests call dispatch_compound() directly with minimal struct compound
 * instances built on the stack/heap.  No NFS server setup is needed: the ops
 * exercised here (empty argarray, illegal opcode, resume callbacks) do not
 * access global server state such as sessions, inodes, or super-blocks.
 *
 * Test cases:
 *   1. Empty argarray    -- loop is never entered; res->status = NFS4_OK.
 *   2. Illegal opcode    -- nfs4_op_illegal fires; loop stops at op 0.
 *   3. Resume: ok        -- rt_next_action callback runs, c_curr_op advances.
 *   4. Resume: error     -- callback sets an error; res->status propagates.
 *   5. Resume: pause     -- callback calls task_pause(); dispatch yields;
 *                          c_curr_op stays at 0 for the next re-entry.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

#include <check.h>

#include "nfsv42_xdr.h"
#include "nfsv42_names.h" /* OP_MAX */
#include "reffs/rpc.h"
#include "reffs/server.h"
#include "reffs/task.h"
#include "nfs4/compound.h"
#include "nfs4/ops.h"
#include "nfs4_test_harness.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/*
 * Allocate a minimal compound for dispatch_compound() unit tests.
 * The caller owns all allocated memory and must free it after the test.
 *
 * nops     : number of argarray / resarray slots to allocate.
 * argop    : the nfs_opnum4 to install in every argarray slot.
 *
 * rt->rt_compound is wired to compound so resume callbacks can navigate
 * back to the compound via rt->rt_compound.
 */
static struct compound *make_compound(struct rpc_trans *rt, struct task *t,
				      unsigned int nops, nfs_opnum4 argop)
{
	struct compound *compound = calloc(1, sizeof(*compound));
	ck_assert_ptr_nonnull(compound);

	compound->c_rt = rt;
	compound->c_args = calloc(1, sizeof(COMPOUND4args));
	compound->c_res = calloc(1, sizeof(COMPOUND4res));
	ck_assert_ptr_nonnull(compound->c_args);
	ck_assert_ptr_nonnull(compound->c_res);

	if (nops > 0) {
		compound->c_args->argarray.argarray_len = nops;
		compound->c_args->argarray.argarray_val =
			calloc(nops, sizeof(nfs_argop4));
		ck_assert_ptr_nonnull(compound->c_args->argarray.argarray_val);

		for (unsigned int i = 0; i < nops; i++)
			compound->c_args->argarray.argarray_val[i].argop =
				argop;

		compound->c_res->resarray.resarray_len = nops;
		compound->c_res->resarray.resarray_val =
			calloc(nops, sizeof(nfs_resop4));
		ck_assert_ptr_nonnull(compound->c_res->resarray.resarray_val);
	}

	rt->rt_task = t;
	rt->rt_compound = compound;

	/*
	 * Provide a stub server_state so dispatch_compound() does not
	 * return NFS4ERR_DELAY.  The ops exercised here do not read any
	 * server_state fields; stats recording is safe on a zero-init'd
	 * struct because reffs_op_stats uses stdatomic.
	 */
	compound->c_server_state = calloc(1, sizeof(struct server_state));
	ck_assert_ptr_nonnull(compound->c_server_state);

	return compound;
}

static void free_compound(struct compound *compound)
{
	if (!compound)
		return;
	if (compound->c_args) {
		free(compound->c_args->argarray.argarray_val);
		free(compound->c_args);
	}
	if (compound->c_res) {
		free(compound->c_res->resarray.resarray_val);
		free(compound->c_res);
	}
	free(compound->c_server_state);
	free(compound);
}

/* ------------------------------------------------------------------ */
/* Callback helpers                                                    */
/* ------------------------------------------------------------------ */

static bool g_cb_called;
static unsigned g_cb_op; /* c_curr_op at time of call */

static uint32_t ok_action(struct rpc_trans *rt)
{
	struct compound *compound = rt->rt_compound;

	g_cb_called = true;
	g_cb_op = compound->c_curr_op;
	/* leave resop status = 0 (NFS4_OK) */
	return 0;
}

static uint32_t err_action(struct rpc_trans *rt)
{
	struct compound *compound = rt->rt_compound;
	nfs_resop4 *resop =
		&compound->c_res->resarray.resarray_val[compound->c_curr_op];

	g_cb_called = true;
	resop->nfs_resop4_u.opillegal.status = NFS4ERR_STALE;
	return 0;
}

static uint32_t pause_action(struct rpc_trans *rt)
{
	g_cb_called = true;
	task_pause(rt->rt_task);
	return NFS4_OP_FLAG_ASYNC;
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

/*
 * 1. Empty compound: the for loop never executes; res->status = NFS4_OK
 *    and resarray_len is set to 0.
 */
START_TEST(test_dispatch_empty)
{
	struct task task = { 0 };
	struct rpc_trans rt = { 0 };

	atomic_store_explicit(&task.t_state, TASK_RUNNING,
			      memory_order_relaxed);

	struct compound *compound = make_compound(&rt, &task, 0, 0);

	ck_assert(!dispatch_compound(compound));

	ck_assert_int_eq(compound->c_res->status, NFS4_OK);
	ck_assert_uint_eq(compound->c_res->resarray.resarray_len, 0);
	ck_assert_uint_eq(compound->c_curr_op, 0);

	free_compound(compound);
}
END_TEST

/*
 * 2. Illegal opcode: argop >= OP_MAX triggers nfs4_op_illegal(); the loop
 *    stops after op 0 and res->status = NFS4ERR_OP_ILLEGAL.
 */
START_TEST(test_dispatch_illegal_op)
{
	struct task task = { 0 };
	struct rpc_trans rt = { 0 };

	atomic_store_explicit(&task.t_state, TASK_RUNNING,
			      memory_order_relaxed);

	/* OP_MAX is not in op_table -- guaranteed to call nfs4_op_illegal */
	struct compound *compound = make_compound(&rt, &task, 1, OP_MAX);

	ck_assert(!dispatch_compound(compound));

	ck_assert_int_eq(compound->c_res->status, NFS4ERR_OP_ILLEGAL);
	ck_assert_uint_eq(compound->c_res->resarray.resarray_len, 1);

	free_compound(compound);
}
END_TEST

/*
 * 3. Resume: successful callback.
 *    rt_next_action is called, cleared, c_curr_op advances past the resumed
 *    op, and dispatch_compound completes NFS4_OK.
 */
START_TEST(test_dispatch_resume_ok)
{
	struct task task = { 0 };
	struct rpc_trans rt = { 0 };

	atomic_store_explicit(&task.t_state, TASK_RUNNING,
			      memory_order_relaxed);

	/* One-op compound, c_curr_op starts at 0 (the paused op) */
	struct compound *compound = make_compound(&rt, &task, 1, OP_ACCESS);
	rt.rt_next_action = ok_action;
	g_cb_called = false;

	ck_assert(!dispatch_compound(compound));

	ck_assert(g_cb_called);
	ck_assert_uint_eq(g_cb_op, 0); /* called at op 0 */
	ck_assert(rt.rt_next_action == NULL); /* cleared before call */
	ck_assert_int_eq(compound->c_res->status, NFS4_OK);
	/* c_curr_op advanced to 1 then the for-loop terminated normally */
	ck_assert_uint_eq(compound->c_curr_op, 1);

	free_compound(compound);
}
END_TEST

/*
 * 4. Resume: callback returns an error.
 *    The error propagates to res->status and dispatch returns early.
 */
START_TEST(test_dispatch_resume_error)
{
	struct task task = { 0 };
	struct rpc_trans rt = { 0 };

	atomic_store_explicit(&task.t_state, TASK_RUNNING,
			      memory_order_relaxed);

	struct compound *compound = make_compound(&rt, &task, 1, OP_ACCESS);
	rt.rt_next_action = err_action;
	g_cb_called = false;

	ck_assert(!dispatch_compound(compound));

	ck_assert(g_cb_called);
	ck_assert(rt.rt_next_action == NULL);
	ck_assert_int_eq(compound->c_res->status, NFS4ERR_STALE);
	ck_assert_uint_eq(compound->c_res->resarray.resarray_len, 1);
	/* c_curr_op was NOT advanced (error path returns without ++) */
	ck_assert_uint_eq(compound->c_curr_op, 0);

	free_compound(compound);
}
END_TEST

/*
 * 5. Resume: callback itself goes async (calls task_pause).
 *    dispatch_compound must return immediately without advancing c_curr_op,
 *    leaving the task in PAUSED state for re-enqueueing.
 */
START_TEST(test_dispatch_resume_pause)
{
	struct task task = { 0 };
	struct rpc_trans rt = { 0 };

	atomic_store_explicit(&task.t_state, TASK_RUNNING,
			      memory_order_relaxed);

	struct compound *compound = make_compound(&rt, &task, 1, OP_ACCESS);
	rt.rt_next_action = pause_action;
	g_cb_called = false;

	ck_assert(dispatch_compound(compound));

	ck_assert(g_cb_called);
	/*
	 * rt_next_action is cleared before the callback is invoked; that is
	 * the protocol -- the callback owns rt from that point.
	 */
	ck_assert(rt.rt_next_action == NULL);
	/* c_curr_op must NOT advance -- the op needs to resume again */
	ck_assert_uint_eq(compound->c_curr_op, 0);
	ck_assert(task_is_paused(&task));

	/*
	 * Do not free the compound -- in the real server it stays alive
	 * until the async completer calls task_resume().  Here we force-
	 * transition back to RUNNING so free_compound is safe.
	 */
	atomic_store_explicit(&task.t_state, TASK_RUNNING,
			      memory_order_relaxed);
	free_compound(compound);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite assembly                                                      */
/* ------------------------------------------------------------------ */

Suite *compound_async_suite(void)
{
	Suite *s = suite_create("Compound Async State Machine");

	TCase *tc = tcase_create("dispatch_compound");
	tcase_add_test(tc, test_dispatch_empty);
	tcase_add_test(tc, test_dispatch_illegal_op);
	tcase_add_test(tc, test_dispatch_resume_ok);
	tcase_add_test(tc, test_dispatch_resume_error);
	tcase_add_test(tc, test_dispatch_resume_pause);
	suite_add_tcase(s, tc);

	return s;
}

int main(void)
{
	return nfs4_test_run(compound_async_suite());
}
