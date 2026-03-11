/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "reffs/server.h"

uuid_t server_uuid;
int server_port = 2049;

void server_boot_uuid_generate(void)
{
	uuid_generate(server_uuid);
}

uuid_t *server_boot_uuid_get(void)
{
	return &server_uuid;
}

void server_port_set(int port)
{
	server_port = port;
}

int server_port_get(void)
{
	return server_port;
}
