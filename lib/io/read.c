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
#include "reffs/stack.h"
#include "reffs/trace/io.h"

// Function to detect a TLS ClientHello
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

static int handle_tls_handshake(int fd, const void *data, size_t len,
				struct io_uring *ring)
{
	BIO *wbio;
	BIO *rbio;
	int pending;
	int ret;
	int accept;
	int ssl_err;

	struct conn_info *ci = io_conn_get(fd);
	if (!ci) {
		LOG("Connection not tracked for fd=%d", fd);
		return ENOTCONN;
	}

	LOG("TLS handshake: processing %zu bytes for fd=%d", len, fd);

	if (io_tls_init_server_context() != 0) {
		LOG("Failed to initialize TLS context");
		return EINVAL;
	}

	if (ci->ci_tls_handshaking && ci->ci_ssl) {
		// Feed the data to SSL
		rbio = SSL_get_rbio(ci->ci_ssl);
		BIO_write(rbio, data, len);

		accept = SSL_accept(ci->ci_ssl);
		ssl_err = SSL_get_error(ci->ci_ssl, accept);

		wbio = SSL_get_wbio(ci->ci_ssl);
		pending = BIO_pending(wbio);

		LOG("SSL_accept returned %d (ssl_err=%d), pending data: %d bytes",
		    accept, ssl_err, pending);

		if (pending > 0) {
			// There is data that needs to be sent back to the client
			char *write_buffer = malloc(pending);
			if (!write_buffer) {
				LOG("Failed to allocate memory for TLS response");
				return ENOMEM;
			}

			int bytes = BIO_read(wbio, write_buffer, pending);
			LOG("Reading %d bytes from wbio for fd=%d", bytes, fd);

			if (accept > 0) {
				ci->ci_handshake_final_pending = true;
				ci->ci_handshake_final_bytes = bytes;
				LOG("Handshake logically complete, sending final message (%d bytes) for fd=%d",
				    bytes, fd);
			}

			ret = io_request_write_op(fd, write_buffer, bytes,
						  IO_CONTEXT_DIRECT_TLS_DATA,
						  NULL, ring);
			if (ret) {
				free(write_buffer);
				return -ret;
			}

			LOG("Submitted TLS response (%d bytes) for fd=%d",
			    bytes, fd);
		}

		if (accept <= 0) {
			if (ssl_err == SSL_ERROR_WANT_READ ||
			    ssl_err == SSL_ERROR_WANT_WRITE) {
				LOG("TLS handshake continuing for fd=%d, need more data",
				    fd);
				return 0;
			}

			io_ssl_err_print(fd, "handshake failed", __func__,
					 __LINE__);
			return EINVAL;
		}

		// Handshake is logically complete, but we don't enable TLS yet
		// That will happen in io_handle_write after final message is sent
		ci->ci_tls_handshaking = false;
		LOG("TLS handshake completed logically for fd=%d, waiting for final message to be sent",
		    fd);
		return 0;
	}

	SSL *ssl = SSL_new(reffs_server_ssl_ctx);
	if (!ssl) {
		LOG("Failed to create SSL for fd=%d", fd);
		return EINVAL;
	}

	rbio = BIO_new(BIO_s_mem());
	wbio = BIO_new(BIO_s_mem());
	if (!rbio || !wbio) {
		if (rbio)
			BIO_free(rbio);
		if (wbio)
			BIO_free(wbio);
		SSL_free(ssl);
		LOG("Failed to create memory BIOs");
		return EINVAL;
	}

	SSL_set_bio(ssl, rbio, wbio);

	BIO_write(rbio, data, len);

	accept = SSL_accept(ssl);
	ssl_err = SSL_get_error(ssl, accept);

	// Check for data to write back, regardless of SSL_accept result
	pending = BIO_pending(wbio);
	if (pending > 0) {
		char *write_buffer = malloc(pending);
		if (!write_buffer) {
			LOG("Failed to allocate memory for initial TLS response");
			SSL_free(ssl);
			return ENOMEM;
		}

		int bytes = BIO_read(wbio, write_buffer, pending);
		LOG("Initial handshake generated %d bytes to send for fd=%d",
		    bytes, fd);

		if (accept > 0) {
			ci->ci_handshake_final_pending = true;
			ci->ci_handshake_final_bytes = bytes;
			LOG("Handshake completed immediately, sending final message");
		}

		ret = io_request_write_op(fd, write_buffer, bytes,
					  IO_CONTEXT_DIRECT_TLS_DATA, NULL,
					  ring);
		if (ret) {
			free(write_buffer);
			SSL_free(ssl);
			return -ret;
		}

		LOG("Submitted initial TLS response (%d bytes) for fd=%d",
		    bytes, fd);
	}

	if (accept <= 0) {
		if (ssl_err == SSL_ERROR_WANT_READ ||
		    ssl_err == SSL_ERROR_WANT_WRITE) {
			ci->ci_ssl = ssl;
			ci->ci_tls_handshaking = true;
			ci->ci_tls_enabled = false;
			ci->ci_handshake_final_pending = false;

			LOG("TLS handshake started for fd=%d, need more data",
			    fd);
			return 0;
		}

		io_ssl_err_print(fd, "handshake failed immediately", __func__,
				 __LINE__);
		SSL_free(ssl);
		return EINVAL;
	}

	ci->ci_ssl = ssl;
	ci->ci_tls_handshaking = false;
	ci->ci_handshake_final_pending = true;

	LOG("TLS handshake completed immediately for fd=%d, waiting for final message to be sent",
	    fd);
	return 0;
}

/*
 * Let the caller shut things down if there is an error
 */
static int request_more_read_data(int fd, struct io_uring *ring,
				  struct io_context *ic)
{
	struct io_uring_sqe *sqe = NULL;
	int ret = 0;

	ic->ic_fd = fd;

	for (int i = 0; i < REFFS_IO_MAX_RETRIES; i++) {
		sqe = io_uring_get_sqe(ring);
		if (sqe)
			break;
		usleep(IO_URING_WAIT_US);
	}

	if (!sqe) {
		return -ENOMEM;
	}

	io_uring_prep_read(sqe, fd, ic->ic_buffer, BUFFER_SIZE, 0);
	sqe->user_data = (uint64_t)(uintptr_t)ic;
	//trace_io_read_submit(ic);
	io_context_update_time(ic);

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

	if (ret > 0)
		ret = 0;

	return ret;
}

int io_request_read_op(int fd, struct connection_info *ci,
		       struct io_uring *ring)
{
	struct io_uring_sqe *sqe = NULL;
	int ret = 0;

	if (fd <= 0 || fd >= MAX_CONNECTIONS) {
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

	for (int i = 0; i < REFFS_IO_MAX_RETRIES; i++) {
		sqe = io_uring_get_sqe(ring);
		if (sqe)
			break;
		usleep(IO_URING_WAIT_US);
	}

	if (!sqe) {
		free(buffer);
		io_socket_close(fd, ENOMEM);
		io_context_destroy(ic);
		return -ENOMEM;
	}

	io_uring_prep_read(sqe, fd, buffer, BUFFER_SIZE, 0);
	sqe->user_data = (uint64_t)(uintptr_t)ic;

	//trace_io_read_submit(ic);

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
		free(buffer);
		io_socket_close(fd, -ret);
		io_context_destroy(ic);
	} else {
		ret = 0;
	}

	return 0;
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
		memcpy(rs->rs_data + rs->rs_position, data, to_copy);
		rs->rs_position += to_copy;

		// Advance the input buffer
		data += to_copy;
		filled -= to_copy;

		// Check if we've completed this fragment
		if (rs->rs_position >= rs->rs_fragment_len) {
			// If this was the last fragment, we've got a complete message
			if (rs->rs_last_fragment) {
				size_t message_size = rs->rs_position;

				// Reset state for next message
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

			// Not the last fragment, reset for next fragment
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

	trace_io_record_marker(bs, marker, last_fragment, marker);

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

	// Ensure we have enough buffer space
	if (!rs->rs_data) {
		rs->rs_capacity = fragment_len * 2;
		rs->rs_data = malloc(rs->rs_capacity);
		if (!rs->rs_data) {
			return -ENOMEM;
		}
	} else if (fragment_len > rs->rs_capacity) {
		size_t new_capacity = fragment_len * 2;

		char *new_data = realloc(rs->rs_data, new_capacity);
		if (!new_data) {
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

	memcpy(rs->rs_data, data, to_copy);
	rs->rs_position = to_copy;

	// Advance buffer pointers
	data += to_copy;
	filled -= to_copy;

	// Check if we've completed this fragment
	if (rs->rs_position >= fragment_len) {
		// Fragment is complete

		// If this was the last fragment, we have a complete message
		if (last_fragment) {
			size_t message_size = rs->rs_position;

			// Reset state for next message
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

		// Not the last fragment, check if we have data for next marker
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

int io_handle_read(struct io_context *ic, int bytes_read, struct io_uring *ring)
{
	int ret = 0;
	bool needs_new_read = true;

	// Extract data from context
	char *buffer = (char *)ic->ic_buffer;
	int client_fd = ic->ic_fd;

	struct conn_info *ci = io_conn_get(client_fd);
	struct buffer_state *bs = NULL;

	if (bytes_read <= 0) {
		// Connection closed or error
		LOG("Connection closed or error (fd: %d, res: %d)", client_fd,
		    bytes_read);

		io_socket_close(client_fd,
				bytes_read < 0 ? -bytes_read : ECONNRESET);

		io_check_for_listener_restart(client_fd, &ic->ic_ci, ring);

		io_context_destroy(ic);
		return 0; // No new read needed for closed connections
	}

	if (ci) {
		ci->ci_last_activity = time(NULL);
#ifdef TLS_DEBUGGING
		LOG("ci=%p th=%d tls=%d ssl=%p", (void *)ci,
		    ci->ci_tls_handshaking, ci->ci_tls_enabled,
		    (void *)ci->ci_ssl);
#endif
		if (ci->ci_tls_handshaking) {
			ret = handle_tls_handshake(ic->ic_fd, ic->ic_buffer,
						   bytes_read, ring);
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
			BIO_write(rbio, ic->ic_buffer, bytes_read);

			// Read decrypted data
			int decrypted = SSL_read(ci->ci_ssl, ic->ic_buffer,
						 ic->ic_buffer_len);
			if (decrypted <= 0) {
				int ssl_err =
					SSL_get_error(ci->ci_ssl, decrypted);
				if (ssl_err == SSL_ERROR_WANT_READ ||
				    ssl_err == SSL_ERROR_WANT_WRITE) {
					goto cleanup;
				}

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

			// Update bytes_read with decrypted count
			bytes_read = decrypted;
		}
	}

	// Check for TLS ClientHello
	if (is_tls_client_hello(ic->ic_buffer, bytes_read)) {
		LOG("TLS ClientHello detected on fd=%d", ic->ic_fd);
		ret = handle_tls_handshake(ic->ic_fd, ic->ic_buffer, bytes_read,
					   ring);
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

	// Get or create buffer state for this connection
	bs = get_buffer_state(client_fd);
	if (!bs) {
		bs = create_buffer_state(client_fd);
		if (!bs) {
			LOG("Failed to create buffer state for fd: %d",
			    client_fd);
			// Socket remains open but we couldn't allocate buffer state
			goto cleanup;
		}
	}

	// Append new data to existing buffer
	if (!append_to_buffer(bs, buffer, bytes_read)) {
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
						      ring);
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
		t->t_ring = ring;

		copy_connection_info(&t->t_ci, &ic->ic_ci);

		// Extract XID for convenience
		if (complete_size >= 4) {
			t->t_xid = ntohl(*(uint32_t *)t->t_buffer);
		} else {
			t->t_xid = 0;
		}

		trace_io_message_complete(client_fd, t->t_xid, complete_size);

		// Queue it for processing
		add_task(t);

		// Reset the record state for the next message
		bs->bs_record.rs_total_len = 0;
		bs->bs_record.rs_position = 0;
	}

get_more:
	// Try to use request_more_read_data first (which reuses the current context)
	ret = request_more_read_data(client_fd, ring, ic);
	if (ret == 0 || ret == EAGAIN) {
		// Successfully submitted new read operation
		needs_new_read = false;
	} else {
		LOG("Failed to request more read data: %s", strerror(ret));
	}

cleanup:
	if (needs_new_read) {
		ret = io_request_read_op(client_fd, &ic->ic_ci, ring);
		if (ret != 0) {
			LOG("Failed to request additional read: %s",
			    strerror(ret));
			io_socket_close(client_fd, ret);
		}
		io_context_destroy(ic);
	}

	return 0;
}
