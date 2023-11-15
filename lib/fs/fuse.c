/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#define FUSE_USE_VERSION 30

#include <fuse.h>

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <time.h>
#include <string.h>
#include <urcu.h>
#include <urcu/rculist.h>
#include <urcu/ref.h>
#include "reffs/super_block.h"
#include "reffs/dirent.h"
#include "reffs/inode.h"
#include "reffs/time.h"

// Remove once this gets fleshed out
#pragma GCC diagnostic ignored "-Wunused-parameter"

int do_fuse_getattr(const char *path, struct stat *st)
{
	struct super_block *sb;
	struct inode *inode;

	sb = super_block_find(1);
	if (!sb)
		return ENOENT;

	inode = inode_get(sb->sb_dirent->d_inode);

	st->st_uid = inode->i_uid;
	st->st_gid = inode->i_gid;
	st->st_mtime = inode->i_mtime.tv_sec;
	st->st_atime = inode->i_atime.tv_sec;
	st->st_ctime = inode->i_ctime.tv_sec;
	st->st_mode = inode->i_mode;
	st->st_size = inode->i_size;
	st->st_nlink = inode->i_nlink;

	inode_put(inode);
	super_block_put(sb);

	return 0;
}

int do_fuse_readdir(const char *path, void *buffer, fuse_fill_dir_t filler,
		    off_t offset, struct fuse_file_info *fi)
{
	filler(buffer, ".", NULL, 0);
	filler(buffer, "..", NULL, 0);

	return 0;
}

int do_fuse_read(const char *path, char *buffer, size_t size, off_t offset,
		 struct fuse_file_info *fi)
{
	return 0;
}

int do_fuse_mkdir(const char *path, mode_t mode)
{
	return 0;
}

int do_fuse_mknod(const char *path, mode_t mode, dev_t rdev)
{
	return 0;
}

int do_fuse_write(const char *path, const char *buffer, size_t size,
		  off_t offset, struct fuse_file_info *info)
{
	return size;
}
