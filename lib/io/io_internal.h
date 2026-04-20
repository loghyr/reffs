/*
 * SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _REFFS_IO_INTERNAL_H
#define _REFFS_IO_INTERNAL_H

/*
 * Private to lib/io/ and its in-tree tests.  Structs here are opaque
 * to external consumers (they hold pointers via the public API and
 * call accessors for any field they need).  See reffs/io.h for the
 * public contract.
 */

#include <stdbool.h>
#include <sys/socket.h>

#include "reffs/io.h"
#include "reffs/tls.h"

struct io_context;

/* Connection info (was public; opaque outside lib/io/). */
struct conn_info {
	int ci_fd;
	enum conn_state ci_state;
	enum conn_role ci_role;
	time_t ci_last_activity;
	struct sockaddr_storage ci_peer;
	socklen_t ci_peer_len;
	struct sockaddr_storage ci_local;
	socklen_t ci_local_len;
	uint32_t ci_xid;
	bool ci_tls_enabled;
	bool ci_tls_handshaking;
	bool ci_handshake_final_pending;
	int ci_handshake_final_bytes;
	SSL *ci_ssl;
	int ci_error;
	int ci_read_count;
	int ci_write_count;
	int ci_accept_count;
	int ci_connect_count;

	/*
	 * Per-fd write serialization gate.  Only one io_context may have a
	 * write SQE in flight for this fd at a time; concurrent writers queue
	 * here and are drained by io_conn_write_done() as each write finishes.
	 * Protected by conn_mutex in connect.c.
	 */
	bool ci_write_active;
	struct io_context *ci_write_pending_head;
	struct io_context *ci_write_pending_tail;
};

#endif /* _REFFS_IO_INTERNAL_H */
