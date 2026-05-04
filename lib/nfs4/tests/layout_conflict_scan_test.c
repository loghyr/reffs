/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * layout_conflict_scan_test -- enumerate sibling layout stateids on an
 * inode for the conflict-detection step of trust-stateid slice 1.
 *
 * The function under test is
 * `stateid_inode_collect_layouts_excluding(inode, exclude_client,
 * &out, &count)` declared in `nfs4/stateid.h`.  It walks the inode's
 * stateid hash table and collects ref-bumped pointers to every
 * Layout_Stateid whose `s_client` differs from `exclude_client`.
 *
 * Slice plan: this is the Mon-PM deliverable of
 * `.claude/design/trust-stateid-slice-1.md`.  Tue wires the scan into
 * `nfs4_op_layoutget` and fires CB_LAYOUTRECALL + REVOKE_STATEID for
 * each entry returned.
 *
 * Tests:
 *   A. inode with no stateids -> count=0, out=NULL
 *   B. inode with only the caller's own stateid -> count=0, out=NULL
 *   C. inode with one other-client stateid -> count=1, out[0]=that one
 *   D. inode with caller + 2 other-client stateids -> count=2 (no caller)
 *   E. inode with 3 other-client stateids and no caller entry -> count=3
 *   F. NULL inode -> count=0, no crash
 *   G. Mixed-tag table (Open + Layout_Stateid for the OTHER client) ->
 *      only the Layout_Stateid is returned
 *   H. Caller frees the array; refs drop without UAF (run under ASAN)
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <check.h>
#include <urcu.h>
#include <urcu/rculfhash.h>

#include "reffs/inode.h"
#include "reffs/stateid.h"
#include "reffs/super_block.h"
#include "nfs4/stateid.h"

#include "nfs4_test_harness.h"

/* ------------------------------------------------------------------ */
/* Fixture                                                             */

static struct super_block *test_sb;
static struct inode *the_inode;

static void scan_setup(void)
{
	nfs4_test_setup();
	test_sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	ck_assert_ptr_nonnull(test_sb);

	uint64_t ino =
		__atomic_add_fetch(&test_sb->sb_next_ino, 1, __ATOMIC_RELAXED);
	the_inode = inode_alloc(test_sb, ino);
	ck_assert_ptr_nonnull(the_inode);
	ck_assert_ptr_nonnull(the_inode->i_stateids);
}

static void scan_teardown(void)
{
	if (the_inode) {
		inode_active_put(the_inode);
		the_inode = NULL;
	}
	if (test_sb) {
		super_block_put(test_sb);
		test_sb = NULL;
	}
	nfs4_test_teardown();
}

/* ------------------------------------------------------------------ */
/* Helpers: insert / remove fake stateids without a live client.       */
/*                                                                     */
/* Pattern lifted from reflected_getattr_test.c:insert_layout_stateid. */
/* The conflict-scan function compares stateid->s_client by pointer    */
/* identity, so we can pass arbitrary `struct client *` cookies        */
/* without standing up a real nfs4_client.                             */

static void insert_layout_with_client(struct layout_stateid *ls,
				      struct client *fake_client, uint32_t id)
{
	memset(ls, 0, sizeof(*ls));
	ls->ls_stid.s_inode = the_inode;
	ls->ls_stid.s_id = id;
	ls->ls_stid.s_tag = Layout_Stateid;
	ls->ls_stid.s_client = fake_client;
	urcu_ref_init(&ls->ls_stid.s_ref);

	cds_lfht_node_init(&ls->ls_stid.s_inode_node);
	rcu_read_lock();
	ls->ls_stid.s_state = STID_IS_INODE_HASHED;
	cds_lfht_add(the_inode->i_stateids, id, &ls->ls_stid.s_inode_node);
	rcu_read_unlock();
}

/* Same as above but with tag = Open_Stateid to test mixed-tag tables. */
static void insert_open_with_client(struct stateid *st,
				    struct client *fake_client, uint32_t id)
{
	memset(st, 0, sizeof(*st));
	st->s_inode = the_inode;
	st->s_id = id;
	st->s_tag = Open_Stateid;
	st->s_client = fake_client;
	urcu_ref_init(&st->s_ref);

	cds_lfht_node_init(&st->s_inode_node);
	rcu_read_lock();
	st->s_state = STID_IS_INODE_HASHED;
	cds_lfht_add(the_inode->i_stateids, id, &st->s_inode_node);
	rcu_read_unlock();
}

static void remove_stateid(struct stateid *st)
{
	rcu_read_lock();
	cds_lfht_del(the_inode->i_stateids, &st->s_inode_node);
	rcu_read_unlock();
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */

START_TEST(test_scan_empty_inode)
{
	struct client *self = (struct client *)0xA;
	struct stateid **out = (struct stateid **)0xdead;
	uint32_t count = 99;

	int rc = stateid_inode_collect_layouts_excluding(the_inode, self, &out,
							 &count);
	ck_assert_int_eq(rc, 0);
	ck_assert_uint_eq(count, 0);
	ck_assert_ptr_null(out);
}
END_TEST

START_TEST(test_scan_only_self)
{
	struct client *self = (struct client *)0xA;
	struct layout_stateid ls;

	insert_layout_with_client(&ls, self, 1);

	struct stateid **out = (struct stateid **)0xdead;
	uint32_t count = 99;

	int rc = stateid_inode_collect_layouts_excluding(the_inode, self, &out,
							 &count);
	ck_assert_int_eq(rc, 0);
	ck_assert_uint_eq(count, 0);
	ck_assert_ptr_null(out);

	remove_stateid(&ls.ls_stid);
}
END_TEST

START_TEST(test_scan_one_other)
{
	struct client *self = (struct client *)0xA;
	struct client *other = (struct client *)0xB;
	struct layout_stateid ls_self, ls_other;

	insert_layout_with_client(&ls_self, self, 1);
	insert_layout_with_client(&ls_other, other, 2);

	struct stateid **out = NULL;
	uint32_t count = 0;

	int rc = stateid_inode_collect_layouts_excluding(the_inode, self, &out,
							 &count);
	ck_assert_int_eq(rc, 0);
	ck_assert_uint_eq(count, 1);
	ck_assert_ptr_nonnull(out);
	ck_assert_ptr_eq(out[0], &ls_other.ls_stid);

	/* Drop the find-ref the scan took. */
	urcu_ref_put(&out[0]->s_ref, NULL);
	free(out);

	remove_stateid(&ls_self.ls_stid);
	remove_stateid(&ls_other.ls_stid);
}
END_TEST

START_TEST(test_scan_two_others_with_self)
{
	struct client *self = (struct client *)0xA;
	struct client *b = (struct client *)0xB;
	struct client *c = (struct client *)0xC;
	struct layout_stateid ls_self, ls_b, ls_c;

	insert_layout_with_client(&ls_self, self, 1);
	insert_layout_with_client(&ls_b, b, 2);
	insert_layout_with_client(&ls_c, c, 3);

	struct stateid **out = NULL;
	uint32_t count = 0;

	int rc = stateid_inode_collect_layouts_excluding(the_inode, self, &out,
							 &count);
	ck_assert_int_eq(rc, 0);
	ck_assert_uint_eq(count, 2);
	ck_assert_ptr_nonnull(out);

	/* Order is unspecified (lfht iteration); accept either permutation. */
	bool saw_b = false, saw_c = false;
	for (uint32_t i = 0; i < count; i++) {
		if (out[i] == &ls_b.ls_stid)
			saw_b = true;
		else if (out[i] == &ls_c.ls_stid)
			saw_c = true;
		ck_assert(out[i]->s_client != self);
		urcu_ref_put(&out[i]->s_ref, NULL);
	}
	ck_assert(saw_b);
	ck_assert(saw_c);
	free(out);

	remove_stateid(&ls_self.ls_stid);
	remove_stateid(&ls_b.ls_stid);
	remove_stateid(&ls_c.ls_stid);
}
END_TEST

START_TEST(test_scan_three_others_no_self)
{
	struct client *self = (struct client *)0xA;
	struct client *b = (struct client *)0xB;
	struct client *c = (struct client *)0xC;
	struct client *d = (struct client *)0xD;
	struct layout_stateid ls_b, ls_c, ls_d;

	insert_layout_with_client(&ls_b, b, 1);
	insert_layout_with_client(&ls_c, c, 2);
	insert_layout_with_client(&ls_d, d, 3);

	struct stateid **out = NULL;
	uint32_t count = 0;

	int rc = stateid_inode_collect_layouts_excluding(the_inode, self, &out,
							 &count);
	ck_assert_int_eq(rc, 0);
	ck_assert_uint_eq(count, 3);
	ck_assert_ptr_nonnull(out);

	for (uint32_t i = 0; i < count; i++) {
		ck_assert(out[i]->s_client != self);
		ck_assert_int_eq(out[i]->s_tag, Layout_Stateid);
		urcu_ref_put(&out[i]->s_ref, NULL);
	}
	free(out);

	remove_stateid(&ls_b.ls_stid);
	remove_stateid(&ls_c.ls_stid);
	remove_stateid(&ls_d.ls_stid);
}
END_TEST

START_TEST(test_scan_null_inode)
{
	struct client *self = (struct client *)0xA;
	struct stateid **out = (struct stateid **)0xdead;
	uint32_t count = 99;

	int rc = stateid_inode_collect_layouts_excluding(NULL, self, &out,
							 &count);
	ck_assert_int_eq(rc, 0);
	ck_assert_uint_eq(count, 0);
	ck_assert_ptr_null(out);
}
END_TEST

START_TEST(test_scan_mixed_tags)
{
	/*
	 * Other-client has BOTH an Open_Stateid and a Layout_Stateid on
	 * the inode.  Only the Layout_Stateid should appear in the
	 * conflict set; the Open_Stateid is not the conflict surface
	 * the slice-1 recall path acts on.
	 */
	struct client *self = (struct client *)0xA;
	struct client *other = (struct client *)0xB;
	struct stateid os_other;
	struct layout_stateid ls_other;

	insert_open_with_client(&os_other, other, 1);
	insert_layout_with_client(&ls_other, other, 2);

	struct stateid **out = NULL;
	uint32_t count = 0;

	int rc = stateid_inode_collect_layouts_excluding(the_inode, self, &out,
							 &count);
	ck_assert_int_eq(rc, 0);
	ck_assert_uint_eq(count, 1);
	ck_assert_ptr_eq(out[0], &ls_other.ls_stid);

	urcu_ref_put(&out[0]->s_ref, NULL);
	free(out);

	remove_stateid(&os_other);
	remove_stateid(&ls_other.ls_stid);
}
END_TEST

/*
 * Stress: 8 other-client layouts; scan returns all 8; caller frees.
 * Catches off-by-one in the realloc-grow loop (the array doubles at
 * 4 -> 8; this exercises the boundary).  Run under ASAN.
 */
START_TEST(test_scan_growth_boundary)
{
	struct client *self = (struct client *)0xA;
	struct layout_stateid ls[8];
	struct client *fakes[8] = {
		(struct client *)0x10, (struct client *)0x20,
		(struct client *)0x30, (struct client *)0x40,
		(struct client *)0x50, (struct client *)0x60,
		(struct client *)0x70, (struct client *)0x80,
	};

	for (uint32_t i = 0; i < 8; i++)
		insert_layout_with_client(&ls[i], fakes[i], 100 + i);

	struct stateid **out = NULL;
	uint32_t count = 0;

	int rc = stateid_inode_collect_layouts_excluding(the_inode, self, &out,
							 &count);
	ck_assert_int_eq(rc, 0);
	ck_assert_uint_eq(count, 8);

	for (uint32_t i = 0; i < count; i++)
		urcu_ref_put(&out[i]->s_ref, NULL);
	free(out);

	for (uint32_t i = 0; i < 8; i++)
		remove_stateid(&ls[i].ls_stid);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */

static Suite *layout_conflict_scan_suite(void)
{
	Suite *s = suite_create("layout_conflict_scan");
	TCase *tc = tcase_create("scan");

	tcase_add_checked_fixture(tc, scan_setup, scan_teardown);
	tcase_add_test(tc, test_scan_empty_inode);
	tcase_add_test(tc, test_scan_only_self);
	tcase_add_test(tc, test_scan_one_other);
	tcase_add_test(tc, test_scan_two_others_with_self);
	tcase_add_test(tc, test_scan_three_others_no_self);
	tcase_add_test(tc, test_scan_null_inode);
	tcase_add_test(tc, test_scan_mixed_tags);
	tcase_add_test(tc, test_scan_growth_boundary);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	return nfs4_test_run(layout_conflict_scan_suite());
}
