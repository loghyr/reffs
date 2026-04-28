/* SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com> */
/* SPDX-License-Identifier: AGPL-3.0-or-later */

#ifndef REFFS_NFS4_CLIENT_HOST_PORT_H
#define REFFS_NFS4_CLIENT_HOST_PORT_H

#include <stddef.h>

/*
 * Parse a host[:port] / [host]:port / [host] / host string into a
 * separate host string and port number.  Handles bracketed IPv6
 * literals correctly; the previous strrchr-based parser truncated
 * unbracketed IPv6 at the last colon and left the brackets attached
 * to bracketed forms.
 *
 * Forms accepted:
 *   "host"               -> host="host",       *port=0
 *   "host:NNN"           -> host="host",       *port=NNN
 *   "[ipv6]"             -> host="ipv6",       *port=0
 *   "[ipv6]:NNN"         -> host="ipv6",       *port=NNN
 *   "ipv6-literal"       -> host="ipv6-literal", *port=0
 *     (>1 unbracketed colons treated as a literal IPv6, no port;
 *      caller resolves via getaddrinfo)
 *
 * *port == 0 on return means "no explicit port", caller should fall
 * back to portmap or its own default.  port range is otherwise
 * 1..65535.
 *
 * Returns 0 on success, -EINVAL on:
 *   - NULL input or NULL out-params
 *   - empty host
 *   - host that does not fit in host_buf_sz (incl. trailing NUL)
 *   - port outside [1, 65535] when explicitly given
 *   - bracketed form with stray characters after the closing bracket
 *     (e.g. "[host]junk")
 *   - "[host]:" with empty port string
 *   - "host:" / ":port" / ":" with an empty side
 */
int mds_parse_host_port(const char *input, char *host_buf, size_t host_buf_sz,
			int *port_out);

#endif /* REFFS_NFS4_CLIENT_HOST_PORT_H */
