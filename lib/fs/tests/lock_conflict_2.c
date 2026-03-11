/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * Unit tests for reffs_lock_find_conflict() and reffs_lock_add().
 *
 * Conflict rules (from lock.c):
 *   - shared vs shared:    no conflict
 *   - shared vs exclusive: conflict
 *   - exclusive vs shared: conflict
 *   - exclusive vs exclusive: conflict
 *   - same owner pointer:  no conflict (own locks never block ourselves)
 *   - lo_match() returns true: treated as same owner, no conflict
 *   - no range overlap:    no conflict regardless of type
 *
 * reffs_lock_add() rules:
 *   - new lock: appended to inode->i_locks and optionally to host_list
 *   - re-lock same owner+range: updates exclusivity in place, no duplicate
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdint.h>
#include <check.h>
#include <stdbool.h>
#include <errno.h>
#include <urcu.h>
#include <urcu/ref.h>

#include "lock_test.h"
#include "reffs/lock.h"
#include "reffs/super_block.h"
#include "reffs/inode.h"
#include "reffs/test.h"

/* Shared superblock for all tests in this file — RAM-backed, no teardown
 * sleep needed.  Created once in main(). */
static struct super_block *g_sb;

/* Helper: allocate a fresh RAM inode suitable for locking tests. */
static struct inode *alloc_test_inode(void)
{
	static uint64_t next_ino = 100;
	uint64_t ino = __atomic_add_fetch(&next_ino, 1, __ATOMIC_RELAXED);
	struct inode *inode = inode_alloc(g_sb, ino);
	ck_assert(inode != NULL);
	inode->i_mode = S_IFREG | 0644;
	return inode;
}

/* ── reffs_lock_find_conflict ────────────────────────────────────────────── */

START_TEST(test_conflict_no_locks)
{
	struct inode *inode = alloc_test_inode();
	struct test_lock_owner *o = test_owner_alloc(1);
	ck_assert(o != NULL);

	/* Empty lock list: no conflict possible */
	struct reffs_lock *c =
		reffs_lock_find_conflict(inode, 0, 100, true, &o->base, NULL);
	ck_assert(c == NULL);

	struct test_lock_owner *owners[] = { o };
	test_inode_cleanup(inode, owners, 1);
}
END_TEST

START_TEST(test_conflict_shared_vs_shared)
{
	struct inode *inode = alloc_test_inode();
	struct test_lock_owner *o1 = test_owner_alloc(1);
	struct test_lock_owner *o2 = test_owner_alloc(2);
	ck_assert(o1 && o2);

	struct reffs_lock *rl = test_lock_alloc(inode, o1, 0, 100, false);
	ck_assert(rl != NULL);
	ck_assert_int_eq(reffs_lock_add(inode, rl, NULL), 0);

	/* Shared request against a shared lock: no conflict */
	struct reffs_lock *c =
		reffs_lock_find_conflict(inode, 0, 100, false, &o2->base, NULL);
	ck_assert(c == NULL);

	struct test_lock_owner *owners[] = { o1, o2 };
	test_inode_cleanup(inode, owners, 2);
}
END_TEST

START_TEST(test_conflict_shared_vs_exclusive)
{
	struct inode *inode = alloc_test_inode();
	struct test_lock_owner *o1 = test_owner_alloc(1);
	struct test_lock_owner *o2 = test_owner_alloc(2);
	ck_assert(o1 && o2);

	/* o1 holds a shared lock */
	struct reffs_lock *rl = test_lock_alloc(inode, o1, 0, 100, false);
	ck_assert(rl != NULL);
	ck_assert_int_eq(reffs_lock_add(inode, rl, NULL), 0);

	/* o2 requests exclusive: conflict */
	struct reffs_lock *c =
		reffs_lock_find_conflict(inode, 0, 100, true, &o2->base, NULL);
	ck_assert(c != NULL);
	ck_assert(c == rl);

	struct test_lock_owner *owners[] = { o1, o2 };
	test_inode_cleanup(inode, owners, 2);
}
END_TEST

START_TEST(test_conflict_exclusive_vs_shared)
{
	struct inode *inode = alloc_test_inode();
	struct test_lock_owner *o1 = test_owner_alloc(1);
	struct test_lock_owner *o2 = test_owner_alloc(2);
	ck_assert(o1 && o2);

	/* o1 holds an exclusive lock */
	struct reffs_lock *rl = test_lock_alloc(inode, o1, 0, 100, true);
	ck_assert(rl != NULL);
	ck_assert_int_eq(reffs_lock_add(inode, rl, NULL), 0);

	/* o2 requests shared: exclusive blocks even shared requests */
	struct reffs_lock *c =
		reffs_lock_find_conflict(inode, 0, 100, false, &o2->base, NULL);
	ck_assert(c != NULL);
	ck_assert(c == rl);

	struct test_lock_owner *owners[] = { o1, o2 };
	test_inode_cleanup(inode, owners, 2);
}
END_TEST

START_TEST(test_conflict_exclusive_vs_exclusive)
{
	struct inode *inode = alloc_test_inode();
	struct test_lock_owner *o1 = test_owner_alloc(1);
	struct test_lock_owner *o2 = test_owner_alloc(2);
	ck_assert(o1 && o2);

	struct reffs_lock *rl = test_lock_alloc(inode, o1, 0, 100, true);
	ck_assert(rl != NULL);
	ck_assert_int_eq(reffs_lock_add(inode, rl, NULL), 0);

	struct reffs_lock *c =
		reffs_lock_find_conflict(inode, 0, 100, true, &o2->base, NULL);
	ck_assert(c != NULL);

	struct test_lock_owner *owners[] = { o1, o2 };
	test_inode_cleanup(inode, owners, 2);
}
END_TEST

START_TEST(test_conflict_same_owner_no_conflict)
{
	struct inode *inode = alloc_test_inode();
	struct test_lock_owner *o = test_owner_alloc(1);
	ck_assert(o != NULL);

	/* Same owner holds exclusive — should never block itself */
	struct reffs_lock *rl = test_lock_alloc(inode, o, 0, 100, true);
	ck_assert(rl != NULL);
	ck_assert_int_eq(reffs_lock_add(inode, rl, NULL), 0);

	struct reffs_lock *c =
		reffs_lock_find_conflict(inode, 0, 100, true, &o->base, NULL);
	ck_assert(c == NULL);

	struct test_lock_owner *owners[] = { o };
	test_inode_cleanup(inode, owners, 1);
}
END_TEST

START_TEST(test_conflict_no_overlap)
{
	struct inode *inode = alloc_test_inode();
	struct test_lock_owner *o1 = test_owner_alloc(1);
	struct test_lock_owner *o2 = test_owner_alloc(2);
	ck_assert(o1 && o2);

	/* o1 locks [0, 99] exclusively */
	struct reffs_lock *rl = test_lock_alloc(inode, o1, 0, 100, true);
	ck_assert(rl != NULL);
	ck_assert_int_eq(reffs_lock_add(inode, rl, NULL), 0);

	/* o2 requests exclusive [200, 299]: no overlap, no conflict */
	struct reffs_lock *c = reffs_lock_find_conflict(inode, 200, 100, true,
							&o2->base, NULL);
	ck_assert(c == NULL);

	struct test_lock_owner *owners[] = { o1, o2 };
	test_inode_cleanup(inode, owners, 2);
}
END_TEST

/*
 * lo_match callback used by test_conflict_lo_match_no_conflict.
 * Always returns true: simulates two distinct owner pointers that
 * represent the same client (e.g. an NLM owner and an NFSv4 owner
 * for the same host).
 */
static bool match_always_true(struct reffs_lock_owner *other, void *arg)
{
	(void)other;
	(void)arg;
	return true;
}

START_TEST(test_conflict_lo_match_no_conflict)
{
	/*
	 * lo_match lets callers declare two different owner pointers as
	 * "the same owner".  When lo_match returns true the lock entry is
	 * skipped, so no conflict is reported even for exclusive ranges.
	 */
	struct inode *inode = alloc_test_inode();
	struct test_lock_owner *o1 = test_owner_alloc(1);
	struct test_lock_owner *o2 = test_owner_alloc(2);
	ck_assert(o1 && o2);

	struct reffs_lock *rl = test_lock_alloc(inode, o1, 0, 100, true);
	ck_assert(rl != NULL);
	ck_assert_int_eq(reffs_lock_add(inode, rl, NULL), 0);

	/* o2's match function considers o1 as itself */
	o2->base.lo_match = match_always_true;

	struct reffs_lock *c =
		reffs_lock_find_conflict(inode, 0, 100, true, &o2->base, NULL);
	ck_assert(c == NULL);

	struct test_lock_owner *owners[] = { o1, o2 };
	test_inode_cleanup(inode, owners, 2);
}
END_TEST

/* ── reffs_lock_add ──────────────────────────────────────────────────────── */

START_TEST(test_lock_add_new)
{
	struct inode *inode = alloc_test_inode();
	struct test_lock_owner *o = test_owner_alloc(1);
	ck_assert(o != NULL);

	ck_assert_int_eq(lock_count(inode), 0);

	struct reffs_lock *rl = test_lock_alloc(inode, o, 0, 100, false);
	ck_assert(rl != NULL);
	ck_assert_int_eq(reffs_lock_add(inode, rl, NULL), 0);

	ck_assert_int_eq(lock_count(inode), 1);

	struct test_lock_owner *owners[] = { o };
	test_inode_cleanup(inode, owners, 1);
}
END_TEST

START_TEST(test_lock_add_relock_upgrades)
{
	/*
	 * Re-locking the same range as the same owner must update
	 * exclusivity in place rather than adding a second entry.
	 */
	struct inode *inode = alloc_test_inode();
	struct test_lock_owner *o = test_owner_alloc(1);
	ck_assert(o != NULL);

	struct reffs_lock *rl1 = test_lock_alloc(inode, o, 0, 100, false);
	ck_assert(rl1 != NULL);
	ck_assert_int_eq(reffs_lock_add(inode, rl1, NULL), 0);
	ck_assert_int_eq(lock_count(inode), 1);

	/*
	 * Same owner, same range, upgrade to exclusive.
	 * reffs_lock_add() must update l_exclusive on the existing entry
	 * and free the new one (returns 0).
	 */
	struct reffs_lock *rl2 = test_lock_alloc(inode, o, 0, 100, true);
	ck_assert(rl2 != NULL);
	ck_assert_int_eq(reffs_lock_add(inode, rl2, NULL), 0);

	/* Still only one lock in the list */
	ck_assert_int_eq(lock_count(inode), 1);

	/* And the in-place entry must now be exclusive */
	struct reffs_lock *le;
	cds_list_for_each_entry(le, &inode->i_locks, l_list)
		ck_assert(le->l_exclusive);

	/*
	 * reffs_lock_add() updates the existing entry in place and returns 0,
	 * but does NOT free rl2 — the caller retains ownership of the passed
	 * struct on the relock path.  Free it explicitly.
	 */
	reffs_lock_free(rl2);
	struct test_lock_owner *owners[] = { o };
	test_inode_cleanup(inode, owners, 1);
}
END_TEST

START_TEST(test_lock_add_different_range_not_merged)
{
	/* Different ranges for the same owner: two separate entries */
	struct inode *inode = alloc_test_inode();
	struct test_lock_owner *o = test_owner_alloc(1);
	ck_assert(o != NULL);

	struct reffs_lock *rl1 = test_lock_alloc(inode, o, 0, 100, false);
	struct reffs_lock *rl2 = test_lock_alloc(inode, o, 200, 100, false);
	ck_assert(rl1 && rl2);

	ck_assert_int_eq(reffs_lock_add(inode, rl1, NULL), 0);
	ck_assert_int_eq(reffs_lock_add(inode, rl2, NULL), 0);
	ck_assert_int_eq(lock_count(inode), 2);

	struct test_lock_owner *owners[] = { o };
	test_inode_cleanup(inode, owners, 1);
}
END_TEST

START_TEST(test_lock_add_host_list)
{
	/* host_list linkage must be populated when non-NULL */
	struct inode *inode = alloc_test_inode();
	struct test_lock_owner *o = test_owner_alloc(1);
	ck_assert(o != NULL);

	CDS_LIST_HEAD(host);

	struct reffs_lock *rl = test_lock_alloc(inode, o, 0, 100, false);
	ck_assert(rl != NULL);
	ck_assert_int_eq(reffs_lock_add(inode, rl, &host), 0);

	/* lock must appear in both the inode list and the host list */
	ck_assert_int_eq(lock_count(inode), 1);
	ck_assert(!cds_list_empty(&host));

	struct test_lock_owner *owners[] = { o };
	test_inode_cleanup(inode, owners, 1);
}
END_TEST

/* ── suite ───────────────────────────────────────────────────────────────── */

Suite *lock_conflict_suite(void)
{
	Suite *s = suite_create("Lock: find_conflict and add");

	TCase *tc_conflict = tcase_create("find_conflict");
	tcase_add_test(tc_conflict, test_conflict_no_locks);
	tcase_add_test(tc_conflict, test_conflict_shared_vs_shared);
	tcase_add_test(tc_conflict, test_conflict_shared_vs_exclusive);
	tcase_add_test(tc_conflict, test_conflict_exclusive_vs_shared);
	tcase_add_test(tc_conflict, test_conflict_exclusive_vs_exclusive);
	tcase_add_test(tc_conflict, test_conflict_same_owner_no_conflict);
	tcase_add_test(tc_conflict, test_conflict_no_overlap);
	tcase_add_test(tc_conflict, test_conflict_lo_match_no_conflict);
	suite_add_tcase(s, tc_conflict);

	TCase *tc_add = tcase_create("add");
	tcase_add_test(tc_add, test_lock_add_new);
	tcase_add_test(tc_add, test_lock_add_relock_upgrades);
	tcase_add_test(tc_add, test_lock_add_different_range_not_merged);
	tcase_add_test(tc_add, test_lock_add_host_list);
	suite_add_tcase(s, tc_add);

	return s;
}

int main(void)
{
	int number_failed;
	SRunner *sr;

	rcu_register_thread();

	g_sb = super_block_alloc(200, "/", REFFS_STORAGE_RAM, NULL);
	if (!g_sb) {
		fprintf(stderr, "super_block_alloc failed\n");
		rcu_unregister_thread();
		return EXIT_FAILURE;
	}

	sr = srunner_create(lock_conflict_suite());
	srunner_set_fork_status(sr, CK_NOFORK);
	srunner_run_all(sr, CK_NORMAL);
	number_failed = srunner_ntests_failed(sr);
	srunner_free(sr);

	super_block_put(g_sb);
	rcu_unregister_thread();

	return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
