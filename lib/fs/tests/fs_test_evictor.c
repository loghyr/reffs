/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * fs_test_evictor.c -- background evictor thread unit tests
 *
 * Tests:
 *   evictor_init_fini           init/fini lifecycle round-trip
 *   evictor_signal_wakes        signal triggers eviction pass
 *   evictor_drain               drain blocks until eviction completes
 *   evictor_signal_after_fini   signal after fini is harmless
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "fs_test_harness.h"
#include <unistd.h>

#include "reffs/evictor.h"

/*
 * The evictor is already running (started by reffs_ns_init in setup).
 * Most tests exercise its API with the live thread.
 */

static void setup(void)
{
	fs_test_setup();
}

static void teardown(void)
{
	fs_test_teardown();
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

/*
 * Verify that init/fini round-trip works.  The global evictor is
 * already running from setup, so we just confirm the mode API works.
 */
START_TEST(test_evictor_init_fini)
{
	/* The evictor was started by reffs_ns_init in setup.
	 * Default mode should be ASYNC. */
	ck_assert_int_eq(evictor_get_mode(), EVICTOR_ASYNC);

	/* Switch to SYNC and back. */
	evictor_set_mode(EVICTOR_SYNC);
	ck_assert_int_eq(evictor_get_mode(), EVICTOR_SYNC);

	evictor_set_mode(EVICTOR_ASYNC);
	ck_assert_int_eq(evictor_get_mode(), EVICTOR_ASYNC);
}
END_TEST

/*
 * Verify that evictor_signal wakes the thread and eviction runs.
 * Create enough inodes to exceed the LRU max, signal the evictor,
 * drain, and verify the count dropped.
 */
START_TEST(test_evictor_signal_wakes)
{
	struct super_block *sb;
	char path[32];
	int i;
	int n = 20;

	sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	ck_assert_ptr_nonnull(sb);

	/* Set a low LRU max so the evictor has work to do. */
	sb->sb_inode_lru_max = 4;
	super_block_put(sb);

	/* Ensure ASYNC mode -- the evictor handles eviction. */
	evictor_set_mode(EVICTOR_ASYNC);

	/* Create files to build LRU pressure. */
	for (i = 0; i < n; i++) {
		snprintf(path, sizeof(path), "/ev_sig_%d", i);
		ck_assert_int_eq(reffs_fs_create(path, S_IFREG | 0644), 0);
	}

	/* Signal and drain -- after this, eviction has run. */
	evictor_signal();
	evictor_drain();

	sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	ck_assert_ptr_nonnull(sb);
	ck_assert_uint_le(sb->sb_inode_lru_count, sb->sb_inode_lru_max);
	super_block_put(sb);

	for (i = 0; i < n; i++) {
		snprintf(path, sizeof(path), "/ev_sig_%d", i);
		ck_assert_int_eq(reffs_fs_unlink(path), 0);
	}
}
END_TEST

/*
 * Verify evictor_drain blocks until a full pass completes.
 * After drain, the LRU count should be within bounds.
 */
START_TEST(test_evictor_drain)
{
	struct super_block *sb;
	char path[32];
	int i;
	int n = 16;

	sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	ck_assert_ptr_nonnull(sb);
	sb->sb_inode_lru_max = 4;
	super_block_put(sb);

	evictor_set_mode(EVICTOR_ASYNC);

	for (i = 0; i < n; i++) {
		snprintf(path, sizeof(path), "/ev_drain_%d", i);
		ck_assert_int_eq(reffs_fs_create(path, S_IFREG | 0644), 0);
	}

	/* Drain is synchronous -- when it returns, eviction is done. */
	evictor_drain();

	sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	ck_assert_ptr_nonnull(sb);
	ck_assert_uint_le(sb->sb_inode_lru_count, sb->sb_inode_lru_max);
	super_block_put(sb);

	for (i = 0; i < n; i++) {
		snprintf(path, sizeof(path), "/ev_drain_%d", i);
		ck_assert_int_eq(reffs_fs_unlink(path), 0);
	}
}
END_TEST

/*
 * Verify that calling evictor_signal after evictor_fini is harmless.
 * The condvar signal on a joined thread with no waiters is a no-op.
 *
 * We shut down and restart the evictor within this test, then confirm
 * the signal doesn't crash.
 */
START_TEST(test_evictor_signal_after_fini)
{
	/* The evictor is running from setup. Shut it down. */
	evictor_fini();

	/* Signal after fini -- must not crash or hang. */
	evictor_signal();
	evictor_signal();

	/* Drain after fini -- must return immediately (running == 0). */
	evictor_drain();

	/* Restart the evictor so teardown (reffs_ns_fini) can shut it
	 * down cleanly without a double-fini. */
	ck_assert_int_eq(evictor_init(), 0);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite assembly                                                       */
/* ------------------------------------------------------------------ */

Suite *fs_evictor_suite(void)
{
	Suite *s = suite_create("fs: background evictor");

	TCase *tc = tcase_create("Evictor lifecycle");
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_evictor_init_fini);
	tcase_add_test(tc, test_evictor_signal_wakes);
	tcase_add_test(tc, test_evictor_drain);
	tcase_add_test(tc, test_evictor_signal_after_fini);
	suite_add_tcase(s, tc);

	return s;
}

int main(void)
{
	return fs_test_run(fs_evictor_suite());
}
