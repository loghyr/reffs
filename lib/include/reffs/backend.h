/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_BACKEND_H
#define _REFFS_BACKEND_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include "reffs/types.h"

struct super_block;
struct inode;
struct data_block;

#define REFFS_DISK_MAGIC_META 0x5245464d /* 'REFM' */
#define REFFS_DISK_MAGIC_DIR 0x52454644 /* 'REFD' */
#define REFFS_DISK_MAGIC_DAT 0x52454641 /* 'REFA' (data) */
#define REFFS_DISK_MAGIC_LNK 0x5245464c /* 'REFL' */

#define REFFS_DISK_VERSION_1 1

struct reffs_disk_header {
	uint32_t rdh_magic;
	uint32_t rdh_version;
};

struct reffs_storage_ops {
	enum reffs_storage_type type;
	const char *name;

	/* Superblock operations */
	int (*sb_alloc)(struct super_block *sb, const char *backend_path);
	void (*sb_free)(struct super_block *sb);

	/* Inode operations */
	int (*inode_alloc)(struct inode *inode);
	void (*inode_free)(struct inode *inode);
	void (*inode_sync)(struct inode *inode);

	/* Data block operations */
	int (*db_alloc)(struct data_block *db, struct inode *inode,
			const char *buffer, size_t size, off_t offset);
	void (*db_free)(struct data_block *db);
	void (*db_release_resources)(struct data_block *db);
	ssize_t (*db_read)(struct data_block *db, char *buffer, size_t size,
			   off_t offset);
	ssize_t (*db_write)(struct data_block *db, const char *buffer,
			    size_t size, off_t offset);
	ssize_t (*db_resize)(struct data_block *db, size_t size);
	size_t (*db_get_size)(struct data_block *db);

	/* Directory operations */
	void (*dir_sync)(struct inode *inode);

	/*
	 * Scan the on-disk directory for dir_ino looking for an entry whose
	 * child ino matches child_ino.  On success fills name_out (NUL-
	 * terminated, truncated to name_max-1) and *cookie_out, returns 0.
	 * Returns ENOENT if not found, or an errno on I/O error.
	 *
	 * This is a cold-path helper used by inode_reconstruct_path_to_root()
	 * to recover the name of an inode whose dirent was evicted.
	 */
	int (*dir_find_entry_by_ino)(struct super_block *sb, uint64_t dir_ino,
				     uint64_t child_ino, char *name_out,
				     size_t name_max, uint64_t *cookie_out);

	/*
	 * Scan the on-disk directory for dir_ino looking for an entry whose
	 * name matches name.  On success fills *child_ino_out and
	 * *cookie_out, returns 0.
	 * Returns ENOENT if not found, or an errno on I/O error.
	 *
	 * Used by dirent_load_child_by_name() for LOOKUP misses when the
	 * directory is not fully resident in memory.
	 */
	int (*dir_find_entry_by_name)(struct super_block *sb, uint64_t dir_ino,
				      const char *name, uint64_t *child_ino_out,
				      uint64_t *cookie_out);
};

void reffs_backend_init(void);
const struct reffs_storage_ops *
reffs_backend_get_ops(enum reffs_storage_type type);

#endif /* _REFFS_BACKEND_H */
