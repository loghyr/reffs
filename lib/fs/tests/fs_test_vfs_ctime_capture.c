/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * fs_test_vfs_ctime_capture.c -- before/after ctime capture in VFS ops
 *
 * Verifies that vfs_mkdir, vfs_symlink, vfs_mknod, vfs_remove, vfs_rmdir,
 * and vfs_rename correctly capture the parent directory's ctime before and
 * after the mutation, atomically under the directory lock.
 *
 * Core invariant: before < after, and after == dir->i_ctime immediately
 * following the call.
 *
 * Tests:
 *  - vfs_mkdir:  before < after, after matches dir ctime
 *  - vfs_symlink: before < after, after matches dir ctime
 *  - vfs_mknod (FIFO): before < after, after matches dir ctime
 *  - vfs_remove: before < after, after matches dir ctime
 *  - vfs_rmdir:  before < after, after matches dir ctime
 *  - vfs_rename (same dir): before < after; old and new refs track same dir
 *  - vfs_rename (cross dir): both directory pairs advance independently
 *  - NULL before/after pointers are accepted without crashing
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <rpc/rpc.h>

#include "fs_test_harness.h"
#include "reffs/vfs.h"

static void setup(void)
{
	fs_test_setup();
}
static void teardown(void)
{
	fs_test_teardown();
}

static void get_ap(struct authunix_parms *ap)
{
	struct reffs_context *ctx = reffs_get_context();
	ap->aup_uid = ctx->uid;
	ap->aup_gid = ctx->gid;
	ap->aup_len = 0;
	ap->aup_gids = NULL;
}

/* ------------------------------------------------------------------ */

START_TEST(test_vfs_mkdir_before_lt_after)
{
	struct super_block *sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	struct inode *root = inode_find(sb, 1);
	struct authunix_parms ap;
	struct timespec before = { 0 }, after = { 0 };
	struct inode *child = NULL;

	get_ap(&ap);
	usleep(1000);

	ck_assert_int_eq(vfs_mkdir(root, "cap_mkdir", 0755, &ap, &child,
				   &before, &after),
			 0);

	ck_assert_ptr_nonnull(child);
	ck_assert_timespec_lt(before, after);
	ck_assert_timespec_eq(after, root->i_ctime);

	vfs_rmdir(root, "cap_mkdir", &ap, NULL, NULL);
	inode_active_put(child);
	inode_active_put(root);
	super_block_put(sb);
}
END_TEST

START_TEST(test_vfs_symlink_before_lt_after)
{
	struct super_block *sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	struct inode *root = inode_find(sb, 1);
	struct authunix_parms ap;
	struct timespec before = { 0 }, after = { 0 };
	struct inode *child = NULL;

	get_ap(&ap);
	usleep(1000);

	ck_assert_int_eq(vfs_symlink(root, "cap_symlink", "/target", &ap,
				     &child, &before, &after),
			 0);

	ck_assert_ptr_nonnull(child);
	ck_assert_timespec_lt(before, after);
	ck_assert_timespec_eq(after, root->i_ctime);

	vfs_remove(root, "cap_symlink", &ap, NULL, NULL);
	inode_active_put(child);
	inode_active_put(root);
	super_block_put(sb);
}
END_TEST

START_TEST(test_vfs_mknod_before_lt_after)
{
	struct super_block *sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	struct inode *root = inode_find(sb, 1);
	struct authunix_parms ap;
	struct timespec before = { 0 }, after = { 0 };
	struct inode *child = NULL;

	get_ap(&ap);
	usleep(1000);

	ck_assert_int_eq(vfs_mknod(root, "cap_fifo", S_IFIFO | 0666, 0, &ap,
				   &child, &before, &after),
			 0);

	ck_assert_ptr_nonnull(child);
	ck_assert_timespec_lt(before, after);
	ck_assert_timespec_eq(after, root->i_ctime);

	vfs_remove(root, "cap_fifo", &ap, NULL, NULL);
	inode_active_put(child);
	inode_active_put(root);
	super_block_put(sb);
}
END_TEST

START_TEST(test_vfs_remove_before_lt_after)
{
	struct super_block *sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	struct inode *root = inode_find(sb, 1);
	struct authunix_parms ap;
	struct timespec before = { 0 }, after = { 0 };

	get_ap(&ap);

	ck_assert_int_eq(vfs_mknod(root, "cap_rm_file", S_IFREG | 0644, 0, &ap,
				   NULL, NULL, NULL),
			 0);

	usleep(1000);

	ck_assert_int_eq(vfs_remove(root, "cap_rm_file", &ap, &before, &after),
			 0);

	ck_assert_timespec_lt(before, after);
	ck_assert_timespec_eq(after, root->i_ctime);

	inode_active_put(root);
	super_block_put(sb);
}
END_TEST

START_TEST(test_vfs_rmdir_before_lt_after)
{
	struct super_block *sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	struct inode *root = inode_find(sb, 1);
	struct authunix_parms ap;
	struct timespec before = { 0 }, after = { 0 };

	get_ap(&ap);

	ck_assert_int_eq(
		vfs_mkdir(root, "cap_rmdir", 0755, &ap, NULL, NULL, NULL), 0);

	usleep(1000);

	ck_assert_int_eq(vfs_rmdir(root, "cap_rmdir", &ap, &before, &after), 0);

	ck_assert_timespec_lt(before, after);
	ck_assert_timespec_eq(after, root->i_ctime);

	inode_active_put(root);
	super_block_put(sb);
}
END_TEST

START_TEST(test_vfs_rename_same_dir_before_lt_after)
{
	struct super_block *sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	struct inode *root = inode_find(sb, 1);
	struct authunix_parms ap;
	struct timespec ob = { 0 }, oa = { 0 }, nb = { 0 }, na = { 0 };

	get_ap(&ap);

	ck_assert_int_eq(vfs_mknod(root, "cap_ren_src", S_IFREG | 0644, 0, &ap,
				   NULL, NULL, NULL),
			 0);

	usleep(1000);

	ck_assert_int_eq(vfs_rename(root, "cap_ren_src", root, "cap_ren_dst",
				    &ap, &ob, &oa, &nb, &na),
			 0);

	/* Same directory: old and new before/after track the same inode. */
	ck_assert_timespec_lt(ob, oa);
	ck_assert_timespec_eq(ob, nb);
	ck_assert_timespec_eq(oa, na);
	ck_assert_timespec_eq(oa, root->i_ctime);

	vfs_remove(root, "cap_ren_dst", &ap, NULL, NULL);
	inode_active_put(root);
	super_block_put(sb);
}
END_TEST

START_TEST(test_vfs_rename_cross_dir_before_lt_after)
{
	struct super_block *sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	struct inode *root = inode_find(sb, 1);
	struct authunix_parms ap;
	struct timespec ob = { 0 }, oa = { 0 }, nb = { 0 }, na = { 0 };
	struct inode *dst_dir = NULL;

	get_ap(&ap);

	ck_assert_int_eq(vfs_mkdir(root, "cap_xdir_dst", 0755, &ap, &dst_dir,
				   NULL, NULL),
			 0);
	ck_assert_int_eq(vfs_mknod(root, "cap_xdir_src", S_IFREG | 0644, 0, &ap,
				   NULL, NULL, NULL),
			 0);

	usleep(1000);

	ck_assert_int_eq(vfs_rename(root, "cap_xdir_src", dst_dir,
				    "cap_xdir_moved", &ap, &ob, &oa, &nb, &na),
			 0);

	/* Both directories have before < after. */
	ck_assert_timespec_lt(ob, oa);
	ck_assert_timespec_lt(nb, na);
	ck_assert_timespec_eq(oa, root->i_ctime);
	ck_assert_timespec_eq(na, dst_dir->i_ctime);

	vfs_remove(dst_dir, "cap_xdir_moved", &ap, NULL, NULL);
	vfs_rmdir(root, "cap_xdir_dst", &ap, NULL, NULL);
	inode_active_put(dst_dir);
	inode_active_put(root);
	super_block_put(sb);
}
END_TEST

START_TEST(test_vfs_ctime_capture_null_params_ok)
{
	struct super_block *sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	struct inode *root = inode_find(sb, 1);
	struct authunix_parms ap;

	get_ap(&ap);

	/* All mutation functions must accept NULL before/after without crash. */
	ck_assert_int_eq(
		vfs_mkdir(root, "null_mkdir", 0755, &ap, NULL, NULL, NULL), 0);
	ck_assert_int_eq(vfs_rmdir(root, "null_mkdir", &ap, NULL, NULL), 0);

	ck_assert_int_eq(vfs_mknod(root, "null_mknod", S_IFREG | 0644, 0, &ap,
				   NULL, NULL, NULL),
			 0);
	ck_assert_int_eq(vfs_remove(root, "null_mknod", &ap, NULL, NULL), 0);

	ck_assert_int_eq(vfs_symlink(root, "null_symlink", "/x", &ap, NULL,
				     NULL, NULL),
			 0);
	ck_assert_int_eq(vfs_remove(root, "null_symlink", &ap, NULL, NULL), 0);

	ck_assert_int_eq(vfs_mknod(root, "null_ren_src", S_IFREG | 0644, 0, &ap,
				   NULL, NULL, NULL),
			 0);
	ck_assert_int_eq(vfs_rename(root, "null_ren_src", root, "null_ren_dst",
				    &ap, NULL, NULL, NULL, NULL),
			 0);
	ck_assert_int_eq(vfs_remove(root, "null_ren_dst", &ap, NULL, NULL), 0);

	inode_active_put(root);
	super_block_put(sb);
}
END_TEST

/* ------------------------------------------------------------------ */

Suite *fs_vfs_ctime_capture_suite(void)
{
	Suite *s = suite_create("vfs: before/after ctime capture");
	TCase *tc = tcase_create("Core");
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_vfs_mkdir_before_lt_after);
	tcase_add_test(tc, test_vfs_symlink_before_lt_after);
	tcase_add_test(tc, test_vfs_mknod_before_lt_after);
	tcase_add_test(tc, test_vfs_remove_before_lt_after);
	tcase_add_test(tc, test_vfs_rmdir_before_lt_after);
	tcase_add_test(tc, test_vfs_rename_same_dir_before_lt_after);
	tcase_add_test(tc, test_vfs_rename_cross_dir_before_lt_after);
	tcase_add_test(tc, test_vfs_ctime_capture_null_params_ok);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	return fs_test_run(fs_vfs_ctime_capture_suite());
}
