/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_RING_H
#define _REFFS_RING_H

/*
 * struct ring_context is opaque to callers outside lib/io/.  The full
 * definition lives in lib/io/ring_internal.h so that <liburing.h>
 * stays contained inside the I/O layer, leaving room for non-Linux
 * backends (FreeBSD aio+kqueue, thread pool).  See
 * docs/io-backend-port-plan.md.
 */
struct ring_context;

/*
 * Allocate / free a ring_context.  Because the struct is opaque,
 * callers cannot stack-allocate it and must use these helpers.
 * The lifecycle is:
 *     rc = ring_context_alloc();
 *     io_handler_init(rc, ...)  or  io_backend_init(rc);
 *     ... use ...
 *     io_handler_fini(rc)  or  io_backend_fini(rc);
 *     ring_context_free(rc);
 */
struct ring_context *ring_context_alloc(void);
void ring_context_free(struct ring_context *rc);

#endif /* _REFFS_RING_H */
