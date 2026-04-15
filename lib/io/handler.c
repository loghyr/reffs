/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <errno.h>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <linux/time_types.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#include "reffs/log.h"
#include "reffs/io.h"
#include "reffs/ring.h"
#include "tsan_uring.h"
#include "reffs/rpc.h"
#include "reffs/trace/io.h"

struct connection_info;

/* Global network ring pointer (set once in io_handler_init). */
static struct ring_context *g_network_rc;

struct ring_context *io_network_get_global(void)
{
	return g_network_rc;
}

/* GSS context cache + server credential -- declared in gss_context.h. */
int gss_ctx_cache_init(void);
void gss_ctx_cache_fini(void);
int gss_server_cred_init(void);
void gss_server_cred_fini(void);
_Bool gss_server_cred_is_available(void);

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

	TRACE("rt=%p xid=0x%08x", (void *)rt, rt->rt_info.ri_xid);
	// Find an empty slot
	for (int i = 0; i < MAX_PENDING_REQUESTS; i++) {
		if (pending_requests[i] == NULL) {
			TRACE("rt=%p xid=0x%08x", (void *)rt,
			      rt->rt_info.ri_xid);
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

	TRACE("xid=0x%08x", xid);
	pthread_mutex_lock(&request_mutex);
	for (int i = 0; i < MAX_PENDING_REQUESTS; i++) {
		if (pending_requests[i] &&
		    pending_requests[i]->rt_info.ri_xid == xid) {
			TRACE("rt=%p xid=0x%08x", (void *)pending_requests[i],
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
	TRACE("xid=0x%08x", xid);
	pthread_mutex_lock(&request_mutex);
	for (int i = 0; i < MAX_PENDING_REQUESTS; i++) {
		if (pending_requests[i] &&
		    pending_requests[i]->rt_info.ri_xid == xid) {
			TRACE("rt=%p xid=0x%08x", (void *)pending_requests[i],
			      xid);
			pending_requests[i] = NULL;
			pthread_mutex_unlock(&request_mutex);
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

/*
 * Shutdown eventfd: the signal handler writes to this to produce a
 * CQE that wakes io_uring_wait_cqe_timeout immediately.  Without it,
 * the wait may not return on SIGTERM when the ring has pending
 * multishot accepts or in-flight I/O.
 */
static int shutdown_efd = -1;

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
		TRACE("io_uring NODROP feature is supported - CQ entries won't be lost");
	} else {
		TRACE("WARNING: io_uring NODROP feature not supported - CQ overflow will drop entries");
	}

	TRACE("Initialized io_uring with SQ size %d, CQ size %d",
	      params.sq_entries, params.cq_entries);
	return 0;
}

int io_handler_init(struct ring_context *rc, const char *tls_cert,
		    const char *tls_key, const char *tls_ca)
{
	// Store global reference for callback paths (CB_RECALL etc.)
	g_network_rc = rc;

	// Initialize pending requests array
	memset(conn_buffers, 0, sizeof(conn_buffers));

	if (pthread_mutex_init(&rc->rc_mutex, NULL) != 0) {
		LOG("Failed to initialize ring mutex");
		return -1;
	}

	// Setup io_uring
	if (setup_io_uring(rc) < 0)
		return -1;

	/*
	 * Create a shutdown eventfd.  The signal handler writes to it
	 * to produce a CQE that wakes io_uring_wait_cqe_timeout when
	 * SIGTERM doesn't interrupt the kernel-level wait (known issue
	 * with pending multishot accepts and in-flight I/O).
	 */
	shutdown_efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (shutdown_efd < 0) {
		LOG("eventfd: %s", strerror(errno));
		return -1;
	}
	rc->rc_shutdown_efd = shutdown_efd;

	last_accept_check = time(NULL);

	if (io_context_init())
		return -1;

	SSL_library_init();
	OpenSSL_add_all_algorithms();
	SSL_load_error_strings();

	io_conn_init();

	/* Eagerly initialize TLS context with configured paths. */
	if (io_tls_init_server_context(tls_cert, tls_key, tls_ca) != 0)
		TRACE("TLS context init deferred (certs not found at startup)");

	/* GSS context cache + server credential (keytab). */
	gss_ctx_cache_init();
	if (gss_server_cred_init() != 0)
		TRACE("GSS server credential not available (no keytab)");

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

	int slot = fd % MAX_CONNECTIONS;
	if (conn_buffers[slot] != NULL) {
		LOG("io: conn_buffers alias: fd=%d slot=%d already occupied -- "
		    "increase MAX_CONNECTIONS or use a hash map",
		    fd, slot);
		free(bs->bs_data);
		free(bs);
		return NULL;
	}
	conn_buffers[slot] = bs;
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

/*
 * Signal-safe shutdown wakeup.  Called from the SIGTERM signal handler.
 * write() to an eventfd is async-signal-safe.
 */
void io_handler_signal_shutdown(void)
{
	if (shutdown_efd >= 0) {
		uint64_t val = 1;
		(void)write(shutdown_efd, &val, sizeof(val));
	}
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

	/*
	 * Mark this thread as the io_uring event loop so add_task()
	 * will not block when the task queue is full.  Blocking here
	 * prevents CQE processing and can deadlock against workers
	 * waiting for BACKEND_PWRITE completions.
	 */
	io_mark_main_thread();

	// Initialize heartbeat system
	if (io_heartbeat_init(rc) < 0) {
		LOG("Failed to initialize heartbeat system");
		return;
	}

	/*
	 * Submit a poll on the shutdown eventfd.  When the signal handler
	 * writes to it, this produces a CQE that wakes the main loop
	 * immediately -- even when io_uring_wait_cqe_timeout wouldn't
	 * return on SIGTERM due to pending operations.
	 */
	if (shutdown_efd >= 0) {
		struct io_uring_sqe *sqe;

		pthread_mutex_lock(&rc->rc_mutex);
		sqe = io_uring_get_sqe(&rc->rc_ring);
		if (sqe) {
			io_uring_prep_poll_add(sqe, shutdown_efd, POLLIN);
			io_uring_sqe_set_data(sqe, NULL);
			io_uring_submit(&rc->rc_ring);
		}
		pthread_mutex_unlock(&rc->rc_mutex);
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

		/*
		 * Wait for completion events.
		 *
		 * Thread safety: on kernel 5.11+ (IORING_FEAT_EXT_ARG),
		 * io_uring_wait_cqe_timeout uses IORING_ENTER_EXT_ARG
		 * to pass the timeout as a syscall argument -- it does NOT
		 * touch the SQ ring.  Worker threads submit write SQEs
		 * under rc_mutex; since this call doesn't access the SQ
		 * ring, no mutex is needed here.
		 *
		 * Audited 2026-04-05: all io_uring_get_sqe + submit call
		 * sites hold rc_mutex.  This is the only wait_cqe_timeout
		 * in the hot path; shutdown drains (below) run after
		 * workers are stopped.
		 */
		int ret = io_uring_wait_cqe_timeout(&rc->rc_ring, &cqe, &ts);

		if (ret == -ETIME) {
			// Timeout - check running flag and continue
			continue;
		} else if (ret == -EINTR) {
			int rl;
			__atomic_load(running_flag, &rl, __ATOMIC_SEQ_CST);
			LOG("io_uring_wait_cqe_timeout interrupted, "
			    "running=%d",
			    rl);
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
			/*
			 * NULL user_data: the shutdown eventfd poll
			 * completed -- the signal handler wrote to it.
			 * Consume the CQE and let the running_flag check
			 * at the top of the loop handle the break.
			 */
			io_uring_cqe_seen(&rc->rc_ring, cqe);
			continue;
		}

		TSAN_ACQUIRE(ic);

#ifdef HAVE_IO_URING_STRESS
		// Fake a slow event loop to cause CQ ring backpressure
		if (ic->ic_op_type == OP_TYPE_WRITE) {
			usleep(5000); // 5ms delay per completion
		}
#endif

		if (ic->ic_state & IO_CONTEXT_SUBMITTED_EAGAIN) {
			TRACE("ZOMBIE COMPLETION: Received CQE for ic=%p id=%u that previously hit EAGAIN. res=%d",
			      (void *)ic, ic->ic_id, cqe->res);
			trace_io_eagain(ic, __func__, __LINE__);
		}

		// trace_io_context(ic, __func__, __LINE__);

		// Handle the completion based on the operation type
		if (cqe->res == -ECANCELED) {
			trace_io_context(ic, __func__, __LINE__);

			TRACE("Operation was cancelled: cqe=%p %p op=%s fd=%d id=%u",
			      (void *)cqe, (void *)ic,
			      io_op_type_to_str(ic->ic_op_type), ic->ic_fd,
			      ic->ic_id);
		} else if (cqe->res < 0) {
			if (ic->ic_op_type == OP_TYPE_HEARTBEAT &&
			    cqe->res == -ETIME) {
				TRACE("Heartbeat timer expired (expected)");
			} else {
				LOG("CQE error for op=%s, fd=%d: %s",
				    io_op_type_to_str(ic->ic_op_type),
				    ic->ic_fd, strerror(-cqe->res));
			}

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
				TRACE("STRAY COMPLETION: ic=%p id=%u is NOT ACTIVE but received CQE. res=%d",
				      (void *)ic, ic->ic_id, cqe->res);
				TRACE("Received completion for non-active context: ic=%p op=%s fd=%d id=%u state=0x%lx res=%d",
				      (void *)ic,
				      io_op_type_to_str(ic->ic_op_type),
				      ic->ic_fd, ic->ic_id, state, cqe->res);

				/*
				 * TLS data write contexts are created by
				 * io_do_tls() via io_request_write_op() with
				 * state=IO_CONTEXT_DIRECT_TLS_DATA, which
				 * overwrites the ACTIVE bit set by
				 * io_context_create().  The write count was
				 * already incremented at creation time, so we
				 * must call io_handle_write() here to drive
				 * io_context_destroy() and decrement the count.
				 * Skipping this causes ci_write_count to grow
				 * without bound, stalling all further I/O on
				 * the connection.
				 */
				if (ic->ic_op_type == OP_TYPE_WRITE &&
				    (state & IO_CONTEXT_DIRECT_TLS_DATA)) {
					io_handle_write(ic, cqe->res, rc);
				}

				/*
				 * Do NOT trace ic here -- io_handle_write
				 * may have destroyed it (ic_state had
				 * IO_CONTEXT_DIRECT_TLS_DATA).
				 */
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

	// Stop workers before cancelling so no new submissions race with the cancel
	wait_for_worker_threads();

	// Now safe to cancel without contention
	pthread_mutex_lock(&rc->rc_mutex);
	struct io_uring_sqe *sqe = io_uring_get_sqe(&rc->rc_ring);
	if (sqe) {
		io_uring_prep_cancel(sqe, NULL, IORING_ASYNC_CANCEL_ALL);
		io_uring_submit(&rc->rc_ring);
	}
	pthread_mutex_unlock(&rc->rc_mutex);

	// Drain pending io_uring operations
	while (1) {
		struct io_uring_cqe *cqe;
		struct __kernel_timespec ts = { .tv_sec = 1, .tv_nsec = 0 };

		int ret = io_uring_wait_cqe_timeout(&rc->rc_ring, &cqe, &ts);
		if (ret == -ETIME) {
			break;
		} else if (ret < 0) {
			LOG("Error draining io_uring: %s", strerror(-ret));
			break;
		}

		io_uring_cqe_seen(&rc->rc_ring, cqe);
	}

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

		io_uring_cqe_seen(&rc->rc_ring, cqe);
	}

	// Cleanup any pending requests
	TRACE("Cleaning up pending requests...");
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

	TRACE("Cleaning up remaining active contexts...");
	io_context_release_active();

	io_context_fini();

	gss_server_cred_fini();
	gss_ctx_cache_fini();

	io_uring_queue_exit(&rc->rc_ring);
	pthread_mutex_destroy(&rc->rc_mutex);

	if (shutdown_efd >= 0) {
		close(shutdown_efd);
		shutdown_efd = -1;
	}
}
