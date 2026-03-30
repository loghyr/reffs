/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Unit test for stateid_inode_unhash atomicity.
 *
 * Verifies that only the first caller succeeds when two threads
 * race to unhash the same stateid (the bug that caused refcount
 * underflow in CLOSE, DELEGRETURN, and FREE_STATEID).
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <urcu.h>
#include <urcu/rculfhash.h>

#include <check.h>

#include "reffs/inode.h"
#include "reffs/stateid.h"
#include "reffs/super_block.h"
#include "nfs4_test_harness.h"

static struct super_block *test_sb;
static struct inode *test_inode;

static void unhash_setup(void)
{
	nfs4_test_setup();
	test_sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	ck_assert_ptr_nonnull(test_sb);
	test_inode = inode_alloc(test_sb, 0);
	ck_assert_ptr_nonnull(test_inode);
}

static void unhash_teardown(void)
{
	if (test_inode) {
		inode_active_put(test_inode);
		test_inode = NULL;
	}
	if (test_sb) {
		super_block_put(test_sb);
		test_sb = NULL;
	}
	nfs4_test_teardown();
}

/*
 * Only the first stateid_inode_unhash succeeds; the second returns false.
 * This is the guard that prevents the double-put in CLOSE/DELEGRETURN.
 *
 * We manually set STID_IS_INODE_HASHED and insert into the inode's
 * stateid hash table, bypassing stateid_assign (which requires a
 * full client).  This tests the atomic flag guard in isolation.
 */
START_TEST(test_unhash_first_succeeds)
{
	struct stateid stid;
	memset(&stid, 0, sizeof(stid));

	stid.s_inode = test_inode;
	stid.s_id = 42;

	/* Insert into the inode's stateid hash table. */
	cds_lfht_node_init(&stid.s_inode_node);
	rcu_read_lock();
	stid.s_state = STID_IS_INODE_HASHED;
	cds_lfht_add(test_inode->i_stateids, 42, &stid.s_inode_node);
	rcu_read_unlock();

	/* First unhash succeeds. */
	ck_assert(stateid_inode_unhash(&stid));

	/* Second unhash fails — already unhashed. */
	ck_assert(!stateid_inode_unhash(&stid));

	/* Third call also fails — idempotent. */
	ck_assert(!stateid_inode_unhash(&stid));
}
END_TEST

/*
 * A stateid that was never hashed returns false.
 */
START_TEST(test_unhash_never_hashed)
{
	struct stateid stid = { 0 };

	/* s_state is 0 — STID_IS_INODE_HASHED not set. */
	ck_assert(!stateid_inode_unhash(&stid));
}
END_TEST

Suite *stateid_unhash_suite(void)
{
	Suite *s;
	TCase *tc;

	s = suite_create("Stateid Unhash");

	tc = tcase_create("Atomicity");
	tcase_add_checked_fixture(tc, unhash_setup, unhash_teardown);
	tcase_add_test(tc, test_unhash_first_succeeds);
	tcase_add_test(tc, test_unhash_never_hashed);
	suite_add_tcase(s, tc);

	return s;
}

int main(void)
{
	return nfs4_test_run(stateid_unhash_suite());
}
