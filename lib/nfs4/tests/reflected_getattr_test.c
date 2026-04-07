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
 * Group C: SETATTR(size) fan-out (gap G2).  Calls nfs4_op_setattr()
 *   with FATTR4_SIZE, a dstore mock, and layout segments attached to
 *   the inode.  Verifies async fan-out fires and sets the flag.
 *
 * Group D: nfs4_layout_implicit_return_rw() -- implicit layout return
 *   (G3/G4 logic).  Calls the helper directly with a real test client
 *   and layout_stateid_alloc'd stateids.  Tests that the RW bit is
 *   cleared and no async fan-out fires when there are no layout segs.
 *
 * Group E: DELEGRETURN implicit layout return (G4).  Calls
 *   nfs4_op_delegreturn() with a real delegation stateid.  Tests that
 *   an async reflected GETATTR fan-out fires when a write layout is
 *   outstanding at DELEGRETURN time.
 *
 * Group F: LAYOUTERROR and LAYOUTSTATS -- no fencing path.
 *   Tests that verify these ops return the expected status and that
 *   LAYOUTSTATS always returns NFS4ERR_NOTSUPP.
 *
 * Group G: LAYOUTGET standalone.  Tests early-exit paths: missing
 *   filehandle, disabled layout type, and no dstores registered.
 *
 * Group H: LAYOUTCOMMIT (Option B).
 *   Tests that verify LAYOUTCOMMIT updates i_size but NOT i_mtime,
 *   and that it returns 0 (sync) in all cases.
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
#include "nfs4/attr.h"
#include "nfs4/stateid.h"
#include "nfs4/client.h"
#include "nfs4/client_persist.h"
#include "nfs4_test_harness.h"

#include "reffs/dstore.h"
#include "reffs/layout_segment.h"
#include "dstore_mock.h"

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

	/*
	 * Allocate two fresh inodes with distinct ino numbers.
	 * inode_alloc(sb, 0) uses ino=0 as a literal hash key, so two
	 * calls with ino=0 return the same inode.  Use sb_next_ino to
	 * assign unique numbers, matching the pattern in
	 * super_block_dirent_create().
	 */
	uint64_t ino_a =
		__atomic_add_fetch(&test_sb->sb_next_ino, 1, __ATOMIC_RELAXED);
	inode_a = inode_alloc(test_sb, ino_a);
	ck_assert_ptr_nonnull(inode_a);

	uint64_t ino_b =
		__atomic_add_fetch(&test_sb->sb_next_ino, 1, __ATOMIC_RELAXED);
	inode_b = inode_alloc(test_sb, ino_b);
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
/* Group D fixture: real nfs4_client for implicit-LR tests           */
/* ------------------------------------------------------------------ */

static struct server_state *g_ss;
static struct nfs4_client *g_nc;

static struct nfs_impl_id4 rg_make_impl_id(void)
{
	static char domain[] = "rg-test.example.com";
	static char name[] = "rg-test";
	struct nfs_impl_id4 id = {
		.nii_domain = { .utf8string_val = domain,
				.utf8string_len = sizeof(domain) - 1 },
		.nii_name = { .utf8string_val = name,
			      .utf8string_len = sizeof(name) - 1 },
	};
	return id;
}

static void rg_client_setup(void)
{
	rg_setup();

	g_ss = server_state_find();
	ck_assert_ptr_nonnull(g_ss);

	static char owner_buf[] = "rg-implicit-lr-client";
	client_owner4 owner = {
		.co_ownerid = { .co_ownerid_val = owner_buf,
				.co_ownerid_len = sizeof(owner_buf) - 1 },
	};
	verifier4 v;
	struct sockaddr_in sin;
	struct nfs_impl_id4 impl = rg_make_impl_id();

	memset(&v, 0xBC, sizeof(v));
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(0x7f000001);
	sin.sin_port = htons(2049);

	nfsstat4 eid_status;
	g_nc = nfs4_client_alloc_or_find(g_ss, &owner, &impl, &v, &sin, 1001,
					 false, &eid_status);
	ck_assert_ptr_nonnull(g_nc);
}

static void rg_client_teardown(void)
{
	if (g_nc) {
		nfs4_client_expire(g_ss, g_nc);
		g_nc = NULL;
	}
	if (g_ss) {
		server_state_put(g_ss);
		g_ss = NULL;
	}
	rg_teardown();
}

/*
 * Build a minimal compound for nfs4_layout_implicit_return_rw() calls.
 * Sets c_inode (active_get ref) and c_nfs4_client.
 * free_rg_ctx() handles cleanup.
 */
static struct rg_ctx *make_implicit_lr_ctx(struct inode *inode,
					   struct nfs4_client *nc)
{
	struct rg_ctx *ctx = make_rg_ctx(0);
	struct compound *c = ctx->compound;

	c->c_inode = inode;
	inode_active_get(inode);
	c->c_nfs4_client = nc;

	return ctx;
}

/* ------------------------------------------------------------------ */
/* Group D: nfs4_layout_implicit_return_rw() direct tests (G3/G4)    */
/* ------------------------------------------------------------------ */

/*
 * No client on the compound -> the helper returns 0 immediately.
 * This is the guard at the top of nfs4_layout_implicit_return_rw().
 */
START_TEST(test_implicit_lr_null_client)
{
	struct rg_ctx *ctx = make_implicit_lr_ctx(inode_a, NULL);
	struct compound *c = ctx->compound;

	uint32_t ret =
		nfs4_layout_implicit_return_rw(c, nfs4_op_layoutreturn_resume);

	ck_assert_int_eq(ret, 0);

	free_rg_ctx(ctx);
}
END_TEST

/*
 * Client is present but the inode has no layout stateids -> returns 0.
 */
START_TEST(test_implicit_lr_no_layout_stateids)
{
	struct rg_ctx *ctx = make_implicit_lr_ctx(inode_a, g_nc);
	struct compound *c = ctx->compound;

	uint32_t ret =
		nfs4_layout_implicit_return_rw(c, nfs4_op_layoutreturn_resume);

	ck_assert_int_eq(ret, 0);

	free_rg_ctx(ctx);
}
END_TEST

/*
 * Layout stateid with IOMODE_READ only -- no RW bit set.
 * nfs4_layout_implicit_return_rw() skips stateids without the RW bit,
 * so it returns 0 and the stateid is undisturbed.
 */
START_TEST(test_implicit_lr_read_only)
{
	struct client *client = nfs4_client_to_client(g_nc);
	struct layout_stateid *ls = layout_stateid_alloc(inode_a, client);

	ck_assert_ptr_nonnull(ls);
	__atomic_or_fetch(&ls->ls_state, LAYOUT_STATEID_IOMODE_READ,
			  __ATOMIC_RELEASE);

	struct rg_ctx *ctx = make_implicit_lr_ctx(inode_a, g_nc);
	struct compound *c = ctx->compound;

	uint32_t ret =
		nfs4_layout_implicit_return_rw(c, nfs4_op_layoutreturn_resume);

	ck_assert_int_eq(ret, 0);
	/* RD bit must still be set -- the stateid was not touched. */
	ck_assert(__atomic_load_n(&ls->ls_state, __ATOMIC_ACQUIRE) &
		  LAYOUT_STATEID_IOMODE_READ);

	free_rg_ctx(ctx);

	/* Manually release the undisturbed stateid. */
	stateid_inode_unhash(&ls->ls_stid);
	stateid_client_unhash(&ls->ls_stid);
	stateid_put(&ls->ls_stid);
	synchronize_rcu();
}
END_TEST

/*
 * Layout stateid with IOMODE_RW set, but no layout segments on the inode
 * (lss_count == 0).  The helper must:
 *   - clear the RW bit on the stateid
 *   - unhash and free the stateid (no other bits remain)
 *   - return 0 (no fan-out launched -- lss_count guard)
 *
 * After the call the stateid is freed via RCU; do not touch ls.
 */
START_TEST(test_implicit_lr_rw_no_segments)
{
	struct client *client = nfs4_client_to_client(g_nc);
	struct layout_stateid *ls = layout_stateid_alloc(inode_a, client);

	ck_assert_ptr_nonnull(ls);
	__atomic_or_fetch(&ls->ls_state, LAYOUT_STATEID_IOMODE_RW,
			  __ATOMIC_RELEASE);

	struct rg_ctx *ctx = make_implicit_lr_ctx(inode_a, g_nc);
	struct compound *c = ctx->compound;

	uint32_t ret =
		nfs4_layout_implicit_return_rw(c, nfs4_op_layoutreturn_resume);

	/*
	 * No layout segments: the T2 fan-out guard prevents async launch.
	 * The stateid was cleared and freed (RCU-deferred), so we can
	 * verify nothing went async.
	 */
	ck_assert_int_eq(ret, 0);

	/* No write layout should remain on the inode. */
	ck_assert(!inode_has_write_layout(inode_a));

	free_rg_ctx(ctx);
	synchronize_rcu();
}
END_TEST

/*
 * COMPOUND_DS_ATTRS_REFRESHED already set.  Even though there is no
 * write layout here, verify the flag does not cause incorrect behavior
 * when combined with the "no layout stateid" path.
 */
START_TEST(test_implicit_lr_flag_already_set_no_layout)
{
	struct rg_ctx *ctx = make_implicit_lr_ctx(inode_a, g_nc);
	struct compound *c = ctx->compound;

	c->c_flags |= COMPOUND_DS_ATTRS_REFRESHED;

	uint32_t ret =
		nfs4_layout_implicit_return_rw(c, nfs4_op_layoutreturn_resume);

	ck_assert_int_eq(ret, 0);

	free_rg_ctx(ctx);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Group F: LAYOUTERROR and LAYOUTSTATS (no-fencing path)            */
/* ------------------------------------------------------------------ */

/*
 * Build a compound pre-loaded with the current FH pointing at inode
 * and all op slots available.  Used by Group F and Group H.
 */
static struct rg_ctx *make_op_ctx(struct inode *inode, unsigned int nops)
{
	struct rg_ctx *ctx = make_rg_ctx(nops);
	struct compound *c = ctx->compound;

	c->c_inode = inode;
	inode_active_get(inode);
	c->c_curr_nfh.nfh_sb = inode->i_sb->sb_id;
	c->c_curr_nfh.nfh_ino = inode->i_ino;
	c->c_curr_op = 0;

	return ctx;
}

/*
 * LAYOUTSTATS is stubbed as NFS4ERR_NOTSUPP (the op is parsed by the
 * server but statistics tracking is deferred).
 */
START_TEST(test_layoutstats_notsupp)
{
	struct rg_ctx *ctx = make_op_ctx(inode_a, 1);
	struct compound *c = ctx->compound;

	c->c_args->argarray.argarray_val[0].argop = OP_LAYOUTSTATS;

	uint32_t ret = nfs4_op_layoutstats(c);

	LAYOUTSTATS4res *res =
		&c->c_res->resarray.resarray_val[0].nfs_resop4_u.oplayoutstats;
	ck_assert_int_eq(ret, 0);
	ck_assert_int_eq(res->lsr_status, NFS4ERR_NOTSUPP);

	free_rg_ctx(ctx);
}
END_TEST

/*
 * LAYOUTERROR with an empty current filehandle returns NFS4ERR_NOFILEHANDLE.
 */
START_TEST(test_layouterror_no_filehandle)
{
	struct rg_ctx *ctx = make_rg_ctx(1);
	struct compound *c = ctx->compound;

	/* Leave c_curr_nfh zeroed (empty) and c_inode NULL. */
	c->c_args->argarray.argarray_val[0].argop = OP_LAYOUTERROR;
	LAYOUTERROR4args *lea =
		&c->c_args->argarray.argarray_val[0].nfs_argop4_u.oplayouterror;
	lea->lea_errors.lea_errors_len = 0;
	lea->lea_errors.lea_errors_val = NULL;

	uint32_t ret = nfs4_op_layouterror(c);

	LAYOUTERROR4res *res =
		&c->c_res->resarray.resarray_val[0].nfs_resop4_u.oplayouterror;
	ck_assert_int_eq(ret, 0);
	ck_assert_int_eq(res->ler_status, NFS4ERR_NOFILEHANDLE);

	free_rg_ctx(ctx);
}
END_TEST

/*
 * LAYOUTERROR with a valid FH but an empty error list (no errors to
 * report).  The function iterates zero times and returns NFS4_OK (0).
 */
START_TEST(test_layouterror_empty_errors)
{
	struct rg_ctx *ctx = make_op_ctx(inode_a, 1);
	struct compound *c = ctx->compound;

	c->c_args->argarray.argarray_val[0].argop = OP_LAYOUTERROR;
	LAYOUTERROR4args *lea =
		&c->c_args->argarray.argarray_val[0].nfs_argop4_u.oplayouterror;
	lea->lea_errors.lea_errors_len = 0;
	lea->lea_errors.lea_errors_val = NULL;

	uint32_t ret = nfs4_op_layouterror(c);

	LAYOUTERROR4res *res =
		&c->c_res->resarray.resarray_val[0].nfs_resop4_u.oplayouterror;
	ck_assert_int_eq(ret, 0);
	ck_assert_int_eq(res->ler_status, NFS4_OK);

	free_rg_ctx(ctx);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Group H: LAYOUTCOMMIT (Option B)                                   */
/* ------------------------------------------------------------------ */

/*
 * LAYOUTCOMMIT without a new-offset report (no_newoffset = false).
 * The inode size must remain unchanged and the op returns 0 (sync).
 */
START_TEST(test_layoutcommit_no_newoffset)
{
	struct rg_ctx *ctx = make_op_ctx(inode_a, 1);
	struct compound *c = ctx->compound;
	int64_t size_before = inode_a->i_size;

	c->c_args->argarray.argarray_val[0].argop = OP_LAYOUTCOMMIT;
	LAYOUTCOMMIT4args *lca =
		&c->c_args->argarray.argarray_val[0].nfs_argop4_u.oplayoutcommit;
	lca->loca_last_write_offset.no_newoffset = false;

	uint32_t ret = nfs4_op_layoutcommit(c);

	ck_assert_int_eq(ret, 0);
	ck_assert_int_eq(inode_a->i_size, size_before);

	LAYOUTCOMMIT4res *res =
		&c->c_res->resarray.resarray_val[0].nfs_resop4_u.oplayoutcommit;
	ck_assert_int_eq(res->locr_status, NFS4_OK);
	ck_assert(!res->LAYOUTCOMMIT4res_u.locr_resok4.locr_newsize
			   .ns_sizechanged);

	free_rg_ctx(ctx);
}
END_TEST

/*
 * LAYOUTCOMMIT with a new-offset larger than the current size.
 * i_size must grow to offset+1; locr_newsize reflects the new size.
 */
START_TEST(test_layoutcommit_updates_size)
{
	struct rg_ctx *ctx = make_op_ctx(inode_a, 1);
	struct compound *c = ctx->compound;

	inode_a->i_size = 0;

	c->c_args->argarray.argarray_val[0].argop = OP_LAYOUTCOMMIT;
	LAYOUTCOMMIT4args *lca =
		&c->c_args->argarray.argarray_val[0].nfs_argop4_u.oplayoutcommit;
	lca->loca_last_write_offset.no_newoffset = true;
	lca->loca_last_write_offset.newoffset4_u.no_offset = 4095;

	uint32_t ret = nfs4_op_layoutcommit(c);

	ck_assert_int_eq(ret, 0);
	ck_assert_int_eq(inode_a->i_size, 4096); /* offset + 1 */

	LAYOUTCOMMIT4res *res =
		&c->c_res->resarray.resarray_val[0].nfs_resop4_u.oplayoutcommit;
	ck_assert_int_eq(res->locr_status, NFS4_OK);
	ck_assert(
		res->LAYOUTCOMMIT4res_u.locr_resok4.locr_newsize.ns_sizechanged);
	ck_assert_int_eq(res->LAYOUTCOMMIT4res_u.locr_resok4.locr_newsize
				 .newsize4_u.ns_size,
			 4096);

	free_rg_ctx(ctx);
}
END_TEST

/*
 * LAYOUTCOMMIT with a new-offset SMALLER than the current size must NOT
 * shrink the file (one-direction update per Option B decision).
 */
START_TEST(test_layoutcommit_no_shrink)
{
	struct rg_ctx *ctx = make_op_ctx(inode_a, 1);
	struct compound *c = ctx->compound;

	inode_a->i_size = 8192;

	c->c_args->argarray.argarray_val[0].argop = OP_LAYOUTCOMMIT;
	LAYOUTCOMMIT4args *lca =
		&c->c_args->argarray.argarray_val[0].nfs_argop4_u.oplayoutcommit;
	lca->loca_last_write_offset.no_newoffset = true;
	lca->loca_last_write_offset.newoffset4_u.no_offset = 999;

	uint32_t ret = nfs4_op_layoutcommit(c);

	ck_assert_int_eq(ret, 0);
	/* Size must not shrink below the current 8192. */
	ck_assert_int_eq(inode_a->i_size, 8192);

	LAYOUTCOMMIT4res *res =
		&c->c_res->resarray.resarray_val[0].nfs_resop4_u.oplayoutcommit;
	ck_assert_int_eq(res->locr_status, NFS4_OK);
	/*
	 * ns_sizechanged is false because the size did not actually change
	 * (new_end = 1000 < current 8192, so the if-branch was not taken).
	 */
	ck_assert(!res->LAYOUTCOMMIT4res_u.locr_resok4.locr_newsize
			   .ns_sizechanged);

	free_rg_ctx(ctx);
}
END_TEST

/*
 * Option B design decision: LAYOUTCOMMIT updates i_size but NOT i_mtime.
 * Fresh mtime is only available after a reflected GETATTR (T1 or T2).
 * The test documents this by recording mtime before/after and asserting
 * they are identical.
 */
START_TEST(test_layoutcommit_mtime_stale)
{
	struct rg_ctx *ctx = make_op_ctx(inode_a, 1);
	struct compound *c = ctx->compound;

	inode_a->i_size = 0;
	struct timespec mtime_before = inode_a->i_mtime;

	c->c_args->argarray.argarray_val[0].argop = OP_LAYOUTCOMMIT;
	LAYOUTCOMMIT4args *lca =
		&c->c_args->argarray.argarray_val[0].nfs_argop4_u.oplayoutcommit;
	lca->loca_last_write_offset.no_newoffset = true;
	lca->loca_last_write_offset.newoffset4_u.no_offset = 1023;

	uint32_t ret = nfs4_op_layoutcommit(c);

	ck_assert_int_eq(ret, 0);
	ck_assert_int_eq(inode_a->i_size, 1024);

	/*
	 * i_mtime must be unchanged -- Option B: only i_size is updated
	 * from lca_last_write_offset.  i_mtime is refreshed only when
	 * T1 or T2 fires a reflected GETATTR to the DSes.
	 */
	ck_assert_int_eq(inode_a->i_mtime.tv_sec, mtime_before.tv_sec);
	ck_assert_int_eq(inode_a->i_mtime.tv_nsec, mtime_before.tv_nsec);

	free_rg_ctx(ctx);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Fixtures: dstore mock variants of the base and client fixtures     */
/* ------------------------------------------------------------------ */

/*
 * Like rg_setup/rg_teardown, but also initialises the global dstore
 * hash table.  Required for any test that allocates a dstore_mock or
 * calls code that walks the dstore table (SETATTR fan-out, LAYOUTGET).
 */
static void rg_dstore_setup(void)
{
	rg_setup();
	ck_assert_int_eq(dstore_init(), 0);
}

static void rg_dstore_teardown(void)
{
	/*
	 * Tear down NFS4 / inode state before destroying the dstore table.
	 * rg_teardown -> nfs4_test_teardown may expire stateids; the dstore
	 * table must remain valid until that completes.
	 */
	rg_teardown();
	dstore_fini();
}

/*
 * Like rg_client_setup/rg_client_teardown, but also initialises the
 * dstore table.  Required for Group E (DELEGRETURN fan-out).
 */
static void rg_client_dstore_setup(void)
{
	rg_client_setup();
	ck_assert_int_eq(dstore_init(), 0);
}

static void rg_client_dstore_teardown(void)
{
	/*
	 * Expire the client (releases stateids) before the dstore table
	 * is destroyed.  nfs4_client_expire may walk layout stateids.
	 */
	rg_client_teardown();
	dstore_fini();
}

/* ------------------------------------------------------------------ */
/* Group C: SETATTR(size) fan-out sets COMPOUND_DS_ATTRS_REFRESHED    */
/* ------------------------------------------------------------------ */

/*
 * Encode FATTR4_SIZE into a stack-allocated fattr4.
 * bm_word and attr_words are caller-provided arrays; the fattr4 holds
 * pointers into them.  Both must remain live until the op under test
 * completes.  Safe for single-call-at-a-time tests (all ours are).
 *
 * XDR uint64 = two network-order uint32_t words (high then low).
 * FATTR4_SIZE is attribute bit 4, so it lives in bitmap word 0.
 */
static void fill_fattr4_size(fattr4 *fa, uint64_t size, uint32_t *bm_word,
			     uint32_t *attr_words)
{
	bm_word[0] = 1U << FATTR4_SIZE; /* bit 4 in word 0 */

	fa->attrmask.bitmap4_len = 1;
	fa->attrmask.bitmap4_val = bm_word;

	attr_words[0] = htonl((uint32_t)(size >> 32));
	attr_words[1] = htonl((uint32_t)(size & 0xFFFFFFFF));
	fa->attr_vals.attrlist4_len = sizeof(uint32_t) * 2;
	fa->attr_vals.attrlist4_val = (char *)attr_words;
}

/*
 * SETATTR(size) with an active write layout triggers an async truncate
 * fan-out to all DSes.  After the fan-out completes, the compound must
 * have COMPOUND_DS_ATTRS_REFRESHED set and the mock DS must have seen
 * exactly one truncate call.
 */
START_TEST(test_setattr_size_fanout_sets_flag)
{
	struct dstore_mock *dm = dstore_mock_alloc(20);

	ck_assert_ptr_nonnull(dm);

	/*
	 * Attach a layout segment pointing at dstore 20 so the fan-out
	 * path in nfs4_op_setattr sees lss_count > 0.
	 */
	inode_a->i_layout_segments = mock_layout_segments_alloc(20, 0, NULL);
	ck_assert_ptr_nonnull(inode_a->i_layout_segments);

	struct rg_ctx *ctx = make_rg_ctx(1);
	struct compound *c = ctx->compound;

	set_compound_current_inode(ctx, inode_a);
	c->c_curr_op = 0;
	c->c_args->argarray.argarray_val[0].argop = OP_SETATTR;

	SETATTR4args *sa =
		&c->c_args->argarray.argarray_val[0].nfs_argop4_u.opsetattr;
	memset(&sa->stateid, 0, sizeof(sa->stateid)); /* anonymous stateid */

	uint32_t bm_word[1], attr_words[2];

	fill_fattr4_size(&sa->obj_attributes, 4096, bm_word, attr_words);

	uint32_t ret = nfs4_op_setattr(c);

	ck_assert_uint_eq(ret, NFS4_OP_FLAG_ASYNC);

	/*
	 * Drive the fan-out: spin until the truncate pthread finishes,
	 * then invoke nfs4_op_setattr_resume inline.
	 */
	mock_drive_fanout(&ctx->rt);

	ck_assert(c->c_flags & COMPOUND_DS_ATTRS_REFRESHED);
	ck_assert_uint_eq(dstore_mock_truncate_calls(dm), 1);

	layout_segments_free(inode_a->i_layout_segments);
	inode_a->i_layout_segments = NULL;

	free_rg_ctx(ctx);
	dstore_mock_free(dm);
}
END_TEST

/*
 * SETATTR(size) with NO layout segments must NOT go async.  The flag
 * must remain clear (synchronous path, no DS fan-out).
 */
START_TEST(test_setattr_size_no_segs_no_fanout)
{
	/*
	 * No i_layout_segments -- the fan-out condition at line 4022 of
	 * attr.c requires lss_count > 0.  Without segments the synchronous
	 * path applies.
	 */
	struct rg_ctx *ctx = make_rg_ctx(1);
	struct compound *c = ctx->compound;

	set_compound_current_inode(ctx, inode_a);
	c->c_curr_op = 0;
	c->c_args->argarray.argarray_val[0].argop = OP_SETATTR;

	SETATTR4args *sa =
		&c->c_args->argarray.argarray_val[0].nfs_argop4_u.opsetattr;
	memset(&sa->stateid, 0, sizeof(sa->stateid));

	uint32_t bm_word[1], attr_words[2];

	fill_fattr4_size(&sa->obj_attributes, 4096, bm_word, attr_words);

	uint32_t ret = nfs4_op_setattr(c);

	/* Synchronous -- must NOT return ASYNC. */
	ck_assert_uint_eq(ret, 0);
	ck_assert(!(c->c_flags & COMPOUND_DS_ATTRS_REFRESHED));

	free_rg_ctx(ctx);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Group E: DELEGRETURN implicit layout return with deleg stateid     */
/* ------------------------------------------------------------------ */

/*
 * DELEGRETURN with an empty filehandle must fail with NFS4ERR_NOFILEHANDLE.
 */
START_TEST(test_delegreturn_no_fh)
{
	struct rg_ctx *ctx = make_rg_ctx(1);
	struct compound *c = ctx->compound;

	/* Leave c_curr_nfh zero -- network_file_handle_empty() returns true. */
	c->c_curr_op = 0;
	c->c_args->argarray.argarray_val[0].argop = OP_DELEGRETURN;

	uint32_t ret = nfs4_op_delegreturn(c);

	ck_assert_uint_eq(ret, 0);

	DELEGRETURN4res *res =
		&c->c_res->resarray.resarray_val[0].nfs_resop4_u.opdelegreturn;
	ck_assert_int_eq(res->status, NFS4ERR_NOFILEHANDLE);

	free_rg_ctx(ctx);
}
END_TEST

/*
 * DELEGRETURN with an all-zeros (special) stateid must fail with
 * NFS4ERR_BAD_STATEID.
 */
START_TEST(test_delegreturn_special_stateid)
{
	struct rg_ctx *ctx = make_rg_ctx(1);
	struct compound *c = ctx->compound;

	set_compound_current_inode(ctx, inode_a);
	c->c_curr_op = 0;
	c->c_args->argarray.argarray_val[0].argop = OP_DELEGRETURN;

	DELEGRETURN4args *da =
		&c->c_args->argarray.argarray_val[0].nfs_argop4_u.opdelegreturn;
	memset(&da->deleg_stateid, 0, sizeof(da->deleg_stateid)); /* special */

	uint32_t ret = nfs4_op_delegreturn(c);

	ck_assert_uint_eq(ret, 0);

	DELEGRETURN4res *res =
		&c->c_res->resarray.resarray_val[0].nfs_resop4_u.opdelegreturn;
	ck_assert_int_eq(res->status, NFS4ERR_BAD_STATEID);

	free_rg_ctx(ctx);
}
END_TEST

/*
 * DELEGRETURN with a valid delegation stateid and no layout segments
 * must succeed synchronously (no DS fan-out).
 */
START_TEST(test_delegreturn_valid_no_layout)
{
	struct client *client = nfs4_client_to_client(g_nc);
	struct delegation_stateid *ds =
		delegation_stateid_alloc(inode_a, client);

	ck_assert_ptr_nonnull(ds);

	stateid4 wire_sid;

	pack_stateid4(&wire_sid, &ds->ds_stid);

	struct rg_ctx *ctx = make_rg_ctx(1);
	struct compound *c = ctx->compound;

	set_compound_current_inode(ctx, inode_a);
	c->c_nfs4_client = g_nc;
	c->c_curr_op = 0;
	c->c_args->argarray.argarray_val[0].argop = OP_DELEGRETURN;

	DELEGRETURN4args *da =
		&c->c_args->argarray.argarray_val[0].nfs_argop4_u.opdelegreturn;
	da->deleg_stateid = wire_sid;

	/*
	 * After DELEGRETURN returns, ds and stid have been freed via RCU.
	 * No access to ds after this point.
	 */
	uint32_t ret = nfs4_op_delegreturn(c);

	/*
	 * No write layout on inode_a so nfs4_layout_implicit_return_rw
	 * returns 0 -- synchronous, no fan-out.
	 */
	ck_assert_uint_eq(ret, 0);

	DELEGRETURN4res *res =
		&c->c_res->resarray.resarray_val[0].nfs_resop4_u.opdelegreturn;
	ck_assert_int_eq(res->status, NFS4_OK);

	free_rg_ctx(ctx);
	/*
	 * nfs4_op_delegreturn freed ds via call_rcu.  synchronize_rcu()
	 * ensures the RCU callback completes before teardown drops the
	 * inode ref (consistent with Group D test discipline).
	 */
	synchronize_rcu();
}
END_TEST

/*
 * DELEGRETURN when the client holds a write layout on the file must
 * trigger an async reflected GETATTR fan-out and set
 * COMPOUND_DS_ATTRS_REFRESHED.
 */
START_TEST(test_delegreturn_write_layout_fires_fanout)
{
	struct dstore_mock *dm = dstore_mock_alloc(21);

	ck_assert_ptr_nonnull(dm);

	/*
	 * Attach a layout segment and a write layout stateid so that
	 * nfs4_layout_implicit_return_rw finds work to do.
	 */
	inode_a->i_layout_segments = mock_layout_segments_alloc(21, 0, NULL);
	ck_assert_ptr_nonnull(inode_a->i_layout_segments);

	struct client *client = nfs4_client_to_client(g_nc);

	/*
	 * ls is consumed (RW bit cleared, stateid freed via call_rcu) by
	 * nfs4_layout_implicit_return_rw inside nfs4_op_delegreturn.
	 * Do not access ls after nfs4_op_delegreturn returns.
	 */
	struct layout_stateid *ls = layout_stateid_alloc(inode_a, client);

	ck_assert_ptr_nonnull(ls);
	__atomic_or_fetch(&ls->ls_state, LAYOUT_STATEID_IOMODE_RW,
			  __ATOMIC_RELEASE);

	/* Allocate the delegation the client is about to return. */
	struct delegation_stateid *ds =
		delegation_stateid_alloc(inode_a, client);

	ck_assert_ptr_nonnull(ds);

	stateid4 wire_sid;

	pack_stateid4(&wire_sid, &ds->ds_stid);

	struct rg_ctx *ctx = make_rg_ctx(1);
	struct compound *c = ctx->compound;

	set_compound_current_inode(ctx, inode_a);
	c->c_nfs4_client = g_nc;
	c->c_curr_op = 0;
	c->c_args->argarray.argarray_val[0].argop = OP_DELEGRETURN;

	DELEGRETURN4args *da =
		&c->c_args->argarray.argarray_val[0].nfs_argop4_u.opdelegreturn;
	da->deleg_stateid = wire_sid;

	uint32_t ret = nfs4_op_delegreturn(c);

	ck_assert_uint_eq(ret, NFS4_OP_FLAG_ASYNC);

	mock_drive_fanout(&ctx->rt);

	ck_assert(c->c_flags & COMPOUND_DS_ATTRS_REFRESHED);
	ck_assert_uint_eq(dstore_mock_getattr_calls(dm), 1);

	/*
	 * Both ls (layout stateid) and ds (delegation stateid) were freed
	 * via call_rcu by nfs4_op_delegreturn/nfs4_layout_implicit_return_rw.
	 * synchronize_rcu() ensures those callbacks complete before the
	 * fixture teardown drops the inode ref -- consistent with Group D.
	 */
	synchronize_rcu();

	layout_segments_free(inode_a->i_layout_segments);
	inode_a->i_layout_segments = NULL;

	free_rg_ctx(ctx);
	dstore_mock_free(dm);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Group G: LAYOUTGET standalone tests                                */
/* ------------------------------------------------------------------ */

/*
 * LAYOUTGET with an empty filehandle must fail with NFS4ERR_NOFILEHANDLE.
 */
START_TEST(test_layoutget_no_fh)
{
	struct rg_ctx *ctx = make_rg_ctx(1);
	struct compound *c = ctx->compound;

	/* Leave c_curr_nfh zero -- no filehandle. */
	c->c_curr_op = 0;
	c->c_args->argarray.argarray_val[0].argop = OP_LAYOUTGET;

	LAYOUTGET4args *la =
		&c->c_args->argarray.argarray_val[0].nfs_argop4_u.oplayoutget;
	la->loga_layout_type = LAYOUT4_FLEX_FILES;

	uint32_t ret = nfs4_op_layoutget(c);

	ck_assert_uint_eq(ret, 0);

	LAYOUTGET4res *res =
		&c->c_res->resarray.resarray_val[0].nfs_resop4_u.oplayoutget;
	ck_assert_int_eq(res->logr_status, NFS4ERR_NOFILEHANDLE);

	free_rg_ctx(ctx);
}
END_TEST

/*
 * LAYOUTGET on an export with sb_layout_types=0 (the default for all
 * test exports) must fail with NFS4ERR_LAYOUTUNAVAILABLE.
 */
START_TEST(test_layoutget_layout_unavailable)
{
	struct rg_ctx *ctx = make_rg_ctx(1);
	struct compound *c = ctx->compound;

	/*
	 * test_sb->sb_layout_types is 0 by default -- no layout type is
	 * configured for this export.  The layout-type check fires before
	 * any client validation, so c_nfs4_client is not needed here.
	 */
	set_compound_current_inode(ctx, inode_a);
	c->c_curr_op = 0;
	c->c_args->argarray.argarray_val[0].argop = OP_LAYOUTGET;

	LAYOUTGET4args *la =
		&c->c_args->argarray.argarray_val[0].nfs_argop4_u.oplayoutget;
	la->loga_layout_type = LAYOUT4_FLEX_FILES;

	uint32_t ret = nfs4_op_layoutget(c);

	ck_assert_uint_eq(ret, 0);

	LAYOUTGET4res *res =
		&c->c_res->resarray.resarray_val[0].nfs_resop4_u.oplayoutget;
	ck_assert_int_eq(res->logr_status, NFS4ERR_LAYOUTUNAVAILABLE);

	free_rg_ctx(ctx);
}
END_TEST

/*
 * LAYOUTGET on a flex-files-enabled export with no dstores registered
 * must fail with NFS4ERR_LAYOUTUNAVAILABLE (nds == 0 path in layout.c).
 */
START_TEST(test_layoutget_no_dstores)
{
	struct rg_ctx *ctx = make_rg_ctx(1);
	struct compound *c = ctx->compound;

	/*
	 * Temporarily enable flex files on the test export so the handler
	 * passes the layout-type check and reaches the dstore collection.
	 * No dstores are registered so nds == 0 -> LAYOUTUNAVAILABLE.
	 */
	test_sb->sb_layout_types |= SB_LAYOUT_FLEX_FILES;

	set_compound_current_inode(ctx, inode_a);
	c->c_nfs4_client = g_nc;
	c->c_curr_op = 0;
	c->c_args->argarray.argarray_val[0].argop = OP_LAYOUTGET;

	LAYOUTGET4args *la =
		&c->c_args->argarray.argarray_val[0].nfs_argop4_u.oplayoutget;
	la->loga_layout_type = LAYOUT4_FLEX_FILES;
	la->loga_minlength = 0;
	la->loga_length = UINT64_MAX; /* NFS4_ALL_FILE equivalent */
	la->loga_iomode = LAYOUTIOMODE4_RW;

	uint32_t ret = nfs4_op_layoutget(c);

	test_sb->sb_layout_types &= ~SB_LAYOUT_FLEX_FILES;

	ck_assert_uint_eq(ret, 0);

	LAYOUTGET4res *res =
		&c->c_res->resarray.resarray_val[0].nfs_resop4_u.oplayoutget;
	ck_assert_int_eq(res->logr_status, NFS4ERR_LAYOUTUNAVAILABLE);

	free_rg_ctx(ctx);
}
END_TEST

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

	/* Group D: nfs4_layout_implicit_return_rw() direct tests (G3/G4). */
	TCase *tc_d = tcase_create("D: implicit layout return");

	tcase_add_checked_fixture(tc_d, rg_client_setup, rg_client_teardown);
	tcase_add_test(tc_d, test_implicit_lr_null_client);
	tcase_add_test(tc_d, test_implicit_lr_no_layout_stateids);
	tcase_add_test(tc_d, test_implicit_lr_read_only);
	tcase_add_test(tc_d, test_implicit_lr_rw_no_segments);
	tcase_add_test(tc_d, test_implicit_lr_flag_already_set_no_layout);
	suite_add_tcase(s, tc_d);

	/* Group F: LAYOUTERROR and LAYOUTSTATS (no-fencing path). */
	TCase *tc_f = tcase_create("F: LAYOUTERROR and LAYOUTSTATS");

	tcase_add_checked_fixture(tc_f, rg_setup, rg_teardown);
	tcase_add_test(tc_f, test_layoutstats_notsupp);
	tcase_add_test(tc_f, test_layouterror_no_filehandle);
	tcase_add_test(tc_f, test_layouterror_empty_errors);
	suite_add_tcase(s, tc_f);

	/* Group C: SETATTR(size) fan-out sets COMPOUND_DS_ATTRS_REFRESHED. */
	TCase *tc_c = tcase_create("C: SETATTR size fan-out");

	tcase_add_checked_fixture(tc_c, rg_dstore_setup, rg_dstore_teardown);
	tcase_add_test(tc_c, test_setattr_size_fanout_sets_flag);
	tcase_add_test(tc_c, test_setattr_size_no_segs_no_fanout);
	suite_add_tcase(s, tc_c);

	/* Group E: DELEGRETURN implicit layout return with deleg stateid. */
	TCase *tc_e = tcase_create("E: DELEGRETURN implicit layout return");

	tcase_add_checked_fixture(tc_e, rg_client_dstore_setup,
				  rg_client_dstore_teardown);
	tcase_add_test(tc_e, test_delegreturn_no_fh);
	tcase_add_test(tc_e, test_delegreturn_special_stateid);
	tcase_add_test(tc_e, test_delegreturn_valid_no_layout);
	tcase_add_test(tc_e, test_delegreturn_write_layout_fires_fanout);
	suite_add_tcase(s, tc_e);

	/* Group G: LAYOUTGET standalone. */
	TCase *tc_g = tcase_create("G: LAYOUTGET standalone");

	tcase_add_checked_fixture(tc_g, rg_client_dstore_setup,
				  rg_client_dstore_teardown);
	tcase_add_test(tc_g, test_layoutget_no_fh);
	tcase_add_test(tc_g, test_layoutget_layout_unavailable);
	tcase_add_test(tc_g, test_layoutget_no_dstores);
	suite_add_tcase(s, tc_g);

	/* Group H: LAYOUTCOMMIT (Option B). */
	TCase *tc_h = tcase_create("H: LAYOUTCOMMIT Option B");

	tcase_add_checked_fixture(tc_h, rg_setup, rg_teardown);
	tcase_add_test(tc_h, test_layoutcommit_no_newoffset);
	tcase_add_test(tc_h, test_layoutcommit_updates_size);
	tcase_add_test(tc_h, test_layoutcommit_no_shrink);
	tcase_add_test(tc_h, test_layoutcommit_mtime_stale);
	suite_add_tcase(s, tc_h);

	return s;
}

int main(void)
{
	return nfs4_test_run(reflected_getattr_suite());
}
