/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <errno.h>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "reffs/log.h"
#include "reffs/io.h"
#include "reffs/log.h"
#include "reffs/network.h"
#include "reffs/ring.h"
#include "ring_internal.h"
#include "tsan_io.h"
#include "trace_io.h"

struct accept_context {
	struct sockaddr_storage ac_addr;
	socklen_t ac_addrlen;
};

int io_request_accept_op(int fd, struct connection_info *ci,
			 struct ring_context *rc)
{
	struct io_uring_sqe *sqe = NULL;
	int ret = 0;
	int retry_count = 0;
	const int max_retries = 5;

	// Register the listening socket if not already tracked
	struct conn_info *conn = io_conn_get(fd);
	if (!conn) {
		conn = io_conn_register(fd, CONN_LISTENING, CONN_ROLE_SERVER);
		if (!conn) {
			LOG("Failed to register listener socket fd=%d", fd);
			// Continue anyway - this is just tracking
		}
	}

	// Use a retry loop instead of recursion
	while (retry_count < max_retries) {
		struct accept_context *actx =
			calloc(1, sizeof(struct accept_context));
		if (!actx) {
			LOG("Failed to allocate buffer for accept - retry %d/%d",
			    retry_count + 1, max_retries);

			// Sleep before retry
			usleep(100000); // 100ms
			retry_count++;
			continue;
		}

		actx->ac_addrlen = sizeof(struct sockaddr_storage);

		struct io_context *ic = io_context_create(OP_TYPE_ACCEPT, fd,
							  actx, sizeof(*actx));
		if (!ic) {
			LOG("Failed to create accept context - retry %d/%d",
			    retry_count + 1, max_retries);
			free(actx);

			// Sleep before retry
			usleep(100000); // 100ms
			retry_count++;
			continue;
		}

		if (ci)
			copy_connection_info(&ic->ic_ci, ci);

		// Flag to track if we successfully submitted the request
		bool submitted = false;

		for (int i = 0; i < REFFS_IO_RING_RETRIES; i++) {
			pthread_mutex_lock(&rc->rc_mutex);
			sqe = io_uring_get_sqe(&rc->rc_ring);
			if (sqe)
				break;
			pthread_mutex_unlock(&rc->rc_mutex);
			sched_yield();
		}

		if (!sqe) {
			LOG("Failed to get SQE for accept - retry %d/%d",
			    retry_count + 1, max_retries);
			io_context_destroy(ic);

			// Sleep before retry
			usleep(100000); // 100ms
			retry_count++;
			continue;
		}

		io_uring_prep_accept(sqe, fd, (struct sockaddr *)&actx->ac_addr,
				     &actx->ac_addrlen, 0);
		sqe->user_data = (uint64_t)(uintptr_t)ic;

		trace_io_accept_submit(ic);

		for (int i = 0; i < REFFS_IO_MAX_RETRIES; i++) {
			ret = io_uring_submit(&rc->rc_ring);
			if (ret >= 0) {
				TSAN_RELEASE(ic);
				submitted = true;
				break;
			} else if (ret == -EAGAIN) {
				LOG("-EAGAIN in io_request_accept_op (retry %d/%d)",
				    i + 1, REFFS_IO_MAX_RETRIES);
				ic->ic_state |= IO_CONTEXT_SUBMITTED_EAGAIN;
				trace_io_eagain(ic, __func__, __LINE__);
				pthread_mutex_unlock(&rc->rc_mutex);
				sched_yield();
				pthread_mutex_lock(&rc->rc_mutex);
			} else {
				break;
			}
		}
		pthread_mutex_unlock(&rc->rc_mutex);

		if (!submitted) {
			LOG("Failed to submit accept operation - retry %d/%d: %s",
			    retry_count + 1, max_retries, strerror(-ret));
			io_context_destroy(ic);

			// Sleep before retry
			usleep(100000); // 100ms
			retry_count++;
			continue;
		}

		// Success!
		return 0;
	}

	// If we get here, we've exhausted all retries
	LOG("CRITICAL: Failed to submit accept operation after %d retries",
	    max_retries);

	// Instead of giving up completely, schedule a retry through the watchdog mechanism
	// by setting the last_accept_check time to a value that will trigger a check soon
	if (conn) {
		conn->ci_state = CONN_ERROR;
		conn->ci_error = (ret < 0) ? -ret : EAGAIN;
	}

	return (ret < 0) ? -ret : EAGAIN;
}
