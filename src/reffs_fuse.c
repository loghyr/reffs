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
#include "reffs.h"
#include "reffs/super_block.h"

struct super_block *root_sb;

// Remove once this gets fleshed out
#pragma GCC diagnostic ignored "-Wunused-parameter"

static int do_fuse_getattr(const char *path, struct stat *st)
{
	return 0;
}

static int do_fuse_readdir(const char *path, void *buffer,
			   fuse_fill_dir_t filler, off_t offset,
			   struct fuse_file_info *fi)
{
	return 0;
}

static int do_fuse_read(const char *path, char *buffer, size_t size,
			off_t offset, struct fuse_file_info *fi)
{
	return 0;
}

static int do_fuse_mkdir(const char *path, mode_t mode)
{
	return 0;
}

static int do_fuse_mknod(const char *path, mode_t mode, dev_t rdev)
{
	return 0;
}

static int do_fuse_write(const char *path, const char *buffer, size_t size,
			 off_t offset, struct fuse_file_info *info)
{
	return size;
}

static struct fuse_operations operations = {
	.getattr = do_fuse_getattr,
	.readdir = do_fuse_readdir,
	.read = do_fuse_read,
	.mkdir = do_fuse_mkdir,
	.mknod = do_fuse_mknod,
	.write = do_fuse_write,
};

int main(int argc, char *argv[])
{
	int ret;

	rcu_register_thread();

	// Perhaps a function to instantiate the root?
	root_sb = super_block_alloc(1);
	if (!root_sb) {
		ret = ENOMEM;
		goto out;
	}

	root_sb->sb_dirent =
		dirent_alloc(NULL, "/"); /* Root is only entry with a "/" */
	if (!root_sb->sb_dirent) {
		ret = ENOMEM;
		goto out_sb;
	}

	root_sb->sb_dirent->d_inode = inode_alloc(root_sb, 1);
	if (!root_sb->sb_dirent->d_inode) {
		ret = ENOMEM;
		goto out_dirent;
	}

	ret = fuse_main(argc, argv, &operations, NULL);

	// And tear it down?
	inode_unhash(root_sb->sb_dirent->d_inode);
	root_sb->sb_dirent->d_inode = NULL;

out_dirent:
	dirent_put(root_sb->sb_dirent);
	root_sb->sb_dirent = NULL;

out_sb:
	super_block_put(root_sb);
	root_sb = NULL;

out:
	rcu_unregister_thread();

	return ret;
}
