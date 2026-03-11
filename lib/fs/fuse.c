/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#define FUSE_USE_VERSION 31

#include <fuse/fuse.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <urcu/list.h>
#include <urcu/map/urcu-memb.h>
#include <urcu/rculist.h>

#include "reffs/context.h"
#include "reffs/dirent.h"
#include "reffs/fs.h"
#include "reffs/fuse.h"
#include "reffs/inode.h"
#include "reffs/log.h"
#include "reffs/super_block.h"

struct fuse_file_info;
struct statvfs;
struct timespec;

// Remove once this gets fleshed out
#pragma GCC diagnostic ignored "-Wunused-parameter"

static void set_fuse_context(void)
{
	struct fuse_context *fctx;

	if (getenv("REFFS_FUSE_UNIT_TEST")) {
		return;
	}

	fctx = fuse_get_context();
	if (fctx && fctx->fuse) {
		struct reffs_context ctx = { .uid = fctx->uid,
					     .gid = fctx->gid };
		reffs_set_context(&ctx);
	} else {
		reffs_set_context(NULL);
	}
}

int reffs_fuse_access(const char *path, int mode)
{
	set_fuse_context();
	return reffs_fs_access(path, mode, 0, 0);
}

int reffs_fuse_chmod(const char *path, mode_t mode)
{
	set_fuse_context();
	return reffs_fs_chmod(path, mode);
}

int reffs_fuse_chown(const char *path, uid_t uid, gid_t gid)
{
	set_fuse_context();
	return reffs_fs_chown(path, uid, gid);
}

int reffs_fuse_create(const char *path, mode_t mode,
		      struct fuse_file_info __attribute__((unused)) * info)
{
	set_fuse_context();
	return reffs_fs_create(path, mode);
}

int reffs_fuse_fallocate(const char *path, int mode, off_t offset, off_t len,
			 struct fuse_file_info __attribute__((unused)) * info)
{
	set_fuse_context();
	return reffs_fs_fallocate(path, mode, offset, len);
}

int reffs_fuse_getattr(const char *path, struct stat *st)
{
	set_fuse_context();
	return reffs_fs_getattr(path, st);
}

int reffs_fuse_mkdir(const char *path, mode_t mode)
{
	set_fuse_context();
	return reffs_fs_mkdir(path, mode);
}

int reffs_fuse_mknod(const char *path, mode_t mode, dev_t rdev)
{
	set_fuse_context();
	return reffs_fs_mknod(path, mode, rdev);
}

int reffs_fuse_read(const char *path, char *buffer, size_t size, off_t offset,
		    struct fuse_file_info __attribute__((unused)) * fi)
{
	set_fuse_context();
	return reffs_fs_read(path, buffer, size, offset);
}

static void fill_stat(struct stat *st, struct inode *inode)
{
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
}

int reffs_fuse_readdir(const char *path, void *buffer, fuse_fill_dir_t filler,
		       off_t offset,
		       struct fuse_file_info __attribute__((unused)) * fi)
{
	struct name_match *nm;
	struct reffs_dirent *rd;
	struct stat st;

	int ret;
	off_t cur = 2;

	TRACE("path=%s offset=%lu", path, offset);

	// For now expose find_matching_directory_entry because how to handle filler()?
	ret = find_matching_directory_entry(&nm, path, LAST_COMPONENT_IS_MATCH);
	if (ret)
		return ret;

	if (offset == 0) {
		fill_stat(&st, nm->nm_dirent->rd_inode);
		filler(buffer, ".", &st, ++offset);
		if (nm->nm_dirent->rd_parent)
			fill_stat(&st, nm->nm_dirent->rd_parent->rd_inode);
		else
			fill_stat(&st, nm->nm_dirent->rd_inode);
		filler(buffer, "..", &st, ++offset);
	}

	rcu_read_lock();
	cds_list_for_each_entry_rcu(rd, &nm->nm_dirent->rd_inode->i_children,
				    rd_siblings) {
		if (cur++ < offset)
			continue;
		fill_stat(&st, rd->rd_inode);
		ret = filler(buffer, rd->rd_name, &st, cur);
		if (ret)
			break;
	}
	rcu_read_unlock();

	name_match_free(nm);

	return 0;
}

int reffs_fuse_readlink(const char *path, char *buffer, size_t len)
{
	set_fuse_context();
	return reffs_fs_readlink(path, buffer, len);
}

int reffs_fuse_rename(const char *src_path, const char *dst_path)
{
	set_fuse_context();
	return reffs_fs_rename(src_path, dst_path);
}

int reffs_fuse_rmdir(const char *path)
{
	set_fuse_context();
	return reffs_fs_rmdir(path);
}

int reffs_fuse_symlink(const char *path, const char *new_path)
{
	set_fuse_context();
	return reffs_fs_symlink(path, new_path);
}

int reffs_fuse_write(const char *path, const char *buffer, size_t size,
		     off_t offset,
		     struct fuse_file_info __attribute__((unused)) * info)
{
	set_fuse_context();
	return reffs_fs_write(path, buffer, size, offset);
}

int reffs_fuse_unlink(const char *path)
{
	set_fuse_context();
	return reffs_fs_unlink(path);
}

int reffs_fuse_link(const char *oldpath, const char *newpath)
{
	set_fuse_context();
	return reffs_fs_link(oldpath, newpath);
}

int reffs_fuse_truncate(const char *path, off_t len)
{
	TRACE("path=%s len=%lu", path, len);

	return 0;
}

int reffs_fuse_open(const char *path,
		    struct fuse_file_info __attribute__((unused)) * info)
{
	TRACE("path=%s", path);

	return 0;
}

int reffs_fuse_flush(const char *path,
		     struct fuse_file_info __attribute__((unused)) * info)
{
	TRACE("path=%s", path);

	return 0;
}

int reffs_fuse_release(const char *path,
		       struct fuse_file_info __attribute__((unused)) * info)
{
	TRACE("path=%s", path);

	return 0;
}

int reffs_fuse_fsync(const char *path, int datasync,
		     struct fuse_file_info __attribute__((unused)) * info)
{
	TRACE("path=%s datasync=%d", path, datasync);

	return 0;
}

int reffs_fuse_statfs(const char *path, struct statvfs *buf)
{
	TRACE("path=%s", path);

	return 0;
}

int reffs_fuse_utimensat(const char *path, const struct timespec times[2])
{
	set_fuse_context();
	return reffs_fs_utimensat(path, times);
}
