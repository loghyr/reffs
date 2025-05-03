/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <liburing.h>
#include <fcntl.h>
#include <pthread.h>
#include <errno.h>
#include <urcu.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <time.h>
#include <stdbool.h>

#include "reffs/log.h"
#include "reffs/rpc.h"
#include "reffs/network.h"
#include "reffs/server.h"
#include "reffs/task.h"
#include "reffs/test.h"
#include "reffs/io.h"
#include "reffs/trace/io.h"

// Maximum size for a single write
#define MAX_WRITE_SIZE (1024 * 1024)

/*
 * Creating responses:
 *
 * rpc_process_task() calls io_rpc_trans_cb() which creates an io_context.
 *
 * io_rpc_trans_cb() calls rpc_trans_writer() which in turn submits
 * the context to io_uring_submit().
 *
 * When op_type_write() is called after io_uring_wait_cqe_timeout()
 * wakes up with the CQE, it also invokes rpc_trans_writer().
 *
 * At this point, rpc_trans_writer() needs to either end the
 * recursion because of either it is done or because of an
 * error.
 *
 * If it does not end the recursion, then it advances to the
 * next fragment and submits it back to io_uring_submit().
 *
 * At no point are there multiple instances of this io_context
 * submitted to io_uring. The sequential nature of rpc_trans_writer()
 * ensures that we avoid memory allocations and we avoid parallel
 * access to the io_context.
 *
 * Note: the first 4 bytes of the orginal buffer are for the
 * record marker. After the first fragment is sent, we use
 * the last 4 bytes of the previous fragment to store the
 * record marker for the current fragment.
 *
 */
static int rpc_trans_writer(struct io_context *ic, struct io_uring *ring)
{
	struct io_uring_sqe *sqe;
	size_t remaining = ic->ic_buffer_len - ic->ic_position;
	int ret = 0;

	struct conn_info *conn = io_conn_get(ic->ic_fd);

	// If no more data to send, we're done
	if (remaining == 0) {
		if (conn) {
			io_conn_remove_write_op(ic->ic_fd);
		}
		io_context_free(ic);
		return 0;
	} else if (remaining < 0) {
		// Error case - shouldn't happen with correct position tracking
		io_socket_close(ic->ic_fd, EINVAL);
		io_context_free(ic);
		return 0;
	}

	// Determine if this is the last fragment to send
	bool last_fragment = (remaining <= MAX_WRITE_SIZE);

	// Calculate size for this fragment
	uint32_t chunk_size;
	char *buffer;
	uint32_t *p;

	if (ic->ic_position == 0) {
		// First fragment - record marker is already in the buffer
		chunk_size = remaining > MAX_WRITE_SIZE ? MAX_WRITE_SIZE :
							  remaining;
		buffer = (char *)ic->ic_buffer;
	} else {
		// Calculate chunk size: either MAX_WRITE_SIZE or remaining + 4 bytes for marker
		chunk_size = remaining > (MAX_WRITE_SIZE - 4) ? MAX_WRITE_SIZE :
								(remaining + 4);

		// Subsequent fragments - we need to reuse the preceding 4 bytes for the record marker
		buffer = (char *)ic->ic_buffer + (ic->ic_position - 4);
	}

	// Set the record marker to reflect the payload size (excluding the marker itself)
	p = (uint32_t *)buffer;
	*p = htonl((last_fragment ? 0x80000000 : 0) | (chunk_size - 4));

	for (int i = 0; i < REFFS_IO_MAX_RETRIES; i++) {
		sqe = io_uring_get_sqe(ring);
		if (sqe)
			break;
		usleep(IO_URING_WAIT_US);
	}

	if (!sqe) {
		io_socket_close(ic->ic_fd, ENOBUFS);
		io_context_free(ic);
		return ENOMEM;
	}

	// Update position for next fragment
	if (ic->ic_position == 0) {
		// After first fragment, position points to end of this chunk
		ic->ic_position += chunk_size;
	} else {
		// For subsequent fragments, position points to end of payload (excluding marker)
		ic->ic_position += (chunk_size - 4);
	}

	if (conn) {
		conn->ci_last_activity = time(NULL);
	}

	// Submit the write operation
	io_uring_prep_write(sqe, ic->ic_fd, buffer, chunk_size, 0);
	sqe->user_data = (uint64_t)(uintptr_t)ic;
	trace_io_write_submit(ic);

	for (int i = 0; i < REFFS_IO_MAX_RETRIES; i++) {
		ret = io_uring_submit(ring);
		if (ret >= 0)
			break;
		if (ret == -EAGAIN) {
			usleep(IO_URING_WAIT_US);
			ret = 0;
			break; // Right now we don't know what io_uring is doing!
		} else
			break;
	}

	if (ret < 0) {
		io_socket_close(ic->ic_fd, -ret);
		io_context_free(ic);
	} else {
		ret = 0;
	}

	return 0;
}

int io_rpc_trans_cb(struct rpc_trans *rt)
{
	struct io_context *ic;

	struct conn_info *conn = io_conn_get(rt->rt_fd);
	if (!conn) {
		LOG("Connection not tracked for fd=%d", rt->rt_fd);
		return ENOTCONN;
	}

	io_conn_dump(rt->rt_fd);

	io_conn_add_write_op(rt->rt_fd);

	ic = io_context_create(OP_TYPE_WRITE, rt->rt_fd, rt->rt_reply,
			       rt->rt_reply_len);
	if (!ic) {
		LOG("Failed to create write context");
		return 0;
	}

	ic->ic_xid = rt->rt_info.ri_xid;
	copy_connection_info(&ic->ic_ci, &rt->rt_info.ri_ci);

	rt->rt_reply = NULL;

	return rpc_trans_writer(ic, rt->rt_ring);
}

int io_handle_write(struct io_context *ic,
		    int __attribute__((unused)) bytes_written,
		    struct io_uring *ring)
{
	// Check connection state
	struct conn_info *conn = io_conn_get(ic->ic_fd);

	// Verify we wrote the expected amount
	if (bytes_written <= 0) {
		LOG("Write operation failed for fd=%d: %s", ic->ic_fd,
		    bytes_written < 0 ? strerror(-bytes_written) :
					"connection closed");

		io_socket_close(ic->ic_fd, bytes_written < 0 ? -bytes_written :
							       ECONNRESET);
		io_context_free(ic);
		return bytes_written < 0 ? -bytes_written : ECONNRESET;
	}

	// Update connection activity
	if (conn) {
		conn->ci_last_activity = time(NULL);
	}

	return rpc_trans_writer(ic, ring);
}
