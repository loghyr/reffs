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
#include <getopt.h>
#include <urcu.h>
#include <urcu/rculist.h>
#include <urcu/ref.h>
#include "reffs/super_block.h"
#include "reffs/inode.h"
#include "reffs/fuse.h"
#include "reffs/log.h"

struct super_block *root_sb;

static struct fuse_operations operations = {
	.access = reffs_fuse_access,
	.chmod = reffs_fuse_chmod,
	.chown = reffs_fuse_chown,
	.fallocate = reffs_fuse_fallocate,
	.getattr = reffs_fuse_getattr,
	.mkdir = reffs_fuse_mkdir,
	.mknod = reffs_fuse_mknod,
	.read = reffs_fuse_read,
	.readdir = reffs_fuse_readdir,
	.readlink = reffs_fuse_readlink,
	.rename = reffs_fuse_rename,
	.rmdir = reffs_fuse_rmdir,
	.symlink = reffs_fuse_symlink,
	.unlink = reffs_fuse_unlink,
	.write = reffs_fuse_write,
};

static void usage(const char *me)
{
	fprintf(stdout, "Usage: %s [options]\n", me);
	fprintf(stdout, "Options:\n");
	fprintf(stdout, " -h  --help         Show help\n");
	fprintf(stdout,
		" -f  --filesystem   Path to root of fuse filesystem\n");
	fprintf(stdout, " -t  --tracing      Enable tracing\n");
}

static struct option options[] = {
	{ "help", no_argument, 0, 'h' },
	{ "filesystem", required_argument, 0, 'f' },
	{ "tracing", no_argument, 0, 't' },
	{ NULL, 0, NULL, 0 },
};

int main(int argc, char *argv[])
{
	int ret;
	struct inode *inode;

	int opt;

	int fuse_argc = 3;
	char *fuse_argv[3];

	fuse_argv[0] = argv[0];
	fuse_argv[1] = "-f";
	fuse_argv[2] = "/tmp/reffs";

	while ((opt = getopt_long(argc, argv, "hf:t", options, NULL)) != -1) {
		switch (opt) {
		case 'f':
			fuse_argv[2] = optarg;
			break;
		case 't':
			reffs_tracing_set(REFFS_TRACE_STATE_ENABLED);
			break;
		case 'h':
			usage(argv[0]);
			exit(1);
		}
	}

	rcu_register_thread();

	// FIXME: This should be init()
	root_sb = super_block_alloc(1);
	if (!root_sb) {
		ret = ENOMEM;
		goto out;
	}

	ret = super_block_dirent_create(root_sb, reffs_life_action_birth);
	if (ret)
		goto out_sb;

	inode = inode_get(root_sb->sb_dirent->d_inode);

	inode->i_uid = getuid();
	inode->i_gid = getgid();
	clock_gettime(CLOCK_REALTIME, &inode->i_mtime);
	inode->i_atime = inode->i_mtime;
	inode->i_btime = inode->i_mtime;
	inode->i_ctime = inode->i_mtime;
	inode->i_mode = S_IFDIR | 0755;
	inode->i_size = 4096;
	inode->i_used = 8;
	inode->i_nlink = 2;

	ret = fuse_main(fuse_argc, fuse_argv, &operations, NULL);

	super_block_dirent_release(root_sb, reffs_life_action_death);
	inode_put(inode);

out_sb:
	// FIXME: This should be destroy()
	super_block_put(root_sb);
	root_sb = NULL;

out:
	synchronize_rcu();
	rcu_barrier();

	rcu_unregister_thread();

	return ret;
}
