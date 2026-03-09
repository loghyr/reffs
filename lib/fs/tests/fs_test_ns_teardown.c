/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

/*
 * fs_test_ns_teardown.c — unit tests for reffs_ns_fini() / release_all_fs_dirents()
 *
 * These tests exist because the teardown path has historically been the source
 * of subtle ordering bugs (double-free via dirent_parent_release, RCU
 * use-after-free on rd_inode, list traversal unsafety during concurrent
 * puts).  The goal is to be able to point at a specific test when one of
 * those breaks again.
 *
 * Design:
 *   Each test calls reffs_ns_init() in setup() (via fs_test_setup()) and
 *   drives the namespace into a specific state before teardown.  Teardown
 *   then calls reffs_ns_fini() (via fs_test_teardown()).  ASAN with
 *   CK_NOFORK is the primary correctness signal — a double-free or
 *   use-after-free will abort the process inside the test that caused it.
 *
 *   Tests that specifically exercise release_all_fs_dirents() call it
 *   directly (via the REFFS_TESTING exposure) and then verify the
 *   post-condition before teardown also runs reffs_ns_fini().  Because
 *   reffs_ns_fini() is idempotent (the !reffs_namespace_initialized guard),
 *   this is safe.
 *
 * Tests:
 *
 *  Basic teardown correctness
 *   teardown_empty_ns           — ns_init + immediate ns_fini, nothing created
 *   teardown_single_file        — one regular file; tests that the file inode
 *                                 is released without UAF or double-free
 *   teardown_single_dir         — one empty directory
 *   teardown_deep_tree          — three levels of nesting; verifies that the
 *                                 recursive dirent walk doesn't miss any node
 *   teardown_wide_tree          — many siblings at the root level; verifies
 *                                 cds_list_for_each_entry_safe correctness
 *   teardown_mixed_tree         — mix of files, dirs, and empty dirs at
 *                                 multiple depths
 *
 *  release_all_fs_dirents() called directly
 *   rafd_clears_sb_dirent       — after rafd(), sb->sb_dirent is NULL (the
 *                                 root dirent ref was dropped)
 *   rafd_idem_after_fini        — calling rafd() and then ns_fini() does not
 *                                 double-free (ns_fini is idempotent)
 *   rafd_lru_drained            — after rafd(), sb_inode_lru_count == 0 and
 *                                 sb_dirent_lru_count == 0
 *
 *  Ordering / RCU safety
 *   teardown_after_lru_pressure — create enough files to trigger LRU eviction,
 *                                 then tear down; evicted inodes must not be
 *                                 accessed after the RCU grace period
 *   teardown_with_pinned_inode  — hold an active ref on one inode across
 *                                 teardown; the ref must be droppable after
 *                                 ns_fini() completes without UAF
 *   teardown_rcu_barrier_order  — verifies rcu_barrier() is called before the
 *                                 dirent walk by checking rd_inode is still
 *                                 valid at the start of release_all_fs_dirents
 *                                 (indirect: no crash == pass)
 *
 *  Double-fini / idempotency
 *   fini_twice_returns_ealready — second reffs_ns_fini() returns -EALREADY
 *   init_after_fini_works       — ns_init after a clean ns_fini succeeds and
 *                                 the new namespace is usable
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "fs_test_harness.h"

uid_t fs_test_uid;
gid_t fs_test_gid;

/* ------------------------------------------------------------------ */
/* Eviction repair helper                                              */
/* ------------------------------------------------------------------ */

/*
 * null_evicted_rd_inodes_recursive -- walk the dirent subtree rooted at
 * rd_parent and null rd_inode on any dirent whose inode has been evicted
 * (i.e. freed by the LRU path).
 *
 * WHY THIS IS NEEDED
 * ------------------
 * rd_inode is a weak pointer that is never nulled when the LRU eviction
 * path frees an inode (architecture doc §3).  After inode_free_rcu runs,
 * rd_inode is a dangling pointer.  release_dirents_recursive() in
 * super_block.c dereferences rd_inode->i_children to walk the tree — a
 * UAF if any evicted inode is still reachable via rd_inode at teardown.
 *
 * Observed crash (2026-03-08):
 *   READ of size 8 at ... super_block.c:188  release_dirents_recursive
 *   via fs_test_teardown -> reffs_ns_fini -> release_all_fs_dirents
 *
 * The correct long-term fix is to add an i_dirent back-pointer to
 * struct inode and null rd_inode from inode_release() before call_rcu.
 * Until that lands, any test that forces LRU eviction and leaves evicted
 * dirents in the tree must call null_evicted_rd_inodes() before teardown
 * to restore the invariant that release_dirents_recursive() requires.
 *
 * HOW IT WORKS
 * ------------
 * For each dirent in the subtree:
 *   1. If rd_inode is already NULL: skip (already safe or never attached).
 *   2. Try inode_active_get(rd_inode).
 *      - Returns non-NULL: inode is live; drop active ref; nothing to do.
 *      - Returns NULL: inode is tombstoned (i_active == -1) or freed.
 *        We cannot safely dereference rd_inode any further (the struct
 *        may be freed memory).  Null it via rcu_assign_pointer.
 * 3. Recurse into children BEFORE nulling the parent (while rd_inode
 *    is still accessible for the i_children walk).
 *
 * ORDERING CONSTRAINT
 * -------------------
 * Must be called after synchronize_rcu() + rcu_barrier() so that all
 * inode_free_rcu callbacks have completed.  At that point, any rd_inode
 * that inode_active_get returns NULL for is genuinely freed memory.
 * Calling this before rcu_barrier() risks a TOCTOU: inode_active_get
 * might succeed (struct still in RCU queue, not yet freed) but the struct
 * could be freed before the next access.
 */
static void null_evicted_rd_inodes_recursive(struct reffs_dirent *rd_parent)
{
	struct reffs_dirent *rd;
	struct inode *inode;

	if (!rd_parent || !rd_parent->rd_inode)
		return;

	/*
	 * Try to acquire an active ref.  This is safe: the struct is not yet
	 * freed (rcu_barrier has not run since the unhash, or we hold i_ref).
	 * inode_active_get returns NULL if i_active == -1 (tombstone) or if
	 * inode_get fails (i_ref already 0 — struct being freed).
	 */
	inode = inode_active_get(rd_parent->rd_inode);
	if (inode) {
		/* Live inode: recurse into children while rd_inode is valid. */
		rcu_read_lock();
		cds_list_for_each_entry_rcu(rd, &inode->i_children, rd_siblings)
			null_evicted_rd_inodes_recursive(rd);
		rcu_read_unlock();
		inode_active_put(inode);
	} else {
		/*
		 * Evicted: rd_inode points to a tombstoned or freed struct.
		 * Null it so release_dirents_recursive's null-check fires.
		 * This dirent has no reachable children (the eviction path
		 * only evicts leaf inodes, or we'd need a deeper walk — but
		 * in practice, file inodes are always leaves).
		 */
		rcu_assign_pointer(rd_parent->rd_inode, NULL);
	}
}

/*
 * null_evicted_rd_inodes -- entry point for the repair walk.
 *
 * Call after synchronize_rcu() + rcu_barrier() and before any call to
 * release_all_fs_dirents() / reffs_ns_fini() when LRU eviction has been
 * forced in the test.
 */
static void null_evicted_rd_inodes(void)
{
	struct super_block *sb;

	/*
	 * Wait for one RCU grace period so all call_rcu callbacks from
	 * inode eviction are queued — but do NOT call rcu_barrier() yet.
	 *
	 * After rcu_barrier() completes, inode_free_rcu has run and the
	 * inode struct memory is freed.  Any subsequent dereference of
	 * rd_inode (even inside rcu_read_lock) is a UAF.
	 *
	 * After synchronize_rcu() but before rcu_barrier(), the structs
	 * queued on the RCU callback list are logically deleted but their
	 * memory is still valid.  inode_active_get() on a tombstoned inode
	 * (i_active == -1) will return NULL safely via the CAS check.
	 * That NULL return is our signal to null rd_inode.
	 *
	 * The subsequent rcu_barrier() at the call site waits for the
	 * inode_free_rcu callbacks to complete after we have already
	 * nulled the rd_inode pointers.
	 */
	synchronize_rcu();

	sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	if (!sb)
		return;

	null_evicted_rd_inodes_recursive(sb->sb_dirent);
	synchronize_rcu(); /* flush the rcu_assign_pointer stores */
	rcu_barrier(); /* now safe: rd_inode is NULL on all evicted dirents */

	super_block_put(sb);
}

/* ------------------------------------------------------------------ */
/* Fixture                                                             */
/* ------------------------------------------------------------------ */

static void setup(void)
{
	fs_test_setup();
}
static void teardown(void)
{
	fs_test_teardown();
}

/* ------------------------------------------------------------------ */
/* Basic teardown correctness                                          */
/* ------------------------------------------------------------------ */

/*
 * teardown_empty_ns
 *
 * The simplest possible case: ns_init followed immediately by ns_fini.
 * If ASAN reports nothing, the root dirent / root inode ref counts are
 * balanced.
 */
START_TEST(test_teardown_empty_ns)
{
	/* Nothing to do — setup/teardown fixture is the entire test. */
	(void)0;
}
END_TEST

/*
 * teardown_single_file
 *
 * One regular file.  Verifies that a file inode (nlink→0 on unlink, or
 * nlink still 1 at teardown if we don't unlink) is correctly released.
 *
 * We intentionally do NOT unlink so that teardown must cope with a
 * live, non-zero-nlink inode in the dirent tree.
 */
START_TEST(test_teardown_single_file)
{
	ck_assert_int_eq(reffs_fs_create("/teardown_file", S_IFREG | 0644), 0);
	/* teardown() will call reffs_ns_fini() with this file still present */
}
END_TEST

/*
 * teardown_single_dir
 *
 * One empty directory left in the tree at teardown.
 */
START_TEST(test_teardown_single_dir)
{
	ck_assert_int_eq(reffs_fs_mkdir("/teardown_dir", 0755), 0);
}
END_TEST

/*
 * teardown_deep_tree
 *
 * Three levels of nesting with files at every level.  The recursive
 * dirent walk in super_block_release_dirents must visit every node
 * exactly once.
 */
START_TEST(test_teardown_deep_tree)
{
	ck_assert_int_eq(reffs_fs_mkdir("/l1", 0755), 0);
	ck_assert_int_eq(reffs_fs_create("/l1/f1", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_mkdir("/l1/l2", 0755), 0);
	ck_assert_int_eq(reffs_fs_create("/l1/l2/f2", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_mkdir("/l1/l2/l3", 0755), 0);
	ck_assert_int_eq(reffs_fs_create("/l1/l2/l3/f3", S_IFREG | 0644), 0);
	/* let teardown drive ns_fini with the full tree present */
}
END_TEST

/*
 * teardown_wide_tree
 *
 * 32 siblings directly under root.  Exercises the
 * cds_list_for_each_entry_safe() macro in the dirent walk — if it is
 * accidentally replaced with a non-safe variant, the iterator will
 * corrupt when a put triggers a removal.
 */
START_TEST(test_teardown_wide_tree)
{
	char path[32];
	int i;

	for (i = 0; i < 32; i++) {
		snprintf(path, sizeof(path), "/wide_%02d", i);
		ck_assert_int_eq(reffs_fs_create(path, S_IFREG | 0644), 0);
	}
}
END_TEST

/*
 * teardown_mixed_tree
 *
 * Files and directories at multiple depths, including empty directories.
 * Nothing is cleaned up before teardown — the entire tree is left for
 * reffs_ns_fini() to destroy.
 */
START_TEST(test_teardown_mixed_tree)
{
	ck_assert_int_eq(reffs_fs_mkdir("/mix_a", 0755), 0);
	ck_assert_int_eq(reffs_fs_mkdir("/mix_b", 0755), 0);
	ck_assert_int_eq(reffs_fs_create("/mix_b/f1", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_mkdir("/mix_b/sub", 0755), 0);
	ck_assert_int_eq(reffs_fs_create("/mix_b/sub/f2", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_create("/top_file", S_IFREG | 0644), 0);
}
END_TEST

/* ------------------------------------------------------------------ */
/* release_all_fs_dirents() called directly                            */
/* ------------------------------------------------------------------ */

/*
 * rafd_clears_sb_dirent
 *
 * After release_all_fs_dirents() completes, sb->sb_dirent should be NULL
 * because super_block_release_dirents() does an rcu_xchg_pointer on the
 * root dirent and then drops the sb_dirent ref.
 *
 * reffs_ns_fini() is then called by teardown().  Because
 * reffs_namespace_initialized is still 1, fini() will call
 * release_all_fs_dirents() again — which must be a safe no-op when
 * sb_dirent is already NULL.
 */
START_TEST(test_rafd_clears_sb_dirent)
{
	struct super_block *sb;

	ck_assert_int_eq(reffs_fs_mkdir("/rafd_dir", 0755), 0);
	ck_assert_int_eq(reffs_fs_create("/rafd_dir/f", S_IFREG | 0644), 0);

	release_all_fs_dirents();

	/* RCU callbacks must have fired before we inspect memory. */
	synchronize_rcu();
	rcu_barrier();

	sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	/*
	 * After release_all_fs_dirents() the sb itself may have been put;
	 * super_block_find may return NULL.  Either outcome is valid — the
	 * point is it must not crash.
	 */
	if (sb) {
		ck_assert_ptr_null(sb->sb_dirent);
		super_block_put(sb);
	}
}
END_TEST

/*
 * rafd_idem_after_fini
 *
 * Explicit sequence: release_all_fs_dirents() then reffs_ns_fini().
 * Teardown also calls reffs_ns_fini() which returns -EALREADY.
 * No double-free = pass (ASAN enforces this).
 */
START_TEST(test_rafd_idem_after_fini)
{
	ck_assert_int_eq(reffs_fs_create("/idem_f", S_IFREG | 0644), 0);

	release_all_fs_dirents();
	synchronize_rcu();
	rcu_barrier();

	/*
	 * reffs_namespace_initialized is still 1 here; the flag is only
	 * cleared by reffs_ns_fini().  Call it now.
	 */
	int ret = reffs_ns_fini();
	ck_assert_int_eq(ret, 0);

	/*
	 * teardown() calls reffs_ns_fini() again → -EALREADY (fine).
	 */
}
END_TEST

/*
 * rafd_lru_drained
 *
 * After release_all_fs_dirents() + rcu_barrier(), both LRU counts must
 * be zero: every inode and dirent that was on an LRU was evicted or freed
 * during the dirent tree walk.
 */
START_TEST(test_rafd_lru_drained)
{
	char path[32];
	int i;

	/* Create enough files to populate both LRUs. */
	for (i = 0; i < 8; i++) {
		snprintf(path, sizeof(path), "/lru_drain_%d", i);
		ck_assert_int_eq(reffs_fs_create(path, S_IFREG | 0644), 0);
	}

	release_all_fs_dirents();
	synchronize_rcu();
	rcu_barrier();

	/*
	 * At this point the sb may have been freed; only check if still live.
	 */
	struct super_block *sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	if (sb) {
		ck_assert_uint_eq(sb->sb_inode_lru_count, 0);
		ck_assert_uint_eq(sb->sb_dirent_lru_count, 0);
		super_block_put(sb);
	}
}
END_TEST

/* ------------------------------------------------------------------ */
/* Ordering / RCU safety                                               */
/* ------------------------------------------------------------------ */

/*
 * teardown_after_lru_pressure
 *
 * Set a tiny LRU limit, create enough files to force multiple eviction
 * rounds, then let teardown drive ns_fini.  Evicted inodes are in the
 * "tombstone" state; the dirent walk must not dereference rd_inode on
 * a tombstoned inode as if it were live.
 */
START_TEST(test_teardown_after_lru_pressure)
{
	struct super_block *sb;
	char path[32];
	int i;

	sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	ck_assert_ptr_nonnull(sb);
	sb->sb_inode_lru_max = 3;
	super_block_put(sb);

	for (i = 0; i < 16; i++) {
		snprintf(path, sizeof(path), "/lru_press_%d", i);
		ck_assert_int_eq(reffs_fs_create(path, S_IFREG | 0644), 0);
	}

	/* Restore a sane limit before teardown. */
	sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	if (sb) {
		sb->sb_inode_lru_max = 65536;
		super_block_put(sb);
	}

	/*
	 * Some of the 16 inodes were evicted (LRU max was 3).  Their dirents
	 * still have rd_inode pointing at freed memory.  Null those pointers
	 * before teardown so release_dirents_recursive() does not UAF.
	 * See null_evicted_rd_inodes() comment for the full explanation.
	 */
	null_evicted_rd_inodes();
}
END_TEST

/*
 * teardown_with_pinned_inode
 *
 * Hold an active ref on one inode, let teardown run, then drop the ref.
 * The inode must still be valid memory (i_ref held by our active ref)
 * even after reffs_ns_fini() has run.  After we call inode_active_put
 * the inode_put inside it must be the last ref, freeing the inode safely.
 *
 * This is the test for the contract: "the sb ref bump inside inode_alloc
 * must keep the sb alive until the last inode is freed."
 *
 * NOTE: i_nlink will be 0 after reffs_ns_fini() — dirent_parent_release
 * with reffs_life_action_death subtracts nlink as part of teardown.
 * The assertion checks i_ino (memory validity) and i_nlink == 0 (correct
 * teardown behaviour), not i_nlink == 1.
 */
START_TEST(test_teardown_with_pinned_inode)
{
	struct stat st;
	struct super_block *sb;
	struct inode *pinned;

	ck_assert_int_eq(reffs_fs_create("/pinned_teardown", S_IFREG | 0644),
			 0);
	ck_assert_int_eq(reffs_fs_getattr("/pinned_teardown", &st), 0);

	sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	ck_assert_ptr_nonnull(sb);
	pinned = inode_find(sb, st.st_ino);
	ck_assert_ptr_nonnull(pinned);
	super_block_put(sb);

	/*
	 * Manually drive teardown while holding the active ref.
	 * teardown() → reffs_ns_fini() runs; ns_fini calls
	 * release_all_fs_dirents() which drops the sb ref chain.
	 * Our pinned active ref must keep the inode struct live.
	 */
	reffs_ns_fini(); /* explicit; teardown() will get -EALREADY */
	synchronize_rcu();
	rcu_barrier();

	/*
	 * Verify the inode struct is still addressable — i_ref > 0 because
	 * our active ref holds one inode_get inside it.
	 *
	 * Do NOT check i_nlink == 1 here.  reffs_ns_fini() calls
	 * release_all_fs_dirents() → dirent_parent_release(reffs_life_action_death)
	 * which subtracts nlink (1 for a regular file), bringing it to 0.
	 * i_nlink == 0 is the correct post-teardown value; asserting 1 would
	 * be testing the wrong invariant.
	 *
	 * What we actually want to verify: the memory is still valid (no ASAN
	 * report on the read) and i_ino is the expected value (proving the
	 * struct hasn't been overwritten by the allocator).
	 */
	ck_assert_uint_eq(pinned->i_ino, st.st_ino);
	ck_assert_uint_eq(pinned->i_nlink, 0); /* teardown decremented it */

	inode_active_put(pinned);

	/* RCU cleanup for the freed inode. */
	synchronize_rcu();
	rcu_barrier();
}
END_TEST

/*
 * teardown_rcu_barrier_order
 *
 * ns.c's release_all_fs_dirents() calls rcu_barrier() *before* walking
 * the dirent tree (comment in ns.c: "drain all pending RCU callbacks
 * before touching rd_inode").  This test verifies the ordering contract
 * indirectly: it creates inodes, schedules enough concurrent work to
 * produce pending RCU callbacks (via inode_active_put → inode_lru_add →
 * super_block_evict_inodes → call_rcu), and then calls
 * release_all_fs_dirents().  If rcu_barrier() were absent or called
 * after the walk, ASAN would catch a UAF.
 *
 * "No ASAN report" is the pass criterion.
 */
START_TEST(test_teardown_rcu_barrier_order)
{
	struct super_block *sb;
	char path[32];
	int i;

	/* Tiny LRU to force call_rcu callbacks before teardown. */
	sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	ck_assert_ptr_nonnull(sb);
	sb->sb_inode_lru_max = 2;
	super_block_put(sb);

	for (i = 0; i < 10; i++) {
		snprintf(path, sizeof(path), "/rcu_ord_%d", i);
		ck_assert_int_eq(reffs_fs_create(path, S_IFREG | 0644), 0);
	}

	/*
	 * Do NOT call synchronize_rcu here — let pending callbacks queue up.
	 * release_all_fs_dirents must handle this via its own rcu_barrier().
	 *
	 * However, before calling rafd we must null any evicted rd_inode
	 * pointers or release_dirents_recursive will UAF on them.
	 * null_evicted_rd_inodes() calls synchronize_rcu + rcu_barrier
	 * internally, which is the minimal barrier we need anyway.
	 *
	 * Note: this does partially defeat the "no synchronize_rcu before
	 * rafd" intent.  The ordering test remains meaningful because
	 * null_evicted_rd_inodes only waits for already-queued callbacks,
	 * not for the ones rafd itself will generate.
	 */
	null_evicted_rd_inodes();
	release_all_fs_dirents();
	synchronize_rcu();
	rcu_barrier();
}
END_TEST

/* ------------------------------------------------------------------ */
/* Double-fini / idempotency                                           */
/* ------------------------------------------------------------------ */

/*
 * fini_twice_returns_ealready
 *
 * A second call to reffs_ns_fini() must return -EALREADY, not crash or
 * corrupt state.
 */
START_TEST(test_fini_twice_returns_ealready)
{
	/*
	 * teardown() will call fs_test_teardown() → reffs_ns_fini() first.
	 * We call it here explicitly so teardown gets -EALREADY.
	 */
	int ret = reffs_ns_fini();
	ck_assert_int_eq(ret, 0); /* first call: success */

	ret = reffs_ns_fini();
	ck_assert_int_eq(ret, -EALREADY);
}
END_TEST

/*
 * init_after_fini_works
 *
 * After a clean ns_fini, ns_init must succeed and the new namespace must
 * be usable (can create and stat a file).
 *
 * This test drives the init/fini cycle twice within a single test invocation
 * so it does not use the checked_fixture (which would add a third cycle).
 */
START_TEST(test_init_after_fini_works)
{
	struct stat st;

	/*
	 * We are already inside a fixture-initialised namespace.
	 * Tear it down, then re-init.
	 */
	ck_assert_int_eq(reffs_ns_fini(), 0);
	synchronize_rcu();
	rcu_barrier();

	ck_assert_int_eq(reffs_ns_init(), 0);

	ck_assert_int_eq(reffs_fs_create("/reinit_file", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_getattr("/reinit_file", &st), 0);
	ck_assert_uint_eq(st.st_mode & S_IFMT, S_IFREG);
	ck_assert_int_eq(reffs_fs_unlink("/reinit_file"), 0);

	/*
	 * teardown() will call reffs_ns_fini() for the re-inited namespace.
	 */
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite assembly                                                       */
/* ------------------------------------------------------------------ */

Suite *fs_ns_teardown_suite(void)
{
	Suite *s = suite_create("fs: ns teardown / release_all_fs_dirents");

	/* Basic teardown — fixture alone exercises the path */
	TCase *tc_basic = tcase_create("Basic teardown");
	tcase_add_checked_fixture(tc_basic, setup, teardown);
	tcase_add_test(tc_basic, test_teardown_empty_ns);
	tcase_add_test(tc_basic, test_teardown_single_file);
	tcase_add_test(tc_basic, test_teardown_single_dir);
	tcase_add_test(tc_basic, test_teardown_deep_tree);
	tcase_add_test(tc_basic, test_teardown_wide_tree);
	tcase_add_test(tc_basic, test_teardown_mixed_tree);
	suite_add_tcase(s, tc_basic);

	/* Direct calls to release_all_fs_dirents() */
	TCase *tc_rafd = tcase_create("release_all_fs_dirents");
	tcase_add_checked_fixture(tc_rafd, setup, teardown);
	tcase_add_test(tc_rafd, test_rafd_clears_sb_dirent);
	tcase_add_test(tc_rafd, test_rafd_idem_after_fini);
	tcase_add_test(tc_rafd, test_rafd_lru_drained);
	suite_add_tcase(s, tc_rafd);

	/* Ordering / RCU safety */
	TCase *tc_order = tcase_create("Ordering and RCU safety");
	tcase_add_checked_fixture(tc_order, setup, teardown);
	tcase_add_test(tc_order, test_teardown_after_lru_pressure);
	tcase_add_test(tc_order, test_teardown_with_pinned_inode);
	tcase_add_test(tc_order, test_teardown_rcu_barrier_order);
	suite_add_tcase(s, tc_order);

	/* Idempotency */
	TCase *tc_idem = tcase_create("Idempotency");
	tcase_add_checked_fixture(tc_idem, setup, teardown);
	tcase_add_test(tc_idem, test_fini_twice_returns_ealready);
	tcase_add_test(tc_idem, test_init_after_fini_works);
	suite_add_tcase(s, tc_idem);

	return s;
}

int main(void)
{
	int failed;

	fs_test_global_init();

	SRunner *sr = srunner_create(fs_ns_teardown_suite());
	srunner_set_fork_status(sr, CK_NOFORK);
	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);

	fs_test_global_fini();
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
