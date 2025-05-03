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

//
// Need to prune out connections that get timed out
//

// Store buffer states by fd
struct buffer_state *conn_buffers[MAX_CONNECTIONS];

// Register a new request for tracking
int io_register_request(struct rpc_trans *rt)
{
	pthread_mutex_lock(&request_mutex);

	// Find an empty slot
	for (int i = 0; i < MAX_PENDING_REQUESTS; i++) {
		if (pending_requests[i] == NULL) {
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

	pthread_mutex_lock(&request_mutex);
	for (int i = 0; i < MAX_PENDING_REQUESTS; i++) {
		if (pending_requests[i] &&
		    pending_requests[i]->rt_info.ri_xid == xid) {
			rt = pending_requests[i];
			break;
		}
	}
	pthread_mutex_unlock(&request_mutex);

	return rt;
}

int io_unregister_request(uint32_t xid)
{
	pthread_mutex_lock(&request_mutex);
	for (int i = 0; i < MAX_PENDING_REQUESTS; i++) {
		if (pending_requests[i] &&
		    pending_requests[i]->rt_info.ri_xid == xid) {
			pending_requests[i] = NULL;
			return 0;
		}
	}
	pthread_mutex_unlock(&request_mutex);

	return ENOENT;
}

// Register client fd
void register_client_fd(int fd)
{
	// In this simplified version, we just track buffer state
	create_buffer_state(fd);
}

// Unregister client fd
void unregister_client_fd(int fd)
{
	struct buffer_state *bs = get_buffer_state(fd);
	if (bs) {
		free(bs->bs_data);
		if (bs->bs_record.rs_data) {
			free(bs->bs_record.rs_data);
		}
		free(bs);
		conn_buffers[fd % MAX_CONNECTIONS] = NULL;
	}
}

// Initialize io_uring
static int setup_io_uring(struct io_uring *ring)
{
	if (io_uring_queue_init(QUEUE_DEPTH, ring, 0) < 0) {
		LOG("io_uring_queue_init: %s", strerror(errno));
		return -1;
	}
	return 0;
}

int io_handler_init(struct io_uring *ring)
{
	// Initialize pending requests array
	memset(conn_buffers, 0, sizeof(conn_buffers));

	// Setup io_uring
	if (setup_io_uring(ring) < 0) {
		return -1;
	}

	last_accept_check = time(NULL);

	return 0;
}

// Create buffer state for a connection
struct buffer_state *create_buffer_state(int fd)
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
struct buffer_state *get_buffer_state(int fd)
{
	return conn_buffers[fd % MAX_CONNECTIONS];
}

// Append data to a buffer, resizing if necessary
bool append_to_buffer(struct buffer_state *bs, const char *data, size_t len)
{
	// Check if we need to resize
	if (bs->bs_filled + len > bs->bs_capacity) {
		size_t new_capacity = bs->bs_capacity * 2;
		char *new_data = realloc(bs->bs_data, new_capacity);
		if (!new_data)
			return false;

		bs->bs_data = new_data;
		bs->bs_capacity = new_capacity;
	}

	// Append the data
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


// Set of listener sockets to monitor
int listener_fds[MAX_LISTENERS];
int num_listeners = 0;

void io_add_listener(int fd)
{
	if (fd > 0)
		listener_fds[num_listeners++] = fd;
}

void io_handler_main_loop(volatile sig_atomic_t *running_flag,
			  struct io_uring *ring)
{
	struct io_uring_cqe *cqe;

	io_conn_init();

	running_context = running_flag;

	while (1) {
		// Set a timeout for io_uring_wait_cqe to allow checking the running flag
		struct __kernel_timespec ts = { .tv_sec = IO_URING_WAIT_SEC,
						.tv_nsec = IO_URING_WAIT_NSEC };

		static time_t last_check = 0;
		time_t now = time(NULL);
		if (now - last_check >= 1) { // Check signal flag every second
			int running_local;
			__atomic_load(running_flag, &running_local,
				      __ATOMIC_SEQ_CST);
			if (!running_local) {
				LOG("Detected shutdown flag, breaking main loop");
				break;
			}

			if (now - last_accept_check >= 5) {
				bool accept_failures = false;

				// Check each listener
				for (int i = 0; i < num_listeners; i++) {
					int fd = listener_fds[i];
					if (fd <= 0)
						continue;

					struct conn_info *conn =
						io_conn_get(fd);
					if (!conn ||
					    conn->ci_state != CONN_LISTENING) {
						LOG("Listener fd=%d not in LISTENING state - resubmitting accept",
						    fd);

						// Try to resubmit accept operation
						int ret = request_accept_op(
							fd, NULL, ring);
						if (ret != 0) {
							accept_failures = true;
							LOG("Watchdog failed to resubmit accept for fd=%d: %s",
							    fd, strerror(ret));
						} else {
							LOG("Watchdog successfully resubmitted accept for fd=%d",
							    fd);
						}
					}
				}

				// If we had failures, check again more quickly next time
				last_accept_check = now;
				if (accept_failures) {
					// Force another check sooner than usual
					last_accept_check -= 3;
				}
			}
		}

		int ret = io_uring_wait_cqe_timeout(ring, &cqe, &ts);
		if (ret == -ETIME) {
			// Timeout - check running flag and continue
			continue;
		} else if (ret < 0) {
			LOG("io_uring_wait_cqe_timeout error: %s",
			    strerror(-ret));
			continue;
		}

		struct io_context *ic =
			(struct io_context *)(uintptr_t)cqe->user_data;
		if (!ic) {
			LOG("Error: NULL io context");
			io_uring_cqe_seen(ring, cqe);
			continue;
		}

		if (cqe->res < 0) {
			LOG("CQE error for op=%s, fd=%d: %s",
			    op_type_to_str(ic->ic_op_type), ic->ic_fd,
			    strerror(-cqe->res));

			io_socket_close(ic->ic_fd, -cqe->res);
			io_context_free(ic);
		} else {
			switch (ic->ic_op_type) {
			case OP_TYPE_ACCEPT:
				ret = io_handle_accept(ic, cqe->res, ring);
				break;

			case OP_TYPE_READ:
				ret = io_handle_read(ic, cqe->res, ring);
				break;

			case OP_TYPE_WRITE:
				ret = io_handle_write(ic, cqe->res, ring);
				break;

			case OP_TYPE_CONNECT:
				ret = io_handle_connect(ic, cqe->res, ring);
				break;

			default:
				LOG("Unknown operation type: %d",
				    ic->ic_op_type);
				io_context_free(ic);
				ret = 0;
				break;
			}
		}

		io_uring_cqe_seen(ring, cqe);
	}

	io_conn_cleanup();
}

void io_handler_cleanup(struct io_uring *ring)
{
	// Drain pending io_uring operations
	while (1) {
		struct io_uring_cqe *cqe;
		struct __kernel_timespec ts = { .tv_sec = 0,
						.tv_nsec = 100000000 };

		int ret = io_uring_wait_cqe_timeout(ring, &cqe, &ts);
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
			if (ic) {
				io_context_free(ic);
			}
		}

		io_uring_cqe_seen(ring, cqe);
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
}
