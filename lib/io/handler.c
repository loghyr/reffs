/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_JEMALLOC
#include <jemalloc/jemalloc.h>
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
#include <signal.h>
#include <urcu.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <time.h>
#include <stdbool.h>

#include "reffs/log.h"
#include "reffs/rpc.h"
#include "reffs/nfs3.h"
#include "reffs/mount3.h"
#include "reffs/network.h"
#include "reffs/server.h"
#include "reffs/task.h"
#include "reffs/test.h"
#include "reffs/io.h"
#include "reffs/trace/io.h"

// Request tracking
struct rpc_trans *pending_requests[MAX_PENDING_REQUESTS];
pthread_mutex_t request_mutex = PTHREAD_MUTEX_INITIALIZER;

static time_t last_accept_check;

// Set of listener sockets to monitor
int listener_fds[MAX_LISTENERS];
int num_listeners = 0;

//
// Need to prune out connections that get timed out
//

// Store buffer states by fd
struct buffer_state *conn_buffers[MAX_CONNECTIONS];

// Register a new request for tracking
int io_register_request(struct rpc_trans *rt)
{
	pthread_mutex_lock(&request_mutex);

	LOG("rt=%p xid=0x%08x", (void *)rt, rt->rt_info.ri_xid);
	// Find an empty slot
	for (int i = 0; i < MAX_PENDING_REQUESTS; i++) {
		if (pending_requests[i] == NULL) {
			LOG("rt=%p xid=0x%08x", (void *)rt, rt->rt_info.ri_xid);
			pending_requests[i] = rt;
			pthread_mutex_unlock(&request_mutex);
			return 0;
		}
	}

	pthread_mutex_unlock(&request_mutex);
	return ENOENT;
}

// Find a request by XID
struct rpc_trans *io_find_request_by_xid(uint32_t xid)
{
	struct rpc_trans *rt = NULL;

	LOG("xid=0x%08x", xid);
	pthread_mutex_lock(&request_mutex);
	for (int i = 0; i < MAX_PENDING_REQUESTS; i++) {
		if (pending_requests[i] &&
		    pending_requests[i]->rt_info.ri_xid == xid) {
			LOG("rt=%p xid=0x%08x", (void *)pending_requests[i],
			    xid);
			rt = pending_requests[i];
			break;
		}
	}
	pthread_mutex_unlock(&request_mutex);

	return rt;
}

int io_unregister_request(uint32_t xid)
{
	LOG("xid=0x%08x", xid);
	pthread_mutex_lock(&request_mutex);
	for (int i = 0; i < MAX_PENDING_REQUESTS; i++) {
		if (pending_requests[i] &&
		    pending_requests[i]->rt_info.ri_xid == xid) {
			LOG("rt=%p xid=0x%08x", (void *)pending_requests[i],
			    xid);
			pending_requests[i] = NULL;
			return 0;
		}
	}
	pthread_mutex_unlock(&request_mutex);

	return ENOENT;
}

// Register client fd
void io_client_fd_register(int fd)
{
	// In this simplified version, we just track buffer state
	io_buffer_state_create(fd);
}

// Unregister client fd
void io_client_fd_unregister(int fd)
{
	struct buffer_state *bs = io_buffer_state_get(fd);
	if (bs) {
		free(bs->bs_data);
		if (bs->bs_record.rs_data) {
			free(bs->bs_record.rs_data);
		}
		free(bs);
		conn_buffers[fd % MAX_CONNECTIONS] = NULL;
	}
}

// Initialize io_uring with larger CQ ring
static int setup_io_uring(struct ring_context *rc)
{
	struct io_uring_params params = { 0 };

	// Request NODROP feature to prevent losing events
	params.flags = IORING_SETUP_CQSIZE;
	params.cq_entries = 4 * QUEUE_DEPTH;

	if (io_uring_queue_init_params(QUEUE_DEPTH, &rc->rc_ring, &params) <
	    0) {
		LOG("io_uring_queue_init_params: %s", strerror(errno));
		return -1;
	}

	// Check if NODROP feature is supported
	if (params.features & IORING_FEAT_NODROP) {
		LOG("io_uring NODROP feature is supported - CQ entries won't be lost");
	} else {
		LOG("WARNING: io_uring NODROP feature not supported - CQ overflow will drop entries");
	}

	LOG("Initialized io_uring with SQ size %d, CQ size %d",
	    params.sq_entries, params.cq_entries);
	return 0;
}

int io_handler_init(struct ring_context *rc)
{
	// Initialize pending requests array
	memset(conn_buffers, 0, sizeof(conn_buffers));

	if (pthread_mutex_init(&rc->rc_mutex, NULL) != 0) {
		LOG("Failed to initialize ring mutex");
		return -1;
	}

	// Setup io_uring
	if (setup_io_uring(rc) < 0)
		return -1;

	last_accept_check = time(NULL);

	if (io_context_init())
		return -1;

	SSL_library_init();
	OpenSSL_add_all_algorithms();
	SSL_load_error_strings();

	io_conn_init();

	return 0;
}

// Create buffer state for a connection
struct buffer_state *io_buffer_state_create(int fd)
{
	struct buffer_state *bs = malloc(sizeof(struct buffer_state));
	if (!bs)
		return NULL;

	bs->bs_fd = fd;
	bs->bs_capacity = BUFFER_SIZE * 2;
	bs->bs_data = malloc(bs->bs_capacity);
	bs->bs_filled = 0;

	// Initialize record state
	bs->bs_record.rs_last_fragment = false;
	bs->bs_record.rs_fragment_len = 0;
	bs->bs_record.rs_data = NULL;
	bs->bs_record.rs_total_len = 0;
	bs->bs_record.rs_capacity = 0;
	bs->bs_record.rs_position = 0;

	if (!bs->bs_data) {
		free(bs);
		return NULL;
	}

	conn_buffers[fd % MAX_CONNECTIONS] = bs;
	return bs;
}

// Get buffer state for a connection
struct buffer_state *io_buffer_state_get(int fd)
{
	return conn_buffers[fd % MAX_CONNECTIONS];
}

// Append data to a buffer, resizing if necessary
bool io_buffer_append(struct buffer_state *bs, const char *data, size_t len)
{
	// Avoid integer overflow in capacity calculation
	if (len > SIZE_MAX / 2 || bs->bs_filled > SIZE_MAX - len) {
		return false; // Prevent integer overflow
	}

	// Check if we need to resize
	if (bs->bs_filled + len > bs->bs_capacity) {
		// Calculate new capacity, ensuring we allocate enough space
		size_t min_needed = bs->bs_filled + len;
		size_t new_capacity = bs->bs_capacity;

		// Double capacity until it's enough
		while (new_capacity < min_needed) {
			if (new_capacity > SIZE_MAX / 2) {
				// Would overflow
				return false;
			}
			new_capacity *= 2;
		}

		// Perform reallocation
		char *new_data = realloc(bs->bs_data, new_capacity);
		if (!new_data)
			return false;

		bs->bs_data = new_data;
		bs->bs_capacity = new_capacity;
	}

	// Append the data after successful reallocation
	memcpy(bs->bs_data + bs->bs_filled, data, len);
	bs->bs_filled += len;

	return true;
}

volatile sig_atomic_t *running_context;

void io_handler_stop(void)
{
	if (running_context)
		*running_context = 0;
}

void io_add_listener(int fd)
{
	if (fd > 0)
		listener_fds[num_listeners++] = fd;
}

void io_check_for_listener_restart(int fd, struct connection_info *ci,
				   struct ring_context *rc)
{
	for (int i = 0; i < num_listeners; i++) {
		if (fd == listener_fds[i]) {
			LOG("Listener socket closed, immediately resubmitting accept");
			io_request_accept_op(fd, ci, rc);
			break;
		}
	}
}

int *io_heartbeat_get_listeners(int *num)
{
	*num = num_listeners;
	return listener_fds;
}

void io_handler_main_loop(volatile sig_atomic_t *running_flag,
			  struct ring_context *rc)
{
	struct io_uring_cqe *cqe;

	running_context = running_flag;

	// Initialize heartbeat system
	if (io_heartbeat_init(rc) < 0) {
		LOG("Failed to initialize heartbeat system");
		return;
	}

	while (1) {
		// Set a timeout for io_uring_wait_cqe to allow checking the running flag
		struct __kernel_timespec ts = { .tv_sec = IO_URING_WAIT_SEC,
						.tv_nsec = IO_URING_WAIT_NSEC };

		// Check if we're still supposed to be running
		int running_local;
		__atomic_load(running_flag, &running_local, __ATOMIC_SEQ_CST);
		if (!running_local) {
			LOG("Detected shutdown flag, breaking main loop");
			break;
		}

		// Wait for completion events
		int ret = io_uring_wait_cqe_timeout(&rc->rc_ring, &cqe, &ts);

		if (ret == -ETIME) {
			// Timeout - check running flag and continue
			continue;
		} else if (ret == -EINTR) {
			// Interrupted system call - this is normal when using pstack or other debugging
			LOG("io_uring_wait_cqe_timeout interrupted, continuing");
			continue;
		} else if (ret < 0) {
			LOG("io_uring_wait_cqe_timeout error: %s",
			    strerror(-ret));
			// Maybe add a small sleep to avoid spinning on persistent errors
			usleep(10000); // 10ms
			continue;
		}

		// Update completion count for statistics
		io_heartbeat_update_completions(1);

		// Get the io_context from the user_data
		struct io_context *ic =
			(struct io_context *)(uintptr_t)cqe->user_data;
		if (!ic) {
			LOG("Error: NULL io context");
			io_uring_cqe_seen(&rc->rc_ring, cqe);
			continue;
		}

		// trace_io_context(ic, __func__, __LINE__);

		// Handle the completion based on the operation type
		if (cqe->res == -ECANCELED) {
			trace_io_context(ic, __func__, __LINE__);

			LOG("Operation was cancelled: cqe=%p %p op=%s fd=%d id=%u",
			    (void *)cqe, (void *)ic,
			    io_op_type_to_str(ic->ic_op_type), ic->ic_fd,
			    ic->ic_id);
		} else if (cqe->res < 0) {
			LOG("CQE error for op=%s, fd=%d: %s",
			    io_op_type_to_str(ic->ic_op_type), ic->ic_fd,
			    strerror(-cqe->res));

			trace_io_context(ic, __func__, __LINE__);

			// Handle timeout for heartbeat operation specially
			if (ic->ic_op_type == OP_TYPE_HEARTBEAT) {
				ret = io_handle_heartbeat(ic, cqe->res, rc);
			} else {
				io_socket_close(ic->ic_fd, -cqe->res);
				io_context_destroy(ic);
			}
		} else {
			uint64_t state;
			__atomic_load(&ic->ic_state, &state, __ATOMIC_RELAXED);
			if (!(state & IO_CONTEXT_ENTRY_STATE_ACTIVE)) {
				LOG("Received completion for non-active context: ic=%p op=%s fd=%d id=%u state=0x%lx",
				    (void *)ic,
				    io_op_type_to_str(ic->ic_op_type),
				    ic->ic_fd, ic->ic_id, state);

				// For TLS handshake, we need to process even non-active contexts
				if (ic->ic_op_type == OP_TYPE_WRITE &&
				    (ic->ic_state &
				     IO_CONTEXT_DIRECT_TLS_DATA)) {
					struct conn_info *ci =
						io_conn_get(ic->ic_fd);
					if (ci &&
					    (ci->ci_tls_handshaking ||
					     ci->ci_handshake_final_pending)) {
						LOG("Processing TLS write completion for special context state");
						ret = io_handle_write(
							ic, cqe->res, rc);
					} else {
						LOG("Skipping processing for non-TLS-handshake context");
					}
				}

				trace_io_context(ic, __func__, __LINE__);
				io_uring_cqe_seen(&rc->rc_ring, cqe);
				continue;
			}

			switch (ic->ic_op_type) {
			case OP_TYPE_ACCEPT:
				ret = io_handle_accept(ic, cqe->res, rc);
				break;

			case OP_TYPE_READ:
				ret = io_handle_read(ic, cqe->res, rc);
				break;

			case OP_TYPE_WRITE:
				ret = io_handle_write(ic, cqe->res, rc);
				break;

			case OP_TYPE_CONNECT:
				ret = io_handle_connect(ic, cqe->res, rc);
				break;

			case OP_TYPE_HEARTBEAT:
				ret = io_handle_heartbeat(ic, cqe->res, rc);
				break;

			default:
				LOG("Unknown operation type: %d",
				    ic->ic_op_type);
				io_context_destroy(ic);
				ret = 0;
				break;
			}
		}

		io_uring_cqe_seen(&rc->rc_ring, cqe);
	}
}

void io_handler_fini(struct ring_context *rc)
{
	io_conn_cleanup();

	// Drain pending io_uring operations
	while (1) {
		struct io_uring_cqe *cqe;
		struct __kernel_timespec ts = { .tv_sec = 0,
						.tv_nsec = 100000000 };

		int ret = io_uring_wait_cqe_timeout(&rc->rc_ring, &cqe, &ts);
		if (ret == -ETIME) {
			// No more completions
			break;
		} else if (ret < 0) {
			LOG("Error draining io_uring: %s", strerror(-ret));
			break;
		}

		// Free associated resources
		if (cqe->user_data) {
			struct io_context *ic =
				(struct io_context *)(uintptr_t)cqe->user_data;
			io_context_destroy(ic);
		}

		io_uring_cqe_seen(&rc->rc_ring, cqe);
	}

	// Wait for worker threads to finish
	wait_for_worker_threads();

	// Cleanup any pending requests
	LOG("Cleaning up pending requests...");
	pthread_mutex_lock(&request_mutex);
	for (int i = 0; i < MAX_PENDING_REQUESTS; i++) {
		if (pending_requests[i]) {
			rpc_protocol_free(pending_requests[i]);
			pending_requests[i] = NULL;
		}
	}
	pthread_mutex_unlock(&request_mutex);

	// Cleanup connection buffers
	for (int i = 0; i < MAX_CONNECTIONS; i++) {
		if (conn_buffers[i]) {
			if (conn_buffers[i]->bs_data) {
				free(conn_buffers[i]->bs_data);
			}
			if (conn_buffers[i]->bs_record.rs_data) {
				free(conn_buffers[i]->bs_record.rs_data);
			}
			free(conn_buffers[i]);
		}
	}

	LOG("Cleaning up remaining active contexts...");
	io_context_release_active();

	io_context_fini();

	io_uring_queue_exit(&rc->rc_ring);
	pthread_mutex_destroy(&rc->rc_mutex);
}
