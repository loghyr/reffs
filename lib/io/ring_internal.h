/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * Private definition of struct ring_context.
 *
 * The public header lib/include/reffs/ring.h forward-declares
 * struct ring_context so callers outside lib/io/ hold it opaquely
 * (they only ever see the pointer).  Code inside lib/io/ that
 * dereferences the struct includes this header to get the full
 * definition.
 *
 * This split keeps <liburing.h> out of public headers, which is a
 * prerequisite for FreeBSD and macOS backends that do not have
 * liburing at all.  See docs/io-backend-port-plan.md.
 */

#ifndef _REFFS_IO_RING_INTERNAL_H
#define _REFFS_IO_RING_INTERNAL_H

#include <liburing.h>
#include <pthread.h>

#include "reffs/ring.h"

struct ring_context {
	struct io_uring rc_ring;
	pthread_mutex_t rc_mutex;
	int rc_shutdown_efd; /* eventfd for signal-safe shutdown wakeup */
};

#endif /* _REFFS_IO_RING_INTERNAL_H */
