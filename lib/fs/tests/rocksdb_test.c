/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * RocksDB backend unit tests.
 *
 * Tests the RocksDB metadata backend (per-sb database) and the
 * RocksDB namespace database (server-wide persistence).  Exercises
 * inode/dir/symlink round-trips, recovery, composed inode_free
 * cleanup, and namespace server_state + client persistence.
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
#include "reffs/persist_ops.h"
#include "reffs/server_persist.h"
#include "reffs/client_persist.h"
#include "reffs/super_block.h"
#include "fs_test_harness.h"

static char state_dir[] = "/tmp/reffs-rocksdb-XXXXXX";

static void rdb_setup(void)
{
	fs_test_setup();
	ck_assert_ptr_nonnull(mkdtemp(state_dir));
}

static void rdb_teardown(void)
{
	char cmd[512];

	snprintf(cmd, sizeof(cmd), "rm -rf %s", state_dir);
	system(cmd);
	strcpy(state_dir, "/tmp/reffs-rocksdb-XXXXXX");

	fs_test_teardown();
}

/* ------------------------------------------------------------------ */
/* Per-sb database tests                                               */
/* ------------------------------------------------------------------ */

START_TEST(test_rocksdb_sb_alloc_free)
{
	struct super_block *sb = super_block_alloc(
		300, "/rdb_test", REFFS_STORAGE_ROCKSDB, state_dir);
	ck_assert_ptr_nonnull(sb);

	/* Verify md.rocksdb directory was created */
	char path[512];
	snprintf(path, sizeof(path), "%s/sb_300/md.rocksdb", state_dir);
	struct stat st;
	ck_assert_int_eq(stat(path, &st), 0);
	ck_assert(S_ISDIR(st.st_mode));

	super_block_put(sb);
}
END_TEST

START_TEST(test_rocksdb_inode_roundtrip)
{
	struct super_block *sb = super_block_alloc(
		301, "/rdb_ino", REFFS_STORAGE_ROCKSDB, state_dir);
	ck_assert_ptr_nonnull(sb);

	int ret = super_block_dirent_create(sb, NULL, reffs_life_action_birth);
	ck_assert_int_eq(ret, 0);

	/* Create a file inode and set attributes */
	struct inode *inode = inode_alloc(sb, 0);
	ck_assert_ptr_nonnull(inode);
	inode->i_mode = S_IFREG | 0644;
	inode->i_uid = 1000;
	inode->i_gid = 1000;
	inode->i_nlink = 1;
	inode->i_size = 42;

	uint64_t ino = inode->i_ino;

	/* Sync to RocksDB */
	inode_sync_to_disk(inode);
	inode_active_put(inode);

	/* Load it back via a fresh inode_alloc */
	struct inode *loaded = inode_alloc(sb, ino);
	ck_assert_ptr_nonnull(loaded);
	ck_assert_uint_eq(loaded->i_uid, 1000);
	ck_assert_uint_eq(loaded->i_gid, 1000);
	ck_assert_uint_eq(loaded->i_nlink, 1);
	ck_assert_int_eq(loaded->i_size, 42);
	ck_assert_uint_eq(loaded->i_mode, S_IFREG | 0644);

	inode_active_put(loaded);
	super_block_release_dirents(sb);
	super_block_put(sb);
}
END_TEST

START_TEST(test_rocksdb_data_posix_roundtrip)
{
	struct super_block *sb = super_block_alloc(
		302, "/rdb_data", REFFS_STORAGE_ROCKSDB, state_dir);
	ck_assert_ptr_nonnull(sb);

	int ret = super_block_dirent_create(sb, NULL, reffs_life_action_birth);
	ck_assert_int_eq(ret, 0);

	struct inode *inode = inode_alloc(sb, 0);
	ck_assert_ptr_nonnull(inode);
	inode->i_mode = S_IFREG | 0644;

	/* Write data through the composed data backend (POSIX) */
	const char *test_data = "rocksdb data test";
	size_t len = strlen(test_data);
	struct data_block *db = data_block_alloc(inode, test_data, len, 0);
	ck_assert_ptr_nonnull(db);

	/* fd should be valid (POSIX data backend) */
	int fd = data_block_get_fd(db);
	ck_assert_int_ge(fd, 0);

	/* Read back */
	char buf[64] = { 0 };
	ssize_t nread = data_block_read(db, buf, len, 0);
	ck_assert_int_eq(nread, (ssize_t)len);
	ck_assert_str_eq(buf, test_data);

	data_block_put(db);
	inode->i_db = NULL;
	inode_active_put(inode);
	super_block_release_dirents(sb);
	super_block_put(sb);
}
END_TEST

START_TEST(test_rocksdb_dir_roundtrip)
{
	struct super_block *sb = super_block_alloc(
		303, "/rdb_dir", REFFS_STORAGE_ROCKSDB, state_dir);
	ck_assert_ptr_nonnull(sb);

	int ret = super_block_dirent_create(sb, NULL, reffs_life_action_birth);
	ck_assert_int_eq(ret, 0);

	/* Create child inodes and attach to root dir */
	struct reffs_dirent *root_de = sb->sb_dirent;
	struct inode *root_inode = root_de->rd_inode;

	struct reffs_dirent *child1 =
		dirent_alloc(root_de, "file1", reffs_life_action_birth, false);
	ck_assert_ptr_nonnull(child1);
	struct inode *ino1 = inode_alloc(sb, 0);
	ck_assert_ptr_nonnull(ino1);
	ino1->i_mode = S_IFREG | 0644;
	dirent_attach_inode(child1, ino1);
	uint64_t child1_ino = ino1->i_ino;
	inode_active_put(ino1);
	dirent_put(child1);

	struct reffs_dirent *child2 =
		dirent_alloc(root_de, "subdir", reffs_life_action_birth, false);
	ck_assert_ptr_nonnull(child2);
	struct inode *ino2 = inode_alloc(sb, 0);
	ck_assert_ptr_nonnull(ino2);
	ino2->i_mode = S_IFDIR | 0755;
	dirent_attach_inode(child2, ino2);
	inode_active_put(ino2);
	dirent_put(child2);

	/* Sync directory to RocksDB */
	if (sb->sb_ops->dir_sync)
		sb->sb_ops->dir_sync(root_inode);

	/* Find by name */
	uint64_t found_ino = 0;
	uint64_t found_cookie = 0;
	ret = sb->sb_ops->dir_find_entry_by_name(sb, root_inode->i_ino, "file1",
						 &found_ino, &found_cookie);
	ck_assert_int_eq(ret, 0);
	ck_assert_uint_eq(found_ino, child1_ino);

	/* Find by ino */
	char name_buf[256] = { 0 };
	ret = sb->sb_ops->dir_find_entry_by_ino(sb, root_inode->i_ino,
						child1_ino, name_buf,
						sizeof(name_buf),
						&found_cookie);
	ck_assert_int_eq(ret, 0);
	ck_assert_str_eq(name_buf, "file1");

	/* Not found */
	ret = sb->sb_ops->dir_find_entry_by_name(
		sb, root_inode->i_ino, "nosuch", &found_ino, &found_cookie);
	ck_assert_int_eq(ret, -ENOENT);

	super_block_release_dirents(sb);
	super_block_put(sb);
}
END_TEST

START_TEST(test_rocksdb_inode_delete_cleanup)
{
	struct super_block *sb = super_block_alloc(
		304, "/rdb_del", REFFS_STORAGE_ROCKSDB, state_dir);
	ck_assert_ptr_nonnull(sb);

	int ret = super_block_dirent_create(sb, NULL, reffs_life_action_birth);
	ck_assert_int_eq(ret, 0);

	struct inode *inode = inode_alloc(sb, 0);
	ck_assert_ptr_nonnull(inode);
	inode->i_mode = S_IFREG | 0644;
	uint64_t ino = inode->i_ino;

	/* Sync metadata to RocksDB */
	inode_sync_to_disk(inode);

	/* Create data file via POSIX data backend */
	struct data_block *db = data_block_alloc(inode, "delete me", 9, 0);
	ck_assert_ptr_nonnull(db);

	/* Verify .dat file exists */
	char dat_path[512];
	snprintf(dat_path, sizeof(dat_path), "%s/sb_304/ino_%lu.dat", state_dir,
		 (unsigned long)ino);
	ck_assert_int_eq(access(dat_path, F_OK), 0);

	data_block_put(db);
	inode->i_db = NULL;

	/* inode_free should clean up RocksDB keys AND .dat file */
	sb->sb_ops->inode_free(inode);

	/* .dat should be gone */
	ck_assert_int_ne(access(dat_path, F_OK), 0);

	/* Loading the inode should return empty (keys deleted) */
	struct inode *ghost = inode_alloc(sb, ino);
	ck_assert_ptr_nonnull(ghost);
	ck_assert_uint_eq(ghost->i_uid, 0); /* fresh, not loaded */
	inode_active_put(ghost);

	inode_active_put(inode);
	super_block_release_dirents(sb);
	super_block_put(sb);
}
END_TEST

START_TEST(test_rocksdb_key_ordering)
{
	/* Verify BE64 keys sort correctly in RocksDB */
	struct super_block *sb = super_block_alloc(
		305, "/rdb_keys", REFFS_STORAGE_ROCKSDB, state_dir);
	ck_assert_ptr_nonnull(sb);

	int ret = super_block_dirent_create(sb, NULL, reffs_life_action_birth);
	ck_assert_int_eq(ret, 0);

	/* Create inodes with specific numbers and sync */
	uint64_t inos[] = { 100, 1, 65536, 256 };
	for (int i = 0; i < 4; i++) {
		struct inode *inode = inode_alloc(sb, inos[i]);
		ck_assert_ptr_nonnull(inode);
		inode->i_mode = S_IFREG | 0644;
		inode->i_uid = inos[i]; /* use uid as marker */
		inode_sync_to_disk(inode);
		inode_active_put(inode);
	}

	/* Load back — each should have the correct uid */
	for (int i = 0; i < 4; i++) {
		struct inode *loaded = inode_alloc(sb, inos[i]);
		ck_assert_ptr_nonnull(loaded);
		ck_assert_uint_eq(loaded->i_uid, inos[i]);
		inode_active_put(loaded);
	}

	super_block_release_dirents(sb);
	super_block_put(sb);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Namespace database tests                                            */
/* ------------------------------------------------------------------ */

START_TEST(test_rocksdb_ns_server_state)
{
	const struct persist_ops *ops = NULL;
	void *ctx = NULL;

	int ret = rocksdb_namespace_init(state_dir, &ops, &ctx);
	ck_assert_int_eq(ret, 0);
	ck_assert_ptr_nonnull(ops);
	ck_assert_ptr_nonnull(ctx);

	/* Save server state */
	struct server_persistent_state sps = {
		.sps_magic = 0x52454646U,
		.sps_version = 1,
		.sps_boot_seq = 42,
		.sps_clean_shutdown = 1,
		.sps_slot_next = 7,
		.sps_lease_time = 90,
	};

	ret = ops->server_state_save(ctx, &sps);
	ck_assert_int_eq(ret, 0);

	/* Load it back */
	struct server_persistent_state loaded = { 0 };
	ret = ops->server_state_load(ctx, &loaded);
	ck_assert_int_eq(ret, 0);
	ck_assert_uint_eq(loaded.sps_boot_seq, 42);
	ck_assert_uint_eq(loaded.sps_slot_next, 7);
	ck_assert_uint_eq(loaded.sps_lease_time, 90);

	ops->fini(ctx);
}
END_TEST

START_TEST(test_rocksdb_ns_server_state_fresh)
{
	const struct persist_ops *ops = NULL;
	void *ctx = NULL;

	int ret = rocksdb_namespace_init(state_dir, &ops, &ctx);
	ck_assert_int_eq(ret, 0);

	/* Load from fresh DB — should return -ENOENT */
	struct server_persistent_state sps = { 0 };
	ret = ops->server_state_load(ctx, &sps);
	ck_assert_int_eq(ret, -ENOENT);

	ops->fini(ctx);
}
END_TEST

START_TEST(test_rocksdb_ns_incarnation_roundtrip)
{
	const struct persist_ops *ops = NULL;
	void *ctx = NULL;

	int ret = rocksdb_namespace_init(state_dir, &ops, &ctx);
	ck_assert_int_eq(ret, 0);

	/* Add two incarnation records */
	struct client_incarnation_record crc1 = {
		.crc_magic = 0x434C4943U,
		.crc_slot = 1,
		.crc_boot_seq = 5,
		.crc_incarnation = 1,
	};
	struct client_incarnation_record crc2 = {
		.crc_magic = 0x434C4943U,
		.crc_slot = 2,
		.crc_boot_seq = 5,
		.crc_incarnation = 1,
	};

	ret = ops->client_incarnation_add(ctx, &crc1);
	ck_assert_int_eq(ret, 0);
	ret = ops->client_incarnation_add(ctx, &crc2);
	ck_assert_int_eq(ret, 0);

	/* Load all */
	struct client_incarnation_record loaded[8];
	size_t count = 0;
	ret = ops->client_incarnation_load(ctx, loaded, 8, &count);
	ck_assert_int_eq(ret, 0);
	ck_assert_uint_eq(count, 2);

	/* Remove one */
	ret = ops->client_incarnation_remove(ctx, 1);
	ck_assert_int_eq(ret, 0);

	/* Load again — should be 1 */
	count = 0;
	ret = ops->client_incarnation_load(ctx, loaded, 8, &count);
	ck_assert_int_eq(ret, 0);
	ck_assert_uint_eq(count, 1);
	ck_assert_uint_eq(loaded[0].crc_slot, 2);

	ops->fini(ctx);
}
END_TEST

struct identity_cb_data {
	int count;
	uint32_t last_slot;
};

static int identity_cb(const struct client_identity_record *r, void *a)
{
	struct identity_cb_data *d = a;

	d->count++;
	d->last_slot = r->cir_slot;
	return 0;
}

START_TEST(test_rocksdb_ns_client_identity)
{
	const struct persist_ops *ops = NULL;
	void *ctx = NULL;

	int ret = rocksdb_namespace_init(state_dir, &ops, &ctx);
	ck_assert_int_eq(ret, 0);

	/* Append an identity record */
	struct client_identity_record cir = {
		.cir_magic = 0x434C4944U,
		.cir_slot = 10,
		.cir_ownerid_len = 6,
	};
	memcpy(cir.cir_ownerid, "client", 6);
	strncpy(cir.cir_domain, "test.com", sizeof(cir.cir_domain) - 1);

	ret = ops->client_identity_append(ctx, &cir);
	ck_assert_int_eq(ret, 0);

	/* Load via callback */
	struct identity_cb_data cb_data = { 0, 0 };
	ret = ops->client_identity_load(ctx, identity_cb, &cb_data);

	ck_assert_int_eq(ret, 0);
	ck_assert_int_eq(cb_data.count, 1);
	ck_assert_uint_eq(cb_data.last_slot, 10);

	ops->fini(ctx);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

static Suite *rocksdb_suite(void)
{
	Suite *s = suite_create("rocksdb_backend");
	TCase *tc;

	tc = tcase_create("per_sb");
	tcase_add_checked_fixture(tc, rdb_setup, rdb_teardown);
	tcase_add_test(tc, test_rocksdb_sb_alloc_free);
	tcase_add_test(tc, test_rocksdb_inode_roundtrip);
	tcase_add_test(tc, test_rocksdb_data_posix_roundtrip);
	tcase_add_test(tc, test_rocksdb_dir_roundtrip);
	tcase_add_test(tc, test_rocksdb_inode_delete_cleanup);
	tcase_add_test(tc, test_rocksdb_key_ordering);
	suite_add_tcase(s, tc);

	tc = tcase_create("namespace");
	tcase_add_checked_fixture(tc, rdb_setup, rdb_teardown);
	tcase_add_test(tc, test_rocksdb_ns_server_state);
	tcase_add_test(tc, test_rocksdb_ns_server_state_fresh);
	tcase_add_test(tc, test_rocksdb_ns_incarnation_roundtrip);
	tcase_add_test(tc, test_rocksdb_ns_client_identity);
	suite_add_tcase(s, tc);

	return s;
}

int main(void)
{
	int nfailed;
	Suite *s = rocksdb_suite();
	SRunner *sr = srunner_create(s);

	srunner_run_all(sr, CK_NORMAL);
	nfailed = srunner_ntests_failed(sr);
	srunner_free(sr);
	return nfailed ? 1 : 0;
}
