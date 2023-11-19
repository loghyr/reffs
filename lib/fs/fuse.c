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
#include "reffs/data_block.h"
#include "reffs/super_block.h"
#include "reffs/dirent.h"
#include "reffs/inode.h"
#include "reffs/time.h"
#include "reffs/test.h"

// Remove once this gets fleshed out
#pragma GCC diagnostic ignored "-Wunused-parameter"

enum reffs_text_case fuse_rtc = reffs_text_case_sensitive;

struct name_match {
	struct dirent *nm_dirent;
	char *nm_name;
};

static bool name_is_child(struct name_match *nm, char *name)
{
	bool exists = false;
	struct dirent *de;

	reffs_strng_compare cmp;

	// In case we refactor
	if (fuse_rtc == reffs_text_case_insensitive)
		cmp = strcasecmp;
	else
		cmp = strcmp;

	rcu_read_lock();
	cds_list_for_each_entry_rcu(de, &nm->nm_dirent->d_children,
				    d_siblings) {
		if (!cmp(de->d_name, name)) {
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
#define LAST_COMPONENT_IS_MATCH (true)
#define LAST_COMPONENT_IS_NEW (false)
int find_matching_directory_entry(struct name_match **nm, const char *path,
				  bool match_end)
{
	struct super_block *sb;
	struct name_match *new;
	char *str;
	char *buf = NULL;
	char *next = NULL;
	char *last;
	bool exists;
	int ret = 0;

	sb = super_block_find(1);
	if (!sb)
		return -ENODEV;

	new = calloc(1, sizeof(*new));
	if (!new) {
		ret = -ENOENT;
		goto found;
	}

	new->nm_dirent = dirent_get(sb->sb_dirent);
	if (!strcmp(path, "/")) {
		ret = -ENOENT;
		goto found;
	}

	new->nm_name = strrchr(path, '/');
	new->nm_name++;

	buf = strdup(path);
	next = buf;

	last = strrchr(buf, '/');
	last++;

	do {
		str = next;
		if (str) {
			// First time!
			if (*str == '/')
				str++;
			next = strchr(str, '/');
			if (next) {
				*next++ = '\0';
				assert(*next != '/');
			}
		}

		if (str)
			exists = name_is_child(new, str);
		else
			exists = false;

		if (!exists) {
			if (next && !match_end && last == next)
				break;
			else if (!next && !match_end && str == last)
				break;
			else {
				dirent_put(new->nm_dirent);
				free(new);
				new = NULL;
				ret = -ENOENT;
				goto found;
			}
		} else if (!match_end && last == str) {
			dirent_put(new->nm_dirent);
			free(new);
			new = NULL;
			ret = -EEXIST;
			goto found;
		}
	} while (next);

found:
	free(buf);

	super_block_put(sb);
	*nm = new;
	return ret;
}

int reffs_fuse_getattr(const char *path, struct stat *st)
{
	struct name_match *nm;
	struct inode *inode;
	int ret;

	ret = find_matching_directory_entry(&nm, path, LAST_COMPONENT_IS_MATCH);
	if (ret)
		return ret;

	inode = nm->nm_dirent->d_inode;

	st->st_ino = inode->i_ino;
	st->st_uid = inode->i_uid;
	st->st_gid = inode->i_gid;
	st->st_mtim = inode->i_mtime;
	st->st_atim = inode->i_atime;
	st->st_ctim = inode->i_ctime;
	st->st_mode = inode->i_mode;
	st->st_size = inode->i_size;
	st->st_nlink = inode->i_nlink;
	st->st_blocks = inode->i_used;
	st->st_blksize = 4096;

	dirent_put(nm->nm_dirent);
	free(nm);

	return 0;
}

int reffs_fuse_mkdir(const char *path, mode_t mode)
{
	struct name_match *nm;
	struct inode *inode = NULL;
	struct super_block *sb;
	struct dirent *de = NULL;

	int ret;

	ret = find_matching_directory_entry(&nm, path, LAST_COMPONENT_IS_NEW);
	if (ret)
		return ret;

	inode = nm->nm_dirent->d_inode;
	sb = inode->i_sb;

	if (!(inode->i_mode & S_IFDIR)) {
		ret = -ENOTDIR;
		goto out;
	}

	if (!strcmp(nm->nm_name, "..")) {
		ret = -ENOTEMPTY;
		goto out;
	}

	pthread_mutex_lock(&nm->nm_dirent->d_lock);
	de = dirent_alloc(nm->nm_dirent, nm->nm_name, reffs_life_action_birth);
	pthread_mutex_unlock(&nm->nm_dirent->d_lock);
	if (!de) {
		ret = -ENOENT;
		goto out;
	}

	de->d_inode = inode_alloc(sb, uatomic_add_return(&sb->sb_next_ino, 1));
	if (!de->d_inode) {
		dirent_parent_release(de, reffs_life_action_death);
		ret = -ENOENT;
		goto out;
	}

	de->d_inode->i_uid = getuid();
	de->d_inode->i_gid = getgid();
	clock_gettime(CLOCK_REALTIME, &de->d_inode->i_mtime);
	de->d_inode->i_atime = inode->i_mtime;
	de->d_inode->i_btime = inode->i_mtime;
	de->d_inode->i_ctime = inode->i_mtime;
	de->d_inode->i_mode = S_IFDIR | mode;
	de->d_inode->i_size = 4096;
	de->d_inode->i_used = 8;
	de->d_inode->i_nlink = 2;

out:
	dirent_put(de);
	dirent_put(nm->nm_dirent);
	free(nm);

	return ret;
}

int reffs_fuse_mknod(const char *path, mode_t mode, dev_t rdev)
{
	struct name_match *nm;
	struct inode *inode = NULL;
	struct super_block *sb;
	struct dirent *de = NULL;

	int ret;

	ret = find_matching_directory_entry(&nm, path, LAST_COMPONENT_IS_NEW);
	if (ret)
		return ret;

	inode = nm->nm_dirent->d_inode;
	sb = inode->i_sb;

	if (!(inode->i_mode & S_IFDIR)) {
		ret = -ENOTDIR;
		goto out;
	}

	if (mode & S_IFDIR) {
		ret = -EISDIR;
		goto out;
	}

	if (!strcmp(nm->nm_name, "..")) {
		ret = -ENOTEMPTY;
		goto out;
	}

	pthread_mutex_lock(&nm->nm_dirent->d_lock);
	de = dirent_alloc(nm->nm_dirent, nm->nm_name, reffs_life_action_birth);
	pthread_mutex_unlock(&nm->nm_dirent->d_lock);
	if (!de) {
		ret = -ENOENT;
		goto out;
	}

	de->d_inode = inode_alloc(sb, uatomic_add_return(&sb->sb_next_ino, 1));
	if (!de->d_inode) {
		dirent_parent_release(de, reffs_life_action_death);
		ret = -ENOENT;
		goto out;
	}

	de->d_inode->i_uid = getuid();
	de->d_inode->i_gid = getgid();
	clock_gettime(CLOCK_REALTIME, &de->d_inode->i_mtime);
	de->d_inode->i_atime = inode->i_mtime;
	de->d_inode->i_btime = inode->i_mtime;
	de->d_inode->i_ctime = inode->i_mtime;
	de->d_inode->i_mode = mode; // For now, assume a file!
	de->d_inode->i_size = 0;
	de->d_inode->i_used = 0;
	de->d_inode->i_nlink = 1;

out:
	dirent_put(de);
	dirent_put(nm->nm_dirent);
	free(nm);

	return ret;
}

int reffs_fuse_read(const char *path, char *buffer, size_t size, off_t offset,
		    struct fuse_file_info *fi)
{
	struct name_match *nm;
	struct inode *inode = NULL;

	int ret;

	ret = find_matching_directory_entry(&nm, path, LAST_COMPONENT_IS_MATCH);
	if (ret)
		return ret;

	pthread_mutex_lock(&nm->nm_dirent->d_lock);

	inode = nm->nm_dirent->d_inode;

	// Perhaps a reader/write lock?
	pthread_mutex_lock(&inode->i_db_lock);

	if (inode->i_mode & S_IFDIR) {
		ret = -EISDIR;
		goto out;
	}

	if (!inode->i_db)
		ret = 0;
	else {
		ret = data_block_read(inode->i_db, buffer, size, offset);
		if (!ret && size) {
			ret = -EOVERFLOW;
		}
	}

	pthread_mutex_lock(&inode->i_attr_lock);
	inode_update_times_now(inode, REFFS_INODE_UPDATE_ATIME);
	pthread_mutex_unlock(&inode->i_attr_lock);

out:
	pthread_mutex_unlock(&inode->i_db_lock);
	pthread_mutex_unlock(&nm->nm_dirent->d_lock);
	dirent_put(nm->nm_dirent);
	free(nm);

	if (ret < 0) {
		errno = -ret;
		ret = -1;
	}

	return ret;
}

int reffs_fuse_readdir(const char *path, void *buffer, fuse_fill_dir_t filler,
		       off_t offset, struct fuse_file_info *fi)
{
	struct name_match *nm;
	struct dirent *de;

	int ret;

	filler(buffer, ".", NULL, 0);
	filler(buffer, "..", NULL, 0);

	ret = find_matching_directory_entry(&nm, path, LAST_COMPONENT_IS_MATCH);
	if (ret)
		return ret;

	rcu_read_lock();
	cds_list_for_each_entry_rcu(de, &nm->nm_dirent->d_children, d_siblings)
		filler(buffer, de->d_name, NULL, 0);
	rcu_read_unlock();

	dirent_put(nm->nm_dirent);
	free(nm);

	return 0;
}

int reffs_fuse_rmdir(const char *path)
{
	struct name_match *nm;
	struct inode *inode = NULL;

	int ret;

	if (!strcmp("/", path))
		return -EBUSY;

	ret = find_matching_directory_entry(&nm, path, LAST_COMPONENT_IS_MATCH);
	if (ret)
		return ret;

	inode = nm->nm_dirent->d_inode;

	pthread_mutex_lock(&nm->nm_dirent->d_lock);
	if (!(inode->i_mode & S_IFDIR)) {
		ret = -ENOTDIR;
		goto out;
	}

	if (!cds_list_empty(&(nm->nm_dirent->d_children))) {
		ret = -ENOTEMPTY;
		goto out;
	}

	if (!strcmp(nm->nm_name, "..")) {
		ret = -ENOTEMPTY;
		goto out;
	}

	dirent_parent_release(nm->nm_dirent, reffs_life_action_death);

out:
	pthread_mutex_unlock(&nm->nm_dirent->d_lock);
	dirent_put(nm->nm_dirent);
	free(nm);

	return ret;
}

int reffs_fuse_write(const char *path, const char *buffer, size_t size,
		     off_t offset, struct fuse_file_info *info)
{
	struct name_match *nm;
	struct inode *inode = NULL;

	int ret;

	ret = find_matching_directory_entry(&nm, path, LAST_COMPONENT_IS_MATCH);
	if (ret)
		return ret;

	pthread_mutex_lock(&nm->nm_dirent->d_lock);

	inode = nm->nm_dirent->d_inode;

	pthread_mutex_lock(&inode->i_db_lock);

	if (inode->i_mode & S_IFDIR) {
		ret = -EISDIR;
		goto out;
	}

	if (!inode->i_db) {
		inode->i_db = data_block_alloc(buffer, size, offset);
		if (!inode->i_db)
			ret = -ENOSPC;
	} else {
		ret = data_block_write(inode->i_db, buffer, size, offset);
	}

	pthread_mutex_lock(&inode->i_attr_lock);
	inode_update_times_now(inode, REFFS_INODE_UPDATE_CTIME |
					      REFFS_INODE_UPDATE_MTIME);
	inode->i_size = inode->i_db->db_size;
	inode->i_used = inode->i_size / 4096 + (inode->i_size % 4096 ? 1 : 0);
	pthread_mutex_unlock(&inode->i_attr_lock);

	ret = size;
out:
	pthread_mutex_unlock(&inode->i_db_lock);
	pthread_mutex_unlock(&nm->nm_dirent->d_lock);
	dirent_put(nm->nm_dirent);
	free(nm);

	if (ret < 0) {
		errno = -ret;
		ret = -1;
	}

	return ret;
}

int reffs_fuse_unlink(const char *path)
{
	struct name_match *nm;
	struct inode *inode = NULL;

	int ret;

	ret = find_matching_directory_entry(&nm, path, LAST_COMPONENT_IS_MATCH);
	if (ret)
		return ret;

	inode = nm->nm_dirent->d_inode;

	pthread_mutex_lock(&nm->nm_dirent->d_lock);
	if (inode->i_mode & S_IFDIR) {
		ret = -EISDIR;
		goto out;
	}

	dirent_parent_release(nm->nm_dirent, reffs_life_action_death);

out:
	pthread_mutex_unlock(&nm->nm_dirent->d_lock);
	dirent_put(nm->nm_dirent);
	free(nm);

	return ret;
}
