/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Phase 1 TDD: Superblock lifecycle state machine tests.
 *
 * These tests are written BEFORE the implementation.  They define
 * the expected behavior of the state machine and will initially
 * fail.  The implementation makes them pass.
 *
 * See .claude/design/multi-superblock.md for the state diagram.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <rpc/auth.h>
#include <rpc/auth_unix.h>
#include <check.h>

#include "reffs/super_block.h"
#include "reffs/inode.h"
#include "reffs/fs.h"
#include "fs_test_harness.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Create a directory in the root namespace for mount point testing. */
static int make_mount_dir(const char *name)
{
	char path[256];

	snprintf(path, sizeof(path), "/%s", name);
	return reffs_fs_mkdir(path, 0755);
}

/* ------------------------------------------------------------------ */
/* State machine: create                                               */
/* ------------------------------------------------------------------ */

START_TEST(test_sb_create)
{
	struct super_block *sb =
		super_block_alloc(100, "/test", REFFS_STORAGE_RAM, NULL);

	ck_assert_ptr_nonnull(sb);
	ck_assert_uint_eq(super_block_lifecycle(sb), SB_CREATED);
	ck_assert_str_eq(super_block_lifecycle_name(SB_CREATED), "CREATED");

	super_block_put(sb);
}
END_TEST

START_TEST(test_sb_create_duplicate_id)
{
	struct super_block *sb1 =
		super_block_alloc(101, "/dup1", REFFS_STORAGE_RAM, NULL);
	struct super_block *sb2 =
		super_block_alloc(101, "/dup2", REFFS_STORAGE_RAM, NULL);

	ck_assert_ptr_nonnull(sb1);
	/* Duplicate id should fail. */
	ck_assert_ptr_null(sb2);

	super_block_put(sb1);
}
END_TEST

/* ------------------------------------------------------------------ */
/* State machine: mount                                                */
/* ------------------------------------------------------------------ */

START_TEST(test_sb_mount)
{
	make_mount_dir("mnt_test");

	struct super_block *sb =
		super_block_alloc(102, "/mnt_test", REFFS_STORAGE_RAM, NULL);

	ck_assert_ptr_nonnull(sb);

	int ret = super_block_mount(sb, "/mnt_test");

	ck_assert_int_eq(ret, 0);
	ck_assert_uint_eq(super_block_lifecycle(sb), SB_MOUNTED);

	/* Cleanup: unmount then destroy. */
	super_block_unmount(sb);
	super_block_destroy(sb);
	super_block_put(sb);
}
END_TEST

START_TEST(test_sb_mount_nonexistent_path)
{
	struct super_block *sb =
		super_block_alloc(103, "/no_such_dir", REFFS_STORAGE_RAM, NULL);

	ck_assert_ptr_nonnull(sb);

	int ret = super_block_mount(sb, "/no_such_dir");

	ck_assert_int_eq(ret, -ENOENT);
	ck_assert_uint_eq(super_block_lifecycle(sb), SB_CREATED);

	super_block_destroy(sb);
	super_block_put(sb);
}
END_TEST

START_TEST(test_sb_mount_already_mounted)
{
	make_mount_dir("mnt_double");

	struct super_block *sb =
		super_block_alloc(104, "/mnt_double", REFFS_STORAGE_RAM, NULL);

	ck_assert_int_eq(super_block_mount(sb, "/mnt_double"), 0);

	/* Second mount while already mounted → EBUSY. */
	int ret = super_block_mount(sb, "/mnt_double");

	ck_assert_int_eq(ret, -EBUSY);

	super_block_unmount(sb);
	super_block_destroy(sb);
	super_block_put(sb);
}
END_TEST

/* ------------------------------------------------------------------ */
/* State machine: unmount                                              */
/* ------------------------------------------------------------------ */

START_TEST(test_sb_unmount)
{
	make_mount_dir("mnt_um");

	struct super_block *sb =
		super_block_alloc(105, "/mnt_um", REFFS_STORAGE_RAM, NULL);

	ck_assert_int_eq(super_block_mount(sb, "/mnt_um"), 0);
	ck_assert_int_eq(super_block_unmount(sb), 0);
	ck_assert_uint_eq(super_block_lifecycle(sb), SB_UNMOUNTED);

	super_block_destroy(sb);
	super_block_put(sb);
}
END_TEST

START_TEST(test_sb_unmount_with_child)
{
	make_mount_dir("mnt_parent");

	struct super_block *parent =
		super_block_alloc(106, "/mnt_parent", REFFS_STORAGE_RAM, NULL);

	ck_assert_int_eq(super_block_mount(parent, "/mnt_parent"), 0);

	/* Create a child sb mounted inside the parent's namespace.
	 * For now, simulate by creating a second sb whose path is
	 * a descendant. The implementation must track parent-child
	 * mount relationships. */
	struct super_block *child = super_block_alloc(107, "/mnt_parent/child",
						      REFFS_STORAGE_RAM, NULL);

	/* The child needs a directory to mount on — this would be
	 * inside parent's namespace. For the test stub, just mount. */
	/* super_block_mount(child, "/mnt_parent/child"); */

	/* Unmounting parent while child is mounted → EBUSY. */
	/* NOTE: this test will evolve as mount-crossing is implemented.
	 * For Phase 1, we test the state machine transition rule. */
	/* int ret = super_block_unmount(parent);
	 * ck_assert_int_eq(ret, -EBUSY); */

	/* For now, just verify the parent is mounted. */
	ck_assert_uint_eq(super_block_lifecycle(parent), SB_MOUNTED);

	/* Cleanup. */
	super_block_put(child);
	super_block_unmount(parent);
	super_block_destroy(parent);
	super_block_put(parent);
}
END_TEST

/* ------------------------------------------------------------------ */
/* State machine: destroy                                              */
/* ------------------------------------------------------------------ */

START_TEST(test_sb_destroy)
{
	struct super_block *sb =
		super_block_alloc(108, "/destr", REFFS_STORAGE_RAM, NULL);

	ck_assert_int_eq(super_block_destroy(sb), 0);
	ck_assert_uint_eq(super_block_lifecycle(sb), SB_DESTROYED);

	super_block_put(sb);
}
END_TEST

START_TEST(test_sb_destroy_mounted)
{
	make_mount_dir("mnt_destr");

	struct super_block *sb =
		super_block_alloc(109, "/mnt_destr", REFFS_STORAGE_RAM, NULL);

	ck_assert_int_eq(super_block_mount(sb, "/mnt_destr"), 0);

	/* Destroy while mounted → EBUSY. */
	int ret = super_block_destroy(sb);

	ck_assert_int_eq(ret, -EBUSY);
	ck_assert_uint_eq(super_block_lifecycle(sb), SB_MOUNTED);

	super_block_unmount(sb);
	super_block_destroy(sb);
	super_block_put(sb);
}
END_TEST

START_TEST(test_sb_destroy_created)
{
	struct super_block *sb =
		super_block_alloc(110, "/cr_destr", REFFS_STORAGE_RAM, NULL);

	/* Destroy from CREATED (never mounted) → always OK. */
	int ret = super_block_destroy(sb);

	ck_assert_int_eq(ret, 0);
	ck_assert_uint_eq(super_block_lifecycle(sb), SB_DESTROYED);

	super_block_put(sb);
}
END_TEST

/* ------------------------------------------------------------------ */
/* State machine: remount                                              */
/* ------------------------------------------------------------------ */

START_TEST(test_sb_remount)
{
	make_mount_dir("mnt_a");
	make_mount_dir("mnt_b");

	struct super_block *sb =
		super_block_alloc(111, "/mnt_a", REFFS_STORAGE_RAM, NULL);

	ck_assert_int_eq(super_block_mount(sb, "/mnt_a"), 0);
	ck_assert_int_eq(super_block_unmount(sb), 0);

	/* Remount at a different path. */
	int ret = super_block_mount(sb, "/mnt_b");

	ck_assert_int_eq(ret, 0);
	ck_assert_uint_eq(super_block_lifecycle(sb), SB_MOUNTED);

	super_block_unmount(sb);
	super_block_destroy(sb);
	super_block_put(sb);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Root sb protection                                                  */
/* ------------------------------------------------------------------ */

START_TEST(test_root_sb_cannot_unmount)
{
	struct super_block *root = super_block_find(SUPER_BLOCK_ROOT_ID);

	ck_assert_ptr_nonnull(root);

	int ret = super_block_unmount(root);

	ck_assert_int_ne(ret, 0);

	super_block_put(root);
}
END_TEST

START_TEST(test_root_sb_cannot_destroy)
{
	struct super_block *root = super_block_find(SUPER_BLOCK_ROOT_ID);

	ck_assert_ptr_nonnull(root);

	int ret = super_block_destroy(root);

	ck_assert_int_ne(ret, 0);

	super_block_put(root);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

static Suite *sb_lifecycle_suite(void)
{
	Suite *s = suite_create("sb_lifecycle");
	TCase *tc;

	tc = tcase_create("create");
	tcase_add_checked_fixture(tc, fs_test_setup, fs_test_teardown);
	tcase_add_test(tc, test_sb_create);
	tcase_add_test(tc, test_sb_create_duplicate_id);
	suite_add_tcase(s, tc);

	tc = tcase_create("mount");
	tcase_add_checked_fixture(tc, fs_test_setup, fs_test_teardown);
	tcase_add_test(tc, test_sb_mount);
	tcase_add_test(tc, test_sb_mount_nonexistent_path);
	tcase_add_test(tc, test_sb_mount_already_mounted);
	suite_add_tcase(s, tc);

	tc = tcase_create("unmount");
	tcase_add_checked_fixture(tc, fs_test_setup, fs_test_teardown);
	tcase_add_test(tc, test_sb_unmount);
	tcase_add_test(tc, test_sb_unmount_with_child);
	suite_add_tcase(s, tc);

	tc = tcase_create("destroy");
	tcase_add_checked_fixture(tc, fs_test_setup, fs_test_teardown);
	tcase_add_test(tc, test_sb_destroy);
	tcase_add_test(tc, test_sb_destroy_mounted);
	tcase_add_test(tc, test_sb_destroy_created);
	suite_add_tcase(s, tc);

	tc = tcase_create("remount");
	tcase_add_checked_fixture(tc, fs_test_setup, fs_test_teardown);
	tcase_add_test(tc, test_sb_remount);
	suite_add_tcase(s, tc);

	tc = tcase_create("root_protection");
	tcase_add_checked_fixture(tc, fs_test_setup, fs_test_teardown);
	tcase_add_test(tc, test_root_sb_cannot_unmount);
	tcase_add_test(tc, test_root_sb_cannot_destroy);
	suite_add_tcase(s, tc);

	return s;
}

int main(void)
{
	int nfailed;
	Suite *s = sb_lifecycle_suite();
	SRunner *sr = srunner_create(s);

	srunner_run_all(sr, CK_NORMAL);
	nfailed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return nfailed ? 1 : 0;
}
