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
#include "reffs/data_block.h"

START_TEST(test_data_integrity)
{
	struct test_context ctx;
	struct inode_disk id_dir = { 0 };
	id_dir.id_mode = S_IFDIR | 0755;
	id_dir.id_nlink = 2;

	struct inode_disk id_file = { 0 };
	id_file.id_mode = S_IFREG | 0644;
	id_file.id_nlink = 1;
	id_file.id_size = 12;

	ck_assert_int_eq(test_setup(&ctx), 0);

	/* Write root metadata */
	test_write_meta(&ctx, 1, &id_dir);

	/* Root directory contains one file */
	int fd;
	test_write_dir_header(&ctx, 1, 4, &fd);
	test_write_dir_entry(fd, 3, 2, "myfile");
	close(fd);

	/* Write file metadata and data */
	const char *test_data = "Hello World!";
	test_write_meta(&ctx, 2, &id_file);
	test_write_dat(&ctx, 2, test_data, 12);

	reffs_fs_recover(ctx.sb);

	struct inode *inode = inode_find(ctx.sb, 2);
	ck_assert(inode);
	ck_assert(inode->i_db != NULL);

	/* Verification 1: Size must be restored via fstat() in data_block_alloc */
	ck_assert_uint_eq(data_block_get_size(inode->i_db), 12);

	/* Verification 2: Data must be readable and identical */
	char buffer[13] = { 0 };
	size_t read_len = data_block_read(inode->i_db, buffer, 12, 0);
	ck_assert_uint_eq(read_len, 12);
	ck_assert_str_eq(buffer, test_data);

	inode_active_put(inode);

	test_teardown(&ctx);
}
END_TEST

Suite *recovery_suite(void)
{
	Suite *s = suite_create("POSIX Recovery 5: Data Integrity");
	TCase *tc = tcase_create("Core");
	tcase_add_test(tc, test_data_integrity);
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
