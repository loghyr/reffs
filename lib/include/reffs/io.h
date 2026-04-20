/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_IO_H
#define _REFFS_IO_H

#include <signal.h>
#include <stdint.h>
#include <pthread.h>
#include <stdbool.h>
#include <sys/socket.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>

#include <sched.h>
#include <urcu.h>
#include <urcu/rculfhash.h>
#include <urcu/ref.h>

#include "reffs/ring.h"
#include "reffs/task.h"
#include "reffs/network.h"
#include "reffs/tls.h"

// Maximum size for a single write
#define IO_MAX_WRITE_SIZE (1024 * 1024)

#define BUFFER_SIZE 4096

#ifdef HAVE_IO_URING_STRESS
#define QUEUE_DEPTH 128
#else
#define QUEUE_DEPTH 2048
#endif

#define NUM_LISTENERS 1
/* Keep in sync with REFFS_MAX_WORKER_THREADS in settings.h */
#define MAX_WORKER_THREADS 64
#define MAX_PENDING_REQUESTS 256
#define MAX_CONNECTIONS 65536 // Maximum number of concurrent client connections
#define MAX_LISTENERS 1024

#define IO_URING_WAIT_SEC (1)
#define IO_URING_WAIT_NSEC (0)

#define IO_URING_WAIT_US \
	((IO_URING_WAIT_SEC * 1000000) + (IO_URING_WAIT_NSEC / 1000))

#define REFFS_IO_RETRY_US (1000) // 1ms for ring contention retries

#define REFFS_IO_MAX_RETRIES (3)
#define REFFS_IO_RING_RETRIES (100) // Aggressive retries for SQE acquisition

/* Queue depth for the backend file-I/O ring (separate from network ring). */
#define BACKEND_QUEUE_DEPTH 512

// Opcodes for different packet types
enum op_type {
	OP_TYPE_ACCEPT = 1,
	OP_TYPE_READ = 2,
	OP_TYPE_WRITE = 3,
	OP_TYPE_CONNECT = 4,
	OP_TYPE_RPC_REQ = 5,
	OP_TYPE_HEARTBEAT = 6,
	OP_TYPE_ALL = 7,
	OP_TYPE_BACKEND_PREAD = 8, /* async pread via backend ring */
	OP_TYPE_BACKEND_PWRITE = 9, /* async pwrite via backend ring */
};

struct rpc_trans;

/*
 * IO operation context -- opaque to callers outside lib/io/.  The full
 * definition lives in lib/io/io_internal.h; diagnostic callers use
 * io_context_probe_snapshot() below to get a POD copy instead of
 * reaching into fields.  Mirrors struct conn_info (commit 9cfa366e)
 * and ring_context (see reffs/ring.h).
 */
struct io_context;

/* IO_CONTEXT_* state flag bits -- shared with the probe1 wire format
 * (see lib/probe1/probe1_xdr.x), so they stay in the public header
 * even though struct io_context itself is opaque.  The static_asserts
 * in probe1_server.c keep wire and internal values in lockstep.
 */
#define IO_CONTEXT_ENTRY_STATE_ACTIVE (1ULL << 0)
#define IO_CONTEXT_ENTRY_STATE_MARKED_DESTROYED (1ULL << 1)
#define IO_CONTEXT_ENTRY_STATE_PENDING_FREE (1ULL << 2)
#define IO_CONTEXT_DIRECT_TLS_DATA (1ULL << 3)
#define IO_CONTEXT_TLS_BIO_PROCESSED (1ULL << 4)
#define IO_CONTEXT_SUBMITTED_EAGAIN (1ULL << 5)
/*
 * Set when io_conn_write_try_start() grants this context the write gate for
 * its fd.  Cleared only when io_conn_write_done() passes the gate to the
 * next queued context.  Prevents concurrent 1MB write SQEs on the same
 * socket fd from interleaving TCP segments and corrupting large reads.
 *
 * Any code that propagates this flag to a continuation io_context (e.g. the
 * partial-write path in io_handle_write) MUST also copy ic_write_gen so that
 * io_conn_write_done() can validate the generation and release the gate.
 */
#define IO_CONTEXT_WRITE_OWNED (1ULL << 6)

/*
 * Buffer-state structs used by the record-marker reassembly layer.
 * Full definitions are in lib/io/io_internal.h; external callers
 * only pass pointers through the public API.
 */
struct record_state;
struct buffer_state;

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

/*
 * struct conn_info is opaque to callers outside lib/io/.  The full
 * definition lives in lib/io/io_internal.h; reach into fields only
 * via the io_conn_* accessors below.  Mirrors the ring_context
 * opacity pattern -- see reffs/ring.h.
 */
struct conn_info;

/* ------------------------------------------------------------------ */
/* Network ring (existing)                                            */
/* ------------------------------------------------------------------ */
/*
 * tls_cert, tls_key, tls_ca may be NULL -- falls back to env vars
 * then /etc/tlshd/ defaults.
 */
int io_handler_init(struct ring_context *rc, const char *tls_cert,
		    const char *tls_key, const char *tls_ca);
void io_handler_fini(struct ring_context *rc);
void io_handler_main_loop(volatile sig_atomic_t *running,
			  struct ring_context *rc);
void io_handler_stop(void);
void io_handler_signal_shutdown(void);

/*
 * Backend-agnostic shutdown of lib/io/net_state.c state (request
 * table, pending buffers).  Backends must call this from their
 * io_handler_fini after draining in-flight operations.
 */
void io_net_state_fini(void);

/* ------------------------------------------------------------------ */
/* Backend file-I/O ring                                              */
/* ------------------------------------------------------------------ */
int io_backend_init(struct ring_context *rc);
void io_backend_fini(struct ring_context *rc);
void io_backend_main_loop(volatile sig_atomic_t *running,
			  struct ring_context *rc);

int io_request_backend_pread(int fd, void *buf, size_t len, off_t offset,
			     struct rpc_trans *rt, struct ring_context *rc);
int io_request_backend_pwrite(int fd, const void *buf, size_t len, off_t offset,
			      struct rpc_trans *rt, struct ring_context *rc);

void io_backend_set_global(struct ring_context *rc);
struct ring_context *io_backend_get_global(void);

struct ring_context *io_network_get_global(void);

int io_lsnr_setup_ipv4(int port);
int io_lsnr_setup_ipv6(int port);
int *io_lsnr_setup(int port);

int io_request_accept_op(int fd, struct connection_info *ci,
			 struct ring_context *rc);
int io_request_read_op(int fd, struct connection_info *ci,
		       struct ring_context *rc);
int io_request_write_op(int fd, char *buf, int len, uint64_t state,
			struct connection_info *ci, struct ring_context *rc);

/*
 * Re-submit a write on an existing io_context.  Used by rpc_trans_writer
 * to submit the next chunk of an ongoing reply without reallocating the
 * ic.  Each backend issues the appropriate primitive (io_uring_prep_write
 * on liburing, kevent(EV_ADD|EV_ONESHOT, EVFILT_WRITE) on kqueue).
 *
 * Caller must have ic->ic_buffer valid and ic->ic_position pointing at
 * the next byte to send.  The primitive computes chunk_size (clamped
 * to IO_MAX_WRITE_SIZE) and sets ic->ic_expected_len before submitting.
 *
 * Returns 0 on successful submission, -errno on permanent failure
 * (ic is destroyed and the socket is closed in the failure path).
 */
int io_resubmit_write(struct io_context *ic, struct ring_context *rc);

/*
 * Re-submit a read on an existing io_context.  Symmetric to
 * io_resubmit_write: reuses ic->ic_buffer (size BUFFER_SIZE) to
 * submit the next read on ic->ic_fd without reallocating.  Called
 * by io_handle_read's "get_more" path; preserves the ic-reuse
 * perf optimization on the hot read path across both backends.
 *
 * Returns 0 on successful submission, -errno on permanent failure.
 */
int io_resubmit_read(struct io_context *ic, struct ring_context *rc);

int create_worker_threads(volatile sig_atomic_t *running,
			  unsigned int nworkers);
void wait_for_worker_threads(void);

void io_mark_main_thread(void);
void add_task(struct task *task);

void io_client_fd_register(int fd);
void io_client_fd_unregister(int fd);

// Buffers
bool io_buffer_append(struct buffer_state *bs, const char *data, size_t len);
struct buffer_state *io_buffer_state_create(int fd);
struct buffer_state *io_buffer_state_get(int fd);

// Handlers
int io_handle_accept(struct io_context *ic, int client_fd,
		     struct ring_context *rc);
int io_handle_connect(struct io_context *ic, int result,
		      struct ring_context *rc);
int io_handle_read(struct io_context *ic, int bytes_read,
		   struct ring_context *rc);
int io_handle_write(struct io_context *ic, int bytes_written,
		    struct ring_context *rc);

// Context handling
struct io_context *io_context_create(enum op_type op_type, int fd, void *buffer,
				     size_t buffer_len);
void io_context_destroy(struct io_context *ic);
void io_context_update_time(struct io_context *ic);

void io_context_list_active(bool list_em);
void io_context_release_active(void);
void io_context_check_stalled(void);
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
	case OP_TYPE_HEARTBEAT:
		return "HEARTBEAT";
	case OP_TYPE_ALL:
		return "ALL";
	case OP_TYPE_BACKEND_PREAD:
		return "BACKEND_PREAD";
	case OP_TYPE_BACKEND_PWRITE:
		return "BACKEND_PWRITE";
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
const char *io_conn_state_to_str(enum conn_state state);
const char *io_conn_role_to_str(enum conn_role role);
int io_conn_check_timeouts(time_t timeout_seconds);

int io_socket_close(int fd, int error);
void io_add_listener(int fd);

void io_conn_dump(int fd);
void io_conn_dump_all(void);

struct io_context_stats {
	uint64_t ics_created;
	uint64_t ics_freed;
	uint64_t ics_active_cancelled;
	uint64_t ics_active_destroyed;
	uint64_t ics_cancelled_freed;
	uint64_t ics_destroyed_freed;
};

void io_context_stats(struct io_context_stats *ics);

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

/*
 * Accessors for callers outside lib/io/ that used to reach into
 * struct conn_info directly.  Return false / no-op if fd has no
 * registered connection.
 */
bool io_conn_is_tls_enabled(int fd);
void io_conn_set_tls_handshaking(int fd, bool handshaking);

/*
 * Per-fd write serialization gate.
 *
 * io_conn_write_try_start() -- atomically claim the write gate for fd.
 *   Returns true  if the gate was free (caller may submit a write SQE).
 *   Returns false if the gate was held (ic is queued; caller must return).
 *   If the fd is gone the gate is considered free so the caller will fail
 *   naturally on SQE submission.
 *
 * io_conn_write_done() -- release the gate and return the next queued
 *   io_context (with WRITE_OWNED set), or NULL if the queue is empty.
 *   The gate remains active (ci_write_active == true) when a next ic is
 *   returned, so the caller simply calls rpc_trans_writer(next_ic, rc).
 */
bool io_conn_write_try_start(int fd, struct io_context *ic);
struct io_context *io_conn_write_done(int fd, uint32_t gen);

void io_check_for_listener_restart(int fd, struct connection_info *ci,
				   struct ring_context *rc);

// Heartbeat code:
int io_heartbeat_init(struct ring_context *rc);
int io_schedule_heartbeat(struct ring_context *rc);
int io_handle_heartbeat(struct io_context *ic, int result,
			struct ring_context *rc);
void io_heartbeat_update_completions(uint64_t count);
int *io_heartbeat_get_listeners(int *num);
uint32_t io_heartbeat_period_get(void);
uint32_t io_heartbeat_period_set(uint32_t seconds);

/*
 * Diagnostic snapshot of an io_context (used by probe1 to dump active
 * contexts without reaching into the opaque struct).  Fields mirror
 * the relevant subset of struct io_context as of the probe call;
 * no pointers into the live state, safe to retain after the call.
 */
struct io_context_snapshot {
	enum op_type op_type;
	int fd;
	uint32_t id;
	uint32_t xid;
	size_t buffer_len;
	size_t position;
	size_t expected_len;
	uint64_t state;
	uint64_t count;
	time_t action_time;
	struct connection_info ci;
};

/*
 * Capture a point-in-time snapshot of all io_contexts whose fd (if
 * non-zero), op_type (if not OP_TYPE_ALL), and state mask match.
 * Returns an array of *out_count entries on success, or NULL if no
 * context matches or allocation fails.  Free with
 * io_context_probe_snapshot_free().
 */
struct io_context_snapshot *io_context_probe_snapshot(int fd, enum op_type op,
						      uint64_t state_mask,
						      int *out_count);
void io_context_probe_snapshot_free(struct io_context_snapshot *arr);

#endif /* _REFFS_IO_H */
