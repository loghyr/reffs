/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
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
#include "reffs/log.h"
#include "reffs/network.h"
#include "reffs/ring.h"
#include "reffs/rpc.h"
#include "reffs/trace/io.h"

// Array to track connection states
static struct conn_info *connections[MAX_CONNECTIONS];
static pthread_mutex_t conn_mutex = PTHREAD_MUTEX_INITIALIZER;

// Initialize connection tracking
int io_conn_init(void)
{
	pthread_mutex_lock(&conn_mutex);
	memset(connections, 0, sizeof(connections));
	pthread_mutex_unlock(&conn_mutex);
	return 0;
}

// Register a new connection
struct conn_info *io_conn_register(int fd, enum conn_state initial_state,
				   enum conn_role role)
{
	struct conn_info *ci = NULL;

	pthread_mutex_lock(&conn_mutex);

	// Find unused slot or reuse existing slot for this fd
	int idx = fd % MAX_CONNECTIONS;
	if (connections[idx] && connections[idx]->ci_fd == fd) {
		ci = connections[idx];
	} else if (!connections[idx] ||
		   connections[idx]->ci_state == CONN_UNUSED) {
		// Allocate new conn_info if needed
		if (!connections[idx]) {
			connections[idx] = malloc(sizeof(struct conn_info));
			if (!connections[idx]) {
				pthread_mutex_unlock(&conn_mutex);
				return NULL;
			}
		}

		ci = connections[idx];
		memset(ci, 0, sizeof(struct conn_info));
		ci->ci_fd = fd;
	} else {
		// Collision with another active connection
		LOG("Connection slot collision for fd=%d", fd);
	}

	if (ci) {
		ci->ci_state = initial_state;
		ci->ci_role = role;
		ci->ci_last_activity = time(NULL);

		// Initialize operation counters
		ci->ci_read_count = 0;
		ci->ci_write_count = 0;
		ci->ci_accept_count = 0;
		ci->ci_connect_count = 0;
	}

	pthread_mutex_unlock(&conn_mutex);
	return ci;
}

// Get connection info
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

// Update the connection state based on operation counts
void io_conn_update_state(int fd)
{
	pthread_mutex_lock(&conn_mutex);

	int idx = fd % MAX_CONNECTIONS;
	if (connections[idx] && connections[idx]->ci_fd == fd) {
		enum conn_state old_state = connections[idx]->ci_state;
		enum conn_state new_state;

		// Determine the new state based on operation counts
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
			// Don't change these special states if no ops are pending
			new_state = old_state;
		} else {
			new_state = CONN_CONNECTED;
		}

		// Only log if state actually changes
		if (old_state != new_state) {
			trace_io_connection_state_change(
				fd, old_state, new_state, __func__, __LINE__);
			connections[idx]->ci_state = new_state;
		}
	}

	pthread_mutex_unlock(&conn_mutex);
}

// Function to add a read operation
int io_conn_add_read_op(int fd)
{
	int ret = -1;
	pthread_mutex_lock(&conn_mutex);

	int idx = fd % MAX_CONNECTIONS;
	if (connections[idx] && connections[idx]->ci_fd == fd) {
		connections[idx]->ci_read_count++;
		connections[idx]->ci_last_activity = time(NULL);

		// Update the connection state based on the new count
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

// Function to remove a read operation
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

			// Update state based on remaining operations
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

// Function to add a write operation
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

		// Update the connection state based on the new count
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

// Function to remove a write operation
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

			// Update state based on remaining operations
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

// Function to add an accept operation
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

		// Update state to ACCEPTING
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

// Function to remove an accept operation
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

			// Update state based on remaining operations
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

// Function to add a connect operation
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

		// Update state to CONNECTING
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

// Function to remove a connect operation
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

			// Update state based on remaining operations
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

// Set connection to error state
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

// Check if a connection is in a particular state
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

	// Clean up SSL if present
	if (ci->ci_ssl) {
		SSL_shutdown(ci->ci_ssl);
		SSL_free(ci->ci_ssl);
		ci->ci_ssl = NULL;
	}
	ci->ci_tls_enabled = false;
	ci->ci_tls_handshaking = false;

	// Free the connection structure itself
	free(ci);
}

// Unregister a connection
int io_conn_unregister(int fd)
{
	int ret = -1;
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

		// Mark as unused, but keep the structure for reuse
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
	return ret;
}

// Function to check if there are any active read operations
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

// Function to check if there are any active write operations
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

// Clean up connection tracking
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

// Utility function to get connection state as string
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

// Helper function to convert role to string
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

// Dump information about a specific connection
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
		// Cast to sockaddr_storage* instead of sockaddr*
		addr_to_string((const struct sockaddr_storage *)&ci->ci_peer,
			       peer_addr, INET6_ADDRSTRLEN, &peer_port);
	}

	if (ci->ci_local_len > 0) {
		// Cast to sockaddr_storage* instead of sockaddr*
		addr_to_string((const struct sockaddr_storage *)&ci->ci_local,
			       local_addr, INET6_ADDRSTRLEN, &local_port);
	}

	trace_io_active_connections(ci, peer_addr, peer_port, local_addr,
				    local_port, now, __func__, __LINE__);
}

// Dump all active connections
void io_conn_dump_all(void)
{
	pthread_mutex_lock(&conn_mutex);

	LOG("=== Active Connections ===");
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

	LOG("Total active connections: %d", active_count);
	LOG("==========================");

	pthread_mutex_unlock(&conn_mutex);
}

// Periodically check for timed-out connections
int io_conn_check_timeouts(time_t timeout_seconds)
{
	time_t now = time(NULL);
	int closed = 0;

	pthread_mutex_lock(&conn_mutex);

	for (int i = 0; i < MAX_CONNECTIONS; i++) {
		if (connections[i] && connections[i]->ci_state != CONN_UNUSED &&
		    connections[i]->ci_fd >= 0) {
			// Check if connection has timed out
			if (now - connections[i]->ci_last_activity >
			    timeout_seconds) {
				LOG("Connection fd=%d timed out (%ld seconds inactive)",
				    connections[i]->ci_fd,
				    now - connections[i]->ci_last_activity);

				// Close the socket
				close(connections[i]->ci_fd);

				// Mark as unused
				connections[i]->ci_state = CONN_UNUSED;
				connections[i]->ci_fd = -1;
				connections[i]->ci_read_count = 0;
				connections[i]->ci_write_count = 0;
				connections[i]->ci_accept_count = 0;
				connections[i]->ci_connect_count = 0;

				closed++;
			}
		}
	}

	pthread_mutex_unlock(&conn_mutex);
	return closed;
}

int io_send_request(struct rpc_trans *rt)
{
	int ret;

	TRACE("fd=%d xid=0x%08x", rt->rt_fd, rt->rt_info.ri_xid);

	// Register the request for tracking
	ret = io_register_request(rt);
	if (ret)
		return ret;

	// Check if we already have a connection
	if (rt->rt_fd <= 0) {
		// Need to establish a connection first
		int sockfd;

		struct sockaddr_in *addr = malloc(sizeof(*addr));
		if (!addr) {
			return ENOMEM;
		}

		// Create socket
		sockfd = socket(AF_INET, SOCK_STREAM, 0);
		if (sockfd < 0) {
			LOG("Failed to create socket: %s", strerror(errno));
			free(addr);
			return errno;
		}

		// Register connection with CONNECTING state
		struct conn_info *ci = io_conn_register(sockfd, CONN_CONNECTING,
							CONN_ROLE_CLIENT);
		if (!ci) {
			LOG("Failed to register connection");
			io_socket_close(sockfd, ENOMEM);
			free(addr);
			return ENOMEM;
		}

		// Set non-blocking
		int flags = fcntl(sockfd, F_GETFL, 0);
		fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

		// Setup connection info
		memset(addr, 0, sizeof(*addr));
		addr->sin_family = AF_INET;
		addr->sin_port = htons(rt->rt_port);

		// Convert IP address from string to binary form
		if (inet_pton(AF_INET, rt->rt_addr_str, &addr->sin_addr) <= 0) {
			LOG("Invalid address: %s", rt->rt_addr_str);
			io_socket_close(sockfd, EINVAL);
			free(addr);
			return EINVAL;
		}

		// Store the socket fd in the rpc_trans structure
		rt->rt_fd = sockfd;

		// Create io_context for connect operation
		struct io_context *ic = io_context_create(
			OP_TYPE_CONNECT, sockfd, addr, sizeof(*addr));
		if (!ic) {
			free(addr);
			io_socket_close(sockfd, ENOMEM);
			return ENOMEM;
		}

		// Store XID for later matching
		ic->ic_xid = rt->rt_info.ri_xid;

		// Store XID in connection info
		ci = io_conn_get(sockfd);
		if (ci) {
			ci->ci_xid = rt->rt_info.ri_xid;
		}

		// Submit connect operation to io_uring
		pthread_mutex_lock(&rt->rt_rc->rc_mutex);
		struct io_uring_sqe *sqe =
			io_uring_get_sqe(&rt->rt_rc->rc_ring);
		if (!sqe) {
			pthread_mutex_unlock(&rt->rt_rc->rc_mutex);
			io_socket_close(sockfd, ENOBUFS);
			io_context_destroy(ic);
			return ENOBUFS;
		}

		io_uring_prep_connect(sqe, sockfd, (struct sockaddr *)addr,
				      sizeof(*addr));
		io_uring_sqe_set_data(sqe, ic);
		trace_io_connect_submit(ic);

		addr = NULL;

		// Submit and wait for connect completion
		io_uring_submit(&rt->rt_rc->rc_ring);
		pthread_mutex_unlock(&rt->rt_rc->rc_mutex);

		return 0; // Connection initiated, will be handled by callback
	}

	// If we already have a connection, check if it's in the CONNECTED state
	struct conn_info *ci = io_conn_get(rt->rt_fd);
	if (!ci ||
	    (ci->ci_state != CONN_CONNECTED && ci->ci_state != CONN_READING &&
	     ci->ci_state != CONN_WRITING && ci->ci_state != CONN_READWRITE)) {
		LOG("Connection is not ready for fd=%d", rt->rt_fd);
		return ENOTCONN;
	}

	// If we already have a connection or after establishing one synchronously
	return io_rpc_trans_cb(rt);
}

int io_handle_connect(struct io_context *ic, int result,
		      struct ring_context __attribute__((unused)) * rc)
{
	char addr_str[INET6_ADDRSTRLEN];
	uint16_t port;

	if (result < 0) {
		LOG("Connect failed for fd=%d: %s", ic->ic_fd,
		    strerror(-result));

		io_socket_close(ic->ic_fd, -result);
		io_context_destroy(ic);
		return -result;
	}

	// Try to get peer information
	ic->ic_ci.ci_peer_len = sizeof(ic->ic_ci.ci_peer);
	if (getpeername(ic->ic_fd, (struct sockaddr *)&ic->ic_ci.ci_peer,
			&ic->ic_ci.ci_peer_len) != 0) {
		LOG("Failed to get peer information: %s", strerror(errno));

		// This is a critical error - the socket is not properly connected
		io_conn_set_error(ic->ic_fd, errno);

		memset(&ic->ic_ci.ci_peer, 0, sizeof(ic->ic_ci.ci_peer));
		ic->ic_ci.ci_peer_len = 0;

		io_socket_close(ic->ic_fd, errno);
		io_context_destroy(ic);
		return errno;
	}

	// Get local socket information
	ic->ic_ci.ci_local_len = sizeof(ic->ic_ci.ci_local);
	if (getsockname(ic->ic_fd, (struct sockaddr *)&ic->ic_ci.ci_local,
			&ic->ic_ci.ci_local_len) == 0) {
		addr_to_string(&ic->ic_ci.ci_local, addr_str, INET6_ADDRSTRLEN,
			       &port);
	} else {
		LOG("Failed to get local information: %s", strerror(errno));
		memset(&ic->ic_ci.ci_local, 0, sizeof(ic->ic_ci.ci_local));
		ic->ic_ci.ci_local_len = 0;
	}

	TRACE("Connection established for fd=%d", ic->ic_fd);

	// Update connection info with peer and local addresses
	struct conn_info *ci = io_conn_get(ic->ic_fd);
	if (ci) {
		memcpy(&ci->ci_peer, &ic->ic_ci.ci_peer, ic->ic_ci.ci_peer_len);
		ci->ci_peer_len = ic->ic_ci.ci_peer_len;

		memcpy(&ci->ci_local, &ic->ic_ci.ci_local,
		       ic->ic_ci.ci_local_len);
		ci->ci_local_len = ic->ic_ci.ci_local_len;
	}

	// Find the corresponding RPC transaction using XID
	struct rpc_trans *rt = io_find_request_by_xid(ic->ic_xid);
	if (!rt) {
		LOG("No matching rpc_trans found for XID=%u", ic->ic_xid);

		io_socket_close(ic->ic_fd, ENOENT);
		io_context_destroy(ic);
		return ENOENT;
	}

	rt->rt_fd = ic->ic_fd;

	copy_connection_info(&ic->ic_ci, &rt->rt_info.ri_ci);
	io_context_destroy(ic);

	// Now that we're connected, prepare the RPC write request
	return io_rpc_trans_cb(rt);
}
