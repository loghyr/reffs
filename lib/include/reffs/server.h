/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_SERVER_H
#define _REFFS_SERVER_H

#include <uuid/uuid.h>

void server_boot_uuid_generate(void);
uuid_t *server_boot_uuid_get(void);

void server_port_set(int port);
int server_port_get(void);

#endif /* _REFFS_SERVER_H */
