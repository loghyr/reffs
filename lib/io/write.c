/* Also update rpc_trans_writer to handle multi-fragment messages correctly */

/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <openssl/types.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "reffs/log.h"
#include "reffs/io.h"
#include "reffs/log.h"
#include "reffs/network.h"
#include "reffs/ring.h"
#include "reffs/rpc.h"
#include "reffs/tls.h"
#include "reffs/trace/io.h"

static int rpc_trans_writer(struct io_context *ic, struct ring_context *rc);

/*
 * Returns < 1 for error
 * 0 for no need for further processing
 * 1 for using kTLS and fall through.
 */
static int io_do_tls(struct io_context *ic, struct ring_context *rc)
{
	struct conn_info *ci = io_conn_get(ic->ic_fd);
	size_t remaining = ic->ic_buffer_len - ic->ic_position;

	TRACE("ic=%p fd=%d type=%s bl=%zu id=%u", (void *)ic, ic->ic_fd,
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

	TRACE("ic=%p fd=%d type=%s bl=%ld id=%u", (void *)ic, ic->ic_fd,
	      io_op_type_to_str(ic->ic_op_type), ic->ic_buffer_len, ic->ic_id);
	if (!ktls_enabled) {
		// Check if we've already processed this context for TLS
		if (ic->ic_state & IO_CONTEXT_TLS_BIO_PROCESSED) {
			TRACE("ic=%p id=%u already processed for TLS, destroying",
			      (void *)ic, ic->ic_id);
			io_context_destroy(ic);
			return 0;
		}

		// Handle in userspace
		rpc_log_packet("TLS: ", ic->ic_buffer, ic->ic_buffer_len);
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

			TRACE("SSL_write processed %d bytes, resulting in %d bytes of TLS data",
			      ret, pending);

			// Mark that we've processed this context for TLS to avoid duplicates
			ic->ic_state |= IO_CONTEXT_TLS_BIO_PROCESSED;

			// Read the encrypted data from the BIO and send it
			if (pending > 0) {
				unsigned char *write_buffer = malloc(pending);
				if (!write_buffer) {
					LOG("Failed to allocate memory for TLS data");
					io_socket_close(ic->ic_fd, ENOMEM);
					io_context_destroy(ic);
					return -ENOMEM;
				}

				int bytes =
					BIO_read(wbio, write_buffer, pending);
				TRACE("Read %d bytes of encrypted data from BIO",
				      bytes);

				if (bytes > 0) {
					ret = io_request_write_op(
						ic->ic_fd, (char *)write_buffer,
						bytes,
						IO_CONTEXT_DIRECT_TLS_DATA,
						&ic->ic_ci, rc);

					if (ret != 0) {
						LOG("Failed to submit TLS data write: %d",
						    ret);
						free(write_buffer);
						io_socket_close(ic->ic_fd,
								-ret);
						io_context_destroy(ic);
						return ret;
					}

					TRACE("Submitted %d bytes of TLS data via io_request_write_op",
					      bytes);
				} else {
					free(write_buffer);
				}
			}
		}

		io_context_destroy(ic);
		return 0;
	}

	return 1;
}

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
		TRACE("ic=%p fd=%d type=%s bl=%ld id=%u", (void *)ic, ic->ic_fd,
		      io_op_type_to_str(ic->ic_op_type), ic->ic_buffer_len,
		      ic->ic_id);
	}

	return ret;
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
static int rpc_trans_writer(struct io_context *ic, struct ring_context *rc)
{
	struct io_uring_sqe *sqe;
	size_t remaining;
	int ret = 0;

	// Check if we're done first
	if (ic->ic_position >= ic->ic_buffer_len) {
#ifdef PARTIAL_WRITE_DEBUG
		TRACE("Buffer complete: ic=%p id=%u position=%zu, buffer_len=%zu",
		      (void *)ic, ic->ic_id, ic->ic_position,
		      ic->ic_buffer_len);
#endif
		trace_io_write_complete(ic->ic_fd, 0, ic);
		io_context_destroy(ic);
		return 0;
	}

	remaining = ic->ic_buffer_len - ic->ic_position;

	trace_io_writer(ic, __func__, __LINE__);

	struct conn_info *ci = io_conn_get(ic->ic_fd);
#ifdef TLS_DEBUGGING
	TRACE("ci=%p ssl=%p tls=%d", (void *)ci, ci ? (void *)ci->ci_ssl : NULL,
	      ci ? ci->ci_tls_enabled : 0);
#endif
	if (ci && ci->ci_ssl && ci->ci_tls_enabled) {
		ret = io_do_tls(ic, rc);
		if (ret <= 0)
			return ret;
	}

	// Always continue from current position without modifying the buffer
	uint32_t chunk_size =
		(remaining > IO_MAX_WRITE_SIZE) ? IO_MAX_WRITE_SIZE : remaining;
	char *buffer = (char *)ic->ic_buffer + ic->ic_position;

	// Log what we're doing
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

	// Get SQE and submit
	for (int i = 0; i < REFFS_IO_RING_RETRIES; i++) {
		pthread_mutex_lock(&rc->rc_mutex);
		sqe = io_uring_get_sqe(&rc->rc_ring);
		if (sqe)
			break;
		pthread_mutex_unlock(&rc->rc_mutex);
		sched_yield();
	}

	if (!sqe) {
		io_context_destroy(ic);
		return ENOMEM;
	}

	ic->ic_expected_len = chunk_size;
	io_uring_prep_write(sqe, ic->ic_fd, buffer, chunk_size, 0);
	sqe->user_data = (uint64_t)(uintptr_t)ic;

	trace_io_submit_write(ic->ic_fd, ic, chunk_size);

	bool submitted = false;
	for (int i = 0; i < REFFS_IO_MAX_RETRIES; i++) {
		ret = io_uring_submit(&rc->rc_ring);
		if (ret >= 0) {
			submitted = true;
			break;
		} else if (ret == -EAGAIN) {
			LOG("-EAGAIN on rpc_trans_writer (retry %d/%d)", i + 1,
			    REFFS_IO_MAX_RETRIES);
			ic->ic_state |= IO_CONTEXT_SUBMITTED_EAGAIN;
			trace_io_eagain(ic, __func__, __LINE__);
			pthread_mutex_unlock(&rc->rc_mutex);
			sched_yield();
			pthread_mutex_lock(&rc->rc_mutex);
		}
	}
	pthread_mutex_unlock(&rc->rc_mutex);

	if (!submitted && ret < 0) {
		io_socket_close(ic->ic_fd, -ret);
		io_context_destroy(ic);
	} else {
		ret = 0;
	}

	return ret;
}

int io_rpc_trans_cb(struct rpc_trans *rt)
{
	struct io_context *ic;

	struct conn_info *ci = io_conn_get(rt->rt_fd);
	if (!ci) {
		LOG("Connection not tracked for fd=%d", rt->rt_fd);
		return ENOTCONN;
	}

#ifdef TLS_DEBUGGING
	TRACE("ci=%p th=%d tls=%d ssl=%p", (void *)ci, ci->ci_tls_handshaking,
	      ci->ci_tls_enabled, (void *)ci->ci_ssl);
#endif

	ic = io_context_create(OP_TYPE_WRITE, rt->rt_fd, rt->rt_reply,
			       rt->rt_reply_len);
	if (!ic) {
		ic = io_context_create(OP_TYPE_WRITE, rt->rt_fd, rt->rt_reply,
				       rt->rt_reply_len);
		LOG("Failed to create write context");
		return 0;
	}

	ic->ic_xid = rt->rt_info.ri_xid;
	copy_connection_info(&ic->ic_ci, &rt->rt_info.ri_ci);

	if (rt->rt_reply_len >= 4) {
		uint32_t *marker_ptr = (uint32_t *)rt->rt_reply;
		uint32_t data_len = rt->rt_reply_len - 4;

		// Always set the last fragment bit for the complete message
		// The message is already properly formed - don't try to fragment it
		*marker_ptr = htonl(0x80000000 | data_len);
#ifdef PARTIAL_WRITE_DEBUG
		TRACE("RPC message: setting marker for %u bytes", data_len);
#endif
	}

	rt->rt_reply = NULL;

	return rpc_trans_writer(ic, rt->rt_rc);
}

int io_handle_write(struct io_context *ic, int bytes_written,
		    struct ring_context *rc)
{
	trace_io_write_complete(ic->ic_fd, bytes_written, ic);

	// Check connection state
	struct conn_info *ci = io_conn_get(ic->ic_fd);

	if (bytes_written > 0 && ic->ic_position == 0) {
		TRACE("WRITE START: ic=%p id=%u fd=%d starting fresh write.",
		      (void *)ic, ic->ic_id, ic->ic_fd);
	}

	// Verify we wrote the expected amount
	if (bytes_written <= 0) {
		LOG("Write operation failed for %p fd=%d: %s", (void *)ic,
		    ic->ic_fd,
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
			TRACE("Final TLS handshake message sent for fd=%d",
			      ic->ic_fd);

			// Clear the pending flag
			ci->ci_handshake_final_pending = false;

			// NOW we can enable TLS mode
			ci->ci_tls_enabled = true;

			// Make sure any buffered BIO data is flushed
			BIO_flush(SSL_get_wbio(ci->ci_ssl));

			TRACE("TLS mode now active for fd=%d", ic->ic_fd);

// Log kTLS status
#ifdef BIO_get_ktls_send
			int ktls_send =
				BIO_get_ktls_send(SSL_get_wbio(ci->ci_ssl));
			int ktls_recv =
				BIO_get_ktls_recv(SSL_get_rbio(ci->ci_ssl));
			TRACE("kTLS status for fd=%d: send=%d, recv=%d",
			      ic->ic_fd, ktls_send, ktls_recv);
#endif
		}
	}

	// If this was direct TLS data, we're done - no need to continue with normal write processing
	if (ic->ic_state & IO_CONTEXT_DIRECT_TLS_DATA) {
		TRACE("Direct TLS data sent for fd=%d", ic->ic_fd);

		// Check if this was part of a TLS handshake
		struct conn_info *ci = io_conn_get(ic->ic_fd);
		if (ci && ci->ci_handshake_final_pending) {
			TRACE("Final TLS handshake message sent for fd=%d",
			      ic->ic_fd);

			// Clear the pending flag
			ci->ci_handshake_final_pending = false;

			// NOW we can enable TLS mode
			// ci->ci_tls_enabled = true;

			// Make sure any buffered BIO data is flushed
			BIO_flush(SSL_get_wbio(ci->ci_ssl));

			TRACE("TLS mode now active for fd=%d", ic->ic_fd);

			// Log kTLS status if available
#ifdef BIO_get_ktls_send
			int ktls_send =
				BIO_get_ktls_send(SSL_get_wbio(ci->ci_ssl));
			int ktls_recv =
				BIO_get_ktls_recv(SSL_get_rbio(ci->ci_ssl));
			TRACE("kTLS status for fd=%d: send=%d, recv=%d",
			      ic->ic_fd, ktls_send, ktls_recv);
#endif
		}

		io_context_destroy(ic);
		return 0;
	}

	// For TLS connections, handle encryption before further processing
	if (ci && ci->ci_ssl && ci->ci_tls_enabled) {
		int ret = io_do_tls(ic, rc);
		if (ret <= 0)
			return ret;
	}

	if ((size_t)bytes_written < ic->ic_expected_len) {
		TRACE("Partial write: ic=%p id=%u fd=%d expected=%zu wrote=%d position=%zu buffer_len=%zu",
		      (void *)ic, ic->ic_id, ic->ic_fd, ic->ic_expected_len,
		      bytes_written, ic->ic_position, ic->ic_buffer_len);

		// Update position by actual bytes written
		ic->ic_position += bytes_written;

		// Create a new context that owns the buffer
		struct io_context *new_ic =
			io_context_create(OP_TYPE_WRITE, ic->ic_fd,
					  ic->ic_buffer, ic->ic_buffer_len);
		if (!new_ic) {
			io_socket_close(ic->ic_fd, ENOMEM);
			io_context_destroy(ic);
			return ENOMEM;
		}

		// Copy state to new context
		new_ic->ic_position = ic->ic_position;
		new_ic->ic_xid = ic->ic_xid;
		new_ic->ic_count = ic->ic_count;
		copy_connection_info(&new_ic->ic_ci, &ic->ic_ci);

		// Clear buffer ownership in old context to prevent double-free
		ic->ic_buffer = NULL;
		ic->ic_buffer_len = 0;

		// Destroy old context without freeing the buffer
		io_context_destroy(ic);

		// Continue with the new context
		return rpc_trans_writer(new_ic, rc);
	}

	// Full chunk written - simply advance position
	ic->ic_position += ic->ic_expected_len;

#ifdef PARTIAL_WRITE_DEBUG
	TRACE("After write: ic=%p id=%u old_pos=%zu new_pos=%zu buffer_len=%zu",
	      (void *)ic, ic->ic_id, ic->ic_position - ic->ic_expected_len,
	      ic->ic_position, ic->ic_buffer_len);
#endif

	// Continue with next fragment
	return rpc_trans_writer(ic, rc);
}
