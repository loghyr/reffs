/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Shared kqueue substrate: struct ring_context allocator, the
 * kq_setup/kq_teardown primitives that build a kqueue fd + a
 * shutdown pipe registered with EVFILT_READ, global ring
 * pointers consulted from the signal-handler path, and the
 * async-signal-safe io_handler_signal_shutdown wake-up.
 *
 * Compiled on FreeBSD (with the aio backend) and on Darwin
 * (with the thread-pool backend).  Linux does not compile this
 * file -- see lib/io/Makefile.am.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/types.h>
#include <unistd.h>

#include "reffs/log.h"
#include "reffs/ring.h"

#include "kqueue_common.h"
#include "posix_shims.h"

static struct ring_context *g_backend_rc;
static struct ring_context *g_network_rc;

void io_backend_set_global(struct ring_context *rc)
{
	g_backend_rc = rc;
}

void io_network_set_global(struct ring_context *rc)
{
	g_network_rc = rc;
}

struct ring_context *io_backend_get_global(void)
{
	return g_backend_rc;
}

struct ring_context *io_network_get_global(void)
{
	return g_network_rc;
}

struct ring_context *ring_context_alloc(void)
{
	struct ring_context *rc = calloc(1, sizeof(*rc));

	if (!rc)
		return NULL;
	rc->rc_kq_fd = -1;
	rc->rc_shutdown_pipe[0] = -1;
	rc->rc_shutdown_pipe[1] = -1;
	return rc;
}

void ring_context_free(struct ring_context *rc)
{
	free(rc);
}

int kq_setup(struct ring_context *rc, const char *tag)
{
	if (pthread_mutex_init(&rc->rc_mutex, NULL) != 0) {
		LOG("%s: mutex init failed", tag);
		return -1;
	}

	rc->rc_kq_fd = kqueue();
	if (rc->rc_kq_fd < 0) {
		LOG("%s: kqueue: %s", tag, strerror(errno));
		goto err_mutex;
	}

	/* Pipe with O_NONBLOCK + O_CLOEXEC on both ends.  The
	 * reffs_pipe_nb_cloexec shim uses pipe2 on platforms that have it
	 * (Linux, FreeBSD) and pipe+fcntl on Darwin. */
	if (reffs_pipe_nb_cloexec(rc->rc_shutdown_pipe) < 0) {
		LOG("%s: pipe: %s", tag, strerror(errno));
		goto err_kq;
	}

	struct kevent ke;

	EV_SET(&ke, rc->rc_shutdown_pipe[0], EVFILT_READ,
	       EV_ADD | EV_CLEAR, 0, 0, NULL);
	if (kevent(rc->rc_kq_fd, &ke, 1, NULL, 0, NULL) < 0) {
		LOG("%s: EVFILT_READ add (shutdown pipe): %s", tag,
		    strerror(errno));
		goto err_pipe;
	}

	TRACE("%s: kqueue=%d shutdown_pipe=(%d,%d)", tag, rc->rc_kq_fd,
	      rc->rc_shutdown_pipe[0], rc->rc_shutdown_pipe[1]);
	return 0;

err_pipe:
	close(rc->rc_shutdown_pipe[0]);
	close(rc->rc_shutdown_pipe[1]);
	rc->rc_shutdown_pipe[0] = rc->rc_shutdown_pipe[1] = -1;
err_kq:
	close(rc->rc_kq_fd);
	rc->rc_kq_fd = -1;
err_mutex:
	pthread_mutex_destroy(&rc->rc_mutex);
	return -1;
}

void kq_teardown(struct ring_context *rc)
{
	if (rc->rc_shutdown_pipe[0] >= 0) {
		close(rc->rc_shutdown_pipe[0]);
		rc->rc_shutdown_pipe[0] = -1;
	}
	if (rc->rc_shutdown_pipe[1] >= 0) {
		close(rc->rc_shutdown_pipe[1]);
		rc->rc_shutdown_pipe[1] = -1;
	}
	if (rc->rc_kq_fd >= 0) {
		close(rc->rc_kq_fd);
		rc->rc_kq_fd = -1;
	}
	pthread_mutex_destroy(&rc->rc_mutex);
}

void io_handler_stop(void)
{
	/*
	 * No per-thread flag to flip here -- each main loop polls its
	 * own volatile sig_atomic_t *running (passed by main()) and
	 * gets woken by io_handler_signal_shutdown() via the pipe.
	 */
}

/*
 * Async-signal-safe shutdown wakeup.  write(2) is on the POSIX list
 * of async-signal-safe functions; kevent(2) is not.  Write a single
 * byte to each ring's shutdown pipe -- the main loop's EVFILT_READ
 * on the pipe fires, the loop observes the updated running flag,
 * and breaks out.  EAGAIN on a full pipe buffer is OK: the first
 * byte already pending delivers the wake-up.
 */
void io_handler_signal_shutdown(void)
{
	struct ring_context *rings[] = { g_network_rc, g_backend_rc };
	static const char wake = 'x';

	for (unsigned i = 0; i < sizeof(rings) / sizeof(rings[0]); i++) {
		struct ring_context *rc = rings[i];

		if (!rc || rc->rc_shutdown_pipe[1] < 0)
			continue;
		(void)write(rc->rc_shutdown_pipe[1], &wake, 1);
	}
}
