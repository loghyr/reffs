/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_FUSE_H
#define _REFFS_FUSE_H

#include <fuse.h>

int do_fuse_getattr(const char *path, struct stat *st);
int do_fuse_readdir(const char *path, void *buffer, fuse_fill_dir_t filler,
		    off_t offset, struct fuse_file_info *fi);
int do_fuse_read(const char *path, char *buffer, size_t size, off_t offset,
		 struct fuse_file_info *fi);
int do_fuse_mkdir(const char *path, mode_t mode);
int do_fuse_mknod(const char *path, mode_t mode, dev_t rdev);
int do_fuse_write(const char *path, const char *buffer, size_t size,
		  off_t offset, struct fuse_file_info *info);

#endif /* _REFFS_FUSE_H */
