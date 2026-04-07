/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * reflected_getattr_test -- DS fan-out trigger and dedup logic.
 *
 * Tests that COMPOUND_DS_ATTRS_REFRESHED is set and cleared correctly,
 * and that inode_has_write_layout() reports the right state.
 *
 * Group A: inode_has_write_layout() baseline (no compound needed).
 *   Manually inserts layout_stateid entries via cds_lfht_add (same
 *   pattern as stateid_unhash.c) without requiring a live client.
 *
 * Group B: PUTFH must clear COMPOUND_DS_ATTRS_REFRESHED when switching
 *   inodes (gap G1).  Calls nfs4_op_putfh() directly with two inodes.
 *
 * Groups C-H: require dstore mock infrastructure or full layout
 *   machinery and are stubbed with #if 0 pending that work.
 *
 * Design document: .claude/design/reflected-getattr-tests.md
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

#include <urcu.h>
#include <urcu/rculfhash.h>

#include <check.h>

#include "nfsv42_xdr.h"
#include "reffs/rpc.h"
#include "reffs/server.h"
#include "reffs/task.h"
#include "reffs/inode.h"
#include "reffs/super_block.h"
#include "reffs/filehandle.h"
#include "reffs/stateid.h"
#include "nfs4/compound.h"
#include "nfs4/ops.h"
#include "nfs4/stateid.h"
#include "nfs4_test_harness.h"

/* ------------------------------------------------------------------ */
/* Shared test fixture                                                 */
/* ------------------------------------------------------------------ */

static struct super_block *test_sb;
static struct inode *inode_a;
static struct inode *inode_b;

static void rg_setup(void)
{
	nfs4_test_setup();
	test_sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	ck_assert_ptr_nonnull(test_sb);

	/* Allocate two fresh inodes for multi-FH tests. */
	inode_a = inode_alloc(test_sb, 0);
	ck_assert_ptr_nonnull(inode_a);

	inode_b = inode_alloc(test_sb, 0);
	ck_assert_ptr_nonnull(inode_b);
}

static void rg_teardown(void)
{
	if (inode_a) {
		inode_active_put(inode_a);
		inode_a = NULL;
	}
	if (inode_b) {
		inode_active_put(inode_b);
		inode_b = NULL;
	}
	if (test_sb) {
		super_block_put(test_sb);
		test_sb = NULL;
	}
	nfs4_test_teardown();
}

/* ------------------------------------------------------------------ */
/* Helper: manually insert a layout_stateid into inode->i_stateids.  */
/*                                                                    */
/* We bypass layout_stateid_alloc (which requires a live client) so  */
/* Group A tests can exercise inode_has_write_layout() in isolation.  */
/* ------------------------------------------------------------------ */

static void insert_layout_stateid(struct inode *inode,
				  struct layout_stateid *ls,
				  uint64_t iomode_bits)
{
	memset(ls, 0, sizeof(*ls));
	ls->ls_state = iomode_bits;
	ls->ls_stid.s_inode = inode;
	ls->ls_stid.s_id = 99;
	ls->ls_stid.s_tag = Layout_Stateid;

	cds_lfht_node_init(&ls->ls_stid.s_inode_node);
	rcu_read_lock();
	ls->ls_stid.s_state = STID_IS_INODE_HASHED;
	cds_lfht_add(inode->i_stateids, 99, &ls->ls_stid.s_inode_node);
	rcu_read_unlock();
}

static void remove_layout_stateid(struct layout_stateid *ls)
{
	rcu_read_lock();
	cds_lfht_del(ls->ls_stid.s_inode->i_stateids,
		     &ls->ls_stid.s_inode_node);
	rcu_read_unlock();
}

/* ------------------------------------------------------------------ */
/* Group A: inode_has_write_layout() baseline                         */
/* ------------------------------------------------------------------ */

/*
 * An inode with no layout stateids has no write layout.
 */
START_TEST(test_inode_has_no_layout)
{
	ck_assert(!inode_has_write_layout(inode_a));
}
END_TEST

/*
 * A layout stateid with only the READ iomode bit set is not a write layout.
 */
START_TEST(test_inode_has_read_only_layout)
{
	struct layout_stateid ls;

	insert_layout_stateid(inode_a, &ls, LAYOUT_STATEID_IOMODE_READ);

	ck_assert(!inode_has_write_layout(inode_a));

	remove_layout_stateid(&ls);
}
END_TEST

/*
 * A layout stateid with LAYOUT_STATEID_IOMODE_RW set IS a write layout.
 */
START_TEST(test_inode_has_write_layout_rw)
{
	struct layout_stateid ls;

	insert_layout_stateid(inode_a, &ls, LAYOUT_STATEID_IOMODE_RW);

	ck_assert(inode_has_write_layout(inode_a));

	remove_layout_stateid(&ls);
}
END_TEST

/*
 * A layout stateid with both READ and RW bits set is a write layout.
 */
START_TEST(test_inode_has_write_layout_both_bits)
{
	struct layout_stateid ls;

	insert_layout_stateid(inode_a, &ls,
			      LAYOUT_STATEID_IOMODE_READ |
				      LAYOUT_STATEID_IOMODE_RW);

	ck_assert(inode_has_write_layout(inode_a));

	remove_layout_stateid(&ls);
}
END_TEST

/*
 * After clearing the RW bit the stateid is no longer a write layout.
 */
START_TEST(test_inode_write_layout_cleared)
{
	struct layout_stateid ls;

	insert_layout_stateid(inode_a, &ls, LAYOUT_STATEID_IOMODE_RW);

	ck_assert(inode_has_write_layout(inode_a));

	__atomic_and_fetch(&ls.ls_state, ~LAYOUT_STATEID_IOMODE_RW,
			   __ATOMIC_ACQ_REL);

	ck_assert(!inode_has_write_layout(inode_a));

	remove_layout_stateid(&ls);
}
END_TEST

/*
 * inode_has_write_layout is per-inode: a write layout on inode A
 * does not affect inode B.
 */
START_TEST(test_write_layout_per_inode)
{
	struct layout_stateid ls;

	insert_layout_stateid(inode_a, &ls, LAYOUT_STATEID_IOMODE_RW);

	ck_assert(inode_has_write_layout(inode_a));
	ck_assert(!inode_has_write_layout(inode_b));

	remove_layout_stateid(&ls);
}
END_TEST

/*
 * NULL inode returns false without crashing.
 */
START_TEST(test_has_write_layout_null_inode)
{
	ck_assert(!inode_has_write_layout(NULL));
}
END_TEST

/* ------------------------------------------------------------------ */
/* Group B: PUTFH must clear COMPOUND_DS_ATTRS_REFRESHED (gap G1)     */
/* ------------------------------------------------------------------ */

/*
 * Compound context used by Group B tests.
 */
struct rg_ctx {
	struct rpc_trans rt;
	struct task task;
	struct compound *compound;
};

static struct rg_ctx *make_rg_ctx(unsigned int nops)
{
	struct rg_ctx *ctx = calloc(1, sizeof(*ctx));

	ck_assert_ptr_nonnull(ctx);

	atomic_store_explicit(&ctx->task.t_state, TASK_RUNNING,
			      memory_order_relaxed);
	ctx->rt.rt_task = &ctx->task;
	ctx->rt.rt_fd = -1;

	struct compound *c = calloc(1, sizeof(*c));

	ck_assert_ptr_nonnull(c);
	c->c_rt = &ctx->rt;
	c->c_args = calloc(1, sizeof(COMPOUND4args));
	c->c_res = calloc(1, sizeof(COMPOUND4res));
	ck_assert_ptr_nonnull(c->c_args);
	ck_assert_ptr_nonnull(c->c_res);

	if (nops > 0) {
		c->c_args->argarray.argarray_len = nops;
		c->c_args->argarray.argarray_val =
			calloc(nops, sizeof(nfs_argop4));
		ck_assert_ptr_nonnull(c->c_args->argarray.argarray_val);

		c->c_res->resarray.resarray_len = nops;
		c->c_res->resarray.resarray_val =
			calloc(nops, sizeof(nfs_resop4));
		ck_assert_ptr_nonnull(c->c_res->resarray.resarray_val);
	}

	c->c_server_state = server_state_find();
	ck_assert_ptr_nonnull(c->c_server_state);

	ctx->rt.rt_compound = c;
	ctx->compound = c;

	return ctx;
}

static void free_rg_ctx(struct rg_ctx *ctx)
{
	if (!ctx)
		return;

	struct compound *c = ctx->compound;

	if (c) {
		server_state_put(c->c_server_state);
		inode_active_put(c->c_inode);
		super_block_put(c->c_curr_sb);
		super_block_put(c->c_saved_sb);
		stateid_put(c->c_curr_stid);
		stateid_put(c->c_saved_stid);
		if (c->c_args) {
			for (u_int i = 0; i < c->c_args->argarray.argarray_len;
			     i++) {
				nfs_argop4 *a =
					&c->c_args->argarray.argarray_val[i];
				if (a->argop == OP_PUTFH)
					free(a->nfs_argop4_u.opputfh.object
						     .nfs_fh4_val);
			}
			free(c->c_args->argarray.argarray_val);
			free(c->c_args);
		}
		if (c->c_res) {
			free(c->c_res->resarray.resarray_val);
			free(c->c_res);
		}
		free(c);
	}
	free(ctx);
}

/*
 * Set up a PUTFH arg for slot idx pointing at the given inode.
 */
static void set_putfh_arg(struct rg_ctx *ctx, unsigned int idx,
			  struct inode *inode)
{
	nfs_argop4 *a = &ctx->compound->c_args->argarray.argarray_val[idx];

	a->argop = OP_PUTFH;

	struct network_file_handle *nfh = calloc(1, sizeof(*nfh));

	ck_assert_ptr_nonnull(nfh);
	nfh->nfh_sb = inode->i_sb->sb_id;
	nfh->nfh_ino = inode->i_ino;

	a->nfs_argop4_u.opputfh.object.nfs_fh4_val = (char *)nfh;
	a->nfs_argop4_u.opputfh.object.nfs_fh4_len = sizeof(*nfh);
}

/*
 * Point the compound's current filehandle at inode and populate
 * c_curr_sb and c_inode so the PUTFH handler sees a real prior state.
 *
 * Must be called before any PUTFH that is meant to SWITCH inodes.
 */
static void set_compound_current_inode(struct rg_ctx *ctx, struct inode *inode)
{
	struct compound *c = ctx->compound;

	super_block_put(c->c_curr_sb);
	c->c_curr_sb = super_block_get(inode->i_sb);

	inode_active_put(c->c_inode);
	c->c_inode = inode;
	inode_active_get(inode);

	c->c_curr_nfh.nfh_sb = inode->i_sb->sb_id;
	c->c_curr_nfh.nfh_ino = inode->i_ino;
}

/*
 * PUTFH to a different inode MUST clear COMPOUND_DS_ATTRS_REFRESHED.
 *
 * Scenario: compound starts with inode A active and the flag set.
 * After PUTFH(B), the flag must be clear -- a DS fan-out done for
 * inode A must not suppress a needed fan-out for inode B.
 */
START_TEST(test_putfh_clears_flag_on_ino_switch)
{
	struct rg_ctx *ctx = make_rg_ctx(1);
	struct compound *c = ctx->compound;

	/* Put inode A into current state with the flag set. */
	set_compound_current_inode(ctx, inode_a);
	c->c_flags |= COMPOUND_DS_ATTRS_REFRESHED;

	/* Execute PUTFH pointing at inode B. */
	set_putfh_arg(ctx, 0, inode_b);
	c->c_curr_op = 0;
	uint32_t status = nfs4_op_putfh(c);

	ck_assert_int_eq(status, NFS4_OK);
	ck_assert(!(c->c_flags & COMPOUND_DS_ATTRS_REFRESHED));

	free_rg_ctx(ctx);
}
END_TEST

/*
 * PUTFH to the SAME inode must NOT clear COMPOUND_DS_ATTRS_REFRESHED.
 *
 * An idempotent re-PUT of the same filehandle (e.g., inside a
 * PUTFH+SAVEFH+PUTFH pattern) should not invalidate a flag that was
 * set by a fan-out for the same inode earlier in the compound.
 */
START_TEST(test_putfh_preserves_flag_same_ino)
{
	struct rg_ctx *ctx = make_rg_ctx(1);
	struct compound *c = ctx->compound;

	/* Put inode A into current state with the flag set. */
	set_compound_current_inode(ctx, inode_a);
	c->c_flags |= COMPOUND_DS_ATTRS_REFRESHED;

	/* Execute PUTFH pointing at the same inode A. */
	set_putfh_arg(ctx, 0, inode_a);
	c->c_curr_op = 0;
	uint32_t status = nfs4_op_putfh(c);

	ck_assert_int_eq(status, NFS4_OK);
	ck_assert(c->c_flags & COMPOUND_DS_ATTRS_REFRESHED);

	free_rg_ctx(ctx);
}
END_TEST

/*
 * PUTFH to a different inode must clear the flag even when no write
 * layout is active on either inode.
 *
 * The flag clearing is a mechanical invariant: "this flag describes
 * inode X, not inode Y".  It is not gated on layout state.
 */
START_TEST(test_putfh_clears_flag_no_layout)
{
	struct rg_ctx *ctx = make_rg_ctx(1);
	struct compound *c = ctx->compound;

	/*
	 * Set the flag as if something set it earlier in the compound
	 * (even though neither inode has a layout -- the dedup logic
	 * does not depend on layout state).
	 */
	set_compound_current_inode(ctx, inode_a);
	c->c_flags |= COMPOUND_DS_ATTRS_REFRESHED;

	set_putfh_arg(ctx, 0, inode_b);
	c->c_curr_op = 0;
	uint32_t status = nfs4_op_putfh(c);

	ck_assert_int_eq(status, NFS4_OK);
	ck_assert(!(c->c_flags & COMPOUND_DS_ATTRS_REFRESHED));

	free_rg_ctx(ctx);
}
END_TEST

/*
 * Two successive PUTFH ops each to a different inode: first PUTFH
 * clears the flag; second PUTFH (switching back to A) also clears it.
 *
 * This covers the three-visit case from the design doc:
 *   PUTFH(a) GETATTR PUTFH(b) GETATTR PUTFH(a) GETATTR
 * where each switch must clear the flag so each GETATTR gets an
 * independent fan-out decision.
 */
START_TEST(test_putfh_clears_flag_on_each_switch)
{
	struct rg_ctx *ctx = make_rg_ctx(2);
	struct compound *c = ctx->compound;

	/* Start at inode A; simulate a fan-out having set the flag. */
	set_compound_current_inode(ctx, inode_a);
	c->c_flags |= COMPOUND_DS_ATTRS_REFRESHED;

	/* First PUTFH: switch to B. */
	set_putfh_arg(ctx, 0, inode_b);
	c->c_curr_op = 0;
	ck_assert_int_eq(nfs4_op_putfh(c), NFS4_OK);
	ck_assert(!(c->c_flags & COMPOUND_DS_ATTRS_REFRESHED));

	/* Simulate a second fan-out for inode B setting the flag. */
	c->c_flags |= COMPOUND_DS_ATTRS_REFRESHED;

	/* Second PUTFH: switch back to A. */
	set_putfh_arg(ctx, 1, inode_a);
	c->c_curr_op = 1;
	ck_assert_int_eq(nfs4_op_putfh(c), NFS4_OK);
	ck_assert(!(c->c_flags & COMPOUND_DS_ATTRS_REFRESHED));

	free_rg_ctx(ctx);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Groups C-H: require dstore mock infrastructure; stubbed for now.  */
/* These will be enabled once mock dstores are available.            */
/* ------------------------------------------------------------------ */

#if 0
/* Group C: SETATTR(size) WCC sets COMPOUND_DS_ATTRS_REFRESHED (G2). */
/* Group D: CLOSE implicit layout return (G3). */
/* Group E: DELEGRETURN implicit layout return (G4). */
/* Group F: LAYOUTERROR / LAYOUTSTATS (no-fencing path). */
/* Group G: LAYOUTGET standalone. */
/* Group H: LAYOUTCOMMIT (Option B). */
#endif

/* ------------------------------------------------------------------ */
/* Suite assembly                                                      */
/* ------------------------------------------------------------------ */

Suite *reflected_getattr_suite(void)
{
	Suite *s = suite_create("Reflected GETATTR");

	/* Group A: inode_has_write_layout() baseline. */
	TCase *tc_a = tcase_create("A: inode_has_write_layout");

	tcase_add_checked_fixture(tc_a, rg_setup, rg_teardown);
	tcase_add_test(tc_a, test_inode_has_no_layout);
	tcase_add_test(tc_a, test_inode_has_read_only_layout);
	tcase_add_test(tc_a, test_inode_has_write_layout_rw);
	tcase_add_test(tc_a, test_inode_has_write_layout_both_bits);
	tcase_add_test(tc_a, test_inode_write_layout_cleared);
	tcase_add_test(tc_a, test_write_layout_per_inode);
	tcase_add_test(tc_a, test_has_write_layout_null_inode);
	suite_add_tcase(s, tc_a);

	/* Group B: PUTFH clears COMPOUND_DS_ATTRS_REFRESHED on ino switch. */
	TCase *tc_b = tcase_create("B: PUTFH flag clearing");

	tcase_add_checked_fixture(tc_b, rg_setup, rg_teardown);
	tcase_add_test(tc_b, test_putfh_clears_flag_on_ino_switch);
	tcase_add_test(tc_b, test_putfh_preserves_flag_same_ino);
	tcase_add_test(tc_b, test_putfh_clears_flag_no_layout);
	tcase_add_test(tc_b, test_putfh_clears_flag_on_each_switch);
	suite_add_tcase(s, tc_b);

	return s;
}

int main(void)
{
	return nfs4_test_run(reflected_getattr_suite());
}
