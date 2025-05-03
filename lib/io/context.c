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

static pthread_mutex_t context_mutex = PTHREAD_MUTEX_INITIALIZER;
#define CONTEXT_TRACKING 1027
static struct io_context *active_contexts[CONTEXT_TRACKING] = { 0 };

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
	ic->ic_creation_time = time(NULL);

	context_created++;

	switch (op_type) {
	case OP_TYPE_READ:
		io_conn_add_read_op(fd);
		break;
	case OP_TYPE_WRITE:
		io_conn_add_write_op(fd);
		break;
	case OP_TYPE_ACCEPT:
		io_conn_add_accept_op(fd);
		break;
	case OP_TYPE_CONNECT:
		io_conn_add_connect_op(fd);
		break;
	default:
		// No specific counter for other op types
		break;
	}

	pthread_mutex_lock(&context_mutex);
	for (int i = 0; i < CONTEXT_TRACKING; i++) {
		if (active_contexts[i] == NULL) {
			active_contexts[i] = ic;
			LOG("Context created: %p op=%s fd=%d slot=%d id=%u",
			    (void *)ic, op_type_to_str(op_type), fd, i,
			    ic->ic_id);
			break;
		}
	}
	pthread_mutex_unlock(&context_mutex);

	return ic;
}

void io_context_free(struct io_context *ic)
{
	if (!ic)
		return;

	context_freed++;

	switch (ic->ic_op_type) {
	case OP_TYPE_READ:
		io_conn_remove_read_op(ic->ic_fd);
		break;
	case OP_TYPE_WRITE:
		io_conn_remove_write_op(ic->ic_fd);
		break;
	case OP_TYPE_ACCEPT:
		io_conn_remove_accept_op(ic->ic_fd);
		break;
	case OP_TYPE_CONNECT:
		io_conn_remove_connect_op(ic->ic_fd);
		break;
	default:
		// No specific counter for other op types
		break;
	}

	pthread_mutex_lock(&context_mutex);
	for (int i = 0; i < CONTEXT_TRACKING; i++) {
		if (active_contexts[i] == ic) {
			active_contexts[i] = NULL;
			LOG("Context freed: %p op=%s fd=%d slot=%d id=%u",
			    (void *)ic, op_type_to_str(ic->ic_op_type),
			    ic->ic_fd, i, ic->ic_id);
			break;
		}
	}
	pthread_mutex_unlock(&context_mutex);

	free(ic->ic_buffer);
	free(ic);
}

void io_dump_active_contexts(void)
{
	LOG("=== Active Contexts ===");
	int count = 0;

	pthread_mutex_lock(&context_mutex);
	time_t now = time(NULL);

	for (int i = 0; i < CONTEXT_TRACKING; i++) {
		if (active_contexts[i] != NULL) {
			struct io_context *ic = active_contexts[i];
			time_t age = (ic->ic_creation_time > 0) ?
					     now - ic->ic_creation_time :
					     0;
			LOG("[%d] %p: op=%s fd=%d age=%ld id=%u", i, (void *)ic,
			    op_type_to_str(ic->ic_op_type), ic->ic_fd,
			    (long)age, ic->ic_id);
			count++;
		}
	}
	pthread_mutex_unlock(&context_mutex);

	LOG("Total active contexts: %d", count);
	LOG("======================");
}

void io_release_active_contexts(struct io_uring *ring)
{
	LOG("=== Freeing Orphaned Contexts ===");
	int count = 0;

	pthread_mutex_lock(&context_mutex);
	time_t now = time(NULL);
	for (int i = 0; i < CONTEXT_TRACKING; i++) {
		if (active_contexts[i] != NULL) {
			struct io_context *ic = active_contexts[i];
			time_t age = (ic->ic_creation_time > 0) ?
					     now - ic->ic_creation_time :
					     0;
			LOG("[%d] %p op=%s fd=%d age=%ld id=%u", i, (void *)ic,
			    op_type_to_str(ic->ic_op_type), ic->ic_fd,
			    (long)(age), ic->ic_id);

			struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
			if (sqe) {
				io_uring_prep_cancel(sqe, ic, 0);
				io_uring_submit(ring);
			}

			// io_context_free(ic);
			count++;
		}
	}
	pthread_mutex_unlock(&context_mutex);

	LOG("Total orphaned contexts: %d", count);
	LOG("======================");
}

void io_check_stalled_operations(struct io_uring *ring)
{
	time_t now = time(NULL);

	pthread_mutex_lock(&context_mutex);
	for (int i = 0; i < CONTEXT_TRACKING; i++) {
		if (active_contexts[i] != NULL) {
			struct io_context *ic = active_contexts[i];

			if (!ic->ic_creation_time)
				continue;

			if (now - ic->ic_creation_time > 180) {
				LOG("FORCE CLEANUP: stalled operation: [%d] %p op=%s fd=%d age=%ld id=%u",
				    i, (void *)ic,
				    op_type_to_str(ic->ic_op_type), ic->ic_fd,
				    (long)(now - ic->ic_creation_time),
				    ic->ic_id);
				io_socket_close(ic->ic_fd, ETIMEDOUT);
				active_contexts[i] = NULL;
				io_context_free(ic);
			} else if (now - ic->ic_creation_time > 60) {
				LOG("Detected stalled operation: [%d] %p op=%s fd=%d age=%ld id=%u",
				    i, (void *)ic,
				    op_type_to_str(ic->ic_op_type), ic->ic_fd,
				    (long)(now - ic->ic_creation_time),
				    ic->ic_id);

				// Mark the context as being cancelled
				ic->ic_cancelled = true;

				// Try to cancel the operation
				struct io_uring_sqe *sqe =
					io_uring_get_sqe(ring);
				if (sqe) {
					io_uring_prep_cancel(sqe, ic, 0);
					// Use a different user_data for the cancellation
					// to distinguish it from the original operation
					io_uring_sqe_set_data(sqe, NULL);
					io_uring_submit(ring);
				}

				// DO NOT free the context here - let the normal completion path handle it
			}
		}
	}
	pthread_mutex_unlock(&context_mutex);
}

int get_context_created(void)
{
	return context_created;
}

int get_context_freed(void)
{
	return context_freed;
}
