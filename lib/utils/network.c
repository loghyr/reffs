/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "reffs/network.h"

void addr_to_string(const struct sockaddr_storage *addr, char *buf,
		    size_t buflen, uint16_t *port)
{
	if (addr->ss_family == AF_INET) {
		// IPv4
		struct sockaddr_in *ipv4 = (struct sockaddr_in *)addr;
		inet_ntop(AF_INET, &ipv4->sin_addr, buf, buflen);
		if (port)
			*port = ntohs(ipv4->sin_port);
	} else if (addr->ss_family == AF_INET6) {
		struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)addr;

		if (IN6_IS_ADDR_V4MAPPED(&ipv6->sin6_addr)) {
			struct in_addr ipv4_addr;
			memcpy(&ipv4_addr, ((char *)&ipv6->sin6_addr) + 12, 4);
			inet_ntop(AF_INET, &ipv4_addr, buf, buflen);
		} else {
			inet_ntop(AF_INET6, &ipv6->sin6_addr, buf, buflen);
		}

		if (port)
			*port = ntohs(ipv6->sin6_port);
	} else {
		snprintf(buf, buflen, "unknown");
		if (port)
			*port = 0;
	}
}

void sockaddr_in_to_str(const struct sockaddr_in *sin, char *buf, size_t buflen)
{
	inet_ntop(AF_INET, &sin->sin_addr, buf, buflen);
}

void sockaddr_in_to_full_str(const struct sockaddr_in *sin, char *buf,
			     size_t buflen)
{
	char ip[INET6_ADDRSTRLEN];
	uint16_t port;

	inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
	port = ntohs(sin->sin_port);

	snprintf(buf, buflen, "%s.%hhu.%hhu", ip,
		 (unsigned char)((port >> 8) & 0xFF),
		 (unsigned char)(port & 0xFF));
}
