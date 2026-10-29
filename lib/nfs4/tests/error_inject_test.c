/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One-shot error injection: mock COMPOUNDs through dispatch_compound().
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <rpc/auth.h>
#include <rpc/xdr.h>

#include <check.h>

#include "nfsv42_xdr.h"
#include "reffs/filehandle.h"
#include "reffs/fs.h"
#include "reffs/inode.h"
#include "reffs/rpc.h"
#include "reffs/server.h"
#include "reffs/super_block.h"
#include "reffs/task.h"
#include "nfs4/client.h"
#include "nfs4/compound.h"
#include "nfs4/error_inject.h"
#include "nfs4/ops.h"
#include "nfs4_test_harness.h"

#define EI_PATH "/ei_file"
#define EI_OTHER "/ei_other"
#define EI_ADDR 0x7f000005

static uint64_t ei_ino;
static uint64_t ei_other_ino;

static uint64_t ei_path_ino(const char *path)
{
	struct stat st;

	ck_assert_int_eq(reffs_fs_create(path, 0644), 0);
	ck_assert_int_eq(reffs_fs_getattr(path, &st), 0);
	return st.st_ino;
}

static void ei_setup(void)
{
	nfs4_test_setup();
	ei_ino = ei_path_ino(EI_PATH);
	ei_other_ino = ei_path_ino(EI_OTHER);
	error_inject_disarm();
}

static void ei_teardown(void)
{
	error_inject_disarm();
	reffs_fs_unlink(EI_PATH);
	reffs_fs_unlink(EI_OTHER);
	nfs4_test_teardown();
}

struct ei_ctx {
	struct rpc_trans rt;
	struct task task;
	struct compound *compound;
	struct nfs4_client *nc;
};

static clientid4 ei_next_clientid(void)
{
	static clientid4 next = 0xE1A50100;

	return next++;
}

static struct ei_ctx *ei_ctx_new(unsigned int nops, bool with_client)
{
	struct ei_ctx *ctx = calloc(1, sizeof(*ctx));
	struct compound *c = calloc(1, sizeof(*c));

	ck_assert_ptr_nonnull(ctx);
	ck_assert_ptr_nonnull(c);
	atomic_store_explicit(&ctx->task.t_state, TASK_RUNNING,
			      memory_order_relaxed);
	ctx->rt.rt_task = &ctx->task;
	ctx->rt.rt_fd = -1;
	ctx->rt.rt_info.ri_xid = 0x1234abcd;
	ctx->rt.rt_info.ri_cred.rc_flavor = AUTH_SYS;

	c->c_rt = &ctx->rt;
	c->c_args = calloc(1, sizeof(COMPOUND4args));
	c->c_res = calloc(1, sizeof(COMPOUND4res));
	ck_assert_ptr_nonnull(c->c_args);
	ck_assert_ptr_nonnull(c->c_res);
	c->c_args->argarray.argarray_len = nops;
	c->c_args->argarray.argarray_val = calloc(nops, sizeof(nfs_argop4));
	c->c_res->resarray.resarray_len = nops;
	c->c_res->resarray.resarray_val = calloc(nops, sizeof(nfs_resop4));
	ck_assert_ptr_nonnull(c->c_args->argarray.argarray_val);
	ck_assert_ptr_nonnull(c->c_res->resarray.resarray_val);
	c->c_server_state = server_state_find();
	ck_assert_ptr_nonnull(c->c_server_state);
	ctx->rt.rt_compound = c;
	ctx->compound = c;

	if (with_client) {
		verifier4 v;
		struct sockaddr_in sin;

		memset(&v, 0x5a, sizeof(v));
		memset(&sin, 0, sizeof(sin));
		sin.sin_family = AF_INET;
		sin.sin_addr.s_addr = htonl(EI_ADDR);
		sin.sin_port = htons(2049);
		ctx->nc = nfs4_client_alloc(&v, &sin, 1, ei_next_clientid(), 0);
		ck_assert_ptr_nonnull(ctx->nc);
		c->c_nfs4_client = nfs4_client_get(ctx->nc);
	}
	return ctx;
}

static clientid4 ei_ctx_clientid(struct ei_ctx *ctx)
{
	return (clientid4)nfs4_client_to_client(ctx->nc)->c_id;
}

static void ei_ctx_free(struct ei_ctx *ctx)
{
	struct compound *c = ctx->compound;

	server_state_put(c->c_server_state);
	inode_active_put(c->c_inode);
	super_block_put(c->c_curr_sb);
	super_block_put(c->c_saved_sb);
	stateid_put(c->c_curr_stid);
	stateid_put(c->c_saved_stid);
	if (c->c_nfs4_client)
		nfs4_client_put(c->c_nfs4_client);
	/* Size the result array back up so every op's result is freed. */
	c->c_res->resarray.resarray_len = c->c_args->argarray.argarray_len;
	xdr_free((xdrproc_t)xdr_COMPOUND4res, (char *)c->c_res);
	xdr_free((xdrproc_t)xdr_COMPOUND4args, (char *)c->c_args);
	free(c->c_res);
	free(c->c_args);
	free(c);
	if (ctx->nc)
		nfs4_client_put(ctx->nc);
	free(ctx);
}

static void set_putfh(struct ei_ctx *ctx, unsigned int idx, uint64_t sb_id,
		      uint64_t ino)
{
	nfs_argop4 *a = &ctx->compound->c_args->argarray.argarray_val[idx];
	struct network_file_handle nfh = { 0 };

	nfh.nfh_sb = sb_id;
	nfh.nfh_ino = ino;
	a->argop = OP_PUTFH;
	a->nfs_argop4_u.opputfh.object.nfs_fh4_val = malloc(sizeof(nfh));
	ck_assert_ptr_nonnull(a->nfs_argop4_u.opputfh.object.nfs_fh4_val);
	memcpy(a->nfs_argop4_u.opputfh.object.nfs_fh4_val, &nfh, sizeof(nfh));
	a->nfs_argop4_u.opputfh.object.nfs_fh4_len = sizeof(nfh);
}

static void set_op(struct ei_ctx *ctx, unsigned int idx, uint32_t op)
{
	ctx->compound->c_args->argarray.argarray_val[idx].argop = op;
}

static nfsstat4 op_status(struct ei_ctx *ctx, unsigned int idx)
{
	return ctx->compound->c_res->resarray.resarray_val[idx]
		.nfs_resop4_u.opillegal.status;
}

/* PUTFH(sb, ino); GETATTR -- what stat(2) sends after SEQUENCE. */
static struct ei_ctx *run_putfh_getattr(uint64_t sb_id, uint64_t ino,
					bool with_client)
{
	struct ei_ctx *ctx = ei_ctx_new(2, with_client);

	set_putfh(ctx, 0, sb_id, ino);
	set_op(ctx, 1, OP_GETATTR);
	dispatch_compound(ctx->compound);
	return ctx;
}

static uint32_t arm(uint64_t sb_id, uint64_t ino, const uint32_t *ops,
		    uint32_t nops)
{
	struct error_inject_params p = {
		.eip_sb_id = sb_id,
		.eip_ino = ino,
		.eip_nops = nops,
	};
	uint32_t id = 0;

	for (uint32_t i = 0; i < nops; i++)
		p.eip_ops[i] = ops[i];
	ck_assert_int_eq(error_inject_arm(&p, &id), 0);
	ck_assert_uint_ne(id, 0);
	return id;
}

static struct error_inject_info *status(void)
{
	static struct error_inject_info info;

	ck_assert_int_eq(error_inject_status(&info), 0);
	return &info;
}

START_TEST(test_getattr_fires_once)
{
	uint32_t id = arm(SUPER_BLOCK_ROOT_ID, ei_ino, NULL, 0);
	struct ei_ctx *ctx;

	ck_assert_uint_eq(status()->eii_state, ERROR_INJECT_ARMED);
	ck_assert_uint_eq(status()->eii_params.eip_status, NFS4ERR_BADHANDLE);
	ck_assert_uint_ne(status()->eii_armed_ns, 0);

	ctx = run_putfh_getattr(SUPER_BLOCK_ROOT_ID, ei_ino, false);
	ck_assert_uint_eq(op_status(ctx, 0), NFS4_OK);
	ck_assert_uint_eq(op_status(ctx, 1), NFS4ERR_BADHANDLE);
	ck_assert_uint_eq(ctx->compound->c_res->status, NFS4ERR_BADHANDLE);
	ck_assert_uint_eq(ctx->compound->c_res->resarray.resarray_len, 2);
	ei_ctx_free(ctx);

	struct error_inject_info *i = status();

	ck_assert_uint_eq(i->eii_id, id);
	ck_assert_uint_eq(i->eii_state, ERROR_INJECT_FIRED);
	ck_assert_uint_eq(i->eii_fired_op, OP_GETATTR);
	ck_assert_uint_eq(i->eii_fired_opidx, 1);
	ck_assert_uint_eq(i->eii_fired_xid, 0x1234abcd);
	ck_assert_uint_ge(i->eii_fired_ns, i->eii_armed_ns);

	/* One-shot: the same compound now succeeds. */
	ctx = run_putfh_getattr(SUPER_BLOCK_ROOT_ID, ei_ino, false);
	ck_assert_uint_eq(op_status(ctx, 1), NFS4_OK);
	ck_assert_uint_eq(ctx->compound->c_res->status, NFS4_OK);
	ei_ctx_free(ctx);
	ck_assert_int_eq(error_inject_disarm(), -ENOENT);
}
END_TEST

START_TEST(test_other_file_does_not_match)
{
	struct ei_ctx *ctx;

	arm(SUPER_BLOCK_ROOT_ID, ei_ino, NULL, 0);

	ctx = run_putfh_getattr(SUPER_BLOCK_ROOT_ID, ei_other_ino, false);
	ck_assert_uint_eq(op_status(ctx, 1), NFS4_OK);
	ei_ctx_free(ctx);
	ctx = run_putfh_getattr(SUPER_BLOCK_ROOT_ID, INODE_ROOT_ID, false);
	ck_assert_uint_eq(op_status(ctx, 1), NFS4_OK);
	ei_ctx_free(ctx);

	ck_assert_uint_eq(status()->eii_state, ERROR_INJECT_ARMED);
	ck_assert_uint_eq(status()->eii_skipped, 0);
}
END_TEST

START_TEST(test_other_sb_does_not_match)
{
	struct ei_ctx *ctx;

	/* Same ino, different fsid. */
	arm(SUPER_BLOCK_ROOT_ID + 100, ei_ino, NULL, 0);
	ctx = run_putfh_getattr(SUPER_BLOCK_ROOT_ID, ei_ino, false);
	ck_assert_uint_eq(op_status(ctx, 1), NFS4_OK);
	ei_ctx_free(ctx);
	ck_assert_uint_eq(status()->eii_state, ERROR_INJECT_ARMED);
}
END_TEST

START_TEST(test_op_set_filters_and_counts_skipped)
{
	const uint32_t ops[] = { OP_CLOSE, OP_OPEN_DOWNGRADE };
	struct ei_ctx *ctx;

	arm(SUPER_BLOCK_ROOT_ID, ei_ino, ops, 2);

	/* GETATTR is not in the set: passes, counted as skipped. */
	ctx = run_putfh_getattr(SUPER_BLOCK_ROOT_ID, ei_ino, false);
	ck_assert_uint_eq(op_status(ctx, 1), NFS4_OK);
	ei_ctx_free(ctx);
	ck_assert_uint_eq(status()->eii_state, ERROR_INJECT_ARMED);
	ck_assert_uint_eq(status()->eii_skipped, 1);

	/* OPEN_DOWNGRADE is: fails before its handler runs. */
	ctx = ei_ctx_new(3, false);
	set_putfh(ctx, 0, SUPER_BLOCK_ROOT_ID, ei_ino);
	set_op(ctx, 1, OP_OPEN_DOWNGRADE);
	set_op(ctx, 2, OP_GETATTR);
	dispatch_compound(ctx->compound);
	ck_assert_uint_eq(op_status(ctx, 0), NFS4_OK);
	ck_assert_uint_eq(op_status(ctx, 1), NFS4ERR_BADHANDLE);
	ck_assert_uint_eq(ctx->compound->c_res->resarray.resarray_len, 2);
	ei_ctx_free(ctx);

	ck_assert_uint_eq(status()->eii_state, ERROR_INJECT_FIRED);
	ck_assert_uint_eq(status()->eii_fired_op, OP_OPEN_DOWNGRADE);
}
END_TEST

/* "Any op" never fires on the ops that only set the current FH. */
START_TEST(test_any_skips_fh_setters)
{
	const uint32_t setters[] = { OP_SEQUENCE,  OP_PUTFH,  OP_PUTPUBFH,
				     OP_PUTROOTFH, OP_SAVEFH, OP_RESTOREFH };
	struct ei_ctx *ctx = ei_ctx_new(1, false);
	struct super_block *sb = super_block_find(SUPER_BLOCK_ROOT_ID);

	ck_assert_ptr_nonnull(sb);
	arm(SUPER_BLOCK_ROOT_ID, ei_ino, NULL, 0);
	/* ei_ctx_free() puts the inode. */
	ctx->compound->c_inode = inode_find(sb, ei_ino);
	ck_assert_ptr_nonnull(ctx->compound->c_inode);
	for (size_t i = 0; i < sizeof(setters) / sizeof(setters[0]); i++)
		ck_assert_uint_eq(error_inject_check(ctx->compound, setters[i]),
				  NFS4_OK);
	ck_assert_uint_eq(status()->eii_state, ERROR_INJECT_ARMED);
	ck_assert_uint_eq(status()->eii_skipped, 0);

	/* Named explicitly, a setter does match. */
	const uint32_t savefh[] = { OP_SAVEFH };

	arm(SUPER_BLOCK_ROOT_ID, ei_ino, savefh, 1);
	ck_assert_uint_eq(error_inject_check(ctx->compound, OP_SAVEFH),
			  NFS4ERR_BADHANDLE);
	ei_ctx_free(ctx);
	super_block_put(sb);
}
END_TEST

START_TEST(test_client_filter)
{
	struct error_inject_params p = {
		.eip_sb_id = SUPER_BLOCK_ROOT_ID,
		.eip_ino = ei_ino,
		.eip_clientid = 0xdeadbeef00000001ULL,
	};
	struct ei_ctx *ctx;

	ck_assert_int_eq(error_inject_arm(&p, NULL), 0);

	/* No client on the compound, or a different client: no match. */
	ctx = run_putfh_getattr(SUPER_BLOCK_ROOT_ID, ei_ino, false);
	ck_assert_uint_eq(op_status(ctx, 1), NFS4_OK);
	ei_ctx_free(ctx);
	ctx = run_putfh_getattr(SUPER_BLOCK_ROOT_ID, ei_ino, true);
	ck_assert_uint_eq(op_status(ctx, 1), NFS4_OK);
	ei_ctx_free(ctx);
	ck_assert_uint_eq(status()->eii_state, ERROR_INJECT_ARMED);

	/* By address. */
	p.eip_clientid = 0;
	p.eip_addr = htonl(EI_ADDR + 1);
	ck_assert_int_eq(error_inject_arm(&p, NULL), 0);
	ctx = run_putfh_getattr(SUPER_BLOCK_ROOT_ID, ei_ino, true);
	ck_assert_uint_eq(op_status(ctx, 1), NFS4_OK);
	ei_ctx_free(ctx);

	p.eip_addr = htonl(EI_ADDR);
	ck_assert_int_eq(error_inject_arm(&p, NULL), 0);
	ctx = run_putfh_getattr(SUPER_BLOCK_ROOT_ID, ei_ino, true);
	ck_assert_uint_eq(op_status(ctx, 1), NFS4ERR_BADHANDLE);
	ck_assert_uint_eq(status()->eii_fired_clientid, ei_ctx_clientid(ctx));
	ei_ctx_free(ctx);

	/* By clientid. */
	ctx = ei_ctx_new(2, true);
	p.eip_addr = 0;
	p.eip_clientid = ei_ctx_clientid(ctx);
	ck_assert_int_eq(error_inject_arm(&p, NULL), 0);
	set_putfh(ctx, 0, SUPER_BLOCK_ROOT_ID, ei_ino);
	set_op(ctx, 1, OP_GETATTR);
	dispatch_compound(ctx->compound);
	ck_assert_uint_eq(op_status(ctx, 1), NFS4ERR_BADHANDLE);
	ei_ctx_free(ctx);
}
END_TEST

START_TEST(test_status_override)
{
	struct error_inject_params p = {
		.eip_sb_id = SUPER_BLOCK_ROOT_ID,
		.eip_ino = ei_ino,
		.eip_status = NFS4ERR_STALE,
	};
	struct ei_ctx *ctx;

	ck_assert_int_eq(error_inject_arm(&p, NULL), 0);
	ctx = run_putfh_getattr(SUPER_BLOCK_ROOT_ID, ei_ino, false);
	ck_assert_uint_eq(op_status(ctx, 1), NFS4ERR_STALE);
	ei_ctx_free(ctx);
}
END_TEST

START_TEST(test_disarm_and_rearm)
{
	struct ei_ctx *ctx;
	uint32_t id1, id2;

	ck_assert_int_eq(error_inject_disarm(), -ENOENT);

	id1 = arm(SUPER_BLOCK_ROOT_ID, ei_ino, NULL, 0);
	ck_assert_int_eq(error_inject_disarm(), 0);
	ck_assert_uint_eq(status()->eii_state, ERROR_INJECT_DISARMED);
	ctx = run_putfh_getattr(SUPER_BLOCK_ROOT_ID, ei_ino, false);
	ck_assert_uint_eq(op_status(ctx, 1), NFS4_OK);
	ei_ctx_free(ctx);

	/* Re-arming replaces the target. */
	arm(SUPER_BLOCK_ROOT_ID, ei_other_ino, NULL, 0);
	id2 = arm(SUPER_BLOCK_ROOT_ID, ei_ino, NULL, 0);
	ck_assert_uint_gt(id2, id1);
	ctx = run_putfh_getattr(SUPER_BLOCK_ROOT_ID, ei_other_ino, false);
	ck_assert_uint_eq(op_status(ctx, 1), NFS4_OK);
	ei_ctx_free(ctx);
	ctx = run_putfh_getattr(SUPER_BLOCK_ROOT_ID, ei_ino, false);
	ck_assert_uint_eq(op_status(ctx, 1), NFS4ERR_BADHANDLE);
	ei_ctx_free(ctx);
	ck_assert_uint_eq(status()->eii_id, id2);
}
END_TEST

START_TEST(test_arm_validation)
{
	struct error_inject_params p = { .eip_sb_id = SUPER_BLOCK_ROOT_ID };

	ck_assert_int_eq(error_inject_arm(&p, NULL), -EINVAL);
	p.eip_ino = ei_ino;
	p.eip_sb_id = 0;
	ck_assert_int_eq(error_inject_arm(&p, NULL), -EINVAL);
	p.eip_sb_id = SUPER_BLOCK_ROOT_ID;
	p.eip_nops = ERROR_INJECT_MAX_OPS + 1;
	ck_assert_int_eq(error_inject_arm(&p, NULL), -EINVAL);
	p.eip_nops = 1;
	p.eip_ops[0] = 100000;
	ck_assert_int_eq(error_inject_arm(&p, NULL), -EINVAL);
}
END_TEST

static Suite *error_inject_suite(void)
{
	Suite *s = suite_create("error_inject");
	TCase *tc = tcase_create("one_shot");

	tcase_add_checked_fixture(tc, ei_setup, ei_teardown);
	tcase_add_test(tc, test_getattr_fires_once);
	tcase_add_test(tc, test_other_file_does_not_match);
	tcase_add_test(tc, test_other_sb_does_not_match);
	tcase_add_test(tc, test_op_set_filters_and_counts_skipped);
	tcase_add_test(tc, test_any_skips_fh_setters);
	tcase_add_test(tc, test_client_filter);
	tcase_add_test(tc, test_status_override);
	tcase_add_test(tc, test_disarm_and_rearm);
	tcase_add_test(tc, test_arm_validation);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	return nfs4_test_run(error_inject_suite());
}
