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
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include "reffs/backend.h"
#include "reffs/super_block.h"
#include "reffs/inode.h"
#include "reffs/data_block.h"
#include "reffs/log.h"

struct ram_db_private {
	char *db_buffer;
};

static int ram_sb_alloc(struct super_block *sb,
			const char *backend_path __attribute__((unused)))
{
	sb->sb_block_size = 4096;
	sb->sb_bytes_max = SIZE_MAX;
	sb->sb_inodes_max = SIZE_MAX;
	return 0;
}

static int ram_db_alloc(struct data_block *db,
			struct inode *inode __attribute__((unused)),
			const char *buffer, size_t size, off_t offset)
{
	struct ram_db_private *priv = calloc(1, sizeof(*priv));
	if (!priv)
		return ENOMEM;

	priv->db_buffer = calloc(db->db_size, 1);
	if (!priv->db_buffer) {
		free(priv);
		return ENOMEM;
	}

	if (buffer)
		memcpy(priv->db_buffer + offset, buffer, size);

	db->db_storage_private = priv;
	return 0;
}

static void ram_db_free(struct data_block *db)
{
	struct ram_db_private *priv = db->db_storage_private;
	if (priv) {
		free(priv->db_buffer);
		free(priv);
		db->db_storage_private = NULL;
	}
}

static ssize_t ram_db_read(struct data_block *db, char *buffer, size_t size,
			   off_t offset)
{
	struct ram_db_private *priv = db->db_storage_private;
	if (!priv || !priv->db_buffer)
		return 0;

	size_t read_len;
	if ((size_t)offset >= db->db_size)
		return 0;

	if (size + offset > db->db_size)
		read_len = db->db_size - offset;
	else
		read_len = size;

	memcpy(buffer, priv->db_buffer + offset, read_len);
	return read_len;
}

static ssize_t ram_db_write(struct data_block *db, const char *buffer,
			    size_t size, off_t offset)
{
	struct ram_db_private *priv = db->db_storage_private;
	if (!priv)
		return -EINVAL;

	size_t new_total = (size_t)offset + size;
	if (new_total > db->db_size) {
		char *new_buffer = realloc(priv->db_buffer, new_total);
		if (!new_buffer) {
			LOG("Could not realloc a db's storage");
			return -ENOSPC;
		}
		/* Zero out any gap between the old end and the new write start */
		if (offset > (off_t)db->db_size) {
			memset(new_buffer + db->db_size, 0,
			       offset - db->db_size);
		}
		priv->db_buffer = new_buffer;
		db->db_size = new_total;
	}

	memcpy(priv->db_buffer + offset, buffer, size);
	return size;
}

static ssize_t ram_db_resize(struct data_block *db, size_t size)
{
	struct ram_db_private *priv = db->db_storage_private;
	if (!priv)
		return -EINVAL;

	size_t old_size = db->db_size;
	char *new_buffer = realloc(priv->db_buffer, size);
	if (size > 0 && !new_buffer) {
		LOG("Could not realloc a db's storage");
		return -ENOSPC;
	}

	/* Zero out any newly allocated space if growing */
	if (size > old_size) {
		memset(new_buffer + old_size, 0, size - old_size);
	}

	priv->db_buffer = new_buffer;
	db->db_size = size;
	return size;
}

static size_t ram_db_get_size(struct data_block *db)
{
	return db->db_size;
}

static int ram_db_get_fd(struct data_block __attribute__((unused)) * db)
{
	return -1;
}

const struct reffs_storage_ops ram_storage_ops = {
	.type = REFFS_STORAGE_RAM,
	.name = "ram",
	.sb_alloc = ram_sb_alloc,
	.db_alloc = ram_db_alloc,
	.db_free = ram_db_free,
	.db_read = ram_db_read,
	.db_write = ram_db_write,
	.db_resize = ram_db_resize,
	.db_get_size = ram_db_get_size,
	.db_get_fd = ram_db_get_fd,
};
