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
#include "reffs/test.h"

// Remove once this gets fleshed out
#pragma GCC diagnostic ignored "-Wunused-parameter"

struct name_match {
	struct dirent *nm_dirent;
	char *nm_name;
};

static bool name_is_child(struct name_match *nm, char *name)
{
	bool exists = false;
	struct dirent *de;

	rcu_read_lock();
	cds_list_for_each_entry_rcu(de, &nm->nm_dirent->d_children,
				    d_siblings) {
		if (!strcmp(de->d_name, name)) {
			exists = true;
			dirent_put(nm->nm_dirent);
			nm->nm_dirent = dirent_get(de);
			break;
		}
	}
	rcu_read_unlock();

	return exists;
}

//
// Very chatty
// Need to clean up
//
#define MATCH_LAST_COMPONENT (true)
#define LAST_COMPONENT_IS_NEW (false)
static struct name_match *find_matching_directory_entry(const char *path,
							bool match_end)
{
	struct super_block *sb;
	struct name_match *nm;
	char *str;
	char *buf = NULL;
	char *next = NULL;
	char *last;
	bool exists;

	sb = super_block_find(1);
	if (!sb)
		return NULL;

	nm = calloc(1, sizeof(*nm));
	if (!nm)
		goto found;

	nm->nm_dirent = dirent_get(sb->sb_dirent);
	if (!strcmp(path, "/"))
		goto found;

	nm->nm_name = strrchr(path, '/');
	nm->nm_name++;

	buf = strdup(path);
	next = buf;

	last = strrchr(buf, '/');
	last++;

	do {
		str = strchr(next, '/');
		if (str) {
			str++;
			next = strchr(str, '/');
			if (next) {
				*next++ = '\0';
				assert(*next != '/');
			}
		} else {
			str = next;
			next = NULL;
		}

		if (str)
			exists = name_is_child(nm, str);
		else
			exists = false;

		if (!exists) {
			if (next && !match_end && last == next)
				break;
			else if (!next && !match_end && str == last)
				break;
			else {
				dirent_put(nm->nm_dirent);
				free(nm);
				nm = NULL;
				goto found;
			}
		}
	} while (next);

found:
	free(buf);

	super_block_put(sb);
	return nm;
}

int do_fuse_getattr(const char *path, struct stat *st)
{
	struct name_match *nm;
	struct inode *inode;

	nm = find_matching_directory_entry(path, MATCH_LAST_COMPONENT);
	if (!nm)
		return -ENOENT;

	inode = inode_get(nm->nm_dirent->d_inode);

	st->st_uid = inode->i_uid;
	st->st_gid = inode->i_gid;
	st->st_mtime = inode->i_mtime.tv_sec;
	st->st_atime = inode->i_atime.tv_sec;
	st->st_ctime = inode->i_ctime.tv_sec;
	st->st_mode = inode->i_mode;
	st->st_size = inode->i_size;
	st->st_nlink = inode->i_nlink;

	dirent_put(nm->nm_dirent);
	free(nm);

	inode_put(inode);

	return 0;
}

int do_fuse_readdir(const char *path, void *buffer, fuse_fill_dir_t filler,
		    off_t offset, struct fuse_file_info *fi)
{
	struct name_match *nm;
	struct dirent *de;

	filler(buffer, ".", NULL, 0);
	filler(buffer, "..", NULL, 0);

	nm = find_matching_directory_entry(path, MATCH_LAST_COMPONENT);
	if (!nm)
		return -ENOENT;

	rcu_read_lock();
	cds_list_for_each_entry_rcu(de, &nm->nm_dirent->d_children, d_siblings)
		filler(buffer, de->d_name, NULL, 0);
	rcu_read_unlock();

	dirent_put(nm->nm_dirent);
	free(nm);

	return 0;
}

int do_fuse_read(const char *path, char *buffer, size_t size, off_t offset,
		 struct fuse_file_info *fi)
{
	return 0;
}

int do_fuse_mkdir(const char *path, mode_t mode)
{
	struct name_match *nm;
	struct inode *inode;
	struct super_block *sb;
	struct dirent *de;

	int ret = 0;

	nm = find_matching_directory_entry(path, LAST_COMPONENT_IS_NEW);
	if (!nm)
		return -ENOENT;

	inode = inode_get(nm->nm_dirent->d_inode);
	sb = inode->i_sb;

	de = dirent_alloc(nm->nm_dirent, nm->nm_name);
	if (!de) {
		ret = -ENOENT;
		goto out;
	}

	de->d_inode = inode_alloc(sb, uatomic_add_return(&sb->sb_next_ino, 1));
	if (!de->d_inode) {
		dirent_put(de);
		ret = -ENOENT;
		goto out;
	}

	de->d_inode->i_uid = getuid();
	de->d_inode->i_gid = getgid();
	clock_gettime(CLOCK_REALTIME, &de->d_inode->i_mtime);
	de->d_inode->i_atime = inode->i_atime;
	de->d_inode->i_btime = inode->i_btime;
	de->d_inode->i_ctime = inode->i_ctime;
	de->d_inode->i_mode = S_IFDIR | mode;
	de->d_inode->i_size = 4096;
	de->d_inode->i_used = 8;
	de->d_inode->i_nlink = 2;

out:
	dirent_put(nm->nm_dirent);
	free(nm);

	inode_put(inode);

	return ret;
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
