/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "reffs/log.h"
#include "reffs/io.h"
#include "reffs/log.h"

int io_lsnr_setup_ipv4(int port)
{
	int listen_fd;
	struct sockaddr_in address;

	listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		LOG("IPv4 socket: %s", strerror(errno));
		return -1;
	}

	// Set socket options to reuse address
	int opt = 1;
	if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) <
	    0) {
		LOG("IPv4 setsockopt: %s", strerror(errno));
		close(listen_fd);
		return -1;
	}

	address.sin_family = AF_INET;
	address.sin_addr.s_addr = INADDR_ANY;
	address.sin_port = htons(port);

	if (bind(listen_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
		LOG("IPv4 bind: %s", strerror(errno));
		close(listen_fd);
		return -1;
	}

	if (listen(listen_fd, 10) < 0) {
		LOG("IPv4 listen: %s", strerror(errno));
		close(listen_fd);
		return -1;
	}

	return listen_fd;
}

int io_lsnr_setup_ipv6(int port)
{
	int listen_fd;
	struct sockaddr_in6 address;

	listen_fd = socket(AF_INET6, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		LOG("IPv6 socket: %s", strerror(errno));
		return -1;
	}

	// Set socket options to reuse address
	int opt = 1;
	if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) <
	    0) {
		LOG("IPv6 setsockopt (reuse): %s", strerror(errno));
		close(listen_fd);
		return -1;
	}

	// Set IPV6_V6ONLY to 1 to ensure this socket only handles IPv6
	int ipv6only = 1;
	if (setsockopt(listen_fd, IPPROTO_IPV6, IPV6_V6ONLY, &ipv6only,
		       sizeof(ipv6only)) < 0) {
		LOG("IPv6 setsockopt (v6only): %s", strerror(errno));
		close(listen_fd);
		return -1;
	}

	memset(&address, 0, sizeof(address));
	address.sin6_family = AF_INET6;
	address.sin6_addr = in6addr_any;
	address.sin6_port = htons(port);

	if (bind(listen_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
		LOG("IPv6 bind: %s", strerror(errno));
		close(listen_fd);
		return -1;
	}

	if (listen(listen_fd, 10) < 0) {
		LOG("IPv6 listen: %s", strerror(errno));
		close(listen_fd);
		return -1;
	}

	return listen_fd;
}

// Combined function that sets up both IPv4 and IPv6 listeners

// Returns an array of file descriptors: [0] = IPv4, [1] = IPv6
// If a listener fails, its value will be -1
int *io_lsnr_setup(int port)
{
	static int fds[2]; // Static to ensure it persists after function returns

	// Set up IPv4 listener
	fds[0] = io_lsnr_setup_ipv4(port);

	// Set up IPv6 listener
	fds[1] = io_lsnr_setup_ipv6(port);

	// Check if both listeners failed
	if (fds[0] < 0 && fds[1] < 0) {
		LOG("Failed to set up both IPv4 and IPv6 listeners");
		return NULL;
	}

	return fds;
}
