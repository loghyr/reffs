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
#include "reffs/super_block.h"

#define BUFFER_SIZE 4096
#define QUEUE_DEPTH 1024
#define NFS_PORT 2049
#define NUM_LISTENERS 1
#define MAX_WORKER_THREADS 4
#define MAX_PENDING_REQUESTS 256
#define MAX_CONNECTIONS 1024 // Maximum number of concurrent client connections

// Global flag for clean shutdown
volatile sig_atomic_t running = 1;

// Opcodes for different packet types
#define OP_TYPE_ACCEPT 1
#define OP_TYPE_READ 2
#define OP_TYPE_WRITE 3
#define OP_TYPE_CONNECT 4
#define OP_TYPE_NFS_REQ 5

// IO operation context structure
struct io_context {
	int ic_op_type;
	int ic_fd;
	uint32_t ic_id;
	void *ic_buffer;

	struct sockaddr_storage peer_addr; // Remote endpoint address
	socklen_t peer_addr_len; // Length of peer address
	struct sockaddr_storage local_addr; // Local endpoint address
	socklen_t local_addr_len; // Length of local address
};

// Task struct for worker queue
struct task {
	char *t_buffer;
	int t_bytes_read;
	uint32_t t_xid;
	int t_fd; // For sending responses
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
int setup_io_uring(struct io_uring *ring);
void handle_nfs_protocol(struct task *task);
struct buffer_state *create_buffer_state(int fd);
struct buffer_state *get_buffer_state(int fd);
bool append_to_buffer(struct buffer_state *bs, const char *data, size_t len);
int decode_nfs_response(char *data, size_t filled, struct io_uring *ring,
			int client_fd);
void register_client_fd(int fd);
void unregister_client_fd(int fd);

// Signal handler
void signal_handler(int sig)
{
	TRACE("Received signal %d, initiating shutdown...", sig);
	running = 0;

	// Wake up any waiting worker threads
	pthread_cond_broadcast(&task_queue_cond);
}

uint32_t generate_id(void)
{
	static uint32_t next_id = 1;
	static pthread_mutex_t id_mutex = PTHREAD_MUTEX_INITIALIZER;

	pthread_mutex_lock(&id_mutex);
	uint32_t id = next_id++;
	pthread_mutex_unlock(&id_mutex);

	return id;
}

// Generate a unique transaction ID for RPC
uint32_t generate_xid(void)
{
	static uint32_t next_xid = 1;
	static pthread_mutex_t xid_mutex = PTHREAD_MUTEX_INITIALIZER;

	pthread_mutex_lock(&xid_mutex);
	uint32_t xid = next_xid++;
	pthread_mutex_unlock(&xid_mutex);

	return xid;
}

// Register a new request for tracking
int register_request(struct nfs_request_context *nrc)
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
struct nfs_request_context *find_request_by_xid(uint32_t xid)
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
void add_task(struct task *task)
{
	pthread_mutex_lock(&task_queue_mutex);
	task_queue[task_queue_tail] = task;
	task_queue_tail = (task_queue_tail + 1) % QUEUE_DEPTH;
	pthread_cond_signal(&task_queue_cond);
	pthread_mutex_unlock(&task_queue_mutex);
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

static void io_context_copy_network_info(struct io_context *dst,
					 const struct io_context *src)
{
	if (!dst || !src)
		return;

	// Copy peer address information
	memcpy(&dst->peer_addr, &src->peer_addr,
	       sizeof(struct sockaddr_storage));
	dst->peer_addr_len = src->peer_addr_len;

	// Copy local address information
	memcpy(&dst->local_addr, &src->local_addr,
	       sizeof(struct sockaddr_storage));
	dst->local_addr_len = src->local_addr_len;
}

// Create an IO context for operations
struct io_context *create_io_context(int op_type, int fd, void *buffer)
{
	struct io_context *ic = calloc(1, sizeof(struct io_context));
	if (!ic) {
		return NULL;
	}

	ic->ic_op_type = op_type;
	ic->ic_fd = fd;
	ic->ic_id = generate_id();
	ic->ic_buffer = buffer;

	return ic;
}

// Simplified XDR encoding for NFS response
int encode_nfs_response(char *buffer, int __attribute__((unused)) buflen,
			uint32_t xid, int status)
{
	// This is a simplified placeholder for a real NFS response
	int pos = 0;

	// RPC Header
	*(uint32_t *)(buffer + pos) = htonl(xid);
	pos += 4;

	// RPC Reply (1)
	*(uint32_t *)(buffer + pos) = htonl(1);
	pos += 4;

	// Status (0 = success)
	*(uint32_t *)(buffer + pos) = htonl(status);
	pos += 4;

	// More fields would be here in a real implementation

	return pos;
}

// Handler for GETATTR response
void handle_getattr_response(struct nfs_request_context *nrc, void *response,
			     int __attribute__((unused)) res_len, int status)
{
	TRACE("GETATTR response received: XID=%u, status=%d", nrc->nrc_xid,
	      status);

	if (status == 0 && response) {
		TRACE("File attributes received");
		// In a real implementation, you would process the attributes here
	} else {
		TRACE("GETATTR failed");
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

int request_more_read_data(struct buffer_state *bs, struct io_uring *ring,
			   struct io_context *ic)
{
	ic->ic_fd = bs->bs_fd;

	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
	io_uring_prep_read(sqe, bs->bs_fd, ic->ic_buffer, BUFFER_SIZE, 0);
	sqe->user_data = (uint64_t)(uintptr_t)ic;
	io_uring_submit(ring);

	return 0;
}

// Process the RPC record marker and reassemble fragments
// Returns:
//   > 0: Complete message available, returns size
//   0: Need more data
//   < 0: Error
int process_record_marker(struct buffer_state *bs, struct io_uring *ring,
			  struct io_context *ic)
{
	char *data = bs->bs_data;
	size_t filled = bs->bs_filled;
	struct record_state *rs = &bs->bs_record;

	// Process record markers until we have a complete message or need more data
	while (filled >= 4) { // Need at least 4 bytes for the record marker
		// If we're starting a new record
		if (rs->rs_position == 0) {
			uint32_t marker = ntohl(*(uint32_t *)data);
			rs->rs_last_fragment = (marker & 0x80000000) != 0;
			rs->rs_fragment_len = marker & 0x7FFFFFFF;

			// Ensure our record buffer is large enough
			if (!rs->rs_data) {
				rs->rs_capacity = rs->rs_fragment_len *
						  2; // Some extra space
				rs->rs_data = malloc(rs->rs_capacity);
				if (!rs->rs_data) {
					return -ENOMEM;
				}
				rs->rs_total_len = 0;
			} else if (rs->rs_total_len + rs->rs_fragment_len >
				   rs->rs_capacity) {
				// Need to resize
				size_t new_capacity = rs->rs_capacity * 2;
				char *new_data =
					realloc(rs->rs_data, new_capacity);
				if (!new_data) {
					return -ENOMEM;
				}
				rs->rs_data = new_data;
				rs->rs_capacity = new_capacity;
			}

			// Move past the marker in the input buffer
			data += 4;
			filled -= 4;

			// If no more data available, we need to read more
			if (filled == 0) {
				// Compact the buffer, removing the 4 bytes we just processed
				memmove(bs->bs_data, bs->bs_data + 4,
					bs->bs_filled - 4);
				bs->bs_filled -= 4;

				return request_more_read_data(bs, ring, ic);
			}
		}

		// Determine how much we can copy
		size_t to_copy = rs->rs_fragment_len - rs->rs_position;
		if (to_copy > filled) {
			to_copy = filled;
		}

		// Copy data into our reassembly buffer
		memcpy(rs->rs_data + rs->rs_total_len, data, to_copy);
		rs->rs_total_len += to_copy;
		rs->rs_position += to_copy;

		// Advance the input buffer
		data += to_copy;
		filled -= to_copy;

		// Check if we've completed this fragment
		if (rs->rs_position >= rs->rs_fragment_len) {
			// Reset position for next fragment
			rs->rs_position = 0;

			// If this was the last fragment, we have a complete message
			if (rs->rs_last_fragment) {
				size_t complete_size = rs->rs_total_len;

				// Update our buffer state for the next processing cycle
				memmove(bs->bs_data, data, filled);
				bs->bs_filled = filled;

				// Return the complete message size
				return complete_size;
			}

			// Otherwise, continue to the next fragment
			// If we're out of data, request more
			if (filled < 4) {
				// Compact the buffer
				memmove(bs->bs_data, data, filled);
				bs->bs_filled = filled;

				return request_more_read_data(bs, ring, ic);
			}
		} else {
			// We need more data for this fragment
			memmove(bs->bs_data, data, filled);
			bs->bs_filled = filled;

			return request_more_read_data(bs, ring, ic);
		}
	}

	// Not enough data to even read a marker
	if (filled > 0) {
		memmove(bs->bs_data, data, filled);
		bs->bs_filled = filled;
	} else {
		bs->bs_filled = 0;
	}

	return request_more_read_data(bs, ring, ic);
}

// NFS protocol handler
void handle_nfs_protocol(struct task *t)
{
	TRACE("NFS Protocol Handler: Received %d bytes", t->t_bytes_read);

	// Process RPC/NFS message
	// This is a simplified example - in a real implementation, you would:
	// 1. Parse the RPC/NFS message properly
	// 2. Handle the operation
	// 3. Send a response

	// Extract XID from the message (first 4 bytes)
	uint32_t xid = ntohl(*(uint32_t *)t->t_buffer);

	// Extract the RPC message type (call=0, reply=1)
	uint32_t msg_type = ntohl(*(uint32_t *)(t->t_buffer + 4));

	// Print basic info about the message
	TRACE("RPC Message: XID=%u, Type=%s", xid,
	      msg_type == 0 ? "CALL" : "REPLY");

	if (msg_type == 0) { // It's a call
		// Extract program, version, procedure
		uint32_t program = ntohl(*(uint32_t *)(t->t_buffer + 12));
		uint32_t version = ntohl(*(uint32_t *)(t->t_buffer + 16));
		uint32_t procedure = ntohl(*(uint32_t *)(t->t_buffer + 20));

		TRACE("RPC Call: Program=%u, Version=%u, Procedure=%u", program,
		      version, procedure);

		if (program == 100003) { // NFS
			printf("NFS Call: ");
			printf("Procedure %d\n", procedure);

			// We would dispatch to the appropriate NFS handler here
			// For this example, we'll just send a simple success response

			// Allocate response buffer
			char *resp_buffer = malloc(BUFFER_SIZE);
			if (resp_buffer) {
				// Encode a simple NFS response
				int resp_len = encode_nfs_response(
					resp_buffer, BUFFER_SIZE, xid, 0);

				TRACE("resp_len = %d", resp_len);

				// Submit response for sending using io_uring
				// This would be implemented in the main event loop
			} else {
				LOG("Failed to allocate response buffer");
			}
		}
	}
}

// Worker thread function
void *worker_thread(void *arg)
{
	int thread_id = *(int *)arg;
	free(arg);

	// Register this thread with userspace RCU
	rcu_register_thread();

	TRACE("Worker thread %d started", thread_id);

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
			handle_nfs_protocol(t);

			// Generate and send a response
			if (t->t_fd > 0) {
				// In a real implementation, you would:
				// 1. Prepare the appropriate response based on the request
				// 2. Send it via io_uring
				// For this example, we just acknowledge
				TRACE("Processing request with XID %u",
				      t->t_xid);
			}

			free(t->t_buffer);
			free(t);
		}
	}

	TRACE("Worker thread %d exiting", thread_id);

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

	TRACE("Listening on port %d", port);
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

	// We now have the correct client_fd from our context

	if (bytes_read <= 0) {
		// Connection closed or error
		LOG("Connection closed or error (fd: %d, res: %d)", client_fd,
		    bytes_read);
		unregister_client_fd(client_fd);
		close(client_fd);
		free(buffer);
		free(ic);
		return 0;
	}

	// Get or create buffer state for this connection
	struct buffer_state *bs = get_buffer_state(client_fd);
	if (!bs) {
		bs = create_buffer_state(client_fd);
		if (!bs) {
			free(buffer);
			free(ic);
			return -ENOMEM;
		}
	}

	// Append new data to existing buffer
	if (!append_to_buffer(bs, buffer, bytes_read)) {
		free(buffer);
		free(ic);
		return -ENOMEM;
	}

	// Process the RPC record marker to get a complete RPC message
	int complete_size = process_record_marker(bs, ring, ic);
	if (complete_size <= 0) {
		if (complete_size < 0) {
			free(buffer);
			free(ic);
		}
		return 0;
	}

	// We have a complete RPC message
	TRACE("Complete RPC message assembled (%d bytes)", complete_size);

	// Create a task for processing
	struct task *t = malloc(sizeof(struct task));
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

static int op_accept_handler(struct io_uring_cqe *cqe, struct io_uring *ring)
{
	// Get the IO context from user_data
	struct io_context *ic = (struct io_context *)(uintptr_t)cqe->user_data;
	if (!ic) {
		LOG("Error: NULL io context in read handler");
		return -EINVAL;
	}

	struct io_uring_sqe *sqe;

	char addr_str[INET6_ADDRSTRLEN];
	uint16_t port;

	// Handle new connection
	int listen_fd = ic->ic_fd;
	int client_fd = cqe->res;

	// Store peer (remote) address information
	ic->peer_addr_len = sizeof(ic->peer_addr);
	if (getpeername(client_fd, (struct sockaddr *)&ic->peer_addr,
			&ic->peer_addr_len) == 0) {
		addr_to_string(&ic->peer_addr, addr_str, INET6_ADDRSTRLEN,
			       &port);
		TRACE("Client connected from %s port %d", addr_str, port);
	} else {
		LOG("Failed to get peer information: %s", strerror(errno));
		memset(&ic->peer_addr, 0, sizeof(ic->peer_addr));
		ic->peer_addr_len = 0;
	}

	// Store local (server) address information
	ic->local_addr_len = sizeof(ic->local_addr);
	if (getsockname(client_fd, (struct sockaddr *)&ic->local_addr,
			&ic->local_addr_len) == 0) {
		addr_to_string(&ic->local_addr, addr_str, INET6_ADDRSTRLEN,
			       &port);
		TRACE("Server local endpoint - %s port %d", addr_str, port);
	} else {
		LOG("Failed to get local information: %s", strerror(errno));
		memset(&ic->local_addr, 0, sizeof(ic->local_addr));
		ic->local_addr_len = 0;
	}

	TRACE("New connection accepted (fd: %d)", client_fd);

	// Register this client
	register_client_fd(client_fd);

	// Prepare to read from this new connection
	char *buffer = malloc(BUFFER_SIZE);
	if (buffer) {
		// Create IO context for the read operation
		struct io_context *ic_read =
			create_io_context(OP_TYPE_READ, client_fd, buffer);
		io_context_copy_network_info(ic_read, ic);
		if (!ic_read) {
			LOG("Failed to create read context");
			free(buffer);
			close(client_fd);
		} else {
			sqe = io_uring_get_sqe(ring);
			io_uring_prep_read(sqe, client_fd, buffer, BUFFER_SIZE,
					   0);
			sqe->user_data = (uint64_t)(uintptr_t)ic_read;
			io_uring_submit(ring);
		}
	} else {
		LOG("malloc: %s", strerror(errno));
		close(client_fd);
	}

	// Submit a new accept for this listener
	struct sockaddr_in client_address;
	socklen_t client_len = sizeof(client_address);

	// Create new IO context for the next accept
	struct io_context *ic_accept =
		create_io_context(OP_TYPE_ACCEPT, listen_fd, NULL);
	if (!ic_accept) {
		LOG("Failed to create accept context");
		free(ic);
		io_uring_cqe_seen(ring, cqe);
		return 0;
	}

	sqe = io_uring_get_sqe(ring);
	io_uring_prep_accept(sqe, listen_fd, (struct sockaddr *)&client_address,
			     &client_len, 0);
	sqe->user_data = (uint64_t)(uintptr_t)ic_accept;
	io_uring_submit(ring);

	free(ic); // Free the completed accept context
	return 0;
}

// Send NFS response using io_uring
int send_nfs_response(struct io_uring *ring, int fd, char *buffer, int len)
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

	// Create an IO context for the write operation
	struct io_context *ic =
		create_io_context(OP_TYPE_WRITE, fd, send_buffer);
	if (!ic) {
		free(send_buffer);
		return -ENOMEM;
	}

	// Submit the write operation to io_uring
	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
	io_uring_prep_write(sqe, fd, send_buffer, len + 4, 0);

	// Associate with the io context
	sqe->user_data = (uint64_t)(uintptr_t)ic;

	io_uring_submit(ring);
	return 0;
}

int main(int __attribute__((unused)) argc, char *__attribute__((unused)) argv[])
{
	int listener_fd;
	struct io_uring ring;
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;

	int exit_code = 0;

	struct super_block *root_sb;

	// Initialize userspace RCU
	rcu_init();

	// Initialize pending requests array
	memset(pending_requests, 0, sizeof(pending_requests));

	// Initialize connection buffers array
	memset(conn_buffers, 0, sizeof(conn_buffers));

	// Setup signal handlers
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = signal_handler;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

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

	root_sb = super_block_alloc(1, "/");
	if (!root_sb) {
		exit_code = ENOMEM;
		goto out;
	}

	server_boot_uuid_generate();

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

	// Setup NFS listener
	listener_fd = setup_listener(NFS_PORT);
	if (listener_fd < 0) {
		LOG("Failed to setup listener on port %d", NFS_PORT);
		exit_code = 1;
		goto out;
	}

	// Setup initial accept operation
	struct sockaddr_in client_address;
	socklen_t client_len = sizeof(client_address);

	// Create IO context for the accept operation
	struct io_context *ic_accept =
		create_io_context(OP_TYPE_ACCEPT, listener_fd, NULL);
	if (!ic_accept) {
		LOG("Failed to create accept context");
		exit_code = 1;
		goto out;
	}

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_accept(sqe, listener_fd,
			     (struct sockaddr *)&client_address, &client_len,
			     0);

	// Associate with the io context
	sqe->user_data = (uint64_t)(uintptr_t)ic_accept;

	io_uring_submit(&ring);

	while (running) {
		// Set a timeout for io_uring_wait_cqe to allow checking the running flag
		struct __kernel_timespec ts = { .tv_sec = 1, .tv_nsec = 0 };

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
			LOG("CQE error: %s", strerror(-cqe->res));
		} else {
			// Get the IO context from user_data
			struct io_context *ic =
				(struct io_context *)(uintptr_t)cqe->user_data;
			if (!ic) {
				LOG("Error: NULL io context");
				io_uring_cqe_seen(&ring, cqe);
				continue;
			}

			switch (ic->ic_op_type) {
			case OP_TYPE_ACCEPT: {
				op_accept_handler(cqe, &ring);
				break;
			}

			case OP_TYPE_READ: {
				op_read_handler(cqe, &ring);
				// Note: op_read_handler now handles freeing the context
				break;
			}

			case OP_TYPE_WRITE: {
				// Write completed, free the buffer
				free(ic->ic_buffer);
				free(ic);
				break;
			}

			case OP_TYPE_NFS_REQ: {
				// NFS request write completed
				TRACE("NFS request sent successfully");
				// For client-side requests, we'd track for the response
				free(ic);
				break;
			}

			default:
				LOG("Unknown operation type: %d",
				    ic->ic_op_type);
				if (ic->ic_buffer) {
					free(ic->ic_buffer);
				}
				free(ic);
				break;
			}
		}

		io_uring_cqe_seen(&ring, cqe);
	}

	TRACE("Main loop exited, cleaning up...");

	// Cleanup listener socket
	close(listener_fd);

	// Wait for worker threads to finish
	TRACE("Waiting for worker threads to exit...");
	for (int i = 0; i < num_worker_threads; i++) {
		pthread_join(worker_threads[i], NULL);
	}

	// Cleanup any pending requests
	TRACE("Cleaning up pending requests...");
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

out:
	// Wait for RCU grace period
	TRACE("Calling rcu_barrier()...");
	rcu_barrier();

	mount3_protocol_deregister();
	nfs3_protocol_deregister();

	// Clean up io_uring
	io_uring_queue_exit(&ring);

	LOG("Shutdown complete");
	return exit_code;
}
