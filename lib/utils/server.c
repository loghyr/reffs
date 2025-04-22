/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "reffs/server.h"

uuid_t server_uuid;

void server_boot_uuid_generate(void)
{
	uuid_generate(server_uuid);
}

uuid_t *server_boot_uuid_get(void)
{
	return &server_uuid;
}
