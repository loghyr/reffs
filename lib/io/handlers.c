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
#include "trace_io.h"

/* Maximum reassembled RPC message size (matches IO_MAX_WRITE_SIZE + headers). */
#define MAX_RPC_MESSAGE_SIZE (2 * 1024 * 1024)

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
			next_ic =
				io_conn_write_done(ic->ic_fd, ic->ic_write_gen);
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
		uint32_t gen_saved = ic->ic_write_gen;
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
					io_conn_write_done(fd_saved, gen_saved);
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
		uint32_t gen_saved = ic->ic_write_gen;
		bool owned = (ic->ic_state & IO_CONTEXT_WRITE_OWNED) != 0;
		int ret = io_do_tls(ic, rc);
		if (ret <= 0) {
			if (ret == 0 && owned) {
				struct io_context *next_ic =
					io_conn_write_done(fd_saved, gen_saved);
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
		if (ic->ic_state & IO_CONTEXT_WRITE_OWNED) {
			new_ic->ic_state |= IO_CONTEXT_WRITE_OWNED;
			new_ic->ic_write_gen = ic->ic_write_gen;
		}

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
// Maximum reassembled RPC message size (matches IO_MAX_WRITE_SIZE + headers)
#define MAX_RPC_MESSAGE_SIZE (2 * 1024 * 1024)

static bool is_tls_client_hello(const unsigned char *buf, size_t len)
{
	// Minimum TLS record is 5 bytes (header) + data
	if (len < 6)
		return false;

	// Check TLS record header:
	// Byte 0: Content type (22 for handshake)
	// Bytes 1-2: TLS version (0x03 0x01/0x03/etc. for TLS)
	// Handshake type (after record header 5 bytes) should be 1 for ClientHello
	return (buf[0] == REFFS_TLS_HANDSHAKE &&
		buf[1] == REFFS_TLS_MAJOR_VERSION &&
		buf[5] == 0x01); // Handshake type 1 = ClientHello
}

static void log_client_hello_details(const unsigned char *buf, size_t len)
{
	if (len < 50)
		return; // Too short to contain useful TLS details

	TRC("TLS ClientHello version: 0x%02x%02x", buf[1], buf[2]);

	// Look for key extensions
	bool found_alpn = false;
	bool found_sni = false;

	// Skip past fixed header to extensions area (simplified - real parsing is more complex)
	size_t i =
		43; // Skip past record header, handshake type, length, version, random, session ID

	// Skip session ID
	if (i < len) {
		size_t session_id_len = buf[i];
		i += 1 + session_id_len;
	}

	// Skip cipher suites
	if (i + 1 < len) {
		size_t cipher_suites_len = (buf[i] << 8) | buf[i + 1];
		i += 2 + cipher_suites_len;
	}

	// Skip compression methods
	if (i < len) {
		size_t compression_methods_len = buf[i];
		i += 1 + compression_methods_len;
	}

	// Now at extensions, if present
	if (i + 1 < len) {
		size_t extensions_len = (buf[i] << 8) | buf[i + 1];
		i += 2;

		// Parse extensions
		size_t extensions_end = i + extensions_len;
		while (i + 3 < extensions_end && i + 3 < len) {
			uint16_t ext_type = (buf[i] << 8) | buf[i + 1];
			uint16_t ext_len = (buf[i + 2] << 8) | buf[i + 3];
			i += 4;

			if (ext_type == 0x0000) { // SNI
				found_sni = true;
				TRC("ClientHello includes SNI extension");
			} else if (ext_type == 0x0010) { // ALPN
				found_alpn = true;
				TRC("ClientHello includes ALPN extension");

				// Try to extract protocols (simplified)
				if (i + 2 < len && i + 2 < extensions_end) {
					size_t proto_list_len = (buf[i] << 8) |
								buf[i + 1];
					i += 2;

					size_t proto_end = i + proto_list_len;
					while (i < proto_end && i < len) {
						size_t proto_len = buf[i];
						i++;

						if (i + proto_len <= len &&
						    proto_len > 0) {
							TRC("ALPN protocol offered: %.*s",
							    (int)proto_len,
							    buf + i);
						}
						i += proto_len;
					}
				}
			} else {
				// Skip this extension
				i += ext_len;
			}
		}
	}

	if (!found_alpn) {
		TRC("WARNING: ClientHello does not contain ALPN extension - this may prevent NFSv3 over TLS");
	}

	if (!found_sni) {
		TRC("NOTE: ClientHello does not contain SNI extension");
	}
}

static int process_ssl_accept(SSL *ssl, struct conn_info *ci, int fd,
			      const void *data, size_t len,
			      struct ring_context *rc)
{
	BIO *rbio = SSL_get_rbio(ssl);
	BIO *wbio = SSL_get_wbio(ssl);
	int accept;
	int ssl_err;
	int pending;
	int ret;

	int bw = BIO_write(rbio, data, len);
	if (bw <= 0) {
		LOG("BIO_write failed during TLS accept: returned %d", bw);
		return -EIO;
	}

	SSL_set_mode(ssl,
		     SSL_MODE_AUTO_RETRY | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

	accept = SSL_accept(ssl);
	ssl_err = SSL_get_error(ssl, accept);

	pending = BIO_pending(wbio);

#ifdef TLS_DEBUGGING
	// Log SSL state
	const SSL_CIPHER *cipher = SSL_get_current_cipher(ssl);
	TRACE("TLS Info: Protocol=%s Cipher=%s", SSL_get_version(ssl),
	      cipher ? SSL_CIPHER_get_name(cipher) : "NONE");

	TRACE("TLS state: %s", SSL_state_string_long(ssl));
#endif

	// Check if handshake completed
	if (SSL_is_init_finished(ssl)) {
		TRC("TLS handshake finished, session established");

		// Check ALPN result but don't fail if not present
		const unsigned char *proto = NULL;
		unsigned int proto_len = 0;
		SSL_get0_alpn_selected(ssl, &proto, &proto_len);

		if (proto_len > 0) {
			TRC("ALPN protocol selected: %.*s", proto_len, proto);
		} else {
			TRC("No ALPN protocol was selected - continuing anyway for compatibility");
		}

		// Always enable TLS if handshake completes, regardless of ALPN
		ci->ci_tls_enabled = true;
		ci->ci_tls_handshaking = false;
	} else {
		TRC("TLS handshake not yet complete");

		// Special case: detect completed handshake without ALPN
		if (len == 6 && ((const unsigned char *)data)[0] == 0x14 &&
		    ((const unsigned char *)data)[1] == 0x03 &&
		    ((const unsigned char *)data)[2] == 0x03) {
			TRC("ChangeCipherSpec detected - checking if this is final handshake message");

			// Special fedora client compatibility - consider handshake complete at this point
			if (SSL_state_string_long(ssl) &&
			    strstr(SSL_state_string_long(ssl), "early data")) {
				TRC("Special case: Detected likely completed handshake - enabling TLS");
				ci->ci_tls_enabled = true;
				ci->ci_tls_handshaking = false;
				ci->ci_handshake_final_pending = true;
				ci->ci_handshake_final_bytes = 0;
			} else {
				TRC("ChangeCipherSpec detected, continuing normal handshake");
			}
		}
	}

	// If there's data to send back to the client
	if (pending > 0) {
		char *write_buffer = malloc(pending);
		if (!write_buffer) {
			LOG("Failed to allocate memory for TLS response");
			return ENOMEM;
		}

		int bytes = BIO_read(wbio, write_buffer, pending);
#ifdef TLS_DEBUGGING
		TRACE("Reading %d bytes from wbio for fd=%d", bytes, fd);
		rpc_log_packet("BIO: ", write_buffer, bytes);
#endif

		// Mark pending handshake completion if appropriate
		if (accept > 0 || SSL_is_init_finished(ssl)) {
			ci->ci_handshake_final_pending = true;
			ci->ci_handshake_final_bytes = bytes;
			TRC("Final handshake message prepared for fd=%d", fd);
		}

		// Send the data
		ret = io_request_write_op(fd, write_buffer, bytes,
					  IO_CONTEXT_DIRECT_TLS_DATA, NULL, rc);
		if (ret) {
			free(write_buffer);
			return -ret;
		}

		TRC("Submitted TLS response (%d bytes) for fd=%d", bytes, fd);
	}

	// Handle SSL accept result
	if (accept <= 0) {
		if (ssl_err == SSL_ERROR_WANT_READ ||
		    ssl_err == SSL_ERROR_WANT_WRITE) {
			TRC("TLS handshake continuing for fd=%d, need more data",
			    fd);
			return 0;
		}

		io_ssl_err_print(fd, "handshake failed", __func__, __LINE__);
		return EINVAL;
	}

	// If we get here, the handshake is complete
	ci->ci_tls_handshaking = false;
	TRC("TLS handshake completed logically for fd=%d, waiting for final message to be sent",
	    fd);
	return 0;
}

static int handle_tls_handshake(int fd, const void *data, size_t len,
				struct ring_context *rc)
{
	struct conn_info *ci = io_conn_get(fd);
	if (!ci) {
		LOG("Connection not tracked for fd=%d", fd);
		return ENOTCONN;
	}

#ifdef TLS_DEBUGGING
	const unsigned char *bytes = (const unsigned char *)data;
	char hexdump[100] = { 0 };
	size_t hexlen = 0;

	// Format at most the first 16 bytes for logging
	for (size_t i = 0; i < len && i < 16 && hexlen < sizeof(hexdump) - 4;
	     i++) {
		hexlen += snprintf(hexdump + hexlen, sizeof(hexdump) - hexlen,
				   "%02x ", bytes[i]);
	}

	TRACE("TLS data first bytes: %s", hexdump);
	TRACE("TLS handshake: processing %zu bytes for fd=%d", len, fd);
#endif

	// Log detailed ClientHello info if this is one
	if (is_tls_client_hello(data, len)) {
		log_client_hello_details(data, len);
	}

	// Initialize TLS context if not already done at startup
	if (io_tls_init_server_context(NULL, NULL, NULL) != 0) {
		LOG("Failed to initialize TLS context");
		return EINVAL;
	}

	// Case 1: Continuing an existing handshake
	if (ci->ci_tls_handshaking && ci->ci_ssl) {
		int ret = process_ssl_accept(ci->ci_ssl, ci, fd, data, len, rc);

		const unsigned char *bytes = (const unsigned char *)data;
		if (bytes[0] == 0x14 && bytes[1] == 0x03 && bytes[2] == 0x03 &&
		    len == 6) {
			TRC("ChangeCipherSpec received, forcing TLS compatibility mode for Fedora client");
			ci->ci_tls_enabled = true;
			ci->ci_tls_handshaking = false;
		}

		return ret;
	}

	// Case 2: New handshake - create SSL object
	SSL *ssl = SSL_new(reffs_server_ssl_ctx);
	if (!ssl) {
		LOG("Failed to create SSL for fd=%d", fd);
		return EINVAL;
	}

#ifdef TLS_DEBUGGING
	TRACE("SSL %p using SSL_CTX %p", (void *)ssl,
	      (void *)SSL_get_SSL_CTX(ssl));
	TRACE("reffs_server_ssl_ctx is %p", (void *)reffs_server_ssl_ctx);

	// Log current ALPN state
	const unsigned char *proto = NULL;
	unsigned int proto_len = 0;
	SSL_get0_alpn_selected(ssl, &proto, &proto_len);
	if (proto_len == 0) {
		TRACE("ALPN not selected at this point");
	} else {
		TRACE("ALPN selected at this point: %.*s", proto_len, proto);
	}
#endif

	// Set up SSL for server role
	SSL_set_accept_state(ssl);
	SSL_set_mode(ssl,
		     SSL_MODE_AUTO_RETRY | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

	// Create memory BIOs for SSL I/O
	BIO *rbio = BIO_new(BIO_s_mem());
	BIO *wbio = BIO_new(BIO_s_mem());
	if (!rbio || !wbio) {
		if (rbio)
			BIO_free(rbio);
		if (wbio)
			BIO_free(wbio);
		SSL_free(ssl);
		LOG("Failed to create memory BIOs");
		return EINVAL;
	}

	// Associate BIOs with SSL object
	SSL_set_bio(ssl, rbio, wbio);

	// Store the SSL object in the connection info
	ci->ci_ssl = ssl;
	ci->ci_tls_handshaking = true;
	ci->ci_tls_enabled = false;
	ci->ci_handshake_final_pending = false;

	int ret = process_ssl_accept(ssl, ci, fd, data, len, rc);

#ifdef TLS_DEBUGGING
	if (len == 6 && bytes[0] == 0x14 && bytes[1] == 0x03 &&
	    bytes[2] == 0x03) {
		TRACE("ChangeCipherSpec received, forcing TLS compatibility mode for Fedora client");
		ci->ci_tls_enabled = true;
		ci->ci_tls_handshaking = false;
	}
#endif

	return ret;
}
// Process the RPC record marker and reassemble fragments
// Returns:
//   > 0: Complete message available, returns size
//   0: Need more data
//   < 0: Error
static int process_record_marker(struct buffer_state *bs)
{
	char *data = bs->bs_data;
	size_t filled = bs->bs_filled;
	struct record_state *rs = &bs->bs_record;

	// If continuing an existing fragment
	if (rs->rs_fragment_len > 0) {
		// Calculate remaining bytes needed
		size_t bytes_remaining = rs->rs_fragment_len - rs->rs_position;
		size_t to_copy = (filled > bytes_remaining) ? bytes_remaining :
							      filled;

		// Copy data to the correct position in the buffer
		memcpy(rs->rs_data + rs->rs_total_len + rs->rs_position, data,
		       to_copy);
		rs->rs_position += to_copy;

		// Advance the input buffer
		data += to_copy;
		filled -= to_copy;

		// Check if we've completed this fragment
		if (rs->rs_position >= rs->rs_fragment_len) {
			// If this was the last fragment, we've got a complete message
			if (rs->rs_last_fragment) {
				size_t message_size =
					rs->rs_total_len + rs->rs_position;

				// Reset state for next message
				rs->rs_total_len = 0;
				rs->rs_position = 0;
				rs->rs_fragment_len = 0;
				rs->rs_last_fragment = false;

				// Update buffer state
				if (filled > 0) {
					memmove(bs->bs_data, data, filled);
					bs->bs_filled = filled;
				} else {
					bs->bs_filled = 0;
				}

				return message_size;
			}

			// Not the last fragment, accumulate offset
			rs->rs_total_len += rs->rs_fragment_len;
			rs->rs_position = 0;
			rs->rs_fragment_len = 0;

			// If we don't have enough data for next marker, update and return
			if (filled < 4) {
				if (filled > 0) {
					memmove(bs->bs_data, data, filled);
					bs->bs_filled = filled;
				} else {
					bs->bs_filled = 0;
				}
				return 0;
			}
		} else {
			// Fragment not complete, need more data
			if (filled > 0) {
				memmove(bs->bs_data, data, filled);
				bs->bs_filled = filled;
			} else {
				bs->bs_filled = 0;
			}

			return 0;
		}
	}

	// Starting a new fragment/message

	// Need at least 4 bytes for the record marker
	if (filled < 4) {
		bs->bs_filled = filled;
		return 0;
	}

	// Extract marker
	uint32_t marker = ntohl(*(uint32_t *)data);
	bool last_fragment = (marker & 0x80000000) != 0;
	uint32_t fragment_len = marker & 0x7FFFFFFF;

	trace_io_record_marker(bs, marker, last_fragment, fragment_len);

	// Skip invalid markers
	if (fragment_len == 0) {
		data += 4;
		filled -= 4;
		if (filled < 4) {
			if (filled > 0) {
				memmove(bs->bs_data, data, filled);
			}
			bs->bs_filled = filled;
			return 0;
		}
		// Update buffer and return instead of recursing
		memmove(bs->bs_data, data, filled);
		bs->bs_filled = filled;
		return 0;
	}

	// Initialize state for this fragment
	rs->rs_last_fragment = last_fragment;
	rs->rs_fragment_len = fragment_len;
	rs->rs_position = 0;

	// Ensure we have enough buffer space for accumulated + new fragment
	size_t needed = rs->rs_total_len + fragment_len;

	// Cap reassembled message size to prevent DoS via unbounded fragments
	if (needed > MAX_RPC_MESSAGE_SIZE) {
		rs->rs_total_len = 0;
		rs->rs_position = 0;
		rs->rs_fragment_len = 0;
		return -EOVERFLOW;
	}

	if (!rs->rs_data) {
		size_t capacity = needed * 2;

		rs->rs_data = malloc(capacity);
		if (!rs->rs_data) {
			rs->rs_total_len = 0;
			rs->rs_position = 0;
			rs->rs_fragment_len = 0;
			rs->rs_capacity = 0;
			return -ENOMEM;
		}
		rs->rs_capacity = capacity;
	} else if (needed > rs->rs_capacity) {
		size_t new_capacity = needed * 2;

		char *new_data = realloc(rs->rs_data, new_capacity);
		if (!new_data) {
			/* Reset so the next fragment does not use stale lengths */
			rs->rs_fragment_len = 0;
			rs->rs_total_len = 0;
			rs->rs_position = 0;
			return -ENOMEM;
		}
		rs->rs_data = new_data;
		rs->rs_capacity = new_capacity;
	}

	// Skip past the marker
	data += 4;
	filled -= 4;

	// If no data after marker, request more
	if (filled == 0) {
		bs->bs_filled = 0;
		return 0;
	}

	// Copy available data for this fragment
	size_t to_copy = (filled > fragment_len) ? fragment_len : filled;

	memcpy(rs->rs_data + rs->rs_total_len, data, to_copy);
	rs->rs_position = to_copy;

	// Advance buffer pointers
	data += to_copy;
	filled -= to_copy;

	// Check if we've completed this fragment
	if (rs->rs_position >= fragment_len) {
		// Fragment is complete

		// If this was the last fragment, we have a complete message
		if (last_fragment) {
			size_t message_size =
				rs->rs_total_len + rs->rs_position;

			// Reset state for next message
			rs->rs_total_len = 0;
			rs->rs_position = 0;
			rs->rs_fragment_len = 0;
			rs->rs_last_fragment = false;

			// Update buffer state
			if (filled > 0) {
				memmove(bs->bs_data, data, filled);
				bs->bs_filled = filled;
			} else {
				bs->bs_filled = 0;
			}

			return message_size;
		}

		// Not the last fragment, accumulate offset
		rs->rs_total_len += rs->rs_fragment_len;
		rs->rs_position = 0;
		rs->rs_fragment_len = 0;

		if (filled < 4) {
			if (filled > 0) {
				memmove(bs->bs_data, data, filled);
			}
			bs->bs_filled = filled;
			return 0;
		}

		// Update buffer state instead of recursing
		memmove(bs->bs_data, data, filled);
		bs->bs_filled = filled;
		return 0;
	}

	// Fragment is incomplete, need more data
	if (filled > 0) {
		memmove(bs->bs_data, data, filled);
	}

	bs->bs_filled = filled;

	return 0;
}

// Handle read completions

int io_handle_read(struct io_context *ic, int bytes_read,
		   struct ring_context *rc)
{
	int ret = 0;
	bool needs_new_read = true;

	// Extract data from context
	char *buffer = (char *)ic->ic_buffer;
	int client_fd = ic->ic_fd;

	trace_io_read(client_fd, bytes_read);

	struct conn_info *ci = io_conn_get(client_fd);
	struct buffer_state *bs = NULL;

	if (bytes_read <= 0) {
		// Connection closed or error
		TRACE("Connection closed or error (fd: %d, res: %d)", client_fd,
		      bytes_read);

		io_socket_close(client_fd,
				bytes_read < 0 ? -bytes_read : ECONNRESET);

		io_check_for_listener_restart(client_fd, &ic->ic_ci, rc);

		io_context_destroy(ic);
		return 0; // No new read needed for closed connections
	}

	if (ci) {
		ci->ci_last_activity = time(NULL);
#ifdef TLS_DEBUGGING
		TRACE("ci=%p th=%d tls=%d ssl=%p", (void *)ci,
		      ci->ci_tls_handshaking, ci->ci_tls_enabled,
		      (void *)ci->ci_ssl);
#endif
		if (ci->ci_tls_handshaking) {
			ret = handle_tls_handshake(ic->ic_fd, ic->ic_buffer,
						   bytes_read, rc);
			if (ret) {
				SSL_free(ci->ci_ssl);
				ci->ci_ssl = NULL;
				ci->ci_tls_handshaking = false;
				io_socket_close(ic->ic_fd, EINVAL);
				io_context_destroy(ic);
				return 0;
			}
			goto get_more;
		}

		// If TLS is enabled, decrypt the data
		if (ci->ci_tls_enabled && ci->ci_ssl) {
			// Feed data to SSL
			BIO *rbio = SSL_get_rbio(ci->ci_ssl);
			int bw = BIO_write(rbio, ic->ic_buffer, bytes_read);
			if (bw != bytes_read) {
				LOG("BIO_write short: expected %d got %d",
				    bytes_read, bw);
				io_socket_close(ic->ic_fd, EIO);
				io_context_destroy(ic);
				return 0;
			}

			// Read decrypted data
			int decrypted = SSL_read(ci->ci_ssl, ic->ic_buffer,
						 ic->ic_buffer_len);
			rpc_log_packet("TLS: ", ic->ic_buffer,
				       ic->ic_buffer_len);

			if (decrypted <= 0) {
				int ssl_err =
					SSL_get_error(ci->ci_ssl, decrypted);

				/* WANT_READ/WANT_WRITE are not errors: the
				 * TLS state machine needs more data or buffer
				 * space.  ERR_get_error() returns 0 in this
				 * case so logging would only produce noise. */
				if (ssl_err == SSL_ERROR_WANT_READ ||
				    ssl_err == SSL_ERROR_WANT_WRITE) {
					goto cleanup;
				}

				long alert = ERR_get_error();
				char alert_str[256];
				ERR_error_string_n(alert, alert_str,
						   sizeof(alert_str));
				LOG("SSL error %d, alert: %s", ssl_err,
				    alert_str);

				// SSL error
				io_ssl_err_print(ic->ic_fd, "read error",
						 __func__, __LINE__);
				SSL_free(ci->ci_ssl);
				ci->ci_ssl = NULL;
				ci->ci_tls_handshaking = false;
				io_socket_close(ic->ic_fd, EINVAL);
				io_context_destroy(ic);
				return 0;
			}

			/*
			 * A single TLS record may carry multiple application-
			 * level chunks; SSL_read only returned what fit in the
			 * buffer for the first chunk.  SSL_pending > 0 means
			 * more plaintext is already buffered inside OpenSSL
			 * and will NOT trigger a new readable event on the
			 * fd -- the wire bytes have been consumed.  Drain
			 * into remaining buffer space before returning to the
			 * event loop.
			 */
			int total = decrypted;
			while (total < (int)ic->ic_buffer_len &&
			       SSL_pending(ci->ci_ssl) > 0) {
				int more = SSL_read(
					ci->ci_ssl,
					(char *)ic->ic_buffer + total,
					(int)ic->ic_buffer_len - total);
				if (more <= 0)
					break;
				total += more;
			}

			// Update bytes_read with decrypted count
			bytes_read = total;
		}
	}

	// Check for TLS ClientHello, but only on a fresh connection that has
	// not yet accumulated any RPC framing.  A buffer_state is created on
	// the first plain-NFS read; if one already exists this connection is
	// carrying RPC traffic and the probe would false-positive on payload
	// bytes that happen to match the TLS record header pattern.
	bs = io_buffer_state_get(client_fd);
	if (!bs && is_tls_client_hello(ic->ic_buffer, bytes_read)) {
		TRACE("TLS ClientHello detected on fd=%d", ic->ic_fd);
		ret = handle_tls_handshake(ic->ic_fd, ic->ic_buffer, bytes_read,
					   rc);
		if (ret) {
			if (ci) {
				SSL_free(ci->ci_ssl);
				ci->ci_ssl = NULL;
				ci->ci_tls_handshaking = false;
				io_socket_close(ic->ic_fd, EINVAL);
				io_context_destroy(ic);

				return 0;
			}
		}

		goto get_more;
	}

	// Create buffer state if this is the first plain-NFS read
	if (!bs) {
		bs = io_buffer_state_create(client_fd);
		if (!bs) {
			LOG("Failed to create buffer state for fd: %d",
			    client_fd);
			// Socket remains open but we couldn't allocate buffer state
			goto cleanup;
		}
	}

	// Append new data to existing buffer
	if (!io_buffer_append(bs, buffer, bytes_read)) {
		LOG("Failed to append to buffer for fd: %d", client_fd);
		// Memory allocation failure but socket is still valid
		goto cleanup;
	}

	// Process the RPC record marker to get a complete RPC message
	while (bs->bs_filled >= 4) {
		int complete_size = process_record_marker(bs);
		if (complete_size < 0) {
			LOG("Error processing record marker for fd: %d",
			    client_fd);
			io_check_for_listener_restart(client_fd, &ic->ic_ci,
						      rc);
			io_socket_close(client_fd, ENOBUFS);
			needs_new_read = false;
			goto cleanup;
		}

		if (complete_size == 0)
			break;

		// Create a task for processing
		struct task *t = calloc(1, sizeof(struct task));
		if (!t) {
			LOG("Failed to allocate task for fd: %d", client_fd);
			goto cleanup;
		}

		// Copy the complete message
		t->t_buffer = malloc(complete_size);
		if (!t->t_buffer) {
			free(t);
			LOG("Failed to allocate task buffer for fd: %d",
			    client_fd);
			goto cleanup;
		}

		memcpy(t->t_buffer, bs->bs_record.rs_data, complete_size);
		t->t_bytes_read = complete_size;
		t->t_fd = client_fd;
		t->t_rc = rc;

		copy_connection_info(&t->t_ci, &ic->ic_ci);

		// Extract XID for convenience
		if (complete_size >= 4) {
			t->t_xid = ntohl(*(uint32_t *)t->t_buffer);
		} else {
			t->t_xid = 0;
		}

		trace_io_message_complete(client_fd, t->t_xid, complete_size);
		trace_io_queued_task(client_fd, t->t_xid, complete_size);

		// Queue it for processing
		add_task(t);

		// Reset the record state for the next message
		bs->bs_record.rs_total_len = 0;
		bs->bs_record.rs_position = 0;
	}

get_more:
	// Reuse the current ic's buffer for the next read.
	ic->ic_fd = client_fd;
	ret = io_resubmit_read(ic, rc);
	if (ret == 0) {
		// Successfully submitted new read operation
		needs_new_read = false;
	} else {
		LOG("Failed to request more read data: %s", strerror(-ret));
	}

cleanup:
	if (needs_new_read) {
		ret = io_request_read_op(client_fd, &ic->ic_ci, rc);
		if (ret != 0) {
			LOG("Failed to request additional read: %s",
			    strerror(ret));
			io_socket_close(client_fd, ret);
		}
		io_context_destroy(ic);
	}

	return 0;
}
