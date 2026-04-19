/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Backend-agnostic completion handlers.
 *
 * io_handle_accept and io_handle_connect run after the OS has signalled
 * that a socket operation completed.  On the liburing backend this is
 * a CQE dispatch in handler.c's main loop; on the kqueue backend it is
 * an EVFILT_READ / EVFILT_WRITE dispatch in backend_kqueue.c's main
 * loop.  In both cases the work the handler does -- register the
 * conn_info, fill in addresses, kick off the read loop, resubmit the
 * accept -- is identical.  The backend-specific submission sites
 * (io_request_accept_op, io_request_read_op, the connect helper) are
 * still backend-local.
 *
 * Calling convention for the int result argument follows io_uring's
 * cqe->res semantics:
 *   io_handle_accept(ic, client_fd_or_neg_errno, rc)
 *     - nonnegative: the accepted client fd
 *     - negative:    a negative errno describing the failure
 *   io_handle_connect(ic, result_or_neg_errno, rc)
 *     - zero:     connect succeeded
 *     - negative: a negative errno describing the failure
 *
 * kqueue callers must translate -1 / errno into -errno before invoking
 * these handlers; see accept_and_dispatch / connect_and_dispatch in
 * backend_kqueue.c.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" // IWYU pragma: keep
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "reffs/log.h"
#include "reffs/io.h"
#include "reffs/network.h"
#include "reffs/ring.h"
#include "reffs/rpc.h"
#include "reffs/tls.h"
#include "reffs/trace/io.h"

/* Forward declaration -- defined below, used by io_rpc_trans_cb. */
static int rpc_trans_writer(struct io_context *ic, struct ring_context *rc);

/*
 * io_do_tls -- TLS write handler (userspace BIO; falls through on kTLS).
 *
 * Returns < 1 for error, 0 for no need for further processing,
 * 1 for using kTLS and fall through.
 *
 * Unreachable on FreeBSD PR #7 because io_tls_init_server_context is
 * not called from the kqueue io_handler_init -- ci->ci_ssl is always
 * NULL.  When PR #8 ports TLS, the io_request_write_op call at the
 * bottom must be adjusted to honor the per-fd write gate (otherwise
 * the subsequent writer's EVFILT_WRITE registration silently replaces
 * this one's on kqueue).  See PR #7 addendum B2 for the analysis.
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

	/*
	 * Guard: the connection may have been torn down (TCP RST from
	 * client) between when the write was queued and when the worker
	 * picked it up.  ci or ci_ssl can be NULL/freed.
	 *
	 * Save ci_ssl to a local -- the event loop thread can set
	 * ci->ci_ssl = NULL at any time after an SSL error.  If we
	 * see NULL, the connection is dead and there's nothing to write.
	 */
	SSL *ssl = ci ? ci->ci_ssl : NULL;
	if (!ssl) {
		TRACE("fd=%d: connection gone, dropping TLS write", ic->ic_fd);
		io_context_destroy(ic);
		return -ECONNRESET;
	}

	/* Write directly using TLS if not using kTLS */
	int ktls_enabled = 0;
#ifdef BIO_get_ktls_send
	ktls_enabled = BIO_get_ktls_send(SSL_get_wbio(ssl));
#endif

	TRACE("ic=%p fd=%d type=%s bl=%ld id=%u", (void *)ic, ic->ic_fd,
	      io_op_type_to_str(ic->ic_op_type), ic->ic_buffer_len, ic->ic_id);
	if (!ktls_enabled) {
		/* Check if we've already processed this context for TLS */
		if (ic->ic_state & IO_CONTEXT_TLS_BIO_PROCESSED) {
			TRACE("ic=%p id=%u already processed for TLS, destroying",
			      (void *)ic, ic->ic_id);
			io_context_destroy(ic);
			return 0;
		}

		/* Handle in userspace */
		rpc_log_packet("TLS: ", ic->ic_buffer, ic->ic_buffer_len);
		int ret = SSL_write(ssl,
				    (char *)ic->ic_buffer + ic->ic_position,
				    remaining);

		if (ret <= 0) {
			int err = SSL_get_error(ssl, ret);
			if (err == SSL_ERROR_WANT_WRITE) {
				io_ssl_err_print(
					ic->ic_fd,
					"unexpected WANT_WRITE on write",
					__func__, __LINE__);
				io_socket_close(ic->ic_fd, EINVAL);
				io_context_destroy(ic);
				return -EINVAL;
			}
			if (err == SSL_ERROR_WANT_READ) {
				io_ssl_err_print(
					ic->ic_fd,
					"WANT_READ during write (key-update?)",
					__func__, __LINE__);
				io_socket_close(ic->ic_fd, EINVAL);
				io_context_destroy(ic);
				return -EINVAL;
			}

			/* Real error */
			io_ssl_err_print(ic->ic_fd, "write error", __func__,
					 __LINE__);
			io_socket_close(ic->ic_fd, EINVAL);
			io_context_destroy(ic);
			return -EINVAL;
		} else {
			BIO *wbio = SSL_get_wbio(ssl);
			int pending = BIO_pending(wbio);

			TRACE("SSL_write processed %d bytes, resulting in %d bytes of TLS data",
			      ret, pending);

			ic->ic_state |= IO_CONTEXT_TLS_BIO_PROCESSED;

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

/*
 * rpc_trans_writer -- orchestrate the write of a single RPC reply.
 *
 * Backend-agnostic.  Claims the per-fd write gate (at most one
 * io_context in flight per fd), optionally encrypts via io_do_tls,
 * then delegates the actual submission to io_resubmit_write, which
 * each backend implements (io_uring_prep_write on liburing, or
 * kevent(EV_ADD|EV_ONESHOT, EVFILT_WRITE) on kqueue).
 *
 * Called from io_rpc_trans_cb (initial reply) and from io_handle_write
 * (next fragment after a chunk completes).
 */
static int rpc_trans_writer(struct io_context *ic, struct ring_context *rc)
{
	int ret = 0;

	/* Check if we're done first */
	if (ic->ic_position >= ic->ic_buffer_len) {
#ifdef PARTIAL_WRITE_DEBUG
		TRACE("Buffer complete: ic=%p id=%u position=%zu, buffer_len=%zu",
		      (void *)ic, ic->ic_id, ic->ic_position,
		      ic->ic_buffer_len);
#endif
		trace_io_write_complete(ic->ic_fd, 0, ic);
		/*
		 * Release the write gate and immediately start the next queued
		 * writer (if any) before destroying this context.  The next_ic
		 * already has WRITE_OWNED set by io_conn_write_done().
		 */
		struct io_context *next_ic = NULL;
		if (ic->ic_state & IO_CONTEXT_WRITE_OWNED)
			next_ic = io_conn_write_done(ic->ic_fd);
		io_context_destroy(ic);
		if (next_ic)
			return rpc_trans_writer(next_ic, rc);
		return 0;
	}

	trace_io_writer(ic, __func__, __LINE__);

	/*
	 * Claim the per-fd write gate before submitting.  Only one
	 * io_context may have a write in flight for a given fd at a time;
	 * concurrent writers queue here and are released by io_conn_write_done()
	 * after each write completes.
	 *
	 * Skip the check on re-entry (partial writes, multi-fragment) -- the
	 * WRITE_OWNED flag means we already hold the gate.
	 */
	if (!(ic->ic_state & IO_CONTEXT_WRITE_OWNED)) {
		if (!io_conn_write_try_start(ic->ic_fd, ic))
			return 0; /* queued; io_conn_write_done() will restart us */
		ic->ic_state |= IO_CONTEXT_WRITE_OWNED;
	}

	struct conn_info *ci = io_conn_get(ic->ic_fd);
#ifdef TLS_DEBUGGING
	TRACE("ci=%p ssl=%p tls=%d", (void *)ci, ci ? (void *)ci->ci_ssl : NULL,
	      ci ? ci->ci_tls_enabled : 0);
#endif
	/*
	 * TLS branch.  On FreeBSD PR #7 this is unreachable because
	 * io_tls_init_server_context is not called from the kqueue
	 * io_handler_init, so ci->ci_ssl is never set.  PR #8 ports TLS
	 * and must address the EVFILT_WRITE collision window that arises
	 * when io_do_tls submits outside the write gate (see addendum).
	 */
	if (ci && ci->ci_ssl && ci->ci_tls_enabled) {
		int fd_saved = ic->ic_fd;
		bool owned = (ic->ic_state & IO_CONTEXT_WRITE_OWNED) != 0;
		ret = io_do_tls(ic, rc);
		if (ret <= 0) {
			/*
			 * Non-kTLS success (ret==0): io_do_tls destroyed ic.
			 * Release the write gate and start the next queued
			 * writer, if any.  Error paths (ret<0) do not hold
			 * the gate because io_do_tls calls io_socket_close,
			 * which drains ci_write_pending via io_conn_unregister.
			 */
			if (ret == 0 && owned) {
				struct io_context *next_ic =
					io_conn_write_done(fd_saved);
				if (next_ic)
					return rpc_trans_writer(next_ic, rc);
			}
			return ret;
		}
	}

	return io_resubmit_write(ic, rc);
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
		LOG("Failed to create write context for fd=%d", rt->rt_fd);
		return ENOMEM;
	}

	ic->ic_xid = rt->rt_info.ri_xid;
	copy_connection_info(&ic->ic_ci, &rt->rt_info.ri_ci);

	if (rt->rt_reply_len >= 4) {
		uint32_t *marker_ptr = (uint32_t *)rt->rt_reply;
		uint32_t data_len = rt->rt_reply_len - 4;

		/*
		 * Always set the last fragment bit.  The message is fully
		 * formed -- do not try to re-fragment it.
		 */
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

	struct conn_info *ci = io_conn_get(ic->ic_fd);

	if (bytes_written > 0 && ic->ic_position == 0) {
		TRACE("WRITE START: ic=%p id=%u fd=%d starting fresh write.",
		      (void *)ic, ic->ic_id, ic->ic_fd);
	}

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

	if (ci) {
		ci->ci_last_activity = time(NULL);

		if (ci->ci_handshake_final_pending) {
			TRACE("Final TLS handshake message sent for fd=%d",
			      ic->ic_fd);
			ci->ci_handshake_final_pending = false;
			ci->ci_tls_enabled = true;
			BIO_flush(SSL_get_wbio(ci->ci_ssl));
			TRACE("TLS mode now active for fd=%d", ic->ic_fd);

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

	if (ic->ic_state & IO_CONTEXT_DIRECT_TLS_DATA) {
		TRACE("Direct TLS data sent for fd=%d", ic->ic_fd);

		struct conn_info *ci2 = io_conn_get(ic->ic_fd);
		if (ci2 && ci2->ci_handshake_final_pending) {
			TRACE("Final TLS handshake message sent for fd=%d",
			      ic->ic_fd);
			ci2->ci_handshake_final_pending = false;
			BIO_flush(SSL_get_wbio(ci2->ci_ssl));
			TRACE("TLS mode now active for fd=%d", ic->ic_fd);

#ifdef BIO_get_ktls_send
			int ktls_send =
				BIO_get_ktls_send(SSL_get_wbio(ci2->ci_ssl));
			int ktls_recv =
				BIO_get_ktls_recv(SSL_get_rbio(ci2->ci_ssl));
			TRACE("kTLS status for fd=%d: send=%d, recv=%d",
			      ic->ic_fd, ktls_send, ktls_recv);
#endif
		}

		io_context_destroy(ic);
		return 0;
	}

	if (ci && ci->ci_ssl && ci->ci_tls_enabled) {
		int fd_saved = ic->ic_fd;
		bool owned = (ic->ic_state & IO_CONTEXT_WRITE_OWNED) != 0;
		int ret = io_do_tls(ic, rc);
		if (ret <= 0) {
			if (ret == 0 && owned) {
				struct io_context *next_ic =
					io_conn_write_done(fd_saved);
				if (next_ic)
					return rpc_trans_writer(next_ic, rc);
			}
			return ret;
		}
	}

	if ((size_t)bytes_written < ic->ic_expected_len) {
		TRACE("Partial write: ic=%p id=%u fd=%d expected=%zu wrote=%d position=%zu buffer_len=%zu",
		      (void *)ic, ic->ic_id, ic->ic_fd, ic->ic_expected_len,
		      bytes_written, ic->ic_position, ic->ic_buffer_len);

		ic->ic_position += bytes_written;

		struct io_context *new_ic =
			io_context_create(OP_TYPE_WRITE, ic->ic_fd,
					  ic->ic_buffer, ic->ic_buffer_len);
		if (!new_ic) {
			io_socket_close(ic->ic_fd, ENOMEM);
			io_context_destroy(ic);
			return ENOMEM;
		}

		new_ic->ic_position = ic->ic_position;
		new_ic->ic_xid = ic->ic_xid;
		new_ic->ic_count = ic->ic_count;
		copy_connection_info(&new_ic->ic_ci, &ic->ic_ci);
		/*
		 * Propagate the write gate ownership.  The new context is a
		 * continuation of the same write; it holds the gate for this fd
		 * and must not try to re-acquire it in rpc_trans_writer().
		 */
		if (ic->ic_state & IO_CONTEXT_WRITE_OWNED)
			new_ic->ic_state |= IO_CONTEXT_WRITE_OWNED;

		/* Clear buffer ownership in old context to prevent double-free */
		ic->ic_buffer = NULL;
		ic->ic_buffer_len = 0;

		io_context_destroy(ic);

		return rpc_trans_writer(new_ic, rc);
	}

	/* Full chunk written -- simply advance position */
	ic->ic_position += ic->ic_expected_len;

#ifdef PARTIAL_WRITE_DEBUG
	TRACE("After write: ic=%p id=%u old_pos=%zu new_pos=%zu buffer_len=%zu",
	      (void *)ic, ic->ic_id, ic->ic_position - ic->ic_expected_len,
	      ic->ic_position, ic->ic_buffer_len);
#endif

	return rpc_trans_writer(ic, rc);
}

int io_handle_accept(struct io_context *ic, int client_fd,
		     struct ring_context *rc)
{
	char addr_str[INET6_ADDRSTRLEN];
	uint16_t port;

	int listen_fd = ic->ic_fd;

	bool accept_resubmitted = false;

	trace_io_context(ic, __func__, __LINE__);

	/*
	 * Always try to re-arm the accept first, to ensure we don't miss
	 * incoming connections while we set up the one we just got.
	 */
	int accept_ret = io_request_accept_op(listen_fd, &ic->ic_ci, rc);
	if (accept_ret == 0) {
		accept_resubmitted = true;
	} else {
		LOG("Failed to resubmit accept request - watchdog will retry later");
	}

	if (client_fd < 0) {
		LOG("Accept failed with error: %s", strerror(-client_fd));

		if (!accept_resubmitted) {
			LOG("Trying one more time to resubmit accept");
			io_request_accept_op(listen_fd, &ic->ic_ci, rc);
		}

		io_context_destroy(ic);
		return -client_fd;
	}

	int flag = 1;
	if (setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag,
		       sizeof(flag)) < 0) {
		LOG("setsockopt TCP_NODELAY failed for fd=%d: %s", client_fd,
		    strerror(errno));
	}

	struct conn_info *client_conn =
		io_conn_register(client_fd, CONN_ACCEPTED, CONN_ROLE_SERVER);
	if (!client_conn) {
		LOG("Failed to register client connection fd=%d", client_fd);
		io_socket_close(client_fd, ENOMEM);
		io_context_destroy(ic);
		return ENOMEM;
	}

	ic->ic_ci.ci_peer_len = sizeof(ic->ic_ci.ci_peer);
	if (getpeername(client_fd, (struct sockaddr *)&ic->ic_ci.ci_peer,
			&ic->ic_ci.ci_peer_len) == 0) {
		addr_to_string(&ic->ic_ci.ci_peer, addr_str, INET6_ADDRSTRLEN,
			       &port);

		memcpy(&client_conn->ci_peer, &ic->ic_ci.ci_peer,
		       ic->ic_ci.ci_peer_len);
		client_conn->ci_peer_len = ic->ic_ci.ci_peer_len;

		TRACE("Accepted connection from %s:%d on fd=%d", addr_str, port,
		      client_fd);
		TRACE("ACCEPTED: fd=%d from %s:%d", client_fd, addr_str, port);
	} else {
		LOG("Failed to get peer information: %s", strerror(errno));
		memset(&ic->ic_ci.ci_peer, 0, sizeof(ic->ic_ci.ci_peer));
		ic->ic_ci.ci_peer_len = 0;
	}

	ic->ic_ci.ci_local_len = sizeof(ic->ic_ci.ci_local);
	if (getsockname(client_fd, (struct sockaddr *)&ic->ic_ci.ci_local,
			&ic->ic_ci.ci_local_len) == 0) {
		addr_to_string(&ic->ic_ci.ci_local, addr_str, INET6_ADDRSTRLEN,
			       &port);

		memcpy(&client_conn->ci_local, &ic->ic_ci.ci_local,
		       ic->ic_ci.ci_local_len);
		client_conn->ci_local_len = ic->ic_ci.ci_local_len;
	} else {
		LOG("Failed to get local information: %s", strerror(errno));
		memset(&ic->ic_ci.ci_local, 0, sizeof(ic->ic_ci.ci_local));
		ic->ic_ci.ci_local_len = 0;
	}

	io_client_fd_register(client_fd);

	struct conn_info *conn = io_conn_get(client_fd);
	if (conn) {
		TRACE("Accepted new connection fd=%d", client_fd);
	} else {
		LOG("Warning: Connection fd=%d not found after registration",
		    client_fd);
	}

	io_request_read_op(client_fd, &ic->ic_ci, rc);

	if (!accept_resubmitted) {
		io_request_accept_op(listen_fd, &ic->ic_ci, rc);
	}

	io_context_destroy(ic);
	return 0;
}

int io_handle_connect(struct io_context *ic, int result,
		      struct ring_context __attribute__((unused)) * rc)
{
	char addr_str[INET6_ADDRSTRLEN];
	uint16_t port;

	if (result < 0) {
		LOG("Connect failed for fd=%d: %s", ic->ic_fd,
		    strerror(-result));

		io_socket_close(ic->ic_fd, -result);
		io_context_destroy(ic);
		return -result;
	}

	ic->ic_ci.ci_peer_len = sizeof(ic->ic_ci.ci_peer);
	if (getpeername(ic->ic_fd, (struct sockaddr *)&ic->ic_ci.ci_peer,
			&ic->ic_ci.ci_peer_len) != 0) {
		LOG("Failed to get peer information: %s", strerror(errno));

		io_conn_set_error(ic->ic_fd, errno);

		memset(&ic->ic_ci.ci_peer, 0, sizeof(ic->ic_ci.ci_peer));
		ic->ic_ci.ci_peer_len = 0;

		io_socket_close(ic->ic_fd, errno);
		io_context_destroy(ic);
		return errno;
	}

	ic->ic_ci.ci_local_len = sizeof(ic->ic_ci.ci_local);
	if (getsockname(ic->ic_fd, (struct sockaddr *)&ic->ic_ci.ci_local,
			&ic->ic_ci.ci_local_len) == 0) {
		addr_to_string(&ic->ic_ci.ci_local, addr_str, INET6_ADDRSTRLEN,
			       &port);
	} else {
		LOG("Failed to get local information: %s", strerror(errno));
		memset(&ic->ic_ci.ci_local, 0, sizeof(ic->ic_ci.ci_local));
		ic->ic_ci.ci_local_len = 0;
	}

	TRACE("Connection established for fd=%d", ic->ic_fd);

	struct conn_info *ci = io_conn_get(ic->ic_fd);
	if (ci) {
		memcpy(&ci->ci_peer, &ic->ic_ci.ci_peer, ic->ic_ci.ci_peer_len);
		ci->ci_peer_len = ic->ic_ci.ci_peer_len;

		memcpy(&ci->ci_local, &ic->ic_ci.ci_local,
		       ic->ic_ci.ci_local_len);
		ci->ci_local_len = ic->ic_ci.ci_local_len;
	}

	struct rpc_trans *rt = io_find_request_by_xid(ic->ic_xid);
	if (!rt) {
		LOG("No matching rpc_trans found for XID=%u", ic->ic_xid);

		io_socket_close(ic->ic_fd, ENOENT);
		io_context_destroy(ic);
		return ENOENT;
	}

	rt->rt_fd = ic->ic_fd;

	copy_connection_info(&ic->ic_ci, &rt->rt_info.ri_ci);
	io_context_destroy(ic);

	return io_rpc_trans_cb(rt);
}
