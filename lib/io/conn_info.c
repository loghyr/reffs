/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Connection-info bookkeeping: per-fd hash of pending I/O op counts,
 * state transitions, timeout sweeps, and the per-fd write
 * serialization gate.  Backend-agnostic -- no io_uring or kqueue
 * dependencies -- so both backends share a single implementation.
 *
 * Extracted from lib/io/connect.c (which retains the io_uring-specific
 * connect-operation logic).
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "reffs/log.h"
#include "reffs/io.h"
#include "reffs/network.h"
#include "trace_io.h"

#include "io_internal.h"

/* Array to track connection states, keyed by (fd % MAX_CONNECTIONS). */
static struct conn_info *connections[MAX_CONNECTIONS];
static pthread_mutex_t conn_mutex = PTHREAD_MUTEX_INITIALIZER;

int io_conn_init(void)
{
	pthread_mutex_lock(&conn_mutex);
	memset(connections, 0, sizeof(connections));
	pthread_mutex_unlock(&conn_mutex);
	return 0;
}

struct conn_info *io_conn_register(int fd, enum conn_state initial_state,
				   enum conn_role role)
{
	struct conn_info *ci = NULL;

	pthread_mutex_lock(&conn_mutex);

	int idx = fd % MAX_CONNECTIONS;
	if (connections[idx] && connections[idx]->ci_fd == fd) {
		ci = connections[idx];
	} else if (!connections[idx] ||
		   connections[idx]->ci_state == CONN_UNUSED) {
		uint32_t saved_gen =
			connections[idx] ? connections[idx]->ci_generation : 0;
		if (!connections[idx]) {
			connections[idx] = malloc(sizeof(struct conn_info));
			if (!connections[idx]) {
				pthread_mutex_unlock(&conn_mutex);
				return NULL;
			}
		}

		ci = connections[idx];
		memset(ci, 0, sizeof(struct conn_info));
		ci->ci_generation = saved_gen + 1;
		ci->ci_fd = fd;
	} else {
		LOG("Connection slot collision for fd=%d", fd);
	}

	if (ci) {
		ci->ci_state = initial_state;
		ci->ci_role = role;
		ci->ci_last_activity = time(NULL);

		ci->ci_read_count = 0;
		ci->ci_write_count = 0;
		ci->ci_accept_count = 0;
		ci->ci_connect_count = 0;

		ci->ci_write_active = false;
		ci->ci_write_pending_head = NULL;
		ci->ci_write_pending_tail = NULL;
	}

	pthread_mutex_unlock(&conn_mutex);
	return ci;
}

struct conn_info *io_conn_get(int fd)
{
	struct conn_info *ci = NULL;
	pthread_mutex_lock(&conn_mutex);

	int idx = fd % MAX_CONNECTIONS;
	if (connections[idx] && connections[idx]->ci_fd == fd) {
		ci = connections[idx];
	}

	pthread_mutex_unlock(&conn_mutex);
	return ci;
}

void io_conn_update_state(int fd)
{
	pthread_mutex_lock(&conn_mutex);

	int idx = fd % MAX_CONNECTIONS;
	if (connections[idx] && connections[idx]->ci_fd == fd) {
		enum conn_state old_state = connections[idx]->ci_state;
		enum conn_state new_state;

		if (connections[idx]->ci_read_count > 0) {
			if (connections[idx]->ci_write_count > 0) {
				new_state = CONN_READWRITE;
			} else {
				new_state = CONN_READING;
			}
		} else if (connections[idx]->ci_write_count > 0) {
			new_state = CONN_WRITING;
		} else if (connections[idx]->ci_accept_count > 0) {
			new_state = CONN_ACCEPTING;
		} else if (connections[idx]->ci_connect_count > 0) {
			new_state = CONN_CONNECTING;
		} else if (old_state == CONN_ERROR ||
			   old_state == CONN_DISCONNECTING ||
			   old_state == CONN_UNUSED) {
			new_state = old_state;
		} else {
			new_state = CONN_CONNECTED;
		}

		if (old_state != new_state) {
			trace_io_connection_state_change(
				fd, old_state, new_state, __func__, __LINE__);
			connections[idx]->ci_state = new_state;
		}
	}

	pthread_mutex_unlock(&conn_mutex);
}

int io_conn_add_read_op(int fd)
{
	int ret = -1;
	pthread_mutex_lock(&conn_mutex);

	int idx = fd % MAX_CONNECTIONS;
	if (connections[idx] && connections[idx]->ci_fd == fd) {
		connections[idx]->ci_read_count++;
		connections[idx]->ci_last_activity = time(NULL);

		enum conn_state old_state = connections[idx]->ci_state;
		enum conn_state new_state;

		if (connections[idx]->ci_write_count > 0) {
			new_state = CONN_READWRITE;
		} else {
			new_state = CONN_READING;
		}

		if (old_state != new_state) {
			trace_io_connection_state_change(
				fd, old_state, new_state, __func__, __LINE__);
			connections[idx]->ci_state = new_state;
		}

		ret = 0;
	}

	pthread_mutex_unlock(&conn_mutex);
	return ret;
}

int io_conn_remove_read_op(int fd)
{
	int ret = -1;
	pthread_mutex_lock(&conn_mutex);

	int idx = fd % MAX_CONNECTIONS;
	if (connections[idx] && connections[idx]->ci_fd == fd) {
		if (connections[idx]->ci_read_count > 0) {
			connections[idx]->ci_read_count--;
			trace_io_connection_count(
				fd, connections[idx]->ci_read_count, __func__,
				__LINE__);

			enum conn_state old_state = connections[idx]->ci_state;
			enum conn_state new_state;

			if (connections[idx]->ci_read_count > 0) {
				if (connections[idx]->ci_write_count > 0) {
					new_state = CONN_READWRITE;
				} else {
					new_state = CONN_READING;
				}
			} else if (connections[idx]->ci_write_count > 0) {
				new_state = CONN_WRITING;
			} else {
				new_state = CONN_CONNECTED;
			}

			if (old_state != new_state) {
				trace_io_connection_state_change(fd, old_state,
								 new_state,
								 __func__,
								 __LINE__);
				connections[idx]->ci_state = new_state;
			}
		} else {
			LOG("Warning: Attempt to remove read op when count is zero for fd=%d",
			    fd);
		}
		connections[idx]->ci_last_activity = time(NULL);
		ret = 0;
	}

	pthread_mutex_unlock(&conn_mutex);
	return ret;
}

int io_conn_add_write_op(int fd)
{
	int ret = -1;
	pthread_mutex_lock(&conn_mutex);

	int idx = fd % MAX_CONNECTIONS;
	if (connections[idx] && connections[idx]->ci_fd == fd) {
		connections[idx]->ci_write_count++;
		connections[idx]->ci_last_activity = time(NULL);
		trace_io_connection_count(fd, connections[idx]->ci_write_count,
					  __func__, __LINE__);

		enum conn_state old_state = connections[idx]->ci_state;
		enum conn_state new_state;

		if (connections[idx]->ci_read_count > 0) {
			new_state = CONN_READWRITE;
		} else {
			new_state = CONN_WRITING;
		}

		if (old_state != new_state) {
			trace_io_connection_state_change(
				fd, old_state, new_state, __func__, __LINE__);
			connections[idx]->ci_state = new_state;
		}

		ret = 0;
	}

	pthread_mutex_unlock(&conn_mutex);
	return ret;
}

int io_conn_remove_write_op(int fd)
{
	int ret = -1;
	pthread_mutex_lock(&conn_mutex);

	int idx = fd % MAX_CONNECTIONS;
	if (connections[idx] && connections[idx]->ci_fd == fd) {
		if (connections[idx]->ci_write_count > 0) {
			connections[idx]->ci_write_count--;

			trace_io_connection_count(
				fd, connections[idx]->ci_write_count, __func__,
				__LINE__);

			enum conn_state old_state = connections[idx]->ci_state;
			enum conn_state new_state;

			if (connections[idx]->ci_write_count > 0) {
				if (connections[idx]->ci_read_count > 0) {
					new_state = CONN_READWRITE;
				} else {
					new_state = CONN_WRITING;
				}
			} else if (connections[idx]->ci_read_count > 0) {
				new_state = CONN_READING;
			} else {
				new_state = CONN_CONNECTED;
			}

			if (old_state != new_state) {
				trace_io_connection_state_change(fd, old_state,
								 new_state,
								 __func__,
								 __LINE__);
				connections[idx]->ci_state = new_state;
			}
		} else {
			LOG("Warning: Attempt to remove write op when count is zero for fd=%d",
			    fd);
		}
		connections[idx]->ci_last_activity = time(NULL);
		ret = 0;
	}

	pthread_mutex_unlock(&conn_mutex);
	return ret;
}

int io_conn_add_accept_op(int fd)
{
	int ret = -1;
	pthread_mutex_lock(&conn_mutex);

	int idx = fd % MAX_CONNECTIONS;
	if (connections[idx] && connections[idx]->ci_fd == fd) {
		connections[idx]->ci_accept_count++;
		connections[idx]->ci_last_activity = time(NULL);
		trace_io_connection_count(fd, connections[idx]->ci_accept_count,
					  __func__, __LINE__);

		if (connections[idx]->ci_state != CONN_ACCEPTING) {
			trace_io_connection_state_change(
				fd, connections[idx]->ci_state, CONN_ACCEPTING,
				__func__, __LINE__);
			connections[idx]->ci_state = CONN_ACCEPTING;
		}

		ret = 0;
	}

	pthread_mutex_unlock(&conn_mutex);
	return ret;
}

int io_conn_remove_accept_op(int fd)
{
	int ret = -1;
	pthread_mutex_lock(&conn_mutex);

	int idx = fd % MAX_CONNECTIONS;
	if (connections[idx] && connections[idx]->ci_fd == fd) {
		if (connections[idx]->ci_accept_count > 0) {
			connections[idx]->ci_accept_count--;
			trace_io_connection_count(
				fd, connections[idx]->ci_accept_count, __func__,
				__LINE__);

			if (connections[idx]->ci_accept_count == 0 &&
			    connections[idx]->ci_state == CONN_ACCEPTING) {
				trace_io_connection_state_change(
					fd, CONN_ACCEPTING, CONN_LISTENING,
					__func__, __LINE__);
				connections[idx]->ci_state = CONN_LISTENING;
			}
		} else {
			LOG("Warning: Attempt to remove accept op when count is zero for fd=%d",
			    fd);
		}
		connections[idx]->ci_last_activity = time(NULL);
		ret = 0;
	}

	pthread_mutex_unlock(&conn_mutex);
	return ret;
}

int io_conn_add_connect_op(int fd)
{
	int ret = -1;
	pthread_mutex_lock(&conn_mutex);

	int idx = fd % MAX_CONNECTIONS;
	if (connections[idx] && connections[idx]->ci_fd == fd) {
		connections[idx]->ci_connect_count++;
		connections[idx]->ci_last_activity = time(NULL);
		trace_io_connection_count(fd,
					  connections[idx]->ci_connect_count,
					  __func__, __LINE__);

		if (connections[idx]->ci_state != CONN_CONNECTING) {
			trace_io_connection_state_change(
				fd, connections[idx]->ci_state, CONN_CONNECTING,
				__func__, __LINE__);
			connections[idx]->ci_state = CONN_CONNECTING;
		}

		ret = 0;
	}

	pthread_mutex_unlock(&conn_mutex);
	return ret;
}

int io_conn_remove_connect_op(int fd)
{
	int ret = -1;

	pthread_mutex_lock(&conn_mutex);

	int idx = fd % MAX_CONNECTIONS;
	if (connections[idx] && connections[idx]->ci_fd == fd) {
		if (connections[idx]->ci_connect_count > 0) {
			connections[idx]->ci_connect_count--;
			trace_io_connection_count(
				fd, connections[idx]->ci_connect_count,
				__func__, __LINE__);

			if (connections[idx]->ci_connect_count == 0 &&
			    connections[idx]->ci_state == CONN_CONNECTING) {
				trace_io_connection_state_change(
					fd, CONN_CONNECTING, CONN_CONNECTED,
					__func__, __LINE__);
				connections[idx]->ci_state = CONN_CONNECTED;
			}
		} else {
			LOG("Warning: Attempt to remove connect op when count is zero for fd=%d",
			    fd);
		}
		connections[idx]->ci_last_activity = time(NULL);
		ret = 0;
	}

	pthread_mutex_unlock(&conn_mutex);
	return ret;
}

int io_conn_set_error(int fd, int error_code)
{
	int ret = -1;
	pthread_mutex_lock(&conn_mutex);

	int idx = fd % MAX_CONNECTIONS;
	if (connections[idx] && connections[idx]->ci_fd == fd) {
		trace_io_connection_state_change(fd, connections[idx]->ci_state,
						 CONN_ERROR, __func__,
						 __LINE__);

		connections[idx]->ci_state = CONN_ERROR;
		connections[idx]->ci_error = error_code;
		connections[idx]->ci_last_activity = time(NULL);
		ret = 0;
	}

	pthread_mutex_unlock(&conn_mutex);
	return ret;
}

bool io_conn_is_state(int fd, enum conn_state state)
{
	bool result = false;
	pthread_mutex_lock(&conn_mutex);

	int idx = fd % MAX_CONNECTIONS;
	if (connections[idx] && connections[idx]->ci_fd == fd) {
		result = (connections[idx]->ci_state == state);
	}

	pthread_mutex_unlock(&conn_mutex);
	return result;
}

void io_conn_destroy(struct conn_info *ci)
{
	if (!ci)
		return;

	if (ci->ci_ssl) {
		SSL_shutdown(ci->ci_ssl);
		SSL_free(ci->ci_ssl);
		ci->ci_ssl = NULL;
	}
	ci->ci_tls_enabled = false;
	ci->ci_tls_handshaking = false;

	free(ci);
}

int io_conn_unregister(int fd)
{
	int ret = -1;
	struct io_context *drain_head = NULL;

	pthread_mutex_lock(&conn_mutex);

	int idx = fd % MAX_CONNECTIONS;
	if (connections[idx] && connections[idx]->ci_fd == fd) {
		TRACE("Unregistering connection fd=%d (state=%s, role=%s)", fd,
		      io_conn_state_to_str(connections[idx]->ci_state),
		      io_conn_role_to_str(connections[idx]->ci_role));

		if (connections[idx]->ci_ssl) {
			SSL_shutdown(connections[idx]->ci_ssl);
			SSL_free(connections[idx]->ci_ssl);
			connections[idx]->ci_ssl = NULL;
			connections[idx]->ci_tls_enabled = false;
			connections[idx]->ci_tls_handshaking = false;
		}

		/*
		 * Drain the pending write queue before clearing ci_fd.
		 * We must extract the list under conn_mutex but call
		 * io_context_destroy() outside it, because destroy takes
		 * conn_mutex internally (sequential, not nested -- safe).
		 */
		drain_head = connections[idx]->ci_write_pending_head;
		connections[idx]->ci_write_pending_head = NULL;
		connections[idx]->ci_write_pending_tail = NULL;
		connections[idx]->ci_write_active = false;

		connections[idx]->ci_state = CONN_UNUSED;
		connections[idx]->ci_fd = -1;
		connections[idx]->ci_read_count = 0;
		connections[idx]->ci_write_count = 0;
		connections[idx]->ci_accept_count = 0;
		connections[idx]->ci_connect_count = 0;
		ret = 0;
	} else {
		LOG("Failed to unregister connection fd=%d - not found or mismatch",
		    fd);
	}

	pthread_mutex_unlock(&conn_mutex);

	while (drain_head) {
		struct io_context *next = drain_head->ic_write_next;
		drain_head->ic_write_next = NULL;
		io_context_destroy(drain_head);
		drain_head = next;
	}

	return ret;
}

bool io_conn_has_read_ops(int fd)
{
	bool has_ops = false;
	pthread_mutex_lock(&conn_mutex);

	int idx = fd % MAX_CONNECTIONS;
	if (connections[idx] && connections[idx]->ci_fd == fd) {
		has_ops = (connections[idx]->ci_read_count > 0);
	}

	pthread_mutex_unlock(&conn_mutex);
	return has_ops;
}

bool io_conn_has_write_ops(int fd)
{
	bool has_ops = false;
	pthread_mutex_lock(&conn_mutex);

	int idx = fd % MAX_CONNECTIONS;
	if (connections[idx] && connections[idx]->ci_fd == fd) {
		has_ops = (connections[idx]->ci_write_count > 0);
	}

	pthread_mutex_unlock(&conn_mutex);
	return has_ops;
}

void io_conn_cleanup(void)
{
	pthread_mutex_lock(&conn_mutex);

	for (int i = 0; i < MAX_CONNECTIONS; i++) {
		if (connections[i]) {
			if (connections[i]->ci_ssl) {
				SSL_shutdown(connections[i]->ci_ssl);
				SSL_free(connections[i]->ci_ssl);
				connections[i]->ci_ssl = NULL;
			}

			free(connections[i]);
			connections[i] = NULL;
		}
	}

	pthread_mutex_unlock(&conn_mutex);
}

int io_socket_close(int fd, int error)
{
	if (fd <= 0) {
		return -EBADF;
	}

	struct conn_info *conn = io_conn_get(fd);
	if (conn) {
		io_conn_set_error(fd, error);
		io_conn_unregister(fd);
	}

	TRACE("Closing %d", fd);

	io_client_fd_unregister(fd);
	return close(fd);
}

const char *io_conn_state_to_str(enum conn_state state)
{
	switch (state) {
	case CONN_UNUSED:
		return "UNUSED";
	case CONN_LISTENING:
		return "LISTENING";
	case CONN_ACCEPTING:
		return "ACCEPTING";
	case CONN_ACCEPTED:
		return "ACCEPTED";
	case CONN_CONNECTING:
		return "CONNECTING";
	case CONN_CONNECTED:
		return "CONNECTED";
	case CONN_READING:
		return "READING";
	case CONN_WRITING:
		return "WRITING";
	case CONN_READWRITE:
		return "READWRITE";
	case CONN_DISCONNECTING:
		return "DISCONNECTING";
	case CONN_ERROR:
		return "ERROR";
	default:
		return "UNKNOWN";
	}
}

const char *io_conn_role_to_str(enum conn_role role)
{
	switch (role) {
	case CONN_ROLE_UNKNOWN:
		return "UNKNOWN";
	case CONN_ROLE_CLIENT:
		return "CLIENT";
	case CONN_ROLE_SERVER:
		return "SERVER";
	case CONN_ROLE_ACCEPTED:
		return "ACCEPTED";
	default:
		return "INVALID";
	}
}

void io_conn_dump(int fd)
{
	struct conn_info *ci = io_conn_get(fd);
	if (!ci) {
		LOG("No connection info for fd=%d", fd);
		return;
	}

	char peer_addr[INET6_ADDRSTRLEN] = { 0 };
	char local_addr[INET6_ADDRSTRLEN] = { 0 };
	uint16_t peer_port = 0, local_port = 0;
	time_t now = time(NULL);

	if (ci->ci_peer_len > 0) {
		addr_to_string((const struct sockaddr_storage *)&ci->ci_peer,
			       peer_addr, INET6_ADDRSTRLEN, &peer_port);
	}

	if (ci->ci_local_len > 0) {
		addr_to_string((const struct sockaddr_storage *)&ci->ci_local,
			       local_addr, INET6_ADDRSTRLEN, &local_port);
	}

	trace_io_active_connections(ci, peer_addr, peer_port, local_addr,
				    local_port, now, __func__, __LINE__);
}

void io_conn_dump_all(void)
{
	pthread_mutex_lock(&conn_mutex);

	TRACE("=== Active Connections ===");
	int active_count = 0;

	time_t now = time(NULL);

	for (int i = 0; i < MAX_CONNECTIONS; i++) {
		if (connections[i] && connections[i]->ci_state != CONN_UNUSED) {
			active_count++;
			struct conn_info *ci = connections[i];

			char peer_addr[INET6_ADDRSTRLEN] = { 0 };
			char local_addr[INET6_ADDRSTRLEN] = { 0 };
			uint16_t peer_port = 0, local_port = 0;

			if (ci->ci_peer_len > 0) {
				addr_to_string((const struct sockaddr_storage
							*)&ci->ci_peer,
					       peer_addr, INET6_ADDRSTRLEN,
					       &peer_port);
			}

			if (ci->ci_local_len > 0) {
				addr_to_string((const struct sockaddr_storage
							*)&ci->ci_local,
					       local_addr, INET6_ADDRSTRLEN,
					       &local_port);
			}

			trace_io_active_connections(ci, peer_addr, peer_port,
						    local_addr, local_port, now,
						    __func__, __LINE__);
		}
	}

	TRACE("Total active connections: %d", active_count);
	TRACE("==========================");

	pthread_mutex_unlock(&conn_mutex);
}

int io_conn_check_timeouts(time_t timeout_seconds)
{
	time_t now = time(NULL);
	/*
	 * Collect timed-out fds under conn_mutex, then release and close
	 * each via io_socket_close() outside the mutex.  Two reasons:
	 *
	 *  1. close(2) under conn_mutex can block indefinitely under
	 *     memory pressure or in the middle of kernel socket teardown;
	 *     every other conn_info lookup in the process stalls while we
	 *     wait.  Release the mutex first.
	 *
	 *  2. The original code used raw close(fd) + manual ci_state flip,
	 *     which left conn_buffers[fd % MAX_CONNECTIONS] populated --
	 *     a buffer_state leak.  io_socket_close() calls
	 *     io_client_fd_unregister() which frees the buffer_state
	 *     properly.
	 *
	 * Race note: between the scan and the close, another thread
	 * could accept a new connection whose fd hashes to the same
	 * conn_info slot.  For timed-out-idle connections this is
	 * vanishingly unlikely (no completions fire on a truly idle fd),
	 * and the worst case is closing a newly-accepted client which
	 * will reconnect.  If this race ever materializes under real
	 * load, the fix is a per-slot CLOSING state to block reuse; for
	 * now the simplicity is worth it.
	 *
	 * Stack array sized at MAX_CONNECTIONS is fine: ~4 KB on the
	 * heartbeat thread, which does not recurse.
	 */
	int to_close[MAX_CONNECTIONS];
	int n_to_close = 0;

	pthread_mutex_lock(&conn_mutex);

	for (int i = 0; i < MAX_CONNECTIONS; i++) {
		if (connections[i] && connections[i]->ci_state != CONN_UNUSED &&
		    connections[i]->ci_fd >= 0) {
			if (now - connections[i]->ci_last_activity >
			    timeout_seconds) {
				LOG("Connection fd=%d timed out (%ld seconds inactive)",
				    connections[i]->ci_fd,
				    now - connections[i]->ci_last_activity);
				to_close[n_to_close++] = connections[i]->ci_fd;
			}
		}
	}

	pthread_mutex_unlock(&conn_mutex);

	for (int i = 0; i < n_to_close; i++)
		io_socket_close(to_close[i], ETIMEDOUT);

	return n_to_close;
}

/*
 * io_conn_write_try_start -- claim the per-fd write serialization gate.
 *
 * If the gate is free (ci_write_active == false), set it true and return
 * true so the caller can submit a write SQE immediately.
 *
 * If the gate is held, append ic to ci_write_pending and return false.
 * The caller must return without touching ic further; io_conn_write_done()
 * will hand off the gate and call rpc_trans_writer() for the next ic.
 *
 * If fd is no longer registered we return true anyway -- the caller will
 * fail naturally on SQE submission.
 */
bool io_conn_write_try_start(int fd, struct io_context *ic)
{
	bool can_start = true;

	pthread_mutex_lock(&conn_mutex);

	int idx = fd % MAX_CONNECTIONS;
	if (connections[idx] && connections[idx]->ci_fd == fd) {
		struct conn_info *ci = connections[idx];
		if (!ci->ci_write_active) {
			ci->ci_write_active = true;
			ic->ic_write_gen = ci->ci_generation;
		} else {
			ic->ic_write_next = NULL;
			if (!ci->ci_write_pending_head) {
				ci->ci_write_pending_head = ic;
				ci->ci_write_pending_tail = ic;
			} else {
				ci->ci_write_pending_tail->ic_write_next = ic;
				ci->ci_write_pending_tail = ic;
			}
			can_start = false;
		}
	}

	pthread_mutex_unlock(&conn_mutex);
	return can_start;
}

/*
 * io_conn_write_done -- release the per-fd write gate after a write completes.
 *
 * gen must match ci->ci_generation.  A mismatch means a stale write CQE
 * arrived for a connection that has already been closed and whose fd was
 * reused by a new connection.  In that case the gate belongs to the new
 * connection and must not be touched; return NULL silently.
 *
 * If there are queued writers, dequeue the head, mark it WRITE_OWNED,
 * stamp its ic_write_gen, and return it.  ci_write_active remains true.
 *
 * If the queue is empty, clear ci_write_active and return NULL.
 */
struct io_context *io_conn_write_done(int fd, uint32_t gen)
{
	struct io_context *next_ic = NULL;

	pthread_mutex_lock(&conn_mutex);

	int idx = fd % MAX_CONNECTIONS;
	if (connections[idx] && connections[idx]->ci_fd == fd &&
	    connections[idx]->ci_generation == gen) {
		struct conn_info *ci = connections[idx];
		if (ci->ci_write_pending_head) {
			next_ic = ci->ci_write_pending_head;
			ci->ci_write_pending_head = next_ic->ic_write_next;
			next_ic->ic_write_next = NULL;
			if (!ci->ci_write_pending_head)
				ci->ci_write_pending_tail = NULL;
			next_ic->ic_state |= IO_CONTEXT_WRITE_OWNED;
			next_ic->ic_write_gen = ci->ci_generation;
		} else {
			ci->ci_write_active = false;
		}
	}

	pthread_mutex_unlock(&conn_mutex);
	return next_ic;
}

bool io_conn_is_tls_enabled(int fd)
{
	bool enabled = false;

	pthread_mutex_lock(&conn_mutex);
	int idx = fd % MAX_CONNECTIONS;
	if (connections[idx] && connections[idx]->ci_fd == fd)
		enabled = connections[idx]->ci_tls_enabled;
	pthread_mutex_unlock(&conn_mutex);

	return enabled;
}

void io_conn_set_tls_handshaking(int fd, bool handshaking)
{
	pthread_mutex_lock(&conn_mutex);
	int idx = fd % MAX_CONNECTIONS;
	if (connections[idx] && connections[idx]->ci_fd == fd)
		connections[idx]->ci_tls_handshaking = handshaking;
	pthread_mutex_unlock(&conn_mutex);
}
