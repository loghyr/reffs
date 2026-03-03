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
#include <dirent.h>
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
#include "reffs/cmp.h"

// Remove once this gets fleshed out
#pragma GCC diagnostic ignored "-Wunused-parameter"

static enum reffs_storage_type global_storage_type = REFFS_STORAGE_RAM;
static char *global_backend_path = NULL;

void reffs_fs_set_storage(enum reffs_storage_type type, const char *path)
{
	global_storage_type = type;
	if (global_backend_path)
		free(global_backend_path);
	if (path)
		global_backend_path = strdup(path);
	else
		global_backend_path = NULL;
}

enum reffs_storage_type reffs_fs_get_storage_type(void)
{
	return global_storage_type;
}

char *reffs_fs_get_backend_path(void)
{
	return global_backend_path;
}

static bool name_is_child(struct name_match *nm, char *name)
{
	bool exists = false;
	struct reffs_dirent *rd;

	reffs_strng_compare cmp = reffs_text_case_cmp();

	rcu_read_lock();
	cds_list_for_each_entry_rcu(rd, &nm->nm_dirent->rd_inode->i_children,
				    rd_siblings) {
		if (!cmp(rd->rd_name, name)) {
			exists = true;
			dirent_put(nm->nm_dirent);
			nm->nm_dirent = dirent_get(rd);
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

	inode = nm->nm_dirent->rd_inode;
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

	pthread_rwlock_wrlock(&nm->nm_dirent->rd_rwlock);

	inode = nm->nm_dirent->rd_inode;

	pthread_mutex_lock(&inode->i_attr_mutex);
	inode_update_times_now(inode, REFFS_INODE_UPDATE_CTIME |
					      REFFS_INODE_UPDATE_MTIME);
	inode->i_mode = (mode & 07777);
	pthread_mutex_unlock(&inode->i_attr_mutex);

	pthread_rwlock_unlock(&nm->nm_dirent->rd_rwlock);
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

	pthread_rwlock_wrlock(&nm->nm_dirent->rd_rwlock);

	inode = nm->nm_dirent->rd_inode;

	pthread_mutex_lock(&inode->i_attr_mutex);
	inode_update_times_now(inode, REFFS_INODE_UPDATE_CTIME |
					      REFFS_INODE_UPDATE_MTIME);
	inode->i_uid = uid;
	inode->i_gid = gid;
	pthread_mutex_unlock(&inode->i_attr_mutex);

	pthread_rwlock_unlock(&nm->nm_dirent->rd_rwlock);
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
	struct reffs_dirent *rd = NULL;

	int ret;

	TRACE("path=%s mode=0%o", path, mode);

	ret = find_matching_directory_entry(&nm, path, LAST_COMPONENT_IS_NEW);
	if (ret)
		goto out;

	inode = nm->nm_dirent->rd_inode;
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

	pthread_rwlock_wrlock(&nm->nm_dirent->rd_rwlock);
	rd = dirent_alloc(nm->nm_dirent, nm->nm_name, reffs_life_action_birth);
	pthread_rwlock_unlock(&nm->nm_dirent->rd_rwlock);
	if (!rd) {
		ret = -ENOENT;
		goto out_puts;
	}

	rd->rd_inode = inode_alloc(sb, __atomic_add_fetch(&sb->sb_next_ino, 1,
							  __ATOMIC_RELAXED));
	if (!rd->rd_inode) {
		dirent_parent_release(rd, reffs_life_action_death);
		ret = -ENOENT;
		goto out_puts;
	}

	rd->rd_inode->i_uid = getuid();
	rd->rd_inode->i_gid = getgid();
	clock_gettime(CLOCK_REALTIME, &rd->rd_inode->i_mtime);
	rd->rd_inode->i_atime = inode->i_mtime;
	rd->rd_inode->i_btime = inode->i_mtime;
	rd->rd_inode->i_ctime = inode->i_mtime;
	rd->rd_inode->i_mode = mode; // For now, assume a file!
	rd->rd_inode->i_size = 0;
	rd->rd_inode->i_used = 0;
	rd->rd_inode->i_nlink = 1;

	inode_sync_to_disk(rd->rd_inode);

out_puts:
	dirent_put(rd);
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

	inode = nm->nm_dirent->rd_inode;

	st->st_ino = inode->i_ino;
	st->st_uid = inode->i_uid;
	st->st_gid = inode->i_gid;
	st->st_mtim = inode->i_mtime;
	st->st_atim = inode->i_atime;
	st->st_ctim = inode->i_ctime;
	st->st_mode = inode->i_mode;
	st->st_size = inode->i_size;
	st->st_nlink = inode->i_nlink;
	st->st_blocks = inode->i_used * (inode->i_sb->sb_block_size / 512);
	st->st_blksize = inode->i_sb->sb_block_size;

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
	struct reffs_dirent *rd = NULL;

	int ret;

	TRACE("path=%s mode=0%o", path, mode);

	ret = find_matching_directory_entry(&nm, path, LAST_COMPONENT_IS_NEW);
	if (ret)
		goto out;

	inode = nm->nm_dirent->rd_inode;
	sb = inode->i_sb;

	if (!(inode->i_mode & S_IFDIR)) {
		ret = -ENOTDIR;
		goto out_puts;
	}

	if (!strcmp(nm->nm_name, "..")) {
		ret = -ENOTEMPTY;
		goto out_puts;
	}

	pthread_rwlock_wrlock(&nm->nm_dirent->rd_rwlock);
	rd = dirent_alloc(nm->nm_dirent, nm->nm_name, reffs_life_action_birth);
	pthread_rwlock_unlock(&nm->nm_dirent->rd_rwlock);
	if (!rd) {
		ret = -ENOENT;
		goto out_puts;
	}

	rd->rd_inode = inode_alloc(sb, __atomic_add_fetch(&sb->sb_next_ino, 1,
							  __ATOMIC_RELAXED));
	if (!rd->rd_inode) {
		dirent_parent_release(rd, reffs_life_action_death);
		ret = -ENOENT;
		goto out_puts;
	}

	rd->rd_inode->i_uid = getuid();
	rd->rd_inode->i_gid = getgid();
	clock_gettime(CLOCK_REALTIME, &rd->rd_inode->i_mtime);
	rd->rd_inode->i_atime = inode->i_mtime;
	rd->rd_inode->i_btime = inode->i_mtime;
	rd->rd_inode->i_ctime = inode->i_mtime;
	rd->rd_inode->i_mode = S_IFDIR | mode;
	rd->rd_inode->i_size = inode->i_sb->sb_block_size;
	rd->rd_inode->i_used = 1;
	rd->rd_inode->i_nlink = 2;

	inode_sync_to_disk(rd->rd_inode);

out_puts:
	dirent_put(rd);
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
	struct reffs_dirent *rd = NULL;

	int ret;

	TRACE("path=%s mode=0%o rdev=%lu", path, mode, rdev);

	ret = find_matching_directory_entry(&nm, path, LAST_COMPONENT_IS_NEW);
	if (ret)
		goto out;

	inode = nm->nm_dirent->rd_inode;
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

	pthread_rwlock_wrlock(&nm->nm_dirent->rd_rwlock);
	rd = dirent_alloc(nm->nm_dirent, nm->nm_name, reffs_life_action_birth);
	pthread_rwlock_unlock(&nm->nm_dirent->rd_rwlock);
	if (!rd) {
		ret = -ENOENT;
		goto out_puts;
	}

	rd->rd_inode = inode_alloc(sb, __atomic_add_fetch(&sb->sb_next_ino, 1,
							  __ATOMIC_RELAXED));
	if (!rd->rd_inode) {
		dirent_parent_release(rd, reffs_life_action_death);
		ret = -ENOENT;
		goto out_puts;
	}

	rd->rd_inode->i_uid = getuid();
	rd->rd_inode->i_gid = getgid();
	clock_gettime(CLOCK_REALTIME, &rd->rd_inode->i_mtime);
	rd->rd_inode->i_atime = inode->i_mtime;
	rd->rd_inode->i_btime = inode->i_mtime;
	rd->rd_inode->i_ctime = inode->i_mtime;
	rd->rd_inode->i_mode = mode; // For now, assume a file!
	rd->rd_inode->i_size = 0;
	rd->rd_inode->i_used = 0;
	rd->rd_inode->i_nlink = 1;

	inode_sync_to_disk(rd->rd_inode);

out_puts:
	dirent_put(rd);
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

	inode = nm->nm_dirent->rd_inode;

	// Perhaps a reader/write lock?
	pthread_rwlock_rdlock(&inode->i_db_rwlock);

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

	pthread_mutex_lock(&inode->i_attr_mutex);
	inode_update_times_now(inode, REFFS_INODE_UPDATE_ATIME);
	pthread_mutex_unlock(&inode->i_attr_mutex);

out_unlock:
	pthread_rwlock_unlock(&inode->i_db_rwlock);
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

static int rename_dest_locked(struct name_match *nm_src,
			      struct reffs_dirent *rd_dst, char *dst_name)
{
	int ret = 0;
	reffs_strng_compare cmp = reffs_text_case_cmp();
	char *name;
	char *old;

	TRACE("nm_src=%s(%s) dst=%s name=%s", nm_src->nm_name,
	      nm_src->nm_dirent->rd_name, rd_dst->rd_name, dst_name);

	/* If they are the same path, do nothing */
	if (nm_src->nm_dirent == rd_dst && !cmp(nm_src->nm_name, dst_name))
		return 0;

	name = strdup(dst_name);
	if (!name) {
		ret = -ENOMEM;
	} else {
		rcu_read_lock();
		old = rcu_xchg_pointer(&nm_src->nm_dirent->rd_name, name);
		rcu_read_unlock();
		free(old);
		pthread_mutex_lock(&rd_dst->rd_inode->i_attr_mutex);
		inode_update_times_now(rd_dst->rd_inode,
				       REFFS_INODE_UPDATE_CTIME |
					       REFFS_INODE_UPDATE_MTIME);
		pthread_mutex_unlock(&rd_dst->rd_inode->i_attr_mutex);
	}

	return ret;
}

static int rename_dest(struct name_match *nm_src, struct reffs_dirent *rd_dst,
		       char *dst_name)
{
	int ret;

	pthread_rwlock_wrlock(&nm_src->nm_dirent->rd_parent->rd_rwlock);
	ret = rename_dest_locked(nm_src, rd_dst, dst_name);
	pthread_rwlock_unlock(&nm_src->nm_dirent->rd_parent->rd_rwlock);

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
	TRACE("nm_src=%s, de=%s", nm_src->nm_name, nm_src->nm_dirent->rd_name);

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
		if (!(nm_dst->nm_dirent->rd_inode->i_mode & S_IFDIR)) {
			ret = -ENOTDIR;
			goto out_unlock;
#ifdef NOT_NOW
		} else if (!cds_list_empty(&(
				   nm_dst->nm_dirent->rd_inode->i_children))) {
			// man page says it must be empty
			ret = -ENOTEMPTY;
			goto out_unlock;
#endif
		}
	}

	TRACE("nm_dst=%s, de=%s", nm_dst->nm_name, nm_dst->nm_dirent->rd_name);

	if (!strcmp(nm_dst->nm_name, "..")) {
		ret = -ENOTEMPTY;
		goto out_unlock;
	}

	if (nm_dst->nm_dirent->rd_inode->i_ino ==
	    nm_src->nm_dirent->rd_inode->i_ino) {
		ret = 0;
		goto out_unlock;
	}

	if (nm_src->nm_dirent == nm_dst->nm_dirent) {
		TRACE("Renaming within the same parent");
		ret = rename_dest(nm_src, nm_dst->nm_dirent, nm_dst->nm_name);
	} else {
		struct reffs_dirent *rd_src_pin;
		struct reffs_dirent *rd_dst_parent;
		struct reffs_dirent *rd_dst_pin;
		struct reffs_dirent *rd_delete_dst = NULL;

		/*
		 * FIXME: Detect other sb boundaries!
		 */
		if (!(nm_dst->nm_dirent->rd_parent)) {
			TRACE("Destination is root");
			struct super_block *sb = super_block_find(1);
			verify(sb);

			rd_dst_parent = dirent_get(sb->sb_dirent);
			super_block_put(sb);
		} else {
			rd_dst_parent =
				dirent_get(nm_dst->nm_dirent->rd_parent);
		}
		TRACE("dst parent de=%s", rd_dst_parent->rd_name);

		verify(rd_dst_parent);
		verify(nm_src->nm_dirent->rd_parent);

		rd_src_pin = dirent_get(nm_src->nm_dirent->rd_parent);

		if (dst_exists) {
			if ((nm_dst->nm_dirent->rd_inode->i_mode & S_IFDIR))
				rd_dst_pin = dirent_get(nm_dst->nm_dirent);
			else {
				rd_dst_pin = dirent_get(rd_dst_parent);
				rd_delete_dst = dirent_get(nm_dst->nm_dirent);
			}
		} else {
			rd_dst_pin = dirent_get(nm_dst->nm_dirent);
			TRACE("Pinned parent de=%s", rd_dst_pin->rd_name);
		}

		pthread_rwlock_wrlock(&rd_dst_pin->rd_rwlock);
		if (rd_dst_pin != rd_src_pin)
			pthread_rwlock_wrlock(&rd_src_pin->rd_rwlock);

		dirent_parent_release(nm_src->nm_dirent,
				      reffs_life_action_update);
		dirent_parent_attach(nm_src->nm_dirent, rd_dst_pin,
				     reffs_life_action_update);
		ret = rename_dest_locked(nm_src, rd_dst_pin, nm_dst->nm_name);
		if (rd_dst_pin != rd_src_pin)
			pthread_rwlock_unlock(&rd_src_pin->rd_rwlock);
		pthread_rwlock_unlock(&rd_dst_pin->rd_rwlock);

		dirent_put(rd_src_pin);
		dirent_put(rd_dst_pin);
		dirent_put(rd_dst_parent);

		dirent_parent_release(rd_delete_dst, reffs_life_action_death);
		dirent_put(rd_delete_dst);
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

	inode = nm->nm_dirent->rd_inode;

	pthread_rwlock_wrlock(&nm->nm_dirent->rd_rwlock);
	if (!(inode->i_mode & S_IFDIR)) {
		ret = -ENOTDIR;
		goto out_unlock;
	}

	if (!cds_list_empty(&(nm->nm_dirent->rd_inode->i_children))) {
		ret = -ENOTEMPTY;
		goto out_unlock;
	}

	if (!strcmp(nm->nm_name, "..")) {
		ret = -ENOTEMPTY;
		goto out_unlock;
	}

	dirent_parent_release(nm->nm_dirent, reffs_life_action_death);
out_unlock:
	pthread_rwlock_unlock(&nm->nm_dirent->rd_rwlock);
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

	inode = nm->nm_dirent->rd_inode;

	pthread_rwlock_wrlock(&nm->nm_dirent->rd_rwlock);
	if (inode->i_mode & S_IFDIR) {
		ret = -EISDIR;
		goto out_unlock;
	}

	dirent_parent_release(nm->nm_dirent, reffs_life_action_death);
out_unlock:
	pthread_rwlock_unlock(&nm->nm_dirent->rd_rwlock);
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

	inode = nm->nm_dirent->rd_inode;
	inode->i_atime = times[0];
	inode->i_mtime = times[1];

	inode_sync_to_disk(inode);

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

	inode = nm->nm_dirent->rd_inode;

	pthread_rwlock_wrlock(&inode->i_db_rwlock);

	if (inode->i_mode & S_IFDIR) {
		ret = -EISDIR;
		goto out_unlock;
	}

	if (!inode->i_db) {
		inode->i_db = data_block_alloc(inode, buffer, size, offset);
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

	pthread_mutex_lock(&inode->i_attr_mutex);
	inode_update_times_now(inode, REFFS_INODE_UPDATE_CTIME |
					      REFFS_INODE_UPDATE_MTIME);
	inode->i_size = inode->i_db->db_size;
	inode->i_used = inode->i_size / inode->i_sb->sb_block_size +
			(inode->i_size % inode->i_sb->sb_block_size ? 1 : 0);
	pthread_mutex_unlock(&inode->i_attr_mutex);

	ret = size;
out_unlock:
	pthread_rwlock_unlock(&inode->i_db_rwlock);
	dirent_put(nm->nm_dirent);
	free(nm);

out:
	TRACE("ret=%d", ret);
	return ret;
}

static int load_inode_attributes(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct inode_disk id;
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "%s/sb_%lu/ino_%lu.meta",
		 sb->sb_backend_path, sb->sb_id, inode->i_ino);

	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return -errno;

	if (read(fd, &id, sizeof(id)) != sizeof(id)) {
		close(fd);
		return -EIO;
	}
	close(fd);

	inode->i_uid = id.id_uid;
	inode->i_gid = id.id_gid;
	inode->i_nlink = id.id_nlink;
	inode->i_mode = id.id_mode;
	inode->i_size = id.id_size;
	inode->i_atime = id.id_atime;
	inode->i_ctime = id.id_ctime;
	inode->i_mtime = id.id_mtime;

	if (inode->i_ino >= sb->sb_next_ino)
		sb->sb_next_ino = inode->i_ino + 1;

	// Also check if data file exists
	snprintf(path, sizeof(path), "%s/sb_%lu/ino_%lu.dat",
		 sb->sb_backend_path, sb->sb_id, inode->i_ino);
	if (access(path, F_OK) == 0) {
		inode->i_db = data_block_alloc(inode, NULL, 0, 0);
		if (inode->i_db) {
			/*
			 * Accumulate sb_bytes_used from the real on-disk size.
			 * data_block_alloc() with size=0 calls fstat() to
			 * populate db_size, so this reflects actual disk usage
			 * rather than the size stored in the .meta file.
			 * Mirror what the write path in nfs3_server.c does so
			 * that FSSTAT returns correct values after recovery.
			 */
			size_t db_size = inode->i_db->db_size;
			size_t old_used;
			size_t new_used;
			do {
				__atomic_load(&sb->sb_bytes_used, &old_used,
					      __ATOMIC_RELAXED);
				new_used = old_used + db_size;
			} while (!__atomic_compare_exchange(
				&sb->sb_bytes_used, &old_used, &new_used, false,
				__ATOMIC_SEQ_CST, __ATOMIC_RELAXED));

			inode->i_used = db_size / sb->sb_block_size +
					(db_size % sb->sb_block_size ? 1 : 0);
		}
	}

	// Also check if symlink file exists
	snprintf(path, sizeof(path), "%s/sb_%lu/ino_%lu.lnk",
		 sb->sb_backend_path, sb->sb_id, inode->i_ino);
	if (access(path, F_OK) == 0) {
		fd = open(path, O_RDONLY);
		if (fd >= 0) {
			struct stat st;
			if (fstat(fd, &st) == 0) {
				inode->i_symlink = malloc(st.st_size + 1);
				if (inode->i_symlink) {
					if (read(fd, inode->i_symlink,
						 st.st_size) == st.st_size) {
						inode->i_symlink[st.st_size] =
							'\0';
					} else {
						free(inode->i_symlink);
						inode->i_symlink = NULL;
					}
				}
			}
			close(fd);
		}
	}

	return 0;
}

static void recover_directory_recursive(struct reffs_dirent *parent)
{
	struct inode *inode = parent->rd_inode;
	struct super_block *sb = inode->i_sb;
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "%s/sb_%lu/ino_%lu.dir",
		 sb->sb_backend_path, sb->sb_id, inode->i_ino);

	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return;

	uint64_t cookie_next;
	if (read(fd, &cookie_next, sizeof(cookie_next)) ==
	    sizeof(cookie_next)) {
		parent->rd_cookie_next = cookie_next;
	} else {
		/* Fallback for old format or empty file */
		parent->rd_cookie_next = 3;
	}

	uint64_t cookie;
	uint64_t ino;
	uint16_t name_len;
	char name[256];

	while (read(fd, &cookie, sizeof(cookie)) == sizeof(cookie)) {
		if (read(fd, &ino, sizeof(ino)) != sizeof(ino))
			break;
		if (read(fd, &name_len, sizeof(name_len)) != sizeof(name_len))
			break;
		if (read(fd, name, name_len) != (ssize_t)name_len)
			break;
		name[name_len] = '\0';

		struct reffs_dirent *rd =
			dirent_alloc(parent, name, reffs_life_action_load);
		if (rd) {
			rd->rd_cookie = cookie;
			rd->rd_inode = inode_alloc(sb, ino);
			if (rd->rd_inode) {
				load_inode_attributes(rd->rd_inode);
				if (rd->rd_inode->i_mode & S_IFDIR) {
					rd->rd_inode->i_parent = rd;
					recover_directory_recursive(rd);
				}
			}
			dirent_put(rd);
		}
	}
	close(fd);
}

void reffs_fs_recover(struct super_block *sb)
{
	if (!sb || sb->sb_storage_type != REFFS_STORAGE_POSIX ||
	    !sb->sb_backend_path)
		return;

	LOG("Starting recovery from %s", sb->sb_backend_path);

	/* Scan directory for all meta files to find the true max inode number */
	char sb_path[PATH_MAX];
	snprintf(sb_path, sizeof(sb_path), "%s/sb_%lu", sb->sb_backend_path,
		 sb->sb_id);

	DIR *dir = opendir(sb_path);
	if (dir) {
		struct dirent *de;
		while ((de = readdir(dir)) != NULL) {
			uint64_t ino;
			if (sscanf(de->d_name, "ino_%lu.meta", &ino) == 1) {
				if (ino >= sb->sb_next_ino)
					sb->sb_next_ino = ino + 1;
			}
		}
		closedir(dir);
	}

	// Root inode is 1
	struct inode *root_inode = sb->sb_dirent->rd_inode;
	if (load_inode_attributes(root_inode) == 0) {
		recover_directory_recursive(sb->sb_dirent);
	}

	LOG("Recovery complete. Max inode: %lu", sb->sb_next_ino);
}

void reffs_fs_for_each_inode(int (*cb)(struct inode *, void *), void *arg)
{
	struct super_block *sb;
	struct inode *inode;
	struct cds_lfht_iter iter;

	rcu_read_lock();
	cds_list_for_each_entry_rcu(sb, super_block_list_head(), sb_link) {
		cds_lfht_for_each_entry(sb->sb_inodes, &iter, inode, i_node) {
			if (cb(inode, arg)) {
				/* Callback can return non-zero to stop early, but we don't need it yet */
			}
		}
	}
	rcu_read_unlock();
}
