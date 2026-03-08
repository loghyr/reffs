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
#include "posix_recovery.h"
#include "reffs/fs.h"
#include "reffs/test.h"

START_TEST(test_nlink_restoration)
{
	struct test_context ctx;
	struct inode_disk id_dir = { 0 };
	id_dir.id_mode = S_IFDIR | 0755;
	id_dir.id_nlink = 2;

	struct inode_disk id_file = { 0 };
	id_file.id_mode = S_IFREG | 0644;
	id_file.id_nlink = 2; /* Two hard links */

	ck_assert_int_eq(test_setup(&ctx), 0);

	/* Write root metadata */
	test_write_meta(&ctx, 1, &id_dir);

	/* Root directory contains two names for the same inode (ino 2) */
	int fd;
	test_write_dir_header(&ctx, 1, 5, &fd);
	test_write_dir_entry(fd, 3, 2, "name1");
	test_write_dir_entry(fd, 4, 2, "name2");
	close(fd);

	/* Write file metadata with nlink=2 */
	test_write_meta(&ctx, 2, &id_file);

	reffs_fs_recover(ctx.sb);

	struct inode *inode = inode_find(ctx.sb, 2);
	ck_assert(inode);

	/*
	 * Verification: nlink must be exactly 2 — the value from the .meta
	 * file.  Two failure modes are possible:
	 *
	 * (a) If the dirent_parent_attach() load-path guard is absent,
	 *     i_nlink would be incremented once per dirent (to 2) before
	 *     load_inode_attributes() overwrites it with 2.  In this case
	 *     the test passes vacuously because the overwrite masks the
	 *     increment.  The guard is still important: without it,
	 *     inode_sync_to_disk() would fire on a zeroed inode, corrupting
	 *     the .meta file (covered by test 13).
	 *
	 * (b) If load_inode_attributes() is called twice for the same inode
	 *     (second hard-link entry) and uses += instead of =, i_nlink
	 *     would be 4.  The assertion below catches that.
	 */
	ck_assert_uint_eq(inode->i_nlink, 2);

	/*
	 * Additionally verify that the second call to load_inode_attributes
	 * (for the "name2" dirent pointing at the same ino 2) did not
	 * allocate a second data_block — there is no .dat file, so i_db
	 * must be NULL.
	 */
	ck_assert(inode->i_db == NULL);

	inode_active_put(inode);

	test_teardown(&ctx);
}
END_TEST

Suite *recovery_suite(void)
{
	Suite *s = suite_create("POSIX Recovery 3: Link Count");
	TCase *tc = tcase_create("Core");
	tcase_add_test(tc, test_nlink_restoration);
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
