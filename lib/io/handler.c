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

// NFS request context for tracking operations
struct nfs_request_context {
	uint32_t nrc_xid; // RPC transaction ID
	int nrc_operation; // NFS operation (GETATTR, READ, etc.)
	int nrc_sockfd; // Socket this request was sent on
	void *nrc_private_data; // Application-specific context
	void (*nrc_cb)(struct nfs_request_context *nrc, void *response,
		       int res_len, int status);
	char *nrc_buffer; // Buffer for response
};

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"

// Request tracking
struct nfs_request_context *pending_requests[MAX_PENDING_REQUESTS];
pthread_mutex_t request_mutex = PTHREAD_MUTEX_INITIALIZER;

// Store buffer states by fd
struct buffer_state *conn_buffers[MAX_CONNECTIONS];

// Generate a unique transaction ID for RPC
static uint32_t generate_xid(void)
{
	static uint32_t next_xid = 1;
	static pthread_mutex_t xid_mutex = PTHREAD_MUTEX_INITIALIZER;

	pthread_mutex_lock(&xid_mutex);
	uint32_t xid = next_xid++;
	pthread_mutex_unlock(&xid_mutex);

	return xid;
}

// Register a new request for tracking
static int register_request(struct nfs_request_context *nrc)
{
	pthread_mutex_lock(&request_mutex);

	// Find an empty slot
	for (int i = 0; i < MAX_PENDING_REQUESTS; i++) {
		if (pending_requests[i] == NULL) {
			pending_requests[i] = nrc;
			pthread_mutex_unlock(&request_mutex);
			return 0;
		}
	}

	pthread_mutex_unlock(&request_mutex);
	return -1; // No free slots
}

// Find a request by XID
static struct nfs_request_context *find_request_by_xid(uint32_t xid)
{
	struct nfs_request_context *nrc = NULL;

	pthread_mutex_lock(&request_mutex);
	for (int i = 0; i < MAX_PENDING_REQUESTS; i++) {
		if (pending_requests[i] &&
		    pending_requests[i]->nrc_xid == xid) {
			nrc = pending_requests[i];
			pending_requests[i] = NULL; // Remove from tracking
			break;
		}
	}
	pthread_mutex_unlock(&request_mutex);

	return nrc;
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

// Handler for GETATTR response
static void handle_getattr_response(struct nfs_request_context *nrc,
				    void *response,
				    int __attribute__((unused)) res_len,
				    int status)
{
	LOG(
	      "GETATTR response received: xid=0x%08x, status=%d", nrc->nrc_xid,
	      status);

	if (status == 0 && response) {
		LOG("File attributes received");
		// In a real implementation, you would process the attributes here
	} else {
		LOG("GETATTR failed");
	}

	// Free the context
	if (nrc->nrc_private_data) {
		free(nrc->nrc_private_data);
	}
	if (nrc->nrc_buffer) {
		free(nrc->nrc_buffer);
	}
	free(nrc);
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

// Send NFS response using io_uring
static int send_nfs_response(struct io_uring *ring, int fd, char *buffer,
			     int len)
{
	// Prefix with record marker (last fragment + length)
	uint32_t marker = htonl(0x80000000 | len);

	// Prepare combined buffer with marker + data
	char *send_buffer = malloc(len + 4);
	if (!send_buffer) {
		return -ENOMEM;
	}

	// Copy marker and data
	memcpy(send_buffer, &marker, 4);
	memcpy(send_buffer + 4, buffer, len);

	// Missing error checking since no callers yet

	// Create an IO context for the write operation
	struct io_context *ic =
		io_context_create(OP_TYPE_WRITE, fd, send_buffer, len + 4);
	if (!ic) {
		free(send_buffer);
		return -ENOMEM;
	}

	// Submit the write operation to io_uring
	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
	io_uring_prep_write(sqe, fd, send_buffer, len + 4, 0);

	// Associate with the io context
	sqe->user_data = (uint64_t)(uintptr_t)ic;
	trace_io_write_submit(ic);

	io_uring_submit(ring);
	return 0;
}

void io_handler_main_loop(volatile sig_atomic_t *running_flag,
			  struct io_uring *ring)
{
	struct io_uring_cqe *cqe;

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
				LOG(
				      "Detected shutdown flag, breaking main loop");
				break;
			}
			last_check = now;
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

			unregister_client_fd(ic->ic_fd);
			close(ic->ic_fd);
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

			case OP_TYPE_RPC_REQ:
				io_context_free(ic);
				ret = 0;
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
			if (pending_requests[i]->nrc_private_data) {
				free(pending_requests[i]->nrc_private_data);
			}
			if (pending_requests[i]->nrc_buffer) {
				free(pending_requests[i]->nrc_buffer);
			}
			free(pending_requests[i]);
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
