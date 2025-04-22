/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_SERVER_H
#define _REFFS_SERVER_H

#include <uuid/uuid.h>

void server_boot_uuid_generate(void);
uuid_t *server_boot_uuid_get(void);

#endif /* _REFFS_SERVER_H */
