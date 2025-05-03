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

/*
 * Let the caller shut things down if there is an error
 */
int request_more_read_data(struct buffer_state *bs, struct io_uring *ring,
			   struct io_context *ic)
{
	struct io_uring_sqe *sqe = NULL;
	int ret = 0;

	ic->ic_fd = bs->bs_fd;

	struct conn_info *conn = io_conn_get(ic->ic_fd);
	if (conn) {
		io_conn_set_state(ic->ic_fd, CONN_READING, 0);
	}

	for (int i = 0; i < REFFS_IO_MAX_RETRIES; i++) {
		sqe = io_uring_get_sqe(ring);
		if (sqe)
			break;
		usleep(IO_URING_WAIT_US);
	}

	if (!sqe) {
		return -ENOMEM;
	}

	io_uring_prep_read(sqe, bs->bs_fd, ic->ic_buffer, BUFFER_SIZE, 0);
	sqe->user_data = (uint64_t)(uintptr_t)ic;
	trace_io_read_submit(ic);

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

int request_additional_read_data(int fd, struct connection_info *ci,
				 struct io_uring *ring)
{
	struct io_uring_sqe *sqe = NULL;
	int ret = 0;

	if (fd <= 0 || fd >= MAX_CONNECTIONS) {
		LOG("Invalid fd: %d", fd);
		return -EINVAL;
	}

	struct conn_info *conn = io_conn_get(fd);
	if (conn) {
		if (conn->ci_state == CONN_CONNECTED ||
		    conn->ci_state == CONN_ACCEPTED) {
			io_conn_set_state(fd, CONN_READING, 0);
		} else {
			LOG("Warning: Not changing state from %s to READING",
			    conn_state_to_str(conn->ci_state));
		}
	}

	char *buffer = malloc(BUFFER_SIZE);
	if (!buffer) {
		LOG("Failed to allocate buffer");
		io_socket_close(fd, ENOMEM);
		return ENOMEM;
	}

	struct io_context *ic =
		io_context_create(OP_TYPE_READ, fd, buffer, BUFFER_SIZE);
	if (!ic) {
		LOG("Failed to create read context");
		free(buffer);
		io_socket_close(fd, ENOMEM);
		return ENOMEM;
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
		io_context_free(ic);
		return ENOMEM;
	}

	io_uring_prep_read(sqe, fd, buffer, BUFFER_SIZE, 0);
	sqe->user_data = (uint64_t)(uintptr_t)ic;

	trace_io_read_submit(ic);

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
		io_context_free(ic);
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
	int ret;

	// Extract data from context
	char *buffer = (char *)ic->ic_buffer;
	int client_fd = ic->ic_fd;

	struct conn_info *conn = io_conn_get(client_fd);

	if (bytes_read <= 0) {
		// Connection closed or error
		LOG("Connection closed or error (fd: %d, res: %d)", client_fd,
		    bytes_read);
		io_socket_close(client_fd,
				bytes_read < 0 ? -bytes_read : ECONNRESET);
		io_context_free(ic);
		return 0;
	}

	if (conn) {
		conn->ci_last_activity = time(NULL);
		// Set state back to CONNECTED after successful read
		io_conn_set_state(client_fd, CONN_CONNECTED, 0);
	}

	// Get or create buffer state for this connection
	struct buffer_state *bs = get_buffer_state(client_fd);
	if (!bs) {
		bs = create_buffer_state(client_fd);
		if (!bs) {
			io_context_free(ic);
			return -ENOMEM;
		}
	}

	// Append new data to existing buffer
	if (!append_to_buffer(bs, buffer, bytes_read)) {
		io_context_free(ic);
		return -ENOMEM;
	}

	// Process the RPC record marker to get a complete RPC message
	while (bs->bs_filled >= 4) {
		int complete_size = process_record_marker(bs);
		if (complete_size < 0) {
			io_socket_close(ic->ic_fd, ENOBUFS);
			io_context_free(ic);
			return 0;
		}

		if (complete_size == 0)
			break;

		// Create a task for processing
		struct task *t = calloc(1, sizeof(struct task));
		if (t) {
			// Copy the complete message
			t->t_buffer = malloc(complete_size);
			if (!t->t_buffer) {
				free(t);
				return -ENOMEM;
			}

			memcpy(t->t_buffer, bs->bs_record.rs_data,
			       complete_size);
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

			trace_io_message_complete(client_fd, t->t_xid,
						  complete_size);

			// Queue it for processing
			add_task(t);

			// Reset the record state for the next message
			bs->bs_record.rs_total_len = 0;
			bs->bs_record.rs_position = 0;
		}
	}

	ret = request_more_read_data(bs, ring, ic);
	if (ret) {
		io_socket_close(ic->ic_fd, ret);
		io_context_free(ic);
	}

	return 0;
}
