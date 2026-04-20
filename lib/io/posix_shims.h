/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Portability shims for POSIX extensions not available on all
 * targets.  reffs uses these primarily for socket and pipe setup
 * where the native atomic-flag forms avoid a fork+exec race
 * window.  On platforms without them (Darwin), fall back to the
 * base call plus an immediate fcntl(2) to set the flags.
 *
 * Called during init paths only (accept loop arming, shutdown
 * pipe creation), never on the hot path.
 */

#ifndef _REFFS_POSIX_SHIMS_H
#define _REFFS_POSIX_SHIMS_H

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * reffs_accept4_nb_cloexec -- accept(2) with the resulting fd
 * set non-blocking and close-on-exec.
 *
 * Return value matches accept4(2): new fd on success, -1 with
 * errno on failure.
 */
static inline int reffs_accept4_nb_cloexec(int sockfd, struct sockaddr *addr,
					   socklen_t *addrlen)
{
#ifdef HAVE_ACCEPT4
	return accept4(sockfd, addr, addrlen, SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
	int fd = accept(sockfd, addr, addrlen);
	if (fd < 0)
		return fd;

	/*
	 * Race window between accept(2) and the fcntl(2) calls below
	 * would leak the fd into a concurrent fork+exec.  reffsd does
	 * not fork, so exposure is zero; documented here so no reader
	 * adds exec(3) paths without reconsidering.
	 */
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
		int saved = errno;
		(void)close(fd);
		errno = saved;
		return -1;
	}
	int fdflags = fcntl(fd, F_GETFD, 0);
	if (fdflags < 0 || fcntl(fd, F_SETFD, fdflags | FD_CLOEXEC) < 0) {
		int saved = errno;
		(void)close(fd);
		errno = saved;
		return -1;
	}
	return fd;
#endif
}

/*
 * reffs_pipe_nb_cloexec -- pipe(2) with both ends set
 * non-blocking and close-on-exec.
 *
 * Returns 0 on success, -1 with errno on failure.  On failure
 * both ends are closed; do not attempt to use fds[0] or fds[1].
 */
static inline int reffs_pipe_nb_cloexec(int fds[2])
{
#ifdef HAVE_PIPE2
	return pipe2(fds, O_NONBLOCK | O_CLOEXEC);
#else
	if (pipe(fds) < 0)
		return -1;

	/* Same fork+exec race note as above.  Single-threaded
	 * caller (io_handler_init) so no exposure. */
	for (int i = 0; i < 2; i++) {
		int flags = fcntl(fds[i], F_GETFL, 0);
		if (flags < 0 ||
		    fcntl(fds[i], F_SETFL, flags | O_NONBLOCK) < 0) {
			int saved = errno;
			(void)close(fds[0]);
			(void)close(fds[1]);
			errno = saved;
			return -1;
		}
		int fdflags = fcntl(fds[i], F_GETFD, 0);
		if (fdflags < 0 ||
		    fcntl(fds[i], F_SETFD, fdflags | FD_CLOEXEC) < 0) {
			int saved = errno;
			(void)close(fds[0]);
			(void)close(fds[1]);
			errno = saved;
			return -1;
		}
	}
	return 0;
#endif
}

#endif /* _REFFS_POSIX_SHIMS_H */
