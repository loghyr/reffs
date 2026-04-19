/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * io_uring connect-operation path: socket creation, io_uring_prep_connect
 * submission, and the io_handle_connect completion handler.  Shared
 * connection bookkeeping (io_conn_register/get/add_*_op/...) lives in
 * conn_info.c.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <netinet/in.h>
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
#include "reffs/ring.h"
#include "ring_internal.h"
#include "reffs/rpc.h"
#include "reffs/trace/io.h"

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
