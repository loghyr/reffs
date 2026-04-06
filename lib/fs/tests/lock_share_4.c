/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * Unit tests for reffs_share_add() and reffs_share_remove().
 *
 * Share semantics (from lock.c):
 *   s_mode   = deny bits  (FSM_DR=1 denies read, FSM_DW=2 denies write)
 *   s_access = access bits (FSA_R=1 read access,  FSA_W=2 write access)
 *
 * A conflict exists between two shares S1 and S2 when:
 *   (S2.access & S1.mode) != 0   // S2 wants something S1 denies
 *   (S1.access & S2.mode) != 0   // S1 has something S2 denies
 *
 * reffs_share_add() returns -EACCES on conflict.
 * When the same owner re-opens, the existing share is updated in place
 * (upgrade) and the new share struct is freed.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdint.h>
#include <check.h>
#include "libreffs_test.h"
#include "fs_test_harness.h"
#include <stdbool.h>
#include <errno.h>
#include <urcu.h>
#include <urcu/ref.h>

#include "lock_test.h"
#include "reffs/lock.h"
#include "reffs/super_block.h"
#include "reffs/inode.h"
#include "reffs/test.h"

static struct super_block *g_sb;

static struct inode *alloc_test_inode(void)
{
	static uint64_t next_ino = 800;
	uint64_t ino = __atomic_add_fetch(&next_ino, 1, __ATOMIC_RELAXED);
	struct inode *inode = inode_alloc(g_sb, ino);
	ck_assert(inode != NULL);
	inode->i_mode = S_IFREG | 0644;
	return inode;
}

/* -- no conflict ------------------------------------------------------------ */

START_TEST(test_share_deny_none_vs_deny_none)
{
	/* FSM_DN + FSA_RW vs FSM_DN + FSA_RW: both deny nothing, no conflict */
	struct inode *inode = alloc_test_inode();
	struct test_lock_owner *o1 = test_owner_alloc(1);
	struct test_lock_owner *o2 = test_owner_alloc(2);
	ck_assert(o1 && o2);

	struct reffs_share *s1 = test_share_alloc(inode, o1, FSM_DN, FSA_RW);
	struct reffs_share *s2 = test_share_alloc(inode, o2, FSM_DN, FSA_RW);
	ck_assert(s1 && s2);

	ck_assert_int_eq(reffs_share_add(inode, s1, NULL), 0);
	ck_assert_int_eq(reffs_share_add(inode, s2, NULL), 0);
	ck_assert_int_eq(share_count(inode), 2);

	struct test_lock_owner *owners[] = { o1, o2 };
	test_inode_cleanup(inode, owners, 2);
}
END_TEST

START_TEST(test_share_deny_read_vs_write_only_access)
{
	/*
	 * S1: deny-read  (FSM_DR=1), access-write (FSA_W=2)
	 * S2: deny-none  (FSM_DN=0), access-write (FSA_W=2)
	 *
	 * Conflict check for S2 adding after S1:
	 *   S2.access(2) & S1.mode(1) = 0  --> ok
	 *   S1.access(2) & S2.mode(0) = 0  --> ok
	 * No conflict: S1 denies read, S2 only wants write.
	 */
	struct inode *inode = alloc_test_inode();
	struct test_lock_owner *o1 = test_owner_alloc(1);
	struct test_lock_owner *o2 = test_owner_alloc(2);
	ck_assert(o1 && o2);

	struct reffs_share *s1 = test_share_alloc(inode, o1, FSM_DR, FSA_W);
	struct reffs_share *s2 = test_share_alloc(inode, o2, FSM_DN, FSA_W);
	ck_assert(s1 && s2);

	ck_assert_int_eq(reffs_share_add(inode, s1, NULL), 0);
	ck_assert_int_eq(reffs_share_add(inode, s2, NULL), 0);
	ck_assert_int_eq(share_count(inode), 2);

	struct test_lock_owner *owners[] = { o1, o2 };
	test_inode_cleanup(inode, owners, 2);
}
END_TEST

/* -- conflict --------------------------------------------------------------- */

START_TEST(test_share_conflict_deny_read_vs_read_access)
{
	/*
	 * S1: deny-read (FSM_DR=1), access-write (FSA_W=2)
	 * S2: deny-none (FSM_DN=0), access-read  (FSA_R=1)
	 *
	 * S2.access(1) & S1.mode(1) = 1 != 0 --> CONFLICT
	 * reffs_share_add must return -EACCES.
	 */
	struct inode *inode = alloc_test_inode();
	struct test_lock_owner *o1 = test_owner_alloc(1);
	struct test_lock_owner *o2 = test_owner_alloc(2);
	ck_assert(o1 && o2);

	struct reffs_share *s1 = test_share_alloc(inode, o1, FSM_DR, FSA_W);
	ck_assert(s1 != NULL);
	ck_assert_int_eq(reffs_share_add(inode, s1, NULL), 0);

	struct reffs_share *s2 = test_share_alloc(inode, o2, FSM_DN, FSA_R);
	ck_assert(s2 != NULL);
	ck_assert_int_eq(reffs_share_add(inode, s2, NULL), -EACCES);

	/* s2 was rejected: must still be only one share in the list */
	ck_assert_int_eq(share_count(inode), 1);

	/* s2 was NOT consumed by reffs_share_add on conflict, free it */
	reffs_share_free(s2);

	struct test_lock_owner *owners[] = { o1, o2 };
	test_inode_cleanup(inode, owners, 2);
}
END_TEST

START_TEST(test_share_conflict_deny_write_vs_write_access)
{
	/*
	 * S1: deny-write (FSM_DW=2), access-read (FSA_R=1)
	 * S2: deny-none  (FSM_DN=0), access-write (FSA_W=2)
	 *
	 * S2.access(2) & S1.mode(2) = 2 != 0 --> CONFLICT
	 */
	struct inode *inode = alloc_test_inode();
	struct test_lock_owner *o1 = test_owner_alloc(1);
	struct test_lock_owner *o2 = test_owner_alloc(2);
	ck_assert(o1 && o2);

	struct reffs_share *s1 = test_share_alloc(inode, o1, FSM_DW, FSA_R);
	ck_assert(s1 != NULL);
	ck_assert_int_eq(reffs_share_add(inode, s1, NULL), 0);

	struct reffs_share *s2 = test_share_alloc(inode, o2, FSM_DN, FSA_W);
	ck_assert(s2 != NULL);
	ck_assert_int_eq(reffs_share_add(inode, s2, NULL), -EACCES);
	ck_assert_int_eq(share_count(inode), 1);

	reffs_share_free(s2);

	struct test_lock_owner *owners[] = { o1, o2 };
	test_inode_cleanup(inode, owners, 2);
}
END_TEST

START_TEST(test_share_conflict_reverse_direction)
{
	/*
	 * S1: deny-none  (FSM_DN=0), access-read  (FSA_R=1)
	 * S2: deny-read  (FSM_DR=1), access-write (FSA_W=2)
	 *
	 * S1.access(1) & S2.mode(1) = 1 != 0 --> CONFLICT (S2 denies what S1 has)
	 */
	struct inode *inode = alloc_test_inode();
	struct test_lock_owner *o1 = test_owner_alloc(1);
	struct test_lock_owner *o2 = test_owner_alloc(2);
	ck_assert(o1 && o2);

	struct reffs_share *s1 = test_share_alloc(inode, o1, FSM_DN, FSA_R);
	ck_assert(s1 != NULL);
	ck_assert_int_eq(reffs_share_add(inode, s1, NULL), 0);

	struct reffs_share *s2 = test_share_alloc(inode, o2, FSM_DR, FSA_W);
	ck_assert(s2 != NULL);
	ck_assert_int_eq(reffs_share_add(inode, s2, NULL), -EACCES);
	ck_assert_int_eq(share_count(inode), 1);

	reffs_share_free(s2);

	struct test_lock_owner *owners[] = { o1, o2 };
	test_inode_cleanup(inode, owners, 2);
}
END_TEST

START_TEST(test_share_conflict_deny_rw_vs_any_access)
{
	/*
	 * S1: deny-both (FSM_DRW=3), access-none (FSA_NONE=0)
	 * S2: deny-none (FSM_DN=0),  access-read (FSA_R=1)
	 *
	 * S2.access(1) & S1.mode(3) = 1 != 0 --> CONFLICT
	 */
	struct inode *inode = alloc_test_inode();
	struct test_lock_owner *o1 = test_owner_alloc(1);
	struct test_lock_owner *o2 = test_owner_alloc(2);
	ck_assert(o1 && o2);

	struct reffs_share *s1 = test_share_alloc(inode, o1, FSM_DRW, FSA_NONE);
	ck_assert(s1 != NULL);
	ck_assert_int_eq(reffs_share_add(inode, s1, NULL), 0);

	struct reffs_share *s2 = test_share_alloc(inode, o2, FSM_DN, FSA_R);
	ck_assert(s2 != NULL);
	ck_assert_int_eq(reffs_share_add(inode, s2, NULL), -EACCES);
	ck_assert_int_eq(share_count(inode), 1);

	reffs_share_free(s2);

	struct test_lock_owner *owners[] = { o1, o2 };
	test_inode_cleanup(inode, owners, 2);
}
END_TEST

/* -- same-owner upgrade ----------------------------------------------------- */

START_TEST(test_share_upgrade_same_owner)
{
	/*
	 * The same owner adds a second share reservation.
	 * reffs_share_add() must update the existing entry in place (upgrade)
	 * and free the new share struct, leaving exactly one share in the list.
	 */
	struct inode *inode = alloc_test_inode();
	struct test_lock_owner *o = test_owner_alloc(1);
	ck_assert(o != NULL);

	struct reffs_share *s1 = test_share_alloc(inode, o, FSM_DN, FSA_R);
	ck_assert(s1 != NULL);
	ck_assert_int_eq(reffs_share_add(inode, s1, NULL), 0);
	ck_assert_int_eq(share_count(inode), 1);

	/*
	 * s2 is passed to reffs_share_add which will free it on the upgrade
	 * path; do NOT call reffs_share_free(s2) afterward.
	 */
	struct reffs_share *s2 = test_share_alloc(inode, o, FSM_DW, FSA_RW);
	ck_assert(s2 != NULL);
	ck_assert_int_eq(reffs_share_add(inode, s2, NULL), 0);

	/* Still only one share */
	ck_assert_int_eq(share_count(inode), 1);

	/* And it must reflect the upgraded mode/access */
	struct reffs_share *se;
	cds_list_for_each_entry(se, &inode->i_shares, s_list) {
		ck_assert_uint_eq(se->s_mode, FSM_DW);
		ck_assert_uint_eq(se->s_access, FSA_RW);
	}

	struct test_lock_owner *owners[] = { o };
	test_inode_cleanup(inode, owners, 1);
}
END_TEST

/* -- remove ----------------------------------------------------------------- */

START_TEST(test_share_remove_existing)
{
	struct inode *inode = alloc_test_inode();
	struct test_lock_owner *o = test_owner_alloc(1);
	ck_assert(o != NULL);

	struct reffs_share *s = test_share_alloc(inode, o, FSM_DN, FSA_RW);
	ck_assert(s != NULL);
	ck_assert_int_eq(reffs_share_add(inode, s, NULL), 0);
	ck_assert_int_eq(share_count(inode), 1);

	ck_assert_int_eq(reffs_share_remove(inode, &o->base, NULL), 0);
	ck_assert_int_eq(share_count(inode), 0);

	struct test_lock_owner *owners[] = { o };
	test_inode_cleanup(inode, owners, 1);
}
END_TEST

START_TEST(test_share_remove_wrong_owner_no_effect)
{
	struct inode *inode = alloc_test_inode();
	struct test_lock_owner *o1 = test_owner_alloc(1);
	struct test_lock_owner *o2 = test_owner_alloc(2);
	ck_assert(o1 && o2);

	struct reffs_share *s = test_share_alloc(inode, o1, FSM_DN, FSA_RW);
	ck_assert(s != NULL);
	ck_assert_int_eq(reffs_share_add(inode, s, NULL), 0);

	/* Removing o2's (non-existent) share must not touch o1's entry */
	ck_assert_int_eq(reffs_share_remove(inode, &o2->base, NULL), 0);
	ck_assert_int_eq(share_count(inode), 1);

	struct test_lock_owner *owners[] = { o1, o2 };
	test_inode_cleanup(inode, owners, 2);
}
END_TEST

START_TEST(test_share_remove_only_own_entry)
{
	/* o1 and o2 both have shares; removing o1 must leave o2's intact */
	struct inode *inode = alloc_test_inode();
	struct test_lock_owner *o1 = test_owner_alloc(1);
	struct test_lock_owner *o2 = test_owner_alloc(2);
	ck_assert(o1 && o2);

	struct reffs_share *s1 = test_share_alloc(inode, o1, FSM_DN, FSA_R);
	struct reffs_share *s2 = test_share_alloc(inode, o2, FSM_DN, FSA_W);
	ck_assert(s1 && s2);

	ck_assert_int_eq(reffs_share_add(inode, s1, NULL), 0);
	ck_assert_int_eq(reffs_share_add(inode, s2, NULL), 0);
	ck_assert_int_eq(share_count(inode), 2);

	ck_assert_int_eq(reffs_share_remove(inode, &o1->base, NULL), 0);
	ck_assert_int_eq(share_count(inode), 1);

	struct reffs_share *se;
	cds_list_for_each_entry(se, &inode->i_shares, s_list)
		ck_assert(se->s_owner == &o2->base);

	struct test_lock_owner *owners[] = { o1, o2 };
	test_inode_cleanup(inode, owners, 2);
}
END_TEST

START_TEST(test_share_remove_nonexistent_no_error)
{
	/* Removing a share for an owner who never added one: must not crash */
	struct inode *inode = alloc_test_inode();
	struct test_lock_owner *o = test_owner_alloc(1);
	ck_assert(o != NULL);

	ck_assert_int_eq(share_count(inode), 0);
	ck_assert_int_eq(reffs_share_remove(inode, &o->base, NULL), 0);
	ck_assert_int_eq(share_count(inode), 0);

	struct test_lock_owner *owners[] = { o };
	test_inode_cleanup(inode, owners, 1);
}
END_TEST

/* -- host_list linkage ------------------------------------------------------ */

START_TEST(test_share_host_list_populated)
{
	struct inode *inode = alloc_test_inode();
	struct test_lock_owner *o = test_owner_alloc(1);
	ck_assert(o != NULL);

	CDS_LIST_HEAD(host);

	struct reffs_share *s = test_share_alloc(inode, o, FSM_DN, FSA_RW);
	ck_assert(s != NULL);
	ck_assert_int_eq(reffs_share_add(inode, s, &host), 0);

	ck_assert(!cds_list_empty(&host));

	struct test_lock_owner *owners[] = { o };
	test_inode_cleanup(inode, owners, 1);
}
END_TEST

START_TEST(test_share_remove_cleans_host_list)
{
	struct inode *inode = alloc_test_inode();
	struct test_lock_owner *o = test_owner_alloc(1);
	ck_assert(o != NULL);

	CDS_LIST_HEAD(host);

	struct reffs_share *s = test_share_alloc(inode, o, FSM_DN, FSA_RW);
	ck_assert(s != NULL);
	ck_assert_int_eq(reffs_share_add(inode, s, &host), 0);
	ck_assert(!cds_list_empty(&host));

	ck_assert_int_eq(reffs_share_remove(inode, &o->base, &host), 0);
	ck_assert(cds_list_empty(&host));

	struct test_lock_owner *owners[] = { o };
	test_inode_cleanup(inode, owners, 1);
}
END_TEST

/* -- suite ----------------------------------------------------------------- */

Suite *lock_share_suite(void)
{
	Suite *s = suite_create("Lock: share_add and share_remove");

	TCase *tc_add = tcase_create("add");
	tcase_add_test(tc_add, test_share_deny_none_vs_deny_none);
	tcase_add_test(tc_add, test_share_deny_read_vs_write_only_access);
	tcase_add_test(tc_add, test_share_conflict_deny_read_vs_read_access);
	tcase_add_test(tc_add, test_share_conflict_deny_write_vs_write_access);
	tcase_add_test(tc_add, test_share_conflict_reverse_direction);
	tcase_add_test(tc_add, test_share_conflict_deny_rw_vs_any_access);
	tcase_add_test(tc_add, test_share_upgrade_same_owner);
	suite_add_tcase(s, tc_add);

	TCase *tc_remove = tcase_create("remove");
	tcase_add_test(tc_remove, test_share_remove_existing);
	tcase_add_test(tc_remove, test_share_remove_wrong_owner_no_effect);
	tcase_add_test(tc_remove, test_share_remove_only_own_entry);
	tcase_add_test(tc_remove, test_share_remove_nonexistent_no_error);
	tcase_add_test(tc_remove, test_share_host_list_populated);
	tcase_add_test(tc_remove, test_share_remove_cleans_host_list);
	suite_add_tcase(s, tc_remove);

	return s;
}

int main(void)
{
	return fs_test_run(lock_share_suite());
}
