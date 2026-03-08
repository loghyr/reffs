/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
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
#include <unistd.h>
#include "posix_recovery.h"
#include "reffs/fs.h"
#include "reffs/inode.h"
#include "reffs/test.h"

START_TEST(test_atomic_write)
{
	struct test_context ctx;
	ck_assert_int_eq(test_setup(&ctx), 0);

	/*
	 * Use the root inode (ino 1), which is already properly allocated and
	 * owned by the root dirent created in test_setup().  Calling
	 * inode_alloc() with a hardcoded ino 2 would bypass sb_next_ino
	 * accounting and leave an orphan inode in the hash table that is not
	 * reachable via any dirent — risking an assert in
	 * super_block_remove_all_inodes() during teardown if the refcount
	 * bookkeeping goes wrong.
	 */
	struct inode *inode = ctx.sb->sb_dirent->rd_inode;
	ck_assert(inode != NULL);

	inode->i_mode = S_IFDIR | 0755;
	inode->i_uid = 0;
	inode->i_gid = 0;
	inode->i_nlink = 2;
	inode->i_size = 0;

	/* 
	 * Call sync - this should trigger the temp-file + rename logic.
	 * We verify that the final file exists and is correct.
	 */
	inode_sync_to_disk(inode);

	char path[1024];
	snprintf(path, sizeof(path), "%s/sb_1/ino_%lu.meta", ctx.backend_path,
		 inode->i_ino);
	ck_assert_int_eq(access(path, F_OK), 0);

	/* Ensure no stray .tmp file is left behind */
	char tmp_path[1024];
	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
	ck_assert_int_ne(access(tmp_path, F_OK), 0);

	/*
	 * Verify the written content round-trips correctly: read the .meta
	 * file back and check the mode field.
	 */
	struct {
		struct reffs_disk_header hdr;
		struct inode_disk id;
	} meta = { 0 };

	int fd = open(path, O_RDONLY);
	ck_assert(fd >= 0);
	ck_assert_int_eq(read(fd, &meta, sizeof(meta)), (int)sizeof(meta));
	close(fd);
	ck_assert_uint_eq(meta.id.id_mode, inode->i_mode);
	ck_assert_uint_eq(meta.id.id_nlink, inode->i_nlink);

	/* Do NOT call inode_put() here — the root inode is owned by the
	 * root dirent and will be released by test_teardown(). */
	test_teardown(&ctx);
}
END_TEST

Suite *recovery_suite(void)
{
	Suite *s = suite_create("POSIX Recovery 6: Atomic Write");
	TCase *tc = tcase_create("Core");
	tcase_add_test(tc, test_atomic_write);
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
