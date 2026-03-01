/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <urcu.h>
#include <urcu/rculist.h>
#include <urcu/ref.h>
#include "reffs/data_block.h"
#include "reffs/inode.h"
#include "reffs/super_block.h"
#include "reffs/log.h"
#include "reffs/rcu.h"

static void data_block_free_rcu(struct rcu_head *rcu)
{
	struct data_block *db =
		caa_container_of(rcu, struct data_block, db_rcu);

	if (db->db_type == REFFS_STORAGE_RAM) {
		free(db->u.ram.db_buffer);
	} else if (db->db_type == REFFS_STORAGE_POSIX) {
		if (db->u.posix.db_fd >= 0)
			close(db->u.posix.db_fd);
		free(db->u.posix.db_path);
	}
	free(db);
}

static void data_block_release(struct urcu_ref *ref)
{
	struct data_block *db =
		caa_container_of(ref, struct data_block, db_ref);

	call_rcu(&db->db_rcu, data_block_free_rcu);
}

struct data_block *data_block_alloc(struct inode *inode, const char *buffer,
				    size_t size, off_t offset)
{
	struct data_block *db;
	struct super_block *sb = inode->i_sb;

	db = calloc(1, sizeof(*db));
	if (!db) {
		LOG("Could not alloc a db");
		return NULL;
	}

	db->db_type = sb->sb_storage_type;
	db->db_size = offset + size;

	if (db->db_type == REFFS_STORAGE_RAM) {
		db->u.ram.db_buffer = calloc(db->db_size, 1);
		if (!db->u.ram.db_buffer) {
			LOG("Could not alloc a db's storage");
			free(db);
			return NULL;
		}
		if (buffer)
			memcpy(db->u.ram.db_buffer + offset, buffer, size);
	} else if (db->db_type == REFFS_STORAGE_POSIX) {
		char path[1024];
		snprintf(path, sizeof(path), "%s/sb_%lu_ino_%lu.dat",
			 sb->sb_backend_path ? sb->sb_backend_path : ".",
			 sb->sb_id, inode->i_ino);

		db->u.posix.db_path = strdup(path);
		db->u.posix.db_fd =
			open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
		if (db->u.posix.db_fd < 0) {
			LOG("Could not open/create backend file %s: %s", path,
			    strerror(errno));
			free(db->u.posix.db_path);
			free(db);
			return NULL;
		}

		if (db->db_size > 0) {
			if (ftruncate(db->u.posix.db_fd, db->db_size) < 0) {
				LOG("Could not truncate backend file %s: %s",
				    path, strerror(errno));
				close(db->u.posix.db_fd);
				free(db->u.posix.db_path);
				free(db);
				return NULL;
			}
			if (buffer) {
				if (pwrite(db->u.posix.db_fd, buffer, size,
					   offset) < 0) {
					LOG("Could not write to backend file %s: %s",
					    path, strerror(errno));
					close(db->u.posix.db_fd);
					free(db->u.posix.db_path);
					free(db);
					return NULL;
				}
			}
		}
	}

	pthread_mutex_init(&db->db_lock, NULL);
	urcu_ref_init(&db->db_ref);

	return db;
}

struct data_block *data_block_get(struct data_block *db)
{
	if (!db)
		return NULL;

	if (!urcu_ref_get_unless_zero(&db->db_ref))
		return NULL;

	return db;
}

void data_block_put(struct data_block *db)
{
	if (!db)
		return;

	urcu_ref_put(&db->db_ref, data_block_release);
}

size_t data_block_read(struct data_block *db, char *buffer, size_t size,
		       off_t offset)
{
	size_t read_len = 0;

	if ((size_t)offset >= db->db_size)
		return 0;

	if (size + offset > db->db_size)
		read_len = db->db_size - offset;
	else
		read_len = size;

	if (db->db_type == REFFS_STORAGE_RAM) {
		memcpy(buffer, db->u.ram.db_buffer + offset, read_len);
	} else if (db->db_type == REFFS_STORAGE_POSIX) {
		ssize_t ret =
			pread(db->u.posix.db_fd, buffer, read_len, offset);
		if (ret < 0) {
			LOG("pread from %s failed: %s", db->u.posix.db_path,
			    strerror(errno));
			return -errno;
		}
		read_len = ret;
	}

	return read_len;
}

size_t data_block_write(struct data_block *db, const char *buffer, size_t size,
			off_t offset)
{
	if (!db)
		return -EINVAL;

	size_t new_total = (size_t)offset + size;

	if (new_total > db->db_size) {
		if (db->db_type == REFFS_STORAGE_RAM) {
			char *new_buffer =
				realloc(db->u.ram.db_buffer, new_total);
			if (!new_buffer) {
				LOG("Could not realloc a db's storage");
				return -ENOSPC;
			}
			db->u.ram.db_buffer = new_buffer;
		} else if (db->db_type == REFFS_STORAGE_POSIX) {
			if (ftruncate(db->u.posix.db_fd, new_total) < 0) {
				LOG("ftruncate of %s failed: %s",
				    db->u.posix.db_path, strerror(errno));
				return -errno;
			}
		}
		db->db_size = new_total;
	}

	if (db->db_type == REFFS_STORAGE_RAM) {
		memcpy(db->u.ram.db_buffer + offset, buffer, size);
	} else if (db->db_type == REFFS_STORAGE_POSIX) {
		ssize_t ret =
			pwrite(db->u.posix.db_fd, buffer, size, (off_t)offset);
		if (ret < 0) {
			LOG("pwrite to %s failed: %s", db->u.posix.db_path,
			    strerror(errno));
			return -errno;
		}
		size = ret;
	}

	return size;
}

size_t data_block_resize(struct data_block *db, size_t size)
{
	if (!db || size == db->db_size)
		return db ? db->db_size : 0;

	if (db->db_type == REFFS_STORAGE_RAM) {
		char *new_buffer = realloc(db->u.ram.db_buffer, size);
		if (size > 0 && !new_buffer) {
			LOG("Could not realloc a db's storage");
			return -ENOSPC;
		}
		db->u.ram.db_buffer = new_buffer;
	} else if (db->db_type == REFFS_STORAGE_POSIX) {
		if (ftruncate(db->u.posix.db_fd, size) < 0) {
			LOG("ftruncate of %s failed: %s", db->u.posix.db_path,
			    strerror(errno));
			return -errno;
		}
	}

	db->db_size = size;

	return size;
}
