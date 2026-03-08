/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
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
};

void reffs_backend_init(void);
const struct reffs_storage_ops *
reffs_backend_get_ops(enum reffs_storage_type type);

#endif /* _REFFS_BACKEND_H */
