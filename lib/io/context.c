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
#include "reffs/trace/io.h"

static int context_created = 0;
static int context_freed = 0;

struct cds_lfht *io_context_ht = NULL;

static bool io_context_unhash(struct io_context *ic)
{
	int ret;
	bool b;
	uint64_t state;

	state = __atomic_fetch_and(&ic->ic_state, ~IO_CONTEXT_IS_HASHED,
				   __ATOMIC_ACQUIRE);
	b = state & IO_CONTEXT_IS_HASHED;
	if (b) {
		ret = cds_lfht_del(io_context_ht, &ic->ic_next);
		if (ret)
			LOG("ret = %d", ret);
		assert(!ret);
		return true;
	}

	return false;
}

int io_context_init(void)
{
	io_context_ht = cds_lfht_new(1024, 1024, 0,
				     CDS_LFHT_AUTO_RESIZE | CDS_LFHT_ACCOUNTING,
				     NULL);
	if (!io_context_ht) {
		LOG("Could not create the io context  hash table");
		return ENOMEM;
	}

	return 0;
}

int io_context_fini(void)
{
	struct cds_lfht_iter iter = { 0 };
	struct io_context *ic;
	int count = 0;
	int ret = 0;

	rcu_read_lock();
	cds_lfht_for_each_entry(io_context_ht, &iter, ic, ic_next) {
		if (io_context_unhash(ic))
			count++;
	}
	rcu_read_unlock();

	if (count)
		LOG("count = %d", count);

	ret = cds_lfht_destroy(io_context_ht, NULL);
	if (ret < 0) {
		LOG("Could not delete a hash table: %m");
	}

	io_context_ht = NULL;

	return 0;
}

static uint32_t generate_id(void)
{
	static uint32_t next_id = 1;
	static pthread_mutex_t id_mutex = PTHREAD_MUTEX_INITIALIZER;

	pthread_mutex_lock(&id_mutex);
	uint32_t id = next_id++;
	pthread_mutex_unlock(&id_mutex);

	return id;
}

static void io_context_free_rcu(struct rcu_head *rcu)
{
	struct io_context *ic =
		caa_container_of(rcu, struct io_context, ic_rcu);

	trace_io_context(ic, __func__);

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

	free(ic->ic_buffer);
	free(ic);
}

static void io_context_release(struct urcu_ref *ref)
{
	struct io_context *ic =
		caa_container_of(ref, struct io_context, ic_ref);

	io_context_unhash(ic);
	trace_io_context(ic, __func__);

	call_rcu(&ic->ic_rcu, io_context_free_rcu);
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

	urcu_ref_init(&ic->ic_ref);

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

	rcu_read_lock();
	__atomic_fetch_or(&ic->ic_state, IO_CONTEXT_IS_HASHED,
			  __ATOMIC_ACQUIRE);
	cds_lfht_add(io_context_ht, ic->ic_id, &ic->ic_next);
	rcu_read_unlock();

	trace_io_context(ic, __func__);

	return ic;
}

void io_dump_active_contexts(void)
{
	LOG("=== Active Contexts ===");

	struct cds_lfht_iter iter = { 0 };
	struct io_context *ic;
	int count = 0;
	time_t now = time(NULL);

	rcu_read_lock();
	cds_lfht_for_each_entry(io_context_ht, &iter, ic, ic_next) {
		time_t age = now - ic->ic_creation_time;
		LOG("%p op=%s fd=%d age=%ld id=%u", (void *)ic,
		    io_op_type_to_str(ic->ic_op_type), ic->ic_fd, (long)age,
		    ic->ic_id);
		count++;
	}
	rcu_read_unlock();

	LOG("Total active contexts: %d", count);
	LOG("======================");
}

void io_release_active_contexts(struct io_uring *ring)
{
	LOG("=== Freeing Orphaned Contexts ===");

	struct cds_lfht_iter iter = { 0 };
	struct io_context *ic;
	int count = 0;
	time_t now = time(NULL);

	rcu_read_lock();
	cds_lfht_for_each_entry(io_context_ht, &iter, ic, ic_next) {
		time_t age = now - ic->ic_creation_time;

		LOG("%p op=%s fd=%d age=%ld id=%u", (void *)ic,
		    io_op_type_to_str(ic->ic_op_type), ic->ic_fd, (long)(age),
		    ic->ic_id);

		struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
		if (sqe) {
			__atomic_fetch_or(&ic->ic_state,
					  IO_CONTEXT_IS_CANCELLED,
					  __ATOMIC_ACQUIRE);
			io_uring_prep_cancel(sqe, ic, 0);
			sqe->cancel_flags |= IORING_ASYNC_CANCEL_ALL;
			io_uring_sqe_set_data(sqe, NULL);
			io_uring_submit(ring);
		}

		count++;
	}
	rcu_read_unlock();

	LOG("Total orphaned contexts: %d", count);
	LOG("======================");
}

void io_check_stalled_operations(struct io_uring *ring)
{
	struct cds_lfht_iter iter = { 0 };
	struct io_context *ic;
	int count = 0;
	time_t now = time(NULL);

	LOG("=== Freeing Stalled Contexts ===");

	rcu_read_lock();
	cds_lfht_for_each_entry(io_context_ht, &iter, ic, ic_next) {
		time_t age = now - ic->ic_creation_time;

		if (age < 60 || ic->ic_op_type == OP_TYPE_ACCEPT)
			continue;

		if (age > 180) {
			io_socket_close(ic->ic_fd, ETIMEDOUT);
			LOG("FORCE CLEANUP: stalled operation: %p op=%s fd=%d age=%ld id=%u",
			    (void *)ic, io_op_type_to_str(ic->ic_op_type),
			    ic->ic_fd, (long)age, ic->ic_id);
		} else
			LOG("Detected stalled operation: %p op=%s fd=%d age=%ld id=%u",
			    (void *)ic, io_op_type_to_str(ic->ic_op_type),
			    ic->ic_fd, (long)(now - ic->ic_creation_time),
			    ic->ic_id);

		struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
		if (sqe) {
			__atomic_fetch_or(&ic->ic_state,
					  IO_CONTEXT_IS_CANCELLED,
					  __ATOMIC_ACQUIRE);
			io_uring_prep_cancel(sqe, ic, 0);
			sqe->cancel_flags |= IORING_ASYNC_CANCEL_ALL;
			io_uring_sqe_set_data(sqe, NULL);
			io_uring_submit(ring);
		}
		count++;
	}
	rcu_read_unlock();

	LOG("Total stalled contexts: %d", count);
	LOG("======================");
}

int get_context_created(void)
{
	return context_created;
}

int get_context_freed(void)
{
	return context_freed;
}

struct io_context *io_context_get(struct io_context *ic)
{
	if (!ic)
		return NULL;

	if (!urcu_ref_get_unless_zero(&ic->ic_ref))
		return NULL;

	trace_io_context(ic, __func__);

	return ic;
}

void io_context_put(struct io_context *ic)
{
	if (!ic)
		return;

	trace_io_context(ic, __func__);
	urcu_ref_put(&ic->ic_ref, io_context_release);
}
