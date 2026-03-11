/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * Test 12: Stray .tmp files from a crashed previous write do not corrupt
 *          recovery and are ignored or cleaned up.
 *
 * The atomic write path (fix for the non-atomic .meta/.dir writes) writes
 * to a temp file then renames it over the target.  If the server crashed
 * after writing the .tmp but before the rename, a stray .tmp file is left
 * behind.  Recovery must:
 *   1. Prefer the existing .meta (the last successful commit) over the .tmp.
 *   2. Not crash or use the partial .tmp data.
 *   3. After a subsequent inode_sync_to_disk(), no .tmp file remains.
 *
 * Related issue (reffs_issues.md):
 *   POSIX Backend — ".dir and .meta writes are not atomic": write to temp
 *   file and rename() atomically over the destination.
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
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "posix_recovery.h"
#include "reffs/fs.h"
#include "reffs/test.h"

START_TEST(test_stray_tmp_ignored)
{
	struct test_context ctx;
	ck_assert_int_eq(test_setup(&ctx), 0);

	struct inode_disk id_dir = { .id_mode = S_IFDIR | 0755, .id_nlink = 2 };
	struct inode_disk id_good = { .id_mode = S_IFREG | 0644,
				      .id_nlink = 1,
				      .id_uid = 42,
				      .id_gid = 42 };
	struct inode_disk id_bad = { 0 }; /* zeroed — simulates partial write */

	test_write_meta(&ctx, 1, &id_dir);
	test_write_meta(&ctx, 2, &id_good);

	/*
	 * Simulate a stray .tmp left by a crash: write garbage (zeroed
	 * inode_disk) to ino_2.meta.tmp.
	 */
	char tmp_path[1024];
	snprintf(tmp_path, sizeof(tmp_path), "%s/sb_1/ino_2.meta.tmp",
		 ctx.backend_path);
	int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	ck_assert(fd >= 0);
	write(fd, &id_bad, sizeof(id_bad));
	close(fd);

	int dir_fd;
	ck_assert_int_eq(test_write_dir_header(&ctx, 1, 4, &dir_fd), 0);
	test_write_dir_entry(dir_fd, 3, 2, "myfile");
	close(dir_fd);

	reffs_fs_recover(ctx.sb);

	/* The committed .meta must win; uid must be 42, not 0 */
	struct inode *inode = inode_find(ctx.sb, 2);
	ck_assert(inode != NULL);
	ck_assert_uint_eq(inode->i_uid, 42);
	ck_assert_uint_eq(inode->i_gid, 42);

	/*
	 * A subsequent sync must remove any stray .tmp.
	 * (Tests that inode_sync_to_disk follows the temp-then-rename
	 *  pattern and doesn't accidentally leave a .tmp behind.)
	 */
	inode_sync_to_disk(inode);
	ck_assert_int_ne(access(tmp_path, F_OK), 0); /* must not exist */

	inode_active_put(inode);
	test_teardown(&ctx);
}
END_TEST

Suite *recovery_suite(void)
{
	Suite *s = suite_create("POSIX Recovery 12: Stray .tmp File Ignored");
	TCase *tc = tcase_create("Core");
	tcase_add_test(tc, test_stray_tmp_ignored);
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
