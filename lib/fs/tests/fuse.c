/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
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

struct super_block *root_sb;

#define BUFFER_LEN (1024)

int main(void)
{
	int ret;
	struct inode *inode;

	struct stat st_pre;
	struct stat st_post;

	char buffer[BUFFER_LEN];

	int rc;

	rcu_register_thread();

	// Perhaps a function to instantiate the root?
	root_sb = super_block_alloc(1);
	if (!root_sb) {
		ret = ENOMEM;
		goto out;
	}

	ret = super_block_dirent_create(root_sb, reffs_life_action_birth);
	verify(!ret);

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

	ret = reffs_fuse_mkdir("/foo", 0755);
	verify(!ret);
	ret = reffs_fuse_mkdir("/foo/bar", 0640);
	verify(!ret);
	ret = reffs_fuse_mkdir("/foo/garbo", 0640);
	verify(!ret);
	ret = reffs_fuse_mkdir("/foo/nurse", 0640);
	verify(!ret);
	ret = reffs_fuse_mkdir("/hello", 0755);
	verify(!ret);
	ret = reffs_fuse_mkdir("/hello/nurse", 0755);
	verify(!ret);

	ret = reffs_fuse_getattr("/foo/bar", &st_pre);
	verify(!ret);
	verify(st_pre.st_uid == inode->i_uid);
	verify(st_pre.st_gid == inode->i_gid);
	verify(st_pre.st_size == inode->i_size);

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

	ret = reffs_fuse_mknod("/foo/bar/nurse", S_IFDIR | 0755, 0);
	verify(ret == -EISDIR);

	ret = reffs_fuse_mknod("/foo/bar/nurse", S_IFREG | 0755, 0);
	verify(!ret);

	ret = reffs_fuse_getattr("/foo/bar/nurse", &st_pre);
	verify(!ret);
	verify(st_pre.st_uid == inode->i_uid);
	verify(st_pre.st_gid == inode->i_gid);
	verify(st_pre.st_size == 0);
	verify(st_pre.st_mode == (S_IFREG | 0755));

	ret = reffs_fuse_write("/foo/bar", "hello", 5, 0, NULL);
	rc = errno;
	verify(ret == -1);
	verify(rc == EISDIR);

	ret = reffs_fuse_write("/foo/bar/nurse", "hello", 5, 0, NULL);
	rc = errno;
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

	bzero(buffer, BUFFER_LEN);
	ret = reffs_fuse_read("/foo/bar/nurse", buffer, 5, 0, NULL);
	rc = errno;
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

	bzero(buffer, BUFFER_LEN);
	ret = reffs_fuse_read("/foo/bar/nurse", buffer, 5, 10, NULL);
	rc = errno;
	verify(ret == -1);
	verify(rc == EOVERFLOW);

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

	ret = reffs_fuse_write("/foo/bar/nurse", "hello", 5, 5, NULL);
	rc = errno;
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

	bzero(buffer, BUFFER_LEN);
	ret = reffs_fuse_read("/foo/bar/nurse", buffer, 5, 5, NULL);
	rc = errno;
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

	bzero(buffer, BUFFER_LEN);
	ret = reffs_fuse_read("/foo/bar/nurse", buffer, 10, 0, NULL);
	rc = errno;
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

	super_block_dirent_release(root_sb, reffs_life_action_death);
	inode_put(inode);

	super_block_put(root_sb);
	root_sb = NULL;

out:
	rcu_unregister_thread();

	return ret;
}
