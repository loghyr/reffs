/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * WRONGSEC unit tests -- RFC 8881 Section 2.6.3.1.
 *
 * Build mock COMPOUND4args/COMPOUND4res and call dispatch_compound()
 * directly.  Two exports with different flavor lists verify that
 * NFS4ERR_WRONGSEC is returned (or suppressed) per the RFC rules.
 *
 * Test fixture:
 *   root sb (sb_id=1):  flavors = [AUTH_SYS, KRB5]
 *   child sb (sb_id=42): mounted at /secure, flavors = [KRB5]
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <stdlib.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include <rpc/auth.h>
#include <rpc/xdr.h>

#include <check.h>

#include "nfsv42_xdr.h"
#include "nfsv42_names.h"
#include "reffs/rpc.h"
#include "reffs/server.h"
#include "reffs/task.h"
#include "reffs/super_block.h"
#include "reffs/inode.h"
#include "reffs/dirent.h"
#include "reffs/fs.h"
#include "reffs/filehandle.h"
#include "nfs4/compound.h"
#include "nfs4/ops.h"
#include "nfs4_test_harness.h"

/* ------------------------------------------------------------------ */
/* Test fixture state                                                  */
/* ------------------------------------------------------------------ */

static struct super_block *child_sb;
static uint64_t child_sb_id = 42;

static void wrongsec_setup(void)
{
	nfs4_test_setup();

	/* Root sb already exists from nfs4_test_setup().
	 * Set root flavors to [AUTH_SYS, KRB5]. */
	struct super_block *root = super_block_find(SUPER_BLOCK_ROOT_ID);

	ck_assert_ptr_nonnull(root);
	enum reffs_auth_flavor root_f[] = { REFFS_AUTH_SYS, REFFS_AUTH_KRB5 };

	super_block_set_flavors(root, root_f, 2);
	super_block_put(root);

	/* Create /secure directory in root sb. */
	ck_assert_int_eq(reffs_fs_mkdir("/secure", 0755), 0);

	/* Create child sb mounted at /secure, flavors = [KRB5]. */
	child_sb = super_block_alloc(child_sb_id, (char *)"/secure",
				     REFFS_STORAGE_RAM, NULL);
	ck_assert_ptr_nonnull(child_sb);
	ck_assert_int_eq(super_block_dirent_create(child_sb, NULL,
						   reffs_life_action_birth),
			 0);
	ck_assert_int_eq(super_block_mount(child_sb, "/secure"), 0);

	enum reffs_auth_flavor child_f[] = { REFFS_AUTH_KRB5 };

	super_block_set_flavors(child_sb, child_f, 1);
}

static void wrongsec_teardown(void)
{
	if (child_sb) {
		super_block_unmount(child_sb);
		super_block_destroy(child_sb);
		super_block_release_dirents(child_sb);
		super_block_put(child_sb);
		child_sb = NULL;
	}

	reffs_fs_rmdir("/secure");
	nfs4_test_teardown();
}

/* ------------------------------------------------------------------ */
/* Compound builder helpers                                            */
/* ------------------------------------------------------------------ */

struct wrongsec_ctx {
	struct rpc_trans rt;
	struct task task;
	struct compound *compound;
};

/*
 * Allocate a compound with nops slots.  The caller fills in each
 * argop via the set_op_* helpers below.
 */
static struct wrongsec_ctx *make_ctx(unsigned int nops, uint32_t flavor,
				     uint32_t gss_svc)
{
	struct wrongsec_ctx *ctx = calloc(1, sizeof(*ctx));

	ck_assert_ptr_nonnull(ctx);

	atomic_store_explicit(&ctx->task.t_state, TASK_RUNNING,
			      memory_order_relaxed);
	ctx->rt.rt_task = &ctx->task;
	ctx->rt.rt_fd = -1;
	ctx->rt.rt_info.ri_cred.rc_flavor = flavor;
	if (flavor == RPCSEC_GSS)
		ctx->rt.rt_info.ri_cred.rc_gss.gc_svc = gss_svc;

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

static void free_ctx(struct wrongsec_ctx *ctx)
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
			/* Free per-op allocations in argarray. */
			for (u_int i = 0; i < c->c_args->argarray.argarray_len;
			     i++) {
				nfs_argop4 *a =
					&c->c_args->argarray.argarray_val[i];
				switch (a->argop) {
				case OP_PUTFH:
					free(a->nfs_argop4_u.opputfh.object
						     .nfs_fh4_val);
					break;
				case OP_LOOKUP:
					free(a->nfs_argop4_u.oplookup.objname
						     .utf8string_val);
					break;
				case OP_SECINFO:
					free(a->nfs_argop4_u.opsecinfo.name
						     .utf8string_val);
					break;
				default:
					break;
				}
			}
			free(c->c_args->argarray.argarray_val);
			free(c->c_args);
		}
		if (c->c_res) {
			/*
			 * Free result-side allocations.  GETFH
			 * allocates nfs_fh4_val; SECINFO allocates
			 * resok + OID bytes.
			 */
			for (u_int i = 0; i < c->c_res->resarray.resarray_len;
			     i++) {
				nfs_resop4 *r =
					&c->c_res->resarray.resarray_val[i];
				if (r->resop == OP_GETFH &&
				    !r->nfs_resop4_u.opgetfh.status) {
					free(r->nfs_resop4_u.opgetfh.GETFH4res_u
						     .resok4.object.nfs_fh4_val);
				}
				if (r->resop == OP_SECINFO &&
				    !r->nfs_resop4_u.opsecinfo.status) {
					SECINFO4resok *si =
						&r->nfs_resop4_u.opsecinfo
							 .SECINFO4res_u.resok4;
					for (u_int j = 0;
					     j < si->SECINFO4resok_len; j++) {
						free(si->SECINFO4resok_val[j]
							     .secinfo4_u
							     .flavor_info.oid
							     .sec_oid4_val);
					}
					free(si->SECINFO4resok_val);
				}
				if (r->resop == OP_SECINFO_NO_NAME &&
				    !r->nfs_resop4_u.opsecinfo_no_name.status) {
					SECINFO4resok *si =
						&r->nfs_resop4_u
							 .opsecinfo_no_name
							 .SECINFO4res_u.resok4;
					for (u_int j = 0;
					     j < si->SECINFO4resok_len; j++) {
						free(si->SECINFO4resok_val[j]
							     .secinfo4_u
							     .flavor_info.oid
							     .sec_oid4_val);
					}
					free(si->SECINFO4resok_val);
				}
			}
			free(c->c_res->resarray.resarray_val);
			free(c->c_res);
		}
		free(c);
	}
	free(ctx);
}

static nfsstat4 op_status(struct wrongsec_ctx *ctx, unsigned int idx)
{
	return ctx->compound->c_res->resarray.resarray_val[idx]
		.nfs_resop4_u.opillegal.status;
}

/* --- Op setters --- */

static void set_op_putrootfh(struct wrongsec_ctx *ctx, unsigned int idx)
{
	ctx->compound->c_args->argarray.argarray_val[idx].argop = OP_PUTROOTFH;
}

static void set_op_putpubfh(struct wrongsec_ctx *ctx, unsigned int idx)
{
	ctx->compound->c_args->argarray.argarray_val[idx].argop = OP_PUTPUBFH;
}

static void set_op_putfh(struct wrongsec_ctx *ctx, unsigned int idx,
			 uint64_t sb_id, uint64_t ino)
{
	nfs_argop4 *argop = &ctx->compound->c_args->argarray.argarray_val[idx];

	argop->argop = OP_PUTFH;

	struct network_file_handle nfh = { 0 };

	nfh.nfh_sb = sb_id;
	nfh.nfh_ino = ino;

	argop->nfs_argop4_u.opputfh.object.nfs_fh4_val = malloc(sizeof(nfh));
	ck_assert_ptr_nonnull(argop->nfs_argop4_u.opputfh.object.nfs_fh4_val);
	memcpy(argop->nfs_argop4_u.opputfh.object.nfs_fh4_val, &nfh,
	       sizeof(nfh));
	argop->nfs_argop4_u.opputfh.object.nfs_fh4_len = sizeof(nfh);
}

static void set_op_savefh(struct wrongsec_ctx *ctx, unsigned int idx)
{
	ctx->compound->c_args->argarray.argarray_val[idx].argop = OP_SAVEFH;
}

static void set_op_restorefh(struct wrongsec_ctx *ctx, unsigned int idx)
{
	ctx->compound->c_args->argarray.argarray_val[idx].argop = OP_RESTOREFH;
}

static void set_op_getattr(struct wrongsec_ctx *ctx, unsigned int idx)
{
	ctx->compound->c_args->argarray.argarray_val[idx].argop = OP_GETATTR;
}

static void set_op_getfh(struct wrongsec_ctx *ctx, unsigned int idx)
{
	ctx->compound->c_args->argarray.argarray_val[idx].argop = OP_GETFH;
}

static void set_op_lookup(struct wrongsec_ctx *ctx, unsigned int idx,
			  const char *name)
{
	nfs_argop4 *argop = &ctx->compound->c_args->argarray.argarray_val[idx];

	argop->argop = OP_LOOKUP;
	argop->nfs_argop4_u.oplookup.objname.utf8string_val = strdup(name);
	argop->nfs_argop4_u.oplookup.objname.utf8string_len = strlen(name);
}

static void set_op_lookupp(struct wrongsec_ctx *ctx, unsigned int idx)
{
	ctx->compound->c_args->argarray.argarray_val[idx].argop = OP_LOOKUPP;
}

static void set_op_secinfo(struct wrongsec_ctx *ctx, unsigned int idx,
			   const char *name)
{
	nfs_argop4 *argop = &ctx->compound->c_args->argarray.argarray_val[idx];

	argop->argop = OP_SECINFO;
	argop->nfs_argop4_u.opsecinfo.name.utf8string_val = strdup(name);
	argop->nfs_argop4_u.opsecinfo.name.utf8string_len = strlen(name);
}

static void set_op_secinfo_no_name(struct wrongsec_ctx *ctx, unsigned int idx,
				   secinfo_style4 style)
{
	nfs_argop4 *argop = &ctx->compound->c_args->argarray.argarray_val[idx];

	argop->argop = OP_SECINFO_NO_NAME;
	argop->nfs_argop4_u.opsecinfo_no_name = style;
}

static void set_op_read(struct wrongsec_ctx *ctx, unsigned int idx)
{
	ctx->compound->c_args->argarray.argarray_val[idx].argop = OP_READ;
}

/* ------------------------------------------------------------------ */
/* Phase 1 Tests                                                       */
/* ------------------------------------------------------------------ */

/*
 * S2.6.3.1.1.3 -- PUTROOTFH + LOOKUP("secure")
 * WRONGSEC comes from LOOKUP, not PUTROOTFH.
 */
START_TEST(test_putrootfh_lookup_wrongsec_on_lookup)
{
	struct wrongsec_ctx *ctx = make_ctx(2, AUTH_SYS, 0);

	set_op_putrootfh(ctx, 0);
	set_op_lookup(ctx, 1, "secure");

	dispatch_compound(ctx->compound);

	/* PUTROOTFH must succeed (next op is LOOKUP). */
	ck_assert_int_eq(op_status(ctx, 0), NFS4_OK);
	/* LOOKUP crosses into /secure which requires KRB5. */
	ck_assert_int_eq(op_status(ctx, 1), NFS4ERR_WRONGSEC);

	free_ctx(ctx);
}
END_TEST

/*
 * S2.6.3.1.1.3 -- PUTROOTFH + LOOKUP with allowed flavor.
 */
START_TEST(test_putrootfh_lookup_allowed_flavor)
{
	struct wrongsec_ctx *ctx = make_ctx(2, RPCSEC_GSS, RPC_GSS_SVC_NONE);

	set_op_putrootfh(ctx, 0);
	set_op_lookup(ctx, 1, "secure");

	dispatch_compound(ctx->compound);

	ck_assert_int_eq(op_status(ctx, 0), NFS4_OK);
	ck_assert_int_eq(op_status(ctx, 1), NFS4_OK);

	free_ctx(ctx);
}
END_TEST

/*
 * S2.6.3.1.1.4 -- PUTFH(secure root) + LOOKUPP.
 * PUTFH must NOT return WRONGSEC.
 */
START_TEST(test_putfh_lookupp_no_wrongsec_on_putfh)
{
	struct wrongsec_ctx *ctx = make_ctx(2, AUTH_SYS, 0);

	set_op_putfh(ctx, 0, child_sb_id, INODE_ROOT_ID);
	set_op_lookupp(ctx, 1);

	dispatch_compound(ctx->compound);

	/* PUTFH must succeed (next op is LOOKUPP, S2.6.3.1.1.4). */
	ck_assert_int_eq(op_status(ctx, 0), NFS4_OK);
	/* LOOKUPP crosses to root which allows AUTH_SYS. */
	ck_assert_int_eq(op_status(ctx, 1), NFS4_OK);

	free_ctx(ctx);
}
END_TEST

/*
 * S2.6.3.1.1.5 -- PUTFH(secure) + SECINFO.
 * Must NOT return WRONGSEC on either op.
 */
START_TEST(test_putfh_secinfo_no_wrongsec)
{
	struct wrongsec_ctx *ctx = make_ctx(2, AUTH_SYS, 0);

	set_op_putfh(ctx, 0, child_sb_id, INODE_ROOT_ID);
	set_op_secinfo(ctx, 1, "anything");

	dispatch_compound(ctx->compound);

	ck_assert_int_eq(op_status(ctx, 0), NFS4_OK);
	/*
	 * SECINFO may return NOENT (component doesn't exist) but
	 * must NOT return WRONGSEC.
	 */
	ck_assert_int_ne(op_status(ctx, 1), NFS4ERR_WRONGSEC);

	free_ctx(ctx);
}
END_TEST

/*
 * S2.6.3.1.1.5 -- PUTFH(secure) + SECINFO_NO_NAME.
 */
START_TEST(test_putfh_secinfo_no_name_no_wrongsec)
{
	struct wrongsec_ctx *ctx = make_ctx(2, AUTH_SYS, 0);

	set_op_putfh(ctx, 0, child_sb_id, INODE_ROOT_ID);
	set_op_secinfo_no_name(ctx, 1, SECINFO_STYLE4_CURRENT_FH);

	dispatch_compound(ctx->compound);

	ck_assert_int_eq(op_status(ctx, 0), NFS4_OK);
	ck_assert_int_eq(op_status(ctx, 1), NFS4_OK);

	free_ctx(ctx);
}
END_TEST

/*
 * S2.6.3.1.1.6 -- PUTFH(secure) as last op.
 * Must NOT return WRONGSEC.
 */
START_TEST(test_putfh_nothing_no_wrongsec)
{
	struct wrongsec_ctx *ctx = make_ctx(1, AUTH_SYS, 0);

	set_op_putfh(ctx, 0, child_sb_id, INODE_ROOT_ID);

	dispatch_compound(ctx->compound);

	ck_assert_int_eq(op_status(ctx, 0), NFS4_OK);

	free_ctx(ctx);
}
END_TEST

/*
 * S2.6.3.1.1.6 -- PUTROOTFH as last op.
 */
START_TEST(test_putrootfh_nothing_no_wrongsec)
{
	struct wrongsec_ctx *ctx = make_ctx(1, AUTH_NONE, 0);

	set_op_putrootfh(ctx, 0);

	dispatch_compound(ctx->compound);

	ck_assert_int_eq(op_status(ctx, 0), NFS4_OK);

	free_ctx(ctx);
}
END_TEST

/*
 * S2.6.3.1.1.7 -- PUTFH(secure) + GETATTR.
 * Put-FH MUST return WRONGSEC.
 */
START_TEST(test_putfh_getattr_wrongsec)
{
	struct wrongsec_ctx *ctx = make_ctx(2, AUTH_SYS, 0);

	set_op_putfh(ctx, 0, child_sb_id, INODE_ROOT_ID);
	set_op_getattr(ctx, 1);

	dispatch_compound(ctx->compound);

	ck_assert_int_eq(op_status(ctx, 0), NFS4ERR_WRONGSEC);

	free_ctx(ctx);
}
END_TEST

/*
 * S2.6.3.1.1.7 -- PUTFH(secure) + GETFH.
 */
START_TEST(test_putfh_getfh_wrongsec)
{
	struct wrongsec_ctx *ctx = make_ctx(2, AUTH_SYS, 0);

	set_op_putfh(ctx, 0, child_sb_id, INODE_ROOT_ID);
	set_op_getfh(ctx, 1);

	dispatch_compound(ctx->compound);

	ck_assert_int_eq(op_status(ctx, 0), NFS4ERR_WRONGSEC);

	free_ctx(ctx);
}
END_TEST

/*
 * S2.6.3.1.1.7 -- PUTROOTFH + GETATTR (positive: flavor matches).
 */
START_TEST(test_putrootfh_getattr_allowed)
{
	struct wrongsec_ctx *ctx = make_ctx(2, AUTH_SYS, 0);

	set_op_putrootfh(ctx, 0);
	set_op_getattr(ctx, 1);

	dispatch_compound(ctx->compound);

	ck_assert_int_eq(op_status(ctx, 0), NFS4_OK);

	free_ctx(ctx);
}
END_TEST

/*
 * S2.6.3.1.1.7 -- PUTPUBFH + GETATTR with wrong flavor.
 */
START_TEST(test_putpubfh_getattr_wrongsec)
{
	struct wrongsec_ctx *ctx = make_ctx(2, AUTH_NONE, 0);

	set_op_putpubfh(ctx, 0);
	set_op_getattr(ctx, 1);

	dispatch_compound(ctx->compound);

	ck_assert_int_eq(op_status(ctx, 0), NFS4ERR_WRONGSEC);

	free_ctx(ctx);
}
END_TEST

/*
 * RESTOREFH -- restore saved FH into restricted export + GETATTR.
 * RESTOREFH must return WRONGSEC.
 */
START_TEST(test_restorefh_into_restricted_export)
{
	struct wrongsec_ctx *ctx = make_ctx(5, AUTH_SYS, 0);

	/* PUTFH(secure), SAVEFH, PUTROOTFH, RESTOREFH, GETATTR */
	set_op_putfh(ctx, 0, child_sb_id, INODE_ROOT_ID);
	set_op_savefh(ctx, 1);
	set_op_putrootfh(ctx, 2);
	set_op_restorefh(ctx, 3);
	set_op_getattr(ctx, 4);

	dispatch_compound(ctx->compound);

	/*
	 * PUTFH(secure): first of two put-FH ops (SAVEFH transparent,
	 * next real put-FH is PUTROOTFH) -- S2.6.3.1.1.2: ignored.
	 */
	ck_assert_int_eq(op_status(ctx, 0), NFS4_OK);
	/* SAVEFH: always OK. */
	ck_assert_int_eq(op_status(ctx, 1), NFS4_OK);
	/*
	 * PUTROOTFH: next real op (skipping nothing) is RESTOREFH,
	 * which is another put-FH -- S2.6.3.1.1.2: ignored.
	 */
	ck_assert_int_eq(op_status(ctx, 2), NFS4_OK);
	/*
	 * RESTOREFH: restores secure FH (/secure, KRB5 only).
	 * Next op is GETATTR ("anything else").
	 * S2.6.3.1.1.7: must return WRONGSEC.
	 */
	ck_assert_int_eq(op_status(ctx, 3), NFS4ERR_WRONGSEC);

	free_ctx(ctx);
}
END_TEST

/*
 * S2.6.3.1.1.8 -- SECINFO consumes FH.
 * Next op gets NOFILEHANDLE, not WRONGSEC.
 */
START_TEST(test_secinfo_consumes_fh)
{
	struct wrongsec_ctx *ctx = make_ctx(3, AUTH_SYS, 0);

	set_op_putrootfh(ctx, 0);
	set_op_secinfo(ctx, 1, "secure");
	set_op_read(ctx, 2);

	dispatch_compound(ctx->compound);

	ck_assert_int_eq(op_status(ctx, 0), NFS4_OK);
	ck_assert_int_eq(op_status(ctx, 1), NFS4_OK);
	/* READ after SECINFO -- FH consumed, must get NOFILEHANDLE. */
	ck_assert_int_eq(op_status(ctx, 2), NFS4ERR_NOFILEHANDLE);

	free_ctx(ctx);
}
END_TEST

/*
 * S2.6.3.1.1.8 -- SECINFO_NO_NAME consumes FH.
 * Use GETFH as the probe op (not GETATTR with empty bitmap,
 * which is a no-op that doesn't check the filehandle).
 */
START_TEST(test_secinfo_no_name_consumes_fh)
{
	struct wrongsec_ctx *ctx = make_ctx(3, AUTH_SYS, 0);

	set_op_putrootfh(ctx, 0);
	set_op_secinfo_no_name(ctx, 1, SECINFO_STYLE4_CURRENT_FH);
	set_op_getfh(ctx, 2);

	dispatch_compound(ctx->compound);

	ck_assert_int_eq(op_status(ctx, 0), NFS4_OK);
	ck_assert_int_eq(op_status(ctx, 1), NFS4_OK);
	ck_assert_int_eq(op_status(ctx, 2), NFS4ERR_NOFILEHANDLE);

	free_ctx(ctx);
}
END_TEST

/* No flavors configured -- everything allowed. */
START_TEST(test_no_flavors_allows_all)
{
	/* Temporarily clear root flavors. */
	struct super_block *root = super_block_find(SUPER_BLOCK_ROOT_ID);

	ck_assert_ptr_nonnull(root);
	super_block_set_flavors(root, NULL, 0);
	super_block_put(root);

	struct wrongsec_ctx *ctx = make_ctx(2, AUTH_NONE, 0);

	set_op_putrootfh(ctx, 0);
	set_op_getattr(ctx, 1);

	dispatch_compound(ctx->compound);

	ck_assert_int_eq(op_status(ctx, 0), NFS4_OK);

	free_ctx(ctx);

	/* Restore root flavors for other tests. */
	root = super_block_find(SUPER_BLOCK_ROOT_ID);
	enum reffs_auth_flavor root_f[] = { REFFS_AUTH_SYS, REFFS_AUTH_KRB5 };

	super_block_set_flavors(root, root_f, 2);
	super_block_put(root);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Phase 2 Tests                                                       */
/* ------------------------------------------------------------------ */

/*
 * S2.6.3.1.1.1 -- PUTROOTFH + SAVEFH + SECINFO.
 * Server skips SAVEFH, sees SECINFO as next real op.
 */
START_TEST(test_putfh_savefh_secinfo_no_wrongsec)
{
	struct wrongsec_ctx *ctx = make_ctx(3, AUTH_SYS, 0);

	set_op_putrootfh(ctx, 0);
	set_op_savefh(ctx, 1);
	set_op_secinfo(ctx, 2, "secure");

	dispatch_compound(ctx->compound);

	ck_assert_int_eq(op_status(ctx, 0), NFS4_OK);
	ck_assert_int_eq(op_status(ctx, 1), NFS4_OK);
	ck_assert_int_ne(op_status(ctx, 2), NFS4ERR_WRONGSEC);

	free_ctx(ctx);
}
END_TEST

/*
 * S2.6.3.1.1.1 -- PUTFH(secure) + SAVEFH + GETATTR.
 * Server skips SAVEFH, sees GETATTR -- must enforce WRONGSEC on PUTFH.
 */
START_TEST(test_putfh_savefh_getattr_wrongsec)
{
	struct wrongsec_ctx *ctx = make_ctx(3, AUTH_SYS, 0);

	set_op_putfh(ctx, 0, child_sb_id, INODE_ROOT_ID);
	set_op_savefh(ctx, 1);
	set_op_getattr(ctx, 2);

	dispatch_compound(ctx->compound);

	ck_assert_int_eq(op_status(ctx, 0), NFS4ERR_WRONGSEC);

	free_ctx(ctx);
}
END_TEST

/*
 * S2.6.3.1.1.2 -- PUTFH(secure) + PUTROOTFH + LOOKUP.
 * First put-FH is ignored; PUTROOTFH is the effective one.
 */
START_TEST(test_two_putfh_lookup_no_wrongsec_on_first)
{
	struct wrongsec_ctx *ctx = make_ctx(3, AUTH_SYS, 0);

	set_op_putfh(ctx, 0, child_sb_id, INODE_ROOT_ID);
	set_op_putrootfh(ctx, 1);
	set_op_lookup(ctx, 2, "secure");

	dispatch_compound(ctx->compound);

	/* First put-FH: ignored (S2.6.3.1.1.2). */
	ck_assert_int_eq(op_status(ctx, 0), NFS4_OK);
	/* PUTROOTFH + LOOKUP: PUTROOTFH OK (S2.6.3.1.1.3). */
	ck_assert_int_eq(op_status(ctx, 1), NFS4_OK);
	/* LOOKUP crosses into /secure (KRB5), client is AUTH_SYS. */
	ck_assert_int_eq(op_status(ctx, 2), NFS4ERR_WRONGSEC);

	free_ctx(ctx);
}
END_TEST

/*
 * S2.6.3.1.1.2 -- PUTFH(secure) + PUTROOTFH + GETATTR.
 * First put-FH ignored; PUTROOTFH + GETATTR: root allows SYS.
 */
START_TEST(test_two_putfh_getattr_no_wrongsec)
{
	struct wrongsec_ctx *ctx = make_ctx(3, AUTH_SYS, 0);

	set_op_putfh(ctx, 0, child_sb_id, INODE_ROOT_ID);
	set_op_putrootfh(ctx, 1);
	set_op_getattr(ctx, 2);

	dispatch_compound(ctx->compound);

	ck_assert_int_eq(op_status(ctx, 0), NFS4_OK);
	ck_assert_int_eq(op_status(ctx, 1), NFS4_OK);

	free_ctx(ctx);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

static Suite *wrongsec_suite(void)
{
	Suite *s = suite_create("WRONGSEC (RFC 8881 S2.6.3.1)");
	TCase *tc;

	tc = tcase_create("phase1");
	tcase_add_checked_fixture(tc, wrongsec_setup, wrongsec_teardown);
	tcase_add_test(tc, test_putrootfh_lookup_wrongsec_on_lookup);
	tcase_add_test(tc, test_putrootfh_lookup_allowed_flavor);
	tcase_add_test(tc, test_putfh_lookupp_no_wrongsec_on_putfh);
	tcase_add_test(tc, test_putfh_secinfo_no_wrongsec);
	tcase_add_test(tc, test_putfh_secinfo_no_name_no_wrongsec);
	tcase_add_test(tc, test_putfh_nothing_no_wrongsec);
	tcase_add_test(tc, test_putrootfh_nothing_no_wrongsec);
	tcase_add_test(tc, test_putfh_getattr_wrongsec);
	tcase_add_test(tc, test_putfh_getfh_wrongsec);
	tcase_add_test(tc, test_putrootfh_getattr_allowed);
	tcase_add_test(tc, test_putpubfh_getattr_wrongsec);
	tcase_add_test(tc, test_restorefh_into_restricted_export);
	tcase_add_test(tc, test_secinfo_consumes_fh);
	tcase_add_test(tc, test_secinfo_no_name_consumes_fh);
	tcase_add_test(tc, test_no_flavors_allows_all);
	suite_add_tcase(s, tc);

	tc = tcase_create("phase2");
	tcase_add_checked_fixture(tc, wrongsec_setup, wrongsec_teardown);
	tcase_add_test(tc, test_putfh_savefh_secinfo_no_wrongsec);
	tcase_add_test(tc, test_putfh_savefh_getattr_wrongsec);
	tcase_add_test(tc, test_two_putfh_lookup_no_wrongsec_on_first);
	tcase_add_test(tc, test_two_putfh_getattr_no_wrongsec);
	suite_add_tcase(s, tc);

	return s;
}

int main(void)
{
	return nfs4_test_run(wrongsec_suite());
}
