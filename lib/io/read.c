/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * io_uring read-submission path.  The completion handler
 * (io_handle_read), TLS handshake / ClientHello probe, and RPC record
 * marker reassembly all live in lib/io/handlers.c -- they are
 * backend-agnostic now that submissions go through the io_resubmit_read
 * primitive defined here.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <errno.h>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "reffs/log.h"
#include "reffs/io.h"
#include "reffs/ring.h"
#include "ring_internal.h"
#include "tsan_io.h"
#include "trace_io.h"

/*
 * io_resubmit_read -- submit another read on ic's fd, reusing ic's
 * buffer.  Called by io_handle_read's "get_more" path; preserves the
 * ic-reuse optimization on the hot read path.
 *
 * Returns 0 on successful submission, -errno on permanent failure.
 */
int io_resubmit_read(struct io_context *ic, struct ring_context *rc)
{
	struct io_uring_sqe *sqe = NULL;
	int ret = 0;

	for (int i = 0; i < REFFS_IO_RING_RETRIES; i++) {
		pthread_mutex_lock(&rc->rc_mutex);
		sqe = io_uring_get_sqe(&rc->rc_ring);
		if (sqe)
			break;
		pthread_mutex_unlock(&rc->rc_mutex);
		sched_yield();
	}

	if (!sqe) {
		return -ENOMEM;
	}

	io_uring_prep_read(sqe, ic->ic_fd, ic->ic_buffer, BUFFER_SIZE, 0);
	sqe->user_data = (uint64_t)(uintptr_t)ic;
	io_context_update_time(ic);

	bool submitted = false;
	for (int i = 0; i < REFFS_IO_MAX_RETRIES; i++) {
		ret = io_uring_submit(&rc->rc_ring);
		if (ret >= 0) {
			TSAN_RELEASE(ic);
			submitted = true;
			break;
		} else if (ret == -EAGAIN) {
			LOG("-EAGAIN in io_resubmit_read (retry %d/%d)", i + 1,
			    REFFS_IO_MAX_RETRIES);
			ic->ic_state |= IO_CONTEXT_SUBMITTED_EAGAIN;
			trace_io_eagain(ic, __func__, __LINE__);
			pthread_mutex_unlock(&rc->rc_mutex);
			sched_yield();
			pthread_mutex_lock(&rc->rc_mutex);
		} else
			break;
	}
	pthread_mutex_unlock(&rc->rc_mutex);

	return (submitted || ret == 0) ? 0 : ret;
}

int io_request_read_op(int fd, struct connection_info *ci,
		       struct ring_context *rc)
{
	struct io_uring_sqe *sqe = NULL;
	int ret = 0;

	if (fd <= 0) {
		LOG("Invalid fd: %d", fd);
		return -EINVAL;
	}

	char *buffer = malloc(BUFFER_SIZE);
	if (!buffer) {
		LOG("Failed to allocate buffer");
		io_socket_close(fd, ENOMEM);
		return -ENOMEM;
	}

	struct io_context *ic =
		io_context_create(OP_TYPE_READ, fd, buffer, BUFFER_SIZE);
	if (!ic) {
		LOG("Failed to create read context");
		free(buffer);
		io_socket_close(fd, ENOMEM);
		return -ENOMEM;
	}

	if (ci)
		copy_connection_info(&ic->ic_ci, ci);

	for (int i = 0; i < REFFS_IO_RING_RETRIES; i++) {
		pthread_mutex_lock(&rc->rc_mutex);
		sqe = io_uring_get_sqe(&rc->rc_ring);
		if (sqe)
			break;
		pthread_mutex_unlock(&rc->rc_mutex);
		sched_yield();
	}

	if (!sqe) {
		free(buffer);
		io_socket_close(fd, ENOMEM);
		io_context_destroy(ic);
		return -ENOMEM;
	}

	io_uring_prep_read(sqe, fd, buffer, BUFFER_SIZE, 0);
	sqe->user_data = (uint64_t)(uintptr_t)ic;

	TRACE("SUBMIT READ: fd=%d ic=%p id=%u", fd, (void *)ic, ic->ic_id);

	bool submitted = false;
	for (int i = 0; i < REFFS_IO_MAX_RETRIES; i++) {
		ret = io_uring_submit(&rc->rc_ring);
		if (ret >= 0) {
			TSAN_RELEASE(ic);
			submitted = true;
			break;
		} else if (ret == -EAGAIN) {
			LOG("-EAGAIN in io_request_read_op (retry %d/%d)",
			    i + 1, REFFS_IO_MAX_RETRIES);
			ic->ic_state |= IO_CONTEXT_SUBMITTED_EAGAIN;
			trace_io_eagain(ic, __func__, __LINE__);
			pthread_mutex_unlock(&rc->rc_mutex);
			sched_yield();
			pthread_mutex_lock(&rc->rc_mutex);
		} else
			break;
	}
	pthread_mutex_unlock(&rc->rc_mutex);

	if (!submitted && ret < 0) {
		free(buffer);
		io_socket_close(fd, -ret);
		io_context_destroy(ic);
		return ret;
	}

	return 0;
}
