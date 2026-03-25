/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
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
#include <limits.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include "reffs/backend.h"
#include "reffs/super_block.h"
#include "reffs/inode.h"
#include "reffs/layout_segment.h"
#include "reffs/data_block.h"
#include "reffs/log.h"
#include "reffs/trace/fs.h"

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

	char sb_dir[PATH_MAX];
	snprintf(sb_dir, sizeof(sb_dir), "%s/sb_%lu", backend_path, sb->sb_id);
	if (mkdir(sb_dir, 0755) < 0 && errno != EEXIST) {
		LOG("mkdir %s failed: %s", sb_dir, strerror(errno));
		free(priv);
		return errno;
	}
	priv->sb_dir = strdup(sb_dir);
	if (!priv->sb_dir) {
		free(priv);
		return ENOMEM;
	}

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

	char path[PATH_MAX];
	char tmp_path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/ino_%lu.meta", sb_priv->sb_dir,
		 inode->i_ino);
	/*
	 * Include the thread ID in the tmp filename to avoid races when
	 * multiple threads concurrently sync the same inode.  Each thread
	 * gets its own tmp file; the final rename() is atomic and the last
	 * writer wins (all contain the same inode snapshot so that is fine).
	 */
	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%lu", path,
		 (unsigned long)pthread_self());

	trace_fs_inode(inode, __func__, __LINE__);

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
	meta.id.id_btime = inode->i_btime;
	meta.id.id_attr_flags = inode->i_attr_flags;
	meta.id.id_parent_ino = inode->i_parent_ino;
	meta.id.id_dev_major = inode->i_dev_major;
	meta.id.id_dev_minor = inode->i_dev_minor;

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
		snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%lu", path,
			 (unsigned long)pthread_self());
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

	/* Sync layout segments if present (MDS mode). */
	if (inode->i_layout_segments &&
	    inode->i_layout_segments->lss_count > 0) {
		struct layout_segments *lss = inode->i_layout_segments;

		snprintf(path, sizeof(path), "%s/ino_%lu.layouts",
			 sb_priv->sb_dir, inode->i_ino);
		snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%lu", path,
			 (unsigned long)pthread_self());
		fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd >= 0) {
			struct reffs_disk_header hdr = {
				.rdh_magic = REFFS_DISK_MAGIC_LAY,
				.rdh_version = REFFS_DISK_VERSION_1,
			};
			bool ok = true;

			ok = ok && write(fd, &hdr, sizeof(hdr)) == sizeof(hdr);
			ok = ok && write(fd, &lss->lss_count,
					 sizeof(lss->lss_count)) ==
					   sizeof(lss->lss_count);

			for (uint32_t s = 0; ok && s < lss->lss_count; s++) {
				struct layout_segment *seg = &lss->lss_segs[s];
				struct layout_segment_disk lsd = {
					.ls_offset = seg->ls_offset,
					.ls_length = seg->ls_length,
					.ls_stripe_unit = seg->ls_stripe_unit,
					.ls_k = seg->ls_k,
					.ls_m = seg->ls_m,
					.ls_nfiles = seg->ls_nfiles,
					.ls_layout_type = seg->ls_layout_type,
				};

				ok = ok && write(fd, &lsd, sizeof(lsd)) ==
						   sizeof(lsd);

				for (uint32_t f = 0; ok && f < seg->ls_nfiles;
				     f++) {
					struct layout_data_file *ldf =
						&seg->ls_files[f];
					struct layout_data_file_disk ldfd = {
						.ldf_dstore_id =
							ldf->ldf_dstore_id,
						.ldf_fh_len = ldf->ldf_fh_len,
						.ldf_size = ldf->ldf_size,
						.ldf_atime = ldf->ldf_atime,
						.ldf_mtime = ldf->ldf_mtime,
						.ldf_ctime = ldf->ldf_ctime,
						.ldf_uid = ldf->ldf_uid,
						.ldf_gid = ldf->ldf_gid,
						.ldf_mode = ldf->ldf_mode,
					};

					memcpy(ldfd.ldf_fh, ldf->ldf_fh,
					       ldf->ldf_fh_len);
					ok = ok &&
					     write(fd, &ldfd, sizeof(ldfd)) ==
						     sizeof(ldfd);
				}
			}

			close(fd);
			if (ok) {
				if (rename(tmp_path, path) < 0) {
					LOG("rename %s failed: %s", tmp_path,
					    strerror(errno));
					unlink(tmp_path);
				}
			} else {
				LOG("Failed to write layouts to %s", tmp_path);
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
		return -EINVAL;

	struct posix_db_private *priv = calloc(1, sizeof(*priv));
	if (!priv)
		return -ENOMEM;

	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/ino_%lu.dat", sb_priv->sb_dir,
		 inode->i_ino);
	priv->db_path = strdup(path);
	if (!priv->db_path) {
		free(priv);
		return -ENOMEM;
	}

	priv->db_fd = open(path, O_RDWR | O_CREAT, 0644);
	if (priv->db_fd < 0) {
		LOG("Failed to open %s: %s", path, strerror(errno));
		free(priv->db_path);
		free(priv);
		return -errno;
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

static int posix_db_get_fd(struct data_block *db)
{
	struct posix_db_private *priv = db->db_storage_private;
	return (priv && priv->db_fd >= 0) ? priv->db_fd : -1;
}

static void posix_dir_sync(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct posix_sb_private *sb_priv = sb->sb_storage_private;
	if (!sb_priv || !inode->i_dirent)
		return;

	trace_fs_inode(inode, __func__, __LINE__);
	struct reffs_dirent *self = inode->i_dirent;

	char path[PATH_MAX];
	char tmp_path[PATH_MAX];
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

	if (write(fd, &self->rd_cookie_next, sizeof(self->rd_cookie_next)) !=
	    sizeof(self->rd_cookie_next)) {
		LOG("write cookie_next failed");
		close(fd);
		unlink(tmp_path);
		return;
	}

	struct reffs_dirent *rd;
	rcu_read_lock();
	cds_list_for_each_entry_rcu(rd, &inode->i_dirent->rd_children,
				    rd_siblings) {
		uint64_t cookie = rd->rd_cookie;
		/*
		 * Use rd_ino (authoritative) rather than rd_inode->i_ino
		 * so we can write even if rd_inode was evicted.
		 */
		uint64_t ino = rd->rd_ino;
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

/*
 * inode_load_from_disk -- previously posix_inode_alloc.
 *
 * Called by the storage backend's inode_alloc hook after the inode has
 * already been inserted into sb_inodes by inode_alloc().  Its job is to
 * populate the inode fields from the on-disk .meta file.
 *
 * If no .meta file exists this is a brand-new inode; return 0 and let the
 * caller set fields (mode, nlink, etc.) as needed.
 */
static int inode_load_from_disk(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct posix_sb_private *sb_priv = sb->sb_storage_private;

	if (!sb_priv)
		return -EINVAL;

	trace_fs_inode(inode, __func__, __LINE__);

	struct inode_disk id;
	struct reffs_disk_header hdr;
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "%s/ino_%lu.meta", sb_priv->sb_dir,
		 inode->i_ino);

	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		if (errno == ENOENT)
			return 0; /* New inode, nothing to load */
		return -errno;
	}

	if (read(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
		close(fd);
		return -EIO;
	}

	if (hdr.rdh_magic != REFFS_DISK_MAGIC_META) {
		LOG("Invalid magic 0x%x for %s", hdr.rdh_magic, path);
		close(fd);
		return -EINVAL;
	}

	if (hdr.rdh_version != REFFS_DISK_VERSION_1) {
		LOG("Unsupported meta version %u for %s", hdr.rdh_version,
		    path);
		close(fd);
		return -EOPNOTSUPP;
	}

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
	inode->i_btime = id.id_btime;
	inode->i_attr_flags = id.id_attr_flags;
	inode->i_parent_ino = id.id_parent_ino;
	inode->i_dev_major = id.id_dev_major;
	inode->i_dev_minor = id.id_dev_minor;

	if (inode->i_ino >= sb->sb_next_ino)
		sb->sb_next_ino = inode->i_ino + 1;

	/* Re-open the data file if it exists. */
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

	/* Re-load symlink target if it exists. */
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

	/* Re-load layout segments if they exist (MDS mode). */
	snprintf(path, sizeof(path), "%s/ino_%lu.layouts", sb_priv->sb_dir,
		 inode->i_ino);
	if (!inode->i_layout_segments && access(path, F_OK) == 0) {
		fd = open(path, O_RDONLY);
		if (fd >= 0) {
			struct reffs_disk_header hdr;
			uint32_t count;
			bool ok = true;

			ok = ok && read(fd, &hdr, sizeof(hdr)) == sizeof(hdr);
			ok = ok && hdr.rdh_magic == REFFS_DISK_MAGIC_LAY;
			ok = ok && hdr.rdh_version == REFFS_DISK_VERSION_1;
			ok = ok &&
			     read(fd, &count, sizeof(count)) == sizeof(count);

			if (ok && count > 0) {
				struct layout_segments *lss =
					layout_segments_alloc();
				if (lss) {
					for (uint32_t s = 0; ok && s < count;
					     s++) {
						struct layout_segment_disk lsd;

						ok = read(fd, &lsd,
							  sizeof(lsd)) ==
						     sizeof(lsd);
						if (!ok)
							break;

						struct layout_data_file *files =
							calloc(lsd.ls_nfiles,
							       sizeof(*files));
						if (!files) {
							ok = false;
							break;
						}

						for (uint32_t f = 0;
						     ok && f < lsd.ls_nfiles;
						     f++) {
							struct layout_data_file_disk
								ldfd;
							ok = read(fd, &ldfd,
								  sizeof(ldfd)) ==
							     sizeof(ldfd);
							if (!ok)
								break;
							files[f].ldf_dstore_id =
								ldfd.ldf_dstore_id;
							files[f].ldf_fh_len =
								ldfd.ldf_fh_len;
							memcpy(files[f].ldf_fh,
							       ldfd.ldf_fh,
							       ldfd.ldf_fh_len);
							files[f].ldf_size =
								ldfd.ldf_size;
							files[f].ldf_atime =
								ldfd.ldf_atime;
							files[f].ldf_mtime =
								ldfd.ldf_mtime;
							files[f].ldf_ctime =
								ldfd.ldf_ctime;
							files[f].ldf_uid =
								ldfd.ldf_uid;
							files[f].ldf_gid =
								ldfd.ldf_gid;
							files[f].ldf_mode =
								ldfd.ldf_mode;
						}

						if (ok) {
							struct layout_segment seg = {
								.ls_offset =
									lsd.ls_offset,
								.ls_length =
									lsd.ls_length,
								.ls_stripe_unit =
									lsd.ls_stripe_unit,
								.ls_k = lsd.ls_k,
								.ls_m = lsd.ls_m,
								.ls_nfiles =
									lsd.ls_nfiles,
								.ls_layout_type =
									lsd.ls_layout_type,
								.ls_files =
									files,
							};

							layout_segments_add(
								lss, &seg);
						} else {
							free(files);
						}
					}

					if (ok)
						inode->i_layout_segments = lss;
					else
						layout_segments_free(lss);
				}
			}
			close(fd);
		}
	}

	trace_fs_inode(inode, __func__, __LINE__);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Directory scan helpers                                               */
/* ------------------------------------------------------------------ */

/*
 * posix_dir_open -- open and validate a .dir file for dir_ino.
 * On success *fd_out is a readable fd positioned just after the header
 * and cookie_next field.  Caller must close(*fd_out).
 * Returns 0, ENOENT (file missing), or another errno.
 */
static int posix_dir_open(struct super_block *sb, uint64_t dir_ino, int *fd_out)
{
	struct posix_sb_private *sb_priv = sb->sb_storage_private;
	if (!sb_priv)
		return -EINVAL;

	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/ino_%lu.dir", sb_priv->sb_dir,
		 dir_ino);

	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return -errno; /* ENOENT if evicted before first dir_sync */

	struct reffs_disk_header hdr;
	if (read(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
		close(fd);
		return -EIO;
	}
	if (hdr.rdh_magic != REFFS_DISK_MAGIC_DIR) {
		LOG("Bad magic 0x%x in %s", hdr.rdh_magic, path);
		close(fd);
		return -EINVAL;
	}
	if (hdr.rdh_version != REFFS_DISK_VERSION_1) {
		LOG("Unsupported dir version %u in %s", hdr.rdh_version, path);
		close(fd);
		return -EOPNOTSUPP;
	}

	/* Skip cookie_next */
	uint64_t cookie_next;
	if (read(fd, &cookie_next, sizeof(cookie_next)) !=
	    sizeof(cookie_next)) {
		close(fd);
		return -EIO;
	}

	*fd_out = fd;
	return 0;
}

/*
 * posix_dir_find_entry_by_ino -- scan dir_ino's .dir file for child_ino.
 *
 * Wire format per entry: uint64_t cookie, uint64_t ino,
 *                        uint16_t name_len, char name[name_len]
 */
static int posix_dir_find_entry_by_ino(struct super_block *sb, uint64_t dir_ino,
				       uint64_t child_ino, char *name_out,
				       size_t name_max, uint64_t *cookie_out)
{
	int fd;
	int err = posix_dir_open(sb, dir_ino, &fd);
	if (err)
		return err;

	err = -ENOENT;
	for (;;) {
		uint64_t cookie, ino;
		uint16_t name_len;

		if (read(fd, &cookie, sizeof(cookie)) != sizeof(cookie))
			break; /* EOF or error */
		if (read(fd, &ino, sizeof(ino)) != sizeof(ino))
			break;
		if (read(fd, &name_len, sizeof(name_len)) != sizeof(name_len))
			break;

		if (name_len > REFFS_MAX_NAME) {
			LOG("corrupt name_len %u in dir ino %lu", name_len,
			    dir_ino);
			err = -EIO;
			break;
		}

		char name_buf[REFFS_MAX_NAME + 1];
		if (read(fd, name_buf, name_len) != name_len)
			break;
		name_buf[name_len] = '\0';

		if (ino == child_ino) {
			size_t copy = name_len < name_max - 1 ? name_len :
								name_max - 1;
			memcpy(name_out, name_buf, copy);
			name_out[copy] = '\0';
			*cookie_out = cookie;
			err = 0;
			break;
		}
	}

	close(fd);
	return err;
}

/*
 * posix_dir_find_entry_by_name -- scan dir_ino's .dir file for 'name'.
 */
static int posix_dir_find_entry_by_name(struct super_block *sb,
					uint64_t dir_ino, const char *name,
					uint64_t *child_ino_out,
					uint64_t *cookie_out)
{
	int fd;
	int err = posix_dir_open(sb, dir_ino, &fd);
	if (err)
		return err;

	size_t target_len = strlen(name);
	err = -ENOENT;

	for (;;) {
		uint64_t cookie, ino;
		uint16_t name_len;

		if (read(fd, &cookie, sizeof(cookie)) != sizeof(cookie))
			break;
		if (read(fd, &ino, sizeof(ino)) != sizeof(ino))
			break;
		if (read(fd, &name_len, sizeof(name_len)) != sizeof(name_len))
			break;

		if (name_len > REFFS_MAX_NAME) {
			LOG("corrupt name_len %u in dir ino %lu", name_len,
			    dir_ino);
			err = -EIO;
			break;
		}

		char name_buf[REFFS_MAX_NAME + 1];
		if (read(fd, name_buf, name_len) != name_len)
			break;
		name_buf[name_len] = '\0';

		if (name_len == target_len &&
		    memcmp(name_buf, name, target_len) == 0) {
			*child_ino_out = ino;
			*cookie_out = cookie;
			err = 0;
			break;
		}
	}

	close(fd);
	return err;
}

static void posix_inode_free(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct posix_sb_private *sb_priv = sb->sb_storage_private;
	if (!sb_priv)
		return;

	trace_fs_inode(inode, __func__, __LINE__);

	char path[PATH_MAX];

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
	.inode_alloc = inode_load_from_disk, /* renamed */
	.inode_free = posix_inode_free,
	.inode_sync = posix_inode_sync,
	.db_alloc = posix_db_alloc,
	.db_free = posix_db_free,
	.db_release_resources = posix_db_release_resources,
	.db_read = posix_db_read,
	.db_write = posix_db_write,
	.db_resize = posix_db_resize,
	.db_get_size = posix_db_get_size,
	.db_get_fd = posix_db_get_fd,
	.dir_sync = posix_dir_sync,
	.dir_find_entry_by_ino = posix_dir_find_entry_by_ino,
	.dir_find_entry_by_name = posix_dir_find_entry_by_name,
};
