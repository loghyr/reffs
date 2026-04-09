/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Unit tests for the DS trust table (trust_stateid.c) and the
 * TRUST_STATEID / REVOKE_STATEID / BULK_REVOKE_STATEID op handlers.
 *
 * Tests cover:
 *   A. Init / fini lifecycle
 *   B. Register and idempotent update
 *   C. Find and reference counting
 *   D. Revoke (single and bulk)
 *   E. trust_stateid_convert_expire
 *   F. TRUST_STATEID op handler (nfs4_op_trust_stateid)
 *   G. REVOKE_STATEID op handler (nfs4_op_revoke_stateid)
 *   H. BULK_REVOKE_STATEID op handler (nfs4_op_bulk_revoke_stateid)
 *   I. CHUNK_WRITE trust validation hook
 *
 * Groups A-E use only the trust table directly.  Groups F-I use a
 * minimal compound mock (see "Compound mock helpers" section) and
 * require nfs4_test_setup() / nfs4_test_teardown() so that
 * nfs4_client_alloc() and the server state are available.
 *
 * The trust table uses liburcu.  RCU thread registration is handled
 * once by reffs_test_run_suite() (CK_NOFORK mode) -- individual
 * setup/teardown fixtures must NOT call rcu_register_thread() again.
 * Groups A-E tear down the trust table between tests via
 * trust_stateid_fini().  Groups F-I rely on nfs4_protocol_deregister()
 * (called by nfs4_test_teardown) to call trust_stateid_fini().
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <netinet/in.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <check.h>
#include <urcu.h>

#include "nfsv42_xdr.h"
#include "reffs/filehandle.h"
#include "reffs/inode.h"
#include "reffs/rcu.h"
#include "reffs/server.h"
#include "reffs/time.h"
#include "reffs/super_block.h"
#include "nfs4/client.h"
#include "nfs4/compound.h"
#include "nfs4/ops.h"
#include "nfs4/trust_stateid.h"

#include "nfs4_test_harness.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/*
 * make_stateid -- build a stateid4 with a recognisable other[] pattern.
 * The seqid is set to 1 so stateid4_is_special() returns false.
 */
static stateid4 make_stateid(uint8_t fill)
{
	stateid4 s;

	s.seqid = 1;
	memset(s.other, fill, NFS4_OTHER_SIZE);
	return s;
}

/*
 * future_expire_ns -- CLOCK_MONOTONIC deadline 2 seconds in the future.
 *
 * trust_stateid_register()'s expire_mono_ns is a CLOCK_MONOTONIC value;
 * using CLOCK_REALTIME here would produce a semantically wrong value on
 * systems where the clocks have diverged (e.g., post-ntpd step).
 */
static uint64_t future_expire_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec +
	       2000000000ULL; /* +2 s */
}

/* ------------------------------------------------------------------ */
/* Fixtures                                                            */
/* ------------------------------------------------------------------ */

static void setup(void)
{
	ck_assert_int_eq(trust_stateid_init(), 0);
}

static void teardown(void)
{
	trust_stateid_fini();
}

/* ------------------------------------------------------------------ */
/* A. Init / fini                                                      */
/* ------------------------------------------------------------------ */

/*
 * Double fini must not crash -- trust_stateid_fini guards on NULL ht.
 */
START_TEST(test_init_fini_idempotent)
{
	/* setup() already called init; call fini twice. */
	trust_stateid_fini();
	trust_stateid_fini(); /* second call: no-op */
	/* reinit so teardown()'s fini is safe */
	ck_assert_int_eq(trust_stateid_init(), 0);
}
END_TEST

/*
 * find() before init (NULL ht) must return NULL, not crash.
 */
START_TEST(test_find_before_init)
{
	trust_stateid_fini(); /* drop ht */

	stateid4 s = make_stateid(0xAB);
	struct trust_entry *te = trust_stateid_find(&s);

	ck_assert_ptr_null(te);

	/* reinit so teardown is safe */
	ck_assert_int_eq(trust_stateid_init(), 0);
}
END_TEST

/* ------------------------------------------------------------------ */
/* B. Register                                                         */
/* ------------------------------------------------------------------ */

/*
 * Registering a new stateid returns 0 and the entry is findable.
 */
START_TEST(test_register_basic)
{
	stateid4 s = make_stateid(0x01);
	int ret = trust_stateid_register(&s, 42, 0xCAFE, LAYOUTIOMODE4_RW,
					 future_expire_ns(), "");
	ck_assert_int_eq(ret, 0);

	struct trust_entry *te = trust_stateid_find(&s);

	ck_assert_ptr_nonnull(te);
	ck_assert_int_eq(memcmp(te->te_other, s.other, NFS4_OTHER_SIZE), 0);
	ck_assert_uint_eq(te->te_ino, 42);
	ck_assert_uint_eq(te->te_clientid, 0xCAFE);
	ck_assert_int_eq(te->te_iomode, LAYOUTIOMODE4_RW);
	ck_assert_uint_ne(atomic_load_explicit(&te->te_expire_ns,
					       memory_order_relaxed),
			  0);
	ck_assert_uint_eq(atomic_load_explicit(&te->te_flags,
					       memory_order_relaxed) &
				  TRUST_ACTIVE,
			  TRUST_ACTIVE);
	trust_entry_put(te);
}
END_TEST

/*
 * Registering the same stateid.other twice is idempotent: the existing
 * entry is updated in-place and find() still returns one entry.
 */
START_TEST(test_register_idempotent)
{
	stateid4 s = make_stateid(0x02);

	trust_stateid_register(&s, 10, 0xCAFE, LAYOUTIOMODE4_READ,
			       future_expire_ns(), "");
	trust_stateid_register(&s, 10, 0xCAFE, LAYOUTIOMODE4_RW,
			       future_expire_ns(), "");

	struct trust_entry *te = trust_stateid_find(&s);

	ck_assert_ptr_nonnull(te);
	/* Second call updated the iomode. */
	ck_assert_int_eq(te->te_iomode, LAYOUTIOMODE4_RW);
	trust_entry_put(te);
}
END_TEST

/*
 * Principal is stored and retrievable.
 */
START_TEST(test_register_with_principal)
{
	stateid4 s = make_stateid(0x03);
	const char *principal = "nfs/mds.example.com@EXAMPLE.COM";

	trust_stateid_register(&s, 1, 0, LAYOUTIOMODE4_RW, future_expire_ns(),
			       principal);

	struct trust_entry *te = trust_stateid_find(&s);

	ck_assert_ptr_nonnull(te);
	ck_assert_str_eq(te->te_principal, principal);
	trust_entry_put(te);
}
END_TEST

/*
 * Principal longer than TRUST_PRINCIPAL_MAX-1 must be safely truncated.
 */
START_TEST(test_register_principal_truncated)
{
	stateid4 s = make_stateid(0x04);
	char long_principal[TRUST_PRINCIPAL_MAX + 64];

	memset(long_principal, 'x', sizeof(long_principal) - 1);
	long_principal[sizeof(long_principal) - 1] = '\0';

	trust_stateid_register(&s, 1, 0, LAYOUTIOMODE4_RW, future_expire_ns(),
			       long_principal);

	struct trust_entry *te = trust_stateid_find(&s);

	ck_assert_ptr_nonnull(te);
	/* Must be NUL-terminated and within bound. */
	ck_assert_int_lt((int)strlen(te->te_principal), TRUST_PRINCIPAL_MAX);
	ck_assert_int_eq(te->te_principal[TRUST_PRINCIPAL_MAX - 1], '\0');
	trust_entry_put(te);
}
END_TEST

/*
 * Two different stateids coexist in the table independently.
 */
START_TEST(test_register_two_entries)
{
	stateid4 s1 = make_stateid(0x11);
	stateid4 s2 = make_stateid(0x22);

	trust_stateid_register(&s1, 1, 0, LAYOUTIOMODE4_RW, future_expire_ns(),
			       "");
	trust_stateid_register(&s2, 2, 0, LAYOUTIOMODE4_READ,
			       future_expire_ns(), "");

	struct trust_entry *te1 = trust_stateid_find(&s1);
	struct trust_entry *te2 = trust_stateid_find(&s2);

	ck_assert_ptr_nonnull(te1);
	ck_assert_ptr_nonnull(te2);
	ck_assert_ptr_ne(te1, te2);
	ck_assert_uint_eq(te1->te_ino, 1);
	ck_assert_uint_eq(te2->te_ino, 2);

	trust_entry_put(te1);
	trust_entry_put(te2);
}
END_TEST

/* ------------------------------------------------------------------ */
/* C. Find and reference counting                                      */
/* ------------------------------------------------------------------ */

/*
 * find() returns NULL for a stateid that was never registered.
 */
START_TEST(test_find_not_found)
{
	stateid4 s = make_stateid(0xFF);
	struct trust_entry *te = trust_stateid_find(&s);

	ck_assert_ptr_null(te);
}
END_TEST

/*
 * find() increments the refcount; put() decrements it.
 * The entry must still be findable after put() (creation ref remains).
 */
START_TEST(test_find_refcount)
{
	stateid4 s = make_stateid(0x05);

	trust_stateid_register(&s, 1, 0, LAYOUTIOMODE4_RW, future_expire_ns(),
			       "");

	struct trust_entry *te = trust_stateid_find(&s);

	ck_assert_ptr_nonnull(te);
	trust_entry_put(te);

	/* Entry still alive (creation ref remains). */
	struct trust_entry *te2 = trust_stateid_find(&s);

	ck_assert_ptr_nonnull(te2);
	trust_entry_put(te2);
}
END_TEST

/*
 * trust_entry_put(NULL) must not crash.
 */
START_TEST(test_put_null)
{
	trust_entry_put(NULL);
}
END_TEST

/*
 * TRUST_ACTIVE flag is set after register.
 */
START_TEST(test_flags_active_after_register)
{
	stateid4 s = make_stateid(0x06);

	trust_stateid_register(&s, 1, 0, LAYOUTIOMODE4_RW, future_expire_ns(),
			       "");

	struct trust_entry *te = trust_stateid_find(&s);

	ck_assert_ptr_nonnull(te);
	ck_assert_uint_ne(atomic_load_explicit(&te->te_flags,
					       memory_order_acquire) &
				  TRUST_ACTIVE,
			  0);
	trust_entry_put(te);
}
END_TEST

/* ------------------------------------------------------------------ */
/* D. Revoke                                                           */
/* ------------------------------------------------------------------ */

/*
 * After revoke(), the entry is no longer findable.
 */
START_TEST(test_revoke_removes_entry)
{
	stateid4 s = make_stateid(0x07);

	trust_stateid_register(&s, 1, 0, LAYOUTIOMODE4_RW, future_expire_ns(),
			       "");
	trust_stateid_revoke(&s);

	struct trust_entry *te = trust_stateid_find(&s);

	ck_assert_ptr_null(te);
}
END_TEST

/*
 * Revoking a stateid that was never registered must not crash.
 */
START_TEST(test_revoke_not_found)
{
	stateid4 s = make_stateid(0xDE);

	trust_stateid_revoke(&s); /* must be a no-op */
}
END_TEST

/*
 * Revoking then re-registering the same stateid.other works.
 */
START_TEST(test_revoke_then_reregister)
{
	stateid4 s = make_stateid(0x08);

	trust_stateid_register(&s, 1, 0, LAYOUTIOMODE4_RW, future_expire_ns(),
			       "");
	trust_stateid_revoke(&s);
	trust_stateid_register(&s, 2, 0, LAYOUTIOMODE4_READ, future_expire_ns(),
			       "");

	struct trust_entry *te = trust_stateid_find(&s);

	ck_assert_ptr_nonnull(te);
	ck_assert_uint_eq(te->te_ino, 2);
	trust_entry_put(te);
}
END_TEST

/*
 * bulk_revoke with a specific clientid removes only that client's entries.
 */
START_TEST(test_bulk_revoke_by_clientid)
{
	stateid4 s1 = make_stateid(0x30);
	stateid4 s2 = make_stateid(0x31);
	stateid4 s3 = make_stateid(0x32);
	const clientid4 cid_a = 0xAAAA;
	const clientid4 cid_b = 0xBBBB;

	trust_stateid_register(&s1, 1, cid_a, LAYOUTIOMODE4_RW,
			       future_expire_ns(), "");
	trust_stateid_register(&s2, 2, cid_a, LAYOUTIOMODE4_RW,
			       future_expire_ns(), "");
	trust_stateid_register(&s3, 3, cid_b, LAYOUTIOMODE4_RW,
			       future_expire_ns(), "");

	trust_stateid_bulk_revoke(cid_a);

	struct trust_entry *te1 = trust_stateid_find(&s1);
	struct trust_entry *te2 = trust_stateid_find(&s2);
	struct trust_entry *te3 = trust_stateid_find(&s3);

	ck_assert_ptr_null(te1);
	ck_assert_ptr_null(te2);
	ck_assert_ptr_nonnull(te3);
	trust_entry_put(te3);
}
END_TEST

/*
 * bulk_revoke with clientid 0 clears all entries.
 */
START_TEST(test_bulk_revoke_all)
{
	stateid4 s1 = make_stateid(0x40);
	stateid4 s2 = make_stateid(0x41);
	stateid4 s3 = make_stateid(0x42);

	trust_stateid_register(&s1, 1, 0xAAAA, LAYOUTIOMODE4_RW,
			       future_expire_ns(), "");
	trust_stateid_register(&s2, 2, 0xBBBB, LAYOUTIOMODE4_RW,
			       future_expire_ns(), "");
	trust_stateid_register(&s3, 3, 0xCCCC, LAYOUTIOMODE4_RW,
			       future_expire_ns(), "");

	trust_stateid_bulk_revoke(0); /* 0 = clear all */

	ck_assert_ptr_null(trust_stateid_find(&s1));
	ck_assert_ptr_null(trust_stateid_find(&s2));
	ck_assert_ptr_null(trust_stateid_find(&s3));
}
END_TEST

/*
 * bulk_revoke on an empty table must not crash.
 */
START_TEST(test_bulk_revoke_empty)
{
	trust_stateid_bulk_revoke(0);
	trust_stateid_bulk_revoke(0xDEAD);
}
END_TEST

/* ------------------------------------------------------------------ */
/* E. trust_stateid_convert_expire                                     */
/* ------------------------------------------------------------------ */

/*
 * Valid future wall-clock expiry maps to a future monotonic deadline.
 */
START_TEST(test_convert_expire_future)
{
	struct timespec wall_ts, mono_ts;

	clock_gettime(CLOCK_REALTIME, &wall_ts);
	clock_gettime(CLOCK_MONOTONIC, &mono_ts);

	uint64_t now_wall =
		(uint64_t)wall_ts.tv_sec * 1000000000ULL + wall_ts.tv_nsec;
	uint64_t now_mono =
		(uint64_t)mono_ts.tv_sec * 1000000000ULL + mono_ts.tv_nsec;

	/* Expire 10 seconds in the future. */
	nfstime4 expire;
	expire.seconds = wall_ts.tv_sec + 10;
	expire.nseconds = (uint32_t)wall_ts.tv_nsec;

	uint64_t deadline =
		trust_stateid_convert_expire(&expire, now_wall, now_mono);

	ck_assert_uint_gt(deadline, now_mono);
	/* Should be roughly now_mono + 10s (within 1s tolerance). */
	ck_assert_uint_gt(deadline, now_mono + 9000000000ULL);
	ck_assert_uint_lt(deadline, now_mono + 11000000000ULL);
}
END_TEST

/*
 * Expiry already in the past returns 0 (rejected; op handler returns
 * NFS4ERR_INVAL rather than registering an already-expired entry).
 */
START_TEST(test_convert_expire_past)
{
	struct timespec wall_ts, mono_ts;

	clock_gettime(CLOCK_REALTIME, &wall_ts);
	clock_gettime(CLOCK_MONOTONIC, &mono_ts);

	uint64_t now_wall =
		(uint64_t)wall_ts.tv_sec * 1000000000ULL + wall_ts.tv_nsec;
	uint64_t now_mono =
		(uint64_t)mono_ts.tv_sec * 1000000000ULL + mono_ts.tv_nsec;

	/* Expire 10 seconds in the past. */
	nfstime4 expire;
	expire.seconds = wall_ts.tv_sec > 10 ? (int64_t)wall_ts.tv_sec - 10 : 0;
	expire.nseconds = 0;

	uint64_t deadline =
		trust_stateid_convert_expire(&expire, now_wall, now_mono);

	ck_assert_uint_eq(deadline, 0);
}
END_TEST

/*
 * Invalid nseconds (>= 1e9) returns 0.
 */
START_TEST(test_convert_expire_invalid_nsec)
{
	nfstime4 expire;
	expire.seconds = 0;
	expire.nseconds = 1000000000u; /* invalid */

	uint64_t deadline = trust_stateid_convert_expire(&expire, 0, 0);

	ck_assert_uint_eq(deadline, 0);
}
END_TEST

/*
 * Max safe value: remaining_ns near UINT64_MAX should not overflow.
 */
START_TEST(test_convert_expire_no_overflow)
{
	/*
	 * Give a future expire that would overflow if added naively.
	 * The function must return UINT64_MAX as the capped value.
	 */
	nfstime4 expire;
	expire.seconds = UINT32_MAX; /* very far future */
	expire.nseconds = 0;

	/* now values that force remaining_ns to be huge */
	uint64_t now_wall = 1000000000ULL; /* 1 second */
	uint64_t now_mono = UINT64_MAX - 10; /* near max */

	uint64_t deadline =
		trust_stateid_convert_expire(&expire, now_wall, now_mono);

	ck_assert_uint_eq(deadline, UINT64_MAX);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Compound mock helpers (Groups F-I)                                  */
/* ------------------------------------------------------------------ */

/*
 * cm_ctx -- minimal compound mock for op-handler tests.
 *
 * Mirrors the rg_ctx pattern from reflected_getattr_test.c but adds
 * a real nfs4_client so nc_exchgid_flags can be set.
 *
 * Usage:
 *   struct cm_ctx *cm = cm_alloc(1, EXCHGID4_FLAG_USE_PNFS_MDS);
 *   cm_set_inode(cm, inode);
 *   cm_set_op(cm, 0, OP_TRUST_STATEID);
 *   // fill args ...
 *   nfs4_op_trust_stateid(cm->compound);
 *   // check res ...
 *   cm_free(cm);
 */
struct cm_ctx {
	struct rpc_trans rt;
	struct task task;
	struct compound *compound;
	struct nfs4_client *nc; /* ref held; freed in cm_free */
};

static struct cm_ctx *cm_alloc(unsigned int nops, uint32_t exchgid_flags)
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

	/*
	 * Allocate a minimal nfs4_client with the requested
	 * EXCHANGE_ID flags.  verifier and address are synthetic.
	 */
	verifier4 v;
	struct sockaddr_in sin;

	memset(&v, 0x42, sizeof(v));
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(0x7f000002);
	sin.sin_port = htons(2049);

	cm->nc = nfs4_client_alloc(&v, &sin, 1, 0xCACE0001, 0);
	ck_assert_ptr_nonnull(cm->nc);
	cm->nc->nc_exchgid_flags = exchgid_flags;
	c->c_nfs4_client = nfs4_client_get(cm->nc);

	cm->rt.rt_compound = c;
	cm->compound = c;
	return cm;
}

/*
 * cm_set_inode -- point the mock compound's current FH at inode.
 * The caller's inode ref is borrowed (not transferred).
 */
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

/*
 * cm_set_op -- point c_curr_op at slot idx with the given opnum.
 */
static void cm_set_op(struct cm_ctx *cm, unsigned int idx, nfs_opnum4 opnum)
{
	ck_assert_uint_lt(idx, cm->compound->c_args->argarray.argarray_len);
	cm->compound->c_args->argarray.argarray_val[idx].argop = opnum;
	cm->compound->c_res->resarray.resarray_val[idx].resop = opnum;
	cm->compound->c_curr_op = (int)idx;
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

/* ------------------------------------------------------------------ */
/* Fixtures for Groups F-I                                             */
/* ------------------------------------------------------------------ */

static struct super_block *g_op_sb;
static struct inode *g_op_inode;

static void op_setup(void)
{
	/*
	 * nfs4_protocol_register() (called by nfs4_test_setup) already
	 * calls trust_stateid_init().  Do NOT call it here again -- a
	 * double-init creates two trust reaper threads but only one
	 * handle is tracked, causing trust_stateid_fini() to hang for
	 * up to TRUST_REAPER_SCAN_SEC (60 s) waiting for the untracked
	 * thread to time out.
	 */
	nfs4_test_setup();

	g_op_sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	ck_assert_ptr_nonnull(g_op_sb);

	g_op_inode = inode_alloc(g_op_sb, 500);
	ck_assert_ptr_nonnull(g_op_inode);
	g_op_inode->i_mode = S_IFREG | 0640;
}

static void op_teardown(void)
{
	inode_active_put(g_op_inode);
	g_op_inode = NULL;
	super_block_put(g_op_sb);
	g_op_sb = NULL;
	/*
	 * nfs4_protocol_deregister() already calls trust_stateid_fini();
	 * no redundant call needed here.
	 */
	nfs4_test_teardown();
}

/* ------------------------------------------------------------------ */
/* F. TRUST_STATEID op handler                                         */
/* ------------------------------------------------------------------ */

/*
 * A valid TRUST_STATEID from an MDS client registers an entry.
 */
START_TEST(test_op_trust_stateid_ok)
{
	struct cm_ctx *cm = cm_alloc(1, EXCHGID4_FLAG_USE_PNFS_MDS);

	cm_set_inode(cm, g_op_inode);
	cm_set_op(cm, 0, OP_TRUST_STATEID);

	TRUST_STATEID4args *args =
		&cm->compound->c_args->argarray.argarray_val[0]
			 .nfs_argop4_u.optrust_stateid;
	args->tsa_layout_stateid = make_stateid(0x11);
	args->tsa_iomode = LAYOUTIOMODE4_RW;
	/* expire 30 seconds from now (wall clock) */
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	args->tsa_expire.seconds = (int64_t)now.tv_sec + 30;
	args->tsa_expire.nseconds = 0;
	args->tsa_principal.utf8string_len = 0;
	args->tsa_principal.utf8string_val = NULL;

	nfs4_op_trust_stateid(cm->compound);

	TRUST_STATEID4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					  .nfs_resop4_u.optrust_stateid;
	ck_assert_int_eq(res->tsr_status, NFS4_OK);

	/* Entry must be findable in the trust table. */
	struct trust_entry *te = trust_stateid_find(&args->tsa_layout_stateid);
	ck_assert_ptr_nonnull(te);
	trust_entry_put(te);

	cm_free(cm);
}
END_TEST

/*
 * Without EXCHGID4_FLAG_USE_PNFS_MDS the DS must return NFS4ERR_PERM.
 */
START_TEST(test_op_trust_stateid_not_from_mds)
{
	struct cm_ctx *cm = cm_alloc(1, 0); /* no MDS flag */

	cm_set_inode(cm, g_op_inode);
	cm_set_op(cm, 0, OP_TRUST_STATEID);

	TRUST_STATEID4args *args =
		&cm->compound->c_args->argarray.argarray_val[0]
			 .nfs_argop4_u.optrust_stateid;
	args->tsa_layout_stateid = make_stateid(0x22);
	args->tsa_iomode = LAYOUTIOMODE4_RW;
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	args->tsa_expire.seconds = (int64_t)now.tv_sec + 30;
	args->tsa_expire.nseconds = 0;

	nfs4_op_trust_stateid(cm->compound);

	TRUST_STATEID4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					  .nfs_resop4_u.optrust_stateid;
	ck_assert_int_eq(res->tsr_status, NFS4ERR_PERM);

	cm_free(cm);
}
END_TEST

/*
 * The anonymous stateid (capability probe) must return NFS4ERR_INVAL,
 * not NFS4ERR_BAD_STATEID, so the MDS can distinguish "not supported"
 * from "bad stateid".
 */
START_TEST(test_op_trust_stateid_anon_rejected)
{
	struct cm_ctx *cm = cm_alloc(1, EXCHGID4_FLAG_USE_PNFS_MDS);

	cm_set_inode(cm, g_op_inode);
	cm_set_op(cm, 0, OP_TRUST_STATEID);

	TRUST_STATEID4args *args =
		&cm->compound->c_args->argarray.argarray_val[0]
			 .nfs_argop4_u.optrust_stateid;
	/* anonymous stateid: seqid=0, other=all-zeros */
	memset(&args->tsa_layout_stateid, 0, sizeof(args->tsa_layout_stateid));
	args->tsa_iomode = LAYOUTIOMODE4_RW;
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	args->tsa_expire.seconds = (int64_t)now.tv_sec + 30;
	args->tsa_expire.nseconds = 0;

	nfs4_op_trust_stateid(cm->compound);

	TRUST_STATEID4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					  .nfs_resop4_u.optrust_stateid;
	ck_assert_int_eq(res->tsr_status, NFS4ERR_INVAL);

	cm_free(cm);
}
END_TEST

/*
 * No current FH must return NFS4ERR_NOFILEHANDLE.
 */
START_TEST(test_op_trust_stateid_no_fh)
{
	struct cm_ctx *cm = cm_alloc(1, EXCHGID4_FLAG_USE_PNFS_MDS);
	/* do NOT call cm_set_inode -- c_curr_nfh stays zero */

	cm_set_op(cm, 0, OP_TRUST_STATEID);

	TRUST_STATEID4args *args =
		&cm->compound->c_args->argarray.argarray_val[0]
			 .nfs_argop4_u.optrust_stateid;
	args->tsa_layout_stateid = make_stateid(0x33);
	args->tsa_iomode = LAYOUTIOMODE4_RW;
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	args->tsa_expire.seconds = (int64_t)now.tv_sec + 30;
	args->tsa_expire.nseconds = 0;

	nfs4_op_trust_stateid(cm->compound);

	TRUST_STATEID4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					  .nfs_resop4_u.optrust_stateid;
	ck_assert_int_eq(res->tsr_status, NFS4ERR_NOFILEHANDLE);

	cm_free(cm);
}
END_TEST

/*
 * A past expiry must return NFS4ERR_INVAL.
 */
START_TEST(test_op_trust_stateid_past_expire)
{
	struct cm_ctx *cm = cm_alloc(1, EXCHGID4_FLAG_USE_PNFS_MDS);

	cm_set_inode(cm, g_op_inode);
	cm_set_op(cm, 0, OP_TRUST_STATEID);

	TRUST_STATEID4args *args =
		&cm->compound->c_args->argarray.argarray_val[0]
			 .nfs_argop4_u.optrust_stateid;
	args->tsa_layout_stateid = make_stateid(0x44);
	args->tsa_iomode = LAYOUTIOMODE4_RW;
	/* expire at epoch = far in the past */
	args->tsa_expire.seconds = 1;
	args->tsa_expire.nseconds = 0;

	nfs4_op_trust_stateid(cm->compound);

	TRUST_STATEID4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					  .nfs_resop4_u.optrust_stateid;
	ck_assert_int_eq(res->tsr_status, NFS4ERR_INVAL);

	cm_free(cm);
}
END_TEST

/*
 * A non-empty tsa_principal from an AUTH_SYS compound (c_gss_principal
 * == NULL) must return NFS4ERR_ACCESS.
 */
START_TEST(test_op_trust_stateid_principal_on_auth_sys)
{
	struct cm_ctx *cm = cm_alloc(1, EXCHGID4_FLAG_USE_PNFS_MDS);

	cm_set_inode(cm, g_op_inode);
	cm_set_op(cm, 0, OP_TRUST_STATEID);
	/* c_gss_principal stays NULL (AUTH_SYS, the default) */

	TRUST_STATEID4args *args =
		&cm->compound->c_args->argarray.argarray_val[0]
			 .nfs_argop4_u.optrust_stateid;
	args->tsa_layout_stateid = make_stateid(0x55);
	args->tsa_iomode = LAYOUTIOMODE4_RW;
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	args->tsa_expire.seconds = (int64_t)now.tv_sec + 30;
	args->tsa_expire.nseconds = 0;
	args->tsa_principal.utf8string_val = (char *)"nfs/ds@REALM";
	args->tsa_principal.utf8string_len = 12;

	nfs4_op_trust_stateid(cm->compound);

	TRUST_STATEID4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					  .nfs_resop4_u.optrust_stateid;
	ck_assert_int_eq(res->tsr_status, NFS4ERR_ACCESS);

	cm_free(cm);
}
END_TEST

/* ------------------------------------------------------------------ */
/* G. REVOKE_STATEID op handler                                        */
/* ------------------------------------------------------------------ */

/*
 * Revoking a registered entry removes it from the trust table.
 */
START_TEST(test_op_revoke_stateid_ok)
{
	/* Pre-register an entry directly. */
	stateid4 stid = make_stateid(0xAA);
	clientid4 cid = 0x1234;

	ck_assert_int_eq(trust_stateid_register(&stid, 999, cid,
						LAYOUTIOMODE4_RW,
						future_expire_ns(), ""),
			 0);

	struct cm_ctx *cm = cm_alloc(1, EXCHGID4_FLAG_USE_PNFS_MDS);

	cm_set_inode(cm, g_op_inode);
	cm_set_op(cm, 0, OP_REVOKE_STATEID);

	REVOKE_STATEID4args *args =
		&cm->compound->c_args->argarray.argarray_val[0]
			 .nfs_argop4_u.oprevoke_stateid;
	args->rsa_layout_stateid = stid;

	nfs4_op_revoke_stateid(cm->compound);

	REVOKE_STATEID4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					   .nfs_resop4_u.oprevoke_stateid;
	ck_assert_int_eq(res->rsr_status, NFS4_OK);

	/* Entry must be gone. */
	struct trust_entry *te = trust_stateid_find(&stid);

	ck_assert_ptr_null(te);

	cm_free(cm);
}
END_TEST

/*
 * Revoking without MDS flag must fail with NFS4ERR_PERM.
 */
START_TEST(test_op_revoke_stateid_not_from_mds)
{
	struct cm_ctx *cm = cm_alloc(1, 0);

	cm_set_inode(cm, g_op_inode);
	cm_set_op(cm, 0, OP_REVOKE_STATEID);

	REVOKE_STATEID4args *args =
		&cm->compound->c_args->argarray.argarray_val[0]
			 .nfs_argop4_u.oprevoke_stateid;
	args->rsa_layout_stateid = make_stateid(0xBB);

	nfs4_op_revoke_stateid(cm->compound);

	REVOKE_STATEID4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					   .nfs_resop4_u.oprevoke_stateid;
	ck_assert_int_eq(res->rsr_status, NFS4ERR_PERM);

	cm_free(cm);
}
END_TEST

/*
 * A special stateid (anonymous) must return NFS4ERR_BAD_STATEID.
 */
START_TEST(test_op_revoke_stateid_special_stateid)
{
	struct cm_ctx *cm = cm_alloc(1, EXCHGID4_FLAG_USE_PNFS_MDS);

	cm_set_inode(cm, g_op_inode);
	cm_set_op(cm, 0, OP_REVOKE_STATEID);

	REVOKE_STATEID4args *args =
		&cm->compound->c_args->argarray.argarray_val[0]
			 .nfs_argop4_u.oprevoke_stateid;
	memset(&args->rsa_layout_stateid, 0, sizeof(args->rsa_layout_stateid));

	nfs4_op_revoke_stateid(cm->compound);

	REVOKE_STATEID4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					   .nfs_resop4_u.oprevoke_stateid;
	ck_assert_int_eq(res->rsr_status, NFS4ERR_BAD_STATEID);

	cm_free(cm);
}
END_TEST

/*
 * No current FH must return NFS4ERR_NOFILEHANDLE.
 */
START_TEST(test_op_revoke_stateid_no_fh)
{
	struct cm_ctx *cm = cm_alloc(1, EXCHGID4_FLAG_USE_PNFS_MDS);

	cm_set_op(cm, 0, OP_REVOKE_STATEID);

	REVOKE_STATEID4args *args =
		&cm->compound->c_args->argarray.argarray_val[0]
			 .nfs_argop4_u.oprevoke_stateid;
	args->rsa_layout_stateid = make_stateid(0xCC);

	nfs4_op_revoke_stateid(cm->compound);

	REVOKE_STATEID4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					   .nfs_resop4_u.oprevoke_stateid;
	ck_assert_int_eq(res->rsr_status, NFS4ERR_NOFILEHANDLE);

	cm_free(cm);
}
END_TEST

/* ------------------------------------------------------------------ */
/* H. BULK_REVOKE_STATEID op handler                                   */
/* ------------------------------------------------------------------ */

/*
 * Bulk-revoking a clientid removes all entries for that client.
 */
START_TEST(test_op_bulk_revoke_stateid_ok)
{
	clientid4 cid = 0xDEAD;
	stateid4 s1 = make_stateid(0xD1);
	stateid4 s2 = make_stateid(0xD2);

	ck_assert_int_eq(trust_stateid_register(&s1, 111, cid, LAYOUTIOMODE4_RW,
						future_expire_ns(), ""),
			 0);
	ck_assert_int_eq(trust_stateid_register(&s2, 222, cid, LAYOUTIOMODE4_RW,
						future_expire_ns(), ""),
			 0);

	struct cm_ctx *cm = cm_alloc(1, EXCHGID4_FLAG_USE_PNFS_MDS);

	cm_set_op(cm, 0, OP_BULK_REVOKE_STATEID);

	BULK_REVOKE_STATEID4args *args =
		&cm->compound->c_args->argarray.argarray_val[0]
			 .nfs_argop4_u.opbulk_revoke_stateid;
	args->brsa_clientid = cid;

	nfs4_op_bulk_revoke_stateid(cm->compound);

	BULK_REVOKE_STATEID4res *res =
		&cm->compound->c_res->resarray.resarray_val[0]
			 .nfs_resop4_u.opbulk_revoke_stateid;
	ck_assert_int_eq(res->brsr_status, NFS4_OK);

	ck_assert_ptr_null(trust_stateid_find(&s1));
	ck_assert_ptr_null(trust_stateid_find(&s2));

	cm_free(cm);
}
END_TEST

/*
 * Bulk revoke without MDS flag must fail.
 */
START_TEST(test_op_bulk_revoke_stateid_not_from_mds)
{
	struct cm_ctx *cm = cm_alloc(1, 0);

	cm_set_op(cm, 0, OP_BULK_REVOKE_STATEID);

	BULK_REVOKE_STATEID4args *args =
		&cm->compound->c_args->argarray.argarray_val[0]
			 .nfs_argop4_u.opbulk_revoke_stateid;
	args->brsa_clientid = 0x1111;

	nfs4_op_bulk_revoke_stateid(cm->compound);

	BULK_REVOKE_STATEID4res *res =
		&cm->compound->c_res->resarray.resarray_val[0]
			 .nfs_resop4_u.opbulk_revoke_stateid;
	ck_assert_int_eq(res->brsr_status, NFS4ERR_PERM);

	cm_free(cm);
}
END_TEST

/* ------------------------------------------------------------------ */
/* I. CHUNK_WRITE trust validation hook                                */
/* ------------------------------------------------------------------ */

/*
 * Build a minimal CHUNK_WRITE compound.  A 512-byte payload of zeros,
 * no CRC32 array, anonymous owner.  The only field under test is the
 * stateid, so the actual write is expected to succeed (or fail for
 * reasons unrelated to trust).
 *
 * We accept NFS4_OK or NFS4ERR_IO as "passed trust validation" --
 * NFS4ERR_IO means the chunk store wasn't initialised (fine for unit
 * tests), but it means the trust check didn't block the write.
 */
#define CHUNK_TEST_SIZE 512

static void chunk_set_write_args(struct cm_ctx *cm, const stateid4 *stid)
{
	static char buf[CHUNK_TEST_SIZE];

	cm_set_op(cm, 0, OP_CHUNK_WRITE);

	CHUNK_WRITE4args *args = &cm->compound->c_args->argarray.argarray_val[0]
					  .nfs_argop4_u.opchunk_write;

	if (stid)
		args->cwa_stateid = *stid;
	/* else: zero-initialised = anonymous stateid */

	args->cwa_offset = 0;
	args->cwa_stable = FILE_SYNC4;
	args->cwa_chunk_size = CHUNK_TEST_SIZE;
	args->cwa_chunks.cwa_chunks_val = buf;
	args->cwa_chunks.cwa_chunks_len = CHUNK_TEST_SIZE;
	/* no CRC32 array */
}

/*
 * A stateid registered in the trust table (TRUST_ACTIVE, not expired)
 * must pass the validation hook.
 */
START_TEST(test_chunk_trusted_stateid_allowed)
{
	stateid4 stid = make_stateid(0xE1);

	ck_assert_int_eq(trust_stateid_register(&stid, g_op_inode->i_ino, 0,
						LAYOUTIOMODE4_RW,
						future_expire_ns(), ""),
			 0);

	struct cm_ctx *cm = cm_alloc(1, 0); /* CHUNK_WRITE has no MDS check */

	cm_set_inode(cm, g_op_inode);
	chunk_set_write_args(cm, &stid);

	nfs4_op_chunk_write(cm->compound);

	CHUNK_WRITE4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					.nfs_resop4_u.opchunk_write;
	/* NFS4_OK or NFS4ERR_IO are both acceptable: trust check passed. */
	ck_assert(res->cwr_status == NFS4_OK || res->cwr_status == NFS4ERR_IO);

	/* Free the per-chunk status array allocated by nfs4_op_chunk_write. */
	free(res->CHUNK_WRITE4res_u.cwr_resok4.cwr_block_status
		     .cwr_block_status_val);
	res->CHUNK_WRITE4res_u.cwr_resok4.cwr_block_status.cwr_block_status_val =
		NULL;

	cm_free(cm);
}
END_TEST

/*
 * A stateid NOT in the trust table must be rejected with
 * NFS4ERR_BAD_STATEID.  The table is non-empty (we inserted a
 * different entry), so the validation hook is active.
 */
START_TEST(test_chunk_untrusted_stateid_rejected)
{
	/* Insert a dummy entry so the table is non-empty. */
	stateid4 other = make_stateid(0xE2);

	ck_assert_int_eq(trust_stateid_register(&other, 1, 0, LAYOUTIOMODE4_RW,
						future_expire_ns(), ""),
			 0);

	stateid4 unknown = make_stateid(0xEE); /* not in table */

	struct cm_ctx *cm = cm_alloc(1, 0);

	cm_set_inode(cm, g_op_inode);
	chunk_set_write_args(cm, &unknown);

	nfs4_op_chunk_write(cm->compound);

	CHUNK_WRITE4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					.nfs_resop4_u.opchunk_write;
	ck_assert_int_eq(res->cwr_status, NFS4ERR_BAD_STATEID);

	cm_free(cm);
}
END_TEST

/*
 * A stateid whose trust entry has already expired must return
 * NFS4ERR_BAD_STATEID.
 */
START_TEST(test_chunk_expired_stateid_rejected)
{
	stateid4 stid = make_stateid(0xE3);
	uint64_t expired_ns = 1; /* 1 ns after epoch = expired */

	ck_assert_int_eq(trust_stateid_register(&stid, g_op_inode->i_ino, 0,
						LAYOUTIOMODE4_RW, expired_ns,
						""),
			 0);

	struct cm_ctx *cm = cm_alloc(1, 0);

	cm_set_inode(cm, g_op_inode);
	chunk_set_write_args(cm, &stid);

	nfs4_op_chunk_write(cm->compound);

	CHUNK_WRITE4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					.nfs_resop4_u.opchunk_write;
	ck_assert_int_eq(res->cwr_status, NFS4ERR_BAD_STATEID);

	cm_free(cm);
}
END_TEST

/*
 * An entry with TRUST_PENDING must return NFS4ERR_DELAY.
 */
START_TEST(test_chunk_pending_stateid_delay)
{
	stateid4 stid = make_stateid(0xE4);

	ck_assert_int_eq(trust_stateid_register(&stid, g_op_inode->i_ino, 0,
						LAYOUTIOMODE4_RW,
						future_expire_ns(), ""),
			 0);

	/* Flip TRUST_ACTIVE off, TRUST_PENDING on. */
	struct trust_entry *te = trust_stateid_find(&stid);

	ck_assert_ptr_nonnull(te);
	atomic_fetch_and_explicit(&te->te_flags, ~TRUST_ACTIVE,
				  memory_order_release);
	atomic_fetch_or_explicit(&te->te_flags, TRUST_PENDING,
				 memory_order_release);
	trust_entry_put(te);

	struct cm_ctx *cm = cm_alloc(1, 0);

	cm_set_inode(cm, g_op_inode);
	chunk_set_write_args(cm, &stid);

	nfs4_op_chunk_write(cm->compound);

	CHUNK_WRITE4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					.nfs_resop4_u.opchunk_write;
	ck_assert_int_eq(res->cwr_status, NFS4ERR_DELAY);

	cm_free(cm);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Group J: trust_stateid_renewal_scan                                 */
/* ------------------------------------------------------------------ */

/*
 * test_trust_renewal_extends_expire:
 * An entry expiring in 1 second (well below lease_sec/2 = 45s) should
 * have its te_expire_ns extended to approximately now + lease_sec.
 */
START_TEST(test_trust_renewal_extends_expire)
{
	stateid4 stid = make_stateid(0x61);
	uint64_t before = reffs_now_ns();
	/* Expires in 1 second -- below the half-lease threshold of 45s */
	uint64_t soon = before + 1000000000ULL;

	ck_assert_int_eq(trust_stateid_register(&stid, 100, 1, LAYOUTIOMODE4_RW,
						soon, ""),
			 0);

	trust_stateid_renewal_scan(90);

	struct trust_entry *te = trust_stateid_find(&stid);
	ck_assert_ptr_nonnull(te);

	uint64_t exp =
		atomic_load_explicit(&te->te_expire_ns, memory_order_acquire);
	trust_entry_put(te);

	/* Extended: should be roughly before + 90s (allow +/-2s clock jitter) */
	ck_assert_uint_ge(exp, before + 88ULL * 1000000000ULL);

	trust_stateid_revoke(&stid);
}
END_TEST

/*
 * test_trust_renewal_skips_far_future:
 * An entry expiring in 100s is well beyond the 45s renewal threshold
 * (lease_sec=90, threshold=45s).  The expiry must not be changed.
 */
START_TEST(test_trust_renewal_skips_far_future)
{
	stateid4 stid = make_stateid(0x62);
	uint64_t far = reffs_now_ns() + 100ULL * 1000000000ULL;

	ck_assert_int_eq(trust_stateid_register(&stid, 100, 1, LAYOUTIOMODE4_RW,
						far, ""),
			 0);

	trust_stateid_renewal_scan(90); /* threshold = 45s; 100s > 45s */

	struct trust_entry *te = trust_stateid_find(&stid);
	ck_assert_ptr_nonnull(te);

	uint64_t exp =
		atomic_load_explicit(&te->te_expire_ns, memory_order_acquire);
	trust_entry_put(te);

	/*
	 * Expiry must not have been shortened.  Allow 2s of clock drift
	 * between registration and the scan.
	 */
	ck_assert_uint_ge(exp, far - 2ULL * 1000000000ULL);

	trust_stateid_revoke(&stid);
}
END_TEST

/*
 * test_trust_renewal_skips_expired:
 * An entry whose te_expire_ns is in the past (exp <= now) is left for
 * the expiry pass.  The renewal scan must not touch it.
 */
START_TEST(test_trust_renewal_skips_expired)
{
	stateid4 stid = make_stateid(0x63);
	uint64_t past = 1000000000ULL; /* epoch + 1s -- safely in the past */

	ck_assert_int_eq(trust_stateid_register(&stid, 100, 1, LAYOUTIOMODE4_RW,
						past, ""),
			 0);

	trust_stateid_renewal_scan(90);

	struct trust_entry *te = trust_stateid_find(&stid);
	ck_assert_ptr_nonnull(te);

	uint64_t exp =
		atomic_load_explicit(&te->te_expire_ns, memory_order_acquire);
	trust_entry_put(te);

	/* Still the original past value -- renewal does not touch expired entries */
	ck_assert_uint_eq(exp, past);

	trust_stateid_revoke(&stid);
}
END_TEST

/*
 * test_trust_renewal_multiple:
 * Two entries: one near-expiry, one far-future.  Only the near one renews.
 */
START_TEST(test_trust_renewal_multiple)
{
	stateid4 near_stid = make_stateid(0x64);
	stateid4 far_stid = make_stateid(0x65);
	uint64_t now = reffs_now_ns();
	uint64_t soon = now + 1000000000ULL; /* 1s */
	uint64_t far = now + 100ULL * 1000000000ULL; /* 100s */

	ck_assert_int_eq(trust_stateid_register(&near_stid, 100, 1,
						LAYOUTIOMODE4_RW, soon, ""),
			 0);
	ck_assert_int_eq(trust_stateid_register(&far_stid, 101, 2,
						LAYOUTIOMODE4_RW, far, ""),
			 0);

	trust_stateid_renewal_scan(90);

	struct trust_entry *te_near = trust_stateid_find(&near_stid);
	struct trust_entry *te_far = trust_stateid_find(&far_stid);
	ck_assert_ptr_nonnull(te_near);
	ck_assert_ptr_nonnull(te_far);

	uint64_t exp_near = atomic_load_explicit(&te_near->te_expire_ns,
						 memory_order_acquire);
	uint64_t exp_far = atomic_load_explicit(&te_far->te_expire_ns,
						memory_order_acquire);

	trust_entry_put(te_near);
	trust_entry_put(te_far);

	/* Near entry was extended */
	ck_assert_uint_ge(exp_near, now + 88ULL * 1000000000ULL);
	/* Far entry was not changed */
	ck_assert_uint_ge(exp_far, far - 2ULL * 1000000000ULL);

	trust_stateid_revoke(&near_stid);
	trust_stateid_revoke(&far_stid);
}
END_TEST

/*
 * test_trust_renewal_zero_lease:
 * With lease_sec=0 the threshold is 0 and no entry qualifies for
 * renewal (nothing has a positive remaining lifetime that is < 0).
 * Verify the function does not crash.
 */
START_TEST(test_trust_renewal_zero_lease)
{
	stateid4 stid = make_stateid(0x66);
	uint64_t soon = reffs_now_ns() + 1000000000ULL;

	ck_assert_int_eq(trust_stateid_register(&stid, 100, 1, LAYOUTIOMODE4_RW,
						soon, ""),
			 0);

	/* lease_sec=0: threshold=0, new_lifetime=0 -- no renewal */
	trust_stateid_renewal_scan(0);

	struct trust_entry *te = trust_stateid_find(&stid);
	ck_assert_ptr_nonnull(te);

	uint64_t exp =
		atomic_load_explicit(&te->te_expire_ns, memory_order_acquire);
	trust_entry_put(te);

	/* Entry should still expire in ~1s, not be zeroed out */
	ck_assert_uint_gt(exp, reffs_now_ns());

	trust_stateid_revoke(&stid);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

static Suite *trust_stateid_suite(void)
{
	Suite *s = suite_create("trust_stateid");

	TCase *tc_init = tcase_create("init_fini");
	tcase_add_checked_fixture(tc_init, setup, teardown);
	tcase_add_test(tc_init, test_init_fini_idempotent);
	tcase_add_test(tc_init, test_find_before_init);
	suite_add_tcase(s, tc_init);

	TCase *tc_register = tcase_create("register");
	tcase_add_checked_fixture(tc_register, setup, teardown);
	tcase_add_test(tc_register, test_register_basic);
	tcase_add_test(tc_register, test_register_idempotent);
	tcase_add_test(tc_register, test_register_with_principal);
	tcase_add_test(tc_register, test_register_principal_truncated);
	tcase_add_test(tc_register, test_register_two_entries);
	suite_add_tcase(s, tc_register);

	TCase *tc_find = tcase_create("find");
	tcase_add_checked_fixture(tc_find, setup, teardown);
	tcase_add_test(tc_find, test_find_not_found);
	tcase_add_test(tc_find, test_find_refcount);
	tcase_add_test(tc_find, test_put_null);
	tcase_add_test(tc_find, test_flags_active_after_register);
	suite_add_tcase(s, tc_find);

	TCase *tc_revoke = tcase_create("revoke");
	tcase_add_checked_fixture(tc_revoke, setup, teardown);
	tcase_add_test(tc_revoke, test_revoke_removes_entry);
	tcase_add_test(tc_revoke, test_revoke_not_found);
	tcase_add_test(tc_revoke, test_revoke_then_reregister);
	tcase_add_test(tc_revoke, test_bulk_revoke_by_clientid);
	tcase_add_test(tc_revoke, test_bulk_revoke_all);
	tcase_add_test(tc_revoke, test_bulk_revoke_empty);
	suite_add_tcase(s, tc_revoke);

	TCase *tc_expire = tcase_create("convert_expire");
	tcase_add_test(tc_expire, test_convert_expire_future);
	tcase_add_test(tc_expire, test_convert_expire_past);
	tcase_add_test(tc_expire, test_convert_expire_invalid_nsec);
	tcase_add_test(tc_expire, test_convert_expire_no_overflow);
	suite_add_tcase(s, tc_expire);

	TCase *tc_f = tcase_create("op_trust_stateid");
	tcase_add_checked_fixture(tc_f, op_setup, op_teardown);
	tcase_add_test(tc_f, test_op_trust_stateid_ok);
	tcase_add_test(tc_f, test_op_trust_stateid_not_from_mds);
	tcase_add_test(tc_f, test_op_trust_stateid_anon_rejected);
	tcase_add_test(tc_f, test_op_trust_stateid_no_fh);
	tcase_add_test(tc_f, test_op_trust_stateid_past_expire);
	tcase_add_test(tc_f, test_op_trust_stateid_principal_on_auth_sys);
	suite_add_tcase(s, tc_f);

	TCase *tc_g = tcase_create("op_revoke_stateid");
	tcase_add_checked_fixture(tc_g, op_setup, op_teardown);
	tcase_add_test(tc_g, test_op_revoke_stateid_ok);
	tcase_add_test(tc_g, test_op_revoke_stateid_not_from_mds);
	tcase_add_test(tc_g, test_op_revoke_stateid_special_stateid);
	tcase_add_test(tc_g, test_op_revoke_stateid_no_fh);
	suite_add_tcase(s, tc_g);

	TCase *tc_h = tcase_create("op_bulk_revoke_stateid");
	tcase_add_checked_fixture(tc_h, op_setup, op_teardown);
	tcase_add_test(tc_h, test_op_bulk_revoke_stateid_ok);
	tcase_add_test(tc_h, test_op_bulk_revoke_stateid_not_from_mds);
	suite_add_tcase(s, tc_h);

	TCase *tc_i = tcase_create("chunk_trust_hook");
	tcase_add_checked_fixture(tc_i, op_setup, op_teardown);
	tcase_add_test(tc_i, test_chunk_trusted_stateid_allowed);
	tcase_add_test(tc_i, test_chunk_untrusted_stateid_rejected);
	tcase_add_test(tc_i, test_chunk_expired_stateid_rejected);
	tcase_add_test(tc_i, test_chunk_pending_stateid_delay);
	suite_add_tcase(s, tc_i);

	TCase *tc_j = tcase_create("renewal");
	tcase_add_checked_fixture(tc_j, setup, teardown);
	tcase_add_test(tc_j, test_trust_renewal_extends_expire);
	tcase_add_test(tc_j, test_trust_renewal_skips_far_future);
	tcase_add_test(tc_j, test_trust_renewal_skips_expired);
	tcase_add_test(tc_j, test_trust_renewal_multiple);
	tcase_add_test(tc_j, test_trust_renewal_zero_lease);
	suite_add_tcase(s, tc_j);

	return s;
}

int main(void)
{
	return reffs_test_run_suite(trust_stateid_suite(), NULL, NULL);
}
