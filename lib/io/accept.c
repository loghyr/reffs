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
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "reffs/log.h"
#include "reffs/io.h"
#include "reffs/log.h"
#include "reffs/network.h"
#include "reffs/ring.h"
#include "ring_internal.h"
#include "tsan_io.h"
#include "reffs/trace/io.h"

struct accept_context {
	struct sockaddr_storage ac_addr;
	socklen_t ac_addrlen;
};

int io_request_accept_op(int fd, struct connection_info *ci,
			 struct ring_context *rc)
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
		struct accept_context *actx =
			calloc(1, sizeof(struct accept_context));
		if (!actx) {
			LOG("Failed to allocate buffer for accept - retry %d/%d",
			    retry_count + 1, max_retries);

			// Sleep before retry
			usleep(100000); // 100ms
			retry_count++;
			continue;
		}

		actx->ac_addrlen = sizeof(struct sockaddr_storage);

		struct io_context *ic = io_context_create(OP_TYPE_ACCEPT, fd,
							  actx, sizeof(*actx));
		if (!ic) {
			LOG("Failed to create accept context - retry %d/%d",
			    retry_count + 1, max_retries);
			free(actx);

			// Sleep before retry
			usleep(100000); // 100ms
			retry_count++;
			continue;
		}

		if (ci)
			copy_connection_info(&ic->ic_ci, ci);

		// Flag to track if we successfully submitted the request
		bool submitted = false;

		for (int i = 0; i < REFFS_IO_RING_RETRIES; i++) {
			pthread_mutex_lock(&rc->rc_mutex);
			sqe = io_uring_get_sqe(&rc->rc_ring);
			if (sqe)
				break;
			pthread_mutex_unlock(&rc->rc_mutex);
			sched_yield();
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

		io_uring_prep_accept(sqe, fd, (struct sockaddr *)&actx->ac_addr,
				     &actx->ac_addrlen, 0);
		sqe->user_data = (uint64_t)(uintptr_t)ic;

		trace_io_accept_submit(ic);

		for (int i = 0; i < REFFS_IO_MAX_RETRIES; i++) {
			ret = io_uring_submit(&rc->rc_ring);
			if (ret >= 0) {
				TSAN_RELEASE(ic);
				submitted = true;
				break;
			} else if (ret == -EAGAIN) {
				LOG("-EAGAIN in io_request_accept_op (retry %d/%d)",
				    i + 1, REFFS_IO_MAX_RETRIES);
				ic->ic_state |= IO_CONTEXT_SUBMITTED_EAGAIN;
				trace_io_eagain(ic, __func__, __LINE__);
				pthread_mutex_unlock(&rc->rc_mutex);
				sched_yield();
				pthread_mutex_lock(&rc->rc_mutex);
			} else {
				break;
			}
		}
		pthread_mutex_unlock(&rc->rc_mutex);

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
		     struct ring_context *rc)
{
	char addr_str[INET6_ADDRSTRLEN];
	uint16_t port;

	// Handle new connection
	int listen_fd = ic->ic_fd;

	bool accept_resubmitted = false;

	trace_io_context(ic, __func__, __LINE__);

	// Always try to set up the next accept first, to ensure we don't miss connections
	int accept_ret = io_request_accept_op(listen_fd, &ic->ic_ci, rc);
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
			io_request_accept_op(listen_fd, &ic->ic_ci, rc);
		}

		io_context_destroy(ic);
		return -client_fd;
	}

	int flag = 1;
	if (setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag,
		       sizeof(flag)) < 0) {
		LOG("setsockopt TCP_NODELAY failed for fd=%d: %s", client_fd,
		    strerror(errno));
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

		TRACE("Accepted connection from %s:%d on fd=%d", addr_str, port,
		      client_fd);
		TRACE("ACCEPTED: fd=%d from %s:%d", client_fd, addr_str, port);
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
		TRACE("Accepted new connection fd=%d", client_fd);
	} else {
		LOG("Warning: Connection fd=%d not found after registration",
		    client_fd);
	}

	// Prepare to read from this new connection
	io_request_read_op(client_fd, &ic->ic_ci, rc);

	// Accept more connections
	if (!accept_resubmitted) {
		io_request_accept_op(listen_fd, &ic->ic_ci, rc);
	}

	io_context_destroy(ic);
	return 0;
}
