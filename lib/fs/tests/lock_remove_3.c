/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * Unit tests for reffs_lock_remove().
 *
 * reffs_lock_remove() implements POSIX partial-range unlocking.  Given an
 * unlock request [u_off, u_end], for each overlapping lock owned by the
 * same owner it takes one of four actions:
 *
 *   Full removal:    unlock covers the entire lock --> delete + free
 *   Split:           unlock punches a hole in the middle of a lock -->
 *                    shorten existing entry (left fragment) + allocate new
 *                    entry for right fragment
 *   Truncate start:  unlock covers the beginning of the lock -->
 *                    advance l_offset to u_end+1
 *   Truncate end:    unlock covers the tail of the lock -->
 *                    reduce l_len to (u_off - l_off)
 *
 * Additionally:
 *   - Locks belonging to a different owner are never touched.
 *   - Non-overlapping locks belonging to the same owner are never touched.
 *   - len=0 (to end-of-file) is handled correctly in all cases.
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
	static uint64_t next_ino = 500;
	uint64_t ino = __atomic_add_fetch(&next_ino, 1, __ATOMIC_RELAXED);
	struct inode *inode = inode_alloc(g_sb, ino);
	ck_assert(inode != NULL);
	inode->i_mode = S_IFREG | 0644;
	return inode;
}

/* Helper: find the first lock on inode owned by owner. */
static struct reffs_lock *first_lock_for(struct inode *inode,
					 struct test_lock_owner *owner)
{
	struct reffs_lock *rl;
	cds_list_for_each_entry(rl, &inode->i_locks, l_list)
		if (rl->l_owner == &owner->base)
			return rl;
	return NULL;
}

/* -- full removal ----------------------------------------------------------- */

START_TEST(test_remove_full_exact)
{
	/* Unlock exactly matches the lock: [0,99] unlocked by [0,99] */
	struct inode *inode = alloc_test_inode();
	struct test_lock_owner *o = test_owner_alloc(1);
	ck_assert(o != NULL);

	struct reffs_lock *rl = test_lock_alloc(inode, o, 0, 100, true);
	ck_assert(rl != NULL);
	ck_assert_int_eq(reffs_lock_add(inode, rl, NULL), 0);
	ck_assert_int_eq(lock_count(inode), 1);

	ck_assert_int_eq(reffs_lock_remove(inode, 0, 100, &o->base, NULL), 0);
	ck_assert_int_eq(lock_count(inode), 0);

	struct test_lock_owner *owners[] = { o };
	test_inode_cleanup(inode, owners, 1);
}
END_TEST

START_TEST(test_remove_full_superset)
{
	/* Unlock covers more than the lock: lock [10,59], unlock [0,100] */
	struct inode *inode = alloc_test_inode();
	struct test_lock_owner *o = test_owner_alloc(1);
	ck_assert(o != NULL);

	struct reffs_lock *rl = test_lock_alloc(inode, o, 10, 50, true);
	ck_assert(rl != NULL);
	ck_assert_int_eq(reffs_lock_add(inode, rl, NULL), 0);

	ck_assert_int_eq(reffs_lock_remove(inode, 0, 100, &o->base, NULL), 0);
	ck_assert_int_eq(lock_count(inode), 0);

	struct test_lock_owner *owners[] = { o };
	test_inode_cleanup(inode, owners, 1);
}
END_TEST

/* -- truncate end ----------------------------------------------------------- */

START_TEST(test_remove_truncate_end)
{
	/*
	 * Lock [0, 99], unlock [50, 99].
	 * u_off(50) > l_off(0) and u_end(99) >= l_end(99): truncate end.
	 * Remaining lock must be [0, 49] (l_len = 50).
	 */
	struct inode *inode = alloc_test_inode();
	struct test_lock_owner *o = test_owner_alloc(1);
	ck_assert(o != NULL);

	struct reffs_lock *rl = test_lock_alloc(inode, o, 0, 100, true);
	ck_assert(rl != NULL);
	ck_assert_int_eq(reffs_lock_add(inode, rl, NULL), 0);

	ck_assert_int_eq(reffs_lock_remove(inode, 50, 50, &o->base, NULL), 0);
	ck_assert_int_eq(lock_count(inode), 1);

	struct reffs_lock *remaining = first_lock_for(inode, o);
	ck_assert(remaining != NULL);
	ck_assert_uint_eq(remaining->l_offset, 0);
	ck_assert_uint_eq(remaining->l_len, 50);

	struct test_lock_owner *owners[] = { o };
	test_inode_cleanup(inode, owners, 1);
}
END_TEST

/* -- truncate start --------------------------------------------------------- */

START_TEST(test_remove_truncate_start)
{
	/*
	 * Lock [0, 99], unlock [0, 49].
	 * u_off(0) <= l_off(0) and u_end(49) < l_end(99): truncate start.
	 * Remaining lock must be [50, 99] (l_offset=50, l_len=50).
	 */
	struct inode *inode = alloc_test_inode();
	struct test_lock_owner *o = test_owner_alloc(1);
	ck_assert(o != NULL);

	struct reffs_lock *rl = test_lock_alloc(inode, o, 0, 100, true);
	ck_assert(rl != NULL);
	ck_assert_int_eq(reffs_lock_add(inode, rl, NULL), 0);

	ck_assert_int_eq(reffs_lock_remove(inode, 0, 50, &o->base, NULL), 0);
	ck_assert_int_eq(lock_count(inode), 1);

	struct reffs_lock *remaining = first_lock_for(inode, o);
	ck_assert(remaining != NULL);
	ck_assert_uint_eq(remaining->l_offset, 50);
	ck_assert_uint_eq(remaining->l_len, 50);

	struct test_lock_owner *owners[] = { o };
	test_inode_cleanup(inode, owners, 1);
}
END_TEST

/* -- split ------------------------------------------------------------------ */

START_TEST(test_remove_split_middle)
{
	/*
	 * Lock [0, 99], unlock [30, 59].
	 * u_off(30) > l_off(0) and u_end(59) < l_end(99): split.
	 * Left fragment:  [0,  29] (offset=0,  len=30)
	 * Right fragment: [60, 99] (offset=60, len=40)
	 */
	struct inode *inode = alloc_test_inode();
	struct test_lock_owner *o = test_owner_alloc(1);
	ck_assert(o != NULL);

	struct reffs_lock *rl = test_lock_alloc(inode, o, 0, 100, true);
	ck_assert(rl != NULL);
	ck_assert_int_eq(reffs_lock_add(inode, rl, NULL), 0);

	ck_assert_int_eq(reffs_lock_remove(inode, 30, 30, &o->base, NULL), 0);
	ck_assert_int_eq(lock_count(inode), 2);

	/* Collect the two fragments */
	uint64_t off_a = 0, len_a = 0, off_b = 0, len_b = 0;
	struct reffs_lock *le;
	int i = 0;
	cds_list_for_each_entry(le, &inode->i_locks, l_list) {
		if (i == 0) {
			off_a = le->l_offset;
			len_a = le->l_len;
		}
		if (i == 1) {
			off_b = le->l_offset;
			len_b = le->l_len;
		}
		i++;
	}

	/* One fragment must be [0,29] and the other [60,99] */
	bool frag1_ok =
		(off_a == 0 && len_a == 30 && off_b == 60 && len_b == 40);
	bool frag2_ok =
		(off_a == 60 && len_a == 40 && off_b == 0 && len_b == 30);
	ck_assert(frag1_ok || frag2_ok);

	struct test_lock_owner *owners[] = { o };
	test_inode_cleanup(inode, owners, 1);
}
END_TEST

START_TEST(test_remove_split_inherits_exclusivity)
{
	/*
	 * After a split the two fragments must retain the original lock's
	 * exclusivity flag.
	 */
	struct inode *inode = alloc_test_inode();
	struct test_lock_owner *o = test_owner_alloc(1);
	ck_assert(o != NULL);

	/* Exclusive lock [0, 99], punch out [40, 59] */
	struct reffs_lock *rl = test_lock_alloc(inode, o, 0, 100, true);
	ck_assert(rl != NULL);
	ck_assert_int_eq(reffs_lock_add(inode, rl, NULL), 0);

	ck_assert_int_eq(reffs_lock_remove(inode, 40, 20, &o->base, NULL), 0);
	ck_assert_int_eq(lock_count(inode), 2);

	struct reffs_lock *le;
	cds_list_for_each_entry(le, &inode->i_locks, l_list)
		ck_assert(le->l_exclusive);

	struct test_lock_owner *owners[] = { o };
	test_inode_cleanup(inode, owners, 1);
}
END_TEST

/* -- wrong owner / no overlap ----------------------------------------------- */

START_TEST(test_remove_wrong_owner_no_effect)
{
	struct inode *inode = alloc_test_inode();
	struct test_lock_owner *o1 = test_owner_alloc(1);
	struct test_lock_owner *o2 = test_owner_alloc(2);
	ck_assert(o1 && o2);

	struct reffs_lock *rl = test_lock_alloc(inode, o1, 0, 100, true);
	ck_assert(rl != NULL);
	ck_assert_int_eq(reffs_lock_add(inode, rl, NULL), 0);

	/* o2 tries to unlock o1's range: must have no effect */
	ck_assert_int_eq(reffs_lock_remove(inode, 0, 100, &o2->base, NULL), 0);
	ck_assert_int_eq(lock_count(inode), 1);

	struct test_lock_owner *owners[] = { o1, o2 };
	test_inode_cleanup(inode, owners, 2);
}
END_TEST

START_TEST(test_remove_no_overlap_no_effect)
{
	struct inode *inode = alloc_test_inode();
	struct test_lock_owner *o = test_owner_alloc(1);
	ck_assert(o != NULL);

	struct reffs_lock *rl = test_lock_alloc(inode, o, 0, 100, true);
	ck_assert(rl != NULL);
	ck_assert_int_eq(reffs_lock_add(inode, rl, NULL), 0);

	/* Unlock [200, 299]: no overlap with [0, 99] */
	ck_assert_int_eq(reffs_lock_remove(inode, 200, 100, &o->base, NULL), 0);
	ck_assert_int_eq(lock_count(inode), 1);

	struct test_lock_owner *owners[] = { o };
	test_inode_cleanup(inode, owners, 1);
}
END_TEST

/* -- len=0 (to-EOF) unlock semantics --------------------------------------- */

START_TEST(test_remove_len0_unlock_removes_finite_lock)
{
	/*
	 * A to-EOF unlock (len=0 from offset 0) must remove any finite lock
	 * that overlaps.
	 * Lock [100, 199], unlock [0, EOF): full removal.
	 */
	struct inode *inode = alloc_test_inode();
	struct test_lock_owner *o = test_owner_alloc(1);
	ck_assert(o != NULL);

	struct reffs_lock *rl = test_lock_alloc(inode, o, 100, 100, true);
	ck_assert(rl != NULL);
	ck_assert_int_eq(reffs_lock_add(inode, rl, NULL), 0);

	ck_assert_int_eq(reffs_lock_remove(inode, 0, 0, &o->base, NULL), 0);
	ck_assert_int_eq(lock_count(inode), 0);

	struct test_lock_owner *owners[] = { o };
	test_inode_cleanup(inode, owners, 1);
}
END_TEST

START_TEST(test_remove_len0_unlock_truncates_start_of_eof_lock)
{
	/*
	 * Lock [0, EOF) (len=0), unlock [0, 99] (len=100).
	 * Unlock covers the start of the to-EOF lock:
	 *   u_off(0) <= l_off(0), u_end(99) < l_end(UINT64_MAX)
	 *   --> truncate start: new l_offset = 100, new l_len = 0 (still to-EOF).
	 */
	struct inode *inode = alloc_test_inode();
	struct test_lock_owner *o = test_owner_alloc(1);
	ck_assert(o != NULL);

	struct reffs_lock *rl = test_lock_alloc(inode, o, 0, 0, true);
	ck_assert(rl != NULL);
	ck_assert_int_eq(reffs_lock_add(inode, rl, NULL), 0);

	ck_assert_int_eq(reffs_lock_remove(inode, 0, 100, &o->base, NULL), 0);
	ck_assert_int_eq(lock_count(inode), 1);

	struct reffs_lock *remaining = first_lock_for(inode, o);
	ck_assert(remaining != NULL);
	ck_assert_uint_eq(remaining->l_offset, 100);
	ck_assert_uint_eq(remaining->l_len, 0); /* still to-EOF */

	struct test_lock_owner *owners[] = { o };
	test_inode_cleanup(inode, owners, 1);
}
END_TEST

START_TEST(test_remove_multiple_locks_only_matching_owner)
{
	/*
	 * Two owners each hold a lock on the same range.
	 * Unlocking for o1 must only remove o1's lock, leaving o2's intact.
	 */
	struct inode *inode = alloc_test_inode();
	struct test_lock_owner *o1 = test_owner_alloc(1);
	struct test_lock_owner *o2 = test_owner_alloc(2);
	ck_assert(o1 && o2);

	struct reffs_lock *rl1 = test_lock_alloc(inode, o1, 0, 100, false);
	struct reffs_lock *rl2 = test_lock_alloc(inode, o2, 0, 100, false);
	ck_assert(rl1 && rl2);

	ck_assert_int_eq(reffs_lock_add(inode, rl1, NULL), 0);
	ck_assert_int_eq(reffs_lock_add(inode, rl2, NULL), 0);
	ck_assert_int_eq(lock_count(inode), 2);

	ck_assert_int_eq(reffs_lock_remove(inode, 0, 100, &o1->base, NULL), 0);
	ck_assert_int_eq(lock_count(inode), 1);

	/* The surviving lock must belong to o2 */
	struct reffs_lock *le;
	cds_list_for_each_entry(le, &inode->i_locks, l_list)
		ck_assert(le->l_owner == &o2->base);

	struct test_lock_owner *owners[] = { o1, o2 };
	test_inode_cleanup(inode, owners, 2);
}
END_TEST

/* -- suite ----------------------------------------------------------------- */

Suite *lock_remove_suite(void)
{
	Suite *s = suite_create("Lock: remove");

	TCase *tc = tcase_create("Core");
	tcase_add_test(tc, test_remove_full_exact);
	tcase_add_test(tc, test_remove_full_superset);
	tcase_add_test(tc, test_remove_truncate_end);
	tcase_add_test(tc, test_remove_truncate_start);
	tcase_add_test(tc, test_remove_split_middle);
	tcase_add_test(tc, test_remove_split_inherits_exclusivity);
	tcase_add_test(tc, test_remove_wrong_owner_no_effect);
	tcase_add_test(tc, test_remove_no_overlap_no_effect);
	tcase_add_test(tc, test_remove_len0_unlock_removes_finite_lock);
	tcase_add_test(tc, test_remove_len0_unlock_truncates_start_of_eof_lock);
	tcase_add_test(tc, test_remove_multiple_locks_only_matching_owner);
	suite_add_tcase(s, tc);

	return s;
}

int main(void)
{
	return fs_test_run(lock_remove_suite());
}
