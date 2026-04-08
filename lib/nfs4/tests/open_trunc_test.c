/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * open_trunc_test.c -- OPEN handler O_RDONLY|O_TRUNC permission tests.
 *
 * pjdfstest open/07.t: open(O_RDONLY|O_TRUNC) on a file not writable
 * by the calling process must return EACCES.  reffs was returning 0
 * (truncation succeeds) because the OPEN handler only checked W_OK
 * when share_access included WRITE, and silently discarded createattrs
 * for existing files in the UNCHECKED4 path.
 *
 * The Linux NFSv4 client maps O_RDONLY|O_TRUNC as:
 *   OPEN(CLAIM_NULL, OPEN4_CREATE, UNCHECKED4,
 *        createattrs{FATTR4_SIZE=0}, share_access=READ)
 *
 * Fix: when FATTR4_SIZE is present in createattrs for an existing file,
 * the server must check W_OK on the file regardless of share_access.
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

#include <check.h>
#include <urcu.h>

#include "nfsv42_xdr.h"
#include "reffs/filehandle.h"
#include "reffs/inode.h"
#include "reffs/rcu.h"
#include "reffs/server.h"
#include "reffs/stateid.h"
#include "reffs/super_block.h"
#include "reffs/vfs.h"
#include "nfs4/client.h"
#include "nfs4/compound.h"
#include "nfs4/ops.h"
#include "nfs4/attr.h"
#include "nfs4/stateid.h"

#include "nfs4_test_harness.h"

/* ------------------------------------------------------------------ */
/* Minimal compound mock (mirrors cm_ctx in trust_stateid_test.c)      */
/* ------------------------------------------------------------------ */

struct cm_ctx {
	struct rpc_trans rt;
	struct task task;
	struct compound *compound;
	struct nfs4_client *nc;
};

static struct cm_ctx *cm_alloc(unsigned int nops, uint32_t uid)
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

	/* Synthetic client with AUTH_SYS credential for the given uid. */
	verifier4 v;
	struct sockaddr_in sin;

	memset(&v, 0x11, sizeof(v));
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(0x7f000002);
	sin.sin_port = htons(2049);

	cm->nc = nfs4_client_alloc(&v, &sin, 1, 0xDEAD0001, 0);
	ck_assert_ptr_nonnull(cm->nc);
	c->c_nfs4_client = nfs4_client_get(cm->nc);

	/* Set the NFS caller credential (synthetic AUTH_SYS). */
	c->c_ap.aup_uid = uid;
	c->c_ap.aup_gid = uid;
	c->c_ap.aup_len = 0;
	c->c_ap.aup_gids = NULL;

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
/* Test fixture                                                         */
/* ------------------------------------------------------------------ */

static struct super_block *g_sb;
static struct inode *g_dir;

/*
 * File names used across tests.  Kept as static char[] so
 * component4 fields can point directly without strdup.
 */
static char g_name_denied[] = "trunc_denied"; /* uid=1000, mode=0444 */
static char g_name_allowed[] = "trunc_allowed"; /* uid=1000, mode=0644 */

/* XDR-encoded FATTR4_SIZE=0: uint64_t in network byte order. */
static char g_size_zero_xdr[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

static void setup(void)
{
	struct authunix_parms ap;
	struct inode *file;
	int ret;

	nfs4_test_setup();

	g_sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	ck_assert_ptr_nonnull(g_sb);

	g_dir = inode_find(g_sb, INODE_ROOT_ID);
	ck_assert_ptr_nonnull(g_dir);

	/*
	 * Create test files owned by uid=1000.  Root dir has mode 0777,
	 * so uid=1000 can write to it.
	 */
	ap.aup_uid = 1000;
	ap.aup_gid = 1000;
	ap.aup_len = 0;
	ap.aup_gids = NULL;

	/* trunc_denied: mode 0444 -- no write permission for uid=2000 */
	ret = vfs_create(g_dir, g_name_denied, 0444, &ap, &file, NULL, NULL);
	ck_assert_int_eq(ret, 0);
	ck_assert_ptr_nonnull(file);
	inode_active_put(file);

	/* trunc_allowed: mode 0644 -- owner (uid=1000) can write */
	ret = vfs_create(g_dir, g_name_allowed, 0644, &ap, &file, NULL, NULL);
	ck_assert_int_eq(ret, 0);
	ck_assert_ptr_nonnull(file);
	inode_active_put(file);
}

static void teardown(void)
{
	struct authunix_parms ap;

	ap.aup_uid = 1000;
	ap.aup_gid = 1000;
	ap.aup_len = 0;
	ap.aup_gids = NULL;

	vfs_remove(g_dir, g_name_denied, &ap, NULL, NULL);
	vfs_remove(g_dir, g_name_allowed, &ap, NULL, NULL);

	inode_active_put(g_dir);
	g_dir = NULL;
	super_block_put(g_sb);
	g_sb = NULL;
	nfs4_test_teardown();
}

/* ------------------------------------------------------------------ */
/* Helper: build OPEN(CLAIM_NULL, UNCHECKED4, FATTR4_SIZE=0, READ)     */
/* ------------------------------------------------------------------ */

/*
 * fill_open_trunc_args -- fill OPEN4args with UNCHECKED4 + FATTR4_SIZE=0.
 *
 * Caller must call bitmap4_destroy on the createattrs attrmask after use:
 *   bitmap4_destroy(&args->openhow.openflag4_u.how.createhow4_u.createattrs.attrmask)
 */
static void fill_open_trunc_args(OPEN4args *args, char *filename,
				 size_t namelen)
{
	int ret;

	args->share_access = OPEN4_SHARE_ACCESS_READ;
	args->share_deny = OPEN4_SHARE_DENY_NONE;

	args->openhow.opentype = OPEN4_CREATE;
	args->openhow.openflag4_u.how.mode = UNCHECKED4;

	fattr4 *fa = &args->openhow.openflag4_u.how.createhow4_u.createattrs;

	ret = bitmap4_init(&fa->attrmask, FATTR4_SIZE);
	ck_assert_int_eq(ret, 0);
	bitmap4_attribute_set(&fa->attrmask, FATTR4_SIZE);

	/* XDR-encoded SIZE=0: 8 zero bytes (uint64_t big-endian). */
	fa->attr_vals.attrlist4_val = g_size_zero_xdr;
	fa->attr_vals.attrlist4_len = 8;

	args->claim.claim = CLAIM_NULL;
	args->claim.open_claim4_u.file.utf8string_val = filename;
	args->claim.open_claim4_u.file.utf8string_len = (u_int)namelen;
}

/* ------------------------------------------------------------------ */
/* 1. O_RDONLY|O_TRUNC on a file not writable by caller: EACCES        */
/* ------------------------------------------------------------------ */

/*
 * Caller uid=2000 tries to open+trunc a file owned by uid=1000 with
 * mode 0444.  No write permission: must return NFS4ERR_ACCESS.
 *
 * Before the fix: returned NFS4_OK (truncation succeeded silently).
 * After the fix: returns NFS4ERR_ACCESS.
 */
START_TEST(test_open_rdonly_trunc_no_write_perm)
{
	struct cm_ctx *cm = cm_alloc(1, 2000 /* caller uid */);

	cm_set_inode(cm, g_dir);
	cm_set_op(cm, 0, OP_OPEN);

	OPEN4args *args = &cm->compound->c_args->argarray.argarray_val[0]
				   .nfs_argop4_u.opopen;
	fill_open_trunc_args(args, g_name_denied, strlen(g_name_denied));

	nfs4_op_open(cm->compound);

	OPEN4res *res = &cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opopen;
	ck_assert_int_eq(res->status, NFS4ERR_ACCESS);

	bitmap4_destroy(&args->openhow.openflag4_u.how.createhow4_u.createattrs
				 .attrmask);
	cm_free(cm);
}
END_TEST

/* ------------------------------------------------------------------ */
/* 2. O_RDONLY|O_TRUNC by the file owner with write perm: NFS4_OK      */
/* ------------------------------------------------------------------ */

/*
 * Caller uid=1000 (owner) opens+truncs a file with mode 0644.
 * Write permission check must succeed and the open must complete.
 * Issue CLOSE after OPEN to release the share reservation and avoid
 * LSAN false positives from the reffs_share inode ref.
 */
START_TEST(test_open_rdonly_trunc_owner_allowed)
{
	struct cm_ctx *cm = cm_alloc(2, 1000 /* caller uid = file owner */);

	cm_set_inode(cm, g_dir);
	cm_set_op(cm, 0, OP_OPEN);

	OPEN4args *args = &cm->compound->c_args->argarray.argarray_val[0]
				   .nfs_argop4_u.opopen;
	fill_open_trunc_args(args, g_name_allowed, strlen(g_name_allowed));

	nfs4_op_open(cm->compound);

	OPEN4res *res = &cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opopen;
	ck_assert_int_eq(res->status, NFS4_OK);

	bitmap4_destroy(&res->OPEN4res_u.resok4.attrset);
	bitmap4_destroy(&args->openhow.openflag4_u.how.createhow4_u.createattrs
				 .attrmask);

	/*
	 * CLOSE the file to release the share reservation.  Use the
	 * current-stateid encoding (seqid=1, other=zeros) so CLOSE
	 * picks up the open stateid left in compound->c_curr_stid by OPEN.
	 */
	cm_set_op(cm, 1, OP_CLOSE);
	CLOSE4args *close_args = &cm->compound->c_args->argarray.argarray_val[1]
					  .nfs_argop4_u.opclose;
	close_args->open_stateid.seqid = 1;
	memset(close_args->open_stateid.other, 0,
	       sizeof(close_args->open_stateid.other));
	nfs4_op_close(cm->compound);

	CLOSE4res *close_res = &cm->compound->c_res->resarray.resarray_val[1]
					.nfs_resop4_u.opclose;
	ck_assert_int_eq(close_res->status, NFS4_OK);

	cm_free(cm);
}
END_TEST

/* ------------------------------------------------------------------ */
/* 3. O_RDONLY without truncation on a read-only file: NFS4_OK         */
/* ------------------------------------------------------------------ */

/*
 * Caller uid=2000 opens a mode=0444 file for READ with UNCHECKED4 but
 * no FATTR4_SIZE in createattrs (no truncation requested).  The file
 * is readable (mode 0444 grants r-- to others), so the open must succeed.
 *
 * Guards against a regression where the fix adds W_OK even when
 * FATTR4_SIZE is absent.
 */
START_TEST(test_open_rdonly_no_trunc_allowed)
{
	struct cm_ctx *cm = cm_alloc(2, 2000 /* caller uid */);

	cm_set_inode(cm, g_dir);
	cm_set_op(cm, 0, OP_OPEN);

	OPEN4args *args = &cm->compound->c_args->argarray.argarray_val[0]
				   .nfs_argop4_u.opopen;

	/*
	 * UNCHECKED4 with no createattrs (attrmask stays zero-initialized
	 * from calloc: bitmap4_len = 0).  No truncation.
	 */
	args->share_access = OPEN4_SHARE_ACCESS_READ;
	args->share_deny = OPEN4_SHARE_DENY_NONE;
	args->openhow.opentype = OPEN4_CREATE;
	args->openhow.openflag4_u.how.mode = UNCHECKED4;

	args->claim.claim = CLAIM_NULL;
	args->claim.open_claim4_u.file.utf8string_val = g_name_denied;
	args->claim.open_claim4_u.file.utf8string_len =
		(u_int)strlen(g_name_denied);

	nfs4_op_open(cm->compound);

	OPEN4res *res = &cm->compound->c_res->resarray.resarray_val[0]
				 .nfs_resop4_u.opopen;
	ck_assert_int_eq(res->status, NFS4_OK);

	/*
	 * CLOSE to release the share reservation; same current-stateid
	 * encoding as test_open_rdonly_trunc_owner_allowed.
	 */
	cm_set_op(cm, 1, OP_CLOSE);
	CLOSE4args *close_args = &cm->compound->c_args->argarray.argarray_val[1]
					  .nfs_argop4_u.opclose;
	close_args->open_stateid.seqid = 1;
	memset(close_args->open_stateid.other, 0,
	       sizeof(close_args->open_stateid.other));
	nfs4_op_close(cm->compound);

	CLOSE4res *close_res = &cm->compound->c_res->resarray.resarray_val[1]
					.nfs_resop4_u.opclose;
	ck_assert_int_eq(close_res->status, NFS4_OK);

	cm_free(cm);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

Suite *open_trunc_suite(void)
{
	Suite *s = suite_create("nfs4: OPEN O_RDONLY|O_TRUNC permission");
	TCase *tc = tcase_create("Core");

	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_open_rdonly_trunc_no_write_perm);
	tcase_add_test(tc, test_open_rdonly_trunc_owner_allowed);
	tcase_add_test(tc, test_open_rdonly_no_trunc_allowed);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	return nfs4_test_run(open_trunc_suite());
}
