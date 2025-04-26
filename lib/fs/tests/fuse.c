/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_JEMALLOC
#include <jemalloc/jemalloc.h>
#endif

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
#include "reffs/test.h"
#include "reffs/log.h"
#include "reffs/fs.h"
#include "reffs/ns.h"

#define BUFFER_LEN (1024)

int main(void)
{
	int ret;
	struct super_block *sb = NULL;
	struct inode *inode = NULL;

	struct stat st_pre;
	struct stat st_src_pre;
	struct stat st_dst_pre;
	struct stat st_post;

	char buffer[BUFFER_LEN];

	rcu_register_thread();

	reffs_tracing_set(REFFS_TRACE_LEVEL_DEBUG);

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

	ret = reffs_fuse_getattr("/", &st_pre);
	verify(!ret);
	verify(st_pre.st_nlink == 2);

	ret = reffs_fuse_mkdir("/foo", 0755);
	verify(!ret);
	ret = reffs_fuse_getattr("/foo", &st_post);
	verify(!ret);
	verify(st_post.st_nlink == 2);

	ret = reffs_fuse_getattr("/", &st_post);
	verify(!ret);
	verify(st_pre.st_nlink == st_post.st_nlink - 1);
	st_pre = st_post;

	ret = reffs_fuse_mkdir("/foo/bar", 0640);
	verify(!ret);
	ret = reffs_fuse_getattr("/foo", &st_pre);
	verify(!ret);
	verify(st_pre.st_nlink == 3);

	ret = reffs_fuse_mkdir("/foo/garbo", 0640);
	verify(!ret);
	ret = reffs_fuse_getattr("/foo", &st_pre);
	verify(!ret);
	verify(st_pre.st_nlink == 4);

	ret = reffs_fuse_mkdir("/foo/nurse", 0640);
	verify(!ret);
	ret = reffs_fuse_getattr("/foo", &st_pre);
	verify(!ret);
	verify(st_pre.st_nlink == 5);

	ret = reffs_fuse_mkdir("/hello", 0755);
	verify(!ret);
	ret = reffs_fuse_getattr("/hello", &st_pre);
	verify(!ret);
	verify(st_pre.st_nlink == 2);

	ret = reffs_fuse_getattr("/", &st_pre);
	verify(!ret);
	verify(st_pre.st_nlink == 4);

	ret = reffs_fuse_mkdir("/hello/nurse", 0755);
	verify(!ret);
	ret = reffs_fuse_getattr("/hello", &st_pre);
	verify(!ret);
	verify(st_pre.st_nlink == 3);

	ret = reffs_fuse_getattr("/foo/bar", &st_pre);
	verify(!ret);
	verify(st_pre.st_uid == inode->i_uid);
	verify(st_pre.st_gid == inode->i_gid);
	verify(st_pre.st_size == inode->i_size);

	usleep(1000);

	ret = reffs_fuse_mkdir("/foo/bar/garbo", 0640);
	verify(!ret);
	ret = reffs_fuse_getattr("/foo/bar", &st_post);
	verify(!ret);

	verify(st_pre.st_ino == st_post.st_ino);
	verify(st_pre.st_uid == st_post.st_uid);
	verify(st_pre.st_gid == st_post.st_gid);
	verify(st_pre.st_size == st_post.st_size);
	verify(st_pre.st_nlink == st_post.st_nlink - 1);
	verify(st_pre.st_atim.tv_sec == st_post.st_atim.tv_sec);
	verify(st_pre.st_atim.tv_nsec == st_post.st_atim.tv_nsec);
	verify((st_pre.st_mtim.tv_sec < st_post.st_mtim.tv_sec) ||
	       ((st_pre.st_mtim.tv_sec == st_post.st_mtim.tv_sec) &&
		((st_pre.st_mtim.tv_nsec < st_post.st_mtim.tv_nsec))));
	verify((st_pre.st_ctim.tv_sec < st_post.st_ctim.tv_sec) ||
	       ((st_pre.st_ctim.tv_sec == st_post.st_ctim.tv_sec) &&
		((st_pre.st_ctim.tv_nsec < st_post.st_ctim.tv_nsec))));

	ret = reffs_fuse_rmdir("/");
	verify(ret == -EBUSY);

	ret = reffs_fuse_rmdir("/foo/bar");
	verify(ret == -ENOTEMPTY);

	ret = reffs_fuse_create("/foo/bar/nurse", S_IFDIR | 0755, NULL);
	verify(ret == -EISDIR);

	ret = reffs_fuse_create("/foo/bar/nurse", S_IFREG | 0755, NULL);
	verify(!ret);

	ret = reffs_fuse_getattr("/foo/bar/nurse", &st_pre);
	verify(!ret);
	verify(st_pre.st_uid == inode->i_uid);
	verify(st_pre.st_gid == inode->i_gid);
	verify(st_pre.st_size == 0);
	verify(st_pre.st_mode == (S_IFREG | 0755));

	usleep(1000);

	ret = reffs_fuse_write("/foo/bar", "hello", 5, 0, NULL);
	verify(ret == -EISDIR);

	ret = reffs_fuse_write("/foo/bar/nurse", "hello", 5, 0, NULL);
	verify(ret == 5);

	ret = reffs_fuse_getattr("/foo/bar/nurse", &st_post);
	verify(!ret);
	verify(st_pre.st_ino == st_post.st_ino);
	verify(st_pre.st_uid == st_post.st_uid);
	verify(st_pre.st_gid == st_post.st_gid);
	verify(st_post.st_size == 5);
	verify(st_pre.st_atim.tv_sec == st_post.st_atim.tv_sec);
	verify(st_pre.st_atim.tv_nsec == st_post.st_atim.tv_nsec);
	verify((st_pre.st_mtim.tv_sec < st_post.st_mtim.tv_sec) ||
	       ((st_pre.st_mtim.tv_sec == st_post.st_mtim.tv_sec) &&
		((st_pre.st_mtim.tv_nsec < st_post.st_mtim.tv_nsec))));
	verify((st_pre.st_ctim.tv_sec < st_post.st_ctim.tv_sec) ||
	       ((st_pre.st_ctim.tv_sec == st_post.st_ctim.tv_sec) &&
		((st_pre.st_ctim.tv_nsec < st_post.st_ctim.tv_nsec))));

	st_pre = st_post;
	usleep(1000);

	bzero(buffer, BUFFER_LEN);
	ret = reffs_fuse_read("/foo/bar/nurse", buffer, 5, 0, NULL);
	verify(ret == 5);

	verify(!strcmp(buffer, "hello"));

	ret = reffs_fuse_getattr("/foo/bar/nurse", &st_post);
	verify(!ret);
	verify(st_pre.st_ino == st_post.st_ino);
	verify(st_pre.st_uid == st_post.st_uid);
	verify(st_pre.st_gid == st_post.st_gid);
	verify(st_post.st_size == 5);
	verify(st_pre.st_mtim.tv_sec == st_post.st_mtim.tv_sec);
	verify(st_pre.st_mtim.tv_nsec == st_post.st_mtim.tv_nsec);
	verify(st_pre.st_ctim.tv_sec == st_post.st_ctim.tv_sec);
	verify(st_pre.st_ctim.tv_nsec == st_post.st_ctim.tv_nsec);
	verify((st_pre.st_atim.tv_sec < st_post.st_atim.tv_sec) ||
	       ((st_pre.st_atim.tv_sec == st_post.st_atim.tv_sec) &&
		((st_pre.st_atim.tv_nsec < st_post.st_atim.tv_nsec))));

	st_pre = st_post;
	usleep(1000);

	bzero(buffer, BUFFER_LEN);
	ret = reffs_fuse_read("/foo/bar/nurse", buffer, 5, 10, NULL);
	verify(ret == -EOVERFLOW);

	ret = reffs_fuse_getattr("/foo/bar/nurse", &st_post);
	verify(!ret);
	verify(st_pre.st_ino == st_post.st_ino);
	verify(st_pre.st_uid == st_post.st_uid);
	verify(st_pre.st_gid == st_post.st_gid);
	verify(st_post.st_size == 5);
	verify(st_pre.st_mtim.tv_sec == st_post.st_mtim.tv_sec);
	verify(st_pre.st_mtim.tv_nsec == st_post.st_mtim.tv_nsec);
	verify(st_pre.st_ctim.tv_sec == st_post.st_ctim.tv_sec);
	verify(st_pre.st_ctim.tv_nsec == st_post.st_ctim.tv_nsec);
	verify((st_pre.st_atim.tv_sec < st_post.st_atim.tv_sec) ||
	       ((st_pre.st_atim.tv_sec == st_post.st_atim.tv_sec) &&
		((st_pre.st_atim.tv_nsec < st_post.st_atim.tv_nsec))));

	st_pre = st_post;
	usleep(1000);

	ret = reffs_fuse_write("/foo/bar/nurse", "hello", 5, 5, NULL);
	verify(ret == 5);

	ret = reffs_fuse_getattr("/foo/bar/nurse", &st_post);
	verify(!ret);
	verify(st_pre.st_ino == st_post.st_ino);
	verify(st_pre.st_uid == st_post.st_uid);
	verify(st_pre.st_gid == st_post.st_gid);
	verify(st_post.st_size == 10);
	verify(st_pre.st_atim.tv_sec == st_post.st_atim.tv_sec);
	verify(st_pre.st_atim.tv_nsec == st_post.st_atim.tv_nsec);
	verify((st_pre.st_mtim.tv_sec < st_post.st_mtim.tv_sec) ||
	       ((st_pre.st_mtim.tv_sec == st_post.st_mtim.tv_sec) &&
		((st_pre.st_mtim.tv_nsec < st_post.st_mtim.tv_nsec))));
	verify((st_pre.st_ctim.tv_sec < st_post.st_ctim.tv_sec) ||
	       ((st_pre.st_ctim.tv_sec == st_post.st_ctim.tv_sec) &&
		((st_pre.st_ctim.tv_nsec < st_post.st_ctim.tv_nsec))));

	st_pre = st_post;
	usleep(1000);

	bzero(buffer, BUFFER_LEN);
	ret = reffs_fuse_read("/foo/bar/nurse", buffer, 5, 5, NULL);
	verify(ret == 5);

	verify(!strcmp(buffer, "hello"));

	ret = reffs_fuse_getattr("/foo/bar/nurse", &st_post);
	verify(!ret);
	verify(st_pre.st_ino == st_post.st_ino);
	verify(st_pre.st_uid == st_post.st_uid);
	verify(st_pre.st_gid == st_post.st_gid);
	verify(st_post.st_size == 10);
	verify(st_pre.st_mtim.tv_sec == st_post.st_mtim.tv_sec);
	verify(st_pre.st_mtim.tv_nsec == st_post.st_mtim.tv_nsec);
	verify(st_pre.st_ctim.tv_sec == st_post.st_ctim.tv_sec);
	verify(st_pre.st_ctim.tv_nsec == st_post.st_ctim.tv_nsec);
	verify((st_pre.st_atim.tv_sec < st_post.st_atim.tv_sec) ||
	       ((st_pre.st_atim.tv_sec == st_post.st_atim.tv_sec) &&
		((st_pre.st_atim.tv_nsec < st_post.st_atim.tv_nsec))));

	st_pre = st_post;
	usleep(1000);

	bzero(buffer, BUFFER_LEN);
	ret = reffs_fuse_read("/foo/bar/nurse", buffer, 10, 0, NULL);
	verify(ret == 10);

	verify(!strcmp(buffer, "hellohello"));

	ret = reffs_fuse_getattr("/foo/bar/nurse", &st_post);
	verify(!ret);
	verify(st_pre.st_ino == st_post.st_ino);
	verify(st_pre.st_uid == st_post.st_uid);
	verify(st_pre.st_gid == st_post.st_gid);
	verify(st_post.st_size == 10);
	verify(st_pre.st_mtim.tv_sec == st_post.st_mtim.tv_sec);
	verify(st_pre.st_mtim.tv_nsec == st_post.st_mtim.tv_nsec);
	verify(st_pre.st_ctim.tv_sec == st_post.st_ctim.tv_sec);
	verify(st_pre.st_ctim.tv_nsec == st_post.st_ctim.tv_nsec);
	verify((st_pre.st_atim.tv_sec < st_post.st_atim.tv_sec) ||
	       ((st_pre.st_atim.tv_sec == st_post.st_atim.tv_sec) &&
		((st_pre.st_atim.tv_nsec < st_post.st_atim.tv_nsec))));
	usleep(1000);

	ret = reffs_fuse_getattr("/foo/bar", &st_pre);
	verify(!ret);

	ret = reffs_fuse_unlink("/foo/bar");
	verify(ret == -EISDIR);

	ret = reffs_fuse_unlink("/foo/bar/nurse");
	verify(!ret);

	ret = reffs_fuse_getattr("/foo/bar/nurse", &st_post);
	verify(ret == -ENOENT);

	ret = reffs_fuse_getattr("/foo/bar", &st_post);
	verify(!ret);

	verify(st_pre.st_ino == st_post.st_ino);
	verify(st_pre.st_uid == st_post.st_uid);
	verify(st_pre.st_gid == st_post.st_gid);
	verify(st_pre.st_size == st_post.st_size);
	verify(st_pre.st_nlink == st_post.st_nlink + 1);
	verify(st_pre.st_atim.tv_sec == st_post.st_atim.tv_sec);
	verify(st_pre.st_atim.tv_nsec == st_post.st_atim.tv_nsec);
	verify((st_pre.st_mtim.tv_sec < st_post.st_mtim.tv_sec) ||
	       ((st_pre.st_mtim.tv_sec == st_post.st_mtim.tv_sec) &&
		((st_pre.st_mtim.tv_nsec < st_post.st_mtim.tv_nsec))));
	verify((st_pre.st_ctim.tv_sec < st_post.st_ctim.tv_sec) ||
	       ((st_pre.st_ctim.tv_sec == st_post.st_ctim.tv_sec) &&
		((st_pre.st_ctim.tv_nsec < st_post.st_ctim.tv_nsec))));

	ret = reffs_fuse_rmdir("/foo/bar");
	verify(ret == -ENOTEMPTY);

	ret = reffs_fuse_getattr("/foo/bar", &st_post);
	verify(!ret);

	ret = reffs_fuse_rmdir("/foo/bar/garbo");
	verify(!ret);
	ret = reffs_fuse_rmdir("/foo/bar");
	verify(!ret);
	ret = reffs_fuse_rmdir("/foo/garbo");
	verify(!ret);
	ret = reffs_fuse_rmdir("/foo/nurse");
	verify(!ret);
	ret = reffs_fuse_rmdir("/foo");
	verify(!ret);
	ret = reffs_fuse_rmdir("/hello/nurse");
	verify(!ret);
	ret = reffs_fuse_rmdir("/hello");
	verify(!ret);

	ret = reffs_fuse_getattr("/", &st_pre);
	verify(!ret);
	verify(st_pre.st_nlink == 2);

	ret = reffs_fuse_mkdir("/foo", 0755);
	verify(!ret);

	ret = reffs_fuse_getattr("/", &st_post);
	verify(!ret);
	verify(st_pre.st_nlink == st_post.st_nlink - 1);

	ret = reffs_fuse_getattr("/foo", &st_pre);
	verify(!ret);

	ret = reffs_fuse_create("/nurse", S_IFREG | 0755, NULL);
	verify(!ret);

	ret = reffs_fuse_getattr("/nurse", &st_pre);
	verify(!ret);
	verify(st_pre.st_uid == inode->i_uid);
	verify(st_pre.st_gid == inode->i_gid);
	verify(st_pre.st_size == 0);
	verify(st_pre.st_mode == (S_IFREG | 0755));

	usleep(1000);

	ret = reffs_fuse_write("/nurse", "hello", 5, 0, NULL);
	verify(ret == 5);

	ret = reffs_fuse_getattr("/nurse", &st_post);
	verify(!ret);
	verify(st_pre.st_ino == st_post.st_ino);
	verify(st_pre.st_uid == st_post.st_uid);
	verify(st_pre.st_gid == st_post.st_gid);
	verify(st_post.st_size == 5);
	verify(st_pre.st_atim.tv_sec == st_post.st_atim.tv_sec);
	verify(st_pre.st_atim.tv_nsec == st_post.st_atim.tv_nsec);
	verify((st_pre.st_mtim.tv_sec < st_post.st_mtim.tv_sec) ||
	       ((st_pre.st_mtim.tv_sec == st_post.st_mtim.tv_sec) &&
		((st_pre.st_mtim.tv_nsec < st_post.st_mtim.tv_nsec))));
	verify((st_pre.st_ctim.tv_sec < st_post.st_ctim.tv_sec) ||
	       ((st_pre.st_ctim.tv_sec == st_post.st_ctim.tv_sec) &&
		((st_pre.st_ctim.tv_nsec < st_post.st_ctim.tv_nsec))));

	st_pre = st_post;

	ret = reffs_fuse_getattr("/", &st_src_pre);
	verify(!ret);

	ret = reffs_fuse_getattr("/foo", &st_dst_pre);
	verify(!ret);

	usleep(1000);

	ret = reffs_fuse_rename("/nurse", "/foo/nurse");
	verify(!ret);

	ret = reffs_fuse_getattr("/nurse", &st_post);
	verify(ret == -ENOENT);

	ret = reffs_fuse_getattr("/foo/nurse", &st_post);
	verify(!ret);
	verify(st_pre.st_ino == st_post.st_ino);
	verify(st_pre.st_uid == st_post.st_uid);
	verify(st_pre.st_gid == st_post.st_gid);
	verify(st_post.st_size == 5);
	verify(st_pre.st_atim.tv_sec == st_post.st_atim.tv_sec);
	verify(st_pre.st_atim.tv_nsec == st_post.st_atim.tv_nsec);
	verify((st_pre.st_mtim.tv_sec == st_post.st_mtim.tv_sec) &&
	       (st_pre.st_mtim.tv_nsec == st_post.st_mtim.tv_nsec));
	verify((st_pre.st_ctim.tv_sec == st_post.st_ctim.tv_sec) &&
	       (st_pre.st_ctim.tv_nsec == st_post.st_ctim.tv_nsec));
	st_pre = st_post;

	ret = reffs_fuse_getattr("/", &st_post);
	verify(!ret);
	verify(st_src_pre.st_ino == st_post.st_ino);
	verify(st_src_pre.st_uid == st_post.st_uid);
	verify(st_src_pre.st_gid == st_post.st_gid);
	verify(st_src_pre.st_nlink == st_post.st_nlink + 1);
	verify(st_src_pre.st_atim.tv_sec == st_post.st_atim.tv_sec);
	verify(st_src_pre.st_atim.tv_nsec == st_post.st_atim.tv_nsec);
	verify((st_src_pre.st_mtim.tv_sec < st_post.st_mtim.tv_sec) ||
	       ((st_src_pre.st_mtim.tv_sec == st_post.st_mtim.tv_sec) &&
		((st_src_pre.st_mtim.tv_nsec < st_post.st_mtim.tv_nsec))));
	verify((st_src_pre.st_ctim.tv_sec < st_post.st_ctim.tv_sec) ||
	       ((st_src_pre.st_ctim.tv_sec == st_post.st_ctim.tv_sec) &&
		((st_src_pre.st_ctim.tv_nsec < st_post.st_ctim.tv_nsec))));
	st_src_pre = st_post;

	ret = reffs_fuse_getattr("/foo", &st_post);
	verify(!ret);
	verify(st_dst_pre.st_ino == st_post.st_ino);
	verify(st_dst_pre.st_uid == st_post.st_uid);
	verify(st_dst_pre.st_gid == st_post.st_gid);
	verify(st_dst_pre.st_nlink == st_post.st_nlink - 1);
	verify(st_dst_pre.st_atim.tv_sec == st_post.st_atim.tv_sec);
	verify(st_dst_pre.st_atim.tv_nsec == st_post.st_atim.tv_nsec);
	verify((st_dst_pre.st_mtim.tv_sec < st_post.st_mtim.tv_sec) ||
	       ((st_dst_pre.st_mtim.tv_sec == st_post.st_mtim.tv_sec) &&
		((st_dst_pre.st_mtim.tv_nsec < st_post.st_mtim.tv_nsec))));
	verify((st_dst_pre.st_ctim.tv_sec < st_post.st_ctim.tv_sec) ||
	       ((st_dst_pre.st_ctim.tv_sec == st_post.st_ctim.tv_sec) &&
		((st_dst_pre.st_ctim.tv_nsec < st_post.st_ctim.tv_nsec))));
	st_dst_pre = st_post;

	usleep(1000);

	ret = reffs_fuse_rename("/foo/nurse", "/nurse");
	verify(!ret);

	ret = reffs_fuse_getattr("/foo/nurse", &st_post);
	verify(ret == -ENOENT);

	ret = reffs_fuse_getattr("/nurse", &st_post);
	verify(!ret);
	verify(st_pre.st_ino == st_post.st_ino);
	verify(st_pre.st_uid == st_post.st_uid);
	verify(st_pre.st_gid == st_post.st_gid);
	verify(st_post.st_size == 5);
	verify(st_pre.st_atim.tv_sec == st_post.st_atim.tv_sec);
	verify(st_pre.st_atim.tv_nsec == st_post.st_atim.tv_nsec);
	verify((st_pre.st_mtim.tv_sec == st_post.st_mtim.tv_sec) &&
	       (st_pre.st_mtim.tv_nsec == st_post.st_mtim.tv_nsec));
	verify((st_pre.st_ctim.tv_sec == st_post.st_ctim.tv_sec) &&
	       (st_pre.st_ctim.tv_nsec == st_post.st_ctim.tv_nsec));
	st_pre = st_post;

	ret = reffs_fuse_getattr("/", &st_post);
	verify(!ret);
	verify(st_src_pre.st_ino == st_post.st_ino);
	verify(st_src_pre.st_uid == st_post.st_uid);
	verify(st_src_pre.st_gid == st_post.st_gid);
	verify(st_src_pre.st_nlink == st_post.st_nlink - 1);
	verify(st_src_pre.st_atim.tv_sec == st_post.st_atim.tv_sec);
	verify(st_src_pre.st_atim.tv_nsec == st_post.st_atim.tv_nsec);
	verify((st_src_pre.st_mtim.tv_sec < st_post.st_mtim.tv_sec) ||
	       ((st_src_pre.st_mtim.tv_sec == st_post.st_mtim.tv_sec) &&
		((st_src_pre.st_mtim.tv_nsec < st_post.st_mtim.tv_nsec))));
	verify((st_src_pre.st_ctim.tv_sec < st_post.st_ctim.tv_sec) ||
	       ((st_src_pre.st_ctim.tv_sec == st_post.st_ctim.tv_sec) &&
		((st_src_pre.st_ctim.tv_nsec < st_post.st_ctim.tv_nsec))));
	st_src_pre = st_post;

	ret = reffs_fuse_getattr("/foo", &st_post);
	verify(!ret);
	verify(st_dst_pre.st_ino == st_post.st_ino);
	verify(st_dst_pre.st_uid == st_post.st_uid);
	verify(st_dst_pre.st_gid == st_post.st_gid);
	verify(st_dst_pre.st_nlink == st_post.st_nlink + 1);
	verify(st_dst_pre.st_atim.tv_sec == st_post.st_atim.tv_sec);
	verify(st_dst_pre.st_atim.tv_nsec == st_post.st_atim.tv_nsec);
	verify((st_dst_pre.st_mtim.tv_sec < st_post.st_mtim.tv_sec) ||
	       ((st_dst_pre.st_mtim.tv_sec == st_post.st_mtim.tv_sec) &&
		((st_dst_pre.st_mtim.tv_nsec < st_post.st_mtim.tv_nsec))));
	verify((st_dst_pre.st_ctim.tv_sec < st_post.st_ctim.tv_sec) ||
	       ((st_dst_pre.st_ctim.tv_sec == st_post.st_ctim.tv_sec) &&
		((st_dst_pre.st_ctim.tv_nsec < st_post.st_ctim.tv_nsec))));
	st_dst_pre = st_post;

	usleep(1000);

	ret = reffs_fuse_unlink("/nurse");
	verify(!ret);

	ret = reffs_fuse_rmdir("/foo");
	verify(!ret);

	ret = reffs_fuse_getattr("/", &st_post);
	verify(!ret);
	verify(st_src_pre.st_ino == st_post.st_ino);
	verify(st_src_pre.st_uid == st_post.st_uid);
	verify(st_src_pre.st_gid == st_post.st_gid);
	verify(st_post.st_nlink == 2);
	verify(st_src_pre.st_nlink == st_post.st_nlink + 2);
	verify(st_src_pre.st_atim.tv_sec == st_post.st_atim.tv_sec);
	verify(st_src_pre.st_atim.tv_nsec == st_post.st_atim.tv_nsec);
	verify((st_src_pre.st_mtim.tv_sec < st_post.st_mtim.tv_sec) ||
	       ((st_src_pre.st_mtim.tv_sec == st_post.st_mtim.tv_sec) &&
		((st_src_pre.st_mtim.tv_nsec < st_post.st_mtim.tv_nsec))));
	verify((st_src_pre.st_ctim.tv_sec < st_post.st_ctim.tv_sec) ||
	       ((st_src_pre.st_ctim.tv_sec == st_post.st_ctim.tv_sec) &&
		((st_src_pre.st_ctim.tv_nsec < st_post.st_ctim.tv_nsec))));

out:
	synchronize_rcu();
	rcu_barrier();

	reffs_ns_fini();

	rcu_unregister_thread();

	return ret;
}
