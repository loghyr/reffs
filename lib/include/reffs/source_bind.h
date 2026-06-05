/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * source_bind -- bind a socket to a specific local source address.
 *
 * The ec_demo client supports running multiple parallel instances
 * from the same host, each pretending to be a different NFS client
 * by sourcing its TCP from a different local IPv4 address.  The MDS
 * sees them as distinct clients (combined with --id, distinct
 * EXCHANGE_ID clientowner blobs); the DS sees them as distinct
 * NFSv3 client endpoints.  Useful for stress-testing multi-client
 * scenarios from a single host that has multiple interfaces or
 * additional addresses configured.
 *
 * source_ip == NULL or "" is the no-op path: returns 0 without
 * touching the socket, identical to the pre-source-IP behaviour.
 *
 * source_ip must be an IPv4 dotted-quad address (e.g. "10.1.1.5"),
 * AND must already be assigned to a local interface on this host
 * (the bind() syscall enforces this -- a bind to an unassigned
 * address fails with EADDRNOTAVAIL).
 */
#ifndef REFFS_CLIENT_SOURCE_BIND_H
#define REFFS_CLIENT_SOURCE_BIND_H

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

static inline int source_bind(int fd, const char *source_ip, const char *who)
{
	if (!source_ip || source_ip[0] == '\0')
		return 0;

	struct sockaddr_in src;

	memset(&src, 0, sizeof(src));
	src.sin_family = AF_INET;
	src.sin_port = 0; /* let bindresvport/kernel pick the port */

	if (inet_pton(AF_INET, source_ip, &src.sin_addr) != 1) {
		fprintf(stderr,
			"%s: source IP '%s' is not a valid IPv4 address\n",
			who ? who : "source_bind", source_ip);
		errno = EINVAL;
		return -EINVAL;
	}

	if (bind(fd, (struct sockaddr *)&src, sizeof(src)) < 0) {
		int e = errno;

		fprintf(stderr,
			"%s: bind to source IP %s failed: %s "
			"(address must already be assigned to a local interface)\n",
			who ? who : "source_bind", source_ip, strerror(e));
		errno = e;
		return -e;
	}

	return 0;
}

#endif /* REFFS_CLIENT_SOURCE_BIND_H */
