/*
 * SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_DATA_BLOCK_H
#define _REFFS_DATA_BLOCK_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <urcu.h>
#include <urcu/ref.h>

struct data_block {
	struct rcu_head db_rcu;
	struct urcu_ref db_ref;

	pthread_mutex_t db_lock;
	char *db_text;
	int db_len;
};

struct data_block *data_block_alloc(char *text, int len);
struct data_block *data_block_get(struct data_block *db);
void data_block_put(struct data_block *db);
bool data_block_unhash(struct data_block *db);

#endif /* _REFFS_DATA_BLOCK_H */
