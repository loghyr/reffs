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

/*
 * libtirpc forward decl -- pulling <rpc/rpc.h> here would force every
 * source_bind.h consumer to carry libtirpc CFLAGS, including lib/tls
 * which has no other reason to.  The signature has been stable since
 * libtirpc 0.2 (Linux) so a local prototype is safe.
 */
extern int bindresvport_sa(int sd, struct sockaddr *sa);

/*
 * Bind the socket to source_ip AND a privileged source port in one
 * step via libtirpc's bindresvport_sa.  Two reasons it MUST be one
 * step:
 *
 *   1. Two separate calls -- bind(fd, src, port=0) then
 *      bindresvport(fd, NULL) -- fail because the second bind on an
 *      already-bound fd returns EINVAL on Linux.
 *
 *   2. The DS-side connect path (lib/nfs4/ps/ds_io.c) never calls
 *      bindresvport after source_bind, so without bindresvport_sa
 *      the source port is whatever the kernel picks at first packet:
 *      >= 1024 on Linux.  Strict-port servers (Hammerspace Anvil)
 *      reject NFSv3 WRITEs from an ephemeral source with
 *      NFS3ERR_PERM -- silently failing the ec_demo write path even
 *      though the MDS-side EXCHANGE_ID succeeded.
 */
static inline int source_bind(int fd, const char *source_ip, const char *who)
{
	if (!source_ip || source_ip[0] == '\0')
		return 0;

	struct sockaddr_in src;

	memset(&src, 0, sizeof(src));
	src.sin_family = AF_INET;
	src.sin_port = 0; /* bindresvport_sa picks a privileged port */

	if (inet_pton(AF_INET, source_ip, &src.sin_addr) != 1) {
		fprintf(stderr,
			"%s: source IP '%s' is not a valid IPv4 address\n",
			who ? who : "source_bind", source_ip);
		errno = EINVAL;
		return -EINVAL;
	}

	if (bindresvport_sa(fd, (struct sockaddr *)&src) < 0) {
		int e = errno;

		fprintf(stderr,
			"%s: bindresvport_sa to source IP %s failed: %s "
			"(address must be assigned to a local interface and "
			"caller must be root for ports < 1024)\n",
			who ? who : "source_bind", source_ip, strerror(e));
		errno = e;
		return -e;
	}

	return 0;
}

#endif /* REFFS_CLIENT_SOURCE_BIND_H */
