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

struct buffer_free {
	char *buffer;
	struct rcu_head rcu;
};

static void buffer_free_rcu(struct rcu_head *rcu)
{
	struct buffer_free *bf = caa_container_of(rcu, struct buffer_free, rcu);

	free(bf->buffer);
	free(bf);
}

static void buffer_free_schedule(char *buffer)
{
	struct buffer_free *bf = malloc(sizeof(*bf));
	if (!bf)
		free(buffer);

	bf->buffer = buffer;
	call_rcu(&bf->rcu, buffer_free_rcu);
}

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
	char *new;
	char *old;

	rcu_read_lock();
	if (size + offset > db->db_size) {
		new = calloc(offset + size, sizeof(*new));
		if (!new) {
			LOG("Could not alloc a db's storage");
			free(db);
			rcu_read_unlock();
			return -ENOSPC;
		}

		if (db->db_buffer)
			memcpy(new, db->db_buffer, db->db_size);
		old = rcu_xchg_pointer(&db->db_buffer, new);
		buffer_free_schedule(old);
		db->db_size = offset + size;
	}
	rcu_read_unlock();

	memcpy(db->db_buffer + offset, buffer, size);

	return size;
}

size_t data_block_resize(struct data_block *db, size_t size)
{
	char *new;
	char *old;

	if (size == db->db_size)
		return size;

	rcu_read_lock();
	if (size > db->db_size) {
		new = calloc(size, sizeof(*new));
		if (!new) {
			LOG("Could not alloc a db's storage");
			free(db);
			rcu_read_unlock();
			return -ENOSPC;
		}

		memcpy(new, db->db_buffer, db->db_size);
		old = rcu_xchg_pointer(&db->db_buffer, new);
		buffer_free_schedule(old);
	} else if (size == 0) {
		old = rcu_xchg_pointer(&db->db_buffer, NULL);
		buffer_free_schedule(old);
	} else {
		new = calloc(size, sizeof(*new));
		if (!new) {
			LOG("Could not alloc a db's storage");
			free(db);
			rcu_read_unlock();
			return -ENOSPC;
		}

		memcpy(new, db->db_buffer, size);
		old = rcu_xchg_pointer(&db->db_buffer, new);
		buffer_free_schedule(old);
	}

	db->db_size = size;
	rcu_read_unlock();

	return db->db_size;
}
