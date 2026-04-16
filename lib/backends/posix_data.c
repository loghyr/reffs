/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * POSIX data backend -- bulk file I/O using POSIX files.
 *
 * Data files live at <backend_path>/sb_<id>/ino_<ino>.dat.
 * Paths are computed from public super_block fields (sb_backend_path,
 * sb_id), NOT from sb_storage_private -- this allows composition with
 * any metadata backend.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "reffs/backend.h"
#include "reffs/data_block.h"
#include "reffs/inode.h"
#include "reffs/log.h"
#include "reffs/super_block.h"
#include "reffs/trace/fs.h"

struct posix_data_private {
	int pd_fd;
	char *pd_path;
	pthread_mutex_t pd_reopen_mutex;
};

/*
 * Build the data file path from public sb fields.
 * Returns 0 on success, -ENAMETOOLONG on overflow.
 */
static int posix_data_path(char *buf, size_t bufsz, struct super_block *sb,
			   uint64_t ino)
{
	int n = snprintf(buf, bufsz, "%s/sb_%lu/ino_%lu.dat",
			 sb->sb_backend_path, (unsigned long)sb->sb_id,
			 (unsigned long)ino);
	if (n < 0 || (size_t)n >= bufsz)
		return -ENAMETOOLONG;
	return 0;
}

int posix_data_db_alloc(struct data_block *db, struct inode *inode,
			const char *buffer, size_t size, off_t offset)
{
	struct super_block *sb = inode->i_sb;

	struct posix_data_private *priv = calloc(1, sizeof(*priv));
	if (!priv)
		return -ENOMEM;

	char path[PATH_MAX];
	int ret = posix_data_path(path, sizeof(path), sb, inode->i_ino);
	if (ret) {
		free(priv);
		return ret;
	}

	priv->pd_path = strdup(path);
	if (!priv->pd_path) {
		free(priv);
		return -ENOMEM;
	}

	pthread_mutex_init(&priv->pd_reopen_mutex, NULL);

	/*
	 * size == 0: recovery path -- open existing file and read its size.
	 *            Do NOT truncate: the file already contains data to restore.
	 * size > 0, buffer != NULL: new file creation -- open with O_TRUNC to
	 *            clear stale data from any prior inode that occupied the
	 *            same ino_N.dat file, then write the initial buffer.
	 * size > 0, buffer == NULL: SETATTR truncate-up -- open with O_TRUNC
	 *            to clear stale data, then ftruncate to the target size so
	 *            sparse reads within [0, offset+size) return zeros.
	 */
	int oflags = O_RDWR | O_CREAT;
	if (size > 0)
		oflags |= O_TRUNC;
	priv->pd_fd = open(path, oflags, 0644);
	if (priv->pd_fd < 0) {
		LOG("Failed to open %s: %s", path, strerror(errno));
		pthread_mutex_destroy(&priv->pd_reopen_mutex);
		free(priv->pd_path);
		free(priv);
		return -errno;
	}

	if (size == 0) {
		struct stat st;
		if (fstat(priv->pd_fd, &st) == 0)
			db->db_size = st.st_size;
	}

	if (buffer && size > 0) {
		if (pwrite(priv->pd_fd, buffer, size, offset) !=
		    (ssize_t)size) {
			int saved_errno = errno;

			LOG("Initial write to %s failed: %s", path,
			    strerror(saved_errno));
			close(priv->pd_fd);
			pthread_mutex_destroy(&priv->pd_reopen_mutex);
			free(priv->pd_path);
			free(priv);
			return -saved_errno;
		}
	} else if (!buffer && size > 0) {
		/*
		 * SETATTR truncate-up with no initial data: extend the POSIX
		 * file to the target size so reads within the grown region
		 * return zeros rather than a short read (or eof=false with
		 * data_len=0) when the file is still 0 bytes after O_TRUNC.
		 * Without this, pread on a 0-byte file returns 0 even though
		 * i_size > 0, causing the client to see EIO.
		 */
		off_t file_size = (off_t)offset + (off_t)size;
		if (ftruncate(priv->pd_fd, file_size) < 0) {
			int saved_errno = errno;

			LOG("ftruncate of %s to %lld failed: %s", path,
			    (long long)file_size, strerror(saved_errno));
			close(priv->pd_fd);
			pthread_mutex_destroy(&priv->pd_reopen_mutex);
			free(priv->pd_path);
			free(priv);
			return -saved_errno;
		}
	}

	db->db_storage_private = priv;
	return 0;
}

void posix_data_db_free(struct data_block *db)
{
	struct posix_data_private *priv = db->db_storage_private;
	if (priv) {
		if (priv->pd_fd >= 0)
			close(priv->pd_fd);
		pthread_mutex_destroy(&priv->pd_reopen_mutex);
		free(priv->pd_path);
		free(priv);
		db->db_storage_private = NULL;
	}
}

void posix_data_db_release_resources(struct data_block *db)
{
	struct posix_data_private *priv = db->db_storage_private;
	if (priv && priv->pd_fd >= 0) {
		close(priv->pd_fd);
		priv->pd_fd = -1;
	}
}

/*
 * Re-open the data file if the FD was released by db_release_resources.
 * Called lazily when a read/write/resize needs the FD.
 *
 * Thread-safe: uses pd_reopen_mutex to prevent concurrent readers from
 * both calling open() and leaking an FD.
 */
static int posix_data_db_reopen(struct posix_data_private *priv)
{
	if (priv->pd_fd >= 0)
		return 0;
	if (!priv->pd_path)
		return -EBADF;

	pthread_mutex_lock(&priv->pd_reopen_mutex);
	/* Re-check under lock -- another thread may have reopened. */
	if (priv->pd_fd >= 0) {
		pthread_mutex_unlock(&priv->pd_reopen_mutex);
		return 0;
	}

	int fd = open(priv->pd_path, O_RDWR, 0644);
	if (fd < 0) {
		int err = errno;
		pthread_mutex_unlock(&priv->pd_reopen_mutex);
		LOG("Failed to reopen %s: %s", priv->pd_path, strerror(err));
		return -err;
	}
	priv->pd_fd = fd;
	pthread_mutex_unlock(&priv->pd_reopen_mutex);
	return 0;
}

ssize_t posix_data_db_read(struct data_block *db, char *buffer, size_t size,
			   off_t offset)
{
	struct posix_data_private *priv = db->db_storage_private;
	if (!priv)
		return -EBADF;
	if (priv->pd_fd < 0) {
		int ret = posix_data_db_reopen(priv);
		if (ret)
			return ret;
	}

	ssize_t ret = pread(priv->pd_fd, buffer, size, offset);
	if (ret < 0) {
		LOG("pread from %s failed: %s", priv->pd_path, strerror(errno));
		return -errno;
	}
	return ret;
}

ssize_t posix_data_db_write(struct data_block *db, const char *buffer,
			    size_t size, off_t offset)
{
	struct posix_data_private *priv = db->db_storage_private;
	if (!priv)
		return -EBADF;
	if (priv->pd_fd < 0) {
		int ret = posix_data_db_reopen(priv);
		if (ret)
			return ret;
	}

	size_t new_total = (size_t)offset + size;
	if (new_total > db->db_size) {
		if (ftruncate(priv->pd_fd, new_total) < 0) {
			LOG("ftruncate of %s failed: %s", priv->pd_path,
			    strerror(errno));
			return -errno;
		}
		db->db_size = new_total;
	}

	ssize_t ret = pwrite(priv->pd_fd, buffer, size, offset);
	if (ret < 0) {
		LOG("pwrite to %s failed: %s", priv->pd_path, strerror(errno));
		return -errno;
	}
	return ret;
}

ssize_t posix_data_db_resize(struct data_block *db, size_t size)
{
	struct posix_data_private *priv = db->db_storage_private;
	if (!priv)
		return -EBADF;
	if (priv->pd_fd < 0) {
		int ret = posix_data_db_reopen(priv);
		if (ret)
			return (ssize_t)ret;
	}

	if (ftruncate(priv->pd_fd, size) < 0) {
		LOG("ftruncate of %s failed: %s", priv->pd_path,
		    strerror(errno));
		return -errno;
	}
	db->db_size = size;
	return (ssize_t)size;
}

size_t posix_data_db_get_size(struct data_block *db)
{
	struct posix_data_private *priv = db->db_storage_private;
	struct stat st;
	if (priv) {
		if (priv->pd_fd < 0)
			posix_data_db_reopen(priv);
		if (priv->pd_fd >= 0 && fstat(priv->pd_fd, &st) == 0)
			return st.st_size;
	}
	return db->db_size;
}

int posix_data_db_get_fd(struct data_block *db)
{
	struct posix_data_private *priv = db->db_storage_private;
	if (priv && priv->pd_fd < 0)
		posix_data_db_reopen(priv);
	return (priv && priv->pd_fd >= 0) ? priv->pd_fd : -1;
}

void posix_data_inode_cleanup(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	if (!sb || !sb->sb_backend_path)
		return;

	char path[PATH_MAX];
	if (posix_data_path(path, sizeof(path), sb, inode->i_ino) == 0)
		unlink(path);
}
