/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "reffs/log.h"
#include "reffs/io.h"
#include "reffs/log.h"
#include "trace_io.h"

#ifdef HAVE_VM
#define IO_CONTEXT_TIMEOUT (5)
#else
#define IO_CONTEXT_TIMEOUT (60)
#endif

static _Atomic uint64_t context_created;
static _Atomic uint64_t context_freed;

static _Atomic uint64_t active_destroyed;
static _Atomic uint64_t cancelled_freed;
static _Atomic uint64_t destroyed_freed;

// Hash table buckets - now contains io_context pointers directly
#define CONTEXT_HASH_SIZE 65536
static struct io_context *context_hash[CONTEXT_HASH_SIZE] = { 0 };
static pthread_mutex_t context_mutex = PTHREAD_MUTEX_INITIALIZER;

// Simple hash function
static inline unsigned int hash_id(uint32_t id)
{
	return id % CONTEXT_HASH_SIZE;
}

static uint32_t generate_id(void)
{
	static _Atomic uint32_t next_id = 1;
	return atomic_fetch_add_explicit(&next_id, 1, memory_order_seq_cst);
}

int io_context_init(void)
{
	memset(context_hash, 0, sizeof(context_hash));

	if (pthread_mutex_init(&context_mutex, NULL) != 0) {
		LOG("Could not initialize context mutex");
		return ENOMEM;
	}

	atomic_store(&active_destroyed, 0);
	atomic_store(&cancelled_freed, 0);
	atomic_store(&destroyed_freed, 0);
	atomic_store(&context_created, 0);
	atomic_store(&context_freed, 0);

	return 0;
}

void io_context_update_time(struct io_context *ic)
{
	ic->ic_action_time = time(NULL);
}

// Add a context to the hash table
static int io_context_register(struct io_context *ic)
{
	pthread_mutex_lock(&context_mutex);

	unsigned int bucket = hash_id(ic->ic_id);

	// Add to head of bucket list
	ic->ic_next = context_hash[bucket];
	context_hash[bucket] = ic;

	pthread_mutex_unlock(&context_mutex);
	return 0;
}

// Find a context by ID
struct io_context *io_context_find(uint32_t id)
{
	pthread_mutex_lock(&context_mutex);

	unsigned int bucket = hash_id(id);
	struct io_context *ic = NULL;

	for (struct io_context *curr = context_hash[bucket]; curr != NULL;
	     curr = curr->ic_next) {
		if (curr->ic_id == id) {
			ic = curr;
			break;
		}
	}

	pthread_mutex_unlock(&context_mutex);
	return ic;
}

// Remove a context from the hash table
static void io_context_unregister(struct io_context *ic)
{
	pthread_mutex_lock(&context_mutex);
	unsigned int bucket = hash_id(ic->ic_id);
	struct io_context **prev_ptr = &context_hash[bucket];
	struct io_context *curr = context_hash[bucket];

	while (curr != NULL) {
		if (curr == ic) {
			*prev_ptr = curr->ic_next;
			break;
		}
		prev_ptr = &curr->ic_next;
		curr = curr->ic_next;
	}
	pthread_mutex_unlock(&context_mutex);
}

void io_context_destroy(struct io_context *ic)
{
	// Claim ownership before touching any ic fields.  A racing second
	// caller will see MARKED_DESTROYED already set and return here,
	// before the first caller frees ic.
	uint64_t state = __atomic_fetch_or(
		&ic->ic_state, IO_CONTEXT_ENTRY_STATE_MARKED_DESTROYED,
		__ATOMIC_SEQ_CST);
	if (state & IO_CONTEXT_ENTRY_STATE_MARKED_DESTROYED)
		return;

	TRACE("DESTROY: ic=%p op=%s fd=%d id=%u", (void *)ic,
	      io_op_type_to_str(ic->ic_op_type), ic->ic_fd, ic->ic_id);
	trace_io_context(ic, __func__, __LINE__);

	// Clear the active state when marking as destroyed
	__atomic_fetch_and(&ic->ic_state, ~IO_CONTEXT_ENTRY_STATE_ACTIVE,
			   __ATOMIC_SEQ_CST);

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

#ifdef HAVE_DESTROY_CACHE
	// Update the action time to track when it was destroyed
	ic->ic_action_time = time(NULL);
	trace_io_context(ic, __func__, __LINE__);
#else
	// Immediate free
	io_context_unregister(ic);
	free(ic->ic_buffer);
	free(ic);
	atomic_fetch_add(&context_freed, 1);
	atomic_fetch_add(&destroyed_freed, 1);
#endif
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
	ic->ic_next = NULL;

	// Set active state
	ic->ic_state = IO_CONTEXT_ENTRY_STATE_ACTIVE;

	// Register the context in our hash table
	if (io_context_register(ic) != 0) {
		free(ic);
		return NULL;
	}

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

	trace_io_context(ic, __func__, __LINE__);

	return ic;
}

struct io_context *io_context_probe(int fd, enum op_type op, uint64_t state,
				    int *count)
{
	struct io_context *head = NULL;
	struct io_context *tail = NULL;
	int matched = 0;

	pthread_mutex_lock(&context_mutex);

	for (unsigned int i = 0; i < CONTEXT_HASH_SIZE; i++) {
		for (struct io_context *ic = context_hash[i]; ic != NULL;
		     ic = ic->ic_next) {
			if ((ic->ic_state & state) == 0)
				continue;

			if ((fd != 0 && ic->ic_fd != fd) ||
			    (op != OP_TYPE_ALL && ic->ic_op_type != op))
				continue;

			struct io_context *copy =
				calloc(1, sizeof(struct io_context));
			if (!copy)
				continue;

			memcpy(copy, ic, sizeof(struct io_context));
			copy->ic_next = NULL;

			if (tail)
				tail->ic_next = copy;
			else
				head = copy;

			tail = copy;
			matched++;
		}
	}

	pthread_mutex_unlock(&context_mutex);

	if (count)
		*count = matched;

	return head;
}

void io_context_list_active(bool listem)
{
	TRACE("=== Active Contexts ===");

	pthread_mutex_lock(&context_mutex);

	int count = 0;
	time_t now = time(NULL);

	for (unsigned int i = 0; i < CONTEXT_HASH_SIZE; i++) {
		for (struct io_context *ic = context_hash[i]; ic != NULL;
		     ic = ic->ic_next) {
			// Only count active contexts
			if (ic->ic_state & IO_CONTEXT_ENTRY_STATE_ACTIVE) {
				if (listem) {
					time_t age = now - ic->ic_action_time;
					TRACE("%p op=%s fd=%d age=%ld id=%u",
					      (void *)ic,
					      io_op_type_to_str(ic->ic_op_type),
					      ic->ic_fd, (long)age, ic->ic_id);
				}
				count++;
			}
		}
	}

	pthread_mutex_unlock(&context_mutex);

	TRACE("Total active contexts: %d", count);
	TRACE("======================");
}

void io_context_release_active(void)
{
	TRACE("=== Freeing Orphaned Contexts ===");

	pthread_mutex_lock(&context_mutex);

	int count = 0;
	time_t now = time(NULL);

	for (unsigned int i = 0; i < CONTEXT_HASH_SIZE; i++) {
		for (struct io_context *ic = context_hash[i]; ic != NULL;
		     ic = ic->ic_next) {
			// Only process active contexts
			if (ic->ic_state & IO_CONTEXT_ENTRY_STATE_ACTIVE) {
				time_t age = now - ic->ic_action_time;

				TRACE("%p op=%s fd=%d age=%ld id=%u",
				      (void *)ic,
				      io_op_type_to_str(ic->ic_op_type),
				      ic->ic_fd, (long)(age), ic->ic_id);

				// Mark as destroyed and clear active state in a single atomic operation
				uint64_t new_state =
					(ic->ic_state |
					 IO_CONTEXT_ENTRY_STATE_MARKED_DESTROYED) &
					~IO_CONTEXT_ENTRY_STATE_ACTIVE;
				__atomic_store(&ic->ic_state, &new_state,
					       __ATOMIC_SEQ_CST);

				count++;
			}
		}
	}

	pthread_mutex_unlock(&context_mutex);

	TRACE("Total orphaned contexts: %d", count);
	TRACE("======================");
}

void io_context_check_stalled(void)
{
	TRACE("=== Freeing Stalled Contexts ===");

	pthread_mutex_lock(&context_mutex);

	int count = 0;
	time_t now = time(NULL);
	int to = IO_CONTEXT_TIMEOUT;

	for (unsigned int i = 0; i < CONTEXT_HASH_SIZE; i++) {
		for (struct io_context *ic = context_hash[i]; ic != NULL;
		     ic = ic->ic_next) {
			// Only process active contexts that are write operations and have timed out
			if ((ic->ic_state & IO_CONTEXT_ENTRY_STATE_ACTIVE) &&
			    ic->ic_op_type == OP_TYPE_WRITE &&
			    (now - ic->ic_action_time) >= to) {
				trace_io_context(ic, __func__, __LINE__);

				// Mark as destroyed and clear active state atomically
				uint64_t new_state =
					(ic->ic_state |
					 IO_CONTEXT_ENTRY_STATE_MARKED_DESTROYED) &
					~IO_CONTEXT_ENTRY_STATE_ACTIVE;
				__atomic_store(&ic->ic_state, &new_state,
					       __ATOMIC_SEQ_CST);

				count++;
			}
		}
	}

	pthread_mutex_unlock(&context_mutex);

	TRACE("Total stalled contexts: %d", count);
	TRACE("======================");
}

#ifdef HAVE_DESTROY_CACHE
void io_context_release_destroyed(void)
{
	pthread_mutex_lock(&context_mutex);

	int count = 0;
	int limit = 100000; // Don't block for too long
	time_t now = time(NULL);

	TRACE("=== Freeing Destroyed Contexts ===");

	for (unsigned int i = 0; i < CONTEXT_HASH_SIZE && count < limit; i++) {
		struct io_context **prev_ptr = &context_hash[i];
		struct io_context *ic = context_hash[i];

		while (ic != NULL && count < limit) {
			// Check if it's marked as destroyed
			if ((ic->ic_state &
			     IO_CONTEXT_ENTRY_STATE_MARKED_DESTROYED) &&
			    !(ic->ic_state &
			      IO_CONTEXT_ENTRY_STATE_PENDING_FREE)) {
				// Mark as pending free
				__atomic_fetch_or(
					&ic->ic_state,
					IO_CONTEXT_ENTRY_STATE_PENDING_FREE,
					__ATOMIC_SEQ_CST);

				trace_io_context(ic, __func__, __LINE__);

				// Remove from hash table
				*prev_ptr = ic->ic_next;

				// Store the next pointer before freeing
				struct io_context *next = ic->ic_next;

				// Free the context
				free(ic->ic_buffer);
				free(ic);

				atomic_fetch_add(&context_freed, 1);
				atomic_fetch_add(&destroyed_freed, 1);
				count++;

				// Move to next context
				ic = next;
			} else {
				// Keep this entry, move to next one
				prev_ptr = &ic->ic_next;
				ic = ic->ic_next;
			}
		}
	}

	pthread_mutex_unlock(&context_mutex);

	TRACE("Total destroyed contexts: %d", count);
	TRACE("======================");
}
#endif

int io_context_fini(void)
{
	pthread_mutex_lock(&context_mutex);

	int count = 0;

	// Free all remaining contexts
	for (unsigned int i = 0; i < CONTEXT_HASH_SIZE; i++) {
		struct io_context *ic = context_hash[i];

		while (ic != NULL) {
			struct io_context *next = ic->ic_next;

			free(ic->ic_buffer);
			free(ic);

			count++;
			ic = next;
		}

		context_hash[i] = NULL;
	}

	pthread_mutex_unlock(&context_mutex);

	pthread_mutex_destroy(&context_mutex);

	if (count > 0) {
		TRACE("Freed %d remaining contexts during shutdown", count);
	}

	return 0;
}

void io_context_validate_hash_tables(void)
{
	pthread_mutex_lock(&context_mutex);

	int count = 0;
	int inconsistent_count = 0;
	int fixed_count = 0;

	bool warned = false;

	TRACE("=== Validating Context Hash Tables ===");

	for (unsigned int i = 0; i < CONTEXT_HASH_SIZE; i++) {
		for (struct io_context *ic = context_hash[i]; ic != NULL;
		     ic = ic->ic_next) {
			count++;

			// Check for inconsistent state
			if ((ic->ic_state & IO_CONTEXT_ENTRY_STATE_ACTIVE) &&
			    (ic->ic_state &
			     IO_CONTEXT_ENTRY_STATE_MARKED_DESTROYED)) {
				if (!warned) {
					LOG("WARNING: %p id=%u has inconsistent state 0x%lx",
					    (void *)ic, ic->ic_id,
					    (unsigned long)ic->ic_state);
					warned = true;
				}
				trace_io_context(ic, __func__, __LINE__);
				inconsistent_count++;

				// Fix the inconsistent state - contexts should not be both active and destroyed
				// Clear the active flag
				__atomic_fetch_and(
					&ic->ic_state,
					~IO_CONTEXT_ENTRY_STATE_ACTIVE,
					__ATOMIC_SEQ_CST);
				fixed_count++;
			}
		}
	}

	pthread_mutex_unlock(&context_mutex);

	TRACE("Examined %d contexts, found %d inconsistencies, fixed %d", count,
	      inconsistent_count, fixed_count);
	TRACE("======================");
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
	ics->ics_active_cancelled = 0; // Not used in new implementation
	ics->ics_active_destroyed = atomic_load(&active_destroyed);
	ics->ics_cancelled_freed = atomic_load(&cancelled_freed);
	ics->ics_destroyed_freed = atomic_load(&destroyed_freed);
}

void io_context_log_stats(void)
{
	TRACE("Context state transitions: active_cancelled=0, active_destroyed=%" PRIu64
	      ", cancelled_freed=%" PRIu64 ", destroyed_freed=%" PRIu64
	      " created=%" PRIu64 " freed=%" PRIu64,
	      atomic_load(&active_destroyed), atomic_load(&cancelled_freed),
	      atomic_load(&destroyed_freed), atomic_load(&context_created),
	      atomic_load(&context_freed));

	// Periodically validate hash tables to catch issues early
	io_context_validate_hash_tables();
}
