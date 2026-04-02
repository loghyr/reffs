/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_RING_H
#define _REFFS_RING_H

#include <stdint.h>
#include <liburing.h>
#include <pthread.h>

struct ring_context {
	struct io_uring rc_ring;
	pthread_mutex_t rc_mutex;
	int rc_shutdown_efd; /* eventfd for signal-safe shutdown wakeup */
};

#endif /* _REFFS_RING_H */
