/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <urcu/list.h>
#include <urcu/map/urcu-memb.h>
#include <urcu/rculfhash.h>
#include <urcu/rculist.h>

#include "reffs/backend.h"
#include "reffs/cmp.h"
#include "reffs/context.h"
#include "reffs/data_block.h"
#include "reffs/dirent.h"
#include "reffs/fs.h"
#include "reffs/identity.h"
#include "reffs/inode.h"
#include "reffs/log.h"
#include "reffs/super_block.h"
#include "reffs/types.h"
#include "reffs/vfs.h"
#include "rpc/auth_unix.h"
#include "reffs/trace/fs.h"

struct timespec;

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

	sb = super_block_find(SUPER_BLOCK_ROOT_ID);
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
		if (!new->nm_name) {
			ret = -ENOMEM;
			goto err;
		}
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

	if (!new->nm_name) {
		ret = -ENOMEM;
		goto err;
	}

	/* Walk components */
	token = strtok_r(buf, "/", &saveptr);
	while (token != NULL) {
		char *next_token = strtok_r(NULL, "/", &saveptr);

		if (strlen(token) > REFFS_MAX_NAME) {
			ret = -ENAMETOOLONG;
			goto err;
		}

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

		struct inode *next_inode = dirent_ensure_inode(next_de);
		if (!next_inode) {
			dirent_put(next_de);
			ret = -ENOENT;
			goto err;
		}
		bool next_is_dir = S_ISDIR(next_inode->i_mode);
		inode_active_put(next_inode);
		if (!next_is_dir) {
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

	inode = dirent_ensure_inode(nm->nm_dirent);
	if (!inode) {
		name_match_free(nm);
		return -ENOENT;
	}

	ap.aup_uid = uid;
	ap.aup_gid = gid;
	ap.aup_len = 0;
	ap.aup_gids = NULL;

	ret = inode_access_check(inode, &ap, mode);

	inode_active_put(inode);
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

	struct inode *inode = dirent_ensure_inode(nm->nm_dirent);
	if (!inode) {
		name_match_free(nm);
		return -ENOENT;
	}
	ret = vfs_setattr(inode, &rs, NULL);
	inode_active_put(inode);
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

	struct inode *inode = dirent_ensure_inode(nm->nm_dirent);
	if (!inode) {
		name_match_free(nm);
		return -ENOENT;
	}
	ret = vfs_setattr(inode, &rs, NULL);
	inode_active_put(inode);
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

	struct inode *dir = dirent_ensure_inode(nm->nm_dirent);
	if (!dir) {
		name_match_free(nm);
		return -ENOENT;
	}

	trace_fs_inode(dir, __func__, __LINE__);
	TRACE("ino=%lu freeing struct at %p", dir->i_ino, (void *)dir);

	ret = vfs_create(dir, nm->nm_name, mode, &ap, NULL, NULL, NULL);
	inode_active_put(dir);

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

	struct inode *dir = dirent_ensure_inode(nm->nm_dirent);
	if (!dir) {
		name_match_free(nm);
		return -ENOENT;
	}
	ret = vfs_mkdir(dir, nm->nm_name, mode, &ap, NULL, NULL, NULL);
	inode_active_put(dir);

	name_match_free(nm);
	return ret;
}

/*
 * Recursive mkdir -- create all intermediate directories as needed.
 * Equivalent to `mkdir -p`.  Returns 0 on success, -errno on failure.
 * Existing directories along the path are silently skipped.
 */
int reffs_fs_mkdir_p(const char *path, mode_t mode)
{
	char buf[REFFS_MAX_PATH + 1];
	int ret;

	if (!path || path[0] != '/')
		return -EINVAL;
	if (strlen(path) > REFFS_MAX_PATH)
		return -ENAMETOOLONG;
	if (strcmp(path, "/") == 0)
		return 0;

	strncpy(buf, path, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';

	/* Walk each component and mkdir, ignoring EEXIST. */
	for (char *p = buf + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		ret = reffs_fs_mkdir(buf, mode);
		if (ret && ret != -EEXIST) {
			return ret;
		}
		*p = '/';
	}

	/* Final component. */
	ret = reffs_fs_mkdir(buf, mode);
	if (ret == -EEXIST)
		ret = 0;
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

	struct inode *dir = dirent_ensure_inode(nm->nm_dirent);
	if (!dir) {
		name_match_free(nm);
		return -ENOENT;
	}
	ret = vfs_mknod(dir, nm->nm_name, mode, rdev, &ap, NULL, NULL, NULL);
	inode_active_put(dir);

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

	struct inode *dir = dirent_ensure_inode(nm->nm_dirent);
	if (!dir) {
		name_match_free(nm);
		return -ENOENT;
	}
	ret = vfs_symlink(dir, nm->nm_name, target, &ap, NULL, NULL, NULL);
	inode_active_put(dir);

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

	ret = find_matching_directory_entry(&nm, path, LAST_COMPONENT_IS_MATCH);
	if (ret) {
		goto out;
	}

	inode = dirent_ensure_inode(nm->nm_dirent);
	if (!inode) {
		ret = -ENOENT;
		name_match_free(nm);
		goto out;
	}

	st->st_ino = inode->i_ino;
	st->st_uid = reffs_id_to_uid(inode->i_uid);
	st->st_gid = reffs_id_to_uid(inode->i_gid);
	st->st_mtim = inode->i_mtime;
	st->st_atim = inode->i_atime;
	st->st_ctim = inode->i_ctime;
	st->st_mode = inode->i_mode;
	st->st_size = inode->i_size;
	st->st_nlink = inode->i_nlink;
	st->st_blocks = inode->i_used * (inode->i_sb->sb_block_size / 512);
	st->st_blksize = inode->i_sb->sb_block_size;

	inode_active_put(inode);
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

	struct inode *src_inode = dirent_ensure_inode(nm_src->nm_dirent);
	if (!src_inode) {
		name_match_free(nm_src);
		name_match_free(nm_dst);
		return -ENOENT;
	}
	struct inode *dst_dir = dirent_ensure_inode(nm_dst->nm_dirent);
	if (!dst_dir) {
		inode_active_put(src_inode);
		name_match_free(nm_src);
		name_match_free(nm_dst);
		return -ENOENT;
	}
	ret = vfs_link(src_inode, dst_dir, nm_dst->nm_name, &ap);
	inode_active_put(src_inode);
	inode_active_put(dst_dir);

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

	inode = dirent_ensure_inode(nm->nm_dirent);
	if (!inode) {
		name_match_free(nm);
		ret = -ENOENT;
		goto out;
	}

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
	inode_active_put(inode);
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

	inode = dirent_ensure_inode(nm->nm_dirent);
	if (!inode) {
		ret = -ENOENT;
		goto out_nm;
	}
	if (!S_ISLNK(inode->i_mode)) {
		ret = -EINVAL;
		goto out_inode;
	}

	if (!inode->i_symlink) {
		ret = -EIO;
		goto out_inode;
	}

	size_t sym_len = strlen(inode->i_symlink);
	size_t copy_len = (sym_len < len) ? sym_len : len;

	memcpy(buffer, inode->i_symlink, copy_len);
	if (copy_len < len)
		buffer[copy_len] = '\0';

out_inode:
	inode_active_put(inode);
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

	struct inode *old_dir =
		dirent_ensure_inode(nm_src->nm_dirent->rd_parent);
	if (!old_dir) {
		name_match_free(nm_src);
		name_match_free(nm_dst);
		return -ENOENT;
	}

	struct reffs_dirent *dst_de =
		dst_exists ? nm_dst->nm_dirent->rd_parent : nm_dst->nm_dirent;
	struct inode *new_dir = dirent_ensure_inode(dst_de);
	if (!new_dir) {
		inode_active_put(old_dir);
		name_match_free(nm_src);
		name_match_free(nm_dst);
		return -ENOENT;
	}

	reffs_get_authunix_parms(&ap);
	ret = vfs_rename(old_dir, nm_src->nm_name, new_dir, nm_dst->nm_name,
			 &ap, NULL, NULL, NULL, NULL);

	inode_active_put(old_dir);
	inode_active_put(new_dir);
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

	struct inode *dir = dirent_ensure_inode(nm->nm_dirent->rd_parent);
	if (!dir) {
		name_match_free(nm);
		return -ENOENT;
	}
	ret = vfs_rmdir(dir, nm->nm_name, &ap, NULL, NULL);
	inode_active_put(dir);

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

	struct inode *udir = dirent_ensure_inode(nm->nm_dirent->rd_parent);
	if (!udir) {
		name_match_free(nm);
		return -ENOENT;
	}
	ret = vfs_remove(udir, nm->nm_name, &ap, NULL, NULL);
	inode_active_put(udir);

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

	struct inode *inode = dirent_ensure_inode(nm->nm_dirent);
	if (!inode) {
		name_match_free(nm);
		return -ENOENT;
	}
	ret = vfs_setattr(inode, &rs, &ap);
	inode_active_put(inode);

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

	inode = dirent_ensure_inode(nm->nm_dirent);
	if (!inode) {
		name_match_free(nm);
		ret = -ENOENT;
		goto out;
	}

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
	old_used = atomic_load_explicit(&inode->i_sb->sb_bytes_used,
					memory_order_relaxed);
	do {
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
	} while (!atomic_compare_exchange_strong_explicit(
		&inode->i_sb->sb_bytes_used, &old_used, new_used,
		memory_order_seq_cst, memory_order_relaxed));

	inode_update_times_now(inode, REFFS_INODE_UPDATE_CTIME |
					      REFFS_INODE_UPDATE_MTIME);

	ret = size;
out_puts:
	pthread_rwlock_unlock(&inode->i_db_rwlock);
	pthread_mutex_unlock(&inode->i_attr_mutex);
	inode_active_put(inode);
	name_match_free(nm);

out:
	TRACE("ret=%d", ret);
	return ret;
}

void reffs_fs_recover(struct super_block *sb)
{
	if (!sb || !sb->sb_ops || !sb->sb_ops->recover)
		return;

	TRACE("Starting recovery for sb %lu from %s", sb->sb_id,
	      sb->sb_backend_path ? sb->sb_backend_path : "(none)");

	sb->sb_ops->recover(sb);

	TRACE("Recovery complete. Max inode: %lu", sb->sb_next_ino);
}

int reffs_fs_usage(struct reffs_fs_usage_stats *stats)
{
	struct super_block *sb;

	memset(stats, 0, sizeof(*stats));

	rcu_read_lock();
	cds_list_for_each_entry_rcu(sb, super_block_list_head(), sb_link) {
		size_t bu;

		bu = atomic_load_explicit(&sb->sb_bytes_used,
					  memory_order_relaxed);
		stats->used_bytes += bu;
		stats->used_files += atomic_load_explicit(&sb->sb_inodes_used,
							  memory_order_relaxed);
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
