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
struct cds_lfht *io_cancelled_ht = NULL;
struct cds_lfht *io_destroyed_ht = NULL;

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

static bool io_cancelled_unhash(struct io_context *ic)
{
	int ret;
	bool b;
	uint64_t state;

	state = __atomic_fetch_and(&ic->ic_state, ~IO_CONTEXT_IS_CANCELLED_HASH,
				   __ATOMIC_ACQUIRE);
	b = state & IO_CONTEXT_IS_CANCELLED_HASH;
	if (b) {
		ret = cds_lfht_del(io_cancelled_ht, &ic->ic_next);
		if (ret)
			LOG("ret = %d", ret);
		assert(!ret);
		return true;
	}

	return false;
}

static bool io_destroyed_unhash(struct io_context *ic)
{
	int ret;
	bool b;
	uint64_t state;

	state = __atomic_fetch_and(&ic->ic_state, ~IO_CONTEXT_IS_DESTROYED_HASH,
				   __ATOMIC_ACQUIRE);
	b = state & IO_CONTEXT_IS_DESTROYED_HASH;
	if (b) {
		ret = cds_lfht_del(io_destroyed_ht, &ic->ic_next);
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
		LOG("Could not create the io context hash table");
		return ENOMEM;
	}

	io_cancelled_ht = cds_lfht_new(
		8, 8, 0, CDS_LFHT_AUTO_RESIZE | CDS_LFHT_ACCOUNTING, NULL);
	if (!io_context_ht) {
		LOG("Could not create the io context cancelled hash table");
		return ENOMEM;
	}

	io_destroyed_ht =
		cds_lfht_new(1024, 1024, 0,
			     CDS_LFHT_AUTO_RESIZE | CDS_LFHT_ACCOUNTING, NULL);
	if (!io_context_ht) {
		LOG("Could not create the io context destroyed hash table");
		return ENOMEM;
	}
	return 0;
}

static void io_context_free_rcu(struct rcu_head *rcu)
{
	struct io_context *ic =
		caa_container_of(rcu, struct io_context, ic_rcu);

	trace_io_context(ic, __func__, __LINE__); // loghyr
	free(ic->ic_buffer);
	free(ic);
}

int io_context_fini(void)
{
	struct cds_lfht_iter iter = { 0 };
	struct io_context *ic;
	int count;
	int ret = 0;

	if (io_context_ht) {
		count = 0;
		rcu_read_lock();
		cds_lfht_for_each_entry(io_context_ht, &iter, ic, ic_next) {
			if (io_context_unhash(ic))
				count++;
		}
		rcu_read_unlock();

		if (count)
			LOG("Contexts = %d", count);

		ret = cds_lfht_destroy(io_context_ht, NULL);
		if (ret < 0) {
			LOG("Could not delete a hash table: %m");
		}

		io_context_ht = NULL;
	}

	if (io_cancelled_ht) {
		count = 0;
		rcu_read_lock();
		cds_lfht_for_each_entry(io_cancelled_ht, &iter, ic, ic_next) {
			if (io_cancelled_unhash(ic)) {
				count++;
				call_rcu(&ic->ic_rcu, io_context_free_rcu);
			}
		}
		rcu_read_unlock();

		if (count)
			LOG("Cancelled = %d", count);

		ret = cds_lfht_destroy(io_cancelled_ht, NULL);
		if (ret < 0) {
			LOG("Could not delete a hash table: %m");
		}

		io_cancelled_ht = NULL;
	}

	if (io_destroyed_ht) {
		count = 0;
		rcu_read_lock();
		cds_lfht_for_each_entry(io_destroyed_ht, &iter, ic, ic_next) {
			if (io_destroyed_unhash(ic)) {
				count++;
				call_rcu(&ic->ic_rcu, io_context_free_rcu);
			}
		}
		rcu_read_unlock();

		if (count)
			LOG("Destroyed = %d", count);

		ret = cds_lfht_destroy(io_destroyed_ht, NULL);
		if (ret < 0) {
			LOG("Could not delete a hash table: %m");
		}

		io_destroyed_ht = NULL;
	}

	return 0;
}

static uint32_t generate_id(void)
{
	static _Atomic uint32_t next_id = 1;
	return atomic_fetch_add_explicit(&next_id, 1, memory_order_seq_cst) + 1;
}

void io_context_update_time(struct io_context *ic)
{
	ic->ic_action_time = time(NULL);
	trace_io_context(ic, __func__, __LINE__); // loghyr
}

static bool mark_io_context_destroyed(struct io_context *ic)
{
	uint64_t old_state, new_state;

	__atomic_load(&ic->ic_state, &old_state, __ATOMIC_ACQUIRE);

	if ((old_state & IO_CONTEXT_MARKED_DESTROYED) &&
	    !(old_state & IO_CONTEXT_IS_DESTROYED)) {
		new_state = old_state | IO_CONTEXT_IS_DESTROYED;

		if (__atomic_compare_exchange(&ic->ic_state, &old_state,
					      &new_state, 0, __ATOMIC_SEQ_CST,
					      __ATOMIC_SEQ_CST)) {
			return true;
		}

		return false;
	}

	return false;
}

void io_context_destroy(struct io_context *ic)
{
	trace_io_context(ic, __func__, __LINE__);

	uint64_t state = __atomic_fetch_or(
		&ic->ic_state, IO_CONTEXT_MARKED_DESTROYED, __ATOMIC_ACQUIRE);
	if (!(state & IO_CONTEXT_MARKED_DESTROYED))
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

	trace_io_context(ic, __func__, __LINE__);

	if (mark_io_context_destroyed(ic)) {
		io_context_unhash(ic);
		ic->ic_action_time = time(NULL);
		rcu_read_lock();
		__atomic_fetch_or(&ic->ic_state, IO_CONTEXT_IS_DESTROYED_HASH,
				  __ATOMIC_ACQUIRE);
		cds_lfht_add(io_cancelled_ht, ic->ic_id, &ic->ic_next);
		rcu_read_unlock();

		return;
	}
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
	ic->ic_action_time = time(NULL);

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

	trace_io_context(ic, __func__, __LINE__);

	return ic;
}

void io_context_list_active(bool listem)
{
	LOG("=== Active Contexts ===");

	struct cds_lfht_iter iter = { 0 };
	struct io_context *ic;
	int count = 0;
	time_t now = time(NULL);

	rcu_read_lock();
	cds_lfht_for_each_entry(io_context_ht, &iter, ic, ic_next) {
		if (listem) {
			time_t age = now - ic->ic_action_time;
			LOG("%p op=%s fd=%d age=%ld id=%u", (void *)ic,
			    io_op_type_to_str(ic->ic_op_type), ic->ic_fd,
			    (long)age, ic->ic_id);
		}
		count++;
	}
	rcu_read_unlock();

	LOG("Total active contexts: %d", count);
	LOG("======================");
}

static bool mark_io_context_cancelled(struct io_context *ic)
{
	uint64_t old_state, new_state;

	__atomic_load(&ic->ic_state, &old_state, __ATOMIC_ACQUIRE);

	if ((old_state & IO_CONTEXT_MARKED_CANCELLED) &&
	    !(old_state & IO_CONTEXT_IS_CANCELLED)) {
		new_state = old_state | IO_CONTEXT_IS_CANCELLED;

		if (__atomic_compare_exchange(&ic->ic_state, &old_state,
					      &new_state, 0, __ATOMIC_SEQ_CST,
					      __ATOMIC_SEQ_CST)) {
			return true;
		}

		return false;
	}

	return false;
}

void ic_context_cancel(struct io_context *ic, struct io_uring *ring)
{
	uint64_t state = __atomic_fetch_or(
		&ic->ic_state, IO_CONTEXT_MARKED_CANCELLED, __ATOMIC_ACQUIRE);
	if (!(state & IO_CONTEXT_MARKED_CANCELLED))
		return;

	switch (ic->ic_op_type) {
	case OP_TYPE_READ:
		request_additional_read_data(ic->ic_fd, &ic->ic_ci, ring);
		break;
	case OP_TYPE_WRITE:
		break;
	case OP_TYPE_ACCEPT:
		request_accept_op(ic->ic_fd, &ic->ic_ci, ring);
		break;
	case OP_TYPE_CONNECT:
		break;
	default:
		break;
	}

	trace_io_context(ic, __func__, __LINE__);

	if (mark_io_context_cancelled(ic)) {
		io_context_unhash(ic);
		ic->ic_action_time = time(NULL);
		rcu_read_lock();
		__atomic_fetch_or(&ic->ic_state, IO_CONTEXT_IS_CANCELLED_HASH,
				  __ATOMIC_ACQUIRE);
		cds_lfht_add(io_cancelled_ht, ic->ic_id, &ic->ic_next);
		rcu_read_unlock();

		return;
	}

	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
	if (sqe) {
		io_uring_prep_cancel(sqe, ic, 0);
		sqe->cancel_flags |= IORING_ASYNC_CANCEL_ALL;
		io_uring_sqe_set_data(sqe, NULL);
		io_uring_submit(ring);
	}
}

void io_context_release_active(struct io_uring *ring)
{
	LOG("=== Freeing Orphaned Contexts ===");

	struct cds_lfht_iter iter = { 0 };
	struct io_context *ic;
	int count = 0;
	time_t now = time(NULL);

	rcu_read_lock();
	cds_lfht_for_each_entry(io_context_ht, &iter, ic, ic_next) {
		time_t age = now - ic->ic_action_time;

		LOG("%p op=%s fd=%d age=%ld id=%u", (void *)ic,
		    io_op_type_to_str(ic->ic_op_type), ic->ic_fd, (long)(age),
		    ic->ic_id);

		ic_context_cancel(ic, ring);

		count++;
	}
	rcu_read_unlock();

	LOG("Total orphaned contexts: %d", count);
	LOG("======================");
}

void io_context_check_stalled(struct io_uring *ring)
{
	struct cds_lfht_iter iter = { 0 };
	struct io_context *ic;
	int count = 0;
	time_t now = time(NULL);

	LOG("=== Freeing Stalled Contexts ===");

	rcu_read_lock();
	cds_lfht_for_each_entry(io_context_ht, &iter, ic, ic_next) {
		time_t age = now - ic->ic_action_time;

		/*
		 * READ and ACCEPT can both sit there forever
		 * waiting on the client to do IO.
		 */
		if (age < 60 || ic->ic_op_type != OP_TYPE_WRITE)
			continue;

		trace_io_context(ic, __func__, __LINE__);

		ic_context_cancel(ic, ring);

		count++;
	}
	rcu_read_unlock();

	LOG("Total stalled contexts: %d", count);
	LOG("======================");
}

void io_context_release_cancelled(void)
{
	struct cds_lfht_iter iter = { 0 };
	struct io_context *ic;
	int count = 0;
	time_t now = time(NULL);

	LOG("=== Freeing Cancelled Contexts ===");

	rcu_read_lock();
	cds_lfht_for_each_entry(io_cancelled_ht, &iter, ic, ic_next) {
		time_t age = now - ic->ic_action_time;

		if (age < 60)
			continue;

		trace_io_context(ic, __func__, __LINE__);
		io_cancelled_unhash(ic);
		count++;
		call_rcu(&ic->ic_rcu, io_context_free_rcu);
	}
	rcu_read_unlock();

	LOG("Total cancelled contexts: %d", count);
	LOG("======================");
}

void io_context_release_destroyed(void)
{
	struct cds_lfht_iter iter = { 0 };
	struct io_context *ic;
	int count = 0;
	time_t now = time(NULL);

	LOG("=== Freeing Destroyed Contexts ===");

	rcu_read_lock();
	cds_lfht_for_each_entry(io_destroyed_ht, &iter, ic, ic_next) {
		time_t age = now - ic->ic_action_time;

		if (age < 60)
			continue;

		trace_io_context(ic, __func__, __LINE__);
		call_rcu(&ic->ic_rcu, io_context_free_rcu);

		count++;
	}
	rcu_read_unlock();

	LOG("Total destroyed contexts: %d", count);
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
