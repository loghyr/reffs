/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <liburing.h>
#include <fcntl.h>
#include <pthread.h>
#include <errno.h>
#include <urcu.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <time.h>
#include <stdbool.h>

#include "reffs/log.h"
#include "reffs/rpc.h"
#include "reffs/network.h"
#include "reffs/server.h"
#include "reffs/task.h"
#include "reffs/test.h"
#include "reffs/io.h"

static int context_created = 0;
static int context_freed = 0;

static uint32_t generate_id(void)
{
	static uint32_t next_id = 1;
	static pthread_mutex_t id_mutex = PTHREAD_MUTEX_INITIALIZER;

	pthread_mutex_lock(&id_mutex);
	uint32_t id = next_id++;
	pthread_mutex_unlock(&id_mutex);

	return id;
}

// Create an IO context for operations
struct io_context *io_context_create(enum op_type op_type, int fd, void *buffer,
				     size_t buffer_len)
{
	struct io_context *ic = calloc(1, sizeof(struct io_context));
	if (!ic) {
		return NULL;
	}

	ic->ic_op_type = op_type;
	ic->ic_fd = fd;
	ic->ic_id = generate_id();
	ic->ic_buffer = buffer;
	ic->ic_buffer_len = buffer_len;

	context_created++;
	TRACE(REFFS_TRACE_LEVEL_NOTICE,
	      "Created io_context %d of type %s (total: %d)", ic->ic_id,
	      op_type_to_str(op_type), context_created);

	return ic;
}

void io_context_free(struct io_context *ic)
{
	if (!ic)
		return;

	context_freed++;
	TRACE(REFFS_TRACE_LEVEL_NOTICE,
	      "Freed io_context %d of type %s (total: %d/%d)", ic->ic_id,
	      op_type_to_str(ic->ic_op_type), context_freed, context_created);

	free(ic->ic_buffer);
	free(ic);
}

int get_context_created(void)
{
	return context_created;
}

int get_context_freed(void)
{
	return context_freed;
}
