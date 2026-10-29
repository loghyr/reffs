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
#include "reffs/fs.h"
#include "reffs/inode.h"
#include "reffs/rpc.h"
#include "reffs/server.h"
#include "reffs/stateid.h"
#include "reffs/super_block.h"
#include "reffs/task.h"
#include "nfs4/client.h"
#include "nfs4/client_persist.h"
#include "nfs4/compound.h"
#include "nfs4/ops.h"
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
	return s;
}

int main(void)
{
	return nfs4_test_run(layout_recall_suite());
}
