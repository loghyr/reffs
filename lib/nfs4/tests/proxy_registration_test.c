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
#include "nfs4/ops.h"
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

	ck_assert_int_eq(res->prar_status, NFS4ERR_NOTSUPP);
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
	tcase_add_test(tc, test_proxy_progress_returns_notsupp);
	suite_add_tcase(s, tc);

	return s;
}

int main(void)
{
	return reffs_test_run_suite(proxy_registration_suite(), NULL, NULL);
}
