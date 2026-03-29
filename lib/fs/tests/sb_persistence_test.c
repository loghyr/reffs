/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Phase 3 TDD: Superblock persistence tests.
 *
 * Tests for registry save/load round-trips, per-sb config
 * persistence, restart survival, and orphan detection.
 *
 * Each test uses a temporary directory as the state_dir.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <check.h>

#include "reffs/dirent.h"
#include "reffs/fs.h"
#include "reffs/inode.h"
#include "reffs/sb_registry.h"
#include "reffs/super_block.h"
#include "fs_test_harness.h"

static char state_dir[] = "/tmp/reffs-sb-persist-XXXXXX";

static void persist_setup(void)
{
	fs_test_setup();
	ck_assert_ptr_nonnull(mkdtemp(state_dir));
}

static void persist_teardown(void)
{
	/* Clean up temp dir contents. */
	char cmd[512];

	snprintf(cmd, sizeof(cmd), "rm -rf %s", state_dir);
	system(cmd);

	/* Reset template for next test. */
	strcpy(state_dir, "/tmp/reffs-sb-persist-XXXXXX");

	fs_test_teardown();
}

/* ------------------------------------------------------------------ */
/* Registry round-trip                                                 */
/* ------------------------------------------------------------------ */

/*
 * Intent: save the registry with a mounted child sb, then load it
 * back and verify the child sb is recreated with the correct state.
 */
START_TEST(test_registry_persist_load)
{
	/* Create a mount point and mount a child sb. */
	ck_assert_int_eq(reffs_fs_mkdir("/persist_a", 0755), 0);

	struct super_block *child = super_block_alloc(10, (char *)"/persist_a",
						      REFFS_STORAGE_RAM, NULL);

	ck_assert_ptr_nonnull(child);
	ck_assert_int_eq(super_block_dirent_create(child, NULL,
						   reffs_life_action_birth),
			 0);
	ck_assert_int_eq(super_block_mount(child, "/persist_a"), 0);

	/* Save the registry. */
	ck_assert_int_eq(sb_registry_save(state_dir), 0);

	/* Unmount and destroy the child sb (simulate shutdown). */
	super_block_unmount(child);
	super_block_destroy(child);
	super_block_release_dirents(child);
	super_block_put(child);

	/* Load the registry — should recreate the sb. */
	ck_assert_int_eq(sb_registry_load(state_dir), 0);

	/* Verify the child sb was recreated. */
	struct super_block *found = super_block_find(10);

	ck_assert_ptr_nonnull(found);
	ck_assert_uint_eq(super_block_lifecycle(found), SB_MOUNTED);
	super_block_put(found);

	/* Clean up the recreated sb. */
	found = super_block_find(10);
	if (found) {
		super_block_unmount(found);
		super_block_destroy(found);
		super_block_release_dirents(found);
		super_block_put(found);
	}

	ck_assert_int_eq(reffs_fs_rmdir("/persist_a"), 0);
}
END_TEST

/*
 * Intent: save with no child sbs, load succeeds with empty registry.
 */
START_TEST(test_registry_empty_round_trip)
{
	ck_assert_int_eq(sb_registry_save(state_dir), 0);
	ck_assert_int_eq(sb_registry_load(state_dir), 0);
}
END_TEST

/*
 * Intent: load with no registry file returns -ENOENT (fresh start).
 */
START_TEST(test_registry_load_missing)
{
	ck_assert_int_eq(sb_registry_load(state_dir), -ENOENT);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Orphan detection                                                    */
/* ------------------------------------------------------------------ */

/*
 * Intent: create an sb_<id> directory without a registry entry.
 * sb_registry_detect_orphans should find it and return count=1.
 */
START_TEST(test_orphan_detection)
{
	/* Save an empty registry. */
	ck_assert_int_eq(sb_registry_save(state_dir), 0);

	/* Create an orphan directory. */
	char orphan_dir[512];

	snprintf(orphan_dir, sizeof(orphan_dir), "%s/sb_99", state_dir);
	ck_assert_int_eq(mkdir(orphan_dir, 0700), 0);

	/* Detect orphans. */
	int orphans = sb_registry_detect_orphans(state_dir);

	ck_assert_int_ge(orphans, 1);
}
END_TEST

/*
 * Intent: no orphans when all sb dirs match registry entries.
 */
START_TEST(test_no_orphans_when_clean)
{
	ck_assert_int_eq(reffs_fs_mkdir("/clean_a", 0755), 0);

	struct super_block *child = super_block_alloc(20, (char *)"/clean_a",
						      REFFS_STORAGE_RAM, NULL);

	ck_assert_ptr_nonnull(child);
	ck_assert_int_eq(super_block_dirent_create(child, NULL,
						   reffs_life_action_birth),
			 0);
	ck_assert_int_eq(super_block_mount(child, "/clean_a"), 0);

	/* Save registry. */
	ck_assert_int_eq(sb_registry_save(state_dir), 0);

	/* Create the matching sb directory. */
	char sb_dir[512];

	snprintf(sb_dir, sizeof(sb_dir), "%s/sb_20", state_dir);
	mkdir(sb_dir, 0700);

	/* No orphans expected. */
	int orphans = sb_registry_detect_orphans(state_dir);

	ck_assert_int_eq(orphans, 0);

	/* Cleanup. */
	super_block_unmount(child);
	super_block_destroy(child);
	super_block_release_dirents(child);
	super_block_put(child);
	ck_assert_int_eq(reffs_fs_rmdir("/clean_a"), 0);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

static Suite *sb_persistence_suite(void)
{
	Suite *s = suite_create("sb_persistence");
	TCase *tc;

	tc = tcase_create("registry");
	tcase_add_checked_fixture(tc, persist_setup, persist_teardown);
	tcase_add_test(tc, test_registry_persist_load);
	tcase_add_test(tc, test_registry_empty_round_trip);
	tcase_add_test(tc, test_registry_load_missing);
	suite_add_tcase(s, tc);

	tc = tcase_create("orphan");
	tcase_add_checked_fixture(tc, persist_setup, persist_teardown);
	tcase_add_test(tc, test_orphan_detection);
	tcase_add_test(tc, test_no_orphans_when_clean);
	suite_add_tcase(s, tc);

	return s;
}

int main(void)
{
	int nfailed;
	Suite *s = sb_persistence_suite();
	SRunner *sr = srunner_create(s);

	srunner_run_all(sr, CK_NORMAL);
	nfailed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return nfailed ? 1 : 0;
}
