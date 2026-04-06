/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Unit tests for NFSv4 delegation stateid lifecycle:
 *
 *   1. Alloc/free: basic allocation and RCU-deferred release.
 *   2. Write flag: DELEG_STATEID_ACCESS_WRITE correctly set and read.
 *   3. Timestamp flag: ds_timestamps correctly set.
 *   4. Find: stateid_find locates a delegation by inode and id.
 *   5. XOR cleanup: DELEGRETURN frees the internal open_stateid
 *      when ds_open is set (RFC 9754 XOR mode).
 *   6. Delegation survives CLOSE: closing the open stateid must NOT
 *      affect the delegation (RFC 5661 S10.4).
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
#include "reffs/inode.h"
#include "reffs/super_block.h"
#include "reffs/stateid.h"
#include "nfs4/client.h"
#include "nfs4/client_persist.h"
#include "nfs4/stateid.h"
#include "nfs4_test_harness.h"

/* ------------------------------------------------------------------ */
/* Test fixture                                                        */
/* ------------------------------------------------------------------ */

static struct server_state *g_ss;
static struct nfs4_client *g_nc;
static struct super_block *g_sb;
static struct inode *g_inode;

static struct nfs_impl_id4 make_impl_id(void)
{
	static char domain[] = "example.com";
	static char name[] = "deleg-test";
	struct nfs_impl_id4 id = {
		.nii_domain = { .utf8string_val = domain,
				.utf8string_len = sizeof(domain) - 1 },
		.nii_name = { .utf8string_val = name,
			      .utf8string_len = sizeof(name) - 1 },
	};
	return id;
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

	memcpy(owner_buf, "deleg-test-client", 17);
	owner.co_ownerid.co_ownerid_val = owner_buf;
	owner.co_ownerid.co_ownerid_len = 17;
	memset(&v, 0xAB, sizeof(v));
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(0x7f000001);
	sin.sin_port = htons(2049);

	nfsstat4 eid_status;
	g_nc = nfs4_client_alloc_or_find(g_ss, &owner, &impl, &v, &sin, 1000,
					 false, &eid_status);
	ck_assert_ptr_nonnull(g_nc);

	g_sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	ck_assert_ptr_nonnull(g_sb);

	g_inode = inode_alloc(g_sb, 100);
	ck_assert_ptr_nonnull(g_inode);
}

static void teardown(void)
{
	inode_active_put(g_inode);
	g_inode = NULL;
	super_block_put(g_sb);
	g_sb = NULL;
	nfs4_client_expire(g_ss, g_nc);
	g_nc = NULL;
	server_state_put(g_ss);
	g_ss = NULL;
	nfs4_test_teardown();
}

/* ------------------------------------------------------------------ */
/* 1. Basic alloc/free lifecycle                                       */
/* ------------------------------------------------------------------ */

START_TEST(test_deleg_alloc_free)
{
	struct client *client = nfs4_client_to_client(g_nc);
	struct delegation_stateid *ds =
		delegation_stateid_alloc(g_inode, client);
	ck_assert_ptr_nonnull(ds);

	struct stateid *stid = &ds->ds_stid;
	ck_assert_int_eq(stid->s_tag, Delegation_Stateid);
	ck_assert_ptr_eq(stid->s_inode, g_inode);
	ck_assert_ptr_eq(stid->s_client, client);

	/* Initial state: no write access, no timestamps, no XOR open */
	ck_assert_uint_eq(ds->ds_state, 0);
	ck_assert(!ds->ds_timestamps);
	ck_assert_ptr_null(ds->ds_open);

	/* Clean up: unhash and drop refs */
	stateid_inode_unhash(stid);
	stateid_client_unhash(stid);
	stateid_put(stid); /* state ref --> freed via RCU */
}
END_TEST

/* ------------------------------------------------------------------ */
/* 2. Write delegation flag                                            */
/* ------------------------------------------------------------------ */

START_TEST(test_deleg_write_flag)
{
	struct client *client = nfs4_client_to_client(g_nc);
	struct delegation_stateid *ds =
		delegation_stateid_alloc(g_inode, client);
	ck_assert_ptr_nonnull(ds);

	/* Set write access */
	ds->ds_state |= DELEG_STATEID_ACCESS_WRITE;
	ck_assert(ds->ds_state & DELEG_STATEID_ACCESS_WRITE);

	/* Verify round-trip via stid_to_delegation */
	struct stateid *stid = &ds->ds_stid;
	struct delegation_stateid *ds2 = stid_to_delegation(stid);
	ck_assert_ptr_eq(ds, ds2);
	ck_assert(ds2->ds_state & DELEG_STATEID_ACCESS_WRITE);

	stateid_inode_unhash(stid);
	stateid_client_unhash(stid);
	stateid_put(stid);
}
END_TEST

/* ------------------------------------------------------------------ */
/* 3. Timestamp delegation flag                                        */
/* ------------------------------------------------------------------ */

START_TEST(test_deleg_timestamp_flag)
{
	struct client *client = nfs4_client_to_client(g_nc);
	struct delegation_stateid *ds =
		delegation_stateid_alloc(g_inode, client);
	ck_assert_ptr_nonnull(ds);

	ds->ds_timestamps = true;
	ck_assert(ds->ds_timestamps);

	stateid_inode_unhash(&ds->ds_stid);
	stateid_client_unhash(&ds->ds_stid);
	stateid_put(&ds->ds_stid);
}
END_TEST

/* ------------------------------------------------------------------ */
/* 4. Find delegation by inode and id                                  */
/* ------------------------------------------------------------------ */

START_TEST(test_deleg_find)
{
	struct client *client = nfs4_client_to_client(g_nc);
	struct delegation_stateid *ds =
		delegation_stateid_alloc(g_inode, client);
	ck_assert_ptr_nonnull(ds);

	struct stateid *stid = &ds->ds_stid;
	uint32_t id = stid->s_id;

	/* Should be findable by inode + id */
	struct stateid *found = stateid_find(g_inode, id);
	ck_assert_ptr_nonnull(found);
	ck_assert_ptr_eq(found, stid);
	ck_assert_int_eq(found->s_tag, Delegation_Stateid);
	stateid_put(found); /* find ref */

	/* Should also be findable via delegation-specific search */
	struct stateid *deleg_found =
		stateid_inode_find_delegation(g_inode, NULL);
	ck_assert_ptr_nonnull(deleg_found);
	ck_assert_ptr_eq(deleg_found, stid);
	stateid_put(deleg_found);

	stateid_inode_unhash(stid);
	stateid_client_unhash(stid);
	stateid_put(stid);
}
END_TEST

/* ------------------------------------------------------------------ */
/* 5. Delegation survives open stateid removal (RFC 5661 S10.4)        */
/*                                                                     */
/* A file is open as long as either an open stateid or a delegation    */
/* stateid is held.  Removing the open stateid must leave the          */
/* delegation intact.                                                  */
/* ------------------------------------------------------------------ */

START_TEST(test_deleg_survives_open_close)
{
	struct client *client = nfs4_client_to_client(g_nc);

	/* Create an open stateid (simulating OPEN) */
	struct open_stateid *os = open_stateid_alloc(g_inode, client);
	ck_assert_ptr_nonnull(os);

	/* Create a delegation stateid on the same inode */
	struct delegation_stateid *ds =
		delegation_stateid_alloc(g_inode, client);
	ck_assert_ptr_nonnull(ds);
	ds->ds_state |= DELEG_STATEID_ACCESS_WRITE;

	uint32_t deleg_id = ds->ds_stid.s_id;

	/* Simulate CLOSE: remove the open stateid */
	stateid_inode_unhash(&os->os_stid);
	stateid_client_unhash(&os->os_stid);
	stateid_put(&os->os_stid); /* state ref --> freed */

	/* The delegation must still be findable */
	struct stateid *found = stateid_find(g_inode, deleg_id);
	ck_assert_ptr_nonnull(found);
	ck_assert_int_eq(found->s_tag, Delegation_Stateid);
	ck_assert(stid_to_delegation(found)->ds_state &
		  DELEG_STATEID_ACCESS_WRITE);
	stateid_put(found); /* find ref */

	/* Clean up the delegation */
	stateid_inode_unhash(&ds->ds_stid);
	stateid_client_unhash(&ds->ds_stid);
	stateid_put(&ds->ds_stid);
}
END_TEST

/* ------------------------------------------------------------------ */
/* 6. XOR mode: ds_open cleaned up during DELEGRETURN                  */
/*                                                                     */
/* RFC 9754: when OPEN_XOR_DELEGATION is used, the server allocates    */
/* an internal open_stateid stored in ds->ds_open.  DELEGRETURN must   */
/* clean it up since no separate CLOSE will arrive.                    */
/* ------------------------------------------------------------------ */

START_TEST(test_deleg_xor_open_linked)
{
	struct client *client = nfs4_client_to_client(g_nc);

	/* Simulate XOR mode: allocate open + delegation, link them */
	struct open_stateid *os = open_stateid_alloc(g_inode, client);
	ck_assert_ptr_nonnull(os);

	struct delegation_stateid *ds =
		delegation_stateid_alloc(g_inode, client);
	ck_assert_ptr_nonnull(ds);
	ds->ds_state |= DELEG_STATEID_ACCESS_WRITE;
	ds->ds_open = os; /* XOR link */

	/* Verify the link */
	ck_assert_ptr_eq(ds->ds_open, os);

	/* Simulate what DELEGRETURN does for XOR cleanup */
	struct open_stateid *xor_os = ds->ds_open;
	ds->ds_open = NULL;
	stateid_inode_unhash(&xor_os->os_stid);
	stateid_client_unhash(&xor_os->os_stid);
	stateid_put(&xor_os->os_stid); /* state ref --> freed */

	/* ds_open is now NULL */
	ck_assert_ptr_null(ds->ds_open);

	/* Delegation itself is still valid */
	struct stateid *found = stateid_find(g_inode, ds->ds_stid.s_id);
	ck_assert_ptr_nonnull(found);
	ck_assert_int_eq(found->s_tag, Delegation_Stateid);
	stateid_put(found);

	/* Clean up delegation */
	stateid_inode_unhash(&ds->ds_stid);
	stateid_client_unhash(&ds->ds_stid);
	stateid_put(&ds->ds_stid);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite assembly                                                      */
/* ------------------------------------------------------------------ */

Suite *delegation_lifecycle_suite(void)
{
	Suite *s = suite_create("Delegation Lifecycle");

	TCase *tc = tcase_create("lifecycle");
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_deleg_alloc_free);
	tcase_add_test(tc, test_deleg_write_flag);
	tcase_add_test(tc, test_deleg_timestamp_flag);
	tcase_add_test(tc, test_deleg_find);
	tcase_add_test(tc, test_deleg_survives_open_close);
	tcase_add_test(tc, test_deleg_xor_open_linked);
	suite_add_tcase(s, tc);

	return s;
}

int main(void)
{
	return nfs4_test_run(delegation_lifecycle_suite());
}
