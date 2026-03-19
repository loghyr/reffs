/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <rpc/types.h>
#include <rpc/xdr.h>
#include <rpc/auth.h>
#include <rpc/auth_unix.h>

#include "reffs/rcu.h"
#include "reffs/cmp.h"
#include "reffs/data_block.h"
#include "reffs/dirent.h"
#include "reffs/identity.h"
#include "reffs/inode.h"
#include "reffs/rcu.h"
#include "reffs/super_block.h"
#include "reffs/types.h"
#include "reffs/vfs.h"

struct timespec;

static int vfs_check_sticky_bit(struct inode *dir, struct inode *file,
				struct authunix_parms *ap)
{
	if (!ap || ap->aup_uid == 0)
		return 0;

	if (!(dir->i_mode & S_ISVTX))
		return 0;

	if ((uint32_t)ap->aup_uid == file->i_uid ||
	    (uint32_t)ap->aup_uid == dir->i_uid)
		return 0;

	return -EACCES;
}

static struct reffs_dirent *vfs_dir_dirent(struct inode *dir)
{
	if (dir->i_dirent)
		return dir->i_dirent;
	return dir->i_sb->sb_dirent;
}

/*
 * Locking order:
 * 1. Lower inode ID attr_mutex
 * 2. Higher inode ID attr_mutex
 * 3. Lower inode ID parent->rd_rwlock
 * 4. Higher inode ID parent->rd_rwlock
 */
static void vfs_lock_dirs(struct inode *d1, struct inode *d2)
{
	struct reffs_dirent *de1 = vfs_dir_dirent(d1);
	struct reffs_dirent *de2 = d2 ? vfs_dir_dirent(d2) : NULL;

	if (!de2 || d1 == d2 || de1 == de2) {
		pthread_mutex_lock(&d1->i_attr_mutex);
		if (d2 && d1 != d2)
			pthread_mutex_lock(&d2->i_attr_mutex);
		pthread_rwlock_wrlock(&de1->rd_rwlock);
		return;
	}

	if (d1->i_ino < d2->i_ino) {
		pthread_mutex_lock(&d1->i_attr_mutex);
		pthread_mutex_lock(&d2->i_attr_mutex);
		pthread_rwlock_wrlock(&de1->rd_rwlock);
		pthread_rwlock_wrlock(&de2->rd_rwlock);
	} else {
		pthread_mutex_lock(&d2->i_attr_mutex);
		pthread_mutex_lock(&d1->i_attr_mutex);
		pthread_rwlock_wrlock(&de2->rd_rwlock);
		pthread_rwlock_wrlock(&de1->rd_rwlock);
	}
}

static void vfs_unlock_dirs(struct inode *d1, struct inode *d2)
{
	struct reffs_dirent *de1 = vfs_dir_dirent(d1);
	struct reffs_dirent *de2 = d2 ? vfs_dir_dirent(d2) : NULL;

	if (!de2 || d1 == d2 || de1 == de2) {
		pthread_rwlock_unlock(&de1->rd_rwlock);
		if (d2 && d1 != d2)
			pthread_mutex_unlock(&d2->i_attr_mutex);
		pthread_mutex_unlock(&d1->i_attr_mutex);
		return;
	}

	if (d1->i_ino < d2->i_ino) {
		pthread_rwlock_unlock(&de2->rd_rwlock);
		pthread_rwlock_unlock(&de1->rd_rwlock);
		pthread_mutex_unlock(&d2->i_attr_mutex);
		pthread_mutex_unlock(&d1->i_attr_mutex);
	} else {
		pthread_rwlock_unlock(&de1->rd_rwlock);
		pthread_rwlock_unlock(&de2->rd_rwlock);
		pthread_mutex_unlock(&d1->i_attr_mutex);
		pthread_mutex_unlock(&d2->i_attr_mutex);
	}
}

static int rtc_cmp(const char *s1, const char *s2)
{
	return reffs_text_case_cmp_of(reffs_case_get())(s1, s2);
}

int vfs_is_subdir(struct inode *child, struct inode *maybe_parent)
{
	/*
	 * TODO: ancestor inodes are not held with active refs during this walk.
	 * Under extreme LRU pressure a directory ancestor could be evicted
	 * between hops.  For now the weak-pointer walk is sufficient.
	 */
	struct inode *curr = child;
	while (curr) {
		if (curr == maybe_parent)
			return 1;
		if (curr->i_dirent && curr->i_dirent->rd_parent)
			curr = curr->i_dirent->rd_parent->rd_inode;
		else
			curr = NULL;
	}
	return 0;
}

/* Internal helpers that assume locks are held */

static int vfs_remove_common_locked(struct inode *dir, const char *name,
				    struct authunix_parms *ap, bool is_dir)
{
	struct reffs_dirent *rd = NULL;
	struct inode *rd_inode = NULL;
	struct reffs_dirent *de_dir = vfs_dir_dirent(dir);
	int ret = 0;
	enum reffs_text_case rtc = reffs_case_get();

	ret = inode_access_check(dir, ap, W_OK);
	if (ret)
		return ret;

	rd = dirent_find(de_dir, rtc, (char *)name);
	if (!rd) {
		return -ENOENT;
	}

	rd_inode = dirent_ensure_inode(rd);
	if (!rd_inode) {
		ret = -ENOENT;
		goto out;
	}

	if (is_dir && !S_ISDIR(rd_inode->i_mode)) {
		ret = -ENOTDIR;
		goto out;
	}
	if (!is_dir && S_ISDIR(rd_inode->i_mode)) {
		ret = -EISDIR;
		goto out;
	}

	ret = vfs_check_sticky_bit(dir, rd_inode, ap);
	if (ret)
		goto out;

	if (is_dir && !cds_list_empty(&rd->rd_children)) {
		ret = -ENOTEMPTY;
		goto out;
	}

	inode_update_times_now(rd_inode, REFFS_INODE_UPDATE_CTIME);
	dirent_parent_release(rd, reffs_life_action_death);

	inode_update_times_now(dir, REFFS_INODE_UPDATE_CTIME |
					    REFFS_INODE_UPDATE_MTIME);

out:
	inode_active_put(rd_inode);
	dirent_put(rd);
	return ret;
}

static int vfs_create_common_locked(struct inode *dir, const char *name,
				    mode_t mode, struct authunix_parms *ap,
				    dev_t rdev, uint32_t type,
				    struct inode **new_inode)
{
	struct reffs_dirent *rd = NULL;
	struct inode *inode = NULL;
	struct super_block *sb = dir->i_sb;
	struct reffs_dirent *de_dir = vfs_dir_dirent(dir);
	int ret = 0;
	enum reffs_text_case rtc = reffs_case_get();

	if (type == S_IFREG && (mode & S_IFMT) == S_IFDIR) {
		return -EISDIR;
	}

	ret = inode_access_check(dir, ap, W_OK);
	if (ret)
		return ret;

	if (strlen(name) > REFFS_MAX_NAME) {
		return -ENAMETOOLONG;
	}

	rd = dirent_find(de_dir, rtc, (char *)name);
	if (rd) {
		dirent_put(rd);
		return -EEXIST;
	}

	rd = dirent_alloc(de_dir, (char *)name, reffs_life_action_birth,
			  (type == S_IFDIR));
	if (!rd) {
		return -ENOMEM;
	}

	inode = inode_alloc(sb, __atomic_add_fetch(&sb->sb_next_ino, 1,
						   __ATOMIC_RELAXED));
	if (!inode) {
		dirent_parent_release(rd, reffs_life_action_death);
		dirent_put(rd);
		return -ENOMEM;
	}

	inode->i_uid = ap->aup_uid;
	inode->i_gid = ap->aup_gid;
	inode->i_mode = type | (mode & ~S_IFMT);
	inode->i_nlink = (type == S_IFDIR) ? 2 : 1;
	inode->i_size = (type == S_IFDIR) ? sb->sb_block_size : 0;
	inode->i_used = (type == S_IFDIR) ? 1 : 0;
	clock_gettime(CLOCK_REALTIME, &inode->i_mtime);
	inode->i_atime = inode->i_mtime;
	inode->i_ctime = inode->i_mtime;
	inode->i_btime = inode->i_mtime;

	if (type == S_IFCHR || type == S_IFBLK) {
		inode->i_dev_major = major(rdev);
		inode->i_dev_minor = minor(rdev);
	}

	dirent_attach_inode(rd, inode);
	rd->rd_ino = inode->i_ino;
	inode->i_parent_ino =
		dir->i_ino; /* needed by inode_reconstruct_path_to_root on reload */

	if (type == S_IFDIR) {
		__atomic_add_fetch(&sb->sb_bytes_used, sb->sb_block_size,
				   __ATOMIC_RELAXED);
	}

	inode_update_times_now(dir, REFFS_INODE_UPDATE_CTIME |
					    REFFS_INODE_UPDATE_MTIME);

	inode_sync_to_disk(inode);
	dirent_sync_to_disk(de_dir);

	/*
	 * Transfer the active ref from inode_alloc to the caller.
	 * If the caller doesn't want it, drop it here; otherwise hand it
	 * off via *new_inode so the NFS handler holds i_active > 0 until
	 * the reply is sent (preventing LRU eviction between CREATE and
	 * the client's subsequent WRITE/GETATTR).
	 */
	if (new_inode) {
		*new_inode = inode; /* transfer active ref to caller */
	} else {
		inode_active_put(inode);
	}

	dirent_put(rd);
	return 0;
}

static int vfs_rename_locked(struct inode *old_dir, const char *old_name,
			     struct inode *new_dir, const char *new_name,
			     struct authunix_parms *ap)
{
	struct inode *inode_src_file = NULL;
	struct inode *inode_dst_file = NULL;
	struct reffs_dirent *rd_src = NULL;
	struct reffs_dirent *rd_dst = NULL;
	struct reffs_dirent *de_old_dir = vfs_dir_dirent(old_dir);
	struct reffs_dirent *de_new_dir = vfs_dir_dirent(new_dir);
	int ret = 0;
	enum reffs_text_case rtc = reffs_case_get();

	rd_src = dirent_find(de_old_dir, rtc, (char *)old_name);
	if (!rd_src) {
		return -ENOENT;
	}
	inode_src_file = dirent_ensure_inode(rd_src);
	if (!inode_src_file) {
		dirent_put(rd_src);
		return -ENOENT;
	}

	rd_dst = dirent_find(de_new_dir, rtc, (char *)new_name);
	if (rd_dst) {
		inode_dst_file = dirent_ensure_inode(rd_dst);
		if (!inode_dst_file) {
			/* dst dirent exists but inode evicted — treat as gone */
			dirent_put(rd_dst);
			rd_dst = NULL;
		}
	}

	if (rd_src == rd_dst) {
		ret = 0;
		goto out;
	}

	if (S_ISDIR(inode_src_file->i_mode)) {
		if (vfs_is_subdir(new_dir, inode_src_file)) {
			ret = -EINVAL;
			goto out;
		}
	}

	ret = inode_access_check(old_dir, ap, W_OK);
	if (ret)
		goto out;

	ret = inode_access_check(new_dir, ap, W_OK);
	if (ret)
		goto out;

	ret = vfs_check_sticky_bit(old_dir, inode_src_file, ap);
	if (ret)
		goto out;

	if (inode_dst_file) {
		ret = vfs_check_sticky_bit(new_dir, inode_dst_file, ap);
		if (ret)
			goto out;

		if (S_ISDIR(inode_src_file->i_mode) !=
		    S_ISDIR(inode_dst_file->i_mode)) {
			ret = S_ISDIR(inode_src_file->i_mode) ? -ENOTDIR :
								-EISDIR;
			goto out;
		}

		if (S_ISDIR(inode_dst_file->i_mode) &&
		    !cds_list_empty(&rd_dst->rd_children)) {
			ret = -ENOTEMPTY;
			goto out;
		}
	}

	if (S_ISDIR(inode_src_file->i_mode) && old_dir != new_dir) {
		ret = inode_access_check(inode_src_file, ap, W_OK);
		if (ret)
			goto out;
	}

	char *new_name_copy = strdup(new_name);
	if (!new_name_copy) {
		ret = -ENOMEM;
		goto out;
	}

	/* Logic for actual moving/renaming */
	if (old_dir == new_dir) {
		if (rd_dst) {
			inode_update_times_now(inode_dst_file,
					       REFFS_INODE_UPDATE_CTIME);
			dirent_parent_release(rd_dst, reffs_life_action_death);
		}
		rcu_read_lock();
		char *old_name_ptr =
			rcu_xchg_pointer(&rd_src->rd_name, new_name_copy);
		reffs_string_release(old_name_ptr);
		rcu_read_unlock();
	} else {
		if (rd_dst) {
			inode_update_times_now(inode_dst_file,
					       REFFS_INODE_UPDATE_CTIME);
			dirent_parent_release(rd_dst, reffs_life_action_death);
		}
		dirent_parent_release(rd_src, S_ISDIR(inode_src_file->i_mode) ?
						      reffs_life_action_move :
						      reffs_life_action_update);
		rd_src->rd_parent = de_new_dir;
		rcu_read_lock();
		char *old_name_ptr =
			rcu_xchg_pointer(&rd_src->rd_name, new_name_copy);
		reffs_string_release(old_name_ptr);
		rcu_read_unlock();
		dirent_parent_attach(rd_src, de_new_dir,
				     S_ISDIR(inode_src_file->i_mode) ?
					     reffs_life_action_move :
					     reffs_life_action_update,
				     S_ISDIR(inode_src_file->i_mode));
	}

	inode_update_times_now(old_dir, REFFS_INODE_UPDATE_CTIME |
						REFFS_INODE_UPDATE_MTIME);
	if (old_dir != new_dir) {
		inode_update_times_now(new_dir,
				       REFFS_INODE_UPDATE_CTIME |
					       REFFS_INODE_UPDATE_MTIME);
	}

out:
	inode_active_put(inode_src_file);
	inode_active_put(inode_dst_file);
	dirent_put(rd_src);
	dirent_put(rd_dst);
	return ret;
}

/* Public API */

int vfs_rename(struct inode *old_dir, const char *old_name,
	       struct inode *new_dir, const char *new_name,
	       struct authunix_parms *ap,
	       struct timespec *old_before, struct timespec *old_after,
	       struct timespec *new_before, struct timespec *new_after)
{
	if (old_dir->i_sb != new_dir->i_sb)
		return -EXDEV;

	if (old_dir == new_dir && !rtc_cmp(old_name, new_name)) {
		if (old_before)
			*old_before = old_dir->i_ctime;
		if (old_after)
			*old_after = old_dir->i_ctime;
		if (new_before)
			*new_before = new_dir->i_ctime;
		if (new_after)
			*new_after = new_dir->i_ctime;
		return 0;
	}

	if (!strcmp(new_name, ".") || !strcmp(new_name, "..")) {
		return -ENOTEMPTY;
	}

	int ret;
	vfs_lock_dirs(old_dir, new_dir);
	if (old_before)
		*old_before = old_dir->i_ctime;
	if (new_before)
		*new_before = new_dir->i_ctime;
	ret = vfs_rename_locked(old_dir, old_name, new_dir, new_name, ap);
	if (old_after)
		*old_after = old_dir->i_ctime;
	if (new_after)
		*new_after = new_dir->i_ctime;
	vfs_unlock_dirs(old_dir, new_dir);
	return ret;
}

int vfs_remove(struct inode *dir, const char *name, struct authunix_parms *ap,
	       struct timespec *dir_before, struct timespec *dir_after)
{
	int ret;
	vfs_lock_dirs(dir, NULL);
	if (dir_before)
		*dir_before = dir->i_ctime;
	ret = vfs_remove_common_locked(dir, name, ap, false);
	if (dir_after)
		*dir_after = dir->i_ctime;
	vfs_unlock_dirs(dir, NULL);
	return ret;
}

int vfs_rmdir(struct inode *dir, const char *name, struct authunix_parms *ap,
	       struct timespec *dir_before, struct timespec *dir_after)
{
	int ret;
	vfs_lock_dirs(dir, NULL);
	if (dir_before)
		*dir_before = dir->i_ctime;
	ret = vfs_remove_common_locked(dir, name, ap, true);
	if (dir_after)
		*dir_after = dir->i_ctime;
	vfs_unlock_dirs(dir, NULL);
	return ret;
}

int vfs_setattr(struct inode *inode, struct reffs_sattr *sattr,
		struct authunix_parms *ap)
{
	int ret = 0;
	uint64_t flags = 0;
	bool user_in_current_group =
		is_user_in_group(ap ? ap->aup_uid : 0, inode->i_gid, ap);

	pthread_mutex_lock(&inode->i_attr_mutex);

	/* If no auth params or root user, allow all changes */
	if (ap && ap->aup_uid != 0) {
		/* Non-root user permissions checks */
		if (sattr->uid_set && (uint32_t)ap->aup_uid != inode->i_uid) {
			ret = -EPERM;
			goto out_unlock;
		}

		/* Changing owner to someone else requires root */
		if (sattr->uid_set && sattr->uid != (uid_t)-1 &&
		    sattr->uid != inode->i_uid) {
			ret = -EPERM;
			goto out_unlock;
		}

		/* Changing group requires being the owner (or root) */
		if (sattr->gid_set && (uint32_t)ap->aup_uid != inode->i_uid) {
			ret = -EPERM;
			goto out_unlock;
		}

		/* Changing group to a real value requires membership */
		if (sattr->gid_set && sattr->gid != (gid_t)-1) {
			gid_t target_gid = sattr->gid;
			bool user_in_target_group =
				is_user_in_group(ap->aup_uid, target_gid, ap);

			if (!user_in_target_group) {
				ret = -EPERM;
				goto out_unlock;
			}
		}

		if (sattr->mode_set && (uint32_t)ap->aup_uid != inode->i_uid) {
			ret = -EPERM;
			goto out_unlock;
		}

		if (sattr->atime_set && (uint32_t)ap->aup_uid != inode->i_uid) {
			if (!sattr->atime_now) {
				ret = -EPERM;
				goto out_unlock;
			}

			ret = inode_access_check(inode, ap, W_OK);
			if (ret) {
				goto out_unlock;
			}
		}

		if (sattr->mtime_set && (uint32_t)ap->aup_uid != inode->i_uid) {
			if (!sattr->mtime_now) {
				ret = -EPERM;
				goto out_unlock;
			}

			ret = inode_access_check(inode, ap, W_OK);
			if (ret) {
				goto out_unlock;
			}
		}
	}

	/* Handle file size changes */
	if (sattr->size_set) {
		if (S_ISDIR(inode->i_mode)) {
			ret = -EISDIR;
			goto out_unlock;
		}

		pthread_rwlock_wrlock(&inode->i_db_rwlock);
		size_t old_size = inode->i_size;
		size_t new_size = sattr->size;

		if (!inode->i_db) {
			if (new_size > 0) {
				inode->i_db = data_block_alloc(inode, NULL,
							       new_size, 0);
				if (!inode->i_db) {
					pthread_rwlock_unlock(
						&inode->i_db_rwlock);
					ret = -ENOSPC;
					goto out_unlock;
				}
			}
			inode->i_size = new_size;
		} else {
			ssize_t res = data_block_resize(inode->i_db, new_size);
			if (res < 0) {
				pthread_rwlock_unlock(&inode->i_db_rwlock);
				ret = -ENOSPC;
				goto out_unlock;
			}
			inode->i_size = res;
		}

		inode->i_used =
			inode->i_size / inode->i_sb->sb_block_size +
			(inode->i_size % inode->i_sb->sb_block_size ? 1 : 0);

		size_t old_used;
		size_t new_used;
		do {
			__atomic_load(&inode->i_sb->sb_bytes_used, &old_used,
				      __ATOMIC_RELAXED);
			if ((size_t)inode->i_size > old_size) {
				new_used = old_used +
					   ((size_t)inode->i_size - old_size);
			} else if (old_size > (size_t)inode->i_size) {
				size_t diff = old_size - (size_t)inode->i_size;
				if (old_used >= diff)
					new_used = old_used - diff;
				else
					new_used = 0;
			} else {
				new_used = old_used;
			}
		} while (!__atomic_compare_exchange(
			&inode->i_sb->sb_bytes_used, &old_used, &new_used,
			false, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED));

		pthread_rwlock_unlock(&inode->i_db_rwlock);
		flags |= REFFS_INODE_UPDATE_CTIME | REFFS_INODE_UPDATE_MTIME;
	}

	if (sattr->mode_set) {
		uint16_t file_type = inode->i_mode & S_IFMT;
		uint16_t new_mode = sattr->mode & 07777;

		if ((new_mode & S_ISGID) && S_ISREG(inode->i_mode) && ap &&
		    ap->aup_uid != 0 && !user_in_current_group) {
			new_mode &= ~S_ISGID;
		}

		inode->i_mode = new_mode | file_type;
		flags |= REFFS_INODE_UPDATE_CTIME | REFFS_INODE_UPDATE_MTIME;
	}

	bool is_uid_change = sattr->uid_set && sattr->uid != (uid_t)-1 &&
			     sattr->uid != inode->i_uid;
	bool is_gid_change = sattr->gid_set && sattr->gid != (gid_t)-1 &&
			     sattr->gid != inode->i_gid;

	if (sattr->uid_set && sattr->uid != (uid_t)-1) {
		inode->i_uid = sattr->uid;
		flags |= REFFS_INODE_UPDATE_CTIME | REFFS_INODE_UPDATE_MTIME;
	}
	if (sattr->gid_set && sattr->gid != (gid_t)-1) {
		inode->i_gid = sattr->gid;
		flags |= REFFS_INODE_UPDATE_CTIME | REFFS_INODE_UPDATE_MTIME;
	}

	if (is_uid_change || is_gid_change) {
		inode->i_mode &= ~(S_ISUID | S_ISGID);
	}

	if (sattr->atime_set) {
		if (sattr->atime_now)
			clock_gettime(CLOCK_REALTIME, &inode->i_atime);
		else
			inode->i_atime = sattr->atime;
		flags |= REFFS_INODE_UPDATE_CTIME;
	}
	if (sattr->mtime_set) {
		if (sattr->mtime_now)
			clock_gettime(CLOCK_REALTIME, &inode->i_mtime);
		else
			inode->i_mtime = sattr->mtime;
		flags |= REFFS_INODE_UPDATE_CTIME;
	}

	if (flags)
		inode_update_times_now(inode, flags);

	inode_sync_to_disk(inode);

out_unlock:
	pthread_mutex_unlock(&inode->i_attr_mutex);
	return ret;
}

static int vfs_exclusive_create_locked(struct inode *dir, const char *name,
				       struct timespec *verf,
				       struct authunix_parms *ap,
				       struct inode **new_inode)
{
	struct reffs_dirent *rd = NULL;
	struct inode *inode = NULL;
	struct super_block *sb = dir->i_sb;
	struct reffs_dirent *de_dir = vfs_dir_dirent(dir);
	int ret = 0;
	enum reffs_text_case rtc = reffs_case_get();

	ret = inode_access_check(dir, ap, W_OK);
	if (ret)
		return ret;

	if (strlen(name) > REFFS_MAX_NAME) {
		return -ENAMETOOLONG;
	}

	rd = dirent_find(de_dir, rtc, (char *)name);
	if (rd) {
		// POSIX: For exclusive, if it exists, check verifier
		struct inode *existing = dirent_ensure_inode(rd);
		if (existing) {
			bool match =
				(existing->i_ctime.tv_sec == verf->tv_sec &&
				 existing->i_ctime.tv_nsec == verf->tv_nsec);
			if (match && new_inode)
				*new_inode = inode_get(existing);
			inode_active_put(existing);
			dirent_put(rd);
			return match ? 0 : -EEXIST;
		}
		/* inode evicted — treat as no verifier match */
		dirent_put(rd);
		return -EEXIST;
	}

	rd = dirent_alloc(de_dir, (char *)name, reffs_life_action_birth, false);
	if (!rd) {
		return -ENOMEM;
	}

	inode = inode_alloc(sb, __atomic_add_fetch(&sb->sb_next_ino, 1,
						   __ATOMIC_RELAXED));
	if (!inode) {
		dirent_parent_release(rd, reffs_life_action_death);
		dirent_put(rd);
		return -ENOMEM;
	}

	inode->i_uid = ap->aup_uid;
	inode->i_gid = ap->aup_gid;
	inode->i_mode = S_IFREG | (dir->i_mode & 0777);
	inode->i_nlink = 1;
	inode->i_size = 0;
	inode->i_used = 0;
	inode->i_ctime = *verf; // The verifier!
	inode->i_atime = inode->i_ctime;
	inode->i_mtime = inode->i_ctime;
	inode->i_btime = inode->i_ctime;

	dirent_attach_inode(rd, inode);
	rd->rd_ino = inode->i_ino;
	inode->i_parent_ino =
		dir->i_ino; /* needed by inode_reconstruct_path_to_root on reload */

	inode_sync_to_disk(inode);
	dirent_sync_to_disk(de_dir);

	if (new_inode) {
		*new_inode = inode; /* transfer active ref to caller */
	} else {
		inode_active_put(inode);
	}

	dirent_put(rd);
	return 0;
}

int vfs_exclusive_create(struct inode *dir, const char *name,
			 struct timespec *verf, struct authunix_parms *ap,
			 struct inode **new_inode)
{
	int ret;
	vfs_lock_dirs(dir, NULL);
	ret = vfs_exclusive_create_locked(dir, name, verf, ap, new_inode);
	vfs_unlock_dirs(dir, NULL);
	return ret;
}

int vfs_mkdir(struct inode *dir, const char *name, mode_t mode,
	      struct authunix_parms *ap, struct inode **new_inode,
	      struct timespec *dir_before, struct timespec *dir_after)
{
	int ret;
	vfs_lock_dirs(dir, NULL);
	if (dir_before)
		*dir_before = dir->i_ctime;
	ret = vfs_create_common_locked(dir, name, mode, ap, 0, S_IFDIR,
				       new_inode);
	if (dir_after)
		*dir_after = dir->i_ctime;
	vfs_unlock_dirs(dir, NULL);
	return ret;
}

int vfs_create(struct inode *dir, const char *name, mode_t mode,
	       struct authunix_parms *ap, struct inode **new_inode)
{
	int ret;
	if (S_ISDIR(mode))
		return -EISDIR;
	vfs_lock_dirs(dir, NULL);
	ret = vfs_create_common_locked(dir, name, mode, ap, 0, S_IFREG,
				       new_inode);
	vfs_unlock_dirs(dir, NULL);
	return ret;
}

int vfs_symlink(struct inode *dir, const char *name, const char *target,
		struct authunix_parms *ap, struct inode **new_inode,
		struct timespec *dir_before, struct timespec *dir_after)
{
	struct inode *inode = NULL;
	int ret;

	if (strlen(target) > REFFS_MAX_PATH) {
		return -ENAMETOOLONG;
	}

	vfs_lock_dirs(dir, NULL);
	if (dir_before)
		*dir_before = dir->i_ctime;
	ret = vfs_create_common_locked(dir, name, 0777, ap, 0, S_IFLNK, &inode);
	if (ret == 0) {
		inode->i_symlink = strdup(target);
		if (!inode->i_symlink) {
			vfs_remove_common_locked(dir, name, ap, false);
			inode_active_put(inode);
			ret = -ENOMEM;
		} else {
			inode->i_size = strlen(target);
			if (new_inode) {
				*new_inode =
					inode; /* transfer active ref to caller */
			} else {
				inode_active_put(inode);
			}
		}
	}
	if (dir_after)
		*dir_after = dir->i_ctime;
	vfs_unlock_dirs(dir, NULL);
	return ret;
}

int vfs_mknod(struct inode *dir, const char *name, mode_t mode, dev_t rdev,
	      struct authunix_parms *ap, struct inode **new_inode,
	      struct timespec *dir_before, struct timespec *dir_after)
{
	int ret;
	vfs_lock_dirs(dir, NULL);
	if (dir_before)
		*dir_before = dir->i_ctime;
	ret = vfs_create_common_locked(dir, name, mode, ap, rdev, mode & S_IFMT,
				       new_inode);
	if (dir_after)
		*dir_after = dir->i_ctime;
	vfs_unlock_dirs(dir, NULL);
	return ret;
}

int vfs_link(struct inode *inode, struct inode *dir, const char *name,
	     struct authunix_parms *ap)
{
	struct reffs_dirent *rd = NULL;
	struct reffs_dirent *de_dir = vfs_dir_dirent(dir);
	int ret = 0;
	enum reffs_text_case rtc = reffs_case_get();

	if (inode->i_sb != dir->i_sb)
		return -EXDEV;

	if (S_ISDIR(inode->i_mode))
		return -EPERM;

	/*
	 * Lock ordering: attr_mutex low-ino first, then rd_rwlock.
	 * inode is a regular file — passing it to vfs_lock_dirs would call
	 * vfs_dir_dirent() on it, returning the file's own i_dirent and
	 * write-locking rd_rwlock on a non-directory dirent, which is wrong
	 * and deadlocks with concurrent readers.  Open-code the locking:
	 * take both attr_mutexes in ino order, then the directory rd_rwlock.
	 */
	if (inode->i_ino < dir->i_ino) {
		pthread_mutex_lock(&inode->i_attr_mutex);
		pthread_mutex_lock(&dir->i_attr_mutex);
	} else {
		pthread_mutex_lock(&dir->i_attr_mutex);
		pthread_mutex_lock(&inode->i_attr_mutex);
	}
	pthread_rwlock_wrlock(&de_dir->rd_rwlock);

	ret = inode_access_check(dir, ap, W_OK);
	if (ret)
		goto out_unlock;

	rd = dirent_find(de_dir, rtc, (char *)name);
	if (rd) {
		ret = -EEXIST;
		goto out_unlock;
	}

	rd = dirent_alloc(de_dir, (char *)name, reffs_life_action_birth, false);
	if (!rd) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	/*
	 * Wire up rd_inode / i_dirent via dirent_attach_inode so that
	 * inode_release() can null rd_inode before call_rcu.
	 * Note: reffs does not support hard links at the protocol level, so
	 * i_dirent will point to the most-recently-linked dirent; that is
	 * sufficient for the UAF fix since the eviction path nulls rd_inode
	 * through whichever dirent i_dirent points to.
	 */
	dirent_attach_inode(rd, inode);
	rd->rd_ino = inode->i_ino;
	inode->i_parent_ino = dir->i_ino;
	__atomic_fetch_add(&inode->i_nlink, 1, __ATOMIC_RELAXED);

	inode_update_times_now(inode, REFFS_INODE_UPDATE_CTIME);
	inode_update_times_now(dir, REFFS_INODE_UPDATE_CTIME |
					    REFFS_INODE_UPDATE_MTIME);

	inode_sync_to_disk(inode);
	dirent_sync_to_disk(de_dir);

out_unlock:
	pthread_rwlock_unlock(&de_dir->rd_rwlock);
	pthread_mutex_unlock(&inode->i_attr_mutex);
	pthread_mutex_unlock(&dir->i_attr_mutex);
	dirent_put(rd);
	return ret;
}
