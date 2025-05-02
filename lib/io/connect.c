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
#include <urcu.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <time.h>
#include <stdbool.h>

#include "reffs/log.h"
#include "reffs/rpc.h"
#include "reffs/network.h"
#include "reffs/server.h"
#include "reffs/task.h"
#include "reffs/test.h"
#include "reffs/io.h"
#include "reffs/trace/io.h"

int io_send_request(struct rpc_trans *rt)
{
	int ret;

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
			close(sockfd);
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
			close(sockfd);
			return ENOMEM;
		}

		addr = NULL;

		// Store XID for later matching
		ic->ic_xid = rt->rt_info.ri_xid;

		// Submit connect operation to io_uring
		struct io_uring_sqe *sqe = io_uring_get_sqe(rt->rt_ring);
		if (!sqe) {
			io_context_free(ic);
			close(sockfd);
			return ENOBUFS;
		}

		io_uring_prep_connect(sqe, sockfd, (struct sockaddr *)&addr,
				      sizeof(addr));
		io_uring_sqe_set_data(sqe, ic);
		trace_io_connect_submit(ic);

		// Submit and wait for connect completion
		io_uring_submit(rt->rt_ring);

		// For synchronous connect, you might want to wait here
		// Or return and let the callback handle it when connect completes

		return 0; // Connection initiated, will be handled by callback
	}

	// If we already have a connection or after establishing one synchronously
	return io_rpc_trans_cb(rt);
}

int io_handle_connect(struct io_context *ic, int result,
		      struct io_uring __attribute__((unused)) * ring)
{
	char addr_str[INET6_ADDRSTRLEN];
	uint16_t port;

	if (result < 0) {
		LOG("Connect failed for fd=%d: %s", ic->ic_fd,
		    strerror(-result));
		close(ic->ic_fd);
		io_context_free(ic);
		return -result;
	}

	ic->ic_ci.ci_peer_len = sizeof(ic->ic_ci.ci_peer);
	if (getpeername(ic->ic_fd, (struct sockaddr *)&ic->ic_ci.ci_peer,
			&ic->ic_ci.ci_peer_len) == 0) {
		addr_to_string(&ic->ic_ci.ci_peer, addr_str, INET6_ADDRSTRLEN,
			       &port);
	} else {
		LOG("Failed to get peer information: %s", strerror(errno));
		memset(&ic->ic_ci.ci_peer, 0, sizeof(ic->ic_ci.ci_peer));
		ic->ic_ci.ci_peer_len = 0;
	}

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

	LOG("Connection established for fd=%d", ic->ic_fd);

	// Find the corresponding RPC transaction using XID
	struct rpc_trans *rt = io_find_request_by_xid(ic->ic_xid);
	if (!rt) {
		LOG("No matching rpc_trans found for XID=%u", ic->ic_xid);
		close(ic->ic_fd);
		io_context_free(ic);
		return ENOENT;
	}

	copy_connection_info(&ic->ic_ci, &rt->rt_info.ri_ci);
	io_context_free(ic);

	// Now that we're connected, prepare the RPC write request
	return io_rpc_trans_cb(rt);
}
