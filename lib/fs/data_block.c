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
#include <unistd.h>
#include <urcu.h>
#include <urcu/rculist.h>
#include <urcu/ref.h>
#include "reffs/data_block.h"
#include "reffs/log.h"
#include "reffs/rcu.h"

static void data_block_free_rcu(struct rcu_head *rcu)
{
	struct data_block *db =
		caa_container_of(rcu, struct data_block, db_rcu);

	free(db->db_buffer);
	free(db);
}

static void data_block_release(struct urcu_ref *ref)
{
	struct data_block *db =
		caa_container_of(ref, struct data_block, db_ref);

	call_rcu(&db->db_rcu, data_block_free_rcu);
}

struct data_block *data_block_alloc(const char *buffer, size_t size,
				    off_t offset)
{
	struct data_block *db;

	db = calloc(1, sizeof(*db));
	if (!db) {
		LOG("Could not alloc a db");
		return NULL;
	}

	db->db_buffer = calloc(offset + size, sizeof(*db->db_buffer));
	if (!db->db_buffer) {
		LOG("Could not alloc a db's storage");
		free(db);
		return NULL;
	}

	db->db_size = offset + size;
	if (buffer)
		memcpy(db->db_buffer + offset, buffer, size);

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
	size_t read = 0;

	if ((size_t)offset > db->db_size)
		return read;

	if (size + offset > db->db_size)
		read = db->db_size - offset;
	else
		read = size;

	memcpy(buffer, db->db_buffer + offset, read);

	return read;
}

size_t data_block_write(struct data_block *db, const char *buffer, size_t size,
			off_t offset)
{
	if (!db)
		return -EINVAL;

	size_t new_total = (size_t)offset + size;

	if (new_total > db->db_size) {
		char *new_buffer = realloc(db->db_buffer, new_total);
		if (!new_buffer) {
			LOG("Could not realloc a db's storage");
			return -ENOSPC;
		}
		db->db_buffer = new_buffer;
		db->db_size = new_total;
	}

	memcpy(db->db_buffer + offset, buffer, size);

	return size;
}

size_t data_block_resize(struct data_block *db, size_t size)
{
	if (!db || size == db->db_size)
		return db ? db->db_size : 0;

	char *new_buffer = realloc(db->db_buffer, size);
	if (size > 0 && !new_buffer) {
		LOG("Could not realloc a db's storage");
		return -ENOSPC;
	}

	db->db_buffer = new_buffer;
	db->db_size = size;

	return size;
}
