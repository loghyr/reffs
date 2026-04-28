/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Unit tests for the PROXY_REGISTRATION + PROXY_PROGRESS op handlers
 * (mirror-of-design proxy-server.md Phase 6a).
 *
 * Slice 6a scope: bare flag-bit + session-context validation, sets
 * nc_is_registered_ps on success.  Allowlist identity check, mTLS
 * support, squat-guard and audit logging land in slice 6b -- tests
 * for those defer with the implementation.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <netinet/in.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <check.h>

#include "nfsv42_xdr.h"
#include "reffs/inode.h"
#include "reffs/server.h"
#include "reffs/super_block.h"
#include "reffs/task.h"
#include "nfs4/client.h"
#include "nfs4/compound.h"
#include "nfs4/migration_record.h"
#include "nfs4/ops.h"
#include "nfs4/proxy_stateid.h"
#include "nfs4/stateid.h"
#include "nfs4_test_harness.h"

/* ------------------------------------------------------------------ */
/* Compound mock                                                       */
/* ------------------------------------------------------------------ */

/*
 * The PROXY_REGISTRATION handler reads three things off the
 * compound: c_nfs4_client (and its nc_exchgid_flags),
 * c_gss_principal, and the args from the resarray slot.  No FH,
 * stateid, or inode is touched -- so a much smaller mock than the
 * trust-stateid harness is sufficient.
 */
struct pr_ctx {
	struct rpc_trans rt;
	struct task task;
	struct compound *compound;
	struct nfs4_client *nc; /* freed in pr_free */
};

/*
 * Unique clientid per pr_alloc call.  test_proxy_registration_reject_
 * bad_prr_flags loops 3 times, alloc/free per iteration -- a hard-coded
 * clientid collides because nfs4_client_alloc -> client_assign fails
 * when the slot is already taken (RCU defers the actual delete).
 */
static clientid4 pr_next_clientid(void)
{
	static clientid4 next = 0xCACE0001;

	return next++;
}

static struct pr_ctx *pr_alloc(uint32_t exchgid_flags,
			       const char *gss_principal)
{
	struct pr_ctx *cm = calloc(1, sizeof(*cm));

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

	c->c_args->argarray.argarray_len = 1;
	c->c_args->argarray.argarray_val = calloc(1, sizeof(nfs_argop4));
	ck_assert_ptr_nonnull(c->c_args->argarray.argarray_val);
	c->c_args->argarray.argarray_val[0].argop = OP_PROXY_REGISTRATION;

	c->c_res->resarray.resarray_len = 1;
	c->c_res->resarray.resarray_val = calloc(1, sizeof(nfs_resop4));
	ck_assert_ptr_nonnull(c->c_res->resarray.resarray_val);
	c->c_res->resarray.resarray_val[0].resop = OP_PROXY_REGISTRATION;

	c->c_server_state = server_state_find();
	ck_assert_ptr_nonnull(c->c_server_state);

	verifier4 v;
	struct sockaddr_in sin;

	memset(&v, 0x42, sizeof(v));
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(0x7f000003);
	sin.sin_port = htons(2049);

	cm->nc = nfs4_client_alloc(&v, &sin, 1, pr_next_clientid(), 0);
	ck_assert_ptr_nonnull(cm->nc);
	cm->nc->nc_exchgid_flags = exchgid_flags;
	c->c_nfs4_client = nfs4_client_get(cm->nc);
	c->c_gss_principal = gss_principal;

	cm->rt.rt_compound = c;
	cm->compound = c;
	return cm;
}

static void pr_free(struct pr_ctx *cm)
{
	if (!cm)
		return;
	struct compound *c = cm->compound;

	if (c) {
		/*
		 * pr_alloc only sets c_server_state and c_nfs4_client;
		 * the other fields (c_inode, c_curr_sb, c_saved_sb,
		 * c_curr_stid, c_saved_stid) are NULL.  The put helpers
		 * are NULL-safe per .claude/standards.md, so calling
		 * them unconditionally future-proofs the mock.
		 */
		server_state_put(c->c_server_state);
		inode_active_put(c->c_inode);
		super_block_put(c->c_curr_sb);
		super_block_put(c->c_saved_sb);
		stateid_put(c->c_curr_stid);
		stateid_put(c->c_saved_stid);
		if (c->c_nfs4_client)
			nfs4_client_put(c->c_nfs4_client);
		if (c->c_args) {
			free(c->c_args->argarray.argarray_val);
			free(c->c_args);
		}
		if (c->c_res) {
			free(c->c_res->resarray.resarray_val);
			free(c->c_res);
		}
		free(c);
	}
	if (cm->nc)
		nfs4_client_put(cm->nc);
	free(cm);
}

static PROXY_REGISTRATION4args *pr_args(struct pr_ctx *cm)
{
	return &cm->compound->c_args->argarray.argarray_val[0]
			.nfs_argop4_u.opproxy_registration;
}

static PROXY_REGISTRATION4res *pr_res(struct pr_ctx *cm)
{
	return &cm->compound->c_res->resarray.resarray_val[0]
			.nfs_resop4_u.opproxy_registration;
}

/*
 * Seed the singleton server_state's [[allowed_ps]] mirror with one
 * principal.  Slice 6b-i flips the default to deny-all, so any test
 * that wants the handler to ACCEPT a GSS principal must call this
 * first.  Calling with NULL clears the list.
 */
static void pr_allowlist_set(const char *principal)
{
	struct server_state *ss = server_state_find();

	ck_assert_ptr_nonnull(ss);
	ss->ss_nallowed_ps = 0;
	if (principal) {
		strncpy(ss->ss_allowed_ps[0], principal,
			REFFS_CONFIG_MAX_PRINCIPAL - 1);
		ss->ss_allowed_ps[0][REFFS_CONFIG_MAX_PRINCIPAL - 1] = '\0';
		ss->ss_nallowed_ps = 1;
	}
	server_state_put(ss);
}

/*
 * Slice 6b-iv: same as pr_allowlist_set but for the TLS-fingerprint
 * column of the allowlist.  An entry sets EITHER principal OR
 * fingerprint, not both -- the entry binds to one identity context.
 * Passing NULL clears the entire list.
 */
static void pr_allowlist_set_fingerprint(const char *fingerprint)
{
	struct server_state *ss = server_state_find();

	ck_assert_ptr_nonnull(ss);
	ss->ss_nallowed_ps = 0;
	if (fingerprint) {
		ss->ss_allowed_ps[0][0] = '\0';
		strncpy(ss->ss_allowed_ps_tls_fingerprint[0], fingerprint,
			REFFS_CONFIG_MAX_TLS_FINGERPRINT - 1);
		ss->ss_allowed_ps_tls_fingerprint
			[0][REFFS_CONFIG_MAX_TLS_FINGERPRINT - 1] = '\0';
		ss->ss_nallowed_ps = 1;
	}
	server_state_put(ss);
}

static void setup(void)
{
	nfs4_test_setup();
}

static void teardown(void)
{
	/*
	 * Reset allowlist before tearing down so a test's allowlist
	 * does not bleed into the next test (the singleton server_state
	 * is recreated by nfs4_test_teardown but explicit reset is
	 * cheap insurance).
	 */
	struct server_state *ss = server_state_find();

	if (ss) {
		ss->ss_nallowed_ps = 0;
		server_state_put(ss);
	}
	nfs4_test_teardown();
}

/* ------------------------------------------------------------------ */
/* PROXY_REGISTRATION                                                  */
/* ------------------------------------------------------------------ */

/*
 * Happy path: USE_NON_PNFS session + GSS principal on the allowlist
 * + zero prr_flags -> NFS4_OK and the client is marked registered.
 *
 * Slice 6b-i: pr_allowlist_set() is required because the default is
 * deny-all.  Slice 6a's version of this test did not need it.
 */
START_TEST(test_proxy_registration_success)
{
	pr_allowlist_set("host/ps.example.com@REALM");

	struct pr_ctx *cm = pr_alloc(EXCHGID4_FLAG_USE_NON_PNFS,
				     "host/ps.example.com@REALM");

	pr_args(cm)->prr_flags = 0;
	pr_args(cm)->prr_registration_id.prr_registration_id_len = 0;
	pr_args(cm)->prr_registration_id.prr_registration_id_val = NULL;

	nfs4_op_proxy_registration(cm->compound);

	ck_assert_int_eq(pr_res(cm)->prrr_status, NFS4_OK);
	ck_assert(cm->compound->c_nfs4_client->nc_is_registered_ps);

	pr_free(cm);
}
END_TEST

/*
 * RFC 8178 S4.4.3: any non-zero bit in the reserved prr_flags field
 * MUST yield NFS4ERR_INVAL.  Pin a few representative values; the
 * intent is "bit 0", "bit 31", and "all bits" each rejected the same
 * way.
 */
START_TEST(test_proxy_registration_reject_bad_prr_flags)
{
	const uint32_t bad_values[] = { 0x1, 0x80000000U, 0xFFFFFFFFU };

	for (size_t i = 0; i < sizeof(bad_values) / sizeof(bad_values[0]);
	     i++) {
		struct pr_ctx *cm = pr_alloc(EXCHGID4_FLAG_USE_NON_PNFS,
					     "host/ps.example.com@REALM");

		pr_args(cm)->prr_flags = bad_values[i];
		pr_args(cm)->prr_registration_id.prr_registration_id_len = 0;

		nfs4_op_proxy_registration(cm->compound);

		ck_assert_int_eq(pr_res(cm)->prrr_status, NFS4ERR_INVAL);
		ck_assert(!cm->compound->c_nfs4_client->nc_is_registered_ps);
		pr_free(cm);
	}
}
END_TEST

/*
 * Per data-mover draft sec-PROXY_REGISTRATION the carrying session
 * MUST have been created with EXCHGID4_FLAG_USE_NON_PNFS (the PS is
 * a regular NFSv4 client of the MDS, not a peer MDS or DS).  A
 * session whose EXCHANGE_ID set USE_PNFS_MDS / USE_PNFS_DS is
 * rejected with NFS4ERR_PERM.
 */
START_TEST(test_proxy_registration_rejects_without_use_non_pnfs)
{
	struct pr_ctx *cm = pr_alloc(EXCHGID4_FLAG_USE_PNFS_MDS,
				     "host/ps.example.com@REALM");

	pr_args(cm)->prr_flags = 0;
	pr_args(cm)->prr_registration_id.prr_registration_id_len = 0;

	nfs4_op_proxy_registration(cm->compound);

	ck_assert_int_eq(pr_res(cm)->prrr_status, NFS4ERR_PERM);
	ck_assert(!cm->compound->c_nfs4_client->nc_is_registered_ps);
	pr_free(cm);
}
END_TEST

/*
 * The data-mover draft (sec-security) forbids AUTH_SYS on the
 * MDS<->PS session.  Slice 6a approximates that with a c_gss_principal
 * NULL check -- catches AUTH_SYS, also (intentionally too-strict)
 * rejects a TLS-only session.  Slice 6b adds the TLS auth-context
 * check.
 */
START_TEST(test_proxy_registration_rejects_auth_sys_session)
{
	struct pr_ctx *cm = pr_alloc(EXCHGID4_FLAG_USE_NON_PNFS,
				     NULL /* AUTH_SYS: no GSS principal */);

	pr_args(cm)->prr_flags = 0;
	pr_args(cm)->prr_registration_id.prr_registration_id_len = 0;

	nfs4_op_proxy_registration(cm->compound);

	ck_assert_int_eq(pr_res(cm)->prrr_status, NFS4ERR_PERM);
	ck_assert(!cm->compound->c_nfs4_client->nc_is_registered_ps);
	pr_free(cm);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Allowlist checks (slice 6b-i)                                       */
/* ------------------------------------------------------------------ */

/*
 * GSS-authenticated session whose principal is NOT on the allowlist
 * is rejected with NFS4ERR_PERM.  The allowlist is the operator-
 * curated list of identities permitted to act as a Proxy Server;
 * anyone else gets the same answer as AUTH_SYS.
 */
START_TEST(test_proxy_registration_reject_not_allowlisted)
{
	pr_allowlist_set("host/ps.example.com@REALM");

	struct pr_ctx *cm = pr_alloc(EXCHGID4_FLAG_USE_NON_PNFS,
				     "host/intruder.example.com@REALM");

	pr_args(cm)->prr_flags = 0;
	pr_args(cm)->prr_registration_id.prr_registration_id_len = 0;

	nfs4_op_proxy_registration(cm->compound);

	ck_assert_int_eq(pr_res(cm)->prrr_status, NFS4ERR_PERM);
	ck_assert(!cm->compound->c_nfs4_client->nc_is_registered_ps);
	pr_free(cm);
}
END_TEST

/*
 * Default deny: an empty allowlist rejects every PROXY_REGISTRATION,
 * even one with a syntactically valid GSS principal.  This is the
 * secure default an admin gets if [[allowed_ps]] is omitted from the
 * config.
 */
START_TEST(test_proxy_registration_reject_empty_allowlist)
{
	pr_allowlist_set(NULL); /* zero entries */

	struct pr_ctx *cm = pr_alloc(EXCHGID4_FLAG_USE_NON_PNFS,
				     "host/ps.example.com@REALM");

	pr_args(cm)->prr_flags = 0;
	pr_args(cm)->prr_registration_id.prr_registration_id_len = 0;

	nfs4_op_proxy_registration(cm->compound);

	ck_assert_int_eq(pr_res(cm)->prrr_status, NFS4ERR_PERM);
	ck_assert(!cm->compound->c_nfs4_client->nc_is_registered_ps);
	pr_free(cm);
}
END_TEST

/*
 * Exact-string match -- realm differences MUST cause a reject.
 * Allowlist entry is "host/ps.example.com@REALM"; an attacker cannot
 * substitute "host/ps.example.com@OTHER" and slip through.  Pinned
 * here because Kerberos display names look interchangeable to a
 * casual reader and a future "case-insensitive realm" change would
 * silently widen the trust boundary.
 */
START_TEST(test_proxy_registration_principal_exact_match)
{
	pr_allowlist_set("host/ps.example.com@REALM");

	struct pr_ctx *cm = pr_alloc(EXCHGID4_FLAG_USE_NON_PNFS,
				     "host/ps.example.com@OTHER");

	pr_args(cm)->prr_flags = 0;
	pr_args(cm)->prr_registration_id.prr_registration_id_len = 0;

	nfs4_op_proxy_registration(cm->compound);

	ck_assert_int_eq(pr_res(cm)->prrr_status, NFS4ERR_PERM);
	ck_assert(!cm->compound->c_nfs4_client->nc_is_registered_ps);
	pr_free(cm);
}
END_TEST

/*
 * Allowlisted GSS principal accepted.  Mirror of the success test
 * but pinned as a separate case so a regression in the allowlist
 * code path is visibly distinct from a regression in the slice-6a
 * baseline.
 */
START_TEST(test_proxy_registration_accept_allowlisted)
{
	pr_allowlist_set("host/ps.example.com@REALM");

	struct pr_ctx *cm = pr_alloc(EXCHGID4_FLAG_USE_NON_PNFS,
				     "host/ps.example.com@REALM");

	pr_args(cm)->prr_flags = 0;
	pr_args(cm)->prr_registration_id.prr_registration_id_len = 0;

	nfs4_op_proxy_registration(cm->compound);

	ck_assert_int_eq(pr_res(cm)->prrr_status, NFS4_OK);
	ck_assert(cm->compound->c_nfs4_client->nc_is_registered_ps);
	pr_free(cm);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Squat-guard / renewal / lease (slice 6b-iii)                        */
/* ------------------------------------------------------------------ */

/*
 * Helper: send PROXY_REGISTRATION on the given context with the given
 * registration_id (NULL/0 for an empty id).  Caller owns cm and
 * checks status / the nc_is_registered_ps flag itself.
 */
static void pr_send(struct pr_ctx *cm, const char *reg_id, uint32_t reg_id_len)
{
	pr_args(cm)->prr_flags = 0;
	pr_args(cm)->prr_registration_id.prr_registration_id_len = reg_id_len;
	pr_args(cm)->prr_registration_id.prr_registration_id_val =
		(char *)reg_id;
	nfs4_op_proxy_registration(cm->compound);
}

/*
 * A second PROXY_REGISTRATION from a different session (different
 * clientid) but the same GSS principal AND the same registration_id
 * is a renewal.  Both clients end up registered; the original
 * client's lease is bumped on the renewal so its still-connected
 * session does not lose privilege between "renewed on new session"
 * and "old session times out on its own".
 */
START_TEST(test_proxy_registration_renewal_same_id)
{
	const char id[] = "ps-id-aaaa-bbbb";

	pr_allowlist_set("host/ps.example.com@REALM");

	struct pr_ctx *first = pr_alloc(EXCHGID4_FLAG_USE_NON_PNFS,
					"host/ps.example.com@REALM");

	pr_send(first, id, sizeof(id) - 1);
	ck_assert_int_eq(pr_res(first)->prrr_status, NFS4_OK);
	ck_assert(first->compound->c_nfs4_client->nc_is_registered_ps);
	uint64_t first_expire_after_register = atomic_load_explicit(
		&first->compound->c_nfs4_client->nc_ps_lease_expire_ns,
		memory_order_acquire);

	struct pr_ctx *second = pr_alloc(EXCHGID4_FLAG_USE_NON_PNFS,
					 "host/ps.example.com@REALM");

	pr_send(second, id, sizeof(id) - 1);
	ck_assert_int_eq(pr_res(second)->prrr_status, NFS4_OK);
	ck_assert(second->compound->c_nfs4_client->nc_is_registered_ps);

	uint64_t first_expire_after_renew = atomic_load_explicit(
		&first->compound->c_nfs4_client->nc_ps_lease_expire_ns,
		memory_order_acquire);
	ck_assert_uint_ge(first_expire_after_renew,
			  first_expire_after_register);

	pr_free(second);
	pr_free(first);
}
END_TEST

/*
 * A second PROXY_REGISTRATION from a different session, same GSS
 * principal, but DIFFERENT registration_id is a squat attempt.  The
 * MDS responds with NFS4ERR_DELAY and refuses to grant the privilege.
 * The original client's grant remains intact.
 */
START_TEST(test_proxy_registration_squat_blocked)
{
	const char id_a[] = "ps-id-aaaa-bbbb";
	const char id_b[] = "ps-id-cccc-dddd";

	pr_allowlist_set("host/ps.example.com@REALM");

	struct pr_ctx *first = pr_alloc(EXCHGID4_FLAG_USE_NON_PNFS,
					"host/ps.example.com@REALM");

	pr_send(first, id_a, sizeof(id_a) - 1);
	ck_assert_int_eq(pr_res(first)->prrr_status, NFS4_OK);

	struct pr_ctx *second = pr_alloc(EXCHGID4_FLAG_USE_NON_PNFS,
					 "host/ps.example.com@REALM");

	pr_send(second, id_b, sizeof(id_b) - 1);

	ck_assert_int_eq(pr_res(second)->prrr_status, NFS4ERR_DELAY);
	ck_assert(!second->compound->c_nfs4_client->nc_is_registered_ps);
	ck_assert(first->compound->c_nfs4_client->nc_is_registered_ps);

	pr_free(second);
	pr_free(first);
}
END_TEST

/*
 * Lease-expired-then-fresh: after the original PS's lease expires,
 * a second registration with a DIFFERENT id is no longer a squat and
 * succeeds.  Simulated by stomping the lease_expire field to 1 (well
 * in the past) -- production code would wait the actual lease period.
 */
START_TEST(test_proxy_registration_after_expiry_succeeds)
{
	const char id_a[] = "ps-id-aaaa-bbbb";
	const char id_b[] = "ps-id-cccc-dddd";

	pr_allowlist_set("host/ps.example.com@REALM");

	struct pr_ctx *first = pr_alloc(EXCHGID4_FLAG_USE_NON_PNFS,
					"host/ps.example.com@REALM");

	pr_send(first, id_a, sizeof(id_a) - 1);
	ck_assert_int_eq(pr_res(first)->prrr_status, NFS4_OK);

	/* Force the first client's lease to be expired. */
	atomic_store_explicit(
		&first->compound->c_nfs4_client->nc_ps_lease_expire_ns, 1,
		memory_order_release);

	struct pr_ctx *second = pr_alloc(EXCHGID4_FLAG_USE_NON_PNFS,
					 "host/ps.example.com@REALM");

	pr_send(second, id_b, sizeof(id_b) - 1);

	ck_assert_int_eq(pr_res(second)->prrr_status, NFS4_OK);
	ck_assert(second->compound->c_nfs4_client->nc_is_registered_ps);

	pr_free(second);
	pr_free(first);
}
END_TEST

/* ------------------------------------------------------------------ */
/* mTLS auth-context (slice 6b-iv)                                     */
/* ------------------------------------------------------------------ */

/*
 * A TLS-only session whose peer cert SHA-256 fingerprint is on the
 * tls_cert_fingerprint allowlist is permitted, even with no GSS
 * principal.  Mocks the production wiring (which is NOT_NOW_BROWN_COW
 * alongside the matching c_gss_principal wiring) by setting
 * c_tls_fingerprint directly.
 */
START_TEST(test_proxy_registration_accept_tls_fingerprint)
{
	const char fp[] = "AB:CD:EF:01:23:45:67:89:AB:CD:EF:01:23:45:67:89:"
			  "AB:CD:EF:01:23:45:67:89:AB:CD:EF:01:23:45:67:89";

	pr_allowlist_set_fingerprint(fp);

	struct pr_ctx *cm =
		pr_alloc(EXCHGID4_FLAG_USE_NON_PNFS,
			 NULL /* no GSS principal -- TLS-only context */);

	cm->compound->c_tls_fingerprint = fp;

	pr_args(cm)->prr_flags = 0;
	pr_args(cm)->prr_registration_id.prr_registration_id_len = 0;

	nfs4_op_proxy_registration(cm->compound);

	ck_assert_int_eq(pr_res(cm)->prrr_status, NFS4_OK);
	ck_assert(cm->compound->c_nfs4_client->nc_is_registered_ps);
	pr_free(cm);
}
END_TEST

/*
 * TLS context present but the fingerprint is NOT on the allowlist.
 * The session has no other identity to fall back on, so the handler
 * rejects with NFS4ERR_PERM -- mirrors the principal-not-allowlisted
 * case from slice 6b-i.
 */
START_TEST(test_proxy_registration_reject_tls_fingerprint_not_allowlisted)
{
	const char allowed_fp[] =
		"AB:CD:EF:01:23:45:67:89:AB:CD:EF:01:23:45:67:89:"
		"AB:CD:EF:01:23:45:67:89:AB:CD:EF:01:23:45:67:89";
	const char wrong_fp[] =
		"FF:FF:FF:01:23:45:67:89:AB:CD:EF:01:23:45:67:89:"
		"AB:CD:EF:01:23:45:67:89:AB:CD:EF:01:23:45:67:89";

	pr_allowlist_set_fingerprint(allowed_fp);

	struct pr_ctx *cm = pr_alloc(EXCHGID4_FLAG_USE_NON_PNFS, NULL);

	cm->compound->c_tls_fingerprint = wrong_fp;

	pr_args(cm)->prr_flags = 0;
	pr_args(cm)->prr_registration_id.prr_registration_id_len = 0;

	nfs4_op_proxy_registration(cm->compound);

	ck_assert_int_eq(pr_res(cm)->prrr_status, NFS4ERR_PERM);
	ck_assert(!cm->compound->c_nfs4_client->nc_is_registered_ps);
	pr_free(cm);
}
END_TEST

/*
 * Both identity contexts NULL (AUTH_SYS over plain TCP).  The slice
 * 6a behaviour MUST be preserved: NFS4ERR_PERM.  This test pins the
 * "if BOTH NULL reject" gate so a future cleanup that accidentally
 * collapses the two checks into "if ANY non-NULL accept" gets caught.
 */
START_TEST(test_proxy_registration_reject_both_contexts_null)
{
	pr_allowlist_set_fingerprint("does-not-matter");

	struct pr_ctx *cm = pr_alloc(EXCHGID4_FLAG_USE_NON_PNFS, NULL);
	/* c_tls_fingerprint stays NULL (calloc default). */

	pr_args(cm)->prr_flags = 0;
	pr_args(cm)->prr_registration_id.prr_registration_id_len = 0;

	nfs4_op_proxy_registration(cm->compound);

	ck_assert_int_eq(pr_res(cm)->prrr_status, NFS4ERR_PERM);
	ck_assert(!cm->compound->c_nfs4_client->nc_is_registered_ps);
	pr_free(cm);
}
END_TEST

/* ------------------------------------------------------------------ */
/* PROXY_PROGRESS (slice 6a stub)                                      */
/* ------------------------------------------------------------------ */

/*
 * Slice 6a: the handler is a NFS4ERR_NOTSUPP stub.  Pin that the
 * stub responds, doesn't crash, and doesn't accidentally inherit
 * any other status.
 */
START_TEST(test_proxy_progress_returns_notsupp)
{
	struct pr_ctx *cm = pr_alloc(EXCHGID4_FLAG_USE_NON_PNFS,
				     "host/ps.example.com@REALM");

	cm->compound->c_args->argarray.argarray_val[0].argop =
		OP_PROXY_PROGRESS;
	cm->compound->c_res->resarray.resarray_val[0].resop = OP_PROXY_PROGRESS;

	nfs4_op_proxy_progress(cm->compound);

	PROXY_PROGRESS4res *res = &cm->compound->c_res->resarray.resarray_val[0]
					   .nfs_resop4_u.opproxy_progress;

	ck_assert_int_eq(res->ppr_status, NFS4ERR_NOTSUPP);
	pr_free(cm);
}
END_TEST

/*
 * Slice 6c-x.0: nfs4_client_registered_ps_identity accessor.
 * The accessor returns the canonical authorization principal for
 * subsequent migration-record owner_reg matching: registration_id
 * if non-empty, else GSS principal, else mTLS fingerprint, else NULL.
 */
START_TEST(test_registered_ps_identity_unregistered)
{
	struct pr_ctx *cm = pr_alloc(EXCHGID4_FLAG_USE_NON_PNFS,
				     "host/ps.example.com@REALM");
	uint32_t len = 999;
	const char *id;

	id = nfs4_client_registered_ps_identity(cm->compound->c_nfs4_client,
						&len);
	ck_assert_ptr_null(id);
	ck_assert_uint_eq(len, 999); /* untouched on miss */
	pr_free(cm);
}
END_TEST

START_TEST(test_registered_ps_identity_prefers_registration_id)
{
	struct pr_ctx *cm = pr_alloc(EXCHGID4_FLAG_USE_NON_PNFS,
				     "host/ps.example.com@REALM");
	struct nfs4_client *nc = cm->compound->c_nfs4_client;
	const char id_bytes[] = { 0xde, 0xad, 0xbe, 0xef, 0x42 };
	uint32_t len = 0;
	const char *out;

	memcpy(nc->nc_ps_registration_id, id_bytes, sizeof(id_bytes));
	nc->nc_ps_registration_id_len = sizeof(id_bytes);
	strncpy(nc->nc_ps_principal, "host/other@REALM",
		sizeof(nc->nc_ps_principal) - 1);
	atomic_store_explicit(&nc->nc_is_registered_ps, true,
			      memory_order_release);

	out = nfs4_client_registered_ps_identity(nc, &len);
	ck_assert_ptr_eq(out, nc->nc_ps_registration_id);
	ck_assert_uint_eq(len, sizeof(id_bytes));
	pr_free(cm);
}
END_TEST

START_TEST(test_registered_ps_identity_falls_back_to_principal)
{
	struct pr_ctx *cm = pr_alloc(EXCHGID4_FLAG_USE_NON_PNFS,
				     "host/ps.example.com@REALM");
	struct nfs4_client *nc = cm->compound->c_nfs4_client;
	uint32_t len = 0;
	const char *out;

	nc->nc_ps_registration_id_len = 0;
	strncpy(nc->nc_ps_principal, "host/ps.example.com@REALM",
		sizeof(nc->nc_ps_principal) - 1);
	atomic_store_explicit(&nc->nc_is_registered_ps, true,
			      memory_order_release);

	out = nfs4_client_registered_ps_identity(nc, &len);
	ck_assert_ptr_eq(out, nc->nc_ps_principal);
	ck_assert_uint_eq(len, (uint32_t)strlen(nc->nc_ps_principal));
	pr_free(cm);
}
END_TEST

START_TEST(test_registered_ps_identity_falls_back_to_tls)
{
	struct pr_ctx *cm = pr_alloc(EXCHGID4_FLAG_USE_NON_PNFS,
				     "host/ps.example.com@REALM");
	struct nfs4_client *nc = cm->compound->c_nfs4_client;
	uint32_t len = 0;
	const char *out;

	nc->nc_ps_registration_id_len = 0;
	nc->nc_ps_principal[0] = '\0';
	strncpy(nc->nc_ps_tls_fingerprint, "sha256:aabbccdd",
		sizeof(nc->nc_ps_tls_fingerprint) - 1);
	atomic_store_explicit(&nc->nc_is_registered_ps, true,
			      memory_order_release);

	out = nfs4_client_registered_ps_identity(nc, &len);
	ck_assert_ptr_eq(out, nc->nc_ps_tls_fingerprint);
	ck_assert_uint_eq(len, (uint32_t)strlen(nc->nc_ps_tls_fingerprint));
	pr_free(cm);
}
END_TEST

/*
 * Two clients with the same prr_registration_id (and otherwise
 * different fields) compare as equal.  This is the load-bearing
 * property for the squat-guard / cross-reconnect-DONE/CANCEL flow:
 * a PS that drops its session and reconnects with a fresh
 * EXCHANGE_ID retains authority over its own in-flight migrations.
 */
START_TEST(test_registered_ps_identity_eq_same_registration_id)
{
	struct pr_ctx *cm_a = pr_alloc(EXCHGID4_FLAG_USE_NON_PNFS,
				       "host/ps.example.com@REALM");
	struct pr_ctx *cm_b =
		pr_alloc(EXCHGID4_FLAG_USE_NON_PNFS, "host/different@REALM");
	struct nfs4_client *a = cm_a->compound->c_nfs4_client;
	struct nfs4_client *b = cm_b->compound->c_nfs4_client;
	const char id_bytes[] = "ps-instance-7";

	memcpy(a->nc_ps_registration_id, id_bytes, sizeof(id_bytes));
	a->nc_ps_registration_id_len = sizeof(id_bytes);
	memcpy(b->nc_ps_registration_id, id_bytes, sizeof(id_bytes));
	b->nc_ps_registration_id_len = sizeof(id_bytes);
	atomic_store_explicit(&a->nc_is_registered_ps, true,
			      memory_order_release);
	atomic_store_explicit(&b->nc_is_registered_ps, true,
			      memory_order_release);

	ck_assert(nfs4_client_registered_ps_identity_eq(a, b));
	pr_free(cm_a);
	pr_free(cm_b);
}
END_TEST

START_TEST(test_registered_ps_identity_eq_different_principal)
{
	struct pr_ctx *cm_a =
		pr_alloc(EXCHGID4_FLAG_USE_NON_PNFS, "host/ps-a@REALM");
	struct pr_ctx *cm_b =
		pr_alloc(EXCHGID4_FLAG_USE_NON_PNFS, "host/ps-b@REALM");
	struct nfs4_client *a = cm_a->compound->c_nfs4_client;
	struct nfs4_client *b = cm_b->compound->c_nfs4_client;

	a->nc_ps_registration_id_len = 0;
	b->nc_ps_registration_id_len = 0;
	strncpy(a->nc_ps_principal, "host/ps-a@REALM",
		sizeof(a->nc_ps_principal) - 1);
	strncpy(b->nc_ps_principal, "host/ps-b@REALM",
		sizeof(b->nc_ps_principal) - 1);
	atomic_store_explicit(&a->nc_is_registered_ps, true,
			      memory_order_release);
	atomic_store_explicit(&b->nc_is_registered_ps, true,
			      memory_order_release);

	ck_assert(!nfs4_client_registered_ps_identity_eq(a, b));
	pr_free(cm_a);
	pr_free(cm_b);
}
END_TEST

/*
 * Absence of identity is NOT a match: two unregistered clients (or
 * one registered + one unregistered) compare as unequal.  This
 * prevents a broken auth path from accidentally granting a default
 * "any unregistered client matches any other" rule.
 */
START_TEST(test_registered_ps_identity_eq_both_unregistered)
{
	struct pr_ctx *cm_a =
		pr_alloc(EXCHGID4_FLAG_USE_NON_PNFS, "host/x@REALM");
	struct pr_ctx *cm_b =
		pr_alloc(EXCHGID4_FLAG_USE_NON_PNFS, "host/y@REALM");

	ck_assert(!nfs4_client_registered_ps_identity_eq(
		cm_a->compound->c_nfs4_client, cm_b->compound->c_nfs4_client));
	pr_free(cm_a);
	pr_free(cm_b);
}
END_TEST

/* ------------------------------------------------------------------ */
/* PROXY_DONE / PROXY_CANCEL handlers (slice 6c-x.3)                   */
/* ------------------------------------------------------------------ */

/*
 * Helper: stamp the calling client as a registered PS with the given
 * canonical identity bytes.  Mirrors what proxy_registration.c does on
 * a successful PROXY_REGISTRATION but lets tests skip the wire-format
 * dance.
 */
static void register_ps_identity(struct nfs4_client *nc, const char *id_bytes,
				 uint32_t id_len)
{
	memcpy(nc->nc_ps_registration_id, id_bytes, id_len);
	nc->nc_ps_registration_id_len = id_len;
	atomic_store_explicit(&nc->nc_is_registered_ps, true,
			      memory_order_release);
}

/*
 * Helper: prep the compound to dispatch PROXY_DONE.  Sets the op slot,
 * arms args, and parks PUTFH-equivalent state on c_curr_nfh so the FH
 * matches the migration record.
 */
static PROXY_DONE4args *pd_setup(struct pr_ctx *cm, const stateid4 *stid,
				 nfsstat4 status, uint64_t ino)
{
	cm->compound->c_args->argarray.argarray_val[0].argop = OP_PROXY_DONE;
	cm->compound->c_res->resarray.resarray_val[0].resop = OP_PROXY_DONE;
	PROXY_DONE4args *args = &cm->compound->c_args->argarray.argarray_val[0]
					 .nfs_argop4_u.opproxy_done;

	args->pd_stateid = *stid;
	args->pd_status = status;
	cm->compound->c_curr_nfh.nfh_ino = ino;
	cm->compound->c_curr_sb = NULL; /* validate skips sb cmp when NULL */
	return args;
}

static PROXY_CANCEL4args *pc_setup(struct pr_ctx *cm, const stateid4 *stid,
				   uint64_t ino)
{
	cm->compound->c_args->argarray.argarray_val[0].argop = OP_PROXY_CANCEL;
	cm->compound->c_res->resarray.resarray_val[0].resop = OP_PROXY_CANCEL;
	PROXY_CANCEL4args *args =
		&cm->compound->c_args->argarray.argarray_val[0]
			 .nfs_argop4_u.opproxy_cancel;

	args->pc_stateid = *stid;
	cm->compound->c_curr_nfh.nfh_ino = ino;
	cm->compound->c_curr_sb = NULL;
	return args;
}

static PROXY_DONE4res *pd_res(struct pr_ctx *cm)
{
	return &cm->compound->c_res->resarray.resarray_val[0]
			.nfs_resop4_u.opproxy_done;
}

static PROXY_CANCEL4res *pc_res(struct pr_ctx *cm)
{
	return &cm->compound->c_res->resarray.resarray_val[0]
			.nfs_resop4_u.opproxy_cancel;
}

START_TEST(test_proxy_done_not_registered_returns_perm)
{
	stateid4 stid;

	ck_assert_int_eq(proxy_stateid_alloc(0x0001, &stid), 0);

	struct pr_ctx *cm =
		pr_alloc(EXCHGID4_FLAG_USE_NON_PNFS, "host/ps@REALM");

	pd_setup(cm, &stid, NFS4_OK, 5001);
	nfs4_op_proxy_done(cm->compound);
	ck_assert_int_eq(pd_res(cm)->pdr_status, NFS4ERR_PERM);
	pr_free(cm);
}
END_TEST

START_TEST(test_proxy_done_stale_boot_returns_stale_stateid)
{
	stateid4 stid;
	struct pr_ctx *cm =
		pr_alloc(EXCHGID4_FLAG_USE_NON_PNFS, "host/ps@REALM");

	register_ps_identity(cm->compound->c_nfs4_client, "ps", 2);
	ck_assert_int_eq(migration_record_init(), 0);

	/*
	 * Mint a stateid for a boot_seq that's not the current
	 * server's boot_seq.  The current server's boot_seq is
	 * non-zero (it bumps on every init); using 0 guarantees a
	 * mismatch.
	 */
	ck_assert_int_eq(proxy_stateid_alloc(0xFFFE, &stid), 0);
	pd_setup(cm, &stid, NFS4_OK, 5002);
	nfs4_op_proxy_done(cm->compound);
	ck_assert_int_eq(pd_res(cm)->pdr_status, NFS4ERR_STALE_STATEID);

	migration_record_fini();
	pr_free(cm);
}
END_TEST

START_TEST(test_proxy_done_unknown_stateid_returns_bad_stateid)
{
	stateid4 stid;
	struct pr_ctx *cm =
		pr_alloc(EXCHGID4_FLAG_USE_NON_PNFS, "host/ps@REALM");
	struct server_state *ss = cm->compound->c_server_state;

	register_ps_identity(cm->compound->c_nfs4_client, "ps", 2);
	ck_assert_int_eq(migration_record_init(), 0);

	ck_assert_int_eq(proxy_stateid_alloc(server_boot_seq(ss), &stid), 0);
	/* No record for this stateid -> NFS4ERR_BAD_STATEID. */
	pd_setup(cm, &stid, NFS4_OK, 5003);
	nfs4_op_proxy_done(cm->compound);
	ck_assert_int_eq(pd_res(cm)->pdr_status, NFS4ERR_BAD_STATEID);

	migration_record_fini();
	pr_free(cm);
}
END_TEST

START_TEST(test_proxy_done_owner_mismatch_returns_perm)
{
	stateid4 stid;
	struct pr_ctx *cm =
		pr_alloc(EXCHGID4_FLAG_USE_NON_PNFS, "host/ps@REALM");
	struct server_state *ss = cm->compound->c_server_state;
	struct migration_record *mr = NULL;

	register_ps_identity(cm->compound->c_nfs4_client, "ps-A", 4);
	ck_assert_int_eq(migration_record_init(), 0);

	ck_assert_int_eq(proxy_stateid_alloc(server_boot_seq(ss), &stid), 0);
	/* Create record OWNED BY ps-B. */
	ck_assert_int_eq(migration_record_create(&stid, NULL, 5004, "ps-B", 4,
						 NULL, 0, 1, &mr),
			 0);
	pd_setup(cm, &stid, NFS4_OK, 5004);
	nfs4_op_proxy_done(cm->compound);
	ck_assert_int_eq(pd_res(cm)->pdr_status, NFS4ERR_PERM);
	/* Record stays intact (find returns it). */
	struct migration_record *found =
		migration_record_find_by_stateid(&stid);

	ck_assert_ptr_eq(found, mr);
	migration_record_put(found);
	migration_record_abandon(mr);
	migration_record_fini();
	pr_free(cm);
}
END_TEST

START_TEST(test_proxy_done_file_mismatch_returns_bad_stateid)
{
	stateid4 stid;
	struct pr_ctx *cm =
		pr_alloc(EXCHGID4_FLAG_USE_NON_PNFS, "host/ps@REALM");
	struct server_state *ss = cm->compound->c_server_state;
	struct migration_record *mr = NULL;

	register_ps_identity(cm->compound->c_nfs4_client, "ps", 2);
	ck_assert_int_eq(migration_record_init(), 0);

	ck_assert_int_eq(proxy_stateid_alloc(server_boot_seq(ss), &stid), 0);
	ck_assert_int_eq(migration_record_create(&stid, NULL, 5005, "ps", 2,
						 NULL, 0, 1, &mr),
			 0);
	/* PUTFH a DIFFERENT file. */
	pd_setup(cm, &stid, NFS4_OK, 9999);
	nfs4_op_proxy_done(cm->compound);
	ck_assert_int_eq(pd_res(cm)->pdr_status, NFS4ERR_BAD_STATEID);

	migration_record_abandon(mr);
	migration_record_fini();
	pr_free(cm);
}
END_TEST

START_TEST(test_proxy_done_old_seqid_returns_old_stateid)
{
	stateid4 stid;
	struct pr_ctx *cm =
		pr_alloc(EXCHGID4_FLAG_USE_NON_PNFS, "host/ps@REALM");
	struct server_state *ss = cm->compound->c_server_state;
	struct migration_record *mr = NULL;

	register_ps_identity(cm->compound->c_nfs4_client, "ps", 2);
	ck_assert_int_eq(migration_record_init(), 0);

	ck_assert_int_eq(proxy_stateid_alloc(server_boot_seq(ss), &stid), 0);
	ck_assert_int_eq(migration_record_create(&stid, NULL, 5006, "ps", 2,
						 NULL, 0, 1, &mr),
			 0);
	/*
	 * Caller presents an OLDER seqid.  The record was created with
	 * stid->seqid (1); presenting 0 mimics a stale renewal slot.
	 */
	stid.seqid = 0;
	pd_setup(cm, &stid, NFS4_OK, 5006);
	nfs4_op_proxy_done(cm->compound);
	ck_assert_int_eq(pd_res(cm)->pdr_status, NFS4ERR_OLD_STATEID);

	migration_record_abandon(mr);
	migration_record_fini();
	pr_free(cm);
}
END_TEST

START_TEST(test_proxy_done_ok_commits_and_unhashes)
{
	stateid4 stid;
	struct pr_ctx *cm =
		pr_alloc(EXCHGID4_FLAG_USE_NON_PNFS, "host/ps@REALM");
	struct server_state *ss = cm->compound->c_server_state;
	struct migration_record *mr = NULL;

	register_ps_identity(cm->compound->c_nfs4_client, "ps", 2);
	ck_assert_int_eq(migration_record_init(), 0);

	ck_assert_int_eq(proxy_stateid_alloc(server_boot_seq(ss), &stid), 0);
	ck_assert_int_eq(migration_record_create(&stid, NULL, 5007, "ps", 2,
						 NULL, 0, 1, &mr),
			 0);
	pd_setup(cm, &stid, NFS4_OK, 5007);
	nfs4_op_proxy_done(cm->compound);
	ck_assert_int_eq(pd_res(cm)->pdr_status, NFS4_OK);

	/* Record is unhashed after commit. */
	ck_assert_ptr_null(migration_record_find_by_stateid(&stid));
	ck_assert_ptr_null(migration_record_find_by_inode(5007));

	migration_record_fini();
	pr_free(cm);
}
END_TEST

START_TEST(test_proxy_done_fail_abandons)
{
	stateid4 stid;
	struct pr_ctx *cm =
		pr_alloc(EXCHGID4_FLAG_USE_NON_PNFS, "host/ps@REALM");
	struct server_state *ss = cm->compound->c_server_state;
	struct migration_record *mr = NULL;

	register_ps_identity(cm->compound->c_nfs4_client, "ps", 2);
	ck_assert_int_eq(migration_record_init(), 0);

	ck_assert_int_eq(proxy_stateid_alloc(server_boot_seq(ss), &stid), 0);
	ck_assert_int_eq(migration_record_create(&stid, NULL, 5008, "ps", 2,
						 NULL, 0, 1, &mr),
			 0);
	pd_setup(cm, &stid, NFS4ERR_DELAY, 5008);
	nfs4_op_proxy_done(cm->compound);
	ck_assert_int_eq(pd_res(cm)->pdr_status, NFS4_OK);
	/* Record gone (abandon path). */
	ck_assert_ptr_null(migration_record_find_by_stateid(&stid));

	migration_record_fini();
	pr_free(cm);
}
END_TEST

START_TEST(test_proxy_cancel_abandons)
{
	stateid4 stid;
	struct pr_ctx *cm =
		pr_alloc(EXCHGID4_FLAG_USE_NON_PNFS, "host/ps@REALM");
	struct server_state *ss = cm->compound->c_server_state;
	struct migration_record *mr = NULL;

	register_ps_identity(cm->compound->c_nfs4_client, "ps", 2);
	ck_assert_int_eq(migration_record_init(), 0);

	ck_assert_int_eq(proxy_stateid_alloc(server_boot_seq(ss), &stid), 0);
	ck_assert_int_eq(migration_record_create(&stid, NULL, 5009, "ps", 2,
						 NULL, 0, 1, &mr),
			 0);
	pc_setup(cm, &stid, 5009);
	nfs4_op_proxy_cancel(cm->compound);
	ck_assert_int_eq(pc_res(cm)->pcr_status, NFS4_OK);
	ck_assert_ptr_null(migration_record_find_by_stateid(&stid));

	migration_record_fini();
	pr_free(cm);
}
END_TEST

START_TEST(test_proxy_done_idempotent_returns_bad_stateid_on_repeat)
{
	stateid4 stid;
	struct pr_ctx *cm =
		pr_alloc(EXCHGID4_FLAG_USE_NON_PNFS, "host/ps@REALM");
	struct server_state *ss = cm->compound->c_server_state;
	struct migration_record *mr = NULL;

	register_ps_identity(cm->compound->c_nfs4_client, "ps", 2);
	ck_assert_int_eq(migration_record_init(), 0);

	ck_assert_int_eq(proxy_stateid_alloc(server_boot_seq(ss), &stid), 0);
	ck_assert_int_eq(migration_record_create(&stid, NULL, 5010, "ps", 2,
						 NULL, 0, 1, &mr),
			 0);
	pd_setup(cm, &stid, NFS4_OK, 5010);
	nfs4_op_proxy_done(cm->compound);
	ck_assert_int_eq(pd_res(cm)->pdr_status, NFS4_OK);

	/*
	 * Second DONE: the record is gone (commit unhashed it), so the
	 * find step in proxy_record_validate returns NULL, surfaces as
	 * NFS4ERR_BAD_STATEID per the priority-ordered rule.
	 */
	pd_setup(cm, &stid, NFS4_OK, 5010);
	nfs4_op_proxy_done(cm->compound);
	ck_assert_int_eq(pd_res(cm)->pdr_status, NFS4ERR_BAD_STATEID);

	migration_record_fini();
	pr_free(cm);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

static Suite *proxy_registration_suite(void)
{
	Suite *s = suite_create("proxy_registration");
	TCase *tc = tcase_create("proxy_registration");

	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_proxy_registration_success);
	tcase_add_test(tc, test_proxy_registration_reject_bad_prr_flags);
	tcase_add_test(tc,
		       test_proxy_registration_rejects_without_use_non_pnfs);
	tcase_add_test(tc, test_proxy_registration_rejects_auth_sys_session);
	tcase_add_test(tc, test_proxy_registration_reject_not_allowlisted);
	tcase_add_test(tc, test_proxy_registration_reject_empty_allowlist);
	tcase_add_test(tc, test_proxy_registration_principal_exact_match);
	tcase_add_test(tc, test_proxy_registration_accept_allowlisted);
	tcase_add_test(tc, test_proxy_registration_renewal_same_id);
	tcase_add_test(tc, test_proxy_registration_squat_blocked);
	tcase_add_test(tc, test_proxy_registration_after_expiry_succeeds);
	tcase_add_test(tc, test_proxy_registration_accept_tls_fingerprint);
	tcase_add_test(
		tc,
		test_proxy_registration_reject_tls_fingerprint_not_allowlisted);
	tcase_add_test(tc, test_proxy_registration_reject_both_contexts_null);
	tcase_add_test(tc, test_proxy_progress_returns_notsupp);
	tcase_add_test(tc, test_registered_ps_identity_unregistered);
	tcase_add_test(tc, test_registered_ps_identity_prefers_registration_id);
	tcase_add_test(tc, test_registered_ps_identity_falls_back_to_principal);
	tcase_add_test(tc, test_registered_ps_identity_falls_back_to_tls);
	tcase_add_test(tc, test_registered_ps_identity_eq_same_registration_id);
	tcase_add_test(tc, test_registered_ps_identity_eq_different_principal);
	tcase_add_test(tc, test_registered_ps_identity_eq_both_unregistered);
	tcase_add_test(tc, test_proxy_done_not_registered_returns_perm);
	tcase_add_test(tc, test_proxy_done_stale_boot_returns_stale_stateid);
	tcase_add_test(tc, test_proxy_done_unknown_stateid_returns_bad_stateid);
	tcase_add_test(tc, test_proxy_done_owner_mismatch_returns_perm);
	tcase_add_test(tc, test_proxy_done_file_mismatch_returns_bad_stateid);
	tcase_add_test(tc, test_proxy_done_old_seqid_returns_old_stateid);
	tcase_add_test(tc, test_proxy_done_ok_commits_and_unhashes);
	tcase_add_test(tc, test_proxy_done_fail_abandons);
	tcase_add_test(tc, test_proxy_cancel_abandons);
	tcase_add_test(
		tc, test_proxy_done_idempotent_returns_bad_stateid_on_repeat);
	suite_add_tcase(s, tc);

	return s;
}

int main(void)
{
	return reffs_test_run_suite(proxy_registration_suite(), NULL, NULL);
}
