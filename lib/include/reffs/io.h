/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_IO_H
#define _REFFS_IO_H

#include <stdint.h>
#include <liburing.h>
#include <pthread.h>
#include <stdbool.h>
#include <sys/socket.h>

#include <urcu.h>
#include <urcu/rculfhash.h>
#include <urcu/ref.h>

#include "reffs/task.h"
#include "reffs/network.h"

#define BUFFER_SIZE 4096
#define QUEUE_DEPTH 2048
#define NUM_LISTENERS 1
#define MAX_WORKER_THREADS 4
#define MAX_PENDING_REQUESTS 256
#define MAX_CONNECTIONS 1024 // Maximum number of concurrent client connections
#define MAX_LISTENERS 1024

#define IO_URING_WAIT_SEC (0)
#define IO_URING_WAIT_NSEC (100000000)

#define IO_URING_WAIT_US \
	((IO_URING_WAIT_SEC * 1000000) + (IO_URING_WAIT_NSEC / 1000))

#define REFFS_IO_MAX_RETRIES (3)

// Opcodes for different packet types
enum op_type {
	OP_TYPE_ACCEPT = 1,
	OP_TYPE_READ = 2,
	OP_TYPE_WRITE = 3,
	OP_TYPE_CONNECT = 4,
	OP_TYPE_RPC_REQ = 5
};

// IO operation context structure
struct io_context {
	struct rcu_head ic_rcu;
	struct cds_lfht_node ic_next;

	enum op_type ic_op_type;
	int ic_fd;
	uint32_t ic_id;
	void *ic_buffer;

	size_t ic_buffer_len;
	size_t ic_position;
	uint32_t ic_xid;

#define IO_CONTEXT_IS_HASHED (1ULL << 0)
#define IO_CONTEXT_MARKED_CANCELLED (1ULL << 1)
#define IO_CONTEXT_IS_CANCELLED (1ULL << 2)
#define IO_CONTEXT_IS_CANCELLED_HASH (1ULL << 3)
#define IO_CONTEXT_MARKED_DESTROYED (1ULL << 4)
#define IO_CONTEXT_IS_DESTROYED (1ULL << 5)
#define IO_CONTEXT_IS_DESTROYED_HASH (1ULL << 6)
	uint64_t ic_state;

	time_t ic_action_time;

	struct connection_info ic_ci;
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

#define CONNECTION_TIMEOUT_SECONDS 60 // Seconds of inactivity before timeout
#define CONNECTION_TIMEOUT_CHECK_INTERVAL 10 // Check every 10 seconds

// Connection states
enum conn_state {
	CONN_UNUSED = 0, // Socket slot not in use
	CONN_LISTENING, // Server socket listening for connections
	CONN_ACCEPTING, // Server socket in the midst of accepting connection
	CONN_ACCEPTED, // Client connection just accepted
	CONN_CONNECTING, // Client-initiated connection in progress
	CONN_CONNECTED, // Successfully connected (both client and server)
	CONN_READING, // Socket reading in progress
	CONN_WRITING, // Socket writing in progress
	CONN_READWRITE, // Socket with both read and write in progress
	CONN_DISCONNECTING, // In the process of disconnecting
	CONN_ERROR // Connection in error state
};

enum conn_role {
	CONN_ROLE_UNKNOWN = 0,
	CONN_ROLE_CLIENT, // Client-initiated connection
	CONN_ROLE_SERVER, // Server listener
	CONN_ROLE_ACCEPTED // Server-accepted client connection
};

// Connection info structure
struct conn_info {
	int ci_fd; // File descriptor
	enum conn_state ci_state; // Current connection state
	enum conn_role ci_role; // Connection role
	time_t ci_last_activity; // Last activity timestamp
	struct sockaddr_storage ci_peer; // Peer address
	socklen_t ci_peer_len; // Peer address length
	struct sockaddr_storage ci_local; // Local address
	socklen_t ci_local_len; // Local address length
	uint32_t ci_xid; // Associated XID
	int ci_error; // Last error code
	int ci_read_count; // Number of pending read operations
	int ci_write_count; // Number of pending write operations
	int ci_accept_count; // Number of pending accept operations
	int ci_connect_count; // Number of pending connect operations
};

// Function declarations
int io_handler_init(struct io_uring *ring);
void io_handler_fini(struct io_uring *ring);
void io_handler_main_loop(volatile sig_atomic_t *running,
			  struct io_uring *ring);
void io_handler_stop(void);

int setup_listener(int port);
int request_accept_op(int fd, struct connection_info *ci,
		      struct io_uring *ring);
int request_additional_read_data(int fd, struct connection_info *ci,
				 struct io_uring *ring);

int create_worker_threads(volatile sig_atomic_t *running);
void wait_for_worker_threads(void);

void add_task(struct task *task);

void register_client_fd(int fd);
void unregister_client_fd(int fd);

bool append_to_buffer(struct buffer_state *bs, const char *data, size_t len);

struct buffer_state *create_buffer_state(int fd);
struct buffer_state *get_buffer_state(int fd);

int request_more_read_data(struct buffer_state *bs, struct io_uring *ring,
			   struct io_context *ic);

// Handlers
int io_handle_accept(struct io_context *ic, int client_fd,
		     struct io_uring *ring);
int io_handle_connect(struct io_context *ic, int result, struct io_uring *ring);
int io_handle_read(struct io_context *ic, int bytes_read,
		   struct io_uring *ring);
int io_handle_write(struct io_context *ic, int bytes_written,
		    struct io_uring *ring);

// Context handling
struct io_context *io_context_create(enum op_type op_type, int fd, void *buffer,
				     size_t buffer_len);
void io_context_destroy(struct io_context *ic);
void io_context_update_time(struct io_context *ic);

void io_context_list_active(bool list_em);
void io_context_release_active(struct io_uring *ring);
void io_context_check_stalled(struct io_uring *ring);
void io_context_release_cancelled(void);
void io_context_release_destroyed(void);

int io_context_init(void);
int io_context_fini(void);

uint64_t io_context_get_created(void);
uint64_t io_context_get_freed(void);
void io_context_log_stats(void);

void *io_worker_thread(void *arg);
void wake_worker_threads(void);

int io_rpc_trans_cb(struct rpc_trans *rt);

int io_register_request(struct rpc_trans *rt);
struct rpc_trans *io_find_request_by_xid(uint32_t xid);
int io_unregister_request(uint32_t xid);

int io_send_request(struct rpc_trans *rt);

static inline const char *io_op_type_to_str(enum op_type op)
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

// Connection tracking functions
int io_conn_init(void);
struct conn_info *io_conn_register(int fd, enum conn_state initial_state,
				   enum conn_role role);
struct conn_info *io_conn_get(int fd);
void io_conn_update_state(int fd);
bool io_conn_is_state(int fd, enum conn_state state);
int io_conn_unregister(int fd);
void io_conn_cleanup(void);
const char *conn_state_to_str(enum conn_state state);
const char *conn_role_to_str(enum conn_role role);
int io_conn_check_timeouts(time_t timeout_seconds);

int io_socket_close(int fd, int error);
void io_add_listener(int fd);

void io_conn_dump(int fd);
void io_conn_dump_all(void);

// New reference counting API
int io_conn_add_read_op(int fd);
int io_conn_remove_read_op(int fd);
int io_conn_add_write_op(int fd);
int io_conn_remove_write_op(int fd);
int io_conn_add_accept_op(int fd);
int io_conn_remove_accept_op(int fd);
int io_conn_add_connect_op(int fd);
int io_conn_remove_connect_op(int fd);
int io_conn_set_error(int fd, int error_code);

// Helper functions for operation tracking
bool io_conn_has_read_ops(int fd);
bool io_conn_has_write_ops(int fd);

void io_check_for_listener_restart(int fd, struct connection_info *ci,
				   struct io_uring *ring);

#endif /* _REFFS_IO_H */
