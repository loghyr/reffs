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
#include <sys/statvfs.h>
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
#include "reffs/vfs.h"
#include "reffs/identity.h"
#include "reffs/log.h"
#include "reffs/cmp.h"
#include "reffs/backend.h"

#include "reffs/context.h"

// Remove once this gets fleshed out
#pragma GCC diagnostic ignored "-Wunused-parameter"

static void reffs_get_authunix_parms(struct authunix_parms *ap);

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

//
// FIXME: Ignores symlinks
// TODO: Check to see if fuse allows them, but in any event, fix????
//
int find_matching_directory_entry(struct name_match **nm, const char *path,
				  bool match_end)
{
	struct super_block *sb;
	struct name_match *new;
	char *buf = NULL;
	char *token;
	char *saveptr;
	struct reffs_dirent *current_de = NULL;
	int ret = 0;

	if (strlen(path) > REFFS_MAX_PATH) {
		return -ENAMETOOLONG;
	}

	*nm = NULL;

	sb = super_block_find(1);
	if (!sb)
		return -ENODEV;

	new = calloc(1, sizeof(*new));
	if (!new) {
		super_block_put(sb);
		return -ENOMEM;
	}

	current_de = dirent_get(sb->sb_dirent);
	new->nm_dirent = current_de;

	if (strcmp(path, "/") == 0) {
		new->nm_name = strdup("/");
		goto found;
	}

	buf = strdup(path);
	if (!buf) {
		ret = -ENOMEM;
		goto err;
	}

	/* Find the last component name */
	char *last_slash = strrchr(path, '/');
	if (last_slash)
		new->nm_name = strdup(last_slash + 1);
	else
		new->nm_name = strdup(path);

	/* Walk components */
	token = strtok_r(buf, "/", &saveptr);
	while (token != NULL) {
		char *next_token = strtok_r(NULL, "/", &saveptr);

		struct reffs_dirent *next_de = dirent_find(
			current_de, reffs_text_case_sensitive, token);

		if (next_token == NULL) {
			/* Last component */
			if (match_end) {
				if (!next_de) {
					ret = -ENOENT;
					goto err;
				}
				dirent_put(current_de);
				current_de = next_de;
				new->nm_dirent = next_de;
			} else {
				if (next_de) {
					dirent_put(next_de);
					ret = -EEXIST;
					goto err;
				}
				/* new->nm_dirent remains the parent */
			}
			break;
		}

		/* Intermediate component must exist and be a directory */
		if (!next_de) {
			ret = -ENOENT;
			goto err;
		}

		if (!S_ISDIR(next_de->rd_inode->i_mode)) {
			dirent_put(next_de);
			ret = -ENOTDIR;
			goto err;
		}

		dirent_put(current_de);
		current_de = next_de;
		new->nm_dirent = current_de;
		token = next_token;
	}

found:
	free(buf);
	super_block_put(sb);
	*nm = new;
	return 0;

err:
	if (ret) {
	}
	free(buf);
	name_match_free(new);
	super_block_put(sb);
	return ret;
}

void name_match_free(struct name_match *nm)
{
	if (!nm)
		return;
	if (nm->nm_dirent)
		dirent_put(nm->nm_dirent);
	free(nm->nm_name);
	free(nm);
}

int reffs_fs_access(const char *path, int mode, uid_t uid, gid_t gid)
{
	struct name_match *nm;
	struct inode *inode;
	int ret;
	struct authunix_parms ap;

	TRACE("path=%s mode=0%o uid=%u gid=%u", path, mode, uid, gid);

	ret = find_matching_directory_entry(&nm, path, LAST_COMPONENT_IS_MATCH);
	if (ret)
		return ret;

	if (mode == F_OK) {
		name_match_free(nm);
		return 0;
	}

	inode = nm->nm_dirent->rd_inode;

	ap.aup_uid = uid;
	ap.aup_gid = gid;
	ap.aup_len = 0;
	ap.aup_gids = NULL;

	ret = inode_access_check(inode, &ap, mode);

	name_match_free(nm);
	return ret;
}

int reffs_fs_chmod(const char *path, mode_t mode)
{
	struct name_match *nm = NULL;
	int ret;
	struct reffs_sattr rs;

	TRACE("path=%s mode=0%o", path, mode);

	ret = find_matching_directory_entry(&nm, path, LAST_COMPONENT_IS_MATCH);
	if (ret)
		return ret;

	memset(&rs, 0, sizeof(rs));
	rs.mode = mode;
	rs.mode_set = true;

	ret = vfs_setattr(nm->nm_dirent->rd_inode, &rs, NULL);
	name_match_free(nm);

	TRACE("ret=%d", ret);
	return ret;
}

int reffs_fs_chown(const char *path, uid_t uid, gid_t gid)
{
	struct name_match *nm = NULL;
	int ret;
	struct reffs_sattr rs;

	TRACE("path=%s uid=%u gid=%u", path, uid, gid);

	ret = find_matching_directory_entry(&nm, path, LAST_COMPONENT_IS_MATCH);
	if (ret)
		return ret;

	memset(&rs, 0, sizeof(rs));
	if (uid != (uid_t)-1) {
		rs.uid = uid;
		rs.uid_set = true;
	}
	if (gid != (gid_t)-1) {
		rs.gid = gid;
		rs.gid_set = true;
	}

	ret = vfs_setattr(nm->nm_dirent->rd_inode, &rs, NULL);
	name_match_free(nm);

	TRACE("ret=%d", ret);
	return ret;
}

static void reffs_get_authunix_parms(struct authunix_parms *ap);

int reffs_fs_create(const char *path, mode_t mode)
{
	struct name_match *nm = NULL;
	int ret;
	struct authunix_parms ap;

	TRACE("path=%s mode=0%o", path, mode);

	ret = find_matching_directory_entry(&nm, path, LAST_COMPONENT_IS_NEW);
	if (ret)
		return ret;

	reffs_get_authunix_parms(&ap);
	ret = vfs_create(nm->nm_dirent->rd_inode, nm->nm_name, mode, &ap, NULL);

	name_match_free(nm);
	return ret;
}

int reffs_fs_mkdir(const char *path, mode_t mode)
{
	struct name_match *nm = NULL;
	int ret;
	struct authunix_parms ap;

	TRACE("path=%s mode=0%o", path, mode);

	ret = find_matching_directory_entry(&nm, path, LAST_COMPONENT_IS_NEW);
	if (ret)
		return ret;

	reffs_get_authunix_parms(&ap);
	ret = vfs_mkdir(nm->nm_dirent->rd_inode, nm->nm_name, mode, &ap, NULL);

	name_match_free(nm);
	return ret;
}

int reffs_fs_mknod(const char *path, mode_t mode, dev_t rdev)
{
	struct name_match *nm = NULL;
	int ret;
	struct authunix_parms ap;

	TRACE("path=%s mode=0%o rdev=%lu", path, mode, rdev);

	ret = find_matching_directory_entry(&nm, path, LAST_COMPONENT_IS_NEW);
	if (ret)
		return ret;

	reffs_get_authunix_parms(&ap);
	ret = vfs_mknod(nm->nm_dirent->rd_inode, nm->nm_name, mode, rdev, &ap,
			NULL);

	name_match_free(nm);
	return ret;
}

int reffs_fs_symlink(const char *target, const char *linkpath)
{
	struct name_match *nm = NULL;
	int ret;
	struct authunix_parms ap;

	TRACE("target=%s linkpath=%s", target, linkpath);

	ret = find_matching_directory_entry(&nm, linkpath,
					    LAST_COMPONENT_IS_NEW);
	if (ret)
		return ret;

	reffs_get_authunix_parms(&ap);
	ret = vfs_symlink(nm->nm_dirent->rd_inode, nm->nm_name, target, &ap,
			  NULL);

	name_match_free(nm);
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

	name_match_free(nm);

out:
	TRACE("ret=%d", ret);
	return ret;
}

int reffs_fs_link(const char *old_path, const char *new_path)
{
	struct name_match *nm_src = NULL;
	struct name_match *nm_dst = NULL;
	int ret;
	struct authunix_parms ap;

	TRACE("old_path=%s new_path=%s", old_path, new_path);

	ret = find_matching_directory_entry(&nm_src, old_path,
					    LAST_COMPONENT_IS_MATCH);
	if (ret)
		return ret;

	ret = find_matching_directory_entry(&nm_dst, new_path,
					    LAST_COMPONENT_IS_NEW);
	if (ret) {
		name_match_free(nm_src);
		return ret;
	}

	reffs_get_authunix_parms(&ap);
	ret = vfs_link(nm_src->nm_dirent->rd_inode, nm_dst->nm_dirent->rd_inode,
		       nm_dst->nm_name, &ap);

	name_match_free(nm_src);
	name_match_free(nm_dst);

	return ret ? -ret : 0;
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

	pthread_mutex_lock(&inode->i_attr_mutex);
	pthread_rwlock_rdlock(&inode->i_db_rwlock);

	if (S_ISDIR(inode->i_mode)) {
		ret = -EISDIR;
		goto out_puts;
	}

	if (!inode->i_db)
		ret = 0;
	else {
		ret = data_block_read(inode->i_db, buffer, size, offset);
		if (!ret && size) {
			ret = -EOVERFLOW;
		}
	}

	inode_update_times_now(inode, REFFS_INODE_UPDATE_ATIME);

out_puts:
	pthread_rwlock_unlock(&inode->i_db_rwlock);
	pthread_mutex_unlock(&inode->i_attr_mutex);
	name_match_free(nm);

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
	struct name_match *nm;
	struct inode *inode;
	int ret;

	TRACE("path=%s len=%lu", path, len);

	ret = find_matching_directory_entry(&nm, path, LAST_COMPONENT_IS_MATCH);
	if (ret)
		goto out;

	inode = nm->nm_dirent->rd_inode;
	if (!S_ISLNK(inode->i_mode)) {
		ret = -EINVAL;
		goto out_nm;
	}

	if (!inode->i_symlink) {
		ret = -EIO;
		goto out_nm;
	}

	size_t sym_len = strlen(inode->i_symlink);
	size_t copy_len = (sym_len < len) ? sym_len : len;

	memcpy(buffer, inode->i_symlink, copy_len);
	if (copy_len < len)
		buffer[copy_len] = '\0';

out_nm:
	name_match_free(nm);
out:
	TRACE("ret=%d", ret);
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
	struct name_match *nm_src = NULL;
	struct name_match *nm_dst = NULL;
	int ret;
	struct authunix_parms ap;
	bool dst_exists = false;

	TRACE("src_path=%s dst_path=%s", src_path, dst_path);

	if (!strcmp(src_path, "/") || !strcmp(dst_path, "/")) {
		return -EFAULT;
	}

	ret = find_matching_directory_entry(&nm_src, src_path,
					    LAST_COMPONENT_IS_MATCH);
	if (ret)
		return ret;

	ret = find_matching_directory_entry(&nm_dst, dst_path,
					    LAST_COMPONENT_IS_MATCH);
	if (ret == 0) {
		dst_exists = true;
	} else if (ret == -ENOENT) {
		ret = find_matching_directory_entry(&nm_dst, dst_path,
						    LAST_COMPONENT_IS_NEW);
		if (ret) {
			name_match_free(nm_src);
			return ret;
		}
	} else {
		name_match_free(nm_src);
		return ret;
	}

	struct inode *old_dir = nm_src->nm_dirent->rd_parent->rd_inode;
	struct inode *new_dir = dst_exists ?
					nm_dst->nm_dirent->rd_parent->rd_inode :
					nm_dst->nm_dirent->rd_inode;

	reffs_get_authunix_parms(&ap);
	ret = vfs_rename(old_dir, nm_src->nm_name, new_dir, nm_dst->nm_name,
			 &ap);

	name_match_free(nm_src);
	name_match_free(nm_dst);

	return ret;
}

int reffs_fs_rmdir(const char *path)
{
	struct name_match *nm = NULL;
	int ret;
	struct authunix_parms ap;

	TRACE("path=%s", path);

	if (!strcmp("/", path)) {
		return -EBUSY;
	}

	ret = find_matching_directory_entry(&nm, path, LAST_COMPONENT_IS_MATCH);
	if (ret)
		return ret;

	reffs_get_authunix_parms(&ap);
	ret = vfs_rmdir(nm->nm_dirent->rd_parent->rd_inode, nm->nm_name, &ap);

	name_match_free(nm);
	return ret;
}

int reffs_fs_unlink(const char *path)
{
	struct name_match *nm = NULL;
	int ret;
	struct authunix_parms ap;

	TRACE("path=%s", path);

	ret = find_matching_directory_entry(&nm, path, LAST_COMPONENT_IS_MATCH);
	if (ret)
		return ret;

	reffs_get_authunix_parms(&ap);
	ret = vfs_remove(nm->nm_dirent->rd_parent->rd_inode, nm->nm_name, &ap);

	name_match_free(nm);
	return ret;
}

int reffs_fs_utimensat(const char *path, const struct timespec times[2])
{
	struct name_match *nm;
	struct authunix_parms ap;
	struct reffs_sattr rs;
	int ret;

	TRACE("path=%s", path);

	ret = find_matching_directory_entry(&nm, path, LAST_COMPONENT_IS_MATCH);
	if (ret)
		return ret;

	reffs_get_authunix_parms(&ap);
	memset(&rs, 0, sizeof(rs));

	if (times == NULL) {
		rs.atime_set = true;
		rs.atime_now = true;
		rs.mtime_set = true;
		rs.mtime_now = true;
	} else {
		if (times[0].tv_nsec != UTIME_OMIT) {
			rs.atime_set = true;
			if (times[0].tv_nsec == UTIME_NOW)
				rs.atime_now = true;
			else
				rs.atime = times[0];
		}
		if (times[1].tv_nsec != UTIME_OMIT) {
			rs.mtime_set = true;
			if (times[1].tv_nsec == UTIME_NOW)
				rs.mtime_now = true;
			else
				rs.mtime = times[1];
		}
	}

	ret = vfs_setattr(nm->nm_dirent->rd_inode, &rs, &ap);

	name_match_free(nm);
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

	pthread_mutex_lock(&inode->i_attr_mutex);
	pthread_rwlock_wrlock(&inode->i_db_rwlock);

	if (S_ISDIR(inode->i_mode)) {
		ret = -EISDIR;
		goto out_puts;
	}

	if (!inode->i_db) {
		inode->i_db = data_block_alloc(inode, buffer, size, offset);
		if (!inode->i_db) {
			ret = -ENOSPC;
			goto out_puts;
		}
	} else {
		ret = data_block_write(inode->i_db, buffer, size, offset);
		if (ret < 0) {
			goto out_puts;
		}
	}

	size_t old_size = inode->i_size;
	inode->i_size = inode->i_db->db_size;
	size_t new_size = inode->i_size;
	inode->i_used = inode->i_size / inode->i_sb->sb_block_size +
			(inode->i_size % inode->i_sb->sb_block_size ? 1 : 0);

	size_t old_used;
	size_t new_used;
	do {
		__atomic_load(&inode->i_sb->sb_bytes_used, &old_used,
			      __ATOMIC_RELAXED);
		if (new_size > old_size) {
			new_used = old_used + (new_size - old_size);
		} else if (old_size > new_size) {
			size_t diff = old_size - new_size;
			if (old_used >= diff)
				new_used = old_used - diff;
			else
				new_used = 0;
		} else {
			new_used = old_used;
		}
	} while (!__atomic_compare_exchange(
		&inode->i_sb->sb_bytes_used, &old_used, &new_used, false,
		__ATOMIC_SEQ_CST, __ATOMIC_RELAXED));

	inode_update_times_now(inode, REFFS_INODE_UPDATE_CTIME |
					      REFFS_INODE_UPDATE_MTIME);

	ret = size;
out_puts:
	pthread_rwlock_unlock(&inode->i_db_rwlock);
	pthread_mutex_unlock(&inode->i_attr_mutex);
	name_match_free(nm);

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

	__atomic_fetch_add(&sb->sb_inodes_used, 1, __ATOMIC_RELAXED);

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

		struct reffs_dirent *rd = dirent_alloc(
			parent, name, reffs_life_action_load, false);
		if (rd) {
			rd->rd_cookie = cookie;
			rd->rd_inode = inode_alloc(sb, ino);
			if (rd->rd_inode) {
				load_inode_attributes(rd->rd_inode);
				if (S_ISDIR(rd->rd_inode->i_mode)) {
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
	if (!sb || sb->sb_ops->type != REFFS_STORAGE_POSIX ||
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
			} else if (strstr(de->d_name, ".tmp")) {
				char tmp_path[PATH_MAX];
				snprintf(tmp_path, sizeof(tmp_path), "%s/%s",
					 sb_path, de->d_name);
				LOG("Deleting stray tmp file %s", tmp_path);
				unlink(tmp_path);
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

int reffs_fs_usage(struct reffs_fs_usage_stats *stats)
{
	struct super_block *sb;

	memset(stats, 0, sizeof(*stats));

	rcu_read_lock();
	cds_list_for_each_entry_rcu(sb, super_block_list_head(), sb_link) {
		stats->used_bytes += sb->sb_bytes_used;
		stats->used_files += sb->sb_inodes_used;
		stats->total_bytes += sb->sb_bytes_max;
		stats->total_files += sb->sb_inodes_max;
	}
	rcu_read_unlock();

	if (stats->total_bytes > stats->used_bytes)
		stats->free_bytes = stats->total_bytes - stats->used_bytes;
	else
		stats->free_bytes = 0;

	if (stats->total_files > stats->used_files)
		stats->free_files = stats->total_files - stats->used_files;
	else
		stats->free_files = 0;

	TRACE("internal: used_bytes=%lu free_bytes=%lu total_bytes=%lu used_files=%lu free_files=%lu total_files=%lu",
	      stats->used_bytes, stats->free_bytes, stats->total_bytes,
	      stats->used_files, stats->free_files, stats->total_files);

	return 0;
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

static void reffs_get_authunix_parms(struct authunix_parms *ap)
{
	struct reffs_context *ctx = reffs_get_context();
	ap->aup_uid = ctx->uid;
	ap->aup_gid = ctx->gid;
	ap->aup_len = 0;
	ap->aup_gids = NULL;
}
