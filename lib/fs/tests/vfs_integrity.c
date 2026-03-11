/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <rpc/rpc.h>
#include "fs_test_harness.h"
#include "reffs/vfs.h"
#include "reffs/dirent.h"

uid_t fs_test_uid;
gid_t fs_test_gid;

static void setup(void)
{
	fs_test_setup();
}
static void teardown(void)
{
	fs_test_teardown();
}

/*
 * Test vfs_is_subdir logic directly.
 * This is critical for preventing recursive rename loops.
 */
START_TEST(test_vfs_is_subdir_logic)
{
	struct name_match *nm_a, *nm_b, *nm_sub;
	struct inode *inode_a, *inode_b, *inode_sub;

	ck_assert_int_eq(reffs_fs_mkdir("/a", 0755), 0);
	ck_assert_int_eq(reffs_fs_mkdir("/b", 0755), 0);
	ck_assert_int_eq(reffs_fs_mkdir("/a/sub", 0755), 0);

	ck_assert_int_eq(find_matching_directory_entry(&nm_a, "/a",
						       LAST_COMPONENT_IS_MATCH),
			 0);
	ck_assert_int_eq(find_matching_directory_entry(&nm_b, "/b",
						       LAST_COMPONENT_IS_MATCH),
			 0);
	ck_assert_int_eq(find_matching_directory_entry(&nm_sub, "/a/sub",
						       LAST_COMPONENT_IS_MATCH),
			 0);

	inode_a = nm_a->nm_dirent->rd_inode;
	inode_b = nm_b->nm_dirent->rd_inode;
	inode_sub = nm_sub->nm_dirent->rd_inode;

	/* /a/sub IS a subdir of /a */
	ck_assert_int_eq(vfs_is_subdir(inode_sub, inode_a), 1);

	/* /a IS NOT a subdir of /a/sub */
	ck_assert_int_eq(vfs_is_subdir(inode_a, inode_sub), 0);

	/* /a IS NOT a subdir of /b */
	ck_assert_int_eq(vfs_is_subdir(inode_a, inode_b), 0);

	/* Root is not a subdir of anything (it should terminate correctly) */
	struct super_block *sb = inode_a->i_sb;
	struct inode *root = sb->sb_dirent->rd_inode;
	ck_assert_int_eq(vfs_is_subdir(root, inode_a), 0);

	name_match_free(nm_a);
	name_match_free(nm_b);
	name_match_free(nm_sub);
}
END_TEST

/*
 * Test vfs_setattr with a focus on sattr application logic.
 */
START_TEST(test_vfs_setattr_logic)
{
	struct name_match *nm;
	struct inode *inode;
	struct reffs_sattr sattr;
	struct authunix_parms ap;

	ck_assert_int_eq(reffs_fs_create("/test", 0644), 0);
	ck_assert_int_eq(find_matching_directory_entry(&nm, "/test",
						       LAST_COMPONENT_IS_MATCH),
			 0);
	inode = nm->nm_dirent->rd_inode;

	ap.aup_uid = inode->i_uid; /* Owner */
	ap.aup_gid = inode->i_gid;
	ap.aup_len = 0;

	/* Change mode */
	memset(&sattr, 0, sizeof(sattr));
	sattr.mode = 0700;
	sattr.mode_set = true;
	ck_assert_int_eq(vfs_setattr(inode, &sattr, &ap), 0);
	ck_assert_uint_eq(inode->i_mode & 0777, 0700);

	/* Change size (truncate) */
	memset(&sattr, 0, sizeof(sattr));
	sattr.size = 100;
	sattr.size_set = true;
	ck_assert_int_eq(vfs_setattr(inode, &sattr, &ap), 0);
	ck_assert_uint_eq(inode->i_size, 100);

	name_match_free(nm);
	ck_assert_int_eq(reffs_fs_unlink("/test"), 0);
}
END_TEST

Suite *vfs_integrity_suite(void)
{
	Suite *s = suite_create("fs: vfs integrity");
	TCase *tc = tcase_create("Core");
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_vfs_is_subdir_logic);
	tcase_add_test(tc, test_vfs_setattr_logic);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	fs_test_global_init();
	SRunner *sr = srunner_create(vfs_integrity_suite());
	srunner_set_fork_status(sr, CK_NOFORK);
	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	fs_test_global_fini();
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
