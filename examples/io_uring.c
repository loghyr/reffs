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
#include <linux/io_uring.h>
#include <fcntl.h>
#include <pthread.h>
#include <errno.h>
#include <signal.h>
#include <urcu.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <time.h>

#define BUFFER_SIZE 4096
#define QUEUE_DEPTH 1024
#define NFS_PORT 2049
#define REFFS_PORT 4098
#define NUM_LISTENERS 2
#define MAX_WORKER_THREADS 4
#define MAX_PENDING_REQUESTS 256

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

// Task struct with port info to distinguish between protocols
struct task {
	char *buffer;
	int bytes_read;
	int port; // Added to track which port the data came from
};

// Track file descriptors and their associated ports
struct fd_info {
	int fd;
	int port;
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

// Request tracking
struct nfs_request_context *pending_requests[MAX_PENDING_REQUESTS];
pthread_mutex_t request_mutex = PTHREAD_MUTEX_INITIALIZER;

// Queue for worker threads
pthread_mutex_t task_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t task_queue_cond = PTHREAD_COND_INITIALIZER;
struct task *task_queue[QUEUE_DEPTH];
int task_queue_head = 0;
int task_queue_tail = 0;

// Socket info storage
struct fd_info client_fds[QUEUE_DEPTH];
pthread_mutex_t client_fds_mutex = PTHREAD_MUTEX_INITIALIZER;

// Thread management
pthread_t worker_threads[MAX_WORKER_THREADS];
int num_worker_threads = 0;

// Forward declarations
void handle_nfs_protocol(struct task *task);
void handle_reffs_protocol(struct task *task);
int setup_io_uring(struct io_uring *ring);

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

// Signal handler
void signal_handler(int sig)
{
	printf("Received signal %d, initiating shutdown...\n", sig);
	running = 0;

	// Wake up any waiting worker threads
	pthread_cond_broadcast(&task_queue_cond);
}

void add_task(struct task *task)
{
	pthread_mutex_lock(&task_queue_mutex);
	task_queue[task_queue_tail] = task;
	task_queue_tail = (task_queue_tail + 1) % QUEUE_DEPTH;
	pthread_cond_signal(&task_queue_cond);
	pthread_mutex_unlock(&task_queue_mutex);
}

// Store client fd and its associated port
void register_client_fd(int fd, int port)
{
	pthread_mutex_lock(&client_fds_mutex);
	for (int i = 0; i < QUEUE_DEPTH; i++) {
		if (client_fds[i].fd == 0) {
			client_fds[i].fd = fd;
			client_fds[i].port = port;
			break;
		}
	}
	pthread_mutex_unlock(&client_fds_mutex);
}

// Get the port associated with a client fd
int get_client_port(int fd)
{
	int port = 0;
	pthread_mutex_lock(&client_fds_mutex);
	for (int i = 0; i < QUEUE_DEPTH; i++) {
		if (client_fds[i].fd == fd) {
			port = client_fds[i].port;
			break;
		}
	}
	pthread_mutex_unlock(&client_fds_mutex);
	return port;
}

// Remove client fd from tracking
void unregister_client_fd(int fd)
{
	pthread_mutex_lock(&client_fds_mutex);
	for (int i = 0; i < QUEUE_DEPTH; i++) {
		if (client_fds[i].fd == fd) {
			client_fds[i].fd = 0;
			client_fds[i].port = 0;
			break;
		}
	}
	pthread_mutex_unlock(&client_fds_mutex);
}

// Extract operation type from sqe->user_data
int get_op_type(uint64_t user_data)
{
	return (user_data >> 56) & 0xFF;
}

// Create user_data with operation type and context pointer
uint64_t create_user_data(int op_type, void *ptr)
{
	return ((uint64_t)op_type << 56) | ((uint64_t)ptr & 0x00FFFFFFFFFFFFFF);
}

// Extract pointer from user_data
void *get_data_ptr(uint64_t user_data)
{
	return (void *)(user_data & 0x00FFFFFFFFFFFFFF);
}

// Simplified XDR encoding for NFS GETATTR request
int encode_nfs_getattr_request(char *buffer, int buflen, uint32_t xid,
			       const char *path)
{
	// This is a simplified placeholder - in a real implementation,
	// you would use proper XDR encoding for RPC and NFS messages

	// Format: XID, RPC version, program, version, procedure + file handle
	int pos = 0;

	// RPC Header
	*(uint32_t *)(buffer + pos) = htonl(xid);
	pos += 4;

	// RPC Call (0)
	*(uint32_t *)(buffer + pos) = htonl(0);
	pos += 4;

	// RPC Version (2)
	*(uint32_t *)(buffer + pos) = htonl(2);
	pos += 4;

	// NFS Program (100003)
	*(uint32_t *)(buffer + pos) = htonl(100003);
	pos += 4;

	// NFS Version (3)
	*(uint32_t *)(buffer + pos) = htonl(3);
	pos += 4;

	// GETATTR Procedure (1)
	*(uint32_t *)(buffer + pos) = htonl(NFS3_GETATTR);
	pos += 4;

	// Auth flavor (0 = AUTH_NONE)
	*(uint32_t *)(buffer + pos) = htonl(0);
	pos += 4;

	// Auth body length (0)
	*(uint32_t *)(buffer + pos) = htonl(0);
	pos += 4;

	// Verifier flavor (0 = AUTH_NONE)
	*(uint32_t *)(buffer + pos) = htonl(0);
	pos += 4;

	// Verifier body length (0)
	*(uint32_t *)(buffer + pos) = htonl(0);
	pos += 4;

	// Simplified file handle (in real code you'd use the actual file handle)
	int path_len = strlen(path);
	*(uint32_t *)(buffer + pos) = htonl(path_len);
	pos += 4;

	memcpy(buffer + pos, path, path_len);
	pos += path_len;

	// Pad to 4-byte boundary if needed
	while (pos % 4 != 0) {
		buffer[pos++] = 0;
	}

	return pos;
}

// Simplified XDR decoding for NFS GETATTR response
// Returns 0 on success, negative value on error
int decode_getattr_response(const char *buffer, int buflen, uint32_t *xid,
			    void *attrs)
{
	// This is a simplified placeholder
	// In a real implementation, you would parse the XDR-encoded RPC/NFS response

	if (buflen < 4) {
		return -EINVAL;
	}

	// Extract XID
	*xid = ntohl(*(uint32_t *)buffer);

	// In a real implementation, you would parse the status and attributes
	// For this example, we'll assume success and simulated attributes

	return 0;
}

// Handler for GETATTR response
void handle_getattr_response(struct nfs_request_context *ctx, void *response,
			     int res_len, int status)
{
	printf("GETATTR response received: XID=%u, status=%d\n", ctx->xid,
	       status);

	if (status == 0 && response) {
		printf("File attributes for %s received\n",
		       (char *)ctx->private_data);
		// In a real implementation, you would process the attributes here
	} else {
		printf("GETATTR failed for %s\n", (char *)ctx->private_data);
	}

	// Free the context
	free(ctx->private_data);
	free(ctx->buffer);
	free(ctx);
}

// NFS protocol handler
void handle_nfs_protocol(struct task *task)
{
	printf("NFS Protocol Handler: Received %d bytes on port %d\n",
	       task->bytes_read, task->port);

	// In a real implementation, you would:
	// 1. Parse the RPC/NFS message
	// 2. Extract the XID
	// 3. Process the operation
	// 4. Send a response

	// For demonstration, we'll just print the first few bytes
	printf("RPC/NFS data first bytes: ");
	for (int i = 0; i < 16 && i < task->bytes_read; i++) {
		printf("%02x ", (unsigned char)task->buffer[i]);
	}
	printf("\n");
}

// Reffs-Ctl protocol handler
void handle_reffs_protocol(struct task *task)
{
	printf("Reffs-Ctl Protocol Handler: Received %d bytes on port %d\n",
	       task->bytes_read, task->port);
	printf("Reffs-Ctl data: %.*s\n", task->bytes_read, task->buffer);
}

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
			// Route to appropriate handler based on port
			if (task->port == NFS_PORT) {
				handle_nfs_protocol(task);
			} else if (task->port == REFFS_PORT) {
				handle_reffs_protocol(task);
			} else {
				printf("Unknown protocol on port %d: %.*s",
				       task->port, task->bytes_read,
				       task->buffer);
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

int setup_io_uring(struct io_uring *ring)
{
	if (io_uring_queue_init(QUEUE_DEPTH, ring, 0) < 0) {
		perror("io_uring_queue_init");
		return -1;
	}
	return 0;
}

// Create and configure a socket for the given port
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

// Function to connect to a remote NFS server
int connect_to_nfs_server(const char *server_ip, int port,
			  struct io_uring *ring)
{
	int sockfd;
	struct sockaddr_in server_addr;

	// Create socket
	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0) {
		perror("socket");
		return -1;
	}

	// Set socket to non-blocking mode
	int flags = fcntl(sockfd, F_GETFL, 0);
	fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

	// Setup server address
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(port);

	if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
		perror("inet_pton");
		close(sockfd);
		return -1;
	}

	// Use io_uring to connect
	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
	io_uring_prep_connect(sqe, sockfd, (struct sockaddr *)&server_addr,
			      sizeof(server_addr));

	// Mark this as a connect operation
	sqe->user_data =
		create_user_data(OP_TYPE_CONNECT, (void *)(uintptr_t)sockfd);

	io_uring_submit(ring);

	printf("Connection request submitted to %s:%d\n", server_ip, port);

	return sockfd;
}

// Function to send an NFS GETATTR request
int send_nfs_getattr(int sockfd, const char *file_path, struct io_uring *ring)
{
	// Create request context
	struct nfs_request_context *ctx =
		malloc(sizeof(struct nfs_request_context));
	if (!ctx) {
		return -ENOMEM;
	}

	// Allocate buffer for the NFS/RPC message
	char *buffer = malloc(BUFFER_SIZE);
	if (!buffer) {
		free(ctx);
		return -ENOMEM;
	}

	// Fill context
	ctx->xid = generate_xid();
	ctx->operation = NFS3_GETATTR;
	ctx->sockfd = sockfd;
	ctx->private_data = strdup(file_path);
	ctx->callback = handle_getattr_response;
	ctx->buffer = buffer;

	// Register the request for tracking
	if (register_request(ctx) < 0) {
		free(ctx->private_data);
		free(buffer);
		free(ctx);
		return -ENOMEM;
	}

	// Encode the GETATTR request
	int msg_len = encode_nfs_getattr_request(buffer, BUFFER_SIZE, ctx->xid,
						 file_path);

	// Submit the write operation to io_uring
	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
	io_uring_prep_write(sqe, sockfd, buffer, msg_len, 0);

	// Mark this as an NFS request operation
	sqe->user_data = create_user_data(OP_TYPE_NFS_REQ, ctx);

	io_uring_submit(ring);

	printf("Sent GETATTR request for %s with XID %u\n", file_path,
	       ctx->xid);

	// Allocate another buffer for the response
	buffer = malloc(BUFFER_SIZE);
	if (!buffer) {
		// Request is already registered, we should deregister it
		// For simplicity, we'll just let it time out in a real implementation
		return -ENOMEM;
	}

	// Submit a read for the response
	sqe = io_uring_get_sqe(ring);
	io_uring_prep_read(sqe, sockfd, buffer, BUFFER_SIZE, 0);

	// Store the buffer pointer in user_data
	sqe->user_data = create_user_data(OP_TYPE_READ, buffer);

	io_uring_submit(ring);

	return 0;
}

int op_read_handler(struct io_uring_cqe *cqe)
{
	// Data received
	char *buffer = (char *)get_data_ptr(cqe->user_data);
	int bytes_read = cqe->res;

	if (bytes_read > 0) {
		// Check for NFS response
		if (bytes_read >= 4) {
			uint32_t xid;
			void *attrs =
				malloc(256); // Simplified attribute structure

			if (decode_getattr_response(buffer, bytes_read, &xid,
						    attrs) == 0) {
				// Find the corresponding request
				struct nfs_request_context *ctx =
					find_request_by_xid(xid);
				if (ctx) {
					// Call the callback
					ctx->callback(ctx, attrs, bytes_read,
						      0);
					free(attrs);
				} else {
					printf("Received response for unknown XID: %u\n",
					       xid);
					free(attrs);
				}
			} else {
				// Not a valid NFS response, process as regular data
				int client_fd =
					cqe->flags; // Assuming flags contains the fd
				int client_port = get_client_port(client_fd);

				struct task *task = malloc(sizeof(struct task));
				if (task) {
					task->buffer = buffer;
					task->bytes_read = bytes_read;
					task->port = client_port;
					add_task(task);

					// Submit another read
					buffer = malloc(BUFFER_SIZE);
					if (buffer) {
						sqe = io_uring_get_sqe(&ring);
						io_uring_prep_read(
							sqe, client_fd, buffer,
							BUFFER_SIZE, 0);
						sqe->user_data =
							create_user_data(
								OP_TYPE_READ,
								buffer);
						io_uring_submit(&ring);
					}
				} else {
					free(buffer);
				}
			}
		}
	} else {
		// Connection closed or error
		int client_fd = cqe->flags; // Assuming flags contains the fd
		printf("Connection closed (fd: %d)\n", client_fd);
		unregister_client_fd(client_fd);
		close(client_fd);
		free(buffer);
	}

	return 0;
}

int main()
{
	int listener_fds[NUM_LISTENERS];
	int ports[NUM_LISTENERS] = { NFS_PORT, REFFS_PORT };
	struct io_uring ring;
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;

	// Initialize userspace RCU
	rcu_init();

	// Initialize pending requests array
	memset(pending_requests, 0, sizeof(pending_requests));

	// Setup signal handlers
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = signal_handler;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	// Initialize client_fds tracking array
	memset(client_fds, 0, sizeof(client_fds));

	// Setup io_uring
	if (setup_io_uring(&ring) < 0) {
		return 1;
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

	// Setup listeners for both ports
	for (int i = 0; i < NUM_LISTENERS; i++) {
		listener_fds[i] = setup_listener(ports[i]);
		if (listener_fds[i] < 0) {
			fprintf(stderr, "Failed to setup listener on port %d\n",
				ports[i]);
			// Clean up previous listeners
			for (int j = 0; j < i; j++) {
				close(listener_fds[j]);
			}
			io_uring_queue_exit(&ring);
			return 1;
		}

		// Setup initial accept operations
		struct sockaddr_in client_address;
		socklen_t client_len = sizeof(client_address);

		sqe = io_uring_get_sqe(&ring);
		io_uring_prep_accept(sqe, listener_fds[i],
				     (struct sockaddr *)&client_address,
				     &client_len, 0);

		// Mark this as an accept operation with the port
		sqe->user_data = create_user_data(
			OP_TYPE_ACCEPT,
			(void *)(uintptr_t)((listener_fds[i] << 16) |
					    ports[i]));

		io_uring_submit(&ring);
	}

	// Example: Connect to a remote NFS server
	/*
    int remote_nfs_fd = connect_to_nfs_server("192.168.1.100", NFS_PORT, &ring);
    if (remote_nfs_fd >= 0) {
        printf("NFS connection request submitted\n");
    }
    */

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
			// Extract operation type
			int op_type = get_op_type(cqe->user_data);
			void *data_ptr = get_data_ptr(cqe->user_data);

			switch (op_type) {
			case OP_TYPE_ACCEPT: {
				// Handle new connection
				uintptr_t fd_port = (uintptr_t)data_ptr;
				int listen_fd = fd_port >> 16;
				int port = fd_port & 0xFFFF;
				int client_fd = cqe->res;

				// Register this client fd with its associated port
				register_client_fd(client_fd, port);

				printf("New connection on port %d (fd: %d)\n",
				       port, client_fd);

				// Prepare to read from this new connection
				char *buffer = malloc(BUFFER_SIZE);
				if (buffer) {
					sqe = io_uring_get_sqe(&ring);
					io_uring_prep_read(sqe, client_fd,
							   buffer, BUFFER_SIZE,
							   0);
					sqe->user_data = create_user_data(
						OP_TYPE_READ, buffer);
					io_uring_submit(&ring);
				} else {
					perror("malloc");
					close(client_fd);
				}

				// Submit a new accept for this listener
				struct sockaddr_in client_address;
				socklen_t client_len = sizeof(client_address);
				sqe = io_uring_get_sqe(&ring);
				io_uring_prep_accept(
					sqe, listen_fd,
					(struct sockaddr *)&client_address,
					&client_len, 0);
				sqe->user_data = create_user_data(
					OP_TYPE_ACCEPT,
					(void *)(uintptr_t)((listen_fd << 16) |
							    port));
				io_uring_submit(&ring);
				break;
			}

			case OP_TYPE_CONNECT: {
				// Connection completion
				int sockfd = (int)(uintptr_t)data_ptr;
				if (cqe->res == 0) {
					printf("Outgoing connection established (fd: %d)\n",
					       sockfd);

					// Example: Now we can send the GETATTR request
					// send_nfs_getattr(sockfd, "/path/to/file", &ring);
				} else {
					printf("Outgoing connection failed (fd: %d): %s\n",
					       sockfd, strerror(-cqe->res));
					close(sockfd);
				}
				break;
			}

			case OP_TYPE_READ: {
				op_read_handler(cqe);
				break;
			}

			case OP_TYPE_WRITE: {
				// Write completed
				// For simplicity, we assume this is a response to a client
				// and we're done with the buffer
				free(data_ptr);
				break;
			}

			case OP_TYPE_NFS_REQ: {
				// NFS request write completed
				struct nfs_request_context *ctx =
					(struct nfs_request_context *)data_ptr;
				printf("NFS %s request (XID %u) sent successfully\n",
				       ctx->operation == NFS3_GETATTR ?
					       "GETATTR" :
					       "OTHER",
				       ctx->xid);
				// The context remains registered for the response
				break;
			}

			default:
				fprintf(stderr, "Unknown operation type: %d\n",
					op_type);
				if (data_ptr) {
					free(data_ptr);
				}
				break;
			}
		}

		io_uring_cqe_seen(&ring, cqe);
	}

	printf("Main loop exited, cleaning up...\n");

	// Cleanup listener sockets
	for (int i = 0; i < NUM_LISTENERS; i++) {
		close(listener_fds[i]);
	}

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
			free(pending_requests[i]->private_data);
			free(pending_requests[i]->buffer);
			free(pending_requests[i]);
			pending_requests[i] = NULL;
		}
	}
	pthread_mutex_unlock(&request_mutex);

	// Wait for RCU grace period
	printf("Calling rcu_barrier()...\n");
	rcu_barrier();

	// Clean up io_uring
	io_uring_queue_exit(&ring);

	printf("Shutdown complete\n");
	return 0;
}
