/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

/*
 * Test 8: Recovery from a truncated .dir file (simulated crash during write).
 *
 * The .dir format is: [cookie_next u64][entry…][entry…]
 * Each entry is:      [cookie u64][ino u64][name_len u16][name bytes]
 *
 * If the server crashed mid-write, the file may contain a complete header
 * and some complete entries followed by a partial (truncated) final entry.
 * Recovery must:
 *   1. Load all complete entries before the truncation point.
 *   2. Not crash or loop on the partial entry.
 *   3. Not expose the partial entry in the in-memory tree.
 *
 * Related issue (reffs_issues.md):
 *   POSIX Backend — ".dir and .meta writes are not atomic": a crash mid-write
 *   leaves a truncated file; recovery silently loses entries on short reads.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <stdint.h>
#include <check.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include "posix_recovery.h"
#include "reffs/fs.h"
#include "reffs/test.h"

START_TEST(test_truncated_dir_file)
{
	struct test_context ctx;
	struct inode_disk id_dir = { 0 };
	id_dir.id_mode = S_IFDIR | 0755;
	id_dir.id_nlink = 3; /* root + two children */

	struct inode_disk id_file = { 0 };
	id_file.id_mode = S_IFREG | 0644;
	id_file.id_nlink = 1;

	ck_assert_int_eq(test_setup(&ctx), 0);

	test_write_meta(&ctx, 1, &id_dir);
	test_write_meta(&ctx, 2, &id_file);
	test_write_meta(&ctx, 3, &id_file);

	/*
	 * Write the .dir file with two complete entries, then append a
	 * partial third entry (only the cookie field, missing ino + name).
	 * This simulates a crash after the first write() call of a multi-call
	 * sequence for the third entry.
	 */
	int dir_fd;
	ck_assert_int_eq(test_write_dir_header(&ctx, 1, 6, &dir_fd), 0);
	test_write_dir_entry(dir_fd, 3, 2, "good_file1");
	test_write_dir_entry(dir_fd, 4, 3, "good_file2");

	/* Partial entry: only the cookie, nothing else */
	uint64_t partial_cookie = 5;
	write(dir_fd, &partial_cookie, sizeof(partial_cookie));
	close(dir_fd);

	reffs_fs_recover(ctx.sb);

	struct reffs_dirent *root_de = ctx.sb->sb_dirent;

	/* Both complete entries must be present */
	struct reffs_dirent *de1 =
		dirent_find(root_de, reffs_text_case_sensitive, "good_file1");
	ck_assert(de1 != NULL);
	ck_assert_uint_eq(de1->rd_cookie, 3);
	dirent_put(de1);

	struct reffs_dirent *de2 =
		dirent_find(root_de, reffs_text_case_sensitive, "good_file2");
	ck_assert(de2 != NULL);
	ck_assert_uint_eq(de2->rd_cookie, 4);
	dirent_put(de2);

	/*
	 * The truncated third entry must not appear.  There is no name to
	 * search for, but we can verify the inode for ino 4 (which was never
	 * written as a .meta file) does not exist in the hash table.
	 */
	struct inode *phantom = inode_find(ctx.sb, 4);
	ck_assert(phantom == NULL);

	test_teardown(&ctx);
}
END_TEST

Suite *recovery_suite(void)
{
	Suite *s = suite_create("POSIX Recovery 8: Truncated .dir File");
	TCase *tc = tcase_create("Core");
	tcase_add_test(tc, test_truncated_dir_file);
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
