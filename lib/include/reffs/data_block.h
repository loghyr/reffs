/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_DATA_BLOCK_H
#define _REFFS_DATA_BLOCK_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <urcu.h>
#include <urcu/ref.h>
#include "reffs/types.h"
#include "reffs/backend.h"

struct data_block {
	struct rcu_head db_rcu;
	struct urcu_ref db_ref;

	pthread_mutex_t db_lock;

	const struct reffs_storage_ops *db_ops;
	void *db_storage_private;

	size_t db_size; /* Length of db_buffer or file size */
};

struct inode;

struct data_block *data_block_alloc(struct inode *inode, const char *buffer,
				    size_t size, off_t offset);
struct data_block *data_block_get(struct data_block *db);
void data_block_put(struct data_block *db);
ssize_t data_block_read(struct data_block *db, char *buffer, size_t size,
			off_t offset);
bool data_block_unhash(struct data_block *db);
ssize_t data_block_write(struct data_block *db, const char *buffer, size_t size,
			 off_t offset);
size_t data_block_resize(struct data_block *db, size_t size);

size_t data_block_get_size(struct data_block *db);

#endif /* _REFFS_DATA_BLOCK_H */
