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
#include "reffs/inode.h"
#include "reffs/fuse.h"

struct super_block *root_sb;

static struct fuse_operations operations = {
	.getattr = reffs_fuse_getattr,
	.readdir = reffs_fuse_readdir,
	// .read = reffs_fuse_read,
	.mkdir = reffs_fuse_mkdir,
	// .mknod = reffs_fuse_mknod,
	.rmdir = reffs_fuse_rmdir,
	// .write = reffs_fuse_write,
};

int main(int argc, char *argv[])
{
	int ret;
	struct inode *inode;

	rcu_register_thread();

	// Perhaps a function to instantiate the root?
	root_sb = super_block_alloc(1);
	if (!root_sb) {
		ret = ENOMEM;
		goto out;
	}

	ret = super_block_dirent_create(root_sb);
	if (ret)
		goto out_sb;

	inode = inode_get(root_sb->sb_dirent->d_inode);

	inode->i_uid = getuid();
	inode->i_gid = getgid();
	clock_gettime(CLOCK_REALTIME, &inode->i_mtime);
	inode->i_atime = inode->i_atime;
	inode->i_btime = inode->i_btime;
	inode->i_ctime = inode->i_ctime;
	inode->i_mode = S_IFDIR | 0755;
	inode->i_size = 4096;
	inode->i_used = 8;
	inode->i_nlink = 2;

	reffs_fuse_mkdir("/foo", 0755);
	reffs_fuse_mkdir("/foo/bar", 0640);
	reffs_fuse_mkdir("/foo/garbo", 0640);
	reffs_fuse_mkdir("/foo/nurse", 0640);
	reffs_fuse_mkdir("/hello", 0755);
	reffs_fuse_mkdir("/hello/nurse", 0755);

	ret = fuse_main(argc, argv, &operations, NULL);

	reffs_fuse_rmdir("/foo/bar");
	reffs_fuse_rmdir("/foo/garbo");
	reffs_fuse_rmdir("/foo/nurse");
	reffs_fuse_rmdir("/foo");
	reffs_fuse_rmdir("/hello/nurse");
	reffs_fuse_rmdir("/hello");

	super_block_dirent_release(root_sb);
	inode_put(inode);

out_sb:
	super_block_put(root_sb);
	root_sb = NULL;

out:
	rcu_unregister_thread();

	return ret;
}
