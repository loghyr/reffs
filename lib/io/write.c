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

static int rpc_trans_writer(struct io_context *ic, struct io_uring *ring);

/*
 * Returns < 1 for error
 * 0 for no need for further processing
 * 1 for using kTLS and fall through.
 */
static int io_do_tls(struct io_context *ic, struct io_uring *ring)
{
	struct conn_info *ci = io_conn_get(ic->ic_fd);

	// For TLS connections, we might need to do userspace TLS
	size_t remaining = ic->ic_buffer_len - ic->ic_position;

	LOG("ic=%p fd=%d type=%s bl=%zu id=%u", (void *)ic, ic->ic_fd,
	    io_op_type_to_str(ic->ic_op_type), ic->ic_buffer_len, ic->ic_id);

	if (remaining == 0) {
		io_context_destroy(ic);
		return 0;
	}

	// Write directly using TLS if not using kTLS
	int ktls_enabled = 0;
#ifdef BIO_get_ktls_send
	ktls_enabled = BIO_get_ktls_send(SSL_get_wbio(ci->ci_ssl));
#endif

	LOG("ktls_enabled=%d", ktls_enabled);
	LOG("ic=%p fd=%d type=%s bl=%ld id=%u", (void *)ic, ic->ic_fd,
	    io_op_type_to_str(ic->ic_op_type), ic->ic_buffer_len, ic->ic_id);
	rpc_log_packet("TLS: ", ic->ic_buffer, ic->ic_buffer_len);
	if (!ktls_enabled) {
		// Handle in userspace
		int ret = SSL_write(ci->ci_ssl,
				    (char *)ic->ic_buffer + ic->ic_position,
				    remaining);

		if (ret <= 0) {
			int err = SSL_get_error(ci->ci_ssl, ret);
			if (err == SSL_ERROR_WANT_WRITE ||
			    err == SSL_ERROR_WANT_READ) {
				// Would block, try again later
				return 0;
			}

			// Real error
			io_ssl_err_print(ic->ic_fd, "write error", __func__,
					 __LINE__);
			io_socket_close(ic->ic_fd, EINVAL);
			io_context_destroy(ic);
			return -EINVAL;
		} else {
			// Get data that was encrypted and is ready to be sent
			BIO *wbio = SSL_get_wbio(ci->ci_ssl);
			int pending = BIO_pending(wbio);

			LOG("SSL_write processed %d bytes, resulting in %d bytes of TLS data",
			    ret, pending);

			if (pending > 0) {
				// Read the encrypted data from the BIO
				char *encrypted_data = malloc(pending);
				if (encrypted_data) {
					int bytes_read = BIO_read(
						wbio, encrypted_data, pending);
					LOG("Read %d bytes of encrypted data from BIO",
					    bytes_read);

					// Log first few bytes of the encrypted data
					if (bytes_read > 0) {
						LOG("First 16 bytes of TLS record: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
						    (unsigned char)
							    encrypted_data[0],
						    (unsigned char)
							    encrypted_data[1],
						    (unsigned char)
							    encrypted_data[2],
						    (unsigned char)
							    encrypted_data[3],
						    (unsigned char)
							    encrypted_data[4],
						    (unsigned char)
							    encrypted_data[5],
						    (unsigned char)
							    encrypted_data[6],
						    (unsigned char)
							    encrypted_data[7],
						    (unsigned char)
							    encrypted_data[8],
						    (unsigned char)
							    encrypted_data[9],
						    (unsigned char)
							    encrypted_data[10],
						    (unsigned char)
							    encrypted_data[11],
						    (unsigned char)
							    encrypted_data[12],
						    (unsigned char)
							    encrypted_data[13],
						    (unsigned char)
							    encrypted_data[14],
						    (unsigned char)
							    encrypted_data[15]);
					}

					free(encrypted_data);
				}
			}
		}

		// Successfully wrote data
		ic->ic_position += ret;

		// If more data to write, call ourselves again
		if (ic->ic_position < ic->ic_buffer_len) {
			return rpc_trans_writer(ic, ring);
		}

		// All data written
		io_context_destroy(ic);
		return 0;
	}

	return 1;
}

int io_request_write_op(int fd, char *buf, int len, uint64_t state,
			struct connection_info *ci, struct io_uring *ring)
{
	struct io_uring_sqe *sqe = NULL;
	int ret = 0;

	if (fd <= 0 || fd >= MAX_CONNECTIONS) {
		LOG("Invalid fd: %d", fd);
		return -EINVAL;
	}

	struct io_context *ic = io_context_create(OP_TYPE_WRITE, fd, buf, len);
	if (!ic) {
		return -ENOMEM;
	}

	ic->ic_state = state;

	if (ci)
		copy_connection_info(&ic->ic_ci, ci);

	for (int i = 0; i < REFFS_IO_MAX_RETRIES; i++) {
		sqe = io_uring_get_sqe(ring);
		if (sqe)
			break;
		usleep(IO_URING_WAIT_US);
	}

	if (!sqe) {
		io_socket_close(fd, ENOMEM);
		io_context_destroy(ic);
		return -ENOMEM;
	}

	io_uring_prep_write(sqe, fd, buf, len, 0);
	io_uring_sqe_set_data(sqe, ic); // loghyr - fix this everywhere

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
		io_socket_close(fd, -ret);
		io_context_destroy(ic);
	} else {
		ret = 0;
		LOG("ic=%p fd=%d type=%s bl=%ld id=%u", (void *)ic, ic->ic_fd,
		    io_op_type_to_str(ic->ic_op_type), ic->ic_buffer_len,
		    ic->ic_id);
	}

	return 0;
}

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

	trace_io_writer(ic, __func__, __LINE__);

	struct conn_info *ci = io_conn_get(ic->ic_fd);
	if (ci && ci->ci_ssl && ci->ci_tls_enabled) {
		ret = io_do_tls(ic, ring);
		if (ret <= 0)
			return ret;
	}

	// If no more data to send, we're done
	if (remaining == 0) {
		io_context_destroy(ic);
		return 0;
	} else if (remaining < 0) {
		// Error case - shouldn't happen with correct position tracking
		io_socket_close(ic->ic_fd, EINVAL);
		io_context_destroy(ic);
		return 0;
	}

	// Determine if this is the last fragment to send
	bool last_fragment = (remaining <= IO_MAX_WRITE_SIZE);

	// Calculate size for this fragment
	uint32_t chunk_size;
	char *buffer;
	uint32_t *p;

	if (ic->ic_position == 0) {
		// First fragment - record marker is already in the buffer
		chunk_size = remaining > IO_MAX_WRITE_SIZE ? IO_MAX_WRITE_SIZE :
							     remaining;
		buffer = (char *)ic->ic_buffer;
	} else {
		// Calculate chunk size: either IO_MAX_WRITE_SIZE or remaining + 4 bytes for marker
		chunk_size = remaining > (IO_MAX_WRITE_SIZE - 4) ?
				     IO_MAX_WRITE_SIZE :
				     (remaining + 4);

		// Subsequent fragments - we need to reuse the preceding 4 bytes for the record marker
		buffer = (char *)ic->ic_buffer + (ic->ic_position - 4);

		ic->ic_count++;
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
		io_context_destroy(ic);
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

	ci = io_conn_get(ic->ic_fd);
	if (ci) {
		ci->ci_last_activity = time(NULL);
		LOG("ci=%p th=%d tls=%d ssl=%p", (void *)ci,
		    ci->ci_tls_handshaking, ci->ci_tls_enabled,
		    (void *)ci->ci_ssl);
	}

	// Submit the write operation
	io_uring_prep_write(sqe, ic->ic_fd, buffer, chunk_size, 0);
	sqe->user_data = (uint64_t)(uintptr_t)ic;
	trace_io_write_submit(ic);
	io_context_update_time(ic);

	for (int i = 0; i < REFFS_IO_MAX_RETRIES; i++) {
		ret = io_uring_submit(ring);
		if (ret >= 0)
			break;
		if (ret == -EAGAIN) {
			usleep(IO_URING_WAIT_US);
			LOG("%d", ret);
			ret = 0;
			break; // Right now we don't know what io_uring is doing!
		} else
			break;
	}

	LOG("%d", ret);
	if (ret < 0) {
		io_socket_close(ic->ic_fd, -ret);
		io_context_destroy(ic);
	} else {
		ret = 0;
	}

	return 0;
}

int io_rpc_trans_cb(struct rpc_trans *rt)
{
	struct io_context *ic;

	LOG("%p", (void *)rt);

	struct conn_info *ci = io_conn_get(rt->rt_fd);
	if (!ci) {
		LOG("Connection not tracked for fd=%d", rt->rt_fd);
		return ENOTCONN;
	}

	LOG("ci=%p th=%d tls=%d ssl=%p", (void *)ci, ci->ci_tls_handshaking,
	    ci->ci_tls_enabled, (void *)ci->ci_ssl);

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

int io_handle_write(struct io_context *ic, int bytes_written,
		    struct io_uring *ring)
{
	// Check connection state
	struct conn_info *ci = io_conn_get(ic->ic_fd);

	// Verify we wrote the expected amount
	if (bytes_written <= 0) {
		LOG("Write operation failed for fd=%d: %s", ic->ic_fd,
		    bytes_written < 0 ? strerror(-bytes_written) :
					"connection closed");

		io_socket_close(ic->ic_fd, bytes_written < 0 ? -bytes_written :
							       ECONNRESET);
		io_context_destroy(ic);
		return bytes_written < 0 ? -bytes_written : ECONNRESET;
	}

	// Update connection activity
	if (ci) {
		ci->ci_last_activity = time(NULL);

		// Check if this was the final handshake message
		if (ci->ci_handshake_final_pending) {
			LOG("Final TLS handshake message sent for fd=%d",
			    ic->ic_fd);

			// Clear the pending flag
			ci->ci_handshake_final_pending = false;

			// NOW we can enable TLS mode
			ci->ci_tls_enabled = true;

			// Make sure any buffered BIO data is flushed
			BIO_flush(SSL_get_wbio(ci->ci_ssl));

			LOG("TLS mode now active for fd=%d", ic->ic_fd);

// Log kTLS status
#ifdef BIO_get_ktls_send
			int ktls_send =
				BIO_get_ktls_send(SSL_get_wbio(ci->ci_ssl));
			int ktls_recv =
				BIO_get_ktls_recv(SSL_get_rbio(ci->ci_ssl));
			LOG("kTLS status for fd=%d: send=%d, recv=%d",
			    ic->ic_fd, ktls_send, ktls_recv);
#endif
		}
	}

	// If this was direct TLS data, we're done - no need to continue with normal write processing
	if (ic->ic_state & IO_CONTEXT_DIRECT_TLS_DATA) {
		LOG("Direct TLS data sent, skipping io_do_tls");
		io_context_destroy(ic);
		return 0;
	}

	// For TLS connections, handle encryption before further processing
	if (ci && ci->ci_ssl && ci->ci_tls_enabled) {
		int ret = io_do_tls(ic, ring);
		if (ret <= 0)
			return ret;
	}

	// Continue with normal write processing for application data
	return rpc_trans_writer(ic, ring);
}
