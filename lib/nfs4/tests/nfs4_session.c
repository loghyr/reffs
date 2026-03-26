/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Unit tests for the NFSv4.1 session infrastructure:
 *
 *   - nfs4_session_alloc / nfs4_session_find / nfs4_session_unhash lifecycle
 *   - fore-channel attribute clamping
 *   - slot table initial state
 *   - multiple sessions per client have distinct session IDs
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <netinet/in.h>

#include <check.h>

#include "nfsv42_xdr.h"
#include "reffs/rcu.h"
#include "reffs/server.h"
#include "nfs4/client.h"
#include "nfs4/client_persist.h"
#include "nfs4/session.h"
#include "nfs4_test_harness.h"

/* ------------------------------------------------------------------ */
/* Test fixture                                                        */
/* ------------------------------------------------------------------ */

static struct server_state *g_ss;
static struct nfs4_client *g_nc;

static void make_owner(client_owner4 *owner, const char *id_str, char *buf,
		       size_t bufsz)
{
	size_t len = strlen(id_str);
	if (len >= bufsz)
		len = bufsz - 1;
	memcpy(buf, id_str, len);
	owner->co_ownerid.co_ownerid_val = buf;
	owner->co_ownerid.co_ownerid_len = (u_int)len;
}

static struct nfs_impl_id4 make_impl_id(void)
{
	static char domain[] = "example.com";
	static char name[] = "test-client";
	struct nfs_impl_id4 id = {
		.nii_domain = { .utf8string_val = domain,
				.utf8string_len = sizeof(domain) - 1 },
		.nii_name = { .utf8string_val = name,
			      .utf8string_len = sizeof(name) - 1 },
	};
	return id;
}

static channel_attrs4 make_fore_chan(uint32_t maxreq, uint32_t maxresp,
				     uint32_t maxresp_cached, uint32_t maxops,
				     uint32_t maxslots)
{
	channel_attrs4 ca = {
		.ca_headerpadsize = 0,
		.ca_maxrequestsize = maxreq,
		.ca_maxresponsesize = maxresp,
		.ca_maxresponsesize_cached = maxresp_cached,
		.ca_maxoperations = maxops,
		.ca_maxrequests = maxslots,
		.ca_rdma_ird = { .ca_rdma_ird_len = 0,
				 .ca_rdma_ird_val = NULL },
	};
	return ca;
}

static void setup(void)
{
	char owner_buf[32];
	client_owner4 owner;
	verifier4 v;
	struct sockaddr_in sin;
	struct nfs_impl_id4 impl = make_impl_id();

	nfs4_test_setup();
	g_ss = server_state_find();
	ck_assert_ptr_nonnull(g_ss);

	memcpy(owner_buf, "session-test-client", 19);
	owner.co_ownerid.co_ownerid_val = owner_buf;
	owner.co_ownerid.co_ownerid_len = 19;
	memset(&v, 0xAB, sizeof(v));
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(0x7f000001);
	sin.sin_port = htons(2049);

	make_owner(&owner, "session-test-client", owner_buf, sizeof(owner_buf));

	g_nc = nfs4_client_alloc_or_find(g_ss, &owner, &impl, &v, &sin);
	ck_assert_ptr_nonnull(g_nc);
}

static void teardown(void)
{
	nfs4_client_expire(g_ss, g_nc);
	g_nc = NULL;
	server_state_put(g_ss);
	g_ss = NULL;
	nfs4_test_teardown();
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

START_TEST(test_session_alloc_basic)
{
	channel_attrs4 ca = make_fore_chan(65536, 65536, 4096, 8, 4);

	struct nfs4_session *ns = nfs4_session_alloc(g_ss, g_nc, &ca, 0);
	ck_assert_ptr_nonnull(ns);

	/* sessionid must not be all zeros */
	uint8_t zeros[sizeof(sessionid4)] = { 0 };
	ck_assert_mem_ne(ns->ns_sessionid, zeros, sizeof(sessionid4));

	/* find by sessionid returns the same session */
	struct nfs4_session *found = nfs4_session_find(g_ss, ns->ns_sessionid);
	ck_assert_ptr_eq(found, ns);
	nfs4_session_put(found);

	nfs4_session_unhash(g_ss, ns);
	nfs4_session_put(ns);
}
END_TEST

START_TEST(test_session_unhash_removes)
{
	channel_attrs4 ca = make_fore_chan(65536, 65536, 4096, 8, 4);
	sessionid4 sid;

	struct nfs4_session *ns = nfs4_session_alloc(g_ss, g_nc, &ca, 0);
	ck_assert_ptr_nonnull(ns);
	memcpy(sid, ns->ns_sessionid, sizeof(sessionid4));

	nfs4_session_unhash(g_ss, ns);
	nfs4_session_put(ns);

	/* After unhash, find must return NULL */
	struct nfs4_session *gone = nfs4_session_find(g_ss, sid);
	ck_assert_ptr_null(gone);
}
END_TEST

START_TEST(test_session_unhash_idempotent)
{
	channel_attrs4 ca = make_fore_chan(65536, 65536, 4096, 8, 4);

	struct nfs4_session *ns = nfs4_session_alloc(g_ss, g_nc, &ca, 0);
	ck_assert_ptr_nonnull(ns);

	bool first = nfs4_session_unhash(g_ss, ns);
	bool second = nfs4_session_unhash(g_ss, ns);

	ck_assert(first);
	ck_assert(!second);

	nfs4_session_put(ns);
}
END_TEST

START_TEST(test_session_extra_ref)
{
	channel_attrs4 ca = make_fore_chan(65536, 65536, 4096, 8, 4);

	struct nfs4_session *ns = nfs4_session_alloc(g_ss, g_nc, &ca, 0);
	ck_assert_ptr_nonnull(ns);

	/* Extra get/put pair must not affect findability */
	struct nfs4_session *extra = nfs4_session_get(ns);
	ck_assert_ptr_eq(extra, ns);
	nfs4_session_put(extra);

	struct nfs4_session *found = nfs4_session_find(g_ss, ns->ns_sessionid);
	ck_assert_ptr_eq(found, ns);
	nfs4_session_put(found);

	nfs4_session_unhash(g_ss, ns);
	nfs4_session_put(ns);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Channel attribute negotiation                                       */
/* ------------------------------------------------------------------ */

START_TEST(test_session_attrs_zero_gets_server_max)
{
	/* Requesting 0 for sizes/ops/slots must get server caps. */
	channel_attrs4 ca = make_fore_chan(0, 0, 0, 0, 0);

	struct nfs4_session *ns = nfs4_session_alloc(g_ss, g_nc, &ca, 0);
	ck_assert_ptr_nonnull(ns);

	ck_assert_uint_eq(ns->ns_maxrequestsize, NFS4_SESSION_MAX_REQUEST_SIZE);
	ck_assert_uint_eq(ns->ns_maxresponsesize,
			  NFS4_SESSION_MAX_RESPONSE_SIZE);
	ck_assert_uint_eq(ns->ns_slot_count, NFS4_SESSION_MAX_SLOTS);

	nfs4_session_unhash(g_ss, ns);
	nfs4_session_put(ns);
}
END_TEST

START_TEST(test_session_attrs_excessive_gets_clamped)
{
	/* Requesting values larger than server caps must be clamped down. */
	channel_attrs4 ca = make_fore_chan(UINT32_MAX, UINT32_MAX, UINT32_MAX,
					   UINT32_MAX, UINT32_MAX);

	struct nfs4_session *ns = nfs4_session_alloc(g_ss, g_nc, &ca, 0);
	ck_assert_ptr_nonnull(ns);

	ck_assert_uint_le(ns->ns_maxrequestsize, NFS4_SESSION_MAX_REQUEST_SIZE);
	ck_assert_uint_le(ns->ns_maxresponsesize,
			  NFS4_SESSION_MAX_RESPONSE_SIZE);
	ck_assert_uint_le(ns->ns_maxresponsesize_cached,
			  NFS4_SESSION_MAX_RESPONSE_CACHED);
	ck_assert_uint_le(ns->ns_maxoperations, NFS4_SESSION_MAX_OPS);
	ck_assert_uint_le(ns->ns_slot_count, NFS4_SESSION_MAX_SLOTS);

	nfs4_session_unhash(g_ss, ns);
	nfs4_session_put(ns);
}
END_TEST

START_TEST(test_session_attrs_reasonable_preserved)
{
	/* Values within server caps must be stored as-is. */
	channel_attrs4 ca = make_fore_chan(32768, 32768, 2048, 4, 8);

	struct nfs4_session *ns = nfs4_session_alloc(g_ss, g_nc, &ca, 0);
	ck_assert_ptr_nonnull(ns);

	ck_assert_uint_eq(ns->ns_maxrequestsize, 32768);
	ck_assert_uint_eq(ns->ns_maxresponsesize, 32768);
	ck_assert_uint_eq(ns->ns_maxresponsesize_cached, 2048);
	ck_assert_uint_eq(ns->ns_maxoperations, 4);
	ck_assert_uint_eq(ns->ns_slot_count, 8);

	nfs4_session_unhash(g_ss, ns);
	nfs4_session_put(ns);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Slot table                                                          */
/* ------------------------------------------------------------------ */

START_TEST(test_session_slots_initial_state)
{
	channel_attrs4 ca = make_fore_chan(65536, 65536, 4096, 8, 4);

	struct nfs4_session *ns = nfs4_session_alloc(g_ss, g_nc, &ca, 0);
	ck_assert_ptr_nonnull(ns);
	ck_assert_ptr_nonnull(ns->ns_slots);

	for (uint32_t i = 0; i < ns->ns_slot_count; i++) {
		ck_assert_int_eq(ns->ns_slots[i].sl_state, NFS4_SLOT_IDLE);
		ck_assert_uint_eq(ns->ns_slots[i].sl_seqid, 0);
		ck_assert_ptr_null(ns->ns_slots[i].sl_reply);
	}

	nfs4_session_unhash(g_ss, ns);
	nfs4_session_put(ns);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Multiple sessions per client                                        */
/* ------------------------------------------------------------------ */

START_TEST(test_session_two_per_client)
{
	channel_attrs4 ca = make_fore_chan(65536, 65536, 4096, 8, 4);

	struct nfs4_session *ns1 = nfs4_session_alloc(g_ss, g_nc, &ca, 0);
	struct nfs4_session *ns2 = nfs4_session_alloc(g_ss, g_nc, &ca, 0);
	ck_assert_ptr_nonnull(ns1);
	ck_assert_ptr_nonnull(ns2);

	/* Sessions must be distinct objects with different IDs */
	ck_assert_ptr_ne(ns1, ns2);
	ck_assert_mem_ne(ns1->ns_sessionid, ns2->ns_sessionid,
			 sizeof(sessionid4));

	/* Both must be independently findable */
	struct nfs4_session *f1 = nfs4_session_find(g_ss, ns1->ns_sessionid);
	struct nfs4_session *f2 = nfs4_session_find(g_ss, ns2->ns_sessionid);
	ck_assert_ptr_eq(f1, ns1);
	ck_assert_ptr_eq(f2, ns2);
	nfs4_session_put(f1);
	nfs4_session_put(f2);

	nfs4_session_unhash(g_ss, ns1);
	nfs4_session_put(ns1);
	nfs4_session_unhash(g_ss, ns2);
	nfs4_session_put(ns2);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite assembly                                                      */
/* ------------------------------------------------------------------ */

Suite *nfs4_session_suite(void)
{
	Suite *s = suite_create("nfs4_session");
	TCase *tc;

	tc = tcase_create("lifecycle");
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_session_alloc_basic);
	tcase_add_test(tc, test_session_unhash_removes);
	tcase_add_test(tc, test_session_unhash_idempotent);
	tcase_add_test(tc, test_session_extra_ref);
	suite_add_tcase(s, tc);

	tc = tcase_create("channel_attrs");
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_session_attrs_zero_gets_server_max);
	tcase_add_test(tc, test_session_attrs_excessive_gets_clamped);
	tcase_add_test(tc, test_session_attrs_reasonable_preserved);
	suite_add_tcase(s, tc);

	tc = tcase_create("slot_table");
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_session_slots_initial_state);
	suite_add_tcase(s, tc);

	tc = tcase_create("multi_session");
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_session_two_per_client);
	suite_add_tcase(s, tc);

	return s;
}

int main(void)
{
	return nfs4_test_run(nfs4_session_suite());
}
