/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * Test 10: Deep nested directory tree recovery.
 *
 * recover_directory_recursive() recurses for every directory.  This test
 * builds a four-level tree (root → A → B → C → leaf file) and verifies:
 *   1. All directories and the leaf are present after recovery.
 *   2. i_parent pointers are set correctly at each level.
 *   3. sb_next_ino is the maximum inode + 1 across the entire tree.
 *
 * Nesting also exercises the "nlink handling during recovery" concern: each
 * directory inode's nlink must be exactly what was written to its .meta file,
 * not inflated by dirent_parent_attach increments that happen before
 * load_inode_attributes overwrites i_nlink.
 *
 * Related issues (reffs_issues.md):
 *   - "nlink handling during recovery is accidentally correct"
 *   - "inode_sync_to_disk called before load_inode_attributes during recovery"
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <stdint.h>
#include <check.h>
#include <stdbool.h>
#include <errno.h>
#include "posix_recovery.h"
#include "reffs/fs.h"
#include "reffs/test.h"

/*
 * Tree layout:
 *   ino 1  /          (dir,  nlink=3: self + "." + child A)
 *   ino 2  /A         (dir,  nlink=3: self + "." + child B)
 *   ino 3  /A/B       (dir,  nlink=3: self + "." + child C)
 *   ino 4  /A/B/C     (dir,  nlink=2: self + "." only, leaf dir)
 *   ino 5  /A/B/C/f   (file, nlink=1)
 */

START_TEST(test_deep_nested_tree)
{
	struct test_context ctx;
	ck_assert_int_eq(test_setup(&ctx), 0);

	struct inode_disk id_file = { .id_mode = S_IFREG | 0644,
				      .id_nlink = 1 };

	/* Write all .meta files */
	struct inode_disk id_root = { .id_mode = S_IFDIR | 0755,
				      .id_nlink = 3 };
	struct inode_disk id_a = { .id_mode = S_IFDIR | 0755, .id_nlink = 3 };
	struct inode_disk id_b = { .id_mode = S_IFDIR | 0755, .id_nlink = 3 };
	struct inode_disk id_c = { .id_mode = S_IFDIR | 0755, .id_nlink = 2 };

	test_write_meta(&ctx, 1, &id_root);
	test_write_meta(&ctx, 2, &id_a);
	test_write_meta(&ctx, 3, &id_b);
	test_write_meta(&ctx, 4, &id_c);
	test_write_meta(&ctx, 5, &id_file);

	/* Write .dir files */
	int fd;
	ck_assert_int_eq(test_write_dir_header(&ctx, 1, 4, &fd), 0);
	test_write_dir_entry(fd, 3, 2, "A");
	close(fd);

	ck_assert_int_eq(test_write_dir_header(&ctx, 2, 4, &fd), 0);
	test_write_dir_entry(fd, 3, 3, "B");
	close(fd);

	ck_assert_int_eq(test_write_dir_header(&ctx, 3, 4, &fd), 0);
	test_write_dir_entry(fd, 3, 4, "C");
	close(fd);

	ck_assert_int_eq(test_write_dir_header(&ctx, 4, 4, &fd), 0);
	test_write_dir_entry(fd, 3, 5, "f");
	close(fd);

	reffs_fs_recover(ctx.sb);

	/* sb_next_ino must be 6 (max ino 5 + 1) */
	ck_assert_uint_eq(ctx.sb->sb_next_ino, 6);

	/* Every node must be findable */
	struct reffs_dirent *root = ctx.sb->sb_dirent;

	struct reffs_dirent *de_a =
		dirent_find(root, reffs_text_case_sensitive, "A");
	ck_assert(de_a != NULL);
	ck_assert(de_a->rd_inode != NULL);
	ck_assert((de_a->rd_inode->i_mode & S_IFMT) == S_IFDIR);
	ck_assert_uint_eq(de_a->rd_inode->i_nlink, 3);

	struct reffs_dirent *de_b =
		dirent_find(de_a, reffs_text_case_sensitive, "B");
	ck_assert(de_b != NULL);
	ck_assert_uint_eq(de_b->rd_inode->i_nlink, 3);

	struct reffs_dirent *de_c =
		dirent_find(de_b, reffs_text_case_sensitive, "C");
	ck_assert(de_c != NULL);
	ck_assert_uint_eq(de_c->rd_inode->i_nlink, 2);

	struct reffs_dirent *de_f =
		dirent_find(de_c, reffs_text_case_sensitive, "f");
	ck_assert(de_f != NULL);
	ck_assert((de_f->rd_inode->i_mode & S_IFMT) == S_IFREG);

	/* i_parent pointers must be wired correctly */
	ck_assert(de_a->rd_inode->i_parent == de_a);
	ck_assert(de_b->rd_inode->i_parent == de_b);
	ck_assert(de_c->rd_inode->i_parent == de_c);

	dirent_put(de_f);
	dirent_put(de_c);
	dirent_put(de_b);
	dirent_put(de_a);

	test_teardown(&ctx);
}
END_TEST

Suite *recovery_suite(void)
{
	Suite *s = suite_create("POSIX Recovery 10: Deep Nested Tree");
	TCase *tc = tcase_create("Core");
	tcase_add_test(tc, test_deep_nested_tree);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int number_failed;
	Suite *s;
	SRunner *sr;

	rcu_register_thread();
	s = recovery_suite();
	sr = srunner_create(s);
	srunner_set_fork_status(sr, CK_NOFORK);
	srunner_run_all(sr, CK_NORMAL);
	number_failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	rcu_unregister_thread();

	return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
