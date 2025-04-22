/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_NETWORK_H
#define _REFFS_NETWORK_H

#include <netdb.h>
#include <arpa/inet.h>

void addr_to_string(const struct sockaddr_storage *addr, char *buf,
		    size_t buflen, uint16_t *port);

#endif /* _REFFS_NETWORK_H */
