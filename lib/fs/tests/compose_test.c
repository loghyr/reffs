/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Backend composition tests.
 *
 * Verify that reffs_backend_compose() correctly assembles md + data
 * function pointers, enforces constraints, and that the composed
 * inode_free cleans up both metadata and data files.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <check.h>

#include "reffs/backend.h"
#include "reffs/data_block.h"
#include "reffs/dirent.h"
#include "reffs/fs.h"
#include "reffs/inode.h"
#include "reffs/super_block.h"
#include "fs_test_harness.h"

static char state_dir[] = "/tmp/reffs-compose-XXXXXX";

static void compose_setup(void)
{
	fs_test_setup();
	ck_assert_ptr_nonnull(mkdtemp(state_dir));
}

static void compose_teardown(void)
{
	char cmd[512];

	snprintf(cmd, sizeof(cmd), "rm -rf %s", state_dir);
	system(cmd);
	strcpy(state_dir, "/tmp/reffs-compose-XXXXXX");

	fs_test_teardown();
}

/* ------------------------------------------------------------------ */
/* Composition constraint tests                                        */
/* ------------------------------------------------------------------ */

START_TEST(test_compose_ram_ram)
{
	struct reffs_storage_ops *ops =
		reffs_backend_compose(REFFS_MD_RAM, REFFS_DATA_RAM);

	ck_assert_ptr_nonnull(ops);
	ck_assert(ops->sb_alloc != NULL);
	ck_assert(ops->db_alloc != NULL);
	ck_assert(ops->db_read != NULL);
	ck_assert(ops->db_write != NULL);
	ck_assert(ops->db_free != NULL);
	ck_assert(ops->db_get_fd != NULL);
	ck_assert(ops->inode_free != NULL);
	ck_assert_int_eq(ops->type, REFFS_STORAGE_RAM);

	reffs_backend_free_ops(ops);
}
END_TEST

START_TEST(test_compose_posix_posix)
{
	struct reffs_storage_ops *ops =
		reffs_backend_compose(REFFS_MD_POSIX, REFFS_DATA_POSIX);

	ck_assert_ptr_nonnull(ops);
	ck_assert(ops->sb_alloc != NULL);
	ck_assert(ops->sb_free != NULL);
	ck_assert(ops->inode_alloc != NULL);
	ck_assert(ops->inode_free != NULL);
	ck_assert(ops->inode_sync != NULL);
	ck_assert(ops->dir_sync != NULL);
	ck_assert(ops->db_alloc != NULL);
	ck_assert(ops->db_read != NULL);
	ck_assert(ops->db_write != NULL);
	ck_assert(ops->db_get_fd != NULL);
	ck_assert_int_eq(ops->type, REFFS_STORAGE_POSIX);

	reffs_backend_free_ops(ops);
}
END_TEST

START_TEST(test_compose_ram_posix_rejected)
{
	/* RAM md + POSIX data violates the constraint */
	struct reffs_storage_ops *ops =
		reffs_backend_compose(REFFS_MD_RAM, REFFS_DATA_POSIX);
	ck_assert_ptr_null(ops);
}
END_TEST

START_TEST(test_compose_posix_ram_rejected)
{
	/* POSIX md + RAM data violates the constraint */
	struct reffs_storage_ops *ops =
		reffs_backend_compose(REFFS_MD_POSIX, REFFS_DATA_RAM);
	ck_assert_ptr_null(ops);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Functional round-trip tests                                         */
/* ------------------------------------------------------------------ */

START_TEST(test_compose_posix_inode_roundtrip)
{
	/*
	 * Create a POSIX-backed sb via super_block_alloc (which uses
	 * the composer), write an inode, sync, and verify the .meta
	 * file exists on disk.
	 */
	struct super_block *sb = super_block_alloc(
		200, "/compose_test", REFFS_STORAGE_POSIX, state_dir);
	ck_assert_ptr_nonnull(sb);

	int ret = super_block_dirent_create(sb, NULL, reffs_life_action_birth);
	ck_assert_int_eq(ret, 0);

	/* The root inode should have been synced to disk */
	struct inode *root = sb->sb_dirent->rd_inode;
	ck_assert_ptr_nonnull(root);

	inode_sync_to_disk(root);

	/* Verify .meta file exists */
	char path[512];
	snprintf(path, sizeof(path), "%s/sb_200/ino_%lu.meta", state_dir,
		 (unsigned long)root->i_ino);
	ck_assert_int_eq(access(path, F_OK), 0);

	super_block_release_dirents(sb);
	super_block_put(sb);
}
END_TEST

START_TEST(test_compose_posix_data_roundtrip)
{
	/*
	 * Create a POSIX-backed sb, allocate a data block, write data,
	 * read it back, and verify the fd is valid.
	 */
	struct super_block *sb = super_block_alloc(
		201, "/compose_data", REFFS_STORAGE_POSIX, state_dir);
	ck_assert_ptr_nonnull(sb);

	int ret = super_block_dirent_create(sb, NULL, reffs_life_action_birth);
	ck_assert_int_eq(ret, 0);

	/* Create a regular file inode */
	struct inode *inode = inode_alloc(sb, 0);
	ck_assert_ptr_nonnull(inode);
	inode->i_mode = S_IFREG | 0644;

	/* Allocate a data block */
	const char *test_data = "hello composition";
	size_t data_len = strlen(test_data);
	struct data_block *db = data_block_alloc(inode, test_data, data_len, 0);
	ck_assert_ptr_nonnull(db);

	/* Verify fd is valid (POSIX data backend) */
	int fd = data_block_get_fd(db);
	ck_assert_int_ge(fd, 0);

	/* Read back */
	char buf[64] = { 0 };
	ssize_t nread = data_block_read(db, buf, data_len, 0);
	ck_assert_int_eq(nread, (ssize_t)data_len);
	ck_assert_str_eq(buf, test_data);

	data_block_put(db);
	inode->i_db = NULL;
	inode_active_put(inode);
	super_block_release_dirents(sb);
	super_block_put(sb);
}
END_TEST

START_TEST(test_compose_inode_free_cleans_both)
{
	/*
	 * Create a POSIX-backed sb, create an inode with both .meta
	 * and .dat files, then trigger inode_free and verify BOTH
	 * files are removed.
	 */
	struct super_block *sb = super_block_alloc(
		202, "/compose_clean", REFFS_STORAGE_POSIX, state_dir);
	ck_assert_ptr_nonnull(sb);

	int ret = super_block_dirent_create(sb, NULL, reffs_life_action_birth);
	ck_assert_int_eq(ret, 0);

	/* Create a file inode */
	struct inode *inode = inode_alloc(sb, 0);
	ck_assert_ptr_nonnull(inode);
	inode->i_mode = S_IFREG | 0644;

	uint64_t ino = inode->i_ino;

	/* Sync metadata to create .meta file */
	inode_sync_to_disk(inode);

	/* Create data file via data_block_alloc */
	const char *test_data = "cleanup test";
	struct data_block *db =
		data_block_alloc(inode, test_data, strlen(test_data), 0);
	ck_assert_ptr_nonnull(db);

	/* Verify both files exist */
	char meta_path[512], dat_path[512];
	snprintf(meta_path, sizeof(meta_path), "%s/sb_202/ino_%lu.meta",
		 state_dir, (unsigned long)ino);
	snprintf(dat_path, sizeof(dat_path), "%s/sb_202/ino_%lu.dat", state_dir,
		 (unsigned long)ino);

	ck_assert_int_eq(access(meta_path, F_OK), 0);
	ck_assert_int_eq(access(dat_path, F_OK), 0);

	/* Free the data block first (closes fd) */
	data_block_put(db);
	inode->i_db = NULL;

	/*
	 * Call inode_free directly -- the composed wrapper should
	 * clean up both .meta (md) and .dat (data).
	 */
	sb->sb_ops->inode_free(inode);

	/* Both files should be gone */
	ck_assert_int_ne(access(meta_path, F_OK), 0);
	ck_assert_int_ne(access(dat_path, F_OK), 0);

	inode_active_put(inode);
	super_block_release_dirents(sb);
	super_block_put(sb);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

static Suite *compose_suite(void)
{
	Suite *s = suite_create("backend_composition");
	TCase *tc;

	tc = tcase_create("constraints");
	tcase_add_checked_fixture(tc, compose_setup, compose_teardown);
	tcase_add_test(tc, test_compose_ram_ram);
	tcase_add_test(tc, test_compose_posix_posix);
	tcase_add_test(tc, test_compose_ram_posix_rejected);
	tcase_add_test(tc, test_compose_posix_ram_rejected);
	suite_add_tcase(s, tc);

	tc = tcase_create("functional");
	tcase_add_checked_fixture(tc, compose_setup, compose_teardown);
	tcase_add_test(tc, test_compose_posix_inode_roundtrip);
	tcase_add_test(tc, test_compose_posix_data_roundtrip);
	tcase_add_test(tc, test_compose_inode_free_cleans_both);
	suite_add_tcase(s, tc);

	return s;
}

int main(void)
{
	int nfailed;
	Suite *s = compose_suite();
	SRunner *sr = srunner_create(s);

	srunner_run_all(sr, CK_NORMAL);
	nfailed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return nfailed ? 1 : 0;
}
