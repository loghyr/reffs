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
#include <rpc/clnt.h>
#include <rpc/rpc_msg.h>
#include <getopt.h>
#include <rpc/pmap_clnt.h>

#include "nfsv3_xdr.h"
#include "mntv3_xdr.h"

#include "reffs/log.h"
#include "reffs/rpc.h"
#include "reffs/nfs3.h"
#include "reffs/mount3.h"
#include "reffs/network.h"
#include "reffs/server.h"
#include "reffs/super_block.h"
#include "reffs/task.h"
#include "reffs/test.h"
#include "reffs/ns.h"

#define BUFFER_SIZE 4096
#define QUEUE_DEPTH 1024
#define NFS_PORT 2049
#define NUM_LISTENERS 1
#define MAX_WORKER_THREADS 4
#define MAX_PENDING_REQUESTS 256
#define MAX_CONNECTIONS 1024 // Maximum number of concurrent client connections

#define IO_URING_WAIT_SEC (0)
#define IO_URING_WAIT_NSEC (100000000)

#define IO_URING_WAIT_US \
	((IO_URING_WAIT_SEC * 1000000) + (IO_URING_WAIT_NSEC / 1000))

#define MAX_RETRIES (3)

// Global flag for clean shutdown
volatile sig_atomic_t running = 1;

// Opcodes for different packet types
enum op_type {
	OP_TYPE_ACCEPT = 1,
	OP_TYPE_READ = 2,
	OP_TYPE_WRITE = 3,
	OP_TYPE_CONNECT = 4,
	OP_TYPE_RPC_REQ = 5
};

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"

static inline const char *op_type_to_str(enum op_type op)
{
	switch (op) {
	case OP_TYPE_ACCEPT:
		return "ACCEPT";
	case OP_TYPE_READ:
		return "READ";
	case OP_TYPE_WRITE:
		return "WRITE";
	case OP_TYPE_CONNECT:
		return "CONNECT";
	case OP_TYPE_RPC_REQ:
		return "RPC_REQ";
	}

	return "unknown";
}

// IO operation context structure
struct io_context {
	enum op_type ic_op_type;
	int ic_fd;
	uint32_t ic_id;
	void *ic_buffer;

	size_t ic_buffer_len;
	size_t ic_position;
	uint32_t ic_xid;

	struct connection_info ic_ci;
};

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

// Record state for reassembling fragmented RPC messages
struct record_state {
	bool rs_last_fragment;
	uint32_t rs_fragment_len;
	char *rs_data;
	size_t rs_total_len;
	size_t rs_capacity;
	uint32_t rs_position;
};

// Connection buffer state for reassembling messages
struct buffer_state {
	int bs_fd;
	char *bs_data;
	size_t bs_filled;
	size_t bs_capacity;
	struct record_state bs_record;
};

// Request tracking
struct nfs_request_context *pending_requests[MAX_PENDING_REQUESTS];
pthread_mutex_t request_mutex = PTHREAD_MUTEX_INITIALIZER;

// Queue for worker threads
pthread_mutex_t task_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t task_queue_cond = PTHREAD_COND_INITIALIZER;
struct task *task_queue[QUEUE_DEPTH];
int task_queue_head = 0;
int task_queue_tail = 0;

// Thread management
pthread_t worker_threads[MAX_WORKER_THREADS];
int num_worker_threads = 0;

// Store buffer states by fd
struct buffer_state *conn_buffers[MAX_CONNECTIONS];

// Forward declarations
static int setup_io_uring(struct io_uring *ring);
static struct buffer_state *create_buffer_state(int fd);
static struct buffer_state *get_buffer_state(int fd);
static bool append_to_buffer(struct buffer_state *bs, const char *data,
			     size_t len);
static int decode_nfs_response(char *data, size_t filled, struct io_uring *ring,
			       int client_fd);
static void register_client_fd(int fd);
static void unregister_client_fd(int fd);

// Signal handler
static void signal_handler(int sig)
{
	TRACE(REFFS_TRACE_LEVEL_ERR,
	      "Received signal %d, initiating shutdown...", sig);
	running = 0;

	// Wake up any waiting worker threads
	pthread_cond_broadcast(&task_queue_cond);
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

// Add task to the worker queue
static void add_task(struct task *task)
{
	pthread_mutex_lock(&task_queue_mutex);
	task_queue[task_queue_tail] = task;
	task_queue_tail = (task_queue_tail + 1) % QUEUE_DEPTH;
	pthread_cond_signal(&task_queue_cond);
	pthread_mutex_unlock(&task_queue_mutex);
}

// Register client fd
static void register_client_fd(int fd)
{
	// In this simplified version, we just track buffer state
	create_buffer_state(fd);
}

// Unregister client fd
static void unregister_client_fd(int fd)
{
	TRACE(REFFS_TRACE_LEVEL_ERR,
	      "Unregistering existing buffer state for %d", fd);
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

static int context_created = 0;
static int context_freed = 0;

// Create an IO context for operations
static struct io_context *io_context_create(enum op_type op_type, int fd,
					    void *buffer, size_t buffer_len)
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

static void io_context_free(struct io_context *ic)
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

// Handler for GETATTR response
static void handle_getattr_response(struct nfs_request_context *nrc,
				    void *response,
				    int __attribute__((unused)) res_len,
				    int status)
{
	TRACE(REFFS_TRACE_LEVEL_WARNING,
	      "GETATTR response received: xid=0x%08x, status=%d", nrc->nrc_xid,
	      status);

	if (status == 0 && response) {
		TRACE(REFFS_TRACE_LEVEL_WARNING, "File attributes received");
		// In a real implementation, you would process the attributes here
	} else {
		TRACE(REFFS_TRACE_LEVEL_WARNING, "GETATTR failed");
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

/*
 * Let the caller shut things down if there is an error
 */
static int request_more_read_data(struct buffer_state *bs,
				  struct io_uring *ring, struct io_context *ic)
{
	struct io_uring_sqe *sqe = NULL;
	int ret = 0;

	ic->ic_fd = bs->bs_fd;

	for (int i = 0; i < MAX_RETRIES; i++) {
		sqe = io_uring_get_sqe(ring);
		if (sqe)
			break;
		TRACE(REFFS_TRACE_LEVEL_ERR, "Waiting for retry %d", i);
		usleep(IO_URING_WAIT_US);
	}

	if (!sqe) {
		TRACE(REFFS_TRACE_LEVEL_ERR, "io_uring_get_sqe failed");
		return -ENOMEM;
	}

	io_uring_prep_read(sqe, bs->bs_fd, ic->ic_buffer, BUFFER_SIZE, 0);
	sqe->user_data = (uint64_t)(uintptr_t)ic;
	TRACE(REFFS_TRACE_LEVEL_NOTICE,
	      "On fd = %d sent a context of type %s and id %d", ic->ic_fd,
	      op_type_to_str(ic->ic_op_type), ic->ic_id);
	for (int i = 0; i < MAX_RETRIES; i++) {
		ret = io_uring_submit(ring);
		if (ret >= 0)
			break;
		if (ret == -EAGAIN)
			usleep(IO_URING_WAIT_US);
		else
			break;
	}

	if (ret < 0)
		TRACE(REFFS_TRACE_LEVEL_ERR, "io_uring_submit failed with %s",
		      strerror(-ret));
	else
		ret = 0;

	return ret;
}

static int request_additional_read_data(int fd, struct connection_info *ci,
					struct io_uring *ring)
{
	struct io_uring_sqe *sqe = NULL;
	int ret;

	char *buffer = malloc(BUFFER_SIZE);
	if (!buffer) {
		LOG("Failed to allocate buffer");
		close(fd);
		return ENOMEM;
	}

	struct io_context *ic =
		io_context_create(OP_TYPE_READ, fd, buffer, BUFFER_SIZE);
	if (!ic) {
		LOG("Failed to create read context");
		free(buffer);
		close(fd);
		return ENOMEM;
	}

	if (ci)
		copy_connection_info(&ic->ic_ci, ci);

	for (int i = 0; i < MAX_RETRIES; i++) {
		sqe = io_uring_get_sqe(ring);
		if (sqe)
			break;
		TRACE(REFFS_TRACE_LEVEL_ERR, "Waiting for retry %d", i);
		usleep(IO_URING_WAIT_US);
	}

	if (!sqe) {
		TRACE(REFFS_TRACE_LEVEL_ERR, "io_uring_get_sqe failed");
		free(buffer);
		close(fd);
		io_context_free(ic);
		return -ENOMEM;
	}

	io_uring_prep_read(sqe, fd, buffer, BUFFER_SIZE, 0);
	sqe->user_data = (uint64_t)(uintptr_t)ic;

	TRACE(REFFS_TRACE_LEVEL_NOTICE,
	      "On fd = %d sent a context of type %s and id %d", fd,
	      op_type_to_str(ic->ic_op_type), ic->ic_id);

	for (int i = 0; i < MAX_RETRIES; i++) {
		ret = io_uring_submit(ring);
		if (ret >= 0)
			break;
		if (ret == -EAGAIN)
			usleep(IO_URING_WAIT_US);
		else
			break;
	}

	if (ret < 0) {
		TRACE(REFFS_TRACE_LEVEL_ERR, "io_uring_submit failed with %s",
		      strerror(-ret));
		free(buffer);
		close(fd);
		io_context_free(ic);
	} else {
		ret = 0;
	}

	return 0;
}

// Process the RPC record marker and reassemble fragments
// Returns:
//   > 0: Complete message available, returns size
//   0: Need more data
//   < 0: Error
static int process_record_marker(struct buffer_state *bs, struct io_uring *ring,
				 struct io_context *ic)
{
	char *data = bs->bs_data;
	size_t filled = bs->bs_filled;
	struct record_state *rs = &bs->bs_record;

	TRACE(REFFS_TRACE_LEVEL_DEBUG,
	      "ENTRY: filled=%zu, rs_position=%u, rs_total_len=%zu, rs_fragment_len=%u, rs_last_fragment=%d",
	      filled, rs->rs_position, rs->rs_total_len, rs->rs_fragment_len,
	      rs->rs_last_fragment);

	// If continuing an existing fragment
	if (rs->rs_fragment_len > 0) {
		TRACE(REFFS_TRACE_LEVEL_DEBUG,
		      "Continuing fragment: position=%u, fragment_len=%u, filled=%zu",
		      rs->rs_position, rs->rs_fragment_len, filled);

		// Calculate remaining bytes needed
		size_t bytes_remaining = rs->rs_fragment_len - rs->rs_position;
		size_t to_copy = (filled > bytes_remaining) ? bytes_remaining :
							      filled;

		// Copy data to the correct position in the buffer
		memcpy(rs->rs_data + rs->rs_position, data, to_copy);
		rs->rs_position += to_copy;

		TRACE(REFFS_TRACE_LEVEL_DEBUG,
		      "Copied %zu more bytes, new position=%u of %u", to_copy,
		      rs->rs_position, rs->rs_fragment_len);

		// Advance the input buffer
		data += to_copy;
		filled -= to_copy;

		// Check if we've completed this fragment
		if (rs->rs_position >= rs->rs_fragment_len) {
			TRACE(REFFS_TRACE_LEVEL_DEBUG,
			      "Fragment complete, last_fragment=%d",
			      rs->rs_last_fragment);

			// If this was the last fragment, we've got a complete message
			if (rs->rs_last_fragment) {
				size_t message_size = rs->rs_position;

				// Reset state for next message
				rs->rs_position = 0;
				rs->rs_fragment_len = 0;
				rs->rs_last_fragment = false;

				// Update buffer state
				if (filled > 0) {
					memmove(bs->bs_data, data, filled);
					bs->bs_filled = filled;
				} else {
					bs->bs_filled = 0;
				}

				TRACE(REFFS_TRACE_LEVEL_DEBUG,
				      "Complete message assembled, size=%zu",
				      message_size);

				// Request more data for future operations if needed
				if (bs->bs_filled < 4) {
					request_additional_read_data(
						bs->bs_fd, &ic->ic_ci, ring);
				}

				return message_size;
			}

			// Not the last fragment, reset for next fragment
			rs->rs_position = 0;
			rs->rs_fragment_len = 0;

			// If we don't have enough data for next marker, request more
			if (filled < 4) {
				if (filled > 0) {
					memmove(bs->bs_data, data, filled);
					bs->bs_filled = filled;
				} else {
					bs->bs_filled = 0;
				}
				return request_more_read_data(bs, ring, ic);
			}
		} else {
			// Fragment not complete, need more data
			if (filled > 0) {
				memmove(bs->bs_data, data, filled);
				bs->bs_filled = filled;
			} else {
				bs->bs_filled = 0;
			}

			TRACE(REFFS_TRACE_LEVEL_NOTICE,
			      "Fragment incomplete, position=%u of %u, requesting more data",
			      rs->rs_position, rs->rs_fragment_len);
			return request_more_read_data(bs, ring, ic);
		}
	}

	// Starting a new fragment/message

	// Need at least 4 bytes for the record marker
	if (filled < 4) {
		bs->bs_filled = filled;
		return request_more_read_data(bs, ring, ic);
	}

	// Extract marker
	uint32_t marker = ntohl(*(uint32_t *)data);
	bool last_fragment = (marker & 0x80000000) != 0;
	uint32_t fragment_len = marker & 0x7FFFFFFF;

	TRACE(REFFS_TRACE_LEVEL_DEBUG,
	      "Starting a new message Len=%u, last_fragment=%d, marker=0x%08x",
	      fragment_len, last_fragment, marker);

	// Skip invalid markers
	if (fragment_len == 0) {
		TRACE(REFFS_TRACE_LEVEL_ERR,
		      "Invalid zero-length fragment, skipping");
		data += 4;
		filled -= 4;
		if (filled < 4) {
			if (filled > 0) {
				memmove(bs->bs_data, data, filled);
			}
			bs->bs_filled = filled;
			return request_more_read_data(bs, ring, ic);
		}
		// Reprocess with next potential marker
		return process_record_marker(bs, ring, ic);
	}

	// Initialize state for this fragment
	rs->rs_last_fragment = last_fragment;
	rs->rs_fragment_len = fragment_len;
	rs->rs_position = 0;

	// Ensure we have enough buffer space
	if (!rs->rs_data) {
		rs->rs_capacity = fragment_len * 2;
		rs->rs_data = malloc(rs->rs_capacity);
		if (!rs->rs_data) {
			TRACE(REFFS_TRACE_LEVEL_ERR,
			      "Failed to allocate record buffer");
			return -ENOMEM;
		}
	} else if (fragment_len > rs->rs_capacity) {
		size_t new_capacity = fragment_len * 2;

		TRACE(REFFS_TRACE_LEVEL_DEBUG,
		      "Resizing buffer from %zu to %zu bytes", rs->rs_capacity,
		      new_capacity);

		char *new_data = realloc(rs->rs_data, new_capacity);
		if (!new_data) {
			TRACE(REFFS_TRACE_LEVEL_ERR,
			      "Failed to resize record buffer");
			return -ENOMEM;
		}
		rs->rs_data = new_data;
		rs->rs_capacity = new_capacity;
	}

	// Skip past the marker
	data += 4;
	filled -= 4;

	// If no data after marker, request more
	if (filled == 0) {
		bs->bs_filled = 0;
		return request_more_read_data(bs, ring, ic);
	}

	// Copy available data for this fragment
	size_t to_copy = (filled > fragment_len) ? fragment_len : filled;

	TRACE(REFFS_TRACE_LEVEL_DEBUG,
	      "Copying %zu bytes, fragment_len=%u, filled=%zu", to_copy,
	      fragment_len, filled);

	memcpy(rs->rs_data, data, to_copy);
	rs->rs_position = to_copy;

	// Advance buffer pointers
	data += to_copy;
	filled -= to_copy;

	// Check if we've completed this fragment
	if (rs->rs_position >= fragment_len) {
		// Fragment is complete

		// If this was the last fragment, we have a complete message
		if (last_fragment) {
			size_t message_size = rs->rs_position;

			// Reset state for next message
			rs->rs_position = 0;
			rs->rs_fragment_len = 0;
			rs->rs_last_fragment = false;

			// Update buffer state
			if (filled > 0) {
				memmove(bs->bs_data, data, filled);
				bs->bs_filled = filled;
			} else {
				bs->bs_filled = 0;
			}

			TRACE(REFFS_TRACE_LEVEL_ERR,
			      "Complete message assembled, size=%zu",
			      message_size);

			// Request more data for future operations if needed
			if (bs->bs_filled < 4) {
				request_additional_read_data(bs->bs_fd,
							     &ic->ic_ci, ring);
			}

			return message_size;
		}

		// Not the last fragment, check if we have data for next marker
		rs->rs_position = 0;
		rs->rs_fragment_len = 0;

		if (filled < 4) {
			if (filled > 0) {
				memmove(bs->bs_data, data, filled);
			}
			bs->bs_filled = filled;
			return request_more_read_data(bs, ring, ic);
		}

		// Continue to process the next fragment
		memmove(bs->bs_data, data, filled);
		bs->bs_filled = filled;
		return process_record_marker(bs, ring, ic);
	}

	// Fragment is incomplete, need more data
	if (filled > 0) {
		memmove(bs->bs_data, data, filled);
	}
	bs->bs_filled = filled;
	TRACE(REFFS_TRACE_LEVEL_DEBUG,
	      "Need more data for current fragment: position=%u, fragment_len=%u",
	      rs->rs_position, fragment_len);
	return request_more_read_data(bs, ring, ic);
}

// Maximum size for a single write
#define MAX_WRITE_SIZE (1024 * 1024)

static int rpc_trans_writer(struct io_context *ic, struct io_uring *ring)
{
	struct io_uring_sqe *sqe;
	size_t remaining = ic->ic_buffer_len - ic->ic_position;
	int ret;

	TRACE(REFFS_TRACE_LEVEL_NOTICE,
	      "Len=%zu, Position=%zu, Remaining=%zu (xid=0x%08x)",
	      ic->ic_buffer_len, ic->ic_position, remaining, ic->ic_xid);

	// If no more data to send, we're done
	if (remaining == 0) {
		io_context_free(ic);
		return 0;
	} else if (remaining < 0) {
		// Error case - shouldn't happen with correct position tracking
		unregister_client_fd(ic->ic_fd);
		close(ic->ic_fd);
		io_context_free(ic);
		return 0;
	}

	// Determine if this is the last fragment to send
	bool last_fragment = (remaining <= MAX_WRITE_SIZE);

	// Calculate size for this fragment
	uint32_t chunk_size;
	char *buffer;
	uint32_t *p;

	if (ic->ic_position == 0) {
		// First fragment - record marker is already in the buffer
		chunk_size = remaining > MAX_WRITE_SIZE ? MAX_WRITE_SIZE :
							  remaining;
		buffer = (char *)ic->ic_buffer;

		// For debugging
		uint32_t original_marker = ntohl(*(uint32_t *)buffer);
		TRACE(REFFS_TRACE_LEVEL_DEBUG,
		      "Original Record Marker=0x%x (xid=0x%08x)",
		      original_marker, ic->ic_xid);
	} else {
		// Calculate chunk size: either MAX_WRITE_SIZE or remaining + 4 bytes for marker
		chunk_size = remaining > (MAX_WRITE_SIZE - 4) ? MAX_WRITE_SIZE :
								(remaining + 4);

		// Subsequent fragments - we need to reuse the preceding 4 bytes for the record marker
		buffer = (char *)ic->ic_buffer + (ic->ic_position - 4);
	}

	// Set the record marker to reflect the payload size (excluding the marker itself)
	p = (uint32_t *)buffer;
	*p = htonl((last_fragment ? 0x80000000 : 0) | (chunk_size - 4));

	TRACE(REFFS_TRACE_LEVEL_NOTICE,
	      "Last Fragment=%d, Chunk=%u,  Record Marker=0x%x (xid=0x%08x)",
	      last_fragment, chunk_size, ntohl(*p), ic->ic_xid);

	for (int i = 0; i < MAX_RETRIES; i++) {
		sqe = io_uring_get_sqe(ring);
		if (sqe)
			break;
		TRACE(REFFS_TRACE_LEVEL_ERR, "Waiting for retry %d", i);
		usleep(IO_URING_WAIT_US);
	}

	if (!sqe) {
		TRACE(REFFS_TRACE_LEVEL_ERR, "io_uring_get_sqe failed");
		unregister_client_fd(ic->ic_fd);
		close(ic->ic_fd);
		io_context_free(ic);
		return ENOMEM;
	}

	int total_fragments =
		(ic->ic_buffer_len + MAX_WRITE_SIZE - 1) / MAX_WRITE_SIZE;

	TRACE(REFFS_TRACE_LEVEL_NOTICE,
	      "Fragment %d/%d: payload_offset=%zu size=%u last=%d (xid=0x%08x)",
	      (int)(ic->ic_position / MAX_WRITE_SIZE), total_fragments,
	      ic->ic_position, chunk_size, last_fragment, ic->ic_xid);

	// Update position for next fragment
	if (ic->ic_position == 0) {
		// After first fragment, position points to end of this chunk
		ic->ic_position += chunk_size;
	} else {
		// For subsequent fragments, position points to end of payload (excluding marker)
		ic->ic_position += (chunk_size - 4);
	}

	int error = 0;
	socklen_t len = sizeof(error);
	if (getsockopt(ic->ic_fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 ||
	    error != 0) {
		TRACE(REFFS_TRACE_LEVEL_ERR, "Socket error before write: %s",
		      strerror(error ? error : errno));
	}

	// Submit the write operation
	io_uring_prep_write(sqe, ic->ic_fd, buffer, chunk_size, 0);
	sqe->user_data = (uint64_t)(uintptr_t)ic;

	for (int i = 0; i < MAX_RETRIES; i++) {
		ret = io_uring_submit(ring);
		if (ret >= 0)
			break;
		if (ret == -EAGAIN)
			usleep(IO_URING_WAIT_US);
		else
			break;
	}

	if (ret < 0) {
		TRACE(REFFS_TRACE_LEVEL_ERR, "io_uring_submit failed: %d", ret);
		unregister_client_fd(ic->ic_fd);
		close(ic->ic_fd);
		io_context_free(ic);
	} else {
		TRACE(REFFS_TRACE_LEVEL_NOTICE,
		      "Submitted %d io_uring operations", ret);
		ret = 0;
	}

	return 0;
}

static int rpc_trans_cb(struct rpc_trans *rt)
{
	struct io_context *ic;

	ic = io_context_create(OP_TYPE_WRITE, rt->rt_fd, rt->rt_reply,
			       rt->rt_reply_len);
	if (!ic) {
		LOG("Failed to create write context");
		TRACE(REFFS_TRACE_LEVEL_ERR,
		      "Dropped RPC reply xid=0x%08x due to no context",
		      rt->rt_info.ri_xid);
		return 0;
	}

	ic->ic_xid = rt->rt_info.ri_xid;
	copy_connection_info(&ic->ic_ci, &rt->rt_info.ri_ci);

	rt->rt_reply = NULL;

	int total_fragments =
		(ic->ic_buffer_len + MAX_WRITE_SIZE - 1) / MAX_WRITE_SIZE;

	TRACE(REFFS_TRACE_LEVEL_NOTICE,
	      "Fragmenting RPC reply of %zu bytes into %d fragments (xid=0x%08x)",
	      ic->ic_buffer_len, total_fragments, ic->ic_xid);

	return rpc_trans_writer(ic, rt->rt_ring);
}

// Worker thread function
static void *worker_thread(void *arg)
{
	int thread_id = *(int *)arg;
	free(arg);

	// Register this thread with userspace RCU
	rcu_register_thread();

	TRACE(REFFS_TRACE_LEVEL_NOTICE, "Worker thread %d started", thread_id);

	while (running) {
		struct task *t = NULL;

		// Use a timeout when checking for tasks during shutdown
		if (!running) {
			break;
		}

		pthread_mutex_lock(&task_queue_mutex);
		if (task_queue_head != task_queue_tail) {
			t = task_queue[task_queue_head];
			task_queue_head = (task_queue_head + 1) % QUEUE_DEPTH;
			pthread_mutex_unlock(&task_queue_mutex);
		} else {
			// Wait with timeout during normal operation
			struct timespec ts;
			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_sec += 1; // 1 second timeout

			int rc = pthread_cond_timedwait(&task_queue_cond,
							&task_queue_mutex, &ts);
			pthread_mutex_unlock(&task_queue_mutex);

			if (rc == ETIMEDOUT && !running) {
				break;
			}

			continue;
		}

		if (t) {
			if (t->t_fd > 0) {
				t->t_cb = rpc_trans_cb;
				TRACE(REFFS_TRACE_LEVEL_NOTICE,
				      "Complete RPC message processed (%d bytes)",
				      t->t_bytes_read);
				int rc = rpc_process_task(t);
				if (rc == ENOMEM) {
					TRACE(REFFS_TRACE_LEVEL_ERR,
					      "Worker thread %d rescheduling",
					      thread_id);
					add_task(t);
					continue;
				}
			}

			free(t->t_buffer);
			free(t);
		}
	}

	TRACE(REFFS_TRACE_LEVEL_NOTICE, "Worker thread %d exiting", thread_id);

	// Unregister this thread from userspace RCU
	rcu_unregister_thread();

	return NULL;
}

// Initialize io_uring
int setup_io_uring(struct io_uring *ring)
{
	if (io_uring_queue_init(QUEUE_DEPTH, ring, 0) < 0) {
		LOG("io_uring_queue_init: %s", strerror(errno));
		return -1;
	}
	return 0;
}

// Setup a listening socket
int setup_listener(int port)
{
	int listen_fd;
	struct sockaddr_in address;

	listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		LOG("socket: %s", strerror(errno));
		return -1;
	}

	// Set socket options to reuse address
	int opt = 1;
	if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) <
	    0) {
		LOG("setsockopt: %s", strerror(errno));
		close(listen_fd);
		return -1;
	}

	address.sin_family = AF_INET;
	address.sin_addr.s_addr = INADDR_ANY;
	address.sin_port = htons(port);

	if (bind(listen_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
		LOG("bind: %s", strerror(errno));
		close(listen_fd);
		return -1;
	}

	if (listen(listen_fd, 10) < 0) {
		LOG("listen: %s", strerror(errno));
		close(listen_fd);
		return -1;
	}

	TRACE(REFFS_TRACE_LEVEL_WARNING, "Listening on port %d", port);
	return listen_fd;
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

static int op_write_handler(struct io_uring_cqe *cqe, struct io_uring *ring)
{
	// Get the IO context from user_data
	struct io_context *ic = (struct io_context *)(uintptr_t)cqe->user_data;
	if (!ic) {
		LOG("Error: NULL io context in write handler");
		return -EINVAL;
	}

	return rpc_trans_writer(ic, ring);
}

static int op_write_handler_failed(struct io_uring_cqe *cqe)
{
	// Get the IO context from user_data
	struct io_context *ic = (struct io_context *)(uintptr_t)cqe->user_data;
	if (!ic) {
		LOG("Error: NULL io context in write handler");
		return -EINVAL;
	}

	size_t remaining = ic->ic_buffer_len - ic->ic_position;

	TRACE(REFFS_TRACE_LEVEL_ERR,
	      "Connection closed: Len=%zu, Position=%zu, Remaining=%zu (xid=0x%08x)",
	      ic->ic_buffer_len, ic->ic_position, remaining, ic->ic_xid);

	unregister_client_fd(ic->ic_fd);
	close(ic->ic_fd);
	io_context_free(ic);

	return 0;
}

// Handle read completions
static int op_read_handler(struct io_uring_cqe *cqe, struct io_uring *ring)
{
	// Get the IO context from user_data
	struct io_context *ic = (struct io_context *)(uintptr_t)cqe->user_data;
	if (!ic) {
		LOG("Error: NULL io context in read handler");
		return -EINVAL;
	}

	// Extract data from context
	char *buffer = (char *)ic->ic_buffer;
	int client_fd = ic->ic_fd;
	int bytes_read = cqe->res;

	if (bytes_read <= 0) {
		// Connection closed or error
		LOG("Connection closed or error (fd: %d, res: %d)", client_fd,
		    bytes_read);
		unregister_client_fd(client_fd);
		close(client_fd);
		io_context_free(ic);
		return 0;
	}

	// Get or create buffer state for this connection
	struct buffer_state *bs = get_buffer_state(client_fd);
	if (!bs) {
		bs = create_buffer_state(client_fd);
		if (!bs) {
			io_context_free(ic);
			return -ENOMEM;
		}
	}

	// Append new data to existing buffer
	if (!append_to_buffer(bs, buffer, bytes_read)) {
		io_context_free(ic);
		return -ENOMEM;
	}

	// Process the RPC record marker to get a complete RPC message
	int complete_size = process_record_marker(bs, ring, ic);
	if (complete_size <= 0) {
		if (complete_size < 0) {
			unregister_client_fd(ic->ic_fd);
			close(ic->ic_fd);
			io_context_free(ic);
		}
		return 0;
	}

	// We have a complete RPC message
	TRACE(REFFS_TRACE_LEVEL_ERR,
	      "Complete RPC message assembled (%d bytes)", complete_size);

	// Create a task for processing
	struct task *t = calloc(1, sizeof(struct task));
	if (t) {
		// Copy the complete message
		t->t_buffer = malloc(complete_size);
		if (!t->t_buffer) {
			free(t);
			return -ENOMEM;
		}

		memcpy(t->t_buffer, bs->bs_record.rs_data, complete_size);
		t->t_bytes_read = complete_size;
		t->t_fd = client_fd;
		t->t_ring = ring;

		copy_connection_info(&t->t_ci, &ic->ic_ci);

		// Extract XID for convenience
		if (complete_size >= 4) {
			t->t_xid = ntohl(*(uint32_t *)t->t_buffer);
		} else {
			t->t_xid = 0;
		}

		// Queue it for processing
		add_task(t);

		// Reset the record state for the next message
		bs->bs_record.rs_total_len = 0;
		bs->bs_record.rs_position = 0;
	}

	return 0;
}

static int request_accept_op(int fd, struct connection_info *ci,
			     struct io_uring *ring)
{
	struct io_uring_sqe *sqe = NULL;
	int ret;

	struct sockaddr *buffer = malloc(sizeof(struct sockaddr));
	if (!buffer) {
		LOG("Failed to allocate buffer");
		close(fd);
		return ENOMEM;
	}

	struct io_context *ic = io_context_create(OP_TYPE_ACCEPT, fd, buffer,
						  sizeof(struct sockaddr));
	if (!ic) {
		LOG("Failed to create read context");
		free(buffer);
		close(fd);
		return ENOMEM;
	}

	if (ci)
		copy_connection_info(&ic->ic_ci, ci);

	for (int i = 0; i < MAX_RETRIES; i++) {
		sqe = io_uring_get_sqe(ring);
		if (sqe)
			break;
		TRACE(REFFS_TRACE_LEVEL_ERR, "Waiting for retry %d", i);
		usleep(IO_URING_WAIT_US);
	}

	if (!sqe) {
		TRACE(REFFS_TRACE_LEVEL_ERR, "io_uring_get_sqe failed");
		close(fd);
		io_context_free(ic);
		return -ENOMEM;
	}

	io_uring_prep_accept(sqe, fd, ic->ic_buffer,
			     (socklen_t *)&ic->ic_buffer_len, 0);
	sqe->user_data = (uint64_t)(uintptr_t)ic;

	TRACE(REFFS_TRACE_LEVEL_NOTICE,
	      "On fd = %d sent a context of type %s and id %d", fd,
	      op_type_to_str(ic->ic_op_type), ic->ic_id);

	for (int i = 0; i < MAX_RETRIES; i++) {
		ret = io_uring_submit(ring);
		if (ret >= 0)
			break;
		if (ret == -EAGAIN)
			usleep(IO_URING_WAIT_US);
		else
			break;
	}

	if (ret < 0) {
		TRACE(REFFS_TRACE_LEVEL_ERR, "io_uring_submit failed with %s",
		      strerror(-ret));
		close(fd);
		io_context_free(ic);
	} else {
		ret = 0;
	}

	return 0;
}

static int op_accept_handler(struct io_uring_cqe *cqe, struct io_uring *ring)
{
	// Get the IO context from user_data
	struct io_context *ic = (struct io_context *)(uintptr_t)cqe->user_data;
	if (!ic) {
		LOG("Error: NULL io context in read handler");
		return -EINVAL;
	}

	char addr_str[INET6_ADDRSTRLEN];
	uint16_t port;

	// Handle new connection
	int listen_fd = ic->ic_fd;
	int client_fd = cqe->res;

	ic->ic_ci.ci_peer_len = sizeof(ic->ic_ci.ci_peer);
	if (getpeername(client_fd, (struct sockaddr *)&ic->ic_ci.ci_peer,
			&ic->ic_ci.ci_peer_len) == 0) {
		addr_to_string(&ic->ic_ci.ci_peer, addr_str, INET6_ADDRSTRLEN,
			       &port);
		TRACE(REFFS_TRACE_LEVEL_WARNING,
		      "Client connected from %s port %d", addr_str, port);
	} else {
		LOG("Failed to get peer information: %s", strerror(errno));
		memset(&ic->ic_ci.ci_peer, 0, sizeof(ic->ic_ci.ci_peer));
		ic->ic_ci.ci_peer_len = 0;
	}

	ic->ic_ci.ci_local_len = sizeof(ic->ic_ci.ci_local);
	if (getsockname(client_fd, (struct sockaddr *)&ic->ic_ci.ci_local,
			&ic->ic_ci.ci_local_len) == 0) {
		addr_to_string(&ic->ic_ci.ci_local, addr_str, INET6_ADDRSTRLEN,
			       &port);
		TRACE(REFFS_TRACE_LEVEL_WARNING,
		      "Server local endpoint - %s port %d", addr_str, port);
	} else {
		LOG("Failed to get local information: %s", strerror(errno));
		memset(&ic->ic_ci.ci_local, 0, sizeof(ic->ic_ci.ci_local));
		ic->ic_ci.ci_local_len = 0;
	}

	TRACE(REFFS_TRACE_LEVEL_WARNING, "New connection accepted (fd: %d)",
	      client_fd);

	// Register this client
	register_client_fd(client_fd);

	// Prepare to read from this new connection
	request_additional_read_data(client_fd, &ic->ic_ci, ring);

	// Accept more connections
	request_accept_op(listen_fd, &ic->ic_ci, ring);

	io_context_free(ic); // Free the completed accept context
	return 0;
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

	TRACE(REFFS_TRACE_LEVEL_NOTICE,
	      "On fd = %d sent a context of type %s and id %d", ic->ic_fd,
	      op_type_to_str(ic->ic_op_type), ic->ic_id);
	io_uring_submit(ring);
	return 0;
}

static void usage(const char *prog)
{
	printf("Usage: %s [options]\n", prog);
	printf("Options:\n");
	printf("  -h  --help                   Print this usage and exit\n");
	printf("  -p  --port=id                Serve NFS traffic from this \"port\"\n");
	printf("  -t  --tracing=lvl            Enable tracing at a level");
	printf("                                     0 - Debug");
	printf("                                     1 - Info");
	printf("                                     2 - Notice");
	printf("                                     3 - Warning");
	printf("                                     4 - Error");
	printf("                                     5 - Disabled");
}

static struct option long_opts[] = {
	{ "help", no_argument, 0, 'h' },
	{ "port", required_argument, 0, 'p' },
	{ "tracing", required_argument, 0, 't' },
	{ NULL, 0, NULL, 0 },
};

int main(int argc, char *argv[])
{
	int listener_fd;
	struct io_uring ring;
	struct io_uring_cqe *cqe;

	int exit_code = 0;

	int port = NFS_PORT;
	int opt;

	// Initialize userspace RCU
	rcu_init();

	server_boot_uuid_generate();

	while ((opt = getopt_long(argc, argv, "p:ht:", long_opts, NULL)) !=
	       -1) {
		switch (opt) {
		case 'p':
			port = atoi(optarg);
			break;
		case 't': {
			int tracing = atoi(optarg);
			enum reffs_trace_level level = tracing;
			if (tracing < 0)
				level = REFFS_TRACE_LEVEL_DEBUG;
			else if (tracing > REFFS_TRACE_LEVEL_DISABLED)
				level = REFFS_TRACE_LEVEL_DISABLED;
			reffs_tracing_set(level);
			break;
		}
		case 'h':
			usage(argv[0]);
			return 0;
		}
	}

	setvbuf(stdout, NULL, _IOLBF, 0);

	// Initialize pending requests array
	memset(pending_requests, 0, sizeof(pending_requests));

	// Initialize connection buffers array
	memset(conn_buffers, 0, sizeof(conn_buffers));

	// Setup signal handlers
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = signal_handler;
	sa.sa_flags = SA_RESTART; // Restart interrupted system calls
	sigemptyset(&sa.sa_mask);
	sigaddset(&sa.sa_mask, SIGINT);
	sigaddset(&sa.sa_mask, SIGTERM);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	// Block signals in main thread temporarily
	pthread_sigmask(SIG_BLOCK, &mask, NULL);

	// Setup io_uring
	if (setup_io_uring(&ring) < 0) {
		return 1;
	}

	// Set up protocol handlers
	if (nfs3_protocol_register()) {
		exit_code = 1;
		goto out;
	}

	if (mount3_protocol_register()) {
		exit_code = 1;
		goto out;
	}

	exit_code = reffs_ns_init();
	if (exit_code)
		goto out;

	// Create worker threads
	for (int i = 0; i < MAX_WORKER_THREADS; i++) {
		int *thread_id = malloc(sizeof(int));
		*thread_id = i;

		if (pthread_create(&worker_threads[i], NULL, worker_thread,
				   thread_id) == 0) {
			num_worker_threads++;
		} else {
			free(thread_id);
			LOG("Failed to create worker thread %d", i);
		}
	}

	pthread_sigmask(SIG_UNBLOCK, &mask, NULL);

	// Setup NFS listener
	listener_fd = setup_listener(port);
	if (listener_fd < 0) {
		LOG("Failed to setup listener on port %d", NFS_PORT);
		exit_code = 1;
		goto out;
	}

	request_accept_op(listener_fd, NULL, &ring);

	if (!pmap_set(NFS3_PROGRAM, NFS_V3, IPPROTO_TCP, port)) {
		LOG("Failed to register with portmapper for NFSv3");
		exit_code = 1;
		goto out;
	}

	if (!pmap_set(MOUNT_PROGRAM, MOUNT_V3, IPPROTO_TCP, port)) {
		LOG("Failed to register with portmapper for MOUNTv3");
		pmap_unset(NFS3_PROGRAM, NFS_V3);
		exit_code = 1;
		goto out;
	}

	while (running) {
		// Set a timeout for io_uring_wait_cqe to allow checking the running flag
		struct __kernel_timespec ts = { .tv_sec = IO_URING_WAIT_SEC,
						.tv_nsec = IO_URING_WAIT_NSEC };

		static time_t last_check = 0;
		time_t now = time(NULL);
		if (now - last_check >= 1) { // Check signal flag every second
			if (!running) {
				TRACE(REFFS_TRACE_LEVEL_WARNING,
				      "Detected shutdown flag, breaking main loop");
				break;
			}
			last_check = now;
		}

		int ret = io_uring_wait_cqe_timeout(&ring, &cqe, &ts);
		if (ret == -ETIME) {
			// Timeout - check running flag and continue
			continue;
		} else if (ret < 0) {
			LOG("io_uring_wait_cqe_timeout error: %s",
			    strerror(-ret));
			continue;
		}

		if (cqe->res < 0) {
			struct io_context *ic =
				(struct io_context *)(uintptr_t)cqe->user_data;
			LOG("CQE error for op=%s, fd=%d: %s",
			    op_type_to_str(ic->ic_op_type), ic->ic_fd,
			    strerror(-cqe->res));
		} else {
			// Get the IO context from user_data
			struct io_context *ic =
				(struct io_context *)(uintptr_t)cqe->user_data;
			if (!ic) {
				LOG("Error: NULL io context");
				io_uring_cqe_seen(&ring, cqe);
				continue;
			}

			TRACE(REFFS_TRACE_LEVEL_NOTICE,
			      "On fd = %d got a context of type %s and id %d",
			      ic->ic_fd, op_type_to_str(ic->ic_op_type),
			      ic->ic_id);

			enum op_type op = ic->ic_op_type;

			switch (ic->ic_op_type) {
			case OP_TYPE_ACCEPT: {
				ret = op_accept_handler(cqe, &ring);
				break;
			}

			case OP_TYPE_READ: {
				ret = op_read_handler(cqe, &ring);
				break;
			}

			case OP_TYPE_WRITE: {
				if (cqe->res < 0) {
					LOG("Write operation failed: %s",
					    strerror(-cqe->res));
					op_write_handler_failed(cqe);
				} else {
					struct io_context *ic =
						(struct io_context *)(uintptr_t)
							cqe->user_data;
					if (!ic) {
						LOG("Error: NULL io context in write handler");
						return -EINVAL;
					}

					TRACE(REFFS_TRACE_LEVEL_WARNING,
					      "Successfully wrote %d bytes for (xid=0x%08x)",
					      cqe->res, ic->ic_xid);
					op_write_handler(cqe, &ring);
				}
				ret = 0;
				break;
			}

			case OP_TYPE_RPC_REQ: {
				TRACE(REFFS_TRACE_LEVEL_WARNING,
				      "NFS request sent successfully");
				ret = 0;
				io_context_free(ic);
				break;
			}

			default:
				LOG("Unknown operation type: %d",
				    ic->ic_op_type);
				io_context_free(ic);
				ret = 0;
				break;
			}

			TRACE(REFFS_TRACE_LEVEL_NOTICE, "%s returned %d",
			      op_type_to_str(op), ret);
		}

		io_uring_cqe_seen(&ring, cqe);
	}

	TRACE(REFFS_TRACE_LEVEL_WARNING, "Main loop exited, cleaning up...");

	// Cleanup listener socket
	close(listener_fd);

	// Drain pending io_uring operations
	while (1) {
		struct io_uring_cqe *cqe;
		struct __kernel_timespec ts = { .tv_sec = 0,
						.tv_nsec = 100000000 };

		int ret = io_uring_wait_cqe_timeout(&ring, &cqe, &ts);
		if (ret == -ETIME) {
			// No more completions
			TRACE(REFFS_TRACE_LEVEL_WARNING, "No more completions");
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
				TRACE(REFFS_TRACE_LEVEL_WARNING,
				      "Cleaning up io_context of type %s and id %d",
				      op_type_to_str(ic->ic_op_type),
				      ic->ic_id);
				io_context_free(ic);
			}
		}

		io_uring_cqe_seen(&ring, cqe);
	}

	// Wait for worker threads to finish
	TRACE(REFFS_TRACE_LEVEL_WARNING,
	      "Waiting for worker threads to exit...");
	for (int i = 0; i < num_worker_threads; i++) {
		pthread_join(worker_threads[i], NULL);
	}

	// Cleanup any pending requests
	TRACE(REFFS_TRACE_LEVEL_WARNING, "Cleaning up pending requests...");
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

	TRACE(REFFS_TRACE_LEVEL_WARNING, "Unregistering Port Mapper");
	pmap_unset(MOUNT_PROGRAM, MOUNT_V3);
	pmap_unset(NFS3_PROGRAM, NFS_V3);

out:
	TRACE(REFFS_TRACE_LEVEL_WARNING,
	      "Final io_context statistics: created=%d, freed=%d, difference=%d",
	      context_created, context_freed, context_created - context_freed);

	// Wait for RCU grace period
	TRACE(REFFS_TRACE_LEVEL_WARNING, "Calling rcu_barrier()...");
	rcu_barrier();

	reffs_ns_fini();

	// Let inodes clear out of memory
	TRACE(REFFS_TRACE_LEVEL_WARNING, "Calling rcu_barrier()...");
	rcu_barrier();

	mount3_protocol_deregister();
	nfs3_protocol_deregister();

	// Clean up io_uring
	io_uring_queue_exit(&ring);

	LOG("Shutdown complete");
	return exit_code;
}

#pragma clang diagnostic pop
