/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * Bulk layout return and recall (RFC 8881 sec 12.5.5.2.1.5, 18.44,
 * 20.3).
 *
 * Fixture: one client holding layout stateids on two files in the
 * root fsid (sb 1) and one file in a second fsid, plus a second
 * client with a layout on the root fsid that must never be touched.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <arpa/inet.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include <check.h>
#include <urcu.h>
#include <urcu/rculfhash.h>

#include "nfsv42_xdr.h"
#include "reffs/dirent.h"
#include "reffs/filehandle.h"
#include "reffs/fs.h"
#include "reffs/inode.h"
#include "reffs/rpc.h"
#include "reffs/server.h"
#include "reffs/stateid.h"
#include "reffs/super_block.h"
#include "reffs/task.h"
#include "nfs4/client.h"
#include "nfs4/client_persist.h"
#include "nfs4/cb.h"
#include "nfs4/compound.h"
#include "nfs4/layout_recall.h"
#include "nfs4/ops.h"
#include "nfs4/session.h"
#include "nfs4/stateid.h"
#include "nfs4_test_harness.h"

#define LR_TEST_SB2_ID 50

static struct server_state *g_ss;
static struct nfs4_client *g_nc;
static struct nfs4_client *g_other;
static struct super_block *g_sb1;
static struct super_block *g_sb2;
static struct inode *g_a1, *g_a2, *g_b1;

static struct nfs4_client *lr_make_client(const char *owner_str, uint32_t addr)
{
	static char domain[] = "lr-test.example.com";
	static char name[] = "lr-test";
	struct nfs_impl_id4 impl = {
		.nii_domain = { .utf8string_val = domain,
				.utf8string_len = sizeof(domain) - 1 },
		.nii_name = { .utf8string_val = name,
			      .utf8string_len = sizeof(name) - 1 },
	};
	client_owner4 owner = {
		.co_ownerid = { .co_ownerid_val = (char *)owner_str,
				.co_ownerid_len = strlen(owner_str) },
	};
	struct sockaddr_in sin = { .sin_family = AF_INET };
	verifier4 v;
	nfsstat4 st;
	struct nfs4_client *nc;

	memset(&v, 0xA5, sizeof(v));
	sin.sin_addr.s_addr = htonl(addr);
	nc = nfs4_client_alloc_or_find(g_ss, &owner, &impl, &v, &sin, 1001,
				       false, &st);
	ck_assert_ptr_nonnull(nc);
	return nc;
}

static struct inode *lr_new_inode(struct super_block *sb)
{
	uint64_t ino =
		__atomic_add_fetch(&sb->sb_next_ino, 1, __ATOMIC_RELAXED);
	struct inode *inode = inode_alloc(sb, ino);

	ck_assert_ptr_nonnull(inode);
	return inode;
}

static void lr_grant(struct inode *inode, struct nfs4_client *nc,
		     uint64_t iomode_bits)
{
	struct layout_stateid *ls =
		layout_stateid_alloc(inode, nfs4_client_to_client(nc));

	ck_assert_ptr_nonnull(ls);
	/* The creation ref stays with the hash tables. */
	__atomic_or_fetch(&ls->ls_state, iomode_bits, __ATOMIC_RELEASE);
}

/* Layout stateids nc holds on sb_id (0 = any), optionally needing bits. */
static unsigned int lr_count(struct nfs4_client *nc, uint64_t sb_id,
			     uint64_t need_bits)
{
	struct client *c = nfs4_client_to_client(nc);
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;
	unsigned int n = 0;

	rcu_read_lock();
	cds_lfht_for_each(c->c_stateids, &iter, node)
	{
		struct stateid *stid =
			caa_container_of(node, struct stateid, s_client_node);

		if (stid->s_tag != Layout_Stateid)
			continue;
		if (sb_id && stid->s_inode->i_sb->sb_id != sb_id)
			continue;
		if (need_bits &&
		    !(__atomic_load_n(&stid_to_layout(stid)->ls_state,
				      __ATOMIC_ACQUIRE) &
		      need_bits))
			continue;
		n++;
	}
	rcu_read_unlock();
	return n;
}

static void lr_setup(void)
{
	nfs4_test_setup();
	g_ss = server_state_find();
	ck_assert_ptr_nonnull(g_ss);

	g_sb1 = super_block_find(SUPER_BLOCK_ROOT_ID);
	ck_assert_ptr_nonnull(g_sb1);
	ck_assert_int_eq(reffs_fs_mkdir("/fsb", 0755), 0);
	g_sb2 = super_block_alloc(LR_TEST_SB2_ID, (char *)"/fsb",
				  REFFS_STORAGE_RAM, NULL);
	ck_assert_ptr_nonnull(g_sb2);
	ck_assert_int_eq(super_block_dirent_create(g_sb2, NULL,
						   reffs_life_action_birth),
			 0);
	ck_assert_int_eq(super_block_mount(g_sb2, "/fsb"), 0);

	g_a1 = lr_new_inode(g_sb1);
	g_a2 = lr_new_inode(g_sb1);
	g_b1 = lr_new_inode(g_sb2);

	g_nc = lr_make_client("lr-client-1", 0x7f000001);
	g_other = lr_make_client("lr-client-2", 0x7f000002);

	lr_grant(g_a1, g_nc, LAYOUT_STATEID_IOMODE_RW);
	lr_grant(g_a2, g_nc,
		 LAYOUT_STATEID_IOMODE_READ | LAYOUT_STATEID_IOMODE_RW);
	lr_grant(g_b1, g_nc, LAYOUT_STATEID_IOMODE_RW);
	lr_grant(g_a1, g_other, LAYOUT_STATEID_IOMODE_RW);
}

static void lr_teardown(void)
{
	nfs4_client_expire(g_ss, g_nc);
	nfs4_client_expire(g_ss, g_other);
	g_nc = g_other = NULL;
	synchronize_rcu();
	inode_active_put(g_a1);
	inode_active_put(g_a2);
	inode_active_put(g_b1);
	g_a1 = g_a2 = g_b1 = NULL;
	super_block_unmount(g_sb2);
	super_block_destroy(g_sb2);
	super_block_release_dirents(g_sb2);
	super_block_put(g_sb2);
	g_sb2 = NULL;
	super_block_put(g_sb1);
	g_sb1 = NULL;
	server_state_put(g_ss);
	g_ss = NULL;
	nfs4_test_teardown();
}

/* ------------------------------------------------------------------ */
/* Minimal one-op compound                                             */
/* ------------------------------------------------------------------ */

struct lr_ctx {
	struct rpc_trans rt;
	struct task task;
	struct compound c;
	COMPOUND4args args;
	COMPOUND4res res;
	nfs_argop4 argop;
	nfs_resop4 resop;
};

static struct lr_ctx *lr_ctx_new(struct inode *inode, struct nfs4_client *nc)
{
	struct lr_ctx *x = calloc(1, sizeof(*x));

	ck_assert_ptr_nonnull(x);
	atomic_store(&x->task.t_state, TASK_RUNNING);
	x->rt.rt_task = &x->task;
	x->rt.rt_fd = -1;
	x->rt.rt_compound = &x->c;
	x->c.c_rt = &x->rt;
	x->c.c_args = &x->args;
	x->c.c_res = &x->res;
	x->args.argarray.argarray_len = 1;
	x->args.argarray.argarray_val = &x->argop;
	x->res.resarray.resarray_len = 1;
	x->res.resarray.resarray_val = &x->resop;
	x->c.c_server_state = g_ss;
	x->c.c_nfs4_client = nc;
	if (inode) {
		x->c.c_inode = inode;
		inode_active_get(inode);
		x->c.c_curr_nfh.nfh_sb = inode->i_sb->sb_id;
		x->c.c_curr_nfh.nfh_ino = inode->i_ino;
	}
	return x;
}

static void lr_ctx_free(struct lr_ctx *x)
{
	inode_active_put(x->c.c_inode);
	free(x);
}

static nfsstat4 lr_layoutreturn(struct inode *inode, struct nfs4_client *nc,
				layoutreturn_type4 type, layoutiomode4 iomode)
{
	struct lr_ctx *x = lr_ctx_new(inode, nc);
	LAYOUTRETURN4args *a = &x->argop.nfs_argop4_u.oplayoutreturn;
	nfsstat4 st;

	x->argop.argop = OP_LAYOUTRETURN;
	a->lora_layout_type = LAYOUT4_FLEX_FILES;
	a->lora_iomode = iomode;
	a->lora_layoutreturn.lr_returntype = type;
	ck_assert_uint_eq(nfs4_op_layoutreturn(&x->c), 0);
	st = x->resop.nfs_resop4_u.oplayoutreturn.lorr_status;
	ck_assert(!x->resop.nfs_resop4_u.oplayoutreturn.LAYOUTRETURN4res_u
			   .lorr_stateid.lrs_present);
	lr_ctx_free(x);
	return st;
}

/* ------------------------------------------------------------------ */
/* Bulk LAYOUTRETURN                                                   */
/* ------------------------------------------------------------------ */

/*
 * LAYOUTRETURN4_FSID frees the caller's layouts on the current FH's
 * fsid only: both root-fsid files go, the second fsid's file and the
 * other client's layout stay (RFC 8881 sec 18.44.3).
 */
START_TEST(test_layoutreturn_fsid_drops_scope)
{
	ck_assert_int_eq(lr_layoutreturn(g_a2, g_nc, LAYOUTRETURN4_FSID,
					 LAYOUTIOMODE4_ANY),
			 NFS4_OK);
	ck_assert_uint_eq(lr_count(g_nc, SUPER_BLOCK_ROOT_ID, 0), 0);
	ck_assert_uint_eq(lr_count(g_nc, LR_TEST_SB2_ID, 0), 1);
	ck_assert_uint_eq(lr_count(g_other, 0, 0), 1);
}
END_TEST

/* LAYOUTRETURN4_ALL frees every layout of the caller and no one else's. */
START_TEST(test_layoutreturn_all_drops_all)
{
	ck_assert_int_eq(lr_layoutreturn(NULL, g_nc, LAYOUTRETURN4_ALL,
					 LAYOUTIOMODE4_ANY),
			 NFS4_OK);
	ck_assert_uint_eq(lr_count(g_nc, 0, 0), 0);
	ck_assert_uint_eq(lr_count(g_other, 0, 0), 1);
}
END_TEST

/*
 * A READ-mode bulk return clears only READ: the RW-only layouts are
 * untouched and the READ|RW one keeps its RW grant.
 */
START_TEST(test_layoutreturn_fsid_read_keeps_rw)
{
	ck_assert_int_eq(lr_layoutreturn(g_a1, g_nc, LAYOUTRETURN4_FSID,
					 LAYOUTIOMODE4_READ),
			 NFS4_OK);
	ck_assert_uint_eq(lr_count(g_nc, SUPER_BLOCK_ROOT_ID, 0), 2);
	ck_assert_uint_eq(lr_count(g_nc, SUPER_BLOCK_ROOT_ID,
				   LAYOUT_STATEID_IOMODE_READ),
			  0);
	ck_assert_uint_eq(lr_count(g_nc, SUPER_BLOCK_ROOT_ID,
				   LAYOUT_STATEID_IOMODE_RW),
			  2);
}
END_TEST

/* FSID needs a current filehandle to name the fsid; ALL does not. */
START_TEST(test_layoutreturn_fsid_no_fh)
{
	ck_assert_int_eq(lr_layoutreturn(NULL, g_nc, LAYOUTRETURN4_FSID,
					 LAYOUTIOMODE4_ANY),
			 NFS4ERR_NOFILEHANDLE);
	ck_assert_uint_eq(lr_count(g_nc, 0, 0), 3);
}
END_TEST

/* An empty bulk return is not an error (sec 18.44.3). */
START_TEST(test_layoutreturn_all_twice)
{
	ck_assert_int_eq(lr_layoutreturn(NULL, g_nc, LAYOUTRETURN4_ALL,
					 LAYOUTIOMODE4_ANY),
			 NFS4_OK);
	ck_assert_int_eq(lr_layoutreturn(NULL, g_nc, LAYOUTRETURN4_ALL,
					 LAYOUTIOMODE4_ANY),
			 NFS4_OK);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Recall engine (manual mode: injected clock and sender)              */
/* ------------------------------------------------------------------ */

#define T_MAX_SENT 16
#define T_MS 1000000ULL

struct t_sent {
	layouttype4 ts_layout_type;
	layoutiomode4 ts_iomode;
	int ts_changed;
	layoutrecall_type4 ts_type;
	fsid4 ts_fsid;
	struct network_file_handle ts_nfh;
	stateid4 ts_stateid;
	struct cb_pending *ts_cp;
};

static uint64_t t_now;
static struct t_sent t_sent[T_MAX_SENT];
static unsigned int t_nsent;
static struct nfs4_session *g_sess;

static uint64_t t_clock(void)
{
	return t_now;
}

static int t_send(struct nfs4_session *session, layouttype4 layout_type,
		  layoutiomode4 iomode, int changed,
		  const layoutrecall4 *recall, struct cb_pending *cp)
{
	struct t_sent *ts;

	ck_assert_uint_lt(t_nsent, T_MAX_SENT);
	ck_assert_ptr_eq(session->ns_client, g_nc);
	ts = &t_sent[t_nsent++];
	ts->ts_layout_type = layout_type;
	ts->ts_iomode = iomode;
	ts->ts_changed = changed;
	ts->ts_type = recall->lor_recalltype;
	if (ts->ts_type == LAYOUTRECALL4_FSID)
		ts->ts_fsid = recall->layoutrecall4_u.lor_fsid;
	if (ts->ts_type == LAYOUTRECALL4_FILE) {
		const layoutrecall_file4 *f =
			&recall->layoutrecall4_u.lor_layout;

		ck_assert_uint_eq(f->lor_fh.nfs_fh4_len, sizeof(ts->ts_nfh));
		memcpy(&ts->ts_nfh, f->lor_fh.nfs_fh4_val, sizeof(ts->ts_nfh));
		ts->ts_stateid = f->lor_stateid;
	}
	ts->ts_cp = cp;
	cp->cp_xid = 0x5000 + t_nsent;
	return 0;
}

/* Complete the newest send as cb_reply_handler would. */
static void t_reply(nfsstat4 op_status)
{
	struct cb_pending *cp;
	nfs_cb_resop4 *res;

	ck_assert_uint_gt(t_nsent, 0);
	cp = t_sent[t_nsent - 1].ts_cp;
	res = calloc(2, sizeof(*res));
	ck_assert_ptr_nonnull(res);
	res[0].resop = OP_CB_SEQUENCE;
	res[1].resop = OP_CB_LAYOUTRECALL;
	res[1].nfs_cb_resop4_u.opcblayoutrecall.clorr_status = op_status;
	cp->cp_res.status = op_status;
	cp->cp_res.resarray.resarray_len = 2;
	cp->cp_res.resarray.resarray_val = res;
	ck_assert(cb_pending_try_complete(cp, 0));
	cp->cp_complete(cp);
}

static void t_setup(void)
{
	channel_attrs4 channel = { .ca_maxoperations = 4, .ca_maxrequests = 1 };

	lr_setup();
	t_now = 1000 * T_MS;
	t_nsent = 0;
	memset(t_sent, 0, sizeof(t_sent));
	layout_recall_test_manual(t_clock, t_send);
	g_sess = nfs4_session_alloc(g_ss, g_nc, &channel, 0);
	ck_assert_ptr_nonnull(g_sess);
	g_sess->ns_cb_fd = 42; /* never written: t_send intercepts */
}

static void t_teardown(void)
{
	nfs4_session_put(g_sess);
	g_sess = NULL;
	lr_teardown();
}

static uint32_t t_start(layoutrecall_type4 type, uint64_t sb_id, uint64_t ino,
			uint32_t limit_ms, uint32_t limit_count)
{
	struct layout_recall_params p = {
		.lrp_clientid = (clientid4)nfs4_client_to_client(g_nc)->c_id,
		.lrp_type = type,
		.lrp_layout_type = LAYOUT4_FLEX_FILES,
		.lrp_iomode = LAYOUTIOMODE4_ANY,
		.lrp_changed = true,
		.lrp_sb_id = sb_id,
		.lrp_ino = ino,
		.lrp_interval_ms = 1000,
		.lrp_limit_ms = limit_ms,
		.lrp_limit_count = limit_count,
	};
	uint32_t id = 0;

	ck_assert_int_eq(layout_recall_start(&p, &id), 0);
	ck_assert_uint_ne(id, 0);
	return id;
}

static void t_info(uint32_t id, struct layout_recall_info *info)
{
	ck_assert_int_eq(layout_recall_status(id, info), 0);
}

static nfsstat4 t_layoutget(struct inode *inode)
{
	struct lr_ctx *x = lr_ctx_new(inode, g_nc);
	LAYOUTGET4args *a = &x->argop.nfs_argop4_u.oplayoutget;
	nfsstat4 st;

	x->argop.argop = OP_LAYOUTGET;
	a->loga_layout_type = LAYOUT4_FLEX_FILES;
	a->loga_iomode = LAYOUTIOMODE4_RW;
	a->loga_length = NFS4_UINT64_MAX;
	(void)nfs4_op_layoutget(&x->c);
	st = x->resop.nfs_resop4_u.oplayoutget.logr_status;
	xdr_free((xdrproc_t)xdr_LAYOUTGET4res,
		 (caddr_t)&x->resop.nfs_resop4_u.oplayoutget);
	lr_ctx_free(x);
	return st;
}

/*
 * The client-series scenario: an FSID recall answered DELAY, DELAY,
 * then NOMATCHING_LAYOUT.  Each resend waits the full interval, the
 * recall completes on NOMATCHING, the caller's layouts on that fsid
 * are freed, and the event log reads back the exact sequence.
 */
START_TEST(test_fsid_delay_delay_nomatching)
{
	struct layout_recall_info *info = calloc(1, sizeof(*info));
	static const uint32_t kinds[] = {
		LAYOUT_RECALL_EV_SEND, LAYOUT_RECALL_EV_REPLY,
		LAYOUT_RECALL_EV_SEND, LAYOUT_RECALL_EV_REPLY,
		LAYOUT_RECALL_EV_SEND, LAYOUT_RECALL_EV_REPLY,
		LAYOUT_RECALL_EV_DONE,
	};
	static const int32_t ops[] = {
		0,
		NFS4ERR_DELAY,
		0,
		NFS4ERR_DELAY,
		0,
		NFS4ERR_NOMATCHING_LAYOUT,
		NFS4ERR_NOMATCHING_LAYOUT,
	};
	uint32_t id =
		t_start(LAYOUTRECALL4_FSID, SUPER_BLOCK_ROOT_ID, 0, 60000, 0);

	ck_assert_ptr_nonnull(info);
	layout_recall_run();
	ck_assert_uint_eq(t_nsent, 1);
	ck_assert_int_eq(t_sent[0].ts_type, LAYOUTRECALL4_FSID);
	ck_assert_uint_eq(t_sent[0].ts_fsid.major, SUPER_BLOCK_ROOT_ID);
	ck_assert_uint_eq(t_sent[0].ts_fsid.minor, 0);
	ck_assert_int_eq(t_sent[0].ts_iomode, LAYOUTIOMODE4_ANY);
	ck_assert_int_eq(t_sent[0].ts_changed, 1);

	for (unsigned int round = 0; round < 2; round++) {
		t_reply(NFS4ERR_DELAY);
		layout_recall_run();
		t_info(id, info);
		ck_assert_int_eq(info->lri_state, LAYOUT_RECALL_WAIT_RESEND);
		/* Not before the interval. */
		t_now += 999 * T_MS;
		layout_recall_run();
		ck_assert_uint_eq(t_nsent, round + 1);
		t_now += 1 * T_MS;
		layout_recall_run();
		ck_assert_uint_eq(t_nsent, round + 2);
	}

	/* Still outstanding: in-scope LAYOUTGET conflicts. */
	ck_assert_int_eq(t_layoutget(g_a1), NFS4ERR_RECALLCONFLICT);

	t_reply(NFS4ERR_NOMATCHING_LAYOUT);
	layout_recall_run();
	t_info(id, info);
	ck_assert_int_eq(info->lri_state, LAYOUT_RECALL_DONE);
	ck_assert_uint_eq(info->lri_sends, 3);
	ck_assert_uint_eq(info->lri_nevents, sizeof(kinds) / sizeof(kinds[0]));
	for (unsigned int i = 0; i < info->lri_nevents; i++) {
		ck_assert_uint_eq(info->lri_events[i].lre_kind, kinds[i]);
		ck_assert_int_eq(info->lri_events[i].lre_op_status, ops[i]);
		/* DONE belongs to the third send. */
		ck_assert_uint_eq(info->lri_events[i].lre_send,
				  i == 6 ? 3 : i / 2 + 1);
		ck_assert_uint_ne(info->lri_events[i].lre_time_ns, 0);
	}
	ck_assert_uint_eq(info->lri_events[6].lre_dropped, 2);

	ck_assert_uint_eq(lr_count(g_nc, SUPER_BLOCK_ROOT_ID, 0), 0);
	ck_assert_uint_eq(lr_count(g_nc, LR_TEST_SB2_ID, 0), 1);
	ck_assert_uint_eq(lr_count(g_other, 0, 0), 1);
	/* Nothing more is sent once done. */
	t_now += 10000 * T_MS;
	layout_recall_run();
	ck_assert_uint_eq(t_nsent, 3);
	ck_assert_int_ne(t_layoutget(g_a1), NFS4ERR_RECALLCONFLICT);
	free(info);
}
END_TEST

/* ALL is encoded as LAYOUTRECALL4_ALL; NOMATCHING frees every fsid. */
START_TEST(test_all_nomatching_drops_all)
{
	struct layout_recall_info info;
	uint32_t id = t_start(LAYOUTRECALL4_ALL, 0, 0, 0, 0);

	layout_recall_run();
	ck_assert_uint_eq(t_nsent, 1);
	ck_assert_int_eq(t_sent[0].ts_type, LAYOUTRECALL4_ALL);
	ck_assert_int_eq(t_layoutget(g_b1), NFS4ERR_RECALLCONFLICT);
	t_reply(NFS4ERR_NOMATCHING_LAYOUT);
	layout_recall_run();
	t_info(id, &info);
	ck_assert_int_eq(info.lri_state, LAYOUT_RECALL_DONE);
	ck_assert_uint_eq(lr_count(g_nc, 0, 0), 0);
	ck_assert_uint_eq(lr_count(g_other, 0, 0), 1);
}
END_TEST

/*
 * FILE recall carries the client's layout stateid with seqid bumped
 * once (sec 12.5.3), the same on resend; OK then a FILE LAYOUTRETURN
 * completes it.
 */
START_TEST(test_file_delay_ok_then_return)
{
	struct layout_recall_info info;
	uint32_t id = t_start(LAYOUTRECALL4_FILE, SUPER_BLOCK_ROOT_ID,
			      g_a1->i_ino, 0, 0);

	layout_recall_run();
	ck_assert_uint_eq(t_nsent, 1);
	ck_assert_int_eq(t_sent[0].ts_type, LAYOUTRECALL4_FILE);
	ck_assert_uint_eq(t_sent[0].ts_nfh.nfh_ino, g_a1->i_ino);
	ck_assert_uint_eq(t_sent[0].ts_nfh.nfh_sb, SUPER_BLOCK_ROOT_ID);
	ck_assert_uint_eq(t_sent[0].ts_stateid.seqid, 1);

	/* FILE recalls only conflict on their own file. */
	ck_assert_int_eq(t_layoutget(g_a1), NFS4ERR_RECALLCONFLICT);
	ck_assert_int_ne(t_layoutget(g_a2), NFS4ERR_RECALLCONFLICT);

	t_reply(NFS4ERR_DELAY);
	layout_recall_run();
	t_now += 1000 * T_MS;
	layout_recall_run();
	ck_assert_uint_eq(t_nsent, 2);
	ck_assert(!memcmp(&t_sent[0].ts_stateid, &t_sent[1].ts_stateid,
			  sizeof(stateid4)));

	t_reply(NFS4_OK);
	layout_recall_run();
	t_info(id, &info);
	ck_assert_int_eq(info.lri_state, LAYOUT_RECALL_ACKED);

	struct lr_ctx *x = lr_ctx_new(g_a1, g_nc);
	LAYOUTRETURN4args *a = &x->argop.nfs_argop4_u.oplayoutreturn;
	uint32_t body[2] = { 0, 0 };

	x->argop.argop = OP_LAYOUTRETURN;
	a->lora_layout_type = LAYOUT4_FLEX_FILES;
	a->lora_iomode = LAYOUTIOMODE4_ANY;
	a->lora_layoutreturn.lr_returntype = LAYOUTRETURN4_FILE;
	a->lora_layoutreturn.layoutreturn4_u.lr_layout.lrf_length =
		NFS4_UINT64_MAX;
	a->lora_layoutreturn.layoutreturn4_u.lr_layout.lrf_stateid =
		t_sent[0].ts_stateid;
	a->lora_layoutreturn.layoutreturn4_u.lr_layout.lrf_body.lrf_body_len =
		sizeof(body);
	a->lora_layoutreturn.layoutreturn4_u.lr_layout.lrf_body.lrf_body_val =
		(char *)body;
	ck_assert_uint_eq(nfs4_op_layoutreturn(&x->c), 0);
	ck_assert_int_eq(x->resop.nfs_resop4_u.oplayoutreturn.lorr_status,
			 NFS4_OK);
	lr_ctx_free(x);

	t_info(id, &info);
	ck_assert_int_eq(info.lri_state, LAYOUT_RECALL_DONE);
	ck_assert_uint_eq(info.lri_events[info.lri_nevents - 2].lre_kind,
			  LAYOUT_RECALL_EV_RETURN);
}
END_TEST

/* DELAY forever with a time limit: EXPIRED, nothing dropped. */
START_TEST(test_limit_time_expires)
{
	struct layout_recall_info info;
	uint32_t id =
		t_start(LAYOUTRECALL4_FSID, SUPER_BLOCK_ROOT_ID, 0, 2500, 0);

	for (unsigned int i = 0; i < 10; i++) {
		layout_recall_run(); /* sends when due */
		t_reply(NFS4ERR_DELAY);
		layout_recall_run(); /* the driver handles replies at once */
		t_info(id, &info);
		if (info.lri_state != LAYOUT_RECALL_WAIT_RESEND)
			break;
		t_now += 1000 * T_MS;
	}
	t_info(id, &info);
	ck_assert_int_eq(info.lri_state, LAYOUT_RECALL_EXPIRED);
	/* Sends at 0s, 1s, 2s; a 3s resend would pass the 2.5s limit. */
	ck_assert_uint_eq(info.lri_sends, 3);
	ck_assert_uint_eq(info.lri_events[info.lri_nevents - 1].lre_kind,
			  LAYOUT_RECALL_EV_EXPIRED);
	ck_assert_uint_eq(lr_count(g_nc, SUPER_BLOCK_ROOT_ID, 0), 2);
	ck_assert_int_ne(t_layoutget(g_a1), NFS4ERR_RECALLCONFLICT);
}
END_TEST

/* Count limit: EXPIRED after exactly limit_count sends. */
START_TEST(test_limit_count_expires)
{
	struct layout_recall_info info;
	uint32_t id = t_start(LAYOUTRECALL4_ALL, 0, 0, 0, 2);

	layout_recall_run();
	t_reply(NFS4ERR_DELAY);
	layout_recall_run();
	t_now += 1000 * T_MS;
	layout_recall_run();
	ck_assert_uint_eq(t_nsent, 2);
	t_reply(NFS4ERR_DELAY);
	layout_recall_run();
	t_now += 5000 * T_MS;
	layout_recall_run();
	t_info(id, &info);
	ck_assert_int_eq(info.lri_state, LAYOUT_RECALL_EXPIRED);
	ck_assert_uint_eq(t_nsent, 2);
}
END_TEST

/*
 * sec 12.5.5.2.1.5: during an FSID recall a FILE return in scope and a
 * return of a different type are RECALLCONFLICT; the matching FSID
 * return completes the recall.  A second recall to the same client is
 * refused while one is outstanding.
 */
START_TEST(test_bulk_layoutreturn_rules)
{
	struct layout_recall_info info;
	struct layout_recall_params p = {
		.lrp_clientid = (clientid4)nfs4_client_to_client(g_nc)->c_id,
		.lrp_type = LAYOUTRECALL4_ALL,
		.lrp_iomode = LAYOUTIOMODE4_ANY,
	};
	uint32_t id = t_start(LAYOUTRECALL4_FSID, SUPER_BLOCK_ROOT_ID, 0, 0, 0);
	uint32_t id2;
	clientid4 clid = p.lrp_clientid;

	ck_assert_int_eq(layout_recall_start(&p, &id2), -EBUSY);
	layout_recall_run();
	t_reply(NFS4_OK);
	layout_recall_run();

	ck_assert_int_eq(layout_recall_layoutreturn_check(
				 clid, LAYOUTRETURN4_FILE, g_a1),
			 NFS4ERR_RECALLCONFLICT);
	ck_assert_int_eq(layout_recall_layoutreturn_check(
				 clid, LAYOUTRETURN4_FILE, g_b1),
			 NFS4_OK);
	ck_assert_int_eq(lr_layoutreturn(NULL, g_nc, LAYOUTRETURN4_ALL,
					 LAYOUTIOMODE4_ANY),
			 NFS4ERR_RECALLCONFLICT);
	ck_assert_uint_eq(lr_count(g_nc, 0, 0), 3);

	ck_assert_int_eq(lr_layoutreturn(g_a1, g_nc, LAYOUTRETURN4_FSID,
					 LAYOUTIOMODE4_ANY),
			 NFS4_OK);
	t_info(id, &info);
	ck_assert_int_eq(info.lri_state, LAYOUT_RECALL_DONE);
	ck_assert_uint_eq(lr_count(g_nc, SUPER_BLOCK_ROOT_ID, 0), 0);
	ck_assert_int_eq(layout_recall_start(&p, &id2), 0);
}
END_TEST

/* Cancel stops resends and clears the conflict. */
START_TEST(test_cancel_stops_resend)
{
	struct layout_recall_info info;
	uint32_t id = t_start(LAYOUTRECALL4_FSID, SUPER_BLOCK_ROOT_ID, 0, 0, 0);

	layout_recall_run();
	t_reply(NFS4ERR_DELAY);
	layout_recall_run();
	ck_assert_int_eq(layout_recall_cancel(id), 0);
	t_now += 5000 * T_MS;
	layout_recall_run();
	ck_assert_uint_eq(t_nsent, 1);
	t_info(0, &info);
	ck_assert_uint_eq(info.lri_id, id);
	ck_assert_int_eq(info.lri_state, LAYOUT_RECALL_CANCELLED);
	ck_assert_int_ne(t_layoutget(g_a1), NFS4ERR_RECALLCONFLICT);
}
END_TEST

/* A reply to a send that is cancelled while in flight is just freed. */
START_TEST(test_cancel_in_flight_reply_ignored)
{
	struct layout_recall_info info;
	uint32_t id = t_start(LAYOUTRECALL4_ALL, 0, 0, 0, 0);

	layout_recall_run();
	ck_assert_int_eq(layout_recall_cancel(id), 0);
	t_reply(NFS4ERR_NOMATCHING_LAYOUT);
	layout_recall_run();
	t_info(id, &info);
	ck_assert_int_eq(info.lri_state, LAYOUT_RECALL_CANCELLED);
	ck_assert_uint_eq(lr_count(g_nc, 0, 0), 3);
}
END_TEST

/* Selection by address, and the error cases of start(). */
START_TEST(test_start_selection_errors)
{
	struct layout_recall_params p = {
		.lrp_addr = htonl(0x7f000001),
		.lrp_type = LAYOUTRECALL4_ALL,
		.lrp_iomode = LAYOUTIOMODE4_ANY,
	};
	struct layout_recall_info info;
	uint32_t id;

	/* g_other has no callback session, so the address picks g_nc. */
	ck_assert_int_eq(layout_recall_start(&p, &id), 0);
	t_info(id, &info);
	ck_assert_uint_eq(info.lri_params.lrp_clientid,
			  nfs4_client_to_client(g_nc)->c_id);
	ck_assert_int_eq(layout_recall_cancel(id), 0);

	p.lrp_addr = htonl(0x7f000009);
	ck_assert_int_eq(layout_recall_start(&p, &id), -ENOENT);
	p.lrp_addr = 0;
	p.lrp_type = LAYOUTRECALL4_FSID;
	p.lrp_sb_id = 9999;
	ck_assert_int_eq(layout_recall_start(&p, &id), -ENOENT);
	p.lrp_type = LAYOUTRECALL4_FILE;
	p.lrp_sb_id = SUPER_BLOCK_ROOT_ID;
	p.lrp_ino = 999999;
	ck_assert_int_eq(layout_recall_start(&p, &id), -ENOENT);
	p.lrp_type = 7;
	ck_assert_int_eq(layout_recall_start(&p, &id), -EINVAL);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Through the real CB send / timeout path                             */
/* ------------------------------------------------------------------ */

static bool t_expire_in_submit;

/* Stands in for the wire; the rpc_trans stays in the pending table. */
static int t_capture_submit(struct rpc_trans *out)
{
	struct cb_pending *cp = out->rt_context;

	ck_assert_uint_lt(t_nsent, T_MAX_SENT);
	t_sent[t_nsent++].ts_cp = cp;
	if (t_expire_in_submit)
		ck_assert(cb_timeout_expire(cp));
	return 0;
}

static int t_send_real(struct nfs4_session *session, layouttype4 layout_type,
		       layoutiomode4 iomode, int changed,
		       const layoutrecall4 *recall, struct cb_pending *cp)
{
	cp->cp_submit = t_capture_submit;
	return nfs4_cb_layoutrecall_send_recall(session, layout_type, iomode,
						changed, recall, cp);
}

/*
 * A reply that never comes: cb_timeout expires the callback, the
 * completion hook fires from the timeout path, and the recall fails
 * with a TIMEOUT event rather than resending.
 */
START_TEST(test_real_timeout_fails)
{
	struct layout_recall_info info;
	uint32_t id;

	layout_recall_test_manual(t_clock, t_send_real);
	id = t_start(LAYOUTRECALL4_ALL, 0, 0, 0, 0);
	layout_recall_run();
	ck_assert_uint_eq(t_nsent, 1);
	ck_assert(cb_timeout_expire(t_sent[0].ts_cp));
	layout_recall_run();
	t_info(id, &info);
	ck_assert_int_eq(info.lri_state, LAYOUT_RECALL_FAILED);
	ck_assert_uint_eq(info.lri_nevents, 3);
	ck_assert_uint_eq(info.lri_events[1].lre_kind,
			  LAYOUT_RECALL_EV_TIMEOUT);
	ck_assert_int_eq(info.lri_events[1].lre_cb_status, -ETIMEDOUT);
	ck_assert_uint_eq(lr_count(g_nc, 0, 0), 3);
}
END_TEST

/*
 * Completion that lands while the send is still submitting is
 * deferred to cb_submit_done() and must still reach the hook.
 */
START_TEST(test_real_timeout_during_submit)
{
	struct layout_recall_info info;
	uint32_t id;

	t_expire_in_submit = true;
	layout_recall_test_manual(t_clock, t_send_real);
	id = t_start(LAYOUTRECALL4_ALL, 0, 0, 0, 0);
	layout_recall_run();
	t_expire_in_submit = false;
	layout_recall_run();
	t_info(id, &info);
	ck_assert_int_eq(info.lri_state, LAYOUT_RECALL_FAILED);
}
END_TEST

/*
 * Shut down with a real callback in flight: fini reclaims it from the
 * pending table (ASAN/LSAN check the refs) without deadlocking against
 * the cb_timeout thread.
 */
START_TEST(test_real_fini_in_flight)
{
	layout_recall_test_manual(t_clock, t_send_real);
	(void)t_start(LAYOUTRECALL4_ALL, 0, 0, 0, 0);
	layout_recall_run();
	ck_assert_uint_eq(t_nsent, 1);
}
END_TEST

/* A READ-only return does not complete an RW recall (iomode cover). */
START_TEST(test_return_iomode_must_cover)
{
	struct layout_recall_info info;
	struct layout_recall_params p = {
		.lrp_clientid = (clientid4)nfs4_client_to_client(g_nc)->c_id,
		.lrp_type = LAYOUTRECALL4_FSID,
		.lrp_iomode = LAYOUTIOMODE4_RW,
		.lrp_sb_id = SUPER_BLOCK_ROOT_ID,
	};
	uint32_t id;

	ck_assert_int_eq(layout_recall_start(&p, &id), 0);
	layout_recall_run();
	t_reply(NFS4_OK);
	layout_recall_run();
	ck_assert_int_eq(lr_layoutreturn(g_a1, g_nc, LAYOUTRETURN4_FSID,
					 LAYOUTIOMODE4_READ),
			 NFS4_OK);
	t_info(id, &info);
	ck_assert_int_eq(info.lri_state, LAYOUT_RECALL_ACKED);
	ck_assert_int_eq(lr_layoutreturn(g_a1, g_nc, LAYOUTRETURN4_FSID,
					 LAYOUTIOMODE4_RW),
			 NFS4_OK);
	t_info(id, &info);
	ck_assert_int_eq(info.lri_state, LAYOUT_RECALL_DONE);
}
END_TEST

/*
 * FILE NOMATCHING after a newer grant (seqid moved past the recall's)
 * leaves that layout alone (sec 12.5.5.2.1.3).
 */
START_TEST(test_file_nomatching_newer_grant_kept)
{
	struct layout_recall_info info;
	struct client *c = nfs4_client_to_client(g_nc);
	uint32_t id = t_start(LAYOUTRECALL4_FILE, SUPER_BLOCK_ROOT_ID,
			      g_a1->i_ino, 0, 0);
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;

	layout_recall_run();
	/* Model a LAYOUTGET reply racing the recall. */
	rcu_read_lock();
	cds_lfht_for_each(g_a1->i_stateids, &iter, node)
	{
		struct stateid *stid =
			caa_container_of(node, struct stateid, s_inode_node);

		if (stid->s_tag == Layout_Stateid && stid->s_client == c)
			__atomic_add_fetch(&stid->s_seqid, 1, __ATOMIC_RELAXED);
	}
	rcu_read_unlock();
	t_reply(NFS4ERR_NOMATCHING_LAYOUT);
	layout_recall_run();
	t_info(id, &info);
	ck_assert_int_eq(info.lri_state, LAYOUT_RECALL_DONE);
	ck_assert_uint_eq(info.lri_events[info.lri_nevents - 1].lre_dropped, 0);
	ck_assert_uint_eq(lr_count(g_nc, SUPER_BLOCK_ROOT_ID, 0), 2);
}
END_TEST

static Suite *layout_recall_suite(void)
{
	Suite *s = suite_create("layout_recall");
	TCase *tc = tcase_create("bulk LAYOUTRETURN");

	tcase_add_checked_fixture(tc, lr_setup, lr_teardown);
	tcase_add_test(tc, test_layoutreturn_fsid_drops_scope);
	tcase_add_test(tc, test_layoutreturn_all_drops_all);
	tcase_add_test(tc, test_layoutreturn_fsid_read_keeps_rw);
	tcase_add_test(tc, test_layoutreturn_fsid_no_fh);
	tcase_add_test(tc, test_layoutreturn_all_twice);
	suite_add_tcase(s, tc);

	TCase *te = tcase_create("recall engine");

	tcase_add_checked_fixture(te, t_setup, t_teardown);
	tcase_add_test(te, test_fsid_delay_delay_nomatching);
	tcase_add_test(te, test_all_nomatching_drops_all);
	tcase_add_test(te, test_file_delay_ok_then_return);
	tcase_add_test(te, test_limit_time_expires);
	tcase_add_test(te, test_limit_count_expires);
	tcase_add_test(te, test_bulk_layoutreturn_rules);
	tcase_add_test(te, test_cancel_stops_resend);
	tcase_add_test(te, test_cancel_in_flight_reply_ignored);
	tcase_add_test(te, test_start_selection_errors);
	tcase_add_test(te, test_real_timeout_fails);
	tcase_add_test(te, test_real_timeout_during_submit);
	tcase_add_test(te, test_real_fini_in_flight);
	tcase_add_test(te, test_return_iomode_must_cover);
	tcase_add_test(te, test_file_nomatching_newer_grant_kept);
	suite_add_tcase(s, te);
	return s;
}

int main(void)
{
	return nfs4_test_run(layout_recall_suite());
}
