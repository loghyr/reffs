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

int request_accept_op(int fd, struct connection_info *ci, struct io_uring *ring)
{
	struct io_uring_sqe *sqe = NULL;
	int ret = 0;

	struct sockaddr *buffer = malloc(sizeof(struct sockaddr));
	if (!buffer) {
		LOG("Failed to allocate buffer");
		close(fd);
		return ENOMEM;
	}

	struct io_context *ic = io_context_create(OP_TYPE_ACCEPT, fd, buffer,
						  sizeof(struct sockaddr));
	if (!ic) {
		LOG("Failed to create read context");
		free(buffer);
		close(fd);
		return ENOMEM;
	}

	if (ci)
		copy_connection_info(&ic->ic_ci, ci);

	for (int i = 0; i < REFFS_IO_MAX_RETRIES; i++) {
		sqe = io_uring_get_sqe(ring);
		if (sqe)
			break;
		usleep(IO_URING_WAIT_US);
	}

	if (!sqe) {
		close(fd);
		io_context_free(ic);
		return -ENOMEM;
	}

	io_uring_prep_accept(sqe, fd, ic->ic_buffer,
			     (socklen_t *)&ic->ic_buffer_len, 0);
	sqe->user_data = (uint64_t)(uintptr_t)ic;

	trace_io_accept_submit(ic);

	for (int i = 0; i < REFFS_IO_MAX_RETRIES; i++) {
		ret = io_uring_submit(ring);
		if (ret >= 0)
			break;
		if (ret == -EAGAIN) {
			usleep(IO_URING_WAIT_US);
			break; // right now we don't know what io_uring is doing!
		} else
			break;
	}

	if (ret < 0) {
		close(fd);
		io_context_free(ic);
	} else {
		ret = 0;
	}

	return 0;
}

int io_handle_accept(struct io_context *ic, int client_fd,
		     struct io_uring *ring)
{
	char addr_str[INET6_ADDRSTRLEN];
	uint16_t port;

	// Handle new connection
	int listen_fd = ic->ic_fd;

	ic->ic_ci.ci_peer_len = sizeof(ic->ic_ci.ci_peer);
	if (getpeername(client_fd, (struct sockaddr *)&ic->ic_ci.ci_peer,
			&ic->ic_ci.ci_peer_len) == 0) {
		addr_to_string(&ic->ic_ci.ci_peer, addr_str, INET6_ADDRSTRLEN,
			       &port);
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
	} else {
		LOG("Failed to get local information: %s", strerror(errno));
		memset(&ic->ic_ci.ci_local, 0, sizeof(ic->ic_ci.ci_local));
		ic->ic_ci.ci_local_len = 0;
	}

	// Register this client
	register_client_fd(client_fd);

	// Prepare to read from this new connection
	request_additional_read_data(client_fd, &ic->ic_ci, ring);

	// Accept more connections
	request_accept_op(listen_fd, &ic->ic_ci, ring);

	io_context_free(ic); // Free the completed accept context
	return 0;
}
