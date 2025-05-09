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
#include "reffs/network.h"
#include "reffs/test.h"
#include "reffs/io.h"
#include "reffs/trace/io.h"

int io_request_accept_op(int fd, struct connection_info *ci, struct io_uring *ring)
{
	struct io_uring_sqe *sqe = NULL;
	int ret = 0;
	int retry_count = 0;
	const int max_retries = 5;

	// Register the listening socket if not already tracked
	struct conn_info *conn = io_conn_get(fd);
	if (!conn) {
		conn = io_conn_register(fd, CONN_LISTENING, CONN_ROLE_SERVER);
		if (!conn) {
			LOG("Failed to register listener socket fd=%d", fd);
			// Continue anyway - this is just tracking
		}
	}

	// Use a retry loop instead of recursion
	while (retry_count < max_retries) {
		struct sockaddr *buffer = malloc(sizeof(struct sockaddr));
		if (!buffer) {
			LOG("Failed to allocate buffer for accept - retry %d/%d",
			    retry_count + 1, max_retries);

			// Sleep before retry
			usleep(100000); // 100ms
			retry_count++;
			continue;
		}

		struct io_context *ic = io_context_create(
			OP_TYPE_ACCEPT, fd, buffer, sizeof(struct sockaddr));
		if (!ic) {
			LOG("Failed to create accept context - retry %d/%d",
			    retry_count + 1, max_retries);
			free(buffer);

			// Sleep before retry
			usleep(100000); // 100ms
			retry_count++;
			continue;
		}

		if (ci)
			copy_connection_info(&ic->ic_ci, ci);

		// Flag to track if we successfully submitted the request
		bool submitted = false;

		for (int i = 0; i < REFFS_IO_MAX_RETRIES; i++) {
			sqe = io_uring_get_sqe(ring);
			if (sqe)
				break;
			usleep(IO_URING_WAIT_US);
		}

		if (!sqe) {
			LOG("Failed to get SQE for accept - retry %d/%d",
			    retry_count + 1, max_retries);
			io_context_destroy(ic);

			// Sleep before retry
			usleep(100000); // 100ms
			retry_count++;
			continue;
		}

		io_uring_prep_accept(sqe, fd, ic->ic_buffer,
				     (socklen_t *)&ic->ic_buffer_len, 0);
		sqe->user_data = (uint64_t)(uintptr_t)ic;

		trace_io_accept_submit(ic);

		for (int i = 0; i < REFFS_IO_MAX_RETRIES; i++) {
			ret = io_uring_submit(ring);
			if (ret >= 0) {
				submitted = true;
				break;
			}
			if (ret == -EAGAIN) {
				submitted = true;
				usleep(IO_URING_WAIT_US);
				ret = 0; // Fix once we know io_uring can handle it!
				break;
			} else {
				break;
			}
		}

		if (!submitted) {
			LOG("Failed to submit accept operation - retry %d/%d: %s",
			    retry_count + 1, max_retries, strerror(-ret));
			io_context_destroy(ic);

			// Sleep before retry
			usleep(100000); // 100ms
			retry_count++;
			continue;
		}

		// Success!
		return 0;
	}

	// If we get here, we've exhausted all retries
	LOG("CRITICAL: Failed to submit accept operation after %d retries",
	    max_retries);

	// Instead of giving up completely, schedule a retry through the watchdog mechanism
	// by setting the last_accept_check time to a value that will trigger a check soon
	if (conn) {
		conn->ci_state = CONN_ERROR;
		conn->ci_error = (ret < 0) ? -ret : EAGAIN;
	}

	return (ret < 0) ? -ret : EAGAIN;
}

int io_handle_accept(struct io_context *ic, int client_fd,
		     struct io_uring *ring)
{
	char addr_str[INET6_ADDRSTRLEN];
	uint16_t port;

	// Handle new connection
	int listen_fd = ic->ic_fd;

	bool accept_resubmitted = false;

	trace_io_context(ic, __func__, __LINE__);

	// Always try to set up the next accept first, to ensure we don't miss connections
	int accept_ret = io_request_accept_op(listen_fd, &ic->ic_ci, ring);
	if (accept_ret == 0) {
		accept_resubmitted = true;
	} else {
		LOG("Failed to resubmit accept request - watchdog will retry later");
		// The watchdog will handle this
	}

	if (client_fd < 0) {
		LOG("Accept failed with error: %s", strerror(-client_fd));

		// If we haven't already resubmitted the accept, try one more time
		if (!accept_resubmitted) {
			LOG("Trying one more time to resubmit accept");
			io_request_accept_op(listen_fd, &ic->ic_ci, ring);
		}

		io_context_destroy(ic);
		return -client_fd;
	}

	// Immediately register the new client connection with ACCEPTED state
	struct conn_info *client_conn =
		io_conn_register(client_fd, CONN_ACCEPTED, CONN_ROLE_SERVER);
	if (!client_conn) {
		LOG("Failed to register client connection fd=%d", client_fd);
		io_socket_close(client_fd, ENOMEM);
		io_context_destroy(ic);
		return ENOMEM;
	}

	// Get peer information
	ic->ic_ci.ci_peer_len = sizeof(ic->ic_ci.ci_peer);
	if (getpeername(client_fd, (struct sockaddr *)&ic->ic_ci.ci_peer,
			&ic->ic_ci.ci_peer_len) == 0) {
		addr_to_string(&ic->ic_ci.ci_peer, addr_str, INET6_ADDRSTRLEN,
			       &port);

		// Store peer information in connection tracking
		memcpy(&client_conn->ci_peer, &ic->ic_ci.ci_peer,
		       ic->ic_ci.ci_peer_len);
		client_conn->ci_peer_len = ic->ic_ci.ci_peer_len;

		LOG("Accepted connection from %s:%d on fd=%d", addr_str, port,
		    client_fd);
	} else {
		LOG("Failed to get peer information: %s", strerror(errno));
		memset(&ic->ic_ci.ci_peer, 0, sizeof(ic->ic_ci.ci_peer));
		ic->ic_ci.ci_peer_len = 0;
	}

	// Get local information
	ic->ic_ci.ci_local_len = sizeof(ic->ic_ci.ci_local);
	if (getsockname(client_fd, (struct sockaddr *)&ic->ic_ci.ci_local,
			&ic->ic_ci.ci_local_len) == 0) {
		addr_to_string(&ic->ic_ci.ci_local, addr_str, INET6_ADDRSTRLEN,
			       &port);

		// Store local information in connection tracking
		memcpy(&client_conn->ci_local, &ic->ic_ci.ci_local,
		       ic->ic_ci.ci_local_len);
		client_conn->ci_local_len = ic->ic_ci.ci_local_len;
	} else {
		LOG("Failed to get local information: %s", strerror(errno));
		memset(&ic->ic_ci.ci_local, 0, sizeof(ic->ic_ci.ci_local));
		ic->ic_ci.ci_local_len = 0;
	}

	// Register this client in buffer state tracking
	io_client_fd_register(client_fd);

	struct conn_info *conn = io_conn_get(client_fd);
	if (conn) {
		// No need to check state since we're using operation counts
		// Just ensure the state is properly set to CONNECTED initially
		LOG("Accepted new connection fd=%d", client_fd);
	} else {
		LOG("Warning: Connection fd=%d not found after registration",
		    client_fd);
	}

	// Prepare to read from this new connection
	io_request_read_op(client_fd, &ic->ic_ci, ring);

	// Accept more connections
	if (!accept_resubmitted) {
		io_request_accept_op(listen_fd, &ic->ic_ci, ring);
	}

	io_context_destroy(ic);
	return 0;
}
