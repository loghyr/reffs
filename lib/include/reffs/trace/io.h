/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _REFFS_TRACE_IO_H
#define _REFFS_TRACE_IO_H

#include <stdbool.h>
#include <stdint.h>
#include "reffs/io.h"
#include "reffs/trace/trace.h"

/* IO specific trace functions */
static inline void trace_io_accept_submit(struct io_context *ic)
{
	reffs_trace_event(REFFS_TRACE_CAT_IO, "io_accept_submit", __LINE__,
			  "fd=%d, op=%s, id=%u, len=%zu", ic->ic_fd,
			  io_op_type_to_str(ic->ic_op_type), ic->ic_id,
			  ic->ic_buffer_len);
}

static inline void trace_io_connect_submit(struct io_context *ic)
{
	reffs_trace_event(REFFS_TRACE_CAT_IO, "io_connect_submit", __LINE__,
			  "fd=%d, op=%s, id=%u, len=%zu", ic->ic_fd,
			  io_op_type_to_str(ic->ic_op_type), ic->ic_id,
			  ic->ic_buffer_len);
}

static inline void trace_io_read_submit(struct io_context *ic)
{
	reffs_trace_event(REFFS_TRACE_CAT_IO, "io_read_submit", __LINE__,
			  "fd=%d, op=%s, id=%u, len=%zu", ic->ic_fd,
			  io_op_type_to_str(ic->ic_op_type), ic->ic_id,
			  ic->ic_buffer_len);
}

static inline void trace_io_write_submit(struct io_context *ic)
{
	reffs_trace_event(REFFS_TRACE_CAT_IO, "io_write_submit", __LINE__,
			  "fd=%d, op=%s, id=%u, len=%zu", ic->ic_fd,
			  io_op_type_to_str(ic->ic_op_type), ic->ic_id,
			  ic->ic_buffer_len);
}

static inline void trace_io_record_marker(struct buffer_state *bs,
					  uint32_t marker, bool last_fragment,
					  uint32_t fragment_len)
{
	reffs_trace_event(REFFS_TRACE_CAT_IO, "io_record_marker", __LINE__,
			  "fd=%d, marker=0x%08x, last=%d, len=%u, filled=%zu",
			  bs->bs_fd, marker, last_fragment, fragment_len,
			  bs->bs_filled);
}

static inline void trace_io_message_complete(int fd, uint32_t xid, size_t size)
{
	reffs_trace_event(REFFS_TRACE_CAT_IO, "io_message_complete", __LINE__,
			  "fd=%d, xid=0x%08x, size=%zu", fd, xid, size);
}

static inline void trace_io_connection_count(int fd, int count,
					     const char *func, int line)
{
	reffs_trace_event(REFFS_TRACE_CAT_IO, func, line, "fd=%d count=%d", fd,
			  count);
}

static inline void trace_io_active_connections(struct conn_info *ci,
					       char *peer_addr,
					       uint16_t peer_port,
					       char *local_addr,
					       uint16_t local_port, time_t now,
					       const char *func, int line)
{
	reffs_trace_event(
		REFFS_TRACE_CAT_IO, func, line,
		"fd=%d state=%s role=%s peer=%s:%d local=%s:%d xid=0x%08x last_activity=%ld reads=%d writes=%d",
		ci->ci_fd, io_conn_state_to_str(ci->ci_state),
		io_conn_role_to_str(ci->ci_role), peer_addr, peer_port,
		local_addr, local_port, ci->ci_xid, now - ci->ci_last_activity,
		ci->ci_read_count, ci->ci_write_count);
}

static inline void trace_io_connection_state_change(int fd, int old_state,
						    int new_state,
						    const char *func, int line)
{
	reffs_trace_event(REFFS_TRACE_CAT_IO, func, line,
			  "fd=%d state change: %s -> %s", fd,
			  io_conn_state_to_str(old_state),
			  io_conn_state_to_str(new_state));
}

static inline void trace_io_context(struct io_context *ic, const char *func,
				    int line)
{
	if (reffs_trace_is_category_enabled(REFFS_TRACE_CAT_IO)) {
		time_t now = time(NULL);
		time_t age = now - ic->ic_action_time;

		reffs_trace_event(
			REFFS_TRACE_CAT_IO, func, line,
			"ic=%p op=%s fd=%d state=0x%lx age=%ld count=%ld id=%u",
			(void *)ic, io_op_type_to_str(ic->ic_op_type),
			ic->ic_fd, ic->ic_state, age, ic->ic_count, ic->ic_id);
	}
}

static inline void trace_io_context_ptr(struct io_context *ic, const char *func,
					int line)
{
	reffs_trace_event(REFFS_TRACE_CAT_IO, func, line, "ic=%p", (void *)ic);
}

static inline void trace_io_writer(struct io_context *ic, const char *func,
				   int line)
{
	if (reffs_trace_is_category_enabled(REFFS_TRACE_CAT_IO)) {
		size_t remaining = ic->ic_buffer_len - ic->ic_position;
		uint32_t chunk_size;

		if (ic->ic_position == 0) {
			chunk_size = remaining > IO_MAX_WRITE_SIZE ?
					     IO_MAX_WRITE_SIZE :
					     remaining;
		} else {
			chunk_size = remaining > (IO_MAX_WRITE_SIZE - 4) ?
					     IO_MAX_WRITE_SIZE :
					     (remaining + 4);
		}

		reffs_trace_event(
			REFFS_TRACE_CAT_IO, func, line,
			"ic=%p fd=%d bl=%ld ip=%ld r=%ld cs=%d count=%ld id=%u",
			(void *)ic, ic->ic_fd, ic->ic_buffer_len,
			ic->ic_position, remaining, chunk_size, ic->ic_count,
			ic->ic_id);
	}
}

#endif /* _REFFS_TRACE_IO_H */
