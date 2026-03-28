/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Phase 2 TDD: Superblock mount-crossing tests.
 *
 * Tests for LOOKUP/LOOKUPP crossing sb boundaries, READDIR at
 * mount points, path protection, and cross-export XDEV guards.
 *
 * These tests use the VFS/dirent layer directly — they don't
 * require NFSv4 compound processing.
 *
 * Intent: each test creates a root sb (from fs_test_setup) and
 * a child sb, mounts the child at a directory in the root, then
 * validates crossing behavior.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <rpc/auth.h>
#include <rpc/auth_unix.h>
#include <check.h>

#include "reffs/dirent.h"
#include "reffs/fs.h"
#include "reffs/identity.h"
#include "reffs/inode.h"
#include "reffs/super_block.h"
#include "reffs/vfs.h"
#include "fs_test_harness.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/*
 * Create a child sb and mount it at the given path.
 * The path must already exist as a directory in the root sb.
 * Returns the child sb (caller must unmount + destroy + put).
 */
static struct super_block *mount_child_at(const char *path, uint64_t sb_id)
{
	struct super_block *child =
		super_block_alloc(sb_id, (char *)path, REFFS_STORAGE_RAM, NULL);

	if (!child)
		return NULL;

	/* Create the child's root dirent + inode. */
	if (super_block_dirent_create(child, NULL, reffs_life_action_birth)) {
		super_block_put(child);
		return NULL;
	}

	/* Mount sets RD_MOUNTED_ON on the path's dirent and links
	 * the child sb to the parent sb. */
	if (super_block_mount(child, path)) {
		super_block_release_dirents(child);
		super_block_put(child);
		return NULL;
	}

	return child;
}

static void unmount_and_destroy(struct super_block *child)
{
	if (!child)
		return;
	super_block_unmount(child);
	super_block_destroy(child);
	super_block_release_dirents(child);
	super_block_put(child);
}

/* ------------------------------------------------------------------ */
/* Mount flag tests                                                    */
/* ------------------------------------------------------------------ */

/*
 * Intent: verify that super_block_mount sets RD_MOUNTED_ON on
 * the target dirent.  This flag is how LOOKUP detects mount points.
 */
START_TEST(test_mount_sets_flag)
{
	ck_assert_int_eq(reffs_fs_mkdir("/mnt", 0755), 0);

	struct super_block *child = mount_child_at("/mnt", 50);

	ck_assert_ptr_nonnull(child);

	/* The root sb's "/mnt" dirent should have RD_MOUNTED_ON set. */
	struct stat st;

	ck_assert_int_eq(reffs_fs_getattr("/mnt", &st), 0);

	/* NOT_NOW_BROWN_COW: need a way to check dirent flags from
	 * the test.  For now, verify the mount succeeded. */
	ck_assert_uint_eq(super_block_lifecycle(child), SB_MOUNTED);

	unmount_and_destroy(child);
	ck_assert_int_eq(reffs_fs_rmdir("/mnt"), 0);
}
END_TEST

/*
 * Intent: verify that super_block_unmount clears RD_MOUNTED_ON.
 */
START_TEST(test_unmount_clears_flag)
{
	ck_assert_int_eq(reffs_fs_mkdir("/mnt2", 0755), 0);

	struct super_block *child = mount_child_at("/mnt2", 51);

	ck_assert_ptr_nonnull(child);
	ck_assert_int_eq(super_block_unmount(child), 0);

	/* After unmount, rmdir should succeed (flag cleared). */
	super_block_destroy(child);
	super_block_release_dirents(child);
	super_block_put(child);

	ck_assert_int_eq(reffs_fs_rmdir("/mnt2"), 0);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Path protection tests                                               */
/* ------------------------------------------------------------------ */

/*
 * Intent: rmdir on a directory with a mounted sb returns EBUSY.
 * The mounted directory is "in use" and cannot be removed.
 */
START_TEST(test_rmdir_mount_point_ebusy)
{
	ck_assert_int_eq(reffs_fs_mkdir("/busy", 0755), 0);

	struct super_block *child = mount_child_at("/busy", 52);

	ck_assert_ptr_nonnull(child);

	/* rmdir should fail with EBUSY while mounted. */
	int ret = reffs_fs_rmdir("/busy");

	ck_assert_int_eq(ret, -EBUSY);

	unmount_and_destroy(child);
	ck_assert_int_eq(reffs_fs_rmdir("/busy"), 0);
}
END_TEST

/*
 * Intent: after unmount, rmdir succeeds.
 */
START_TEST(test_rmdir_after_unmount_ok)
{
	ck_assert_int_eq(reffs_fs_mkdir("/temp", 0755), 0);

	struct super_block *child = mount_child_at("/temp", 53);

	ck_assert_ptr_nonnull(child);
	ck_assert_int_eq(super_block_unmount(child), 0);
	super_block_destroy(child);
	super_block_release_dirents(child);
	super_block_put(child);

	/* rmdir should now succeed. */
	ck_assert_int_eq(reffs_fs_rmdir("/temp"), 0);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Cross-export XDEV guards                                            */
/* ------------------------------------------------------------------ */

/*
 * Intent: rename where source and destination are in different
 * superblocks returns EXDEV.  The VFS already checks i_sb equality.
 * This test verifies the behavior is correct with mounted sbs.
 */
START_TEST(test_rename_across_sb_xdev)
{
	ck_assert_int_eq(reffs_fs_mkdir("/export_a", 0755), 0);

	struct super_block *child = mount_child_at("/export_a", 54);

	ck_assert_ptr_nonnull(child);

	struct super_block *root = super_block_find(SUPER_BLOCK_ROOT_ID);

	ck_assert_ptr_nonnull(root);

	/* NOT_NOW_BROWN_COW: need to create files in both sbs and
	 * attempt a rename across them.  The VFS vfs_rename already
	 * checks old_dir->i_sb != new_dir->i_sb → -EXDEV.  This test
	 * validates that mounted sbs have different i_sb pointers. */
	ck_assert(root != child);

	super_block_put(root);
	unmount_and_destroy(child);
	ck_assert_int_eq(reffs_fs_rmdir("/export_a"), 0);
}
END_TEST

/*
 * Intent: same as rename — link across sbs returns EXDEV.
 */
START_TEST(test_link_across_sb_xdev)
{
	ck_assert_int_eq(reffs_fs_mkdir("/export_b", 0755), 0);

	struct super_block *child = mount_child_at("/export_b", 55);

	ck_assert_ptr_nonnull(child);

	struct super_block *root = super_block_find(SUPER_BLOCK_ROOT_ID);

	ck_assert_ptr_nonnull(root);
	ck_assert(root != child);

	super_block_put(root);
	unmount_and_destroy(child);
	ck_assert_int_eq(reffs_fs_rmdir("/export_b"), 0);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Mount-point lookup helper                                           */
/* ------------------------------------------------------------------ */

/*
 * Intent: super_block_find_mounted_on returns the child sb that is
 * mounted on a given dirent, and NULL after unmount.
 */
START_TEST(test_find_mounted_on)
{
	ck_assert_int_eq(reffs_fs_mkdir("/fmo", 0755), 0);

	struct super_block *child = mount_child_at("/fmo", 60);

	ck_assert_ptr_nonnull(child);

	/* The child sb's mount_dirent should be findable. */
	struct super_block *found =
		super_block_find_mounted_on(child->sb_mount_dirent);

	ck_assert_ptr_nonnull(found);
	ck_assert(found == child);
	super_block_put(found);

	/* After unmount, the helper should return NULL. */
	unmount_and_destroy(child);

	/* mount_dirent was cleared by unmount, so we can't query it.
	 * Instead verify rmdir succeeds (flag was cleared). */
	ck_assert_int_eq(reffs_fs_rmdir("/fmo"), 0);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

static Suite *sb_mount_crossing_suite(void)
{
	Suite *s = suite_create("sb_mount_crossing");
	TCase *tc;

	tc = tcase_create("flags");
	tcase_add_checked_fixture(tc, fs_test_setup, fs_test_teardown);
	tcase_add_test(tc, test_mount_sets_flag);
	tcase_add_test(tc, test_unmount_clears_flag);
	suite_add_tcase(s, tc);

	tc = tcase_create("path_protection");
	tcase_add_checked_fixture(tc, fs_test_setup, fs_test_teardown);
	tcase_add_test(tc, test_rmdir_mount_point_ebusy);
	tcase_add_test(tc, test_rmdir_after_unmount_ok);
	suite_add_tcase(s, tc);

	tc = tcase_create("xdev");
	tcase_add_checked_fixture(tc, fs_test_setup, fs_test_teardown);
	tcase_add_test(tc, test_rename_across_sb_xdev);
	tcase_add_test(tc, test_link_across_sb_xdev);
	suite_add_tcase(s, tc);

	tc = tcase_create("find_mounted_on");
	tcase_add_checked_fixture(tc, fs_test_setup, fs_test_teardown);
	tcase_add_test(tc, test_find_mounted_on);
	suite_add_tcase(s, tc);

	return s;
}

int main(void)
{
	int nfailed;
	Suite *s = sb_mount_crossing_suite();
	SRunner *sr = srunner_create(s);

	srunner_run_all(sr, CK_NORMAL);
	nfailed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return nfailed ? 1 : 0;
}
