/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Shared kqueue substrate: the struct ring_context layout for
 * kqueue-based backends, plus primitives used by both the
 * FreeBSD aio backend (backend_kqueue.c) and the Darwin thread-
 * pool backend (backend_darwin.c).
 *
 * This header is internal to lib/io/ -- external callers use the
 * opaque ring_context_alloc/_free + accessor API via
 * lib/include/reffs/ring.h.
 */

#ifndef _REFFS_KQUEUE_COMMON_H
#define _REFFS_KQUEUE_COMMON_H

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <pthread.h>

struct ring_context;

/* Batch size for kevent() event drain in either main loop. */
#define KQUEUE_BATCH_SIZE 64

/*
 * On Linux, struct ring_context wraps a struct io_uring.  On
 * FreeBSD/Darwin it wraps a kqueue fd.  Callers outside lib/io/
 * treat it as opaque (ring.h forward-declares it), so the two
 * layouts can diverge freely.
 *
 * rc_shutdown_pipe is the async-signal-safe wake-up channel: the
 * signal handler writes one byte to rc_shutdown_pipe[1] (write(2)
 * is in the POSIX async-signal-safe list); the main loop watches
 * rc_shutdown_pipe[0] via EVFILT_READ and drains on wake-up.
 * This replaces EVFILT_USER + NOTE_TRIGGER (the kqueue-native
 * approach) because kevent(2) is NOT async-signal-safe on
 * FreeBSD or Darwin -- calling it from a signal handler is
 * undefined behavior.
 */
struct ring_context {
	int rc_kq_fd;
	pthread_mutex_t rc_mutex;
	int rc_shutdown_pipe[2]; /* [0] read end, [1] write end */
};

/*
 * Allocate a ring_context with fd fields set to -1.  Caller is
 * responsible for calling kq_setup (below) to initialize the
 * kqueue and shutdown pipe, and kq_teardown + ring_context_free
 * to release them.
 */
struct ring_context *ring_context_alloc(void);
void ring_context_free(struct ring_context *rc);

/*
 * Initialize the kqueue fd, shutdown pipe, and mutex.  Registers
 * the shutdown-pipe read-end with EVFILT_READ so the main loop
 * wakes when the signal handler writes to it.  Returns 0 on
 * success, -1 on failure (ring_context fields reset to -1 /
 * uninitialized on the error path).
 *
 * Called by each backend's io_handler_init / io_backend_init to
 * stand up its own ring.
 */
int kq_setup(struct ring_context *rc, const char *tag);
void kq_teardown(struct ring_context *rc);

/*
 * Global ring_context pointers set by each backend at init time
 * and consulted by the signal-shutdown path (which must wake all
 * main loops) and by external code looking up the active rings.
 */
void io_backend_set_global(struct ring_context *rc);
struct ring_context *io_backend_get_global(void);
struct ring_context *io_network_get_global(void);

/*
 * Setter for the network ring.  Each backend's io_handler_init
 * calls this after successful kq_setup.
 */
void io_network_set_global(struct ring_context *rc);

#endif /* _REFFS_KQUEUE_COMMON_H */
