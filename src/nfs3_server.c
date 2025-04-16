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

// NFS operations
#define NFS3_NULL 0
#define NFS3_GETATTR 1
#define NFS3_SETATTR 2
#define NFS3_LOOKUP 3
#define NFS3_READ 6
#define NFS3_WRITE 7

// IO operation context structure
typedef struct {
	int op_type;
	int fd;
	void *buffer;
} io_context_t;

// Task struct for worker queue
struct task {
	char *buffer;
	int bytes_read;
	uint32_t xid;
	int fd; // For sending responses
};

// NFS request context for tracking operations
struct nfs_request_context {
	uint32_t xid; // RPC transaction ID
	int operation; // NFS operation (GETATTR, READ, etc.)
	int sockfd; // Socket this request was sent on
	void *private_data; // Application-specific context
	void (*callback)(struct nfs_request_context *ctx, void *response,
			 int res_len, int status);
	char *buffer; // Buffer for response
};

// Record state for reassembling fragmented RPC messages
struct record_state {
	bool last_fragment;
	uint32_t fragment_len;
	char *data;
	size_t total_len;
	size_t capacity;
	uint32_t position;
};

// Connection buffer state for reassembling messages
struct buffer_state {
	int fd;
	char *data;
	size_t filled;
	size_t capacity;
	struct record_state record;
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
bool append_to_buffer(struct buffer_state *state, const char *data, size_t len);
int decode_nfs_response(char *data, size_t filled, struct io_uring *ring,
			int client_fd);
void register_client_fd(int fd);
void unregister_client_fd(int fd);

// Signal handler
void signal_handler(int sig)
{
	printf("Received signal %d, initiating shutdown...\n", sig);
	running = 0;

	// Wake up any waiting worker threads
	pthread_cond_broadcast(&task_queue_cond);
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
int register_request(struct nfs_request_context *ctx)
{
	pthread_mutex_lock(&request_mutex);

	// Find an empty slot
	for (int i = 0; i < MAX_PENDING_REQUESTS; i++) {
		if (pending_requests[i] == NULL) {
			pending_requests[i] = ctx;
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
	struct nfs_request_context *ctx = NULL;

	pthread_mutex_lock(&request_mutex);
	for (int i = 0; i < MAX_PENDING_REQUESTS; i++) {
		if (pending_requests[i] && pending_requests[i]->xid == xid) {
			ctx = pending_requests[i];
			pending_requests[i] = NULL; // Remove from tracking
			break;
		}
	}
	pthread_mutex_unlock(&request_mutex);

	return ctx;
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
	struct buffer_state *state = get_buffer_state(fd);
	if (state) {
		free(state->data);
		if (state->record.data) {
			free(state->record.data);
		}
		free(state);
		conn_buffers[fd % MAX_CONNECTIONS] = NULL;
	}
}

// Create an IO context for operations
io_context_t *create_io_context(int op_type, int fd, void *buffer)
{
	io_context_t *ctx = malloc(sizeof(io_context_t));
	if (!ctx) {
		return NULL;
	}

	ctx->op_type = op_type;
	ctx->fd = fd;
	ctx->buffer = buffer;

	return ctx;
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
void handle_getattr_response(struct nfs_request_context *ctx, void *response,
			     int __attribute__((unused)) res_len, int status)
{
	printf("GETATTR response received: XID=%u, status=%d\n", ctx->xid,
	       status);

	if (status == 0 && response) {
		printf("File attributes received\n");
		// In a real implementation, you would process the attributes here
	} else {
		printf("GETATTR failed\n");
	}

	// Free the context
	if (ctx->private_data) {
		free(ctx->private_data);
	}
	if (ctx->buffer) {
		free(ctx->buffer);
	}
	free(ctx);
}

// Process the RPC record marker and reassemble fragments
// Returns:
//   > 0: Complete message available, returns size
//   0: Need more data
//   < 0: Error
int process_record_marker(struct buffer_state *buffer_state,
			  struct io_uring *ring)
{
	char *data = buffer_state->data;
	size_t filled = buffer_state->filled;
	struct record_state *record = &buffer_state->record;

	// Process record markers until we have a complete message or need more data
	while (filled >= 4) { // Need at least 4 bytes for the record marker
		// If we're starting a new record
		if (record->position == 0) {
			uint32_t marker = ntohl(*(uint32_t *)data);
			record->last_fragment = (marker & 0x80000000) != 0;
			record->fragment_len = marker & 0x7FFFFFFF;

			// Ensure our record buffer is large enough
			if (!record->data) {
				record->capacity = record->fragment_len *
						   2; // Some extra space
				record->data = malloc(record->capacity);
				if (!record->data) {
					return -ENOMEM;
				}
				record->total_len = 0;
			} else if (record->total_len + record->fragment_len >
				   record->capacity) {
				// Need to resize
				size_t new_capacity = record->capacity * 2;
				char *new_data =
					realloc(record->data, new_capacity);
				if (!new_data) {
					return -ENOMEM;
				}
				record->data = new_data;
				record->capacity = new_capacity;
			}

			// Move past the marker in the input buffer
			data += 4;
			filled -= 4;

			// If no more data available, we need to read more
			if (filled == 0) {
				// Compact the buffer, removing the 4 bytes we just processed
				memmove(buffer_state->data,
					buffer_state->data + 4,
					buffer_state->filled - 4);
				buffer_state->filled -= 4;

				// Request more data
				char *new_buffer = malloc(BUFFER_SIZE);
				if (new_buffer) {
					// Create an IO context for this read operation
					io_context_t *ctx = create_io_context(
						OP_TYPE_READ, buffer_state->fd,
						new_buffer);
					if (!ctx) {
						free(new_buffer);
						return -ENOMEM;
					}

					struct io_uring_sqe *sqe =
						io_uring_get_sqe(ring);
					io_uring_prep_read(sqe,
							   buffer_state->fd,
							   new_buffer,
							   BUFFER_SIZE, 0);
					sqe->user_data =
						(uint64_t)(uintptr_t)ctx;
					io_uring_submit(ring);
				}
				return 0; // Need more data
			}
		}

		// Determine how much we can copy
		size_t to_copy = record->fragment_len - record->position;
		if (to_copy > filled) {
			to_copy = filled;
		}

		// Copy data into our reassembly buffer
		memcpy(record->data + record->total_len, data, to_copy);
		record->total_len += to_copy;
		record->position += to_copy;

		// Advance the input buffer
		data += to_copy;
		filled -= to_copy;

		// Check if we've completed this fragment
		if (record->position >= record->fragment_len) {
			// Reset position for next fragment
			record->position = 0;

			// If this was the last fragment, we have a complete message
			if (record->last_fragment) {
				size_t complete_size = record->total_len;

				// Update our buffer state for the next processing cycle
				memmove(buffer_state->data, data, filled);
				buffer_state->filled = filled;

				// Return the complete message size
				return complete_size;
			}

			// Otherwise, continue to the next fragment
			// If we're out of data, request more
			if (filled < 4) {
				// Compact the buffer
				memmove(buffer_state->data, data, filled);
				buffer_state->filled = filled;

				// Request more data
				char *new_buffer = malloc(BUFFER_SIZE);
				if (new_buffer) {
					// Create an IO context for this read operation
					io_context_t *ctx = create_io_context(
						OP_TYPE_READ, buffer_state->fd,
						new_buffer);
					if (!ctx) {
						free(new_buffer);
						return -ENOMEM;
					}

					struct io_uring_sqe *sqe =
						io_uring_get_sqe(ring);
					io_uring_prep_read(sqe,
							   buffer_state->fd,
							   new_buffer,
							   BUFFER_SIZE, 0);
					sqe->user_data =
						(uint64_t)(uintptr_t)ctx;
					io_uring_submit(ring);
				}
				return 0; // Need more data
			}
		} else {
			// We need more data for this fragment
			memmove(buffer_state->data, data, filled);
			buffer_state->filled = filled;

			// Request more data
			char *new_buffer = malloc(BUFFER_SIZE);
			if (new_buffer) {
				// Create an IO context for this read operation
				io_context_t *ctx = create_io_context(
					OP_TYPE_READ, buffer_state->fd,
					new_buffer);
				if (!ctx) {
					free(new_buffer);
					return -ENOMEM;
				}

				struct io_uring_sqe *sqe =
					io_uring_get_sqe(ring);
				io_uring_prep_read(sqe, buffer_state->fd,
						   new_buffer, BUFFER_SIZE, 0);
				sqe->user_data = (uint64_t)(uintptr_t)ctx;
				io_uring_submit(ring);
			}
			return 0; // Need more data
		}
	}

	// Not enough data to even read a marker
	if (filled > 0) {
		memmove(buffer_state->data, data, filled);
		buffer_state->filled = filled;
	} else {
		buffer_state->filled = 0;
	}

	// Request more data
	char *new_buffer = malloc(BUFFER_SIZE);
	if (new_buffer) {
		// Create an IO context for this read operation
		io_context_t *ctx = create_io_context(
			OP_TYPE_READ, buffer_state->fd, new_buffer);
		if (!ctx) {
			free(new_buffer);
			return -ENOMEM;
		}

		struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
		io_uring_prep_read(sqe, buffer_state->fd, new_buffer,
				   BUFFER_SIZE, 0);
		sqe->user_data = (uint64_t)(uintptr_t)ctx;
		io_uring_submit(ring);
	}

	return 0; // Need more data
}

// NFS protocol handler
void handle_nfs_protocol(struct task *task)
{
	printf("NFS Protocol Handler: Received %d bytes\n", task->bytes_read);

	// Process RPC/NFS message
	// This is a simplified example - in a real implementation, you would:
	// 1. Parse the RPC/NFS message properly
	// 2. Handle the operation
	// 3. Send a response

	// Extract XID from the message (first 4 bytes)
	uint32_t xid = ntohl(*(uint32_t *)task->buffer);

	// Extract the RPC message type (call=0, reply=1)
	uint32_t msg_type = ntohl(*(uint32_t *)(task->buffer + 4));

	// Print basic info about the message
	printf("RPC Message: XID=%u, Type=%s\n", xid,
	       msg_type == 0 ? "CALL" : "REPLY");

	if (msg_type == 0) { // It's a call
		// Extract program, version, procedure
		uint32_t program = ntohl(*(uint32_t *)(task->buffer + 12));
		uint32_t version = ntohl(*(uint32_t *)(task->buffer + 16));
		uint32_t procedure = ntohl(*(uint32_t *)(task->buffer + 20));

		printf("RPC Call: Program=%u, Version=%u, Procedure=%u\n",
		       program, version, procedure);

		if (program == 100003) { // NFS
			printf("NFS Call: ");
			switch (procedure) {
			case NFS3_NULL:
				printf("NULL\n");
				break;
			case NFS3_GETATTR:
				printf("GETATTR\n");
				break;
			case NFS3_LOOKUP:
				printf("LOOKUP\n");
				break;
			case NFS3_READ:
				printf("READ\n");
				break;
			case NFS3_WRITE:
				printf("WRITE\n");
				break;
			default:
				printf("Procedure %d\n", procedure);
			}

			// We would dispatch to the appropriate NFS handler here
			// For this example, we'll just send a simple success response

			// Allocate response buffer
			char *resp_buffer = malloc(BUFFER_SIZE);
			if (resp_buffer) {
				// Encode a simple NFS response
				int resp_len = encode_nfs_response(
					resp_buffer, BUFFER_SIZE, xid, 0);

				printf("resp_len = %d\n", resp_len);

				// Submit response for sending using io_uring
				// This would be implemented in the main event loop
			} else {
				printf("Failed to allocate response buffer\n");
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

	printf("Worker thread %d started\n", thread_id);

	while (running) {
		struct task *task = NULL;

		// Use a timeout when checking for tasks during shutdown
		if (!running) {
			break;
		}

		pthread_mutex_lock(&task_queue_mutex);
		if (task_queue_head != task_queue_tail) {
			task = task_queue[task_queue_head];
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

		if (task) {
			handle_nfs_protocol(task);

			// Generate and send a response
			if (task->fd > 0) {
				// In a real implementation, you would:
				// 1. Prepare the appropriate response based on the request
				// 2. Send it via io_uring
				// For this example, we just acknowledge
				printf("Processing request with XID %u\n",
				       task->xid);
			}

			free(task->buffer);
			free(task);
		}
	}

	printf("Worker thread %d exiting\n", thread_id);

	// Unregister this thread from userspace RCU
	rcu_unregister_thread();

	return NULL;
}

// Initialize io_uring
int setup_io_uring(struct io_uring *ring)
{
	if (io_uring_queue_init(QUEUE_DEPTH, ring, 0) < 0) {
		perror("io_uring_queue_init");
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
		perror("socket");
		return -1;
	}

	// Set socket options to reuse address
	int opt = 1;
	if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) <
	    0) {
		perror("setsockopt");
		close(listen_fd);
		return -1;
	}

	address.sin_family = AF_INET;
	address.sin_addr.s_addr = INADDR_ANY;
	address.sin_port = htons(port);

	if (bind(listen_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
		perror("bind");
		close(listen_fd);
		return -1;
	}

	if (listen(listen_fd, 10) < 0) {
		perror("listen");
		close(listen_fd);
		return -1;
	}

	printf("Listening on port %d\n", port);
	return listen_fd;
}

// Create buffer state for a connection
struct buffer_state *create_buffer_state(int fd)
{
	struct buffer_state *state = malloc(sizeof(struct buffer_state));
	if (!state)
		return NULL;

	state->fd = fd;
	state->capacity = BUFFER_SIZE * 2;
	state->data = malloc(state->capacity);
	state->filled = 0;

	// Initialize record state
	state->record.last_fragment = false;
	state->record.fragment_len = 0;
	state->record.data = NULL;
	state->record.total_len = 0;
	state->record.capacity = 0;
	state->record.position = 0;

	if (!state->data) {
		free(state);
		return NULL;
	}

	conn_buffers[fd % MAX_CONNECTIONS] = state;
	return state;
}

// Get buffer state for a connection
struct buffer_state *get_buffer_state(int fd)
{
	return conn_buffers[fd % MAX_CONNECTIONS];
}

// Append data to a buffer, resizing if necessary
bool append_to_buffer(struct buffer_state *state, const char *data, size_t len)
{
	// Check if we need to resize
	if (state->filled + len > state->capacity) {
		size_t new_capacity = state->capacity * 2;
		char *new_data = realloc(state->data, new_capacity);
		if (!new_data)
			return false;

		state->data = new_data;
		state->capacity = new_capacity;
	}

	// Append the data
	memcpy(state->data + state->filled, data, len);
	state->filled += len;
	return true;
}

// Handle read completions
int op_read_handler(struct io_uring_cqe *cqe, struct io_uring *ring)
{
	// Get the IO context from user_data
	io_context_t *ctx = (io_context_t *)(uintptr_t)cqe->user_data;
	if (!ctx) {
		fprintf(stderr, "Error: NULL io context in read handler\n");
		return -EINVAL;
	}

	// Extract data from context
	char *buffer = (char *)ctx->buffer;
	int client_fd = ctx->fd;
	int bytes_read = cqe->res;

	// We now have the correct client_fd from our context

	if (bytes_read <= 0) {
		// Connection closed or error
		printf("Connection closed or error (fd: %d, res: %d)\n",
		       client_fd, bytes_read);
		unregister_client_fd(client_fd);
		close(client_fd);
		free(buffer);
		free(ctx);
		return 0;
	}

	// Get or create buffer state for this connection
	struct buffer_state *state = get_buffer_state(client_fd);
	if (!state) {
		state = create_buffer_state(client_fd);
		if (!state) {
			free(buffer);
			free(ctx);
			return -ENOMEM;
		}
	}

	// Append new data to existing buffer
	if (!append_to_buffer(state, buffer, bytes_read)) {
		free(buffer);
		free(ctx);
		return -ENOMEM;
	}

	free(buffer); // Original buffer no longer needed
	free(ctx); // Free the context

	// Process the RPC record marker to get a complete RPC message
	int complete_size = process_record_marker(state, ring);

	if (complete_size > 0) {
		// We have a complete RPC message
		printf("Complete RPC message assembled (%d bytes)\n",
		       complete_size);

		// Create a task for processing
		struct task *task = malloc(sizeof(struct task));
		if (task) {
			// Copy the complete message
			task->buffer = malloc(complete_size);
			if (!task->buffer) {
				free(task);
				return -ENOMEM;
			}
			memcpy(task->buffer, state->record.data, complete_size);
			task->bytes_read = complete_size;
			task->fd = client_fd;

			// Extract XID for convenience
			if (complete_size >= 4) {
				task->xid = ntohl(*(uint32_t *)task->buffer);
			} else {
				task->xid = 0;
			}

			// Queue it for processing
			add_task(task);

			// Reset the record state for the next message
			state->record.total_len = 0;
			state->record.position = 0;
		}
	}

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
	io_context_t *ctx = create_io_context(OP_TYPE_WRITE, fd, send_buffer);
	if (!ctx) {
		free(send_buffer);
		return -ENOMEM;
	}

	// Submit the write operation to io_uring
	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
	io_uring_prep_write(sqe, fd, send_buffer, len + 4, 0);

	// Associate with the io context
	sqe->user_data = (uint64_t)(uintptr_t)ctx;

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

	// Create worker threads
	for (int i = 0; i < MAX_WORKER_THREADS; i++) {
		int *thread_id = malloc(sizeof(int));
		*thread_id = i;

		if (pthread_create(&worker_threads[i], NULL, worker_thread,
				   thread_id) == 0) {
			num_worker_threads++;
		} else {
			free(thread_id);
			fprintf(stderr, "Failed to create worker thread %d\n",
				i);
		}
	}

	// Setup NFS listener
	listener_fd = setup_listener(NFS_PORT);
	if (listener_fd < 0) {
		fprintf(stderr, "Failed to setup listener on port %d\n",
			NFS_PORT);
		exit_code = 1;
		goto out;
	}

	// Setup initial accept operation
	struct sockaddr_in client_address;
	socklen_t client_len = sizeof(client_address);

	// Create IO context for the accept operation
	io_context_t *accept_ctx =
		create_io_context(OP_TYPE_ACCEPT, listener_fd, NULL);
	if (!accept_ctx) {
		fprintf(stderr, "Failed to create accept context\n");
		exit_code = 1;
		goto out;
	}

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_accept(sqe, listener_fd,
			     (struct sockaddr *)&client_address, &client_len,
			     0);

	// Associate with the io context
	sqe->user_data = (uint64_t)(uintptr_t)accept_ctx;

	io_uring_submit(&ring);

	while (running) {
		// Set a timeout for io_uring_wait_cqe to allow checking the running flag
		struct __kernel_timespec ts = { .tv_sec = 1, .tv_nsec = 0 };

		int ret = io_uring_wait_cqe_timeout(&ring, &cqe, &ts);
		if (ret == -ETIME) {
			// Timeout - check running flag and continue
			continue;
		} else if (ret < 0) {
			fprintf(stderr, "io_uring_wait_cqe_timeout error: %s\n",
				strerror(-ret));
			continue;
		}

		if (cqe->res < 0) {
			fprintf(stderr, "CQE error: %s\n", strerror(-cqe->res));
		} else {
			// Get the IO context from user_data
			io_context_t *ctx =
				(io_context_t *)(uintptr_t)cqe->user_data;
			if (!ctx) {
				fprintf(stderr, "Error: NULL io context\n");
				io_uring_cqe_seen(&ring, cqe);
				continue;
			}

			switch (ctx->op_type) {
			case OP_TYPE_ACCEPT: {
				// Handle new connection
				int listen_fd = ctx->fd;
				int client_fd = cqe->res;

				printf("New connection accepted (fd: %d)\n",
				       client_fd);

				// Register this client
				register_client_fd(client_fd);

				// Prepare to read from this new connection
				char *buffer = malloc(BUFFER_SIZE);
				if (buffer) {
					// Create IO context for the read operation
					io_context_t *read_ctx =
						create_io_context(OP_TYPE_READ,
								  client_fd,
								  buffer);
					if (!read_ctx) {
						fprintf(stderr,
							"Failed to create read context\n");
						free(buffer);
						close(client_fd);
					} else {
						sqe = io_uring_get_sqe(&ring);
						io_uring_prep_read(
							sqe, client_fd, buffer,
							BUFFER_SIZE, 0);
						sqe->user_data =
							(uint64_t)(uintptr_t)
								read_ctx;
						io_uring_submit(&ring);
					}
				} else {
					perror("malloc");
					close(client_fd);
				}

				// Submit a new accept for this listener
				struct sockaddr_in client_address;
				socklen_t client_len = sizeof(client_address);

				// Create new IO context for the next accept
				io_context_t *accept_ctx = create_io_context(
					OP_TYPE_ACCEPT, listen_fd, NULL);
				if (!accept_ctx) {
					fprintf(stderr,
						"Failed to create accept context\n");
					free(ctx);
					io_uring_cqe_seen(&ring, cqe);
					continue;
				}

				sqe = io_uring_get_sqe(&ring);
				io_uring_prep_accept(
					sqe, listen_fd,
					(struct sockaddr *)&client_address,
					&client_len, 0);
				sqe->user_data =
					(uint64_t)(uintptr_t)accept_ctx;
				io_uring_submit(&ring);

				free(ctx); // Free the completed accept context
				break;
			}

			case OP_TYPE_READ: {
				op_read_handler(cqe, &ring);
				// Note: op_read_handler now handles freeing the context
				break;
			}

			case OP_TYPE_WRITE: {
				// Write completed, free the buffer
				free(ctx->buffer);
				free(ctx);
				break;
			}

			case OP_TYPE_NFS_REQ: {
				// NFS request write completed
				printf("NFS request sent successfully\n");
				// For client-side requests, we'd track for the response
				free(ctx);
				break;
			}

			default:
				fprintf(stderr, "Unknown operation type: %d\n",
					ctx->op_type);
				if (ctx->buffer) {
					free(ctx->buffer);
				}
				free(ctx);
				break;
			}
		}

		io_uring_cqe_seen(&ring, cqe);
	}

	printf("Main loop exited, cleaning up...\n");

	// Cleanup listener socket
	close(listener_fd);

	// Wait for worker threads to finish
	printf("Waiting for worker threads to exit...\n");
	for (int i = 0; i < num_worker_threads; i++) {
		pthread_join(worker_threads[i], NULL);
	}

	// Cleanup any pending requests
	printf("Cleaning up pending requests...\n");
	pthread_mutex_lock(&request_mutex);
	for (int i = 0; i < MAX_PENDING_REQUESTS; i++) {
		if (pending_requests[i]) {
			if (pending_requests[i]->private_data) {
				free(pending_requests[i]->private_data);
			}
			if (pending_requests[i]->buffer) {
				free(pending_requests[i]->buffer);
			}
			free(pending_requests[i]);
			pending_requests[i] = NULL;
		}
	}
	pthread_mutex_unlock(&request_mutex);

	// Cleanup connection buffers
	for (int i = 0; i < MAX_CONNECTIONS; i++) {
		if (conn_buffers[i]) {
			if (conn_buffers[i]->data) {
				free(conn_buffers[i]->data);
			}
			if (conn_buffers[i]->record.data) {
				free(conn_buffers[i]->record.data);
			}
			free(conn_buffers[i]);
		}
	}

out:
	// Wait for RCU grace period
	printf("Calling rcu_barrier()...\n");
	rcu_barrier();

	mount3_protocol_deregister();
	nfs3_protocol_deregister();

	// Clean up io_uring
	io_uring_queue_exit(&ring);

	printf("Shutdown complete\n");
	return exit_code;
}
