/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * fs_test_lru.c — inode LRU eviction unit tests
 *
 * Each test sets sb->sb_inode_lru_max to TEST_LRU_MAX=4 after setup so
 * that eviction pressure is triggered with a handful of creates.
 *
 * All reffs_fs_*() operations and vfs_remove_common_locked() now call
 * inode_active_get() on rd_inode after the path walk (fs.c / vfs.c fix,
 * 2026-03-09), so drain_lru() followed by reffs_fs_*() calls is safe.
 * All fault-in tests run unconditionally.
 *
 * drain_lru() is now safe in all tests: i_dirent back-pointer (inode.h)
 * causes inode_release() to null rd_inode via rcu_assign_pointer before
 * call_rcu, so no dirent can carry a freed-inode rd_inode pointer into
 * teardown.  The null_evicted_rd_inodes() workaround in fs_test_ns_teardown.c
 * has been removed.
 *
 * Tests:
 *
 *  Eviction accounting and pressure
 *   lru_count_at_idle                  lru_count ≤ lru_max after idle creates
 *   lru_eviction_fires                 lru_count ≤ lru_max after lru_max+1 creates
 *   lru_count_never_exceeds_max        invariant holds across 3×lru_max creates
 *   lru_active_ref_pins_inode          held active ref survives eviction flood
 *   lru_no_leak_on_rmdir               lru_count stable after mkdir+rmdir
 *   lru_no_leak_on_unlink              lru_count stable after create+unlink
 *   lru_burst_creates                  4×lru_max creates+stats+removes
 *   lru_lru_max_1                      lru_max=1, every put evicts
 *   lru_create_write_survives_eviction Bug G regression (organic pressure)
 *
 *  Structural eviction probe
 *   lru_eviction_produces_tombstone    after organic eviction, i_active == -1
 *                                      and rd_ino is still set (Bug F check)
 *
 *  Eviction fault-in (drain then reffs_fs_*)
 *   lru_inode_find_after_evict         drain then getattr; must return 0
 *   lru_rd_ino_set_on_create           Bug F regression via drain+getattr
 *   lru_ensure_inode_after_evict       drain; reloaded inode has correct attrs
 *   lru_dir_evict_and_readdir          drain parent+children; stat all
 *   lru_nlink_survives_eviction        drain; nlink correct after reload
 *   lru_rename_across_eviction         rename; drain; old=ENOENT, new=same ino
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "fs_test_harness.h"
#include "posix_recovery.h"
#include <urcu/call-rcu.h>

#include "reffs/evictor.h"

#define TEST_LRU_MAX 4

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static size_t lru_count(void)
{
	struct super_block *sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	ck_assert_ptr_nonnull(sb);
	size_t n = sb->sb_inode_lru_count;
	super_block_put(sb);
	return n;
}

/*
 * drain_lru — evict every idle inode and wait for RCU callbacks.
 *
 * After this returns:
 *  - Every inode that was on the LRU has i_active == -1 (tombstone).
 *  - inode_free_rcu has been called for each; memory is freed.
 *
 * *** HAZARD: do not call this if teardown will walk the dirent tree ***
 * *** while files created before the drain are still in the tree.    ***
 *
 * release_dirents_recursive() (super_block.c:159) previously walked
 * i_children via rd_inode; after the rd_children refactor the child list
 * lives on the dirent itself, so rd_inode eviction no longer affects
 * teardown correctness.  The hazard note above is now historical.
 */
static void drain_lru(void)
{
	struct super_block *sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	ck_assert_ptr_nonnull(sb);
	if (sb->sb_inode_lru_count > 0)
		super_block_evict_inodes(sb, sb->sb_inode_lru_count);
	super_block_put(sb);
	synchronize_rcu();
	/* No rcu_barrier() here — we need inode structs to remain in memory
         * so dirent_ensure_inode can safely read i_active and return
         * NULL on a tombstone.  inode_free_rcu runs in the background; by the
         * time it fires, any caller that got NULL has already reloaded via
         * inode_alloc and replaced rd_inode with a fresh pointer.
         *
         * rcu_barrier() would complete inode_free_rcu synchronously, leaving
         * rd_inode as a non-NULL pointer to freed memory — inode_active_get
         * would then crash reading urcu_ref before it could check i_active.
         */
}

/* ------------------------------------------------------------------ */
/* Fixtures                                                            */
/* ------------------------------------------------------------------ */

/*
 * Each test case gets a fresh POSIX-backed superblock in a mkdtemp
 * directory so that eviction fault-in tests exercise real on-disk I/O
 * rather than the RAM backend's in-memory short-circuit.
 *
 * test_context owns the temp dir and the POSIX superblock (id=1, "/").
 * We stash it in a static so setup/teardown — which receive no args from
 * libcheck — can share it.
 */
static struct test_context lru_ctx;

static void setup(void)
{
	ck_assert_int_eq(test_setup(&lru_ctx), 0);

	/*
	 * Force synchronous eviction so tests can assert LRU count
	 * immediately after creates without timing dependency on the
	 * background evictor thread.
	 */
	evictor_set_mode(EVICTOR_SYNC);

	struct super_block *sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	ck_assert_ptr_nonnull(sb);
	sb->sb_inode_lru_max = TEST_LRU_MAX;
	super_block_put(sb);
}

static void teardown(void)
{
	struct super_block *sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	if (sb) {
		sb->sb_inode_lru_max = 65536;
		super_block_put(sb);
	}
	test_teardown(&lru_ctx);
}

/* ------------------------------------------------------------------ */
/* Always-run tests                                                    */
/* ------------------------------------------------------------------ */

START_TEST(test_lru_count_at_idle)
{
	char path[32];
	int i;

	for (i = 0; i < TEST_LRU_MAX; i++) {
		snprintf(path, sizeof(path), "/lru_idle_%d", i);
		ck_assert_int_eq(reffs_fs_create(path, S_IFREG | 0644), 0);
	}

	ck_assert_uint_le(lru_count(), (size_t)TEST_LRU_MAX);

	for (i = 0; i < TEST_LRU_MAX; i++) {
		snprintf(path, sizeof(path), "/lru_idle_%d", i);
		ck_assert_int_eq(reffs_fs_unlink(path), 0);
	}
}
END_TEST

START_TEST(test_lru_eviction_fires)
{
	char path[32];
	int i;
	int n = TEST_LRU_MAX + 1;

	for (i = 0; i < n; i++) {
		snprintf(path, sizeof(path), "/evict_fire_%d", i);
		ck_assert_int_eq(reffs_fs_create(path, S_IFREG | 0644), 0);
	}

	ck_assert_uint_le(lru_count(), (size_t)TEST_LRU_MAX);

	for (i = 0; i < n; i++) {
		snprintf(path, sizeof(path), "/evict_fire_%d", i);
		ck_assert_int_eq(reffs_fs_unlink(path), 0);
	}
}
END_TEST

START_TEST(test_lru_count_never_exceeds_max)
{
	char path[32];
	int i;
	int n = TEST_LRU_MAX * 3;

	for (i = 0; i < n; i++) {
		snprintf(path, sizeof(path), "/lru_cap_%d", i);
		ck_assert_int_eq(reffs_fs_create(path, S_IFREG | 0644), 0);
		ck_assert_uint_le(lru_count(), (size_t)TEST_LRU_MAX);
	}

	for (i = 0; i < n; i++) {
		snprintf(path, sizeof(path), "/lru_cap_%d", i);
		ck_assert_int_eq(reffs_fs_unlink(path), 0);
	}
}
END_TEST

START_TEST(test_lru_active_ref_pins_inode)
{
	struct stat st;
	struct super_block *sb;
	struct inode *pinned_inode;
	uint64_t pinned_ino;
	char path[32];
	int i;

	ck_assert_int_eq(reffs_fs_create("/pinned", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_getattr("/pinned", &st), 0);
	pinned_ino = st.st_ino;

	sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	ck_assert_ptr_nonnull(sb);
	pinned_inode = inode_find(sb, pinned_ino);
	ck_assert_ptr_nonnull(pinned_inode);
	super_block_put(sb);

	for (i = 0; i < TEST_LRU_MAX * 2; i++) {
		snprintf(path, sizeof(path), "/pin_filler_%d", i);
		ck_assert_int_eq(reffs_fs_create(path, S_IFREG | 0644), 0);
	}

	/*
	 * With the active ref held, the pinned inode's i_active > 0.
	 * Verify it was NOT tombstoned by the eviction flood.
	 */
	int64_t active;
	__atomic_load(&pinned_inode->i_active, &active, __ATOMIC_ACQUIRE);
	ck_assert_int_gt((int)active, 0);

	inode_active_put(pinned_inode);

	/* Organic pressure only — getattr is safe here. */
	ck_assert_int_eq(reffs_fs_getattr("/pinned", &st), 0);
	ck_assert_uint_eq(st.st_ino, pinned_ino);

	ck_assert_int_eq(reffs_fs_unlink("/pinned"), 0);
	for (i = 0; i < TEST_LRU_MAX * 2; i++) {
		snprintf(path, sizeof(path), "/pin_filler_%d", i);
		ck_assert_int_eq(reffs_fs_unlink(path), 0);
	}
}
END_TEST

START_TEST(test_lru_no_leak_on_rmdir)
{
	size_t before, after;

	drain_lru();
	before = lru_count();

	ck_assert_int_eq(reffs_fs_mkdir("/leak_dir", 0755), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/leak_dir"), 0);

	synchronize_rcu();
	rcu_barrier();
	drain_lru();
	after = lru_count();

	ck_assert_uint_eq(after, before);
}
END_TEST

START_TEST(test_lru_no_leak_on_unlink)
{
	size_t before, after;

	drain_lru();
	before = lru_count();

	ck_assert_int_eq(reffs_fs_create("/leak_file", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_unlink("/leak_file"), 0);

	synchronize_rcu();
	rcu_barrier();
	drain_lru();
	after = lru_count();

	ck_assert_uint_eq(after, before);
}
END_TEST

START_TEST(test_lru_burst_creates)
{
	char path[32];
	struct stat st;
	int i;
	int n = TEST_LRU_MAX * 4;

	for (i = 0; i < n; i++) {
		snprintf(path, sizeof(path), "/burst_%d", i);
		ck_assert_int_eq(reffs_fs_create(path, S_IFREG | 0644), 0);
	}

	for (i = 0; i < n; i++) {
		snprintf(path, sizeof(path), "/burst_%d", i);
		ck_assert_int_eq(reffs_fs_getattr(path, &st), 0);
	}

	for (i = 0; i < n; i++) {
		snprintf(path, sizeof(path), "/burst_%d", i);
		ck_assert_int_eq(reffs_fs_unlink(path), 0);
	}
}
END_TEST

START_TEST(test_lru_lru_max_1)
{
	struct super_block *sb;
	struct stat st;
	char path[32];
	int i;
	int n = 6;

	sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	ck_assert_ptr_nonnull(sb);
	sb->sb_inode_lru_max = 1;
	super_block_put(sb);

	for (i = 0; i < n; i++) {
		snprintf(path, sizeof(path), "/max1_%d", i);
		ck_assert_int_eq(reffs_fs_create(path, S_IFREG | 0644), 0);
	}

	for (i = n - 1; i >= 0; i--) {
		snprintf(path, sizeof(path), "/max1_%d", i);
		ck_assert_int_eq(reffs_fs_getattr(path, &st), 0);
		ck_assert_uint_eq(st.st_mode & S_IFMT, S_IFREG);
	}

	for (i = 0; i < n; i++) {
		snprintf(path, sizeof(path), "/max1_%d", i);
		ck_assert_int_eq(reffs_fs_unlink(path), 0);
	}
}
END_TEST

START_TEST(test_lru_create_write_survives_eviction)
{
	char path[32];
	struct stat st;
	int i;
	int n = TEST_LRU_MAX * 3;

	for (i = 0; i < n; i++) {
		snprintf(path, sizeof(path), "/cw_%d", i);
		ck_assert_int_eq(reffs_fs_create(path, S_IFREG | 0644), 0);
	}

	for (i = 0; i < n; i++) {
		snprintf(path, sizeof(path), "/cw_%d", i);
		ck_assert_int_eq(reffs_fs_getattr(path, &st), 0);
		ck_assert_uint_eq(st.st_mode & S_IFMT, S_IFREG);
	}

	for (i = 0; i < n; i++) {
		snprintf(path, sizeof(path), "/cw_%d", i);
		ck_assert_int_eq(reffs_fs_unlink(path), 0);
	}
}
END_TEST

/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/* Structural eviction probe                                           */
/* ------------------------------------------------------------------ */

/*
 * test_lru_eviction_produces_tombstone
 *
 * Verifies the inode lifecycle through organic LRU eviction:
 *
 *   (a) After inode_active_put() the inode goes onto the LRU (i_active==0).
 *   (b) After one more create pushes lru_count over lru_max, the eviction
 *       path fires: i_active becomes -1 (tombstone) and the inode is
 *       unhashed.  Our extra inode_get() keeps the struct live so we can
 *       read i_active safely.
 *   (c) i_ino is preserved in the struct until inode_free_rcu fires.
 *   (d) inode_find() for the evicted ino returns either NULL (fully
 *       unhashed) or a fresh struct with a different address (backend
 *       reload) — never the tombstoned original.
 *
 * Uses organic eviction (one extra create at lru_max=1) rather than
 * drain_lru() so that teardown's dirent walk only encounters live inodes.
 * Both files are unlinked before returning.
 */
START_TEST(test_lru_eviction_produces_tombstone)
{
	struct super_block *sb;
	struct inode *inode;
	struct stat st;
	uint64_t saved_ino;
	int64_t active_val;

	/*
	 * Set lru_max to 1 so a single extra create after active_put is
	 * guaranteed to push lru_count over the limit and fire eviction.
	 */
	sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	ck_assert_ptr_nonnull(sb);
	sb->sb_inode_lru_max = 1;
	super_block_put(sb);

	ck_assert_int_eq(reffs_fs_create("/tombstone_probe", S_IFREG | 0644),
			 0);
	ck_assert_int_eq(reffs_fs_getattr("/tombstone_probe", &st), 0);
	saved_ino = st.st_ino;

	sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	ck_assert_ptr_nonnull(sb);
	inode = inode_find(sb, saved_ino);
	ck_assert_ptr_nonnull(inode);

	/*
	 * Take an extra i_ref BEFORE releasing active, so the struct stays
	 * in memory after eviction fires inode_release → call_rcu.
	 * Paired with inode_put() below.
	 */
	inode_get(inode);

	/*
	 * Release active ref: i_active → 0, inode queued on LRU.
	 * lru_count is now 1 == lru_max; no eviction yet.
	 */
	inode_active_put(inode);
	super_block_put(sb);

	/*
	 * Create one more file.  inode_lru_add() for the new inode sees
	 * lru_count (2) > lru_max (1) and calls super_block_evict_inodes(sb,1).
	 * The eviction path picks the oldest LRU entry (tombstone_probe's
	 * inode), sets i_active = -1, unhashes it, and drops the hash ref.
	 */
	ck_assert_int_eq(reffs_fs_create("/tombstone_trigger", S_IFREG | 0644),
			 0);

	/* Let any pending RCU callbacks from the eviction settle. */
	synchronize_rcu();
	rcu_barrier();

	/* (a) Probe inode must now be tombstoned. */
	__atomic_load(&inode->i_active, &active_val, __ATOMIC_ACQUIRE);
	ck_assert_int_eq((int)active_val, -1);

	/* (b) i_ino is stable in the struct until inode_free_rcu. */
	ck_assert_uint_eq(inode->i_ino, saved_ino);

	/*
	 * (c) inode_find returns either NULL or a fresh struct.
	 *     NULL:     inode was fully unhashed; find misses.
	 *     non-NULL: a fresh allocation from the backend; must be a
	 *               different pointer with i_active > 0.
	 */
	sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	ck_assert_ptr_nonnull(sb);
	struct inode *refound = inode_find(sb, saved_ino);
	if (refound) {
		int64_t refound_active;
		__atomic_load(&refound->i_active, &refound_active,
			      __ATOMIC_ACQUIRE);
		ck_assert_int_gt((int)refound_active, 0);
		ck_assert_ptr_ne(refound, inode);
		inode_active_put(refound);
	}
	super_block_put(sb);

	inode_put(inode);

	ck_assert_int_eq(reffs_fs_unlink("/tombstone_probe"), 0);
	ck_assert_int_eq(reffs_fs_unlink("/tombstone_trigger"), 0);
}
END_TEST
/* ------------------------------------------------------------------ */
/* Eviction fault-in tests                                             */
/* ------------------------------------------------------------------ */

START_TEST(test_lru_inode_find_after_evict)
{
	struct stat st;

	ck_assert_int_eq(reffs_fs_create("/evict_me", S_IFREG | 0644), 0);
	drain_lru();

	ck_assert_int_eq(reffs_fs_getattr("/evict_me", &st), 0);
	ck_assert_uint_eq(st.st_mode & S_IFMT, S_IFREG);

	ck_assert_int_eq(reffs_fs_unlink("/evict_me"), 0);
}
END_TEST

START_TEST(test_lru_rd_ino_set_on_create)
{
	char path[32];
	struct stat st;
	int i;
	int n = TEST_LRU_MAX + 2;

	for (i = 0; i < n; i++) {
		snprintf(path, sizeof(path), "/rd_ino_%d", i);
		ck_assert_int_eq(reffs_fs_create(path, S_IFREG | 0644), 0);
	}

	drain_lru();

	for (i = 0; i < n; i++) {
		snprintf(path, sizeof(path), "/rd_ino_%d", i);
		ck_assert_int_eq(reffs_fs_getattr(path, &st), 0);
		ck_assert_uint_eq(st.st_mode & S_IFMT, S_IFREG);
	}

	for (i = 0; i < n; i++) {
		snprintf(path, sizeof(path), "/rd_ino_%d", i);
		ck_assert_int_eq(reffs_fs_unlink(path), 0);
	}
}
END_TEST

START_TEST(test_lru_ensure_inode_after_evict)
{
	struct stat st_before, st_after;

	ck_assert_int_eq(reffs_fs_mkdir("/ensure_dir", 0755), 0);
	ck_assert_int_eq(reffs_fs_getattr("/ensure_dir", &st_before), 0);

	drain_lru();

	ck_assert_int_eq(reffs_fs_getattr("/ensure_dir", &st_after), 0);
	ck_assert_uint_eq(st_after.st_ino, st_before.st_ino);
	ck_assert_uint_eq(st_after.st_mode, st_before.st_mode);
	ck_assert_uint_eq(st_after.st_nlink, st_before.st_nlink);

	ck_assert_int_eq(reffs_fs_rmdir("/ensure_dir"), 0);
}
END_TEST

START_TEST(test_lru_dir_evict_and_readdir)
{
	struct stat st;
	int i;

	TRACE("test_lru_dir_evict_and_readdir");

	ck_assert_int_eq(reffs_fs_mkdir("/evdir", 0755), 0);
	for (i = 0; i < TEST_LRU_MAX + 1; i++) {
		char path[64];
		snprintf(path, sizeof(path), "/evdir/child_%d", i);
		ck_assert_int_eq(reffs_fs_create(path, S_IFREG | 0644), 0);
	}

	drain_lru();

	ck_assert_int_eq(reffs_fs_getattr("/evdir", &st), 0);
	ck_assert_uint_eq(st.st_mode & S_IFMT, S_IFDIR);

	for (i = 0; i < TEST_LRU_MAX + 1; i++) {
		char path[64];
		snprintf(path, sizeof(path), "/evdir/child_%d", i);
		ck_assert_int_eq(reffs_fs_getattr(path, &st), 0);
		ck_assert_uint_eq(st.st_mode & S_IFMT, S_IFREG);
	}

	for (i = 0; i < TEST_LRU_MAX + 1; i++) {
		char path[64];
		snprintf(path, sizeof(path), "/evdir/child_%d", i);
		ck_assert_int_eq(reffs_fs_unlink(path), 0);
	}
	ck_assert_int_eq(reffs_fs_rmdir("/evdir"), 0);
}
END_TEST

START_TEST(test_lru_nlink_survives_eviction)
{
	struct stat st_before, st_after;

	ck_assert_int_eq(reffs_fs_mkdir("/nlink_parent", 0755), 0);
	ck_assert_int_eq(reffs_fs_mkdir("/nlink_parent/a", 0755), 0);
	ck_assert_int_eq(reffs_fs_mkdir("/nlink_parent/b", 0755), 0);

	ck_assert_int_eq(reffs_fs_getattr("/nlink_parent", &st_before), 0);
	ck_assert_uint_eq(st_before.st_nlink, 4);

	drain_lru();

	ck_assert_int_eq(reffs_fs_getattr("/nlink_parent", &st_after), 0);
	ck_assert_uint_eq(st_after.st_nlink, st_before.st_nlink);

	ck_assert_int_eq(reffs_fs_rmdir("/nlink_parent/a"), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/nlink_parent/b"), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/nlink_parent"), 0);
}
END_TEST

START_TEST(test_lru_rename_across_eviction)
{
	struct stat st_before, st_after;

	ck_assert_int_eq(reffs_fs_create("/rename_src", S_IFREG | 0644), 0);
	ck_assert_int_eq(reffs_fs_getattr("/rename_src", &st_before), 0);

	ck_assert_int_eq(reffs_fs_rename("/rename_src", "/rename_dst"), 0);

	drain_lru();

	ck_assert_int_eq(reffs_fs_getattr("/rename_src", &st_after), -ENOENT);
	ck_assert_int_eq(reffs_fs_getattr("/rename_dst", &st_after), 0);
	ck_assert_uint_eq(st_after.st_ino, st_before.st_ino);

	ck_assert_int_eq(reffs_fs_unlink("/rename_dst"), 0);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite assembly                                                       */
/* ------------------------------------------------------------------ */

Suite *fs_lru_suite(void)
{
	Suite *s = suite_create("fs: inode LRU eviction");

	TCase *tc_always = tcase_create("Eviction accounting and pressure");
	tcase_add_checked_fixture(tc_always, setup, teardown);
	tcase_add_test(tc_always, test_lru_count_at_idle);
	tcase_add_test(tc_always, test_lru_eviction_fires);
	tcase_add_test(tc_always, test_lru_count_never_exceeds_max);
	tcase_add_test(tc_always, test_lru_active_ref_pins_inode);
	tcase_add_test(tc_always, test_lru_no_leak_on_rmdir);
	tcase_add_test(tc_always, test_lru_no_leak_on_unlink);
	tcase_add_test(tc_always, test_lru_burst_creates);
	tcase_add_test(tc_always, test_lru_lru_max_1);
	tcase_add_test(tc_always, test_lru_create_write_survives_eviction);
	suite_add_tcase(s, tc_always);

	TCase *tc_probe = tcase_create("Structural eviction probe");
	tcase_add_checked_fixture(tc_probe, setup, teardown);
	tcase_add_test(tc_probe, test_lru_eviction_produces_tombstone);
	suite_add_tcase(s, tc_probe);

	TCase *tc_faultin = tcase_create("Eviction fault-in");
	tcase_add_checked_fixture(tc_faultin, setup, teardown);
	tcase_add_test(tc_faultin, test_lru_inode_find_after_evict);
	tcase_add_test(tc_faultin, test_lru_rd_ino_set_on_create);
	tcase_add_test(tc_faultin, test_lru_ensure_inode_after_evict);
	tcase_add_test(tc_faultin, test_lru_dir_evict_and_readdir);
	tcase_add_test(tc_faultin, test_lru_nlink_survives_eviction);
	tcase_add_test(tc_faultin, test_lru_rename_across_eviction);
	suite_add_tcase(s, tc_faultin);

	return s;
}

int main(void)
{
	return fs_test_run(fs_lru_suite());
}
