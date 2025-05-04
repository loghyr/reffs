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

// Initialize io_uring with larger CQ ring
static int setup_io_uring(struct io_uring *ring)
{
	struct io_uring_params params = { 0 };

	// Request NODROP feature to prevent losing events
	params.flags = IORING_SETUP_CQSIZE;
	params.cq_entries = 4 * QUEUE_DEPTH;

	if (io_uring_queue_init_params(QUEUE_DEPTH, ring, &params) < 0) {
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

int io_handler_init(struct io_uring *ring)
{
	// Initialize pending requests array
	memset(conn_buffers, 0, sizeof(conn_buffers));

	// Setup io_uring
	if (setup_io_uring(ring) < 0)
		return -1;

	last_accept_check = time(NULL);

	if (io_context_init())
		return -1;

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

void io_add_listener(int fd)
{
	if (fd > 0)
		listener_fds[num_listeners++] = fd;
}

void io_check_for_listener_restart(int fd, struct connection_info *ci,
				   struct io_uring *ring)
{
	for (int i = 0; i < num_listeners; i++) {
		if (fd == listener_fds[i]) {
			LOG("Listener socket closed, immediately resubmitting accept");
			request_accept_op(fd, ci, ring);
			break;
		}
	}
}

void io_handler_main_loop(volatile sig_atomic_t *running_flag,
			  struct io_uring *ring)
{
	struct io_uring_cqe *cqe;

	static uint64_t total_completions = 0;
	static time_t last_stat_time = 0;
	static uint64_t last_completions = 0;
	static time_t last_overflow_check = 0;

	io_conn_init();

	running_context = running_flag;

	while (1) {
		// Set a timeout for io_uring_wait_cqe to allow checking the running flag
		struct __kernel_timespec ts = { .tv_sec = IO_URING_WAIT_SEC,
						.tv_nsec = IO_URING_WAIT_NSEC };

		static time_t last_check = 0;
		time_t now = time(NULL);

		static time_t last_heartbeat = 0;
		if (now - last_heartbeat >=
		    60) { // Log heartbeat every 60 seconds
			last_heartbeat = now;
			LOG("HEARTBEAT: Main loop is running at timestamp %ld ctx(c=%ld, f=%ld) lsnrs=%d",
			    (long)now, io_context_get_created(),
			    io_context_get_freed(), num_listeners);
			io_context_list_active(false);
			io_context_check_stalled(ring);
			io_context_release_cancelled();
			io_context_release_destroyed();
			io_context_log_stats();
		}

		if (now - last_overflow_check >= 10) {
			if (io_uring_cq_has_overflow(ring)) {
				LOG("WARNING: CQ ring overflow detected! Context count: %ld",
				    io_context_get_created() -
					    io_context_get_freed());

				last_overflow_check = now;

				// Try to flush events from overflow
				int ret = io_uring_get_events(ring);
				if (ret < 0) {
					LOG("Error getting events: %s",
					    strerror(-ret));
				} else {
					LOG("Flushed %d events from overflow",
					    ret);
				}
			}
		}

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
					if (conn &&
					    conn->ci_state != CONN_LISTENING &&
					    conn->ci_accept_count == 0) {
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

				// Define timeout in seconds
				const int conn_timeout = 60; // 1 minute timeout

				// Scan all active connections
				for (int fd = 3; fd < MAX_CONNECTIONS; fd++) {
					struct conn_info *conn =
						io_conn_get(fd);
					if (conn &&
					    conn->ci_state == CONN_CONNECTED) {
						// Check for stale connections
						if (now - conn->ci_last_activity >
						    conn_timeout) {
							LOG("Connection fd=%d inactive for %ld seconds - closing",
							    fd,
							    (long)(now -
								   conn->ci_last_activity));
							io_socket_close(
								fd, ETIMEDOUT);
							continue;
						}

						// Ensure each active connection has a pending read operation
						if (conn->ci_read_count == 0) {
							LOG("Connection fd=%d has no pending read operations - submitting read",
							    fd);
							int ret =
								request_additional_read_data(
									fd,
									NULL,
									ring);
							if (ret != 0) {
								LOG("Failed to submit read for fd=%d: %s",
								    fd,
								    strerror(
									    ret));
								// If we can't submit a read, the connection is effectively dead
								io_socket_close(
									fd,
									ret);
							}
						} else if (conn->ci_read_count >
							   1) {
							// This is a potential issue - more than one reader
							LOG("Warning: Connection fd=%d has %d pending read operations",
							    fd,
							    conn->ci_read_count);
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

		total_completions++;
		if (now - last_stat_time >= 10) {
			uint64_t rate = (total_completions - last_completions) /
					(now - last_stat_time);
			LOG("Completion processing rate: %lu/sec (total: %lu)",
			    rate, total_completions);
			last_completions = total_completions;
			last_stat_time = now;
		}
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

		trace_io_context_ptr((void *)(uintptr_t)cqe->user_data,
				     __func__, __LINE__); // loghyr

		struct io_context *ic =
			(struct io_context *)(uintptr_t)cqe->user_data;
		if (!ic) {
			LOG("Error: NULL io context");
			io_uring_cqe_seen(ring, cqe);
			continue;
		}

		trace_io_context(ic, __func__, __LINE__); // loghyr

		if (cqe->res == -ECANCELED) {
			trace_io_context(ic, __func__, __LINE__); // loghyr

			LOG("Operation was cancelled: cqe=%p %p op=%s fd=%d id=%u",
			    (void *)cqe, (void *)ic,
			    io_op_type_to_str(ic->ic_op_type), ic->ic_fd,
			    ic->ic_id);
		} else if (cqe->res < 0) {
			LOG("CQE error for op=%s, fd=%d: %s",
			    io_op_type_to_str(ic->ic_op_type), ic->ic_fd,
			    strerror(-cqe->res));

			trace_io_context(ic, __func__, __LINE__); // loghyr

			io_socket_close(ic->ic_fd, -cqe->res);
			io_context_destroy(ic);
		} else {
			uint64_t state;
			__atomic_load(&ic->ic_state, &state, __ATOMIC_RELAXED);
			if (state & IO_CONTEXT_IS_DESTROYED) {
				trace_io_context(ic, __func__, __LINE__);
				abort();
				continue;
			} else if (state & IO_CONTEXT_IS_CANCELLED) {
				io_uring_cqe_seen(ring, cqe);
				trace_io_context(ic, __func__, __LINE__);
				continue;
			}

			switch (ic->ic_op_type) {
			case OP_TYPE_ACCEPT:
				ret = io_handle_accept(ic, cqe->res, ring);
				break;

			case OP_TYPE_READ:
				trace_io_context(ic, __func__,
						 __LINE__); // loghyr
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
				io_context_destroy(ic);
				ret = 0;
				break;
			}
		}

		io_uring_cqe_seen(ring, cqe);
	}

	io_conn_cleanup();
}

void io_handler_fini(struct io_uring *ring)
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
			io_context_destroy(ic);
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

	LOG("Cleaning up remaining active contexts...");
	io_context_release_active(ring);

	io_context_fini();
}
