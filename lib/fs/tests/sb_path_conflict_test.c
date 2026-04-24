/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Unit tests for super_block_check_path_conflict().
 *
 * Validates that:
 * - Exact match with an existing mount --> -EEXIST
 * - New path is parent of existing mount --> -EBUSY
 * - New path is child of existing mount --> allowed (0)
 * - Unrelated paths --> no conflict (0)
 * - Unmounted sb --> no conflict
 * - /foo vs /foobar --> not a false prefix match
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <string.h>
#include <uuid/uuid.h>

#include <check.h>

#include "reffs/dirent.h"
#include "reffs/fs.h"
#include "reffs/inode.h"
#include "reffs/super_block.h"
#include "fs_test_harness.h"

static struct super_block *mount_child_at(const char *path, uint64_t sb_id)
{
	struct super_block *child =
		super_block_alloc(sb_id, (char *)path, REFFS_STORAGE_RAM, NULL);

	if (!child)
		return NULL;
	uuid_generate(child->sb_uuid);
	if (super_block_dirent_create(child, NULL, reffs_life_action_birth)) {
		super_block_put(child);
		return NULL;
	}
	if (super_block_mount(child, path)) {
		super_block_release_dirents(child);
		super_block_put(child);
		return NULL;
	}
	return child;
}

static void unmount_and_destroy(struct super_block *child)
{
	super_block_unmount(child);
	super_block_destroy(child);
	super_block_release_dirents(child);
	super_block_put(child);
}

/*
 * Intent: mounting at a path that already has a mount --> -EEXIST.
 */
START_TEST(test_path_conflict_exact_match)
{
	ck_assert_int_eq(reffs_fs_mkdir("/alpo", 0755), 0);

	struct super_block *child = mount_child_at("/alpo", 50);

	ck_assert_ptr_nonnull(child);

	int ret = super_block_check_path_conflict(0, "/alpo");

	ck_assert_int_eq(ret, -EEXIST);

	unmount_and_destroy(child);
	ck_assert_int_eq(reffs_fs_rmdir("/alpo"), 0);
}
END_TEST

/*
 * Intent: creating a parent of an existing mount --> -EBUSY.
 * If /foo/bar/garbo is mounted, creating /foo/bar would change
 * the namespace traversal for the existing child.
 */
START_TEST(test_path_conflict_parent_of_mounted)
{
	ck_assert_int_eq(reffs_fs_mkdir_p("/foo/bar/garbo", 0755), 0);

	struct super_block *child = mount_child_at("/foo/bar/garbo", 51);

	ck_assert_ptr_nonnull(child);

	int ret = super_block_check_path_conflict(0, "/foo/bar");

	ck_assert_int_eq(ret, -EBUSY);

	unmount_and_destroy(child);
	ck_assert_int_eq(reffs_fs_rmdir("/foo/bar/garbo"), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/foo/bar"), 0);
	ck_assert_int_eq(reffs_fs_rmdir("/foo"), 0);
}
END_TEST

/*
 * Intent: creating a child under an existing mount is allowed.
 * This is the normal case for nested mounts.
 */
START_TEST(test_path_conflict_child_of_mounted)
{
	ck_assert_int_eq(reffs_fs_mkdir("/foo2", 0755), 0);

	struct super_block *child = mount_child_at("/foo2", 52);

	ck_assert_ptr_nonnull(child);

	int ret = super_block_check_path_conflict(0, "/foo2/bar/deeper");

	ck_assert_int_eq(ret, 0);

	unmount_and_destroy(child);
	ck_assert_int_eq(reffs_fs_rmdir("/foo2"), 0);
}
END_TEST

/*
 * Intent: unrelated paths --> no conflict.
 */
START_TEST(test_path_conflict_no_conflict)
{
	ck_assert_int_eq(reffs_fs_mkdir("/alpo2", 0755), 0);

	struct super_block *child = mount_child_at("/alpo2", 53);

	ck_assert_ptr_nonnull(child);

	int ret = super_block_check_path_conflict(0, "/bravo");

	ck_assert_int_eq(ret, 0);

	unmount_and_destroy(child);
	ck_assert_int_eq(reffs_fs_rmdir("/alpo2"), 0);
}
END_TEST

/*
 * Intent: unmounted sb --> no conflict at the same path.
 */
START_TEST(test_path_conflict_unmounted_no_conflict)
{
	ck_assert_int_eq(reffs_fs_mkdir("/alpo3", 0755), 0);

	struct super_block *child = mount_child_at("/alpo3", 54);

	ck_assert_ptr_nonnull(child);
	super_block_unmount(child);

	int ret = super_block_check_path_conflict(0, "/alpo3");

	ck_assert_int_eq(ret, 0);

	super_block_destroy(child);
	super_block_release_dirents(child);
	super_block_put(child);
	ck_assert_int_eq(reffs_fs_rmdir("/alpo3"), 0);
}
END_TEST

/*
 * Intent: /foo is NOT a prefix of /foobar (boundary check).
 */
START_TEST(test_path_conflict_not_false_prefix)
{
	ck_assert_int_eq(reffs_fs_mkdir("/foo3", 0755), 0);

	struct super_block *child = mount_child_at("/foo3", 55);

	ck_assert_ptr_nonnull(child);

	int ret = super_block_check_path_conflict(0, "/foo3bar");

	ck_assert_int_eq(ret, 0);

	unmount_and_destroy(child);
	ck_assert_int_eq(reffs_fs_rmdir("/foo3"), 0);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

static Suite *sb_path_conflict_suite(void)
{
	Suite *s = suite_create("sb_path_conflict");
	TCase *tc;

	tc = tcase_create("core");
	tcase_add_checked_fixture(tc, fs_test_setup, fs_test_teardown);
	tcase_add_test(tc, test_path_conflict_exact_match);
	tcase_add_test(tc, test_path_conflict_parent_of_mounted);
	tcase_add_test(tc, test_path_conflict_child_of_mounted);
	tcase_add_test(tc, test_path_conflict_no_conflict);
	tcase_add_test(tc, test_path_conflict_unmounted_no_conflict);
	tcase_add_test(tc, test_path_conflict_not_false_prefix);
	suite_add_tcase(s, tc);

	return s;
}

int main(void)
{
	int nfailed;
	Suite *s = sb_path_conflict_suite();
	SRunner *sr = srunner_create(s);

	srunner_run_all(sr, CK_NORMAL);
	nfailed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return nfailed ? 1 : 0;
}
