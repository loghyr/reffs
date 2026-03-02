/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_NETWORK_H
#define _REFFS_NETWORK_H

#include <netdb.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdbool.h>

struct connection_info {
	struct sockaddr_storage ci_peer;
	socklen_t ci_peer_len;
	struct sockaddr_storage ci_local;
	socklen_t ci_local_len;
};

void addr_to_string(const struct sockaddr_storage *addr, char *buf,
		    size_t buflen, uint16_t *port);

static inline void copy_connection_info(struct connection_info *dst,
					struct connection_info *src)
{
	if (!dst || !src)
		return;

	memcpy(&dst->ci_peer, &src->ci_peer, sizeof(struct sockaddr_storage));
	dst->ci_peer_len = src->ci_peer_len;

	memcpy(&dst->ci_local, &src->ci_local, sizeof(struct sockaddr_storage));
	dst->ci_local_len = src->ci_local_len;
}

#endif /* _REFFS_NETWORK_H */
