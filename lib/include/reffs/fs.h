/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_FS_H
#define _REFFS_FS_H

#include <sys/stat.h>
#include "reffs/types.h"
#include "reffs/super_block.h"

struct reffs_dirent;
struct name_match {
	struct reffs_dirent *nm_dirent;
	char *nm_name;
};

#define LAST_COMPONENT_IS_MATCH (true)
#define LAST_COMPONENT_IS_NEW (false)
int find_matching_directory_entry(struct name_match **nm, const char *path,
				  bool match_end);

void reffs_fs_set_storage(enum reffs_storage_type type, const char *path);
enum reffs_storage_type reffs_fs_get_storage_type(void);
char *reffs_fs_get_backend_path(void);

void reffs_fs_recover(struct super_block *sb);
void reffs_fs_for_each_inode(int (*cb)(struct inode *, void *), void *arg);

struct reffs_fs_usage_stats {
	uint64_t total_bytes;
	uint64_t used_bytes;
	uint64_t free_bytes;
	uint64_t total_files;
	uint64_t used_files;
	uint64_t free_files;
};

int reffs_fs_usage(struct reffs_fs_usage_stats *stats);

int reffs_fs_access(const char *path, int mode, uid_t uid, gid_t gid);
int reffs_fs_chmod(const char *path, mode_t mode);
int reffs_fs_chown(const char *path, uid_t uid, gid_t gid);
int reffs_fs_create(const char *path, mode_t mode);
int reffs_fs_fallocate(const char *path, int mode, off_t offset, off_t len);
int reffs_fs_getattr(const char *path, struct stat *st);
int reffs_fs_mkdir(const char *path, mode_t mode);
int reffs_fs_mknod(const char *path, mode_t mode, dev_t rdev);
int reffs_fs_read(const char *path, char *buffer, size_t size, off_t offset);
int reffs_fs_readdir(const char *path, void *buffer, char *filler,
		     off_t offset);
int reffs_fs_readlink(const char *path, char *buffer, size_t len);
int reffs_fs_rename(const char *path, const char *new_path);
int reffs_fs_rmdir(const char *path);
int reffs_fs_symlink(const char *path, const char *new_path);
int reffs_fs_unlink(const char *path);
int reffs_fs_utimensat(const char *path, const struct timespec times[2]);
int reffs_fs_write(const char *path, const char *buffer, size_t size,
		   off_t offset);

#endif /* _REFFS_FS_H */
