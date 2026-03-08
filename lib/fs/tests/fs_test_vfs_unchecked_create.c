/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <rpc/rpc.h>
#include "fs_test_harness.h"
#include "reffs/vfs.h"
#include "reffs/data_block.h"
#include "reffs/fs.h"

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

START_TEST(test_vfs_create_unchecked_truncate)
{
	struct super_block *sb;
	struct inode *dir;
	struct inode *file = NULL;
	struct authunix_parms ap;
	int ret;
	const char *name = "testfile";
	char buf[1024];
	char readbuf[1024];
	struct reffs_sattr rs;

	sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	ck_assert_ptr_nonnull(sb);
	dir = inode_find(sb, 1);
	ck_assert_ptr_nonnull(dir);

	struct reffs_context *ctx = reffs_get_context();
	ap.aup_uid = ctx->uid;
	ap.aup_gid = ctx->gid;
	ap.aup_len = 0;
	ap.aup_gids = NULL;

	/* 1. Create file */
	ret = vfs_create(dir, name, 0644, &ap, &file);
	ck_assert_int_eq(ret, 0);
	ck_assert_ptr_nonnull(file);

	/* 2. Write data (all 'A') */
	memset(buf, 'A', sizeof(buf));
	pthread_rwlock_wrlock(&file->i_db_rwlock);
	if (!file->i_db) {
		file->i_db = data_block_alloc(file, buf, sizeof(buf), 0);
	} else {
		data_block_write(file->i_db, buf, sizeof(buf), 0);
	}
	file->i_size = sizeof(buf);
	pthread_rwlock_unlock(&file->i_db_rwlock);

	/* 3. Simulate UNCHECKED CREATE with O_TRUNC */
	inode_put(file);
	file = NULL;

	/* vfs_create returns -EEXIST if file already exists */
	ret = vfs_create(dir, name, 0644, &ap, &file);
	ck_assert_int_eq(ret, -EEXIST);

	/* Get existing inode */
	file = inode_name_get_inode(dir, (char *)name);
	ck_assert_ptr_nonnull(file);

	/* Truncate to 0 */
	memset(&rs, 0, sizeof(rs));
	rs.size = 0;
	rs.size_set = true;
	ret = vfs_setattr(file, &rs, &ap);
	ck_assert_int_eq(ret, 0);
	ck_assert_int_eq(file->i_size, 0);

	/* 4. Write new data (all 'B') */
	memset(buf, 'B', sizeof(buf));
	pthread_rwlock_wrlock(&file->i_db_rwlock);
	/* In our case, i_db should still exist but be size 0 */
	ck_assert_ptr_nonnull(file->i_db);
	ret = data_block_write(file->i_db, buf, sizeof(buf), 0);
	ck_assert_int_eq(ret, sizeof(buf));
	file->i_size = sizeof(buf);
	pthread_rwlock_unlock(&file->i_db_rwlock);

	/* 5. Read back and verify */
	memset(readbuf, 0, sizeof(readbuf));
	pthread_rwlock_rdlock(&file->i_db_rwlock);
	ret = data_block_read(file->i_db, readbuf, sizeof(readbuf), 0);
	pthread_rwlock_unlock(&file->i_db_rwlock);
	ck_assert_int_eq(ret, sizeof(readbuf));

	/* Compare memory - should be all 'B' */
	ck_assert_mem_eq(readbuf, buf, sizeof(buf));

	inode_put(file);
	inode_put(dir);
	super_block_put(sb);
}
END_TEST

START_TEST(test_vfs_write_gap_zero_fill)
{
	struct super_block *sb;
	struct inode *dir;
	struct inode *file = NULL;
	struct authunix_parms ap;
	int ret;
	const char *name = "gapfile";
	char buf[1024];
	char readbuf[2048];

	sb = super_block_find(SUPER_BLOCK_ROOT_ID);
	dir = inode_find(sb, 1);

	struct reffs_context *ctx = reffs_get_context();
	ap.aup_uid = ctx->uid;
	ap.aup_gid = ctx->gid;
	ap.aup_len = 0;
	ap.aup_gids = NULL;

	/* 1. Create file */
	ret = vfs_create(dir, name, 0644, &ap, &file);
	ck_assert_int_eq(ret, 0);

	/* 2. Write at offset 1024 */
	memset(buf, 'X', sizeof(buf));
	pthread_rwlock_wrlock(&file->i_db_rwlock);
	if (!file->i_db) {
		file->i_db = data_block_alloc(file, buf, sizeof(buf), 1024);
	} else {
		data_block_write(file->i_db, buf, sizeof(buf), 1024);
	}
	file->i_size = 1024 + sizeof(buf);
	pthread_rwlock_unlock(&file->i_db_rwlock);

	/* 3. Read back from 0 */
	memset(readbuf, 0xEE, sizeof(readbuf));
	pthread_rwlock_rdlock(&file->i_db_rwlock);
	ret = data_block_read(file->i_db, readbuf, 2048, 0);
	pthread_rwlock_unlock(&file->i_db_rwlock);
	ck_assert_int_eq(ret, 2048);

	/* 4. Verify gap is 0 and data is 'X' */
	for (int i = 0; i < 1024; i++) {
		ck_assert_msg(readbuf[i] == 0,
			      "Gap at index %d should be 0, found %d", i,
			      readbuf[i]);
	}
	for (int i = 1024; i < 2048; i++) {
		ck_assert_msg(readbuf[i] == 'X',
			      "Data at index %d should be 'X', found %c", i,
			      readbuf[i]);
	}

	inode_put(file);
	inode_put(dir);
	super_block_put(sb);
}
END_TEST

Suite *fs_vfs_unchecked_create_suite(void)
{
	Suite *s = suite_create("fs: vfs unchecked create");
	TCase *tc = tcase_create("Core");
	tcase_add_checked_fixture(tc, setup, teardown);
	tcase_add_test(tc, test_vfs_create_unchecked_truncate);
	tcase_add_test(tc, test_vfs_write_gap_zero_fill);
	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int failed;
	fs_test_global_init();
	SRunner *sr = srunner_create(fs_vfs_unchecked_create_suite());
	srunner_set_fork_status(sr, CK_NOFORK);
	srunner_run_all(sr, CK_NORMAL);
	failed = srunner_ntests_failed(sr);
	srunner_free(sr);
	fs_test_global_fini();
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
