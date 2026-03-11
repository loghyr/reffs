/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
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
#include "reffs/fs.h"
#include "reffs/ns.h"
#include "reffs/trace/common.h"

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
	.link = reffs_fuse_link,
	.truncate = reffs_fuse_truncate,
	.open = reffs_fuse_open,
	.flush = reffs_fuse_flush,
	.create = reffs_fuse_create,
	.release = reffs_fuse_release,
	.fsync = reffs_fuse_fsync,
	.statfs = reffs_fuse_statfs,
	.utimens = reffs_fuse_utimensat,
};

static void usage(const char *me)
{
	fprintf(stdout, "Usage: %s [options]\n", me);
	fprintf(stdout, "Options:\n");
	fprintf(stdout, " -h  --help         Show help\n");
	fprintf(stdout,
		" -f  --filesystem   Path to root of fuse filesystem\n");
}

static struct option options[] = {
	{ "help", no_argument, 0, 'h' },
	{ "filesystem", required_argument, 0, 'f' },
	{ NULL, 0, NULL, 0 },
};

int main(int argc, char *argv[])
{
	int ret;
	struct inode *inode;
	struct super_block *sb;

	int opt;

	int fuse_argc = 3;
	char *fuse_argv[3];

	fuse_argv[0] = argv[0];
	fuse_argv[1] = "-f";
	fuse_argv[2] = "/tmp/reffs";

	while ((opt = getopt_long(argc, argv, "hf:t:", options, NULL)) != -1) {
		switch (opt) {
		case 'f':
			fuse_argv[2] = optarg;
			break;
		case 'h':
			usage(argv[0]);
			exit(1);
		}
	}

	rcu_register_thread();

	ret = reffs_ns_init();
	if (ret)
		goto out;

	sb = super_block_find(1);
	if (!sb)
		goto out;

	inode = inode_find(sb, 1);
	if (!inode) {
		super_block_put(sb);
		goto out;
	}

	inode->i_uid = getuid();
	inode->i_gid = getgid();

	inode_put(inode);
	super_block_put(sb);

	ret = fuse_main(fuse_argc, fuse_argv, &operations, NULL);

out:
	synchronize_rcu();
	rcu_barrier();

	reffs_ns_fini();

	rcu_unregister_thread();

	return ret;
}
