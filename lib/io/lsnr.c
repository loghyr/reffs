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
#include <fcntl.h>
#include <errno.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <stdbool.h>

#include "reffs/log.h"
#include "reffs/network.h"
#include "reffs/test.h"
#include "reffs/io.h"

// Setup a listening socket
int setup_listener(int port)
{
	int listen_fd;
	struct sockaddr_in address;

	listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		LOG("socket: %s", strerror(errno));
		return -1;
	}

	// Set socket options to reuse address
	int opt = 1;
	if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) <
	    0) {
		LOG("setsockopt: %s", strerror(errno));
		close(listen_fd);
		return -1;
	}

	address.sin_family = AF_INET;
	address.sin_addr.s_addr = INADDR_ANY;
	address.sin_port = htons(port);

	if (bind(listen_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
		LOG("bind: %s", strerror(errno));
		close(listen_fd);
		return -1;
	}

	if (listen(listen_fd, 10) < 0) {
		LOG("listen: %s", strerror(errno));
		close(listen_fd);
		return -1;
	}

	TRACE(REFFS_TRACE_LEVEL_WARNING, "Listening on port %d", port);
	return listen_fd;
}
