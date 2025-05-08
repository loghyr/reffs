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

static _Atomic uint64_t context_created;
static _Atomic uint64_t context_freed;

static _Atomic uint64_t active_cancelled;
static _Atomic uint64_t active_destroyed;
static _Atomic uint64_t cancelled_freed;
static _Atomic uint64_t destroyed_freed;

struct cds_lfht *io_active_ht = NULL;
struct cds_lfht *io_cancel_ht = NULL;
struct cds_lfht *io_destroy_ht = NULL;

int context_match(struct cds_lfht_node *node, const void *key)
{
	const uint32_t *id = key;
	struct io_context *ic =
		caa_container_of(node, struct io_context, ic_active_node);
	return ic->ic_id == *id;
}

static bool active_unhash(struct io_context *ic)
{
	int ret;
	bool b;
	uint64_t state;

	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;

	rcu_read_lock();
	state = __atomic_fetch_and(&ic->ic_state, ~IO_CONTEXT_IS_HASHED,
				   __ATOMIC_SEQ_CST);
	b = state & IO_CONTEXT_IS_HASHED;

	cds_lfht_lookup(io_active_ht, ic->ic_id, context_match, &ic->ic_id,
			&iter);
	node = cds_lfht_iter_get_node(&iter);
	if (node == &ic->ic_active_node) {
		ret = cds_lfht_del(io_active_ht, &ic->ic_active_node);
		if (ret && ret != -ENOENT) {
			LOG("ret = %d", ret);
			assert(!ret);
		}
	}

	rcu_read_unlock();
	return b;
}

static bool cancel_unhash(struct io_context *ic)
{
	int ret;
	bool b;
	uint64_t state;

	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;

	rcu_read_lock();
	state = __atomic_fetch_and(&ic->ic_state, ~IO_CONTEXT_IS_CANCELLED_HASH,
				   __ATOMIC_SEQ_CST);
	b = state & IO_CONTEXT_IS_CANCELLED_HASH;

	cds_lfht_lookup(io_cancel_ht, ic->ic_id, context_match, &ic->ic_id,
			&iter);
	node = cds_lfht_iter_get_node(&iter);
	if (node == &ic->ic_active_node) {
		ret = cds_lfht_del(io_cancel_ht, &ic->ic_cancel_node);
		if (ret && ret != -ENOENT) {
			LOG("ret = %d", ret);
			assert(!ret);
		}
	}

	rcu_read_unlock();
	return b;
}

static bool ic_destroy_unhash(struct io_context *ic)
{
	int ret;
	bool b;
	uint64_t state;

	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;

	rcu_read_lock();
	state = __atomic_fetch_and(&ic->ic_state, ~IO_CONTEXT_IS_DESTROYED_HASH,
				   __ATOMIC_SEQ_CST);
	b = state & IO_CONTEXT_IS_DESTROYED_HASH;

	cds_lfht_lookup(io_destroy_ht, ic->ic_id, context_match, &ic->ic_id,
			&iter);
	node = cds_lfht_iter_get_node(&iter);
	if (node == &ic->ic_active_node) {
		ret = cds_lfht_del(io_destroy_ht, &ic->ic_destroy_node);
		if (ret && ret != -ENOENT) {
			LOG("ret = %d", ret);
			assert(!ret);
		}
	}

	rcu_read_unlock();
	return b;
}

// Remove context from all hash tables to avoid any inconsistencies
static void io_context_remove_from_all_hash_tables(struct io_context *ic)
{
	active_unhash(ic);
	cancel_unhash(ic);
	ic_destroy_unhash(ic);
}

int io_context_init(void)
{
	io_active_ht = cds_lfht_new(1024, 1024, 0,
				    CDS_LFHT_AUTO_RESIZE | CDS_LFHT_ACCOUNTING,
				    NULL);
	if (!io_active_ht) {
		LOG("Could not create the io context hash table");
		return ENOMEM;
	}

	io_cancel_ht = cds_lfht_new(
		8, 8, 0, CDS_LFHT_AUTO_RESIZE | CDS_LFHT_ACCOUNTING, NULL);
	if (!io_cancel_ht) {
		LOG("Could not create the io context cancelled hash table");
		return ENOMEM;
	}

	io_destroy_ht = cds_lfht_new(1024, 1024, 0,
				     CDS_LFHT_AUTO_RESIZE | CDS_LFHT_ACCOUNTING,
				     NULL);
	if (!io_destroy_ht) {
		LOG("Could not create the io context destroyed hash table");
		return ENOMEM;
	}

	atomic_store(&active_cancelled, 0);
	atomic_store(&active_destroyed, 0);
	atomic_store(&cancelled_freed, 0);
	atomic_store(&destroyed_freed, 0);
	atomic_store(&context_created, 0);
	atomic_store(&context_freed, 0);

	return 0;
}

static void io_context_free_rcu(struct rcu_head *rcu)
{
	struct io_context *ic =
		caa_container_of(rcu, struct io_context, ic_rcu);

	atomic_fetch_add(&context_freed, 1);
	trace_io_context(ic, __func__, __LINE__);
	free(ic->ic_buffer);
	free(ic);
}

static void io_context_free_with_checks(struct io_context *ic)
{
	// Make sure it's not in any hash table
	io_context_remove_from_all_hash_tables(ic);

	// Now schedule the actual free
	call_rcu(&ic->ic_rcu, io_context_free_rcu);
}

int io_context_fini(void)
{
	struct cds_lfht_iter iter = { 0 };
	struct io_context *ic;
	int count;
	int ret = 0;

	if (io_active_ht) {
		count = 0;
		rcu_read_lock();
		cds_lfht_for_each_entry(io_active_ht, &iter, ic,
					ic_active_node) {
			if (active_unhash(ic))
				count++;
		}
		rcu_read_unlock();

		if (count)
			LOG("Contexts = %d", count);

		ret = cds_lfht_destroy(io_active_ht, NULL);
		if (ret < 0) {
			LOG("Could not delete a hash table: %m");
		}

		io_active_ht = NULL;
	}

	if (io_cancel_ht) {
		count = 0;
		rcu_read_lock();
		cds_lfht_for_each_entry(io_cancel_ht, &iter, ic,
					ic_cancel_node) {
			if (cancel_unhash(ic)) {
				count++;
				atomic_fetch_add(&cancelled_freed, 1);
				io_context_free_with_checks(ic);
			}
		}
		rcu_read_unlock();

		if (count)
			LOG("Cancelled = %d", count);

		ret = cds_lfht_destroy(io_cancel_ht, NULL);
		if (ret < 0) {
			LOG("Could not delete a hash table: %m");
		}

		io_cancel_ht = NULL;
	}

	if (io_destroy_ht) {
		count = 0;
		rcu_read_lock();
		cds_lfht_for_each_entry(io_destroy_ht, &iter, ic,
					ic_destroy_node) {
			if (ic_destroy_unhash(ic)) {
				count++;
				atomic_fetch_add(&destroyed_freed, 1);
				io_context_free_with_checks(ic);
			}
		}
		rcu_read_unlock();

		if (count)
			LOG("Destroyed = %d", count);

		ret = cds_lfht_destroy(io_destroy_ht, NULL);
		if (ret < 0) {
			LOG("Could not delete a hash table: %m");
		}

		io_destroy_ht = NULL;
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
}

static bool mark_io_context_destroyed(struct io_context *ic)
{
	uint64_t old_state, new_state;

	do {
		__atomic_load(&ic->ic_state, &old_state, __ATOMIC_SEQ_CST);

		// Already destroyed or being destroyed by another thread
		if (old_state & IO_CONTEXT_IS_DESTROYED)
			return false;

		// Set both flags atomically
		new_state = old_state | IO_CONTEXT_MARKED_DESTROYED |
			    IO_CONTEXT_IS_DESTROYED;

	} while (!__atomic_compare_exchange(&ic->ic_state, &old_state,
					    &new_state, 0, __ATOMIC_SEQ_CST,
					    __ATOMIC_SEQ_CST));

	return true;
}

void io_context_destroy(struct io_context *ic)
{
	trace_io_context(ic, __func__, __LINE__);

	// Only continue if we can mark it
	uint64_t state = __atomic_fetch_or(
		&ic->ic_state, IO_CONTEXT_MARKED_DESTROYED, __ATOMIC_SEQ_CST);
	if (state & IO_CONTEXT_MARKED_DESTROYED)
		return;

	atomic_fetch_add(&active_destroyed, 1);

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

	// Try to fully mark it destroyed
	if (mark_io_context_destroyed(ic)) {
		// CRITICAL SECTION - ensure hash table operations are consistent
		rcu_read_lock();

		// Double-check the context isn't already unhashed
		if (ic->ic_state & IO_CONTEXT_IS_HASHED) {
			active_unhash(ic);
			trace_io_context(ic, __func__, __LINE__);
		}

		// Only add to destroy hash if not already there
		if (!(ic->ic_state & IO_CONTEXT_IS_DESTROYED_HASH)) {
			ic->ic_action_time = time(NULL);
			__atomic_fetch_or(&ic->ic_state,
					  IO_CONTEXT_IS_DESTROYED_HASH,
					  __ATOMIC_SEQ_CST);
			cds_lfht_add(io_destroy_ht, ic->ic_id,
				     &ic->ic_destroy_node);
			trace_io_context(ic, __func__, __LINE__);
		}

		rcu_read_unlock();
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

	cds_lfht_node_init(&ic->ic_active_node);
	cds_lfht_node_init(&ic->ic_destroy_node);
	cds_lfht_node_init(&ic->ic_cancel_node);

	atomic_fetch_add(&context_created, 1);

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
			  __ATOMIC_SEQ_CST);
	cds_lfht_add(io_active_ht, ic->ic_id, &ic->ic_active_node);
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
	cds_lfht_for_each_entry(io_active_ht, &iter, ic, ic_active_node) {
		if (!(ic->ic_state & IO_CONTEXT_IS_HASHED) ||
		    (ic->ic_state &
		     (IO_CONTEXT_IS_DESTROYED | IO_CONTEXT_IS_CANCELLED))) {
			LOG("WARNING: Context %p with id %u found in active hash with invalid state 0x%lx",
			    (void *)ic, ic->ic_id, (unsigned long)ic->ic_state);

			active_unhash(ic);
			continue;
		}

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

	do {
		__atomic_load(&ic->ic_state, &old_state, __ATOMIC_SEQ_CST);

		// Already cancelled or being cancelled by another thread
		if (old_state & IO_CONTEXT_IS_CANCELLED)
			return false;

		// Set both flags atomically
		new_state = old_state | IO_CONTEXT_MARKED_CANCELLED |
			    IO_CONTEXT_IS_CANCELLED;

	} while (!__atomic_compare_exchange(&ic->ic_state, &old_state,
					    &new_state, 0, __ATOMIC_SEQ_CST,
					    __ATOMIC_SEQ_CST));

	return true;
}

void ic_context_cancel(struct io_context *ic, struct io_uring *ring)
{
	// Only continue if we can mark it
	uint64_t state = __atomic_fetch_or(
		&ic->ic_state, IO_CONTEXT_MARKED_CANCELLED, __ATOMIC_SEQ_CST);
	if (state & IO_CONTEXT_MARKED_CANCELLED)
		return;

	atomic_fetch_add(&active_cancelled, 1);

	switch (ic->ic_op_type) {
	case OP_TYPE_READ:
		io_request_read_op(ic->ic_fd, &ic->ic_ci, ring);
		break;
	case OP_TYPE_WRITE:
		break;
	case OP_TYPE_ACCEPT:
		io_request_accept_op(ic->ic_fd, &ic->ic_ci, ring);
		break;
	case OP_TYPE_CONNECT:
		break;
	case OP_TYPE_HEARTBEAT:
		break;
	default:
		break;
	}

	trace_io_context(ic, __func__, __LINE__);

	// Try to fully mark it cancelled
	if (mark_io_context_cancelled(ic)) {
		// CRITICAL SECTION - ensure hash table operations are consistent
		rcu_read_lock();

		// Double-check the context isn't already unhashed
		if (ic->ic_state & IO_CONTEXT_IS_HASHED) {
			active_unhash(ic);
		}

		// Only add to cancel hash if not already there
		if (!(ic->ic_state & IO_CONTEXT_IS_CANCELLED_HASH)) {
			ic->ic_action_time = time(NULL);
			__atomic_fetch_or(&ic->ic_state,
					  IO_CONTEXT_IS_CANCELLED_HASH,
					  __ATOMIC_SEQ_CST);
			cds_lfht_add(io_cancel_ht, ic->ic_id,
				     &ic->ic_cancel_node);
		}

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
	cds_lfht_for_each_entry(io_active_ht, &iter, ic, ic_active_node) {
		if (!(ic->ic_state & IO_CONTEXT_IS_HASHED) ||
		    (ic->ic_state &
		     (IO_CONTEXT_IS_DESTROYED | IO_CONTEXT_IS_CANCELLED))) {
			LOG("WARNING: Context %p with id %u found in active hash with invalid state 0x%lx",
			    (void *)ic, ic->ic_id, (unsigned long)ic->ic_state);

			active_unhash(ic);
			continue;
		}

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

static int adaptive_timeout(void)
{
	int to = 60;

	uint64_t active_count = context_created - context_freed;
	if (active_count > 100000)
		to = 240;
	else if (active_count > 50000)
		to = 180;
	else if (active_count > 10000)
		to = 120;

	return to;
}

void io_context_check_stalled(struct io_uring *ring)
{
	struct cds_lfht_iter iter = { 0 };
	struct io_context *ic;
	int count = 0;
	time_t now = time(NULL);

	LOG("=== Freeing Stalled Contexts ===");

	int to = adaptive_timeout();

	rcu_read_lock();
	cds_lfht_for_each_entry(io_active_ht, &iter, ic, ic_active_node) {
		if (!(ic->ic_state & IO_CONTEXT_IS_HASHED) ||
		    (ic->ic_state &
		     (IO_CONTEXT_IS_DESTROYED | IO_CONTEXT_IS_CANCELLED))) {
			LOG("WARNING: Context %p with id %u found in active hash with invalid state 0x%lx",
			    (void *)ic, ic->ic_id, (unsigned long)ic->ic_state);

			active_unhash(ic);
			continue;
		}

		time_t age = now - ic->ic_action_time;

		/*
		 * READ and ACCEPT can both sit there forever
		 * waiting on the client to do IO.
		 */
		if (age < to || ic->ic_op_type != OP_TYPE_WRITE)
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

	int to = adaptive_timeout();

	rcu_read_lock();
	cds_lfht_for_each_entry(io_cancel_ht, &iter, ic, ic_cancel_node) {
		// Skip contexts that don't have the right flags set
		if (!(ic->ic_state & IO_CONTEXT_IS_CANCELLED_HASH)) {
			LOG("WARNING: Context %p with id %u found in cancel hash with invalid state 0x%lx",
			    (void *)ic, ic->ic_id, (unsigned long)ic->ic_state);

			// Force remove it from the cancel hash table
			cancel_unhash(ic);
			continue;
		}

		time_t age = now - ic->ic_action_time;

		if (age < to)
			continue;

		trace_io_context(ic, __func__, __LINE__);
		cancel_unhash(ic);
		count++;
		atomic_fetch_add(&cancelled_freed, 1);
		io_context_free_with_checks(ic);
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

	int to = adaptive_timeout();

	rcu_read_lock();
	cds_lfht_for_each_entry(io_destroy_ht, &iter, ic, ic_destroy_node) {
		// Skip contexts that don't have the right flags set
		if (!(ic->ic_state & IO_CONTEXT_IS_DESTROYED_HASH)) {
			LOG("WARNING: Context %p with id %u found in destroy hash with invalid state 0x%lx",
			    (void *)ic, ic->ic_id, (unsigned long)ic->ic_state);

			// Force remove it from the destroy hash table
			ic_destroy_unhash(ic);
			continue;
		}

		time_t age = now - ic->ic_action_time;

		if (age < to)
			continue;

		trace_io_context(ic, __func__, __LINE__);
		ic_destroy_unhash(ic);
		atomic_fetch_add(&destroyed_freed, 1);
		io_context_free_with_checks(ic);

		count++;
	}
	rcu_read_unlock();

	LOG("Total destroyed contexts: %d", count);
	LOG("======================");
}

// Periodically validate hash table consistency
void io_context_validate_hash_tables(void)
{
	struct cds_lfht_iter iter = { 0 };
	struct io_context *ic;
	int inconsistent_count = 0;

	LOG("=== Validating Hash Tables ===");

	rcu_read_lock();

	// Check active hash table
	cds_lfht_for_each_entry(io_active_ht, &iter, ic, ic_active_node) {
		if (!(ic->ic_state & IO_CONTEXT_IS_HASHED) ||
		    (ic->ic_state &
		     (IO_CONTEXT_IS_DESTROYED | IO_CONTEXT_IS_CANCELLED))) {
			LOG("WARNING: Context %p in active hash with inconsistent state 0x%lx",
			    (void *)ic, (unsigned long)ic->ic_state);
			inconsistent_count++;

			// Fix it
			active_unhash(ic);
		}
	}

	// Check cancel hash table
	cds_lfht_for_each_entry(io_cancel_ht, &iter, ic, ic_cancel_node) {
		if (!(ic->ic_state & IO_CONTEXT_IS_CANCELLED_HASH)) {
			LOG("WARNING: Context %p in cancel hash with inconsistent state 0x%lx",
			    (void *)ic, (unsigned long)ic->ic_state);
			inconsistent_count++;

			// Fix it
			cancel_unhash(ic);
		}
	}

	// Check destroy hash table
	cds_lfht_for_each_entry(io_destroy_ht, &iter, ic, ic_destroy_node) {
		if (!(ic->ic_state & IO_CONTEXT_IS_DESTROYED_HASH)) {
			LOG("WARNING: Context %p in destroy hash with inconsistent state 0x%lx",
			    (void *)ic, (unsigned long)ic->ic_state);
			inconsistent_count++;

			// Fix it
			ic_destroy_unhash(ic);
		}
	}

	rcu_read_unlock();

	LOG("Total inconsistent contexts: %d", inconsistent_count);
	LOG("======================");
}

uint64_t io_context_get_created(void)
{
	return context_created;
}

uint64_t io_context_get_freed(void)
{
	return context_freed;
}

void io_context_stats(struct io_context_stats *ics)
{
	ics->ics_created = atomic_load(&context_created);
	ics->ics_freed = atomic_load(&context_freed);
	ics->ics_active_cancelled = atomic_load(&active_cancelled);
	ics->ics_active_destroyed = atomic_load(&active_destroyed);
	ics->ics_cancelled_freed = atomic_load(&cancelled_freed);
	ics->ics_destroyed_freed = atomic_load(&destroyed_freed);
}

void io_context_log_stats(void)
{
	LOG("Context state transitions: active_cancelled=%ld, active_destroyed=%ld, "
	    "cancelled_freed=%ld, destroyed_freed=%ld created=%ld freed=%ld",
	    atomic_load(&active_cancelled), atomic_load(&active_destroyed),
	    atomic_load(&cancelled_freed), atomic_load(&destroyed_freed),
	    atomic_load(&context_created), atomic_load(&context_freed));

	// Periodically validate hash tables to catch issues early
	io_context_validate_hash_tables();
}
