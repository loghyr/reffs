/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Backend-agnostic completion handlers.
 *
 * io_handle_accept and io_handle_connect run after the OS has signalled
 * that a socket operation completed.  On the liburing backend this is
 * a CQE dispatch in handler.c's main loop; on the kqueue backend it is
 * an EVFILT_READ / EVFILT_WRITE dispatch in backend_kqueue.c's main
 * loop.  In both cases the work the handler does -- register the
 * conn_info, fill in addresses, kick off the read loop, resubmit the
 * accept -- is identical.  The backend-specific submission sites
 * (io_request_accept_op, io_request_read_op, the connect helper) are
 * still backend-local.
 *
 * Calling convention for the int result argument follows io_uring's
 * cqe->res semantics:
 *   io_handle_accept(ic, client_fd_or_neg_errno, rc)
 *     - nonnegative: the accepted client fd
 *     - negative:    a negative errno describing the failure
 *   io_handle_connect(ic, result_or_neg_errno, rc)
 *     - zero:     connect succeeded
 *     - negative: a negative errno describing the failure
 *
 * kqueue callers must translate -1 / errno into -errno before invoking
 * these handlers; see accept_and_dispatch / connect_and_dispatch in
 * backend_kqueue.c.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "reffs/log.h"
#include "reffs/io.h"
#include "reffs/network.h"
#include "reffs/ring.h"
#include "reffs/rpc.h"
#include "reffs/trace/io.h"

int io_handle_accept(struct io_context *ic, int client_fd,
		     struct ring_context *rc)
{
	char addr_str[INET6_ADDRSTRLEN];
	uint16_t port;

	int listen_fd = ic->ic_fd;

	bool accept_resubmitted = false;

	trace_io_context(ic, __func__, __LINE__);

	/*
	 * Always try to re-arm the accept first, to ensure we don't miss
	 * incoming connections while we set up the one we just got.
	 */
	int accept_ret = io_request_accept_op(listen_fd, &ic->ic_ci, rc);
	if (accept_ret == 0) {
		accept_resubmitted = true;
	} else {
		LOG("Failed to resubmit accept request - watchdog will retry later");
	}

	if (client_fd < 0) {
		LOG("Accept failed with error: %s", strerror(-client_fd));

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

	struct conn_info *client_conn =
		io_conn_register(client_fd, CONN_ACCEPTED, CONN_ROLE_SERVER);
	if (!client_conn) {
		LOG("Failed to register client connection fd=%d", client_fd);
		io_socket_close(client_fd, ENOMEM);
		io_context_destroy(ic);
		return ENOMEM;
	}

	ic->ic_ci.ci_peer_len = sizeof(ic->ic_ci.ci_peer);
	if (getpeername(client_fd, (struct sockaddr *)&ic->ic_ci.ci_peer,
			&ic->ic_ci.ci_peer_len) == 0) {
		addr_to_string(&ic->ic_ci.ci_peer, addr_str, INET6_ADDRSTRLEN,
			       &port);

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

	ic->ic_ci.ci_local_len = sizeof(ic->ic_ci.ci_local);
	if (getsockname(client_fd, (struct sockaddr *)&ic->ic_ci.ci_local,
			&ic->ic_ci.ci_local_len) == 0) {
		addr_to_string(&ic->ic_ci.ci_local, addr_str, INET6_ADDRSTRLEN,
			       &port);

		memcpy(&client_conn->ci_local, &ic->ic_ci.ci_local,
		       ic->ic_ci.ci_local_len);
		client_conn->ci_local_len = ic->ic_ci.ci_local_len;
	} else {
		LOG("Failed to get local information: %s", strerror(errno));
		memset(&ic->ic_ci.ci_local, 0, sizeof(ic->ic_ci.ci_local));
		ic->ic_ci.ci_local_len = 0;
	}

	io_client_fd_register(client_fd);

	struct conn_info *conn = io_conn_get(client_fd);
	if (conn) {
		TRACE("Accepted new connection fd=%d", client_fd);
	} else {
		LOG("Warning: Connection fd=%d not found after registration",
		    client_fd);
	}

	io_request_read_op(client_fd, &ic->ic_ci, rc);

	if (!accept_resubmitted) {
		io_request_accept_op(listen_fd, &ic->ic_ci, rc);
	}

	io_context_destroy(ic);
	return 0;
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

	ic->ic_ci.ci_peer_len = sizeof(ic->ic_ci.ci_peer);
	if (getpeername(ic->ic_fd, (struct sockaddr *)&ic->ic_ci.ci_peer,
			&ic->ic_ci.ci_peer_len) != 0) {
		LOG("Failed to get peer information: %s", strerror(errno));

		io_conn_set_error(ic->ic_fd, errno);

		memset(&ic->ic_ci.ci_peer, 0, sizeof(ic->ic_ci.ci_peer));
		ic->ic_ci.ci_peer_len = 0;

		io_socket_close(ic->ic_fd, errno);
		io_context_destroy(ic);
		return errno;
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

	TRACE("Connection established for fd=%d", ic->ic_fd);

	struct conn_info *ci = io_conn_get(ic->ic_fd);
	if (ci) {
		memcpy(&ci->ci_peer, &ic->ic_ci.ci_peer, ic->ic_ci.ci_peer_len);
		ci->ci_peer_len = ic->ic_ci.ci_peer_len;

		memcpy(&ci->ci_local, &ic->ic_ci.ci_local,
		       ic->ic_ci.ci_local_len);
		ci->ci_local_len = ic->ic_ci.ci_local_len;
	}

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

	return io_rpc_trans_cb(rt);
}
