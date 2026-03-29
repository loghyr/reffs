/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * RAM data backend — in-memory bulk file I/O.
 *
 * All data lives in heap buffers.  No disk persistence.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "reffs/backend.h"
#include "reffs/data_block.h"
#include "reffs/inode.h"
#include "reffs/log.h"

struct ram_data_private {
	char *rd_buffer;
};

int ram_data_db_alloc(struct data_block *db,
		      struct inode *inode __attribute__((unused)),
		      const char *buffer, size_t size, off_t offset)
{
	struct ram_data_private *priv = calloc(1, sizeof(*priv));
	if (!priv)
		return -ENOMEM;

	priv->rd_buffer = calloc(db->db_size, 1);
	if (!priv->rd_buffer) {
		free(priv);
		return -ENOMEM;
	}

	if (buffer)
		memcpy(priv->rd_buffer + offset, buffer, size);

	db->db_storage_private = priv;
	return 0;
}

void ram_data_db_free(struct data_block *db)
{
	struct ram_data_private *priv = db->db_storage_private;
	if (priv) {
		free(priv->rd_buffer);
		free(priv);
		db->db_storage_private = NULL;
	}
}

ssize_t ram_data_db_read(struct data_block *db, char *buffer, size_t size,
			 off_t offset)
{
	struct ram_data_private *priv = db->db_storage_private;
	if (!priv || !priv->rd_buffer)
		return 0;

	size_t read_len;
	if ((size_t)offset >= db->db_size)
		return 0;

	if (size + offset > db->db_size)
		read_len = db->db_size - offset;
	else
		read_len = size;

	memcpy(buffer, priv->rd_buffer + offset, read_len);
	return read_len;
}

ssize_t ram_data_db_write(struct data_block *db, const char *buffer,
			  size_t size, off_t offset)
{
	struct ram_data_private *priv = db->db_storage_private;
	if (!priv)
		return -EINVAL;

	size_t new_total = (size_t)offset + size;
	if (new_total > db->db_size) {
		char *new_buffer = realloc(priv->rd_buffer, new_total);
		if (!new_buffer) {
			LOG("Could not realloc a db's storage");
			return -ENOSPC;
		}
		/* Zero out any gap between old end and new write start */
		if (offset > (off_t)db->db_size)
			memset(new_buffer + db->db_size, 0,
			       offset - db->db_size);
		priv->rd_buffer = new_buffer;
		db->db_size = new_total;
	}

	memcpy(priv->rd_buffer + offset, buffer, size);
	return size;
}

ssize_t ram_data_db_resize(struct data_block *db, size_t size)
{
	struct ram_data_private *priv = db->db_storage_private;
	if (!priv)
		return -EINVAL;

	size_t old_size = db->db_size;
	char *new_buffer = realloc(priv->rd_buffer, size);
	if (size > 0 && !new_buffer) {
		LOG("Could not realloc a db's storage");
		return -ENOSPC;
	}

	/* Zero out any newly allocated space if growing */
	if (size > old_size)
		memset(new_buffer + old_size, 0, size - old_size);

	priv->rd_buffer = new_buffer;
	db->db_size = size;
	return size;
}

size_t ram_data_db_get_size(struct data_block *db)
{
	return db->db_size;
}

int ram_data_db_get_fd(struct data_block *db __attribute__((unused)))
{
	return -1;
}

void ram_data_inode_cleanup(struct inode *inode __attribute__((unused)))
{
	/* RAM backend: nothing on disk to clean up */
}
