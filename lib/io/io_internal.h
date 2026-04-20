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
 *
 * Lock ordering (2026-04-20 audit, #32):
 *   conn_mutex   (lib/io/conn_info.c)   protects connections[] and per-ci state
 *   context_mutex (lib/io/context.c)    protects context_hash[]
 *   request_mutex (lib/io/net_state.c)  protects pending_requests[]  (leaf)
 *   rc_mutex     (struct ring_context)  protects io_uring SQ          (leaf)
 *   job_mutex    (lib/io/backend_darwin.c) protects the thread-pool queue (leaf)
 *
 * Canonical acquisition order when nesting is required:
 *     conn_mutex -> context_mutex
 *
 * All other pairs are never nested.  io_context_destroy is the only
 * function that takes context_mutex, and it is always called either
 * with no locks held (most call sites) or after an explicit
 * pthread_mutex_unlock(&conn_mutex) (e.g. io_conn_unregister).  The
 * audit found no site that acquires these mutexes in the reverse
 * order.  Do not introduce context_mutex -> conn_mutex acquisition
 * without updating this comment and every inner-to-outer caller.
 */

#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>
#include <time.h>

#include "reffs/io.h"
#include "reffs/network.h"
#include "reffs/tls.h"

struct rpc_trans;

/*
 * IO operation context (was public; opaque outside lib/io/).
 * External diagnostic callers copy the relevant subset into a
 * struct io_context_snapshot instead of reaching into fields.
 */
struct io_context {
	enum op_type ic_op_type;
	int ic_fd;
	uint32_t ic_id;
	void *ic_buffer;

	size_t ic_buffer_len;
	size_t ic_position;
	size_t ic_expected_len;

	uint32_t ic_xid;

	uint64_t ic_state;

	time_t ic_action_time;

	uint64_t ic_count;

	struct connection_info ic_ci;

	/*
	 * Backend I/O ops (OP_TYPE_BACKEND_PREAD/PWRITE) store the owning
	 * rpc_trans here so the completion handler can resume the task.
	 * NULL for all network ops.
	 */
	struct rpc_trans *ic_rt;

	struct io_context *ic_next;

	/*
	 * Per-fd write serialization queue.  Used only while this context is
	 * waiting for the write gate on ic_fd.  NULL when not queued.
	 * Distinct from ic_next (which links contexts in the active-context
	 * hash) so both lists can be maintained simultaneously.
	 */
	struct io_context *ic_write_next;

	/*
	 * Generation of the conn_info slot at the time this context claimed
	 * the write gate.  io_conn_write_done() validates this so a stale
	 * write CQE arriving after the fd is closed and reused cannot
	 * prematurely release the new connection's gate.
	 */
	uint32_t ic_write_gen;
};

/* Record state for reassembling fragmented RPC messages. */
struct record_state {
	bool rs_last_fragment;
	uint32_t rs_fragment_len;
	char *rs_data;
	size_t rs_total_len;
	size_t rs_capacity;
	uint32_t rs_position;
};

/* Connection buffer state for reassembling messages. */
struct buffer_state {
	int bs_fd;
	char *bs_data;
	size_t bs_filled;
	size_t bs_capacity;
	struct record_state bs_record;
};

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

	/*
	 * Monotonically increasing generation counter.  Incremented each
	 * time this slot is reused for a new connection.  Stored in
	 * ic_write_gen by io_conn_write_try_start() and verified by
	 * io_conn_write_done() so stale write CQEs from a closed fd cannot
	 * corrupt the gate state of a new connection that reused the same
	 * fd number.
	 */
	uint32_t ci_generation;
};

#endif /* _REFFS_IO_INTERNAL_H */
