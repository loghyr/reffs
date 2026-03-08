/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include "reffs/backend.h"
#include "reffs/super_block.h"
#include "reffs/inode.h"
#include "reffs/data_block.h"
#include "reffs/log.h"

struct posix_sb_private {
	char *sb_dir;
};

struct posix_db_private {
	int db_fd;
	char *db_path;
};

static int posix_sb_alloc(struct super_block *sb, const char *backend_path)
{
	struct posix_sb_private *priv = calloc(1, sizeof(*priv));
	if (!priv)
		return ENOMEM;

	char sb_dir[1024];
	snprintf(sb_dir, sizeof(sb_dir), "%s/sb_%lu", backend_path, sb->sb_id);
	if (mkdir(sb_dir, 0755) < 0 && errno != EEXIST) {
		LOG("mkdir %s failed: %s", sb_dir, strerror(errno));
		free(priv);
		return errno;
	}
	priv->sb_dir = strdup(sb_dir);

	struct statvfs sv;
	if (statvfs(backend_path, &sv) == 0) {
		sb->sb_block_size = sv.f_bsize;
		sb->sb_bytes_max = (size_t)sv.f_blocks * sv.f_frsize;
		sb->sb_inodes_max = sv.f_files;
	} else {
		sb->sb_block_size = 4096;
	}

	sb->sb_storage_private = priv;
	return 0;
}

static void posix_sb_free(struct super_block *sb)
{
	struct posix_sb_private *priv = sb->sb_storage_private;
	if (priv) {
		free(priv->sb_dir);
		free(priv);
		sb->sb_storage_private = NULL;
	}
}

static void posix_inode_sync(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct posix_sb_private *sb_priv = sb->sb_storage_private;
	if (!sb_priv)
		return;

	char path[1024];
	char tmp_path[1024];
	snprintf(path, sizeof(path), "%s/ino_%lu.meta", sb_priv->sb_dir,
		 inode->i_ino);
	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

	struct {
		struct reffs_disk_header hdr;
		struct inode_disk id;
	} meta;

	meta.hdr.rdh_magic = REFFS_DISK_MAGIC_META;
	meta.hdr.rdh_version = REFFS_DISK_VERSION_1;

	meta.id.id_uid = inode->i_uid;
	meta.id.id_gid = inode->i_gid;
	meta.id.id_nlink = inode->i_nlink;
	meta.id.id_mode = inode->i_mode;
	meta.id.id_size = inode->i_size;
	meta.id.id_atime = inode->i_atime;
	meta.id.id_ctime = inode->i_ctime;
	meta.id.id_mtime = inode->i_mtime;
	meta.id.id_attr_flags = inode->i_attr_flags;

	int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) {
		if (write(fd, &meta, sizeof(meta)) == sizeof(meta)) {
			close(fd);
			if (rename(tmp_path, path) < 0) {
				LOG("rename %s to %s failed: %s", tmp_path,
				    path, strerror(errno));
				unlink(tmp_path);
			}
		} else {
			LOG("Failed to write inode meta to %s", tmp_path);
			close(fd);
			unlink(tmp_path);
		}
	} else {
		LOG("Failed to open %s for sync: %s", tmp_path,
		    strerror(errno));
	}

	if (S_ISLNK(inode->i_mode) && inode->i_symlink) {
		snprintf(path, sizeof(path), "%s/ino_%lu.lnk", sb_priv->sb_dir,
			 inode->i_ino);
		snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
		fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd >= 0) {
			size_t len = strlen(inode->i_symlink);
			if (write(fd, inode->i_symlink, len) == (ssize_t)len) {
				close(fd);
				if (rename(tmp_path, path) < 0) {
					LOG("rename %s to %s failed: %s",
					    tmp_path, path, strerror(errno));
					unlink(tmp_path);
				}
			} else {
				LOG("Failed to write symlink target to %s",
				    tmp_path);
				close(fd);
				unlink(tmp_path);
			}
		}
	}
}

static int posix_db_alloc(struct data_block *db, struct inode *inode,
			  const char *buffer, size_t size, off_t offset)
{
	struct super_block *sb = inode->i_sb;
	struct posix_sb_private *sb_priv = sb->sb_storage_private;
	if (!sb_priv)
		return EINVAL;

	struct posix_db_private *priv = calloc(1, sizeof(*priv));
	if (!priv)
		return ENOMEM;

	char path[1024];
	snprintf(path, sizeof(path), "%s/ino_%lu.dat", sb_priv->sb_dir,
		 inode->i_ino);
	priv->db_path = strdup(path);

	priv->db_fd = open(path, O_RDWR | O_CREAT, 0644);
	if (priv->db_fd < 0) {
		LOG("Failed to open %s: %s", path, strerror(errno));
		free(priv->db_path);
		free(priv);
		return errno;
	}

	if (size == 0) {
		struct stat st;
		if (fstat(priv->db_fd, &st) == 0) {
			db->db_size = st.st_size;
		}
	}

	if (buffer && size > 0) {
		if (pwrite(priv->db_fd, buffer, size, offset) !=
		    (ssize_t)size) {
			LOG("Initial write to %s failed", path);
		}
	}

	db->db_storage_private = priv;
	return 0;
}

static void posix_db_free(struct data_block *db)
{
	struct posix_db_private *priv = db->db_storage_private;
	if (priv) {
		if (priv->db_fd >= 0)
			close(priv->db_fd);
		free(priv->db_path);
		free(priv);
		db->db_storage_private = NULL;
	}
}

static void posix_db_release_resources(struct data_block *db)
{
	struct posix_db_private *priv = db->db_storage_private;
	if (priv && priv->db_fd >= 0) {
		close(priv->db_fd);
		priv->db_fd = -1;
	}
}

static ssize_t posix_db_read(struct data_block *db, char *buffer, size_t size,
			     off_t offset)
{
	struct posix_db_private *priv = db->db_storage_private;
	if (!priv || priv->db_fd < 0)
		return -EBADF;

	ssize_t ret = pread(priv->db_fd, buffer, size, offset);
	if (ret < 0) {
		LOG("pread from %s failed: %s", priv->db_path, strerror(errno));
		return -errno;
	}
	return ret;
}

static ssize_t posix_db_write(struct data_block *db, const char *buffer,
			      size_t size, off_t offset)
{
	struct posix_db_private *priv = db->db_storage_private;
	if (!priv || priv->db_fd < 0)
		return -EBADF;

	size_t new_total = (size_t)offset + size;
	if (new_total > db->db_size) {
		if (ftruncate(priv->db_fd, new_total) < 0) {
			LOG("ftruncate of %s failed: %s", priv->db_path,
			    strerror(errno));
			return -errno;
		}
		db->db_size = new_total;
	}

	ssize_t ret = pwrite(priv->db_fd, buffer, size, offset);
	if (ret < 0) {
		LOG("pwrite to %s failed: %s", priv->db_path, strerror(errno));
		return -errno;
	}
	return ret;
}

static ssize_t posix_db_resize(struct data_block *db, size_t size)
{
	struct posix_db_private *priv = db->db_storage_private;
	if (!priv || priv->db_fd < 0)
		return -EBADF;

	if (ftruncate(priv->db_fd, size) < 0) {
		LOG("ftruncate of %s failed: %s", priv->db_path,
		    strerror(errno));
		return -errno;
	}
	db->db_size = size;
	return (ssize_t)size;
}

static size_t posix_db_get_size(struct data_block *db)
{
	struct posix_db_private *priv = db->db_storage_private;
	struct stat st;
	if (priv && priv->db_fd >= 0 && fstat(priv->db_fd, &st) == 0) {
		return st.st_size;
	}
	return db->db_size;
}

static void posix_dir_sync(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct posix_sb_private *sb_priv = sb->sb_storage_private;
	if (!sb_priv || !inode->i_parent)
		return;

	struct reffs_dirent *parent = inode->i_parent;

	char path[1024];
	char tmp_path[1024];
	snprintf(path, sizeof(path), "%s/ino_%lu.dir", sb_priv->sb_dir,
		 inode->i_ino);
	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

	int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0) {
		LOG("Failed to open directory file %s: %s", tmp_path,
		    strerror(errno));
		return;
	}

	struct reffs_disk_header hdr = {
		.rdh_magic = REFFS_DISK_MAGIC_DIR,
		.rdh_version = REFFS_DISK_VERSION_1,
	};
	if (write(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
		LOG("write dir header failed");
		close(fd);
		unlink(tmp_path);
		return;
	}

	if (write(fd, &parent->rd_cookie_next,
		  sizeof(parent->rd_cookie_next)) !=
	    sizeof(parent->rd_cookie_next)) {
		LOG("write cookie_next failed");
		close(fd);
		unlink(tmp_path);
		return;
	}

	struct reffs_dirent *rd;
	rcu_read_lock();
	cds_list_for_each_entry_rcu(rd, &inode->i_children, rd_siblings) {
		uint64_t cookie = rd->rd_cookie;
		uint64_t ino = rd->rd_inode ? rd->rd_inode->i_ino : 0;
		uint16_t name_len = strlen(rd->rd_name);

		if (write(fd, &cookie, sizeof(cookie)) != sizeof(cookie))
			LOG("write cookie failed");
		if (write(fd, &ino, sizeof(ino)) != sizeof(ino))
			LOG("write ino failed");
		if (write(fd, &name_len, sizeof(name_len)) != sizeof(name_len))
			LOG("write name_len failed");
		if (write(fd, rd->rd_name, name_len) != (ssize_t)name_len)
			LOG("write name failed");
	}
	rcu_read_unlock();

	close(fd);
	if (rename(tmp_path, path) < 0) {
		LOG("rename %s to %s failed: %s", tmp_path, path,
		    strerror(errno));
		unlink(tmp_path);
	}
}

static int posix_inode_alloc(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct posix_sb_private *sb_priv = sb->sb_storage_private;
	if (!sb_priv)
		return EINVAL;

	struct inode_disk id;
	struct reffs_disk_header hdr;
	char path[1024];

	snprintf(path, sizeof(path), "%s/ino_%lu.meta", sb_priv->sb_dir,
		 inode->i_ino);

	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		if (errno == ENOENT)
			return 0; // New inode
		return errno;
	}

	if (read(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
		close(fd);
		return EIO;
	}

	if (hdr.rdh_magic != REFFS_DISK_MAGIC_META) {
		LOG("Invalid magic 0x%x for %s", hdr.rdh_magic, path);
		close(fd);
		return EINVAL;
	}

	if (hdr.rdh_version != REFFS_DISK_VERSION_1) {
		LOG("Unsupported meta version %u for %s", hdr.rdh_version,
		    path);
		close(fd);
		return EOPNOTSUPP;
	}

	if (read(fd, &id, sizeof(id)) != sizeof(id)) {
		close(fd);
		return EIO;
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
	inode->i_attr_flags = id.id_attr_flags;

	if (inode->i_ino >= sb->sb_next_ino)
		sb->sb_next_ino = inode->i_ino + 1;

	// Also check if data file exists
	snprintf(path, sizeof(path), "%s/ino_%lu.dat", sb_priv->sb_dir,
		 inode->i_ino);
	if (!inode->i_db && access(path, F_OK) == 0) {
		inode->i_db = data_block_alloc(inode, NULL, 0, 0);
		if (inode->i_db) {
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
	snprintf(path, sizeof(path), "%s/ino_%lu.lnk", sb_priv->sb_dir,
		 inode->i_ino);
	if (!inode->i_symlink && access(path, F_OK) == 0) {
		fd = open(path, O_RDONLY);
		if (fd >= 0) {
			struct stat st;
			if (fstat(fd, &st) == 0) {
				inode->i_symlink = malloc(st.st_size + 1);
				if (inode->i_symlink) {
					if (read(fd, inode->i_symlink,
						 st.st_size) ==
					    (ssize_t)st.st_size) {
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

static void posix_inode_free(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct posix_sb_private *sb_priv = sb->sb_storage_private;
	if (!sb_priv)
		return;

	char path[1024];

	snprintf(path, sizeof(path), "%s/ino_%lu.meta", sb_priv->sb_dir,
		 inode->i_ino);
	unlink(path);

	snprintf(path, sizeof(path), "%s/ino_%lu.dat", sb_priv->sb_dir,
		 inode->i_ino);
	unlink(path);

	snprintf(path, sizeof(path), "%s/ino_%lu.dir", sb_priv->sb_dir,
		 inode->i_ino);
	unlink(path);

	snprintf(path, sizeof(path), "%s/ino_%lu.lnk", sb_priv->sb_dir,
		 inode->i_ino);
	unlink(path);
}

const struct reffs_storage_ops posix_storage_ops = {
	.type = REFFS_STORAGE_POSIX,
	.name = "posix",
	.sb_alloc = posix_sb_alloc,
	.sb_free = posix_sb_free,
	.inode_alloc = posix_inode_alloc,
	.inode_free = posix_inode_free,
	.inode_sync = posix_inode_sync,
	.db_alloc = posix_db_alloc,
	.db_free = posix_db_free,
	.db_release_resources = posix_db_release_resources,
	.db_read = posix_db_read,
	.db_write = posix_db_write,
	.db_resize = posix_db_resize,
	.db_get_size = posix_db_get_size,
	.dir_sync = posix_dir_sync,
};
