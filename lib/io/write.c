/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * io_uring write-submission path.  The completion handler
 * (io_handle_write), reply orchestration (rpc_trans_writer), TLS
 * write glue (io_do_tls), and the io_rpc_trans_cb entry point all
 * live in lib/io/handlers.c -- they are backend-agnostic now that
 * submissions go through the io_resubmit_write primitive defined
 * here.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <arpa/inet.h>
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
#include "reffs/network.h"
#include "reffs/ring.h"
#include "ring_internal.h"
#include "tsan_io.h"
#include "reffs/rpc.h"
#include "trace/io.h"

int io_request_write_op(int fd, char *buf, int len, uint64_t state,
			struct connection_info *ci, struct ring_context *rc)
{
	struct io_uring_sqe *sqe = NULL;
	int ret = 0;

	if (fd <= 0) {
		LOG("Invalid fd: %d", fd);
		return -EBADF;
	}

	struct io_context *ic = io_context_create(OP_TYPE_WRITE, fd, buf, len);
	if (!ic) {
		return -ENOMEM;
	}

	ic->ic_state = state;

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
		io_socket_close(fd, ENOMEM);
		io_context_destroy(ic);
		return -ENOMEM;
	}

	ic->ic_expected_len = len;
	io_uring_prep_write(sqe, fd, buf, len, 0);
	io_uring_sqe_set_data(sqe, ic);

	trace_io_submit_write(fd, ic, len);

	bool submitted = false;
	for (int i = 0; i < REFFS_IO_MAX_RETRIES; i++) {
		ret = io_uring_submit(&rc->rc_ring);
		if (ret >= 0) {
			TSAN_RELEASE(ic);
			submitted = true;
			break;
		} else if (ret == -EAGAIN) {
			LOG("-EAGAIN on write submit (retry %d/%d)", i + 1,
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

	if (!submitted && ret < 0) {
		io_socket_close(fd, -ret);
		io_context_destroy(ic);
	} else {
		ret = 0;
		/*
		 * Do NOT access ic after submit -- the CQE can fire
		 * immediately on the event loop thread, destroying ic
		 * before we reach this line.  Use the fd parameter
		 * (stack-local) for the trace instead.
		 */
		TRACE("write submitted fd=%d len=%d", fd, len);
	}

	return ret;
}

/*
 * io_resubmit_write -- submit the next chunk of ic's write to io_uring.
 *
 * Called by rpc_trans_writer (in handlers.c) so the submission
 * primitive is a per-backend concern while the orchestration stays
 * shared.
 *
 * Returns 0 on successful submission, -errno on permanent failure.
 * On failure ic is destroyed and the socket is closed.
 */
int io_resubmit_write(struct io_context *ic, struct ring_context *rc)
{
	struct io_uring_sqe *sqe = NULL;
	size_t remaining = ic->ic_buffer_len - ic->ic_position;
	uint32_t chunk_size = (remaining > IO_MAX_WRITE_SIZE) ?
				      IO_MAX_WRITE_SIZE :
				      (uint32_t)remaining;
	char *buffer = (char *)ic->ic_buffer + ic->ic_position;
	int ret = 0;

	ic->ic_expected_len = chunk_size;

#ifdef PARTIAL_WRITE_DEBUG
	if (ic->ic_position == 0) {
		uint32_t *p = (uint32_t *)buffer;
		uint32_t marker = ntohl(*p);
		TRACE("First fragment: ic=%p id=%u pos=%zu, remaining=%zu, chunk_size=%u, marker=0x%08x",
		      (void *)ic, ic->ic_id, ic->ic_position, remaining,
		      chunk_size, marker);
	} else {
		TRACE("Continuing fragment: ic=%p id=%u pos=%zu, remaining=%zu, chunk_size=%u",
		      (void *)ic, ic->ic_id, ic->ic_position, remaining,
		      chunk_size);
	}
#endif

	for (int i = 0; i < REFFS_IO_RING_RETRIES; i++) {
		pthread_mutex_lock(&rc->rc_mutex);
		sqe = io_uring_get_sqe(&rc->rc_ring);
		if (sqe)
			break;
		pthread_mutex_unlock(&rc->rc_mutex);
		sched_yield();
	}

	if (!sqe) {
		/*
		 * Cannot get an SQE -- close the socket so the pending write
		 * queue is drained (io_conn_unregister drains ci_write_pending).
		 * The client will reconnect.
		 */
		if (ic->ic_state & IO_CONTEXT_WRITE_OWNED)
			io_socket_close(ic->ic_fd, ENOMEM);
		io_context_destroy(ic);
		return -ENOMEM;
	}

	io_uring_prep_write(sqe, ic->ic_fd, buffer, chunk_size, 0);
	sqe->user_data = (uint64_t)(uintptr_t)ic;

	trace_io_submit_write(ic->ic_fd, ic, chunk_size);

	bool submitted = false;
	for (int i = 0; i < REFFS_IO_MAX_RETRIES; i++) {
		ret = io_uring_submit(&rc->rc_ring);
		if (ret >= 0) {
			TSAN_RELEASE(ic);
			submitted = true;
			break;
		} else if (ret == -EAGAIN) {
			LOG("-EAGAIN on io_resubmit_write (retry %d/%d)", i + 1,
			    REFFS_IO_MAX_RETRIES);
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

	if (!submitted && ret < 0) {
		io_socket_close(ic->ic_fd, -ret);
		io_context_destroy(ic);
		return ret;
	}

	return 0;
}
