/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Unit tests for the NFSv4 locking operations (lock.c).
 *
 * Tests cover:
 *   A. LOCKT -- test for conflicting lock (no stateid needed).
 *   B. LOCK new_lock_owner -- acquire lock via open stateid.
 *   C. LOCK existing_lock_owner -- extend/reacquire via lock stateid.
 *   D. LOCKU -- unlock and stateid seqid increment.
 *   E. FREE_STATEID -- free a lock stateid; open stateid blocked.
 *   F. RELEASE_LOCKOWNER -- remove lock owner from client list.
 *   G. TEST_STATEID -- per-stateid validation.
 *
 * Lock-owner reuse: lock owners are matched by clientid + owner bytes.
 * Each test group uses a unique owner string to prevent cross-test
 * interference in nc_lock_owners.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <netinet/in.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <check.h>
#include <urcu.h>

#include "nfsv42_xdr.h"
#include "reffs/filehandle.h"
#include "reffs/inode.h"
#include "reffs/lock.h"
#include "reffs/server.h"
#include "reffs/super_block.h"
#include "nfs4/client.h"
#include "nfs4/compound.h"
#include "nfs4/ops.h"
#include "nfs4/stateid.h"

#include "nfs4_test_harness.h"

/* ------------------------------------------------------------------ */
/* Compound mock helpers (identical pattern to chunk_test.c)           */
/* ------------------------------------------------------------------ */

struct cm_ctx {
	struct rpc_trans rt;
	struct task task;
	struct compound *compound;
	struct nfs4_client *nc;
};

static struct cm_ctx *cm_alloc(unsigned int nops)
{
	struct cm_ctx *cm = calloc(1, sizeof(*cm));

	ck_assert_ptr_nonnull(cm);

	atomic_store_explicit(&cm->task.t_state, TASK_RUNNING,
			      memory_order_relaxed);
	cm->rt.rt_task = &cm->task;
	cm->rt.rt_fd = -1;

	struct compound *c = calloc(1, sizeof(*c));

	ck_assert_ptr_nonnull(c);
	c->c_rt = &cm->rt;
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

	verifier4 v;
	struct sockaddr_in sin;

	memset(&v, 0x22, sizeof(v));
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(0x7f000004);
	sin.sin_port = htons(2049);

	cm->nc = nfs4_client_alloc(&v, &sin, 1, 0xC0DE0002, 0);
	ck_assert_ptr_nonnull(cm->nc);
	c->c_nfs4_client = nfs4_client_get(cm->nc);

	cm->rt.rt_compound = c;
	cm->compound = c;
	return cm;
}

static void cm_set_inode(struct cm_ctx *cm, struct inode *inode)
{
	struct compound *c = cm->compound;

	super_block_put(c->c_curr_sb);
	c->c_curr_sb = super_block_get(inode->i_sb);

	inode_active_put(c->c_inode);
	c->c_inode = inode;
	inode_active_get(inode);

	c->c_curr_nfh.nfh_sb = inode->i_sb->sb_id;
	c->c_curr_nfh.nfh_ino = inode->i_ino;
}

static void cm_set_op(struct cm_ctx *cm, unsigned int idx, nfs_opnum4 opnum)
{
	ck_assert_uint_lt(idx, cm->compound->c_args->argarray.argarray_len);
	cm->compound->c_args->argarray.argarray_val[idx].argop = opnum;
	cm->compound->c_res->resarray.resarray_val[idx].resop = opnum;
	cm->compound->c_curr_op = (int)idx;
}

static void cm_reset_slot(struct cm_ctx *cm, unsigned int idx)
{
	memset(&cm->compound->c_args->argarray.argarray_val[idx], 0,
	       sizeof(nfs_argop4));
	memset(&cm->compound->c_res->resarray.resarray_val[idx], 0,
	       sizeof(nfs_resop4));
}

static void cm_free(struct cm_ctx *cm)
{
	if (!cm)
		return;
	struct compound *c = cm->compound;

	if (c) {
		server_state_put(c->c_server_state);
		inode_active_put(c->c_inode);
		super_block_put(c->c_curr_sb);
		super_block_put(c->c_saved_sb);
		stateid_put(c->c_curr_stid);
		stateid_put(c->c_saved_stid);
		nfs4_client_put(c->c_nfs4_client);
		free(c->c_args->argarray.argarray_val);
		free(c->c_args);
		free(c->c_res->resarray.resarray_val);
		free(c->c_res);
		free(c);
	}
	nfs4_client_put(cm->nc);
	free(cm);
}

/*
 * Minimal urcu_ref release callback for manually-constructed lock owners.
 * nfs4_lock_owner_release is static in lock.c; tests that build owners
 * by hand must supply a non-NULL release to avoid a NULL-ptr deref in
 * urcu_ref_put.  Memory is freed explicitly by the test after the op.
 */
static void noop_lo_release(struct urcu_ref *ref __attribute__((unused)))
{
}

/* ------------------------------------------------------------------ */
/* Helper: set LOCKT args                                              */
/* ------------------------------------------------------------------ */

static void set_lockt_args(struct cm_ctx *cm, uint64_t offset, uint64_t length,
			   nfs_lock_type4 locktype, const char *owner_str,
			   size_t owner_len)
{
	cm_set_op(cm, 0, OP_LOCKT);
	LOCKT4args *args = &cm->compound->c_args->argarray.argarray_val[0]
				    .nfs_argop4_u.oplockt;
	args->locktype = locktype;
	args->offset = offset;
	args->length = length;
	args->owner.clientid = (clientid4)nfs4_client_to_client(cm->nc)->c_id;
	args->owner.owner.owner_val = (char *)owner_str;
	args->owner.owner.owner_len = owner_len;
}

/* ------------------------------------------------------------------ */
/* Helper: set LOCK args for new_lock_owner path                       */
/* ------------------------------------------------------------------ */

static void set_lock_args_new_owner(struct cm_ctx *cm, uint64_t offset,
				    uint64_t length, nfs_lock_type4 locktype,
				    const stateid4 *open_stid,
				    const char *owner_str, size_t owner_len)
{
	cm_set_op(cm, 0, OP_LOCK);
	LOCK4args *args = &cm->compound->c_args->argarray.argarray_val[0]
				   .nfs_argop4_u.oplock;
	args->locktype = locktype;
	args->reclaim = false;
	args->offset = offset;
	args->length = length;
	args->locker.new_lock_owner = true;
	args->locker.locker4_u.open_owner.open_seqid = 0;
	args->locker.locker4_u.open_owner.open_stateid = *open_stid;
	args->locker.locker4_u.open_owner.lock_seqid = 0;
	args->locker.locker4_u.open_owner.lock_owner.clientid =
		(clientid4)nfs4_client_to_client(cm->nc)->c_id;
	args->locker.locker4_u.open_owner.lock_owner.owner.owner_val =
		(char *)owner_str;
	args->locker.locker4_u.open_owner.lock_owner.owner.owner_len =
		owner_len;
}

/* ------------------------------------------------------------------ */
/* Helper: set LOCKU args                                              */
/* ------------------------------------------------------------------ */

static void set_locku_args(struct cm_ctx *cm, uint64_t offset, uint64_t length,
			   const stateid4 *lock_stid)
{
	cm_set_op(cm, 0, OP_LOCKU);
	LOCKU4args *args = &cm->compound->c_args->argarray.argarray_val[0]
				    .nfs_argop4_u.oplocku;
	args->locktype = READ_LT;
	args->seqid = 0;
	args->lock_stateid = *lock_stid;
	args->offset = offset;
	args->length = length;
}

/* ------------------------------------------------------------------ */
/* Fixtures                                                            */
/* ------------------------------------------------------------------ */

static struct super_block *g_sb;
static struct inode *g_inode;

static void lock_setup(void)
{
	nfs4_test_setup();
	g_sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	ck_assert_ptr_nonnull(g_sb);

	uint64_t ino =
		__atomic_add_fetch(&g_sb->sb_next_ino, 1, __ATOMIC_RELAXED);
	g_inode = inode_alloc(g_sb, ino);
	ck_assert_ptr_nonnull(g_inode);
	g_inode->i_mode = S_IFREG | 0640;
}

static void lock_teardown(void)
{
	if (g_inode) {
		inode_active_put(g_inode);
		g_inode = NULL;
	}
	if (g_sb) {
		super_block_put(g_sb);
		g_sb = NULL;
	}
	nfs4_test_teardown();
}

/* ------------------------------------------------------------------ */
/* A. LOCKT                                                            */
/* ------------------------------------------------------------------ */

/*
 * LOCKT with no filehandle returns NFS4ERR_NOFILEHANDLE.
 */
START_TEST(test_lockt_no_fh)
{
	struct cm_ctx *cm = cm_alloc(1);
	/* c_curr_nfh is zero-initialised -- empty FH. */
	set_lockt_args(cm, 0, 512, READ_LT, "lockt-nofh", 10);

	nfs4_op_lockt(cm->compound);

	LOCKT4res *res = &cm->compound->c_res->resarray.resarray_val[0]
				  .nfs_resop4_u.oplockt;
	ck_assert_int_eq(res->status, NFS4ERR_NOFILEHANDLE);
	cm_free(cm);
}
END_TEST

/*
 * LOCKT during grace period returns NFS4ERR_GRACE.
 */
START_TEST(test_lockt_grace)
{
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);

	/* Force the server into grace state. */
	struct server_state *ss = cm->compound->c_server_state;
	atomic_store_explicit(&ss->ss_lifecycle, SERVER_IN_GRACE,
			      memory_order_release);

	set_lockt_args(cm, 0, 512, READ_LT, "lockt-grace", 11);
	nfs4_op_lockt(cm->compound);

	LOCKT4res *res = &cm->compound->c_res->resarray.resarray_val[0]
				  .nfs_resop4_u.oplockt;
	ck_assert_int_eq(res->status, NFS4ERR_GRACE);

	/* Restore state so teardown works cleanly. */
	atomic_store_explicit(&ss->ss_lifecycle, SERVER_GRACE_ENDED,
			      memory_order_release);
	cm_free(cm);
}
END_TEST

/*
 * LOCKT when no conflict exists returns NFS4_OK.
 */
START_TEST(test_lockt_no_conflict)
{
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);
	set_lockt_args(cm, 0, 512, READ_LT, "lockt-ok", 8);
	nfs4_op_lockt(cm->compound);

	LOCKT4res *res = &cm->compound->c_res->resarray.resarray_val[0]
				  .nfs_resop4_u.oplockt;
	ck_assert_int_eq(res->status, NFS4_OK);
	cm_free(cm);
}
END_TEST

/*
 * LOCKT returns NFS4ERR_DENIED when an incompatible lock exists.
 *
 * Strategy: add a write lock directly via reffs_lock_add, then issue
 * a LOCKT READ request that conflicts.
 */
START_TEST(test_lockt_conflict)
{
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);

	/* Plant a conflicting write lock via the internal lock API. */
	static const char lo_bytes[] = "conflict-owner";
	struct nfs4_lock_owner *clo = calloc(1, sizeof(*clo));
	ck_assert_ptr_nonnull(clo);
	urcu_ref_init(&clo->lo_base.lo_ref);
	clo->lo_base.lo_release = noop_lo_release;
	clo->lo_owner.n_len = sizeof(lo_bytes) - 1;
	clo->lo_owner.n_bytes = malloc(clo->lo_owner.n_len);
	ck_assert_ptr_nonnull(clo->lo_owner.n_bytes);
	memcpy(clo->lo_owner.n_bytes, lo_bytes, clo->lo_owner.n_len);
	clo->lo_clientid = 0xDEADBEEF;

	struct reffs_lock *existing = calloc(1, sizeof(*existing));
	ck_assert_ptr_nonnull(existing);
	existing->l_owner = &clo->lo_base;
	existing->l_offset = 0;
	existing->l_len = 512;
	existing->l_exclusive = true;
	existing->l_inode = inode_active_get(g_inode);

	pthread_mutex_lock(&g_inode->i_lock_mutex);
	int ret = reffs_lock_add(g_inode, existing, NULL);
	pthread_mutex_unlock(&g_inode->i_lock_mutex);
	ck_assert_int_eq(ret, 0);

	/* LOCKT READ on the same range -- should be DENIED. */
	set_lockt_args(cm, 0, 512, READ_LT, "lockt-conflict", 14);
	nfs4_op_lockt(cm->compound);

	LOCKT4res *res = &cm->compound->c_res->resarray.resarray_val[0]
				  .nfs_resop4_u.oplockt;
	ck_assert_int_eq(res->status, NFS4ERR_DENIED);
	ck_assert_uint_eq(res->LOCKT4res_u.denied.offset, 0);
	ck_assert_uint_eq(res->LOCKT4res_u.denied.length, 512);

	/* Clean up: remove the planted lock so inode is clean for teardown. */
	pthread_mutex_lock(&g_inode->i_lock_mutex);
	reffs_lock_remove(g_inode, 0, 512, &clo->lo_base, NULL);
	pthread_mutex_unlock(&g_inode->i_lock_mutex);
	free(res->LOCKT4res_u.denied.owner.owner.owner_val);
	free(clo->lo_owner.n_bytes);
	free(clo);

	cm_free(cm);
}
END_TEST

/* ------------------------------------------------------------------ */
/* B. LOCK new_lock_owner                                              */
/* ------------------------------------------------------------------ */

/*
 * LOCK with new_lock_owner and an invalid (non-open) stateid type
 * returns NFS4ERR_BAD_STATEID.
 */
START_TEST(test_lock_new_owner_bad_stateid_type)
{
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);

	/* Build a wire stateid with type=Lock_Stateid, not Open_Stateid. */
	struct client *client = nfs4_client_to_client(cm->nc);
	struct lock_stateid *ls = lock_stateid_alloc(g_inode, client);
	ck_assert_ptr_nonnull(ls);
	stateid4 bad_stid;
	pack_stateid4(&bad_stid, &ls->ls_stid);

	set_lock_args_new_owner(cm, 0, 512, READ_LT, &bad_stid, "lock-bad-type",
				13);
	nfs4_op_lock(cm->compound);

	LOCK4res *res = &cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.oplock;
	ck_assert_int_eq(res->status, NFS4ERR_BAD_STATEID);

	/* Clean up the lock stateid we planted. */
	stateid_inode_unhash(&ls->ls_stid);
	stateid_client_unhash(&ls->ls_stid);
	stateid_put(&ls->ls_stid);

	cm_free(cm);
}
END_TEST

/*
 * LOCK new_lock_owner success: open stateid -> lock stateid returned.
 */
START_TEST(test_lock_new_owner_success)
{
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);

	struct client *client = nfs4_client_to_client(cm->nc);
	struct open_stateid *os = open_stateid_alloc(g_inode, client);
	ck_assert_ptr_nonnull(os);

	stateid4 open_wire;
	pack_stateid4(&open_wire, &os->os_stid);

	set_lock_args_new_owner(cm, 0, 512, READ_LT, &open_wire,
				"lock-new-owner", 14);
	nfs4_op_lock(cm->compound);

	LOCK4res *res = &cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.oplock;
	ck_assert_int_eq(res->status, NFS4_OK);

	/* The result stateid must not be all-zeros (a real lock stateid). */
	stateid4 *lock_stid = &res->LOCK4res_u.resok4.lock_stateid;
	ck_assert(!stateid4_is_special(lock_stid));

	/*
	 * Clean up: free the lock stateid (created inside nfs4_op_lock),
	 * and the open stateid we created.  The lock handler took a
	 * find-ref on the open stateid; it already released it.  We still
	 * hold the state ref (from open_stateid_alloc) and the find ref
	 * from pack_stateid4 is now part of compound->c_curr_stid.
	 * cm_free() drops c_curr_stid; we just need to drop the state ref
	 * for the open stateid.
	 */
	uint32_t sid, id, type, cookie;
	unpack_stateid4(lock_stid, &sid, &id, &type, &cookie);
	struct stateid *ls_stid = stateid_find(g_inode, id);
	if (ls_stid) {
		stateid_inode_unhash(ls_stid);
		stateid_client_unhash(ls_stid);
		stateid_put(ls_stid); /* state ref */
		stateid_put(ls_stid); /* find ref */
	}

	/* Drop the open stateid state ref. */
	stateid_inode_unhash(&os->os_stid);
	stateid_client_unhash(&os->os_stid);
	stateid_put(&os->os_stid);

	cm_free(cm);
}
END_TEST

/* ------------------------------------------------------------------ */
/* C. LOCK existing_lock_owner                                         */
/* ------------------------------------------------------------------ */

/*
 * LOCK with existing_lock_owner and wrong stateid type returns
 * NFS4ERR_BAD_STATEID.
 */
START_TEST(test_lock_existing_bad_stateid_type)
{
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);

	struct client *client = nfs4_client_to_client(cm->nc);
	struct open_stateid *os = open_stateid_alloc(g_inode, client);
	ck_assert_ptr_nonnull(os);
	stateid4 open_wire;
	pack_stateid4(&open_wire, &os->os_stid);

	cm_set_op(cm, 0, OP_LOCK);
	LOCK4args *args = &cm->compound->c_args->argarray.argarray_val[0]
				   .nfs_argop4_u.oplock;
	args->locktype = READ_LT;
	args->reclaim = false;
	args->offset = 0;
	args->length = 512;
	args->locker.new_lock_owner = false;
	/* Use the open stateid where a lock stateid is expected. */
	args->locker.locker4_u.lock_owner.lock_stateid = open_wire;

	nfs4_op_lock(cm->compound);

	LOCK4res *res = &cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.oplock;
	ck_assert_int_eq(res->status, NFS4ERR_BAD_STATEID);

	stateid_inode_unhash(&os->os_stid);
	stateid_client_unhash(&os->os_stid);
	stateid_put(&os->os_stid);
	cm_free(cm);
}
END_TEST

/* ------------------------------------------------------------------ */
/* D. LOCKU                                                            */
/* ------------------------------------------------------------------ */

/*
 * LOCKU with no filehandle returns NFS4ERR_NOFILEHANDLE.
 */
START_TEST(test_locku_no_fh)
{
	struct cm_ctx *cm = cm_alloc(1);
	/* No inode set -- empty FH. */
	stateid4 stid;
	memset(&stid, 0, sizeof(stid));
	set_locku_args(cm, 0, 512, &stid);
	nfs4_op_locku(cm->compound);

	LOCKU4res *res = &cm->compound->c_res->resarray.resarray_val[0]
				  .nfs_resop4_u.oplocku;
	ck_assert_int_eq(res->status, NFS4ERR_NOFILEHANDLE);
	cm_free(cm);
}
END_TEST

/*
 * LOCKU with wrong stateid type returns NFS4ERR_BAD_STATEID.
 */
START_TEST(test_locku_bad_stateid_type)
{
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);

	struct client *client = nfs4_client_to_client(cm->nc);
	struct open_stateid *os = open_stateid_alloc(g_inode, client);
	ck_assert_ptr_nonnull(os);
	stateid4 open_wire;
	pack_stateid4(&open_wire, &os->os_stid);

	/* Pass an open stateid where LOCKU expects a lock stateid. */
	set_locku_args(cm, 0, 512, &open_wire);
	nfs4_op_locku(cm->compound);

	LOCKU4res *res = &cm->compound->c_res->resarray.resarray_val[0]
				  .nfs_resop4_u.oplocku;
	ck_assert_int_eq(res->status, NFS4ERR_BAD_STATEID);

	stateid_inode_unhash(&os->os_stid);
	stateid_client_unhash(&os->os_stid);
	stateid_put(&os->os_stid);
	cm_free(cm);
}
END_TEST

/*
 * LOCKU success: lock stateid seqid increments and lock is removed.
 *
 * Uses new_lock_owner LOCK to acquire, then LOCKU to release.
 */
START_TEST(test_locku_success)
{
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);

	/* Step 1: create open stateid and acquire lock. */
	struct client *client = nfs4_client_to_client(cm->nc);
	struct open_stateid *os = open_stateid_alloc(g_inode, client);
	ck_assert_ptr_nonnull(os);
	stateid4 open_wire;
	pack_stateid4(&open_wire, &os->os_stid);

	set_lock_args_new_owner(cm, 0, 512, READ_LT, &open_wire, "locku-ok", 8);
	nfs4_op_lock(cm->compound);

	LOCK4res *lock_res = &cm->compound->c_res->resarray.resarray_val[0]
				      .nfs_resop4_u.oplock;
	ck_assert_int_eq(lock_res->status, NFS4_OK);
	stateid4 lock_wire = lock_res->LOCK4res_u.resok4.lock_stateid;
	uint32_t seqid_before = lock_wire.seqid;

	/* Step 2: LOCKU -- unlock the range. */
	cm_reset_slot(cm, 0);
	set_locku_args(cm, 0, 512, &lock_wire);
	nfs4_op_locku(cm->compound);

	LOCKU4res *locku_res = &cm->compound->c_res->resarray.resarray_val[0]
					.nfs_resop4_u.oplocku;
	ck_assert_int_eq(locku_res->status, NFS4_OK);
	/* seqid must have incremented. */
	ck_assert_uint_gt(locku_res->LOCKU4res_u.lock_stateid.seqid,
			  seqid_before);

	/* Step 3: verify no conflict now (LOCKT READ same range -> OK). */
	cm_reset_slot(cm, 0);
	set_lockt_args(cm, 0, 512, READ_LT, "locku-ok-check", 14);
	nfs4_op_lockt(cm->compound);
	LOCKT4res *lockt_res = &cm->compound->c_res->resarray.resarray_val[0]
					.nfs_resop4_u.oplockt;
	ck_assert_int_eq(lockt_res->status, NFS4_OK);

	/*
	 * Clean up: the lock stateid is still alive (LOCKU only removes
	 * the lock record, not the stateid).  Free it.
	 */
	uint32_t sid, id, type, cookie;
	unpack_stateid4(&locku_res->LOCKU4res_u.lock_stateid, &sid, &id, &type,
			&cookie);
	struct stateid *ls_stid = stateid_find(g_inode, id);
	if (ls_stid) {
		stateid_inode_unhash(ls_stid);
		stateid_client_unhash(ls_stid);
		stateid_put(ls_stid); /* state ref */
		stateid_put(ls_stid); /* find ref */
	}

	/* Drop the open stateid. */
	stateid_inode_unhash(&os->os_stid);
	stateid_client_unhash(&os->os_stid);
	stateid_put(&os->os_stid);

	cm_free(cm);
}
END_TEST

/* ------------------------------------------------------------------ */
/* E. FREE_STATEID                                                     */
/* ------------------------------------------------------------------ */

/*
 * FREE_STATEID on an open stateid returns NFS4ERR_LOCKS_HELD.
 * (RFC 8881 S18.38.3: client must CLOSE before freeing.)
 */
START_TEST(test_free_stateid_open_locked)
{
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);

	struct client *client = nfs4_client_to_client(cm->nc);
	struct open_stateid *os = open_stateid_alloc(g_inode, client);
	ck_assert_ptr_nonnull(os);
	stateid4 open_wire;
	pack_stateid4(&open_wire, &os->os_stid);

	cm_set_op(cm, 0, OP_FREE_STATEID);
	FREE_STATEID4args *args =
		&cm->compound->c_args->argarray.argarray_val[0]
			 .nfs_argop4_u.opfree_stateid;
	args->fsa_stateid = open_wire;

	nfs4_op_free_stateid(cm->compound);

	FREE_STATEID4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					 .nfs_resop4_u.opfree_stateid;
	ck_assert_int_eq(res->fsr_status, NFS4ERR_LOCKS_HELD);

	/* Clean up the open stateid. */
	stateid_inode_unhash(&os->os_stid);
	stateid_client_unhash(&os->os_stid);
	stateid_put(&os->os_stid);
	cm_free(cm);
}
END_TEST

/*
 * FREE_STATEID on a lock stateid succeeds.
 */
START_TEST(test_free_stateid_lock_ok)
{
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);

	struct client *client = nfs4_client_to_client(cm->nc);
	struct lock_stateid *ls = lock_stateid_alloc(g_inode, client);
	ck_assert_ptr_nonnull(ls);
	stateid4 lock_wire;
	pack_stateid4(&lock_wire, &ls->ls_stid);

	cm_set_op(cm, 0, OP_FREE_STATEID);
	FREE_STATEID4args *args =
		&cm->compound->c_args->argarray.argarray_val[0]
			 .nfs_argop4_u.opfree_stateid;
	args->fsa_stateid = lock_wire;

	nfs4_op_free_stateid(cm->compound);

	FREE_STATEID4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					 .nfs_resop4_u.opfree_stateid;
	ck_assert_int_eq(res->fsr_status, NFS4_OK);

	/*
	 * The stateid was freed by FREE_STATEID (unhashed + two puts).
	 * Do NOT try to free it again.
	 */
	cm_free(cm);
}
END_TEST

/* ------------------------------------------------------------------ */
/* F. RELEASE_LOCKOWNER                                                */
/* ------------------------------------------------------------------ */

/*
 * RELEASE_LOCKOWNER for an owner with no locks succeeds.
 * The owner is added to nc_lock_owners by a prior LOCK acquisition;
 * after all locks are released, the owner can be freed.
 *
 * We bypass the LOCK handler and insert a lock owner directly.
 */
START_TEST(test_release_lockowner_no_locks)
{
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);

	/* Insert a lock owner with ref=1 (just the list ref, no locks). */
	static const char owner_bytes[] = "release-owner-nolocks";
	struct nfs4_client *nc = cm->nc;

	struct nfs4_lock_owner *lo = calloc(1, sizeof(*lo));
	ck_assert_ptr_nonnull(lo);
	urcu_ref_init(&lo->lo_base.lo_ref);
	lo->lo_base.lo_release = noop_lo_release;
	lo->lo_clientid = (clientid4)nfs4_client_to_client(nc)->c_id;
	lo->lo_owner.n_len = sizeof(owner_bytes) - 1;
	lo->lo_owner.n_bytes = malloc(lo->lo_owner.n_len);
	ck_assert_ptr_nonnull(lo->lo_owner.n_bytes);
	memcpy(lo->lo_owner.n_bytes, owner_bytes, lo->lo_owner.n_len);

	pthread_mutex_lock(&nc->nc_lock_owners_mutex);
	cds_list_add(&lo->lo_base.lo_list, &nc->nc_lock_owners);
	pthread_mutex_unlock(&nc->nc_lock_owners_mutex);

	/* RELEASE_LOCKOWNER for that owner. */
	cm_set_op(cm, 0, OP_RELEASE_LOCKOWNER);
	RELEASE_LOCKOWNER4args *args =
		&cm->compound->c_args->argarray.argarray_val[0]
			 .nfs_argop4_u.oprelease_lockowner;
	args->lock_owner.clientid = lo->lo_clientid;
	args->lock_owner.owner.owner_val = (char *)owner_bytes;
	args->lock_owner.owner.owner_len = sizeof(owner_bytes) - 1;

	nfs4_op_release_lockowner(cm->compound);

	RELEASE_LOCKOWNER4res *res =
		&cm->compound->c_res->resarray.resarray_val[0]
			 .nfs_resop4_u.oprelease_lockowner;
	ck_assert_int_eq(res->status, NFS4_OK);

	/*
	 * RELEASE_LOCKOWNER removed lo from nc_lock_owners and called
	 * lock_owner_put, which drove the urcu_ref to 0 and invoked
	 * noop_lo_release.  noop_lo_release does not free memory, so we
	 * must free lo->lo_owner.n_bytes and lo here.
	 */
	free(lo->lo_owner.n_bytes);
	free(lo);
	cm_free(cm);
}
END_TEST

/* ------------------------------------------------------------------ */
/* G. TEST_STATEID                                                     */
/* ------------------------------------------------------------------ */

/*
 * TEST_STATEID on a special (anonymous) stateid returns
 * NFS4ERR_BAD_STATEID.
 */
START_TEST(test_test_stateid_special)
{
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);

	cm_set_op(cm, 0, OP_TEST_STATEID);
	TEST_STATEID4args *args =
		&cm->compound->c_args->argarray.argarray_val[0]
			 .nfs_argop4_u.optest_stateid;
	args->ts_stateids.ts_stateids_len = 1;
	stateid4 special_stid = { 0 };
	args->ts_stateids.ts_stateids_val = &special_stid;

	nfs4_op_test_stateid(cm->compound);

	TEST_STATEID4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					 .nfs_resop4_u.optest_stateid;
	ck_assert_int_eq(res->tsr_status, NFS4_OK);
	ck_assert_uint_eq(res->TEST_STATEID4res_u.tsr_resok4.tsr_status_codes
				  .tsr_status_codes_len,
			  1);
	ck_assert_int_eq(res->TEST_STATEID4res_u.tsr_resok4.tsr_status_codes
				 .tsr_status_codes_val[0],
			 NFS4ERR_BAD_STATEID);
	free(res->TEST_STATEID4res_u.tsr_resok4.tsr_status_codes
		     .tsr_status_codes_val);
	cm_free(cm);
}
END_TEST

/*
 * TEST_STATEID on a valid client stateid (slot matches) returns NFS4_OK.
 *
 * Construct a stateid4 whose id field encodes the client slot so the
 * slot-matching check in nfs4_op_test_stateid passes.
 */
START_TEST(test_test_stateid_valid_client)
{
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);

	/* Build a stateid whose id carries the right client slot. */
	struct client *client = nfs4_client_to_client(cm->nc);
	struct open_stateid *os = open_stateid_alloc(g_inode, client);
	ck_assert_ptr_nonnull(os);
	stateid4 open_wire;
	pack_stateid4(&open_wire, &os->os_stid);

	cm_set_op(cm, 0, OP_TEST_STATEID);
	TEST_STATEID4args *args =
		&cm->compound->c_args->argarray.argarray_val[0]
			 .nfs_argop4_u.optest_stateid;
	args->ts_stateids.ts_stateids_len = 1;
	args->ts_stateids.ts_stateids_val = &open_wire;

	nfs4_op_test_stateid(cm->compound);

	TEST_STATEID4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					 .nfs_resop4_u.optest_stateid;
	ck_assert_int_eq(res->tsr_status, NFS4_OK);
	ck_assert_uint_eq(res->TEST_STATEID4res_u.tsr_resok4.tsr_status_codes
				  .tsr_status_codes_len,
			  1);
	ck_assert_int_eq(res->TEST_STATEID4res_u.tsr_resok4.tsr_status_codes
				 .tsr_status_codes_val[0],
			 NFS4_OK);
	free(res->TEST_STATEID4res_u.tsr_resok4.tsr_status_codes
		     .tsr_status_codes_val);

	stateid_inode_unhash(&os->os_stid);
	stateid_client_unhash(&os->os_stid);
	stateid_put(&os->os_stid);
	cm_free(cm);
}
END_TEST

/*
 * TEST_STATEID with multiple stateids returns per-stateid codes.
 * First: valid (NFS4_OK), second: special (NFS4ERR_BAD_STATEID).
 */
START_TEST(test_test_stateid_multiple)
{
	struct cm_ctx *cm = cm_alloc(1);

	cm_set_inode(cm, g_inode);

	struct client *client = nfs4_client_to_client(cm->nc);
	struct open_stateid *os = open_stateid_alloc(g_inode, client);
	ck_assert_ptr_nonnull(os);
	stateid4 stids[2];
	pack_stateid4(&stids[0], &os->os_stid); /* valid */
	memset(&stids[1], 0, sizeof(stids[1])); /* special (all-zeros) */

	cm_set_op(cm, 0, OP_TEST_STATEID);
	TEST_STATEID4args *args =
		&cm->compound->c_args->argarray.argarray_val[0]
			 .nfs_argop4_u.optest_stateid;
	args->ts_stateids.ts_stateids_len = 2;
	args->ts_stateids.ts_stateids_val = stids;

	nfs4_op_test_stateid(cm->compound);

	TEST_STATEID4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					 .nfs_resop4_u.optest_stateid;
	ck_assert_int_eq(res->tsr_status, NFS4_OK);
	ck_assert_uint_eq(res->TEST_STATEID4res_u.tsr_resok4.tsr_status_codes
				  .tsr_status_codes_len,
			  2);
	ck_assert_int_eq(res->TEST_STATEID4res_u.tsr_resok4.tsr_status_codes
				 .tsr_status_codes_val[0],
			 NFS4_OK);
	ck_assert_int_eq(res->TEST_STATEID4res_u.tsr_resok4.tsr_status_codes
				 .tsr_status_codes_val[1],
			 NFS4ERR_BAD_STATEID);
	free(res->TEST_STATEID4res_u.tsr_resok4.tsr_status_codes
		     .tsr_status_codes_val);

	stateid_inode_unhash(&os->os_stid);
	stateid_client_unhash(&os->os_stid);
	stateid_put(&os->os_stid);
	cm_free(cm);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite assembly                                                      */
/* ------------------------------------------------------------------ */

static Suite *lock_suite(void)
{
	Suite *s = suite_create("lock");
	TCase *tc;

	/* A. LOCKT */
	tc = tcase_create("lockt");
	tcase_add_checked_fixture(tc, lock_setup, lock_teardown);
	tcase_add_test(tc, test_lockt_no_fh);
	tcase_add_test(tc, test_lockt_grace);
	tcase_add_test(tc, test_lockt_no_conflict);
	tcase_add_test(tc, test_lockt_conflict);
	suite_add_tcase(s, tc);

	/* B. LOCK new_lock_owner */
	tc = tcase_create("lock_new_owner");
	tcase_add_checked_fixture(tc, lock_setup, lock_teardown);
	tcase_add_test(tc, test_lock_new_owner_bad_stateid_type);
	tcase_add_test(tc, test_lock_new_owner_success);
	suite_add_tcase(s, tc);

	/* C. LOCK existing_lock_owner */
	tc = tcase_create("lock_existing_owner");
	tcase_add_checked_fixture(tc, lock_setup, lock_teardown);
	tcase_add_test(tc, test_lock_existing_bad_stateid_type);
	suite_add_tcase(s, tc);

	/* D. LOCKU */
	tc = tcase_create("locku");
	tcase_add_checked_fixture(tc, lock_setup, lock_teardown);
	tcase_add_test(tc, test_locku_no_fh);
	tcase_add_test(tc, test_locku_bad_stateid_type);
	tcase_add_test(tc, test_locku_success);
	suite_add_tcase(s, tc);

	/* E. FREE_STATEID */
	tc = tcase_create("free_stateid");
	tcase_add_checked_fixture(tc, lock_setup, lock_teardown);
	tcase_add_test(tc, test_free_stateid_open_locked);
	tcase_add_test(tc, test_free_stateid_lock_ok);
	suite_add_tcase(s, tc);

	/* F. RELEASE_LOCKOWNER */
	tc = tcase_create("release_lockowner");
	tcase_add_checked_fixture(tc, lock_setup, lock_teardown);
	tcase_add_test(tc, test_release_lockowner_no_locks);
	suite_add_tcase(s, tc);

	/* G. TEST_STATEID */
	tc = tcase_create("test_stateid");
	tcase_add_checked_fixture(tc, lock_setup, lock_teardown);
	tcase_add_test(tc, test_test_stateid_special);
	tcase_add_test(tc, test_test_stateid_valid_client);
	tcase_add_test(tc, test_test_stateid_multiple);
	suite_add_tcase(s, tc);

	return s;
}

int main(void)
{
	return nfs4_test_run(lock_suite());
}
