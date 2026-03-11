/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include "reffs/backend.h"
#include "reffs/log.h"

extern const struct reffs_storage_ops ram_storage_ops;
extern const struct reffs_storage_ops posix_storage_ops;

static const struct reffs_storage_ops *backends[] = {
	[REFFS_STORAGE_RAM] = &ram_storage_ops,
	[REFFS_STORAGE_POSIX] = &posix_storage_ops,
};

void reffs_backend_init(void)
{
	/* Any global initialization for backends can go here */
}

const struct reffs_storage_ops *
reffs_backend_get_ops(enum reffs_storage_type type)
{
	if (type < 0 || type >= (sizeof(backends) / sizeof(backends[0]))) {
		LOG("Unknown storage type: %d", type);
		return NULL;
	}
	TRACE("Storage type is %d:", type);
	return backends[type];
}
