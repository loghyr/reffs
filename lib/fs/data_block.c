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
#include "reffs/super_block.h"
#include "reffs/inode.h"
#include "reffs/log.h"
#include "reffs/rcu.h"
#include "reffs/backend.h"

static void data_block_free_rcu(struct rcu_head *rcu)
{
	struct data_block *db =
		caa_container_of(rcu, struct data_block, db_rcu);

	free(db);
}

static void data_block_release(struct urcu_ref *ref)
{
	struct data_block *db =
		caa_container_of(ref, struct data_block, db_ref);

	if (db->db_ops && db->db_ops->db_free)
		db->db_ops->db_free(db);

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

	db->db_size = offset + size;
	db->db_ops = sb->sb_ops;

	if (db->db_ops && db->db_ops->db_alloc) {
		int ret = db->db_ops->db_alloc(db, inode, buffer, size, offset);
		if (ret != 0) {
			free(db);
			return NULL;
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

ssize_t data_block_read(struct data_block *db, char *buffer, size_t size,
			off_t offset)
{
	if (!db)
		return -EINVAL;

	if (db->db_ops && db->db_ops->db_read)
		return db->db_ops->db_read(db, buffer, size, offset);

	return -ENOSYS;
}

ssize_t data_block_write(struct data_block *db, const char *buffer, size_t size,
			 off_t offset)
{
	if (!db)
		return -EINVAL;

	if (db->db_ops && db->db_ops->db_write)
		return db->db_ops->db_write(db, buffer, size, offset);

	return -ENOSYS;
}

ssize_t data_block_resize(struct data_block *db, size_t size)
{
	if (!db)
		return 0;

	if (db->db_ops && db->db_ops->db_resize)
		return db->db_ops->db_resize(db, size);

	return db->db_size;
}

size_t data_block_get_size(struct data_block *db)
{
	if (!db)
		return 0;

	if (db->db_ops && db->db_ops->db_get_size)
		return db->db_ops->db_get_size(db);

	return db->db_size;
}
