/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#include <string.h>
#include <urcu.h>
#include <urcu/rculist.h>
#include <urcu/ref.h>
#include "reffs/data_block.h"
#include "reffs/log.h"

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

	call_rcu(&db->db_rcu, data_block_free_rcu);
}

struct data_block *data_block_alloc(char *text, int len)
{
	struct data_block *db;

	db = calloc(1, sizeof(*db));
	if (!db) {
		LOG("Could not alloc a db");
		return NULL;
	}

	db->db_text = malloc(len);
	if (!db->db_text) {
		LOG("Could not alloc a db's storage");
		free(db);
		return NULL;
	}

	db->db_len = len;
	memcpy(db->db_text, text, len);

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
