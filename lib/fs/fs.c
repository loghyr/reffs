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
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
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
#include "reffs/fs.h"
#include "reffs/log.h"

// Remove once this gets fleshed out
#pragma GCC diagnostic ignored "-Wunused-parameter"

enum reffs_text_case fuse_rtc = reffs_text_case_sensitive;

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
// FIXME: Ignores symlinks
// TODO: Check to see if fuse allows them, but in any event, fix????
//
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
	if (!strcmp(path, "/"))
		goto found;

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

int reffs_fs_access(const char *path, int mode, uid_t uid, gid_t gid)
{
	struct name_match *nm;
	struct inode *inode;

	int ret;

	TRACE("path=%s mode=0%o", path, mode);

	ret = find_matching_directory_entry(&nm, path, LAST_COMPONENT_IS_MATCH);
	if (ret)
		goto out;

	if (mode == F_OK)
		goto out_puts;

	inode = nm->nm_dirent->d_inode;
	if (uid == inode->i_uid) {
		if ((mode & W_OK) && !(inode->i_mode & S_IWUSR)) {
			ret = -EACCES;
			goto out_puts;
		}
		if ((mode & R_OK) && !(inode->i_mode & S_IRUSR)) {
			ret = -EACCES;
			goto out_puts;
		}
		if ((mode & X_OK) && !(inode->i_mode & S_IXUSR)) {
			ret = -EACCES;
			goto out_puts;
		}
	} else if (gid == inode->i_gid) {
		if ((mode & W_OK) && !(inode->i_mode & S_IWGRP)) {
			ret = -EACCES;
			goto out_puts;
		}
		if ((mode & R_OK) && !(inode->i_mode & S_IRGRP)) {
			ret = -EACCES;
			goto out_puts;
		}
		if ((mode & X_OK) && !(inode->i_mode & S_IXGRP)) {
			ret = -EACCES;
			goto out_puts;
		}
	} else {
		if ((mode & W_OK) && !(inode->i_mode & S_IWOTH)) {
			ret = -EACCES;
			goto out_puts;
		}
		if ((mode & R_OK) && !(inode->i_mode & S_IROTH)) {
			ret = -EACCES;
			goto out_puts;
		}
		if ((mode & X_OK) && !(inode->i_mode & S_IXOTH)) {
			ret = -EACCES;
			goto out_puts;
		}
	}

out_puts:
	dirent_put(nm->nm_dirent);
	free(nm);

out:
	TRACE("ret=%d", ret);
	return ret;
}

int reffs_fs_chmod(const char *path, mode_t mode)
{
	struct name_match *nm;
	struct inode *inode = NULL;

	int ret;

	TRACE("path=%s mode=0%o", path, mode);

	ret = find_matching_directory_entry(&nm, path, LAST_COMPONENT_IS_MATCH);
	if (ret)
		goto out;

	pthread_mutex_lock(&nm->nm_dirent->d_lock);

	inode = nm->nm_dirent->d_inode;

	pthread_mutex_lock(&inode->i_db_lock);

	pthread_mutex_lock(&inode->i_attr_lock);
	inode_update_times_now(inode, REFFS_INODE_UPDATE_CTIME |
					      REFFS_INODE_UPDATE_MTIME);
	inode->i_mode = (mode & 07777);
	pthread_mutex_unlock(&inode->i_attr_lock);

	pthread_mutex_unlock(&inode->i_db_lock);
	pthread_mutex_unlock(&nm->nm_dirent->d_lock);
	dirent_put(nm->nm_dirent);
	free(nm);

out:
	TRACE("ret=%d", ret);
	return ret;
}

int reffs_fs_chown(const char *path, uid_t uid, gid_t gid)
{
	struct name_match *nm;
	struct inode *inode = NULL;

	int ret;

	TRACE("path=%s uid=%u gid=%u", path, uid, gid);

	ret = find_matching_directory_entry(&nm, path, LAST_COMPONENT_IS_MATCH);
	if (ret)
		goto out;

	pthread_mutex_lock(&nm->nm_dirent->d_lock);

	inode = nm->nm_dirent->d_inode;

	pthread_mutex_lock(&inode->i_db_lock);

	pthread_mutex_lock(&inode->i_attr_lock);
	inode_update_times_now(inode, REFFS_INODE_UPDATE_CTIME |
					      REFFS_INODE_UPDATE_MTIME);
	inode->i_uid = uid;
	inode->i_gid = gid;
	pthread_mutex_unlock(&inode->i_attr_lock);

	pthread_mutex_unlock(&inode->i_db_lock);
	pthread_mutex_unlock(&nm->nm_dirent->d_lock);
	dirent_put(nm->nm_dirent);
	free(nm);

out:
	TRACE("ret=%d", ret);
	return ret;
}

int reffs_fs_create(const char *path, mode_t mode)
{
	struct name_match *nm;
	struct inode *inode = NULL;
	struct super_block *sb;
	struct dirent *de = NULL;

	int ret;

	TRACE("path=%s mode=0%o", path, mode);

	ret = find_matching_directory_entry(&nm, path, LAST_COMPONENT_IS_NEW);
	if (ret)
		goto out;

	inode = nm->nm_dirent->d_inode;
	sb = inode->i_sb;

	if (!(inode->i_mode & S_IFDIR)) {
		ret = -ENOTDIR;
		goto out_puts;
	}

	if (mode & S_IFDIR) {
		ret = -EISDIR;
		goto out_puts;
	}

	if (!strcmp(nm->nm_name, "..")) {
		ret = -ENOTEMPTY;
		goto out_puts;
	}

	pthread_mutex_lock(&nm->nm_dirent->d_lock);
	de = dirent_alloc(nm->nm_dirent, nm->nm_name, reffs_life_action_birth);
	pthread_mutex_unlock(&nm->nm_dirent->d_lock);
	if (!de) {
		ret = -ENOENT;
		goto out_puts;
	}

	de->d_inode = inode_alloc(sb, uatomic_add_return(&sb->sb_next_ino, 1));
	if (!de->d_inode) {
		dirent_parent_release(de, reffs_life_action_death);
		ret = -ENOENT;
		goto out_puts;
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

out_puts:
	dirent_put(de);
	dirent_put(nm->nm_dirent);
	free(nm);

out:
	TRACE("ret=%d", ret);
	return ret;
}

int reffs_fs_fallocate(const char *path, int mode, off_t offset, off_t len)
{
	TRACE("path=%s mode=0%o offset=%lu len=%lu", path, mode, offset, len);
	return 0;
}

int reffs_fs_getattr(const char *path, struct stat *st)
{
	struct name_match *nm;
	struct inode *inode;
	int ret;

	TRACE("path=%s", path);

	ret = find_matching_directory_entry(&nm, path, LAST_COMPONENT_IS_MATCH);
	if (ret)
		goto out;

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

out:
	TRACE("ret=%d", ret);
	return ret;
}

int reffs_fs_mkdir(const char *path, mode_t mode)
{
	struct name_match *nm;
	struct inode *inode = NULL;
	struct super_block *sb;
	struct dirent *de = NULL;

	int ret;

	TRACE("path=%s mode=0%o", path, mode);

	ret = find_matching_directory_entry(&nm, path, LAST_COMPONENT_IS_NEW);
	if (ret)
		goto out;

	inode = nm->nm_dirent->d_inode;
	sb = inode->i_sb;

	if (!(inode->i_mode & S_IFDIR)) {
		ret = -ENOTDIR;
		goto out_puts;
	}

	if (!strcmp(nm->nm_name, "..")) {
		ret = -ENOTEMPTY;
		goto out_puts;
	}

	pthread_mutex_lock(&nm->nm_dirent->d_lock);
	de = dirent_alloc(nm->nm_dirent, nm->nm_name, reffs_life_action_birth);
	pthread_mutex_unlock(&nm->nm_dirent->d_lock);
	if (!de) {
		ret = -ENOENT;
		goto out_puts;
	}

	de->d_inode = inode_alloc(sb, uatomic_add_return(&sb->sb_next_ino, 1));
	if (!de->d_inode) {
		dirent_parent_release(de, reffs_life_action_death);
		ret = -ENOENT;
		goto out_puts;
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

out_puts:
	dirent_put(de);
	dirent_put(nm->nm_dirent);
	free(nm);

out:
	TRACE("ret=%d", ret);
	return ret;
}

int reffs_fs_mknod(const char *path, mode_t mode, dev_t rdev)
{
	struct name_match *nm;
	struct inode *inode = NULL;
	struct super_block *sb;
	struct dirent *de = NULL;

	int ret;

	TRACE("path=%s mode=0%o rdev=%lu", path, mode, rdev);

	ret = find_matching_directory_entry(&nm, path, LAST_COMPONENT_IS_NEW);
	if (ret)
		goto out;

	inode = nm->nm_dirent->d_inode;
	sb = inode->i_sb;

	if (!(inode->i_mode & S_IFDIR)) {
		ret = -ENOTDIR;
		goto out_puts;
	}

	if (mode & S_IFDIR) {
		ret = -EISDIR;
		goto out_puts;
	}

	if (!strcmp(nm->nm_name, "..")) {
		ret = -ENOTEMPTY;
		goto out_puts;
	}

	pthread_mutex_lock(&nm->nm_dirent->d_lock);
	de = dirent_alloc(nm->nm_dirent, nm->nm_name, reffs_life_action_birth);
	pthread_mutex_unlock(&nm->nm_dirent->d_lock);
	if (!de) {
		ret = -ENOENT;
		goto out_puts;
	}

	de->d_inode = inode_alloc(sb, uatomic_add_return(&sb->sb_next_ino, 1));
	if (!de->d_inode) {
		dirent_parent_release(de, reffs_life_action_death);
		ret = -ENOENT;
		goto out_puts;
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

out_puts:
	dirent_put(de);
	dirent_put(nm->nm_dirent);
	free(nm);

out:
	TRACE("ret=%d", ret);
	return ret;
}

int reffs_fs_read(const char *path, char *buffer, size_t size, off_t offset)
{
	struct name_match *nm;
	struct inode *inode = NULL;

	int ret;

	TRACE("path=%s size=%lu offset=%lu", path, size, offset);

	ret = find_matching_directory_entry(&nm, path, LAST_COMPONENT_IS_MATCH);
	if (ret)
		goto out;

	pthread_mutex_lock(&nm->nm_dirent->d_lock);

	inode = nm->nm_dirent->d_inode;

	// Perhaps a reader/write lock?
	pthread_mutex_lock(&inode->i_db_lock);

	if (inode->i_mode & S_IFDIR) {
		ret = -EISDIR;
		goto out_unlock;
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

out_unlock:
	pthread_mutex_unlock(&inode->i_db_lock);
	pthread_mutex_unlock(&nm->nm_dirent->d_lock);
	dirent_put(nm->nm_dirent);
	free(nm);

out:
	TRACE("ret=%d", ret);
	return ret;
}

/* For FUSE, doing it there. */
int reffs_fs_readdir(const char *path, void *buffer, char *filler, off_t offset)
{
	int ret = 0;
	TRACE("path=%s offset=%lu", path, offset);

	TRACE("ret=%d", ret);
	return ret;
}

int reffs_fs_readlink(const char *path, char *buffer, size_t len)
{
	int ret = 0;
	TRACE("path=%s len=%lu", path, len);

	TRACE("ret=%d", ret);
	return ret;
}

static int rename_dest_locked(struct name_match *nm_src, struct dirent *de_dst,
			      char *dst_name)
{
	int ret = 0;
	reffs_strng_compare cmp;
	char *name;
	char *old;

	TRACE("nm_src=%s(%s) dst=%s name=%s", nm_src->nm_name,
	      nm_src->nm_dirent->d_name, de_dst->d_name, dst_name);

	// In case we refactor
	if (fuse_rtc == reffs_text_case_insensitive)
		cmp = strcasecmp;
	else
		cmp = strcmp;

	/* If they are the same path, do nothing */
	if (nm_src->nm_dirent == de_dst && !cmp(nm_src->nm_name, dst_name))
		return 0;

	name = strdup(dst_name);
	if (!name) {
		ret = -ENOMEM;
	} else {
		rcu_read_lock();
		old = rcu_xchg_pointer(&nm_src->nm_dirent->d_name, name);
		rcu_read_unlock();
		free(old);
		pthread_mutex_lock(&de_dst->d_inode->i_attr_lock);
		inode_update_times_now(de_dst->d_inode,
				       REFFS_INODE_UPDATE_CTIME |
					       REFFS_INODE_UPDATE_MTIME);
		pthread_mutex_unlock(&de_dst->d_inode->i_attr_lock);
	}

	return ret;
}

static int rename_dest(struct name_match *nm_src, struct dirent *de_dst,
		       char *dst_name)
{
	int ret;

	pthread_mutex_lock(&nm_src->nm_dirent->d_parent->d_lock);
	ret = rename_dest_locked(nm_src, de_dst, dst_name);
	pthread_mutex_unlock(&nm_src->nm_dirent->d_parent->d_lock);

	return ret;
}

//
// FIXME: Symlinks
//
// Fuse seems to do this:
// mv: cannot move '/tmp/reffs/passwd' to '/tmp/reffs/foo/bar/passwd': No such file or directory
//
int reffs_fs_rename(const char *src_path, const char *dst_path)
{
	struct name_match *nm_src;
	struct name_match *nm_dst;

	int ret;

	bool dst_exists = false;

	TRACE("src_path=%s dst_path=%s", src_path, dst_path);

	if (!strcmp(src_path, "/") || !strcmp(dst_path, "/")) {
		ret = -EFAULT;
		goto out;
	}

	ret = find_matching_directory_entry(&nm_src, src_path,
					    LAST_COMPONENT_IS_MATCH);
	if (ret)
		goto out;
	TRACE("nm_src=%s, de=%s", nm_src->nm_name, nm_src->nm_dirent->d_name);

	// TODO: make sure the paths are not overlapped if dirs
	ret = find_matching_directory_entry(&nm_dst, dst_path,
					    LAST_COMPONENT_IS_MATCH);
	if (ret) {
		if (ret == -ENOENT) {
			TRACE("Finding target directory");
			ret = find_matching_directory_entry(
				&nm_dst, dst_path, LAST_COMPONENT_IS_NEW);
		}

		if (ret) {
			dirent_put(nm_src->nm_dirent);
			free(nm_src);
			goto out;
		}
	} else {
		TRACE("Already exists");
		dst_exists = true;
	}

	if (!dst_exists) {
		if (!(nm_dst->nm_dirent->d_inode->i_mode & S_IFDIR)) {
			ret = -ENOTDIR;
			goto out_unlock;
#ifdef NOT_NOW
		} else if (!cds_list_empty(&(nm_dst->nm_dirent->d_children))) {
			// man page says it must be empty
			ret = -ENOTEMPTY;
			goto out_unlock;
#endif
		}
	}

	TRACE("nm_dst=%s, de=%s", nm_dst->nm_name, nm_dst->nm_dirent->d_name);

	if (!strcmp(nm_dst->nm_name, "..")) {
		ret = -ENOTEMPTY;
		goto out_unlock;
	}

	if (nm_dst->nm_dirent->d_inode->i_ino ==
	    nm_src->nm_dirent->d_inode->i_ino) {
		ret = 0;
		goto out_unlock;
	}

	if (nm_src->nm_dirent == nm_dst->nm_dirent) {
		TRACE("Renaming within the same parent");
		ret = rename_dest(nm_src, nm_dst->nm_dirent, nm_dst->nm_name);
	} else {
		struct dirent *de_src_pin;
		struct dirent *de_dst_parent;
		struct dirent *de_dst_pin;
		struct dirent *de_delete_dst = NULL;

		/*
		 * FIXME: Detect other sb boundaries!
		 */
		if (!(nm_dst->nm_dirent->d_parent)) {
			TRACE("Destination is root");
			struct super_block *sb = super_block_find(1);
			verify(sb);

			de_dst_parent = dirent_get(sb->sb_dirent);
			super_block_put(sb);
		} else {
			de_dst_parent = dirent_get(nm_dst->nm_dirent->d_parent);
		}
		TRACE("dst parent de=%s", de_dst_parent->d_name);

		verify(de_dst_parent);
		verify(nm_src->nm_dirent->d_parent);

		de_src_pin = dirent_get(nm_src->nm_dirent->d_parent);

		if (dst_exists) {
			if ((nm_dst->nm_dirent->d_inode->i_mode & S_IFDIR))
				de_dst_pin = dirent_get(nm_dst->nm_dirent);
			else {
				de_dst_pin = dirent_get(de_dst_parent);
				de_delete_dst = dirent_get(nm_dst->nm_dirent);
			}
		} else {
			de_dst_pin = dirent_get(nm_dst->nm_dirent);
			TRACE("Pinned parent de=%s", de_dst_pin->d_name);
		}

		pthread_mutex_lock(&de_dst_pin->d_lock);
		if (de_dst_pin != de_src_pin)
			pthread_mutex_lock(&de_src_pin->d_lock);

		dirent_parent_release(nm_src->nm_dirent,
				      reffs_life_action_update);
		dirent_parent_attach(nm_src->nm_dirent, de_dst_pin,
				     reffs_life_action_update);
		ret = rename_dest_locked(nm_src, de_dst_pin, nm_dst->nm_name);
		if (de_dst_pin != de_src_pin)
			pthread_mutex_unlock(&de_src_pin->d_lock);
		pthread_mutex_unlock(&de_dst_pin->d_lock);

		dirent_put(de_src_pin);
		dirent_put(de_dst_pin);
		dirent_put(de_dst_parent);

		dirent_parent_release(de_delete_dst, reffs_life_action_death);
		dirent_put(de_delete_dst);
	}

out_unlock:
	dirent_put(nm_src->nm_dirent);
	free(nm_src);
	dirent_put(nm_dst->nm_dirent);
	free(nm_dst);

out:
	TRACE("ret=%d", ret);
	return ret;
}

int reffs_fs_rmdir(const char *path)
{
	struct name_match *nm;
	struct inode *inode = NULL;

	int ret;

	TRACE("path=%s", path);

	if (!strcmp("/", path)) {
		ret = -EBUSY;
		goto out;
	}

	ret = find_matching_directory_entry(&nm, path, LAST_COMPONENT_IS_MATCH);
	if (ret)
		goto out;

	inode = nm->nm_dirent->d_inode;

	pthread_mutex_lock(&nm->nm_dirent->d_lock);
	if (!(inode->i_mode & S_IFDIR)) {
		ret = -ENOTDIR;
		goto out_unlock;
	}

	if (!cds_list_empty(&(nm->nm_dirent->d_children))) {
		ret = -ENOTEMPTY;
		goto out_unlock;
	}

	if (!strcmp(nm->nm_name, "..")) {
		ret = -ENOTEMPTY;
		goto out_unlock;
	}

	dirent_parent_release(nm->nm_dirent, reffs_life_action_death);
out_unlock:
	pthread_mutex_unlock(&nm->nm_dirent->d_lock);
	dirent_put(nm->nm_dirent);
	free(nm);
out:
	TRACE("ret=%d", ret);
	return ret;
}

int reffs_fs_symlink(const char *path, const char *new_path)
{
	int ret = 0;

	TRACE("path=%s new_path=%s", path, new_path);
	TRACE("ret=%d", ret);
	return ret;
}

int reffs_fs_unlink(const char *path)
{
	struct name_match *nm;
	struct inode *inode = NULL;

	int ret;

	TRACE("path=%s", path);

	ret = find_matching_directory_entry(&nm, path, LAST_COMPONENT_IS_MATCH);
	if (ret)
		goto out;

	inode = nm->nm_dirent->d_inode;

	pthread_mutex_lock(&nm->nm_dirent->d_lock);
	if (inode->i_mode & S_IFDIR) {
		ret = -EISDIR;
		goto out_unlock;
	}

	dirent_parent_release(nm->nm_dirent, reffs_life_action_death);
out_unlock:
	pthread_mutex_unlock(&nm->nm_dirent->d_lock);
	dirent_put(nm->nm_dirent);
	free(nm);
out:
	TRACE("ret=%d", ret);
	return ret;
}

int reffs_fs_utimensat(const char *path, const struct timespec times[2])
{
	struct name_match *nm;
	struct inode *inode = NULL;

	int ret;

	TRACE("path=%s atime=(%lu.%ld) mtime=(%lu.%ld)", path, times[0].tv_sec,
	      times[0].tv_nsec, times[1].tv_sec, times[1].tv_nsec);

	ret = find_matching_directory_entry(&nm, path, LAST_COMPONENT_IS_MATCH);
	if (ret)
		goto out;

	inode = nm->nm_dirent->d_inode;
	inode->i_atime = times[0];
	inode->i_mtime = times[1];

	dirent_put(nm->nm_dirent);
	free(nm);
out:
	TRACE("ret=%d", ret);
	return ret;
}

int reffs_fs_write(const char *path, const char *buffer, size_t size,
		   off_t offset)
{
	struct name_match *nm;
	struct inode *inode = NULL;

	int ret;

	TRACE("path=%s size=%lu offset=%lu", path, size, offset);

	ret = find_matching_directory_entry(&nm, path, LAST_COMPONENT_IS_MATCH);
	if (ret)
		goto out;

	pthread_mutex_lock(&nm->nm_dirent->d_lock);

	inode = nm->nm_dirent->d_inode;

	pthread_mutex_lock(&inode->i_db_lock);

	if (inode->i_mode & S_IFDIR) {
		ret = -EISDIR;
		goto out_unlock;
	}

	if (!inode->i_db) {
		inode->i_db = data_block_alloc(buffer, size, offset);
		if (!inode->i_db) {
			ret = -ENOSPC;
			goto out_unlock;
		}
	} else {
		ret = data_block_write(inode->i_db, buffer, size, offset);
		if (ret < 0) {
			goto out_unlock;
		}
	}

	pthread_mutex_lock(&inode->i_attr_lock);
	inode_update_times_now(inode, REFFS_INODE_UPDATE_CTIME |
					      REFFS_INODE_UPDATE_MTIME);
	inode->i_size = inode->i_db->db_size;
	inode->i_used = inode->i_size / 4096 + (inode->i_size % 4096 ? 1 : 0);
	pthread_mutex_unlock(&inode->i_attr_lock);

	ret = size;
out_unlock:
	pthread_mutex_unlock(&inode->i_db_lock);
	pthread_mutex_unlock(&nm->nm_dirent->d_lock);
	dirent_put(nm->nm_dirent);
	free(nm);

out:
	TRACE("ret=%d", ret);
	return ret;
}
