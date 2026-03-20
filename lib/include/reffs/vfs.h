/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_VFS_H
#define _REFFS_VFS_H

#include <rpc/auth_unix.h>
#include "reffs/inode.h"

/**
 * vfs_rename - Rename a file or directory using inodes.
 * @old_dir: Inode of the source directory.
 * @old_name: Name of the entry in the source directory.
 * @new_dir: Inode of the destination directory.
 * @new_name: Name of the entry in the destination directory.
 * @ap: Authentication parameters for permission checks.
 *
 * This is the core POSIX-enforcing rename operation, shared by all
 * protocol frontends (NFS, FUSE).
 *
 * Returns 0 on success, or a positive errno value (consistent with
 * the project's internal style, though some layers map these to NFS errors).
 */
int vfs_rename(struct inode *old_dir, const char *old_name,
	       struct inode *new_dir, const char *new_name,
	       struct authunix_parms *ap, struct timespec *old_before,
	       struct timespec *old_after, struct timespec *new_before,
	       struct timespec *new_after);

int vfs_remove(struct inode *dir, const char *name, struct authunix_parms *ap,
	       struct timespec *dir_before, struct timespec *dir_after);
int vfs_rmdir(struct inode *dir, const char *name, struct authunix_parms *ap,
	      struct timespec *dir_before, struct timespec *dir_after);

struct reffs_sattr {
	mode_t mode;
	bool mode_set;
	uid_t uid;
	bool uid_set;
	gid_t gid;
	bool gid_set;
	uint64_t size;
	bool size_set;
	struct timespec atime;
	bool atime_set;
	bool atime_now;
	struct timespec mtime;
	bool mtime_set;
	bool mtime_now;
};

int vfs_setattr(struct inode *inode, struct reffs_sattr *sattr,
		struct authunix_parms *ap);

int vfs_mkdir(struct inode *dir, const char *name, mode_t mode,
	      struct authunix_parms *ap, struct inode **new_inode,
	      struct timespec *dir_before, struct timespec *dir_after);
int vfs_create(struct inode *dir, const char *name, mode_t mode,
	       struct authunix_parms *ap, struct inode **new_inode,
	       struct timespec *dir_before, struct timespec *dir_after);
int vfs_exclusive_create(struct inode *dir, const char *name,
			 struct timespec *verf, struct authunix_parms *ap,
			 struct inode **new_inode);
int vfs_symlink(struct inode *dir, const char *name, const char *target,
		struct authunix_parms *ap, struct inode **new_inode,
		struct timespec *dir_before, struct timespec *dir_after);
int vfs_mknod(struct inode *dir, const char *name, mode_t mode, dev_t rdev,
	      struct authunix_parms *ap, struct inode **new_inode,
	      struct timespec *dir_before, struct timespec *dir_after);

int vfs_link(struct inode *inode, struct inode *dir, const char *name,
	     struct authunix_parms *ap);

/**
 * vfs_is_subdir - Check if an inode is a subdirectory (at any depth) of another.
 * @child: The potential subdirectory to check.
 * @parent: The potential parent directory.
 *
 * Returns 1 if child is a subdirectory of parent, 0 otherwise.
 */
int vfs_is_subdir(struct inode *child, struct inode *parent);

#endif /* _REFFS_VFS_H */
