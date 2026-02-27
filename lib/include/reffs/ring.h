/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_RING_H
#define _REFFS_RING_H

#include <stdint.h>
#include <liburing.h>
#include <pthread.h>

struct ring_context {
	struct io_uring rc_ring;
	pthread_mutex_t rc_mutex;
};

#endif /* _REFFS_RING_H */
