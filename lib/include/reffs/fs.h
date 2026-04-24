/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_FS_H
#define _REFFS_FS_H

#include <sys/stat.h>

/*
 * Darwin spells the nanosecond-resolution timespec stat fields with
 * the historical BSD names (st_*timespec); POSIX 2008 (Linux) uses
 * st_*tim.  Alias here rather than silently via configure.ac
 * CPPFLAGS so readers see the platform delta in source.  Every TU
 * that accesses these fields already pulls reffs/fs.h directly or
 * transitively (via reffs/inode.h / fs_test_harness.h).
 */
#ifdef __APPLE__
#define st_atim st_atimespec
#define st_mtim st_mtimespec
#define st_ctim st_ctimespec
#endif

#include "reffs/types.h"
#include "reffs/super_block.h"

struct reffs_dirent;
struct name_match {
	struct reffs_dirent *nm_dirent;
	char *nm_name;
};

#define LAST_COMPONENT_IS_MATCH (true)
#define LAST_COMPONENT_IS_NEW (false)
/*
 * Resolve `path` against a listener-scoped namespace.
 *
 *   listener_id == 0 resolves against the native root SB (pre-proxy
 *   behavior).  listener_id > 0 resolves against the proxy listener's
 *   own root SB (sb_id=1 + sb_listener_id=N), letting callers that
 *   operate on a [[proxy_mds]] namespace share this primitive without
 *   hard-coding the native root.
 */
int find_matching_directory_entry(struct name_match **nm, uint32_t listener_id,
				  const char *path, bool match_end);
void name_match_free(struct name_match *nm);

void reffs_fs_set_storage(enum reffs_storage_type type, const char *path);
enum reffs_storage_type reffs_fs_get_storage_type(void);
char *reffs_fs_get_backend_path(void);

void reffs_fs_recover(struct super_block *sb);
void reffs_fs_for_each_inode(int (*cb)(struct inode *, void *), void *arg);

struct reffs_fs_usage_stats {
	uint64_t total_bytes;
	uint64_t used_bytes;
	uint64_t free_bytes;
	uint64_t total_files;
	uint64_t used_files;
	uint64_t free_files;
};

int reffs_fs_usage(struct reffs_fs_usage_stats *stats);

int reffs_fs_access(const char *path, int mode, uid_t uid, gid_t gid);
int reffs_fs_chmod(const char *path, mode_t mode);
int reffs_fs_chown(const char *path, uid_t uid, gid_t gid);
int reffs_fs_create(const char *path, mode_t mode);
int reffs_fs_fallocate(const char *path, int mode, off_t offset, off_t len);
int reffs_fs_getattr(const char *path, struct stat *st);
int reffs_fs_getattr(const char *path, struct stat *st);
int reffs_fs_link(const char *old_path, const char *new_path);
int reffs_fs_mkdir(const char *path, mode_t mode);
int reffs_fs_mkdir_p(const char *path, mode_t mode);

/*
 * Listener-scoped variants.  listener_id == 0 matches the legacy
 * native behavior; non-zero resolves path components against the
 * proxy listener's own root SB.  The legacy helpers above are thin
 * wrappers (listener_id=0) so existing callers see no change.
 */
int reffs_fs_mkdir_for_listener(uint32_t listener_id, const char *path,
				mode_t mode);
int reffs_fs_mkdir_p_for_listener(uint32_t listener_id, const char *path,
				  mode_t mode);
int reffs_fs_mknod(const char *path, mode_t mode, dev_t rdev);
int reffs_fs_read(const char *path, char *buffer, size_t size, off_t offset);
int reffs_fs_readdir(const char *path, void *buffer, char *filler,
		     off_t offset);
int reffs_fs_readlink(const char *path, char *buffer, size_t len);
int reffs_fs_rename(const char *path, const char *new_path);
int reffs_fs_rmdir(const char *path);
int reffs_fs_symlink(const char *target, const char *linkpath);
int reffs_fs_unlink(const char *path);
int reffs_fs_utimensat(const char *path, const struct timespec times[2]);
int reffs_fs_write(const char *path, const char *buffer, size_t size,
		   off_t offset);

#endif /* _REFFS_FS_H */
