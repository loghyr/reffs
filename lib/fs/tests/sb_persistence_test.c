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

#include "reffs/client_match.h"
#include "reffs/dirent.h"
#include "reffs/fs.h"
#include "reffs/inode.h"
#include "reffs/sb_registry.h"
#include "reffs/super_block.h"
#include "reffs/settings.h"
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

	/* Load the registry -- should recreate the sb. */
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
/* UUID persistence                                                    */
/* ------------------------------------------------------------------ */

/*
 * Intent: UUID assigned at creation is preserved across save/load.
 */
START_TEST(test_registry_uuid_persisted)
{
	ck_assert_int_eq(reffs_fs_mkdir("/uuid_a", 0755), 0);

	struct super_block *child = super_block_alloc(10, (char *)"/uuid_a",
						      REFFS_STORAGE_RAM, NULL);

	ck_assert_ptr_nonnull(child);
	uuid_generate(child->sb_uuid);
	ck_assert_int_eq(super_block_dirent_create(child, NULL,
						   reffs_life_action_birth),
			 0);
	ck_assert_int_eq(super_block_mount(child, "/uuid_a"), 0);

	/* Save the original UUID. */
	uuid_t original_uuid;

	uuid_copy(original_uuid, child->sb_uuid);

	/* Save, destroy, reload. */
	ck_assert_int_eq(sb_registry_save(state_dir), 0);
	super_block_unmount(child);
	super_block_destroy(child);
	super_block_release_dirents(child);
	super_block_put(child);

	ck_assert_int_eq(sb_registry_load(state_dir), 0);

	struct super_block *reloaded = super_block_find(10);

	ck_assert_ptr_nonnull(reloaded);
	ck_assert_int_eq(uuid_compare(reloaded->sb_uuid, original_uuid), 0);

	super_block_unmount(reloaded);
	super_block_destroy(reloaded);
	super_block_release_dirents(reloaded);
	super_block_put(reloaded);
	ck_assert_int_eq(reffs_fs_rmdir("/uuid_a"), 0);
}
END_TEST

/*
 * Intent: UUID is stable across TWO save/load cycles (catches
 * bugs where save re-generates instead of copying).
 */
START_TEST(test_registry_uuid_stable_across_restarts)
{
	ck_assert_int_eq(reffs_fs_mkdir("/uuid_b", 0755), 0);

	struct super_block *child = super_block_alloc(11, (char *)"/uuid_b",
						      REFFS_STORAGE_RAM, NULL);

	ck_assert_ptr_nonnull(child);
	uuid_generate(child->sb_uuid);
	ck_assert_int_eq(super_block_dirent_create(child, NULL,
						   reffs_life_action_birth),
			 0);
	ck_assert_int_eq(super_block_mount(child, "/uuid_b"), 0);

	uuid_t original_uuid;

	uuid_copy(original_uuid, child->sb_uuid);

	/* Cycle 1: save, destroy, load. */
	ck_assert_int_eq(sb_registry_save(state_dir), 0);
	super_block_unmount(child);
	super_block_destroy(child);
	super_block_release_dirents(child);
	super_block_put(child);
	ck_assert_int_eq(sb_registry_load(state_dir), 0);

	/* Cycle 2: save again, destroy, load again. */
	ck_assert_int_eq(sb_registry_save(state_dir), 0);
	child = super_block_find(11);
	ck_assert_ptr_nonnull(child);
	super_block_unmount(child);
	super_block_destroy(child);
	super_block_release_dirents(child);
	super_block_put(child);
	ck_assert_int_eq(sb_registry_load(state_dir), 0);

	/* UUID must still match the original. */
	struct super_block *reloaded = super_block_find(11);

	ck_assert_ptr_nonnull(reloaded);
	ck_assert_int_eq(uuid_compare(reloaded->sb_uuid, original_uuid), 0);

	super_block_unmount(reloaded);
	super_block_destroy(reloaded);
	super_block_release_dirents(reloaded);
	super_block_put(reloaded);
	ck_assert_int_eq(reffs_fs_rmdir("/uuid_b"), 0);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Persistent ID counter                                               */
/* ------------------------------------------------------------------ */

/*
 * Intent: alloc_id returns monotonically increasing IDs.
 */
START_TEST(test_alloc_id_monotonic)
{
	uint64_t id1 = sb_registry_alloc_id(state_dir);
	uint64_t id2 = sb_registry_alloc_id(state_dir);
	uint64_t id3 = sb_registry_alloc_id(state_dir);

	ck_assert_uint_ge(id1, SB_REGISTRY_FIRST_ID);
	ck_assert_uint_gt(id2, id1);
	ck_assert_uint_gt(id3, id2);
}
END_TEST

/*
 * Intent: alloc_id counter survives save/load cycle.
 */
START_TEST(test_alloc_id_persists_across_restart)
{
	uint64_t id1 = sb_registry_alloc_id(state_dir);

	/* sb_registry_alloc_id already calls save internally. */
	ck_assert_int_eq(sb_registry_load(state_dir), 0);

	uint64_t id2 = sb_registry_alloc_id(state_dir);

	ck_assert_uint_gt(id2, id1);
}
END_TEST

/*
 * Intent: a destroyed sb's id is never reused.
 */
START_TEST(test_alloc_id_never_reuses)
{
	ck_assert_int_eq(reffs_fs_mkdir("/reuse", 0755), 0);

	uint64_t id1 = sb_registry_alloc_id(state_dir);
	struct super_block *child = super_block_alloc(id1, (char *)"/reuse",
						      REFFS_STORAGE_RAM, NULL);

	ck_assert_ptr_nonnull(child);
	uuid_generate(child->sb_uuid);
	ck_assert_int_eq(super_block_dirent_create(child, NULL,
						   reffs_life_action_birth),
			 0);
	ck_assert_int_eq(super_block_mount(child, "/reuse"), 0);

	/* Destroy the sb. */
	super_block_unmount(child);
	super_block_destroy(child);
	super_block_release_dirents(child);
	super_block_put(child);

	/* Next alloc must return a HIGHER id. */
	uint64_t id2 = sb_registry_alloc_id(state_dir);

	ck_assert_uint_gt(id2, id1);

	ck_assert_int_eq(reffs_fs_rmdir("/reuse"), 0);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Client rules persistence                                            */
/* ------------------------------------------------------------------ */

/*
 * Intent: set 3 client rules on a superblock, save, destroy, reload,
 * and verify all rule fields match (match string, rw, squash, flavors).
 */
START_TEST(test_client_rules_persisted)
{
	ck_assert_int_eq(reffs_fs_mkdir("/clirules", 0755), 0);

	struct super_block *child = super_block_alloc(20, (char *)"/clirules",
						      REFFS_STORAGE_RAM, NULL);

	ck_assert_ptr_nonnull(child);
	uuid_generate(child->sb_uuid);
	ck_assert_int_eq(super_block_dirent_create(child, NULL,
						   reffs_life_action_birth),
			 0);
	ck_assert_int_eq(super_block_mount(child, "/clirules"), 0);

	/* Build 3 test rules. */
	struct sb_client_rule rules[3];

	memset(rules, 0, sizeof(rules));

	strncpy(rules[0].scr_match, "10.0.0.1", SB_CLIENT_MATCH_MAX - 1);
	rules[0].scr_rw = true;
	rules[0].scr_root_squash = true;
	rules[0].scr_all_squash = false;
	rules[0].scr_flavors[0] = REFFS_AUTH_KRB5;
	rules[0].scr_nflavors = 1;

	strncpy(rules[1].scr_match, "192.168.0.0/16", SB_CLIENT_MATCH_MAX - 1);
	rules[1].scr_rw = false;
	rules[1].scr_root_squash = true;
	rules[1].scr_all_squash = false;
	rules[1].scr_flavors[0] = REFFS_AUTH_SYS;
	rules[1].scr_nflavors = 1;

	strncpy(rules[2].scr_match, "*", SB_CLIENT_MATCH_MAX - 1);
	rules[2].scr_rw = true;
	rules[2].scr_root_squash = false;
	rules[2].scr_all_squash = true;
	rules[2].scr_flavors[0] = REFFS_AUTH_SYS;
	rules[2].scr_flavors[1] = REFFS_AUTH_KRB5P;
	rules[2].scr_nflavors = 2;

	super_block_set_client_rules(child, rules, 3);
	ck_assert_uint_eq(child->sb_nclient_rules, 3);

	/* Save registry + client rules. */
	ck_assert_int_eq(sb_registry_save(state_dir), 0);

	super_block_unmount(child);
	super_block_destroy(child);
	super_block_release_dirents(child);
	super_block_put(child);

	/* Load and verify. */
	ck_assert_int_eq(sb_registry_load(state_dir), 0);

	struct super_block *reloaded = super_block_find(20);

	ck_assert_ptr_nonnull(reloaded);
	ck_assert_uint_eq(reloaded->sb_nclient_rules, 3);

	/* Rule 0: exact host */
	ck_assert_str_eq(reloaded->sb_client_rules[0].scr_match, "10.0.0.1");
	ck_assert(reloaded->sb_client_rules[0].scr_rw);
	ck_assert(reloaded->sb_client_rules[0].scr_root_squash);
	ck_assert(!reloaded->sb_client_rules[0].scr_all_squash);
	ck_assert_uint_eq(reloaded->sb_client_rules[0].scr_nflavors, 1);
	ck_assert_int_eq((int)reloaded->sb_client_rules[0].scr_flavors[0],
			 (int)REFFS_AUTH_KRB5);

	/* Rule 1: CIDR, read-only */
	ck_assert_str_eq(reloaded->sb_client_rules[1].scr_match,
			 "192.168.0.0/16");
	ck_assert(!reloaded->sb_client_rules[1].scr_rw);
	ck_assert(reloaded->sb_client_rules[1].scr_root_squash);

	/* Rule 2: wildcard, all_squash, two flavors */
	ck_assert_str_eq(reloaded->sb_client_rules[2].scr_match, "*");
	ck_assert(!reloaded->sb_client_rules[2].scr_root_squash);
	ck_assert(reloaded->sb_client_rules[2].scr_all_squash);
	ck_assert_uint_eq(reloaded->sb_client_rules[2].scr_nflavors, 2);

	super_block_unmount(reloaded);
	super_block_destroy(reloaded);
	super_block_release_dirents(reloaded);
	super_block_put(reloaded);
	ck_assert_int_eq(reffs_fs_rmdir("/clirules"), 0);
}
END_TEST

/*
 * Intent: loading an sb with no .clients file leaves nclient_rules == 0
 * (absent file = no rules configured).
 */
START_TEST(test_client_rules_absent_no_access)
{
	ck_assert_int_eq(reffs_fs_mkdir("/norules", 0755), 0);

	struct super_block *child = super_block_alloc(21, (char *)"/norules",
						      REFFS_STORAGE_RAM, NULL);

	ck_assert_ptr_nonnull(child);
	uuid_generate(child->sb_uuid);
	ck_assert_int_eq(super_block_dirent_create(child, NULL,
						   reffs_life_action_birth),
			 0);
	ck_assert_int_eq(super_block_mount(child, "/norules"), 0);

	/* Save with no rules set (no .clients file written). */
	ck_assert_int_eq(sb_registry_save(state_dir), 0);

	super_block_unmount(child);
	super_block_destroy(child);
	super_block_release_dirents(child);
	super_block_put(child);

	/* Verify no .clients file was written. */
	char clients_path[256];

	snprintf(clients_path, sizeof(clients_path), "%s/sb_21.clients",
		 state_dir);
	ck_assert_int_ne(access(clients_path, F_OK), 0);

	/* Load and verify nclient_rules == 0 (absent file = no rules). */
	ck_assert_int_eq(sb_registry_load(state_dir), 0);

	struct super_block *reloaded = super_block_find(21);

	ck_assert_ptr_nonnull(reloaded);
	ck_assert_uint_eq(reloaded->sb_nclient_rules, 0);

	super_block_unmount(reloaded);
	super_block_destroy(reloaded);
	super_block_release_dirents(reloaded);
	super_block_put(reloaded);
	ck_assert_int_eq(reffs_fs_rmdir("/norules"), 0);
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

	tc = tcase_create("uuid");
	tcase_add_checked_fixture(tc, persist_setup, persist_teardown);
	tcase_add_test(tc, test_registry_uuid_persisted);
	tcase_add_test(tc, test_registry_uuid_stable_across_restarts);
	suite_add_tcase(s, tc);

	tc = tcase_create("alloc_id");
	tcase_add_checked_fixture(tc, persist_setup, persist_teardown);
	tcase_add_test(tc, test_alloc_id_monotonic);
	tcase_add_test(tc, test_alloc_id_persists_across_restart);
	tcase_add_test(tc, test_alloc_id_never_reuses);
	suite_add_tcase(s, tc);

	tc = tcase_create("client_rules");
	tcase_add_checked_fixture(tc, persist_setup, persist_teardown);
	tcase_add_test(tc, test_client_rules_persisted);
	tcase_add_test(tc, test_client_rules_absent_no_access);
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
